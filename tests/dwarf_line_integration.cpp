#include "debugger/debugger.hpp"
#include "dwarf/line_table.hpp"
#include "dwarf/local_value.hpp"
#include "elf/elf.hpp"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint64_t kExpectedLocalRawValue = 0x1020304050607080ULL;
constexpr const char* kExpectedLocalValue = "local_value = 1161981756646125696";
constexpr const char* kExpectedParameterValue = "parameter = 1161981756646125696";

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_mapped_source(const mdbg::SourceLocation& location) {
  require(std::filesystem::path(location.file).filename() == "mapped_source.c",
          "line table returned an unexpected source file: " + location.file);
  require(location.line == 400, "line_probe must map to synthetic source line 400");
}

void test_runtime_mapping(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  require(lines.available(), "DWARF fixture should expose line ranges");

  const auto probe = elf.find_symbol("line_probe");
  require(probe.has_value(), "line_probe symbol missing from fixture");

  const auto virtual_location = lines.find_virtual_address(probe->value);
  require(virtual_location.has_value(), "virtual line lookup failed for line_probe");
  require_mapped_source(*virtual_location);

  const auto reverse_virtual = lines.find_virtual_source("mapped_source.c", 400);
  require(reverse_virtual.has_value(), "reverse source lookup failed for mapped_source.c:400");
  const auto reverse_virtual_location = lines.find_virtual_address(*reverse_virtual);
  require(reverse_virtual_location.has_value(),
          "reverse source address did not map back to a source location");
  require_mapped_source(*reverse_virtual_location);
  const auto reverse_symbol = elf.find_symbol_by_virtual_address(*reverse_virtual);
  require(reverse_symbol.has_value() && reverse_symbol->symbol.name == "line_probe",
          "reverse source lookup did not select an address inside line_probe");
  require(!lines.find_virtual_source("mapped_source.c", 9999).has_value(),
          "missing source line must not resolve to an address");

  const auto runtime_address = elf.runtime_address(debugger.pid(), *probe);
  const auto runtime_location =
      lines.find_runtime_address(debugger.pid(), runtime_address, elf);
  require(runtime_location.has_value(), "runtime line lookup failed for line_probe");
  require_mapped_source(*runtime_location);

  const auto reverse_runtime =
      lines.find_runtime_source(debugger.pid(), "mapped_source.c", 400, elf);
  require(reverse_runtime.has_value(),
          "runtime reverse source lookup failed for mapped_source.c:400");
  const auto reverse_runtime_location =
      lines.find_runtime_address(debugger.pid(), *reverse_runtime, elf);
  require(reverse_runtime_location.has_value(),
          "runtime reverse source address did not map back to a source location");
  require_mapped_source(*reverse_runtime_location);
  const auto reverse_runtime_symbol =
      elf.find_symbol_by_runtime_address(debugger.pid(), *reverse_runtime);
  require(reverse_runtime_symbol.has_value() &&
              reverse_runtime_symbol->symbol.name == "line_probe",
          "runtime reverse source lookup did not select line_probe");

  debugger.add_breakpoint(static_cast<std::uintptr_t>(*reverse_runtime));
  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == *reverse_runtime,
          "source-derived breakpoint was not hit");
  const auto repaired_rip = debugger.registers().rip;
  require(repaired_rip == *reverse_runtime,
          "source-derived managed breakpoint did not expose repaired RIP");
  const auto stopped_location =
      lines.find_runtime_address(debugger.pid(), repaired_rip, elf);
  require(stopped_location.has_value(), "stopped RIP did not resolve to a source line");
  require_mapped_source(*stopped_location);
}

void test_local_value_api(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {"value"});
  const mdbg::ElfFile elf(fixture);
  const auto probe = elf.find_symbol("local_value_probe");
  require(probe.has_value(), "local_value_probe symbol missing from fixture");
  const auto address = static_cast<std::uintptr_t>(elf.runtime_address(debugger.pid(), *probe));
  debugger.add_breakpoint(address);
  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint && stop.breakpoint_address == address,
          "local-value fixture did not stop at the compiler-generated probe");

  const auto value = mdbg::inspect_local_integer(debugger, elf, "local_value");
  require(value.name == "local_value", "local-value API returned the wrong variable name");
  require(value.raw_value == kExpectedLocalRawValue,
          "local-value API returned the wrong runtime integer value");
  require(value.byte_size == sizeof(std::uint64_t) && !value.is_signed,
          "local-value API returned the wrong uint64_t type metadata");
  require(std::filesystem::equivalent(value.module_path, fixture),
          "local-value API did not preserve the owning module identity");

  bool missing_failed = false;
  try {
    (void)mdbg::inspect_local_integer(debugger, elf, "missing_local");
  } catch (const std::exception&) {
    missing_failed = true;
  }
  require(missing_failed, "missing local variable did not fail explicitly");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "local-value fixture did not exit cleanly after inspection");
}

void test_formal_parameter_api(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const auto probe = elf.find_symbol("formal_parameter_probe");
  require(probe.has_value(), "formal_parameter_probe symbol missing from optimized fixture");
  const auto address = static_cast<std::uintptr_t>(elf.runtime_address(debugger.pid(), *probe));
  debugger.add_breakpoint(address);
  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint && stop.breakpoint_address == address,
          "formal-parameter fixture did not stop while the parameter was live");

  const auto value = mdbg::inspect_local_integer(debugger, elf, "parameter");
  require(value.name == "parameter", "formal-parameter API returned the wrong name");
  require(value.raw_value == kExpectedLocalRawValue,
          "formal-parameter API returned the wrong register-resident value");
  require(value.byte_size == sizeof(std::uint64_t) && !value.is_signed,
          "formal-parameter API returned the wrong uint64_t type metadata");
  require(std::filesystem::equivalent(value.module_path, fixture),
          "formal-parameter API did not preserve the owning module identity");

  bool missing_failed = false;
  try {
    (void)mdbg::inspect_local_integer(debugger, elf, "missing_parameter");
  } catch (const std::exception&) {
    missing_failed = true;
  }
  require(missing_failed, "missing formal parameter did not fail explicitly");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "formal-parameter fixture did not exit cleanly after inspection");
}

std::string run_cli_script(const std::string& integration_path, const std::string& fixture,
                           const char* mode, const std::string& script,
                           const char* context) {
  const auto mdbg_path =
      (std::filesystem::absolute(integration_path).parent_path() / "mdbg").string();
  require(std::filesystem::exists(mdbg_path), "mdbg executable is missing beside integration test");

  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create CLI pipes");
  }

  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("failed to fork CLI");
  if (child == 0) {
    ::setpgid(0, 0);
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    if (mode != nullptr) {
      ::execl(mdbg_path.c_str(), mdbg_path.c_str(), fixture.c_str(), mode, nullptr);
    } else {
      ::execl(mdbg_path.c_str(), mdbg_path.c_str(), fixture.c_str(), nullptr);
    }
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);

  std::size_t written = 0;
  while (written < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + written, script.size() - written);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write CLI script");
    written += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  bool eof = false;
  while (!eof) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      ::kill(-child, SIGKILL);
      ::kill(child, SIGKILL);
      throw std::runtime_error(std::string("timed out waiting for ") + context);
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
    const int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
    if (result == -1 && errno == EINTR) continue;
    if (result <= 0) {
      ::kill(-child, SIGKILL);
      ::kill(child, SIGKILL);
      throw std::runtime_error(std::string("timed out reading ") + context + " output");
    }

    char buffer[512];
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error(std::string("failed to read ") + context + " output");
    if (count == 0) {
      eof = true;
    } else {
      output.append(buffer, static_cast<std::size_t>(count));
    }
  }
  ::close(output_pipe[0]);

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          std::string(context) + " did not exit cleanly\n" + output);
  return output;
}

void test_cli_local_value(const std::string& integration_path, const std::string& fixture) {
  const auto output = run_cli_script(
      integration_path, fixture, "value",
      "break local_value_probe\n"
      "continue\n"
      "print local_value\n"
      "continue\n",
      "value-inspection CLI");
  require(output.find("Breakpoint 1") != std::string::npos,
          "CLI did not install the local-value probe breakpoint\n" + output);
  require(output.find(kExpectedLocalValue) != std::string::npos,
          "CLI did not render the compiler-generated local integer value\n" + output);
}

void test_cli_formal_parameter(const std::string& integration_path,
                               const std::string& fixture) {
  const auto output = run_cli_script(
      integration_path, fixture, nullptr,
      "break formal_parameter_probe\n"
      "continue\n"
      "print parameter\n"
      "print missing_parameter\n"
      "continue\n",
      "formal-parameter CLI");
  require(output.find("Breakpoint 1") != std::string::npos,
          "CLI did not install the formal-parameter probe breakpoint\n" + output);
  require(output.find(kExpectedParameterValue) != std::string::npos,
          "CLI did not render the register-resident formal parameter\n" + output);
  require(output.find("print failed:") != std::string::npos,
          "CLI did not report a missing formal parameter explicitly\n" + output);
}

void test_missing_debug_line(const std::string& stripped_fixture) {
  const mdbg::DwarfLineTable lines(stripped_fixture);
  require(!lines.available(), "stripped fixture must not claim DWARF line coverage");
  require(!lines.find_virtual_address(0).has_value(),
          "empty line table must not resolve arbitrary addresses");
  require(!lines.find_virtual_source("mapped_source.c", 400).has_value(),
          "empty line table must not resolve source locations");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) return 2;
  try {
    test_runtime_mapping(argv[1]);
    test_local_value_api(argv[1]);
    test_cli_local_value(argv[0], argv[1]);
    test_missing_debug_line(argv[2]);
    test_runtime_mapping(argv[3]);
    test_formal_parameter_api(argv[4]);
    test_cli_formal_parameter(argv[0], argv[4]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "DWARF line integration failure: %s\n", error.what());
    return 1;
  }
}

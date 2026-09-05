#include "debugger/debugger.hpp"
#include "dwarf/local_value.hpp"
#include "elf/elf.hpp"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr std::uint64_t kExpectedEntryParameter = UINT64_C(0x1020304050607080);
constexpr std::uint64_t kExpectedEntryRdiSentinel = UINT64_C(0x777788889999aaaa);
constexpr std::uint64_t kExpectedOptimizedLocal = UINT64_C(0x1e3c1e781e3c1ef0);
constexpr const char* kExpectedEntryCliValue = "entry_parameter = 1161981756646125696";
constexpr const char* kExpectedCliValue = "optimized_local = 2178649820992642800";

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::uintptr_t entry_value_address(const mdbg::ElfFile& elf, pid_t pid,
                                   const mdbg::ElfSymbol& function) {
  if (function.size == 0) {
    throw std::runtime_error("entry-value function symbol has zero size");
  }
  return static_cast<std::uintptr_t>(elf.runtime_address(pid, function) + function.size - 1);
}

void test_missing_entry_snapshot(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const auto entry_function = elf.find_symbol("inspect_entry_parameter");
  require(entry_function.has_value(), "DWARF5 entry function symbol is missing");
  const auto entry_address = entry_value_address(elf, debugger.pid(), *entry_function);
  debugger.add_breakpoint(entry_address);

  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == entry_address,
          "DWARF5 fixture did not stop in the compiler entry-value range");
  require(debugger.registers().rdi == kExpectedEntryRdiSentinel,
          "entry-value range did not preserve the expected current RDI sentinel");

  bool missing_snapshot_failed = false;
  std::string missing_snapshot_error = "inspection unexpectedly succeeded";
  try {
    (void)mdbg::inspect_local_integer(debugger, elf, "entry_parameter");
  } catch (const std::runtime_error& error) {
    missing_snapshot_error = error.what();
    missing_snapshot_failed =
        missing_snapshot_error.find("observed function-entry breakpoint snapshot") !=
        std::string::npos;
  }
  require(missing_snapshot_failed,
          "DW_OP_entry_value must require historical entry state; actual result: " +
              missing_snapshot_error);

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "missing-snapshot fixture did not exit cleanly");
}

void test_direct_api(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const auto entry_function = elf.find_symbol("inspect_entry_parameter");
  const auto optimized_probe = elf.find_symbol("optimized_local_probe");
  require(entry_function.has_value(), "DWARF5 entry function symbol is missing");
  require(optimized_probe.has_value(), "DWARF5 optimized-local probe symbol is missing");
  const auto function_address = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *entry_function));
  const auto entry_address = entry_value_address(elf, debugger.pid(), *entry_function);
  const auto optimized_address = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *optimized_probe));
  debugger.add_breakpoint(function_address);
  debugger.add_breakpoint(entry_address);
  debugger.add_breakpoint(optimized_address);

  auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == function_address,
          "DWARF5 fixture did not expose the function-entry register state");
  require(debugger.registers().rdi == kExpectedEntryParameter,
          "function-entry breakpoint did not observe the original RDI parameter");

  stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == entry_address,
          "DWARF5 fixture did not stop in the compiler entry-value range");
  const auto current_rdi = debugger.registers().rdi;
  require(current_rdi == kExpectedEntryRdiSentinel,
          "entry-value range did not preserve the expected current RDI sentinel");

  const auto entry_value =
      mdbg::inspect_local_integer(debugger, elf, "entry_parameter");
  require(entry_value.name == "entry_parameter",
          "DWARF5 entry-value lookup returned wrong name");
  require(entry_value.raw_value == kExpectedEntryParameter,
          "DWARF5 entry-value lookup returned " + std::to_string(entry_value.raw_value) +
              " while current RDI is " + std::to_string(current_rdi));
  require(entry_value.byte_size == sizeof(std::uint64_t) && !entry_value.is_signed,
          "DWARF5 entry-value lookup returned wrong uint64_t type metadata");
  require(std::filesystem::equivalent(entry_value.module_path, fixture),
          "DWARF5 entry-value lookup lost owning module identity");

  stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == optimized_address,
          "DWARF5 fixture did not stop while optimized_local was live");

  const auto value = mdbg::inspect_local_integer(debugger, elf, "optimized_local");
  require(value.name == "optimized_local", "DWARF5 local lookup returned wrong name");
  require(value.raw_value == kExpectedOptimizedLocal,
          "DWARF5 local lookup returned wrong runtime value");
  require(value.byte_size == sizeof(std::uint64_t) && !value.is_signed,
          "DWARF5 local lookup returned wrong uint64_t type metadata");
  require(std::filesystem::equivalent(value.module_path, fixture),
          "DWARF5 local lookup lost owning module identity");

  bool missing_failed = false;
  try {
    (void)mdbg::inspect_local_integer(debugger, elf, "missing_optimized_local");
  } catch (const std::exception&) {
    missing_failed = true;
  }
  require(missing_failed, "missing DWARF5 local did not fail explicitly");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "DWARF5 optimized-local fixture did not exit cleanly");
}

std::string read_until(int fd, const std::string& needle,
                       std::chrono::steady_clock::time_point deadline) {
  std::string output;
  while (output.find(needle) == std::string::npos) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) throw std::runtime_error("timed out waiting for CLI output: " + needle);
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd descriptor{fd, POLLIN | POLLHUP, 0};
    const int polled = ::poll(&descriptor, 1, static_cast<int>(remaining));
    if (polled == -1 && errno == EINTR) continue;
    if (polled <= 0) throw std::runtime_error("timed out reading CLI output: " + needle);
    char buffer[512];
    const auto count = ::read(fd, buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("CLI exited before output: " + needle);
    output.append(buffer, static_cast<std::size_t>(count));
  }
  return output;
}

pid_t wait_for_tracee_child(pid_t mdbg_pid) {
  const auto children_path = std::filesystem::path("/proc") / std::to_string(mdbg_pid) /
                             "task" / std::to_string(mdbg_pid) / "children";
  for (int attempt = 0; attempt < 200; ++attempt) {
    std::ifstream input(children_path);
    pid_t tracee = -1;
    pid_t extra = -1;
    if ((input >> tracee) && !(input >> extra) && tracee > 0) return tracee;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  throw std::runtime_error("mdbg did not expose exactly one launched tracee child");
}

std::string run_cli(const std::string& mdbg_path, const std::string& fixture) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create DWARF5 CLI pipes");
  }

  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("failed to fork DWARF5 CLI");
  if (child == 0) {
    ::setpgid(0, 0);
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    ::execl(mdbg_path.c_str(), mdbg_path.c_str(), fixture.c_str(), nullptr);
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  std::string output = read_until(output_pipe[0], "(mdbg) ", deadline);

  const pid_t tracee = wait_for_tracee_child(child);
  const mdbg::ElfFile elf(fixture);
  const auto entry_function = elf.find_symbol("inspect_entry_parameter");
  require(entry_function.has_value(), "DWARF5 CLI entry function symbol is missing");
  const auto entry_address = entry_value_address(elf, tracee, *entry_function);

  std::ostringstream address_text;
  address_text << "0x" << std::hex << entry_address;
  const std::string script =
      "break inspect_entry_parameter\n"
      "break " + address_text.str() + "\n"
      "break optimized_local_probe\n"
      "continue\n"
      "continue\n"
      "print entry_parameter\n"
      "continue\n"
      "print optimized_local\n"
      "continue\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset,
                               script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write DWARF5 CLI script");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      ::kill(-child, SIGKILL);
      ::kill(child, SIGKILL);
      throw std::runtime_error("timed out waiting for DWARF5 CLI");
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
    const int polled = ::poll(&descriptor, 1, static_cast<int>(remaining));
    if (polled == -1 && errno == EINTR) continue;
    if (polled <= 0) throw std::runtime_error("timed out reading DWARF5 CLI output");
    char buffer[512];
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read DWARF5 CLI output");
    if (count == 0) break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(output_pipe[0]);

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "DWARF5 CLI did not exit cleanly\n" + output);
  return output;
}

void test_cli(const std::string& mdbg_path, const std::string& fixture) {
  const auto output = run_cli(mdbg_path, fixture);
  require(output.find("Breakpoint 1") != std::string::npos &&
              output.find("Breakpoint 2") != std::string::npos &&
              output.find("Breakpoint 3") != std::string::npos,
          "DWARF5 CLI did not install all source-value breakpoints\n" + output);
  require(output.find(kExpectedEntryCliValue) != std::string::npos,
          "DWARF5 CLI did not print the entry-value parameter\n" + output);
  require(output.find(kExpectedCliValue) != std::string::npos,
          "DWARF5 CLI did not print optimized_local\n" + output);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: dwarf5_local_value_integration <fixture> <mdbg>\n";
    return 2;
  }
  try {
    test_missing_entry_snapshot(argv[1]);
    test_direct_api(argv[1]);
    test_cli(argv[2], argv[1]);
    std::cout << "DWARF5 optimized local integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "DWARF5 local-value integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

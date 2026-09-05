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
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint64_t kExpectedOptimizedLocal = UINT64_C(0x1e3c1e781e3c1ef0);
constexpr const char* kExpectedCliValue = "optimized_local = 2178649820992642800";

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void test_direct_api(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const auto probe = elf.find_symbol("optimized_local_probe");
  require(probe.has_value(), "DWARF5 optimized-local probe symbol is missing");
  const auto address =
      static_cast<std::uintptr_t>(elf.runtime_address(debugger.pid(), *probe));
  debugger.add_breakpoint(address);

  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == address,
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
  const std::string script =
      "break optimized_local_probe\n"
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

  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
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
  require(output.find("Breakpoint 1") != std::string::npos,
          "DWARF5 CLI did not install optimized-local breakpoint\n" + output);
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
    test_direct_api(argv[1]);
    test_cli(argv[2], argv[1]);
    std::cout << "DWARF5 optimized local integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "DWARF5 local-value integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

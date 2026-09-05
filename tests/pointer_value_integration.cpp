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

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string run_cli(const std::string& integration_path, const std::string& fixture) {
  const auto mdbg =
      (std::filesystem::absolute(integration_path).parent_path() / "mdbg").string();
  require(std::filesystem::exists(mdbg), "mdbg executable is missing beside integration test");

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
    ::execl(mdbg.c_str(), mdbg.c_str(), fixture.c_str(), nullptr);
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  const std::string script =
      "break local_pointer_probe\n"
      "continue\n"
      "print local_pointer\n"
      "continue\n";
  std::size_t written = 0;
  while (written < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + written, script.size() - written);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write pointer CLI script");
    written += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      ::kill(-child, SIGKILL);
      ::kill(child, SIGKILL);
      throw std::runtime_error("timed out waiting for pointer CLI");
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
    const int polled = ::poll(&descriptor, 1, static_cast<int>(remaining));
    if (polled == -1 && errno == EINTR) continue;
    if (polled <= 0) throw std::runtime_error("timed out reading pointer CLI output");

    char buffer[512];
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read pointer CLI output");
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
          "pointer CLI tracee did not exit cleanly\n" + output);
  return output;
}

void test_pointer_api(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const auto probe = elf.find_symbol("local_pointer_probe");
  const auto target = elf.find_symbol("pointer_target");
  require(probe && target, "pointer fixture symbols are unavailable");

  const auto probe_address =
      static_cast<std::uintptr_t>(elf.runtime_address(debugger.pid(), *probe));
  const auto target_address =
      static_cast<std::uint64_t>(elf.runtime_address(debugger.pid(), *target));
  debugger.add_breakpoint(probe_address);
  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == probe_address,
          "pointer fixture did not stop while local_pointer was live");

  const auto value = mdbg::inspect_local_value(debugger, elf, "local_pointer");
  require(value.name == "local_pointer", "pointer lookup returned the wrong name");
  require(value.kind == mdbg::LocalValueKind::Pointer,
          "pointer lookup did not preserve pointer scalar type metadata");
  require(value.raw_value == target_address,
          "pointer lookup did not recover the live pointee address");
  require(value.byte_size == sizeof(std::uint64_t) && !value.is_signed,
          "pointer lookup returned the wrong pointer width/sign metadata");
  require(std::filesystem::equivalent(value.module_path, fixture),
          "pointer lookup lost owning module identity");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "pointer fixture did not exit cleanly after inspection");
}

void test_pointer_cli(const std::string& integration_path, const std::string& fixture) {
  const auto output = run_cli(integration_path, fixture);
  require(output.find("local_pointer = 0x") != std::string::npos,
          "CLI did not render the pointer scalar in hexadecimal\n" + output);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: pointer_value_integration <fixture>\n";
    return 2;
  }
  try {
    test_pointer_api(argv[1]);
    test_pointer_cli(argv[0], argv[1]);
    std::cout << "pointer value integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "pointer value integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

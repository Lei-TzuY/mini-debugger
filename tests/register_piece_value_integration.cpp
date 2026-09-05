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

constexpr std::uint64_t kFirst = UINT64_C(0x1122334455667788);
constexpr std::uint64_t kSecond = UINT64_C(0x99aabbccddeeff00);
constexpr const char* kCliValue =
    "pair = { first = 1234605616436508552, second = 11072869122414935808 }";

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void test_direct_api(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const auto probe = elf.find_symbol("register_pair_probe");
  require(probe.has_value(), "register-piece probe symbol is unavailable");
  const auto address = static_cast<std::uintptr_t>(elf.runtime_address(debugger.pid(), *probe));
  debugger.add_breakpoint(address);

  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint && stop.breakpoint_address == address,
          "register-piece fixture did not stop at the compiler-proven probe");

  const auto value = mdbg::inspect_local_value(debugger, elf, "pair");
  require(value.kind == mdbg::LocalValueKind::Structure,
          "register-piece formal parameter did not preserve structure kind");
  require(value.byte_size == 16 && value.members.size() == 2,
          "register-piece aggregate shape is incorrect");
  require(std::filesystem::equivalent(value.module_path, fixture),
          "register-piece aggregate lost module ownership");
  require(value.members[0].name == "first" && value.members[0].byte_size == 8 &&
              !value.members[0].is_signed && value.members[0].raw_value == kFirst,
          "first register piece was not reconstructed correctly");
  require(value.members[1].name == "second" && value.members[1].byte_size == 8 &&
              !value.members[1].is_signed && value.members[1].raw_value == kSecond,
          "second register piece was not reconstructed correctly");

  bool integer_api_rejected = false;
  try {
    (void)mdbg::inspect_local_integer(debugger, elf, "pair");
  } catch (const std::runtime_error&) {
    integer_api_rejected = true;
  }
  require(integer_api_rejected,
          "integer-only API accepted a register-piece structure value");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "register-piece fixture did not exit cleanly");
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

std::string run_cli(const std::string& mdbg_path, const std::string& fixture) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create register-piece CLI pipes");
  }

  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("failed to fork register-piece CLI");
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
  const std::string script =
      "break register_pair_probe\n"
      "continue\n"
      "print pair\n"
      "continue\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset, script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write register-piece CLI script");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      ::kill(-child, SIGKILL);
      ::kill(child, SIGKILL);
      throw std::runtime_error("timed out waiting for register-piece CLI");
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
    const int polled = ::poll(&descriptor, 1, static_cast<int>(remaining));
    if (polled == -1 && errno == EINTR) continue;
    if (polled <= 0) throw std::runtime_error("timed out reading register-piece CLI output");
    char buffer[512];
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read register-piece CLI output");
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
          "register-piece CLI did not exit cleanly");
  return output;
}

void test_cli(const std::string& fixture, const std::string& mdbg_path) {
  const auto output = run_cli(mdbg_path, fixture);
  require(output.find(kCliValue) != std::string::npos,
          "CLI did not render the reconstructed register-piece aggregate; output:\n" + output);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: register_piece_value_integration <fixture> <mdbg>\n";
    return 2;
  }
  try {
    test_direct_api(argv[1]);
    test_cli(argv[1], argv[2]);
    std::cout << "register-piece value integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "register-piece integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

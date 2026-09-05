#include "debugger/debugger.hpp"
#include "ptrace/ptrace.hpp"

#include <poll.h>
#include <sys/wait.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

struct FixtureAddresses {
  std::uintptr_t one;
  std::uintptr_t two;
  std::uintptr_t value;
};

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string temp_path() {
  char pattern[] = "/tmp/mdbg-memory-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd == -1) throw std::runtime_error("mkstemp failed");
  ::close(fd);
  ::unlink(pattern);
  return pattern;
}

FixtureAddresses read_addresses(const std::string& path) {
  std::ifstream input(path);
  std::string one, two, value;
  input >> one >> two >> value;
  if (!input) throw std::runtime_error("fixture did not publish addresses");
  return {static_cast<std::uintptr_t>(std::stoull(one, nullptr, 0)),
          static_cast<std::uintptr_t>(std::stoull(two, nullptr, 0)),
          static_cast<std::uintptr_t>(std::stoull(value, nullptr, 0))};
}

std::vector<std::byte> encode_u64(std::uint64_t value) {
  std::vector<std::byte> bytes(sizeof(value));
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
  return bytes;
}

std::uint64_t decode_u64(const std::vector<std::byte>& bytes) {
  require(bytes.size() == sizeof(std::uint64_t), "unexpected uint64 readback size");
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[index]))
             << (index * 8U);
  }
  return value;
}

struct Session {
  std::string path;
  mdbg::Debugger debugger;
  FixtureAddresses addresses;

  explicit Session(const std::string& fixture)
      : path(temp_path()), debugger(mdbg::Debugger::launch(fixture, {path, "sequence"})),
        addresses{} {
    require(debugger.stop_info().reason == mdbg::StopReason::InitialExec,
            "launch did not stop after exec");
    const auto sync = debugger.continue_execution();
    require(sync.reason == mdbg::StopReason::Signal && sync.value == SIGSTOP,
            "fixture synchronization SIGSTOP was not observed");
    addresses = read_addresses(path);
  }

  ~Session() { std::remove(path.c_str()); }
};

void test_bounded_data_write(const std::string& fixture) {
  Session session(fixture);
  constexpr std::uint64_t kOriginal = 0x1122334455667788ULL;
  constexpr std::uint64_t kReplacement = 0x8877665544332211ULL;

  const auto watchpoint =
      session.debugger.add_write_watchpoint(session.addresses.value, sizeof(std::uint64_t));
  const auto stop_before = session.debugger.stop_info();
  session.debugger.write_memory(session.addresses.value, encode_u64(kReplacement));
  require(decode_u64(session.debugger.read_memory(session.addresses.value,
                                                  sizeof(std::uint64_t))) == kReplacement,
          "ordinary debugger memory write did not round-trip");
  require(session.debugger.stop_info().reason == stop_before.reason &&
              session.debugger.stop_info().tid == stop_before.tid,
          "debugger-originated memory write fabricated a stop");
  require(session.debugger.remove_watchpoint(watchpoint),
          "watchpoint could not be removed after debugger-originated write");

  bool empty_rejected = false;
  try {
    session.debugger.write_memory(session.addresses.value, {});
  } catch (const std::invalid_argument&) {
    empty_rejected = true;
  }
  require(empty_rejected, "empty memory write was not rejected");

  bool oversized_rejected = false;
  try {
    session.debugger.write_memory(session.addresses.value,
                                  std::vector<std::byte>(4097, std::byte{0}));
  } catch (const std::invalid_argument&) {
    oversized_rejected = true;
  }
  require(oversized_rejected, "memory write bound was not enforced");

  bool ptrace_error = false;
  try {
    session.debugger.write_memory(1, {std::byte{0x7f}});
  } catch (const mdbg::lowlevel::PtraceError&) {
    ptrace_error = true;
  }
  require(ptrace_error, "invalid memory write did not preserve ptrace failure reporting");

  session.debugger.write_memory(session.addresses.value, encode_u64(kOriginal));
  const auto exit = session.debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "fixture did not exit cleanly after ordinary memory mutation was restored");
}

void test_installed_breakpoint_overlap(const std::string& fixture) {
  Session session(fixture);
  const auto original = session.debugger.read_memory(session.addresses.one, 1).front();
  const auto replacement = original == std::byte{0x90} ? std::byte{0x91} : std::byte{0x90};

  const auto id = session.debugger.add_breakpoint(session.addresses.one);
  session.debugger.write_memory(session.addresses.one, {replacement});

  require(session.debugger.read_memory(session.addresses.one, 1).front() == std::byte{0xcc},
          "installed breakpoint was physically overwritten by debugger memory write");
  const auto breakpoints = session.debugger.breakpoints();
  require(breakpoints.size() == 1 && breakpoints.front().id == id &&
              breakpoints.front().original_byte == replacement && breakpoints.front().installed,
          "breakpoint saved byte did not adopt the debugger memory write");

  require(session.debugger.remove_breakpoint(id),
          "managed breakpoint could not be removed after overlapping memory write");
  require(session.debugger.read_memory(session.addresses.one, 1).front() == replacement,
          "breakpoint removal restored stale pre-mutation program byte");

  session.debugger.write_memory(session.addresses.one, {original});
  const auto exit = session.debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "fixture did not exit cleanly after mutated code byte was restored");
}

void test_pending_displaced_step_overlap_is_rejected(const std::string& fixture) {
  Session session(fixture);
  const auto before = session.debugger.read_memory(session.addresses.one - 1, 2);
  session.debugger.add_breakpoint(session.addresses.one);

  const auto hit = session.debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint &&
              hit.breakpoint_address == session.addresses.one,
          "managed breakpoint did not reach pending displaced-step state");

  bool rejected = false;
  try {
    session.debugger.write_memory(session.addresses.one - 1,
                                  {std::byte{0x90}, std::byte{0x90}});
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "write spanning pending displaced breakpoint was not rejected");
  require(session.debugger.read_memory(session.addresses.one - 1, 2) == before,
          "rejected pending-step overlap partially mutated inferior memory");

  const auto second_hit = session.debugger.continue_execution();
  require(second_hit.reason == mdbg::StopReason::Breakpoint &&
              second_hit.breakpoint_address == session.addresses.one,
          "breakpoint did not survive rejected pending-step mutation");
  const auto id = session.debugger.breakpoints().front().id;
  require(session.debugger.remove_breakpoint(id),
          "breakpoint could not be removed after second hit");
  const auto exit = session.debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "fixture did not exit cleanly after pending-step rejection");
}

struct CliProcess {
  pid_t pid{-1};
  int input{-1};
  int output{-1};

  CliProcess() = default;
  CliProcess(const CliProcess&) = delete;
  CliProcess& operator=(const CliProcess&) = delete;

  ~CliProcess() {
    if (input != -1) ::close(input);
    if (output != -1) ::close(output);
    if (pid > 0) {
      ::kill(-pid, SIGKILL);
      ::kill(pid, SIGKILL);
      int status = 0;
      while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {
      }
    }
  }
};

CliProcess spawn_cli(const std::string& mdbg, const std::string& fixture,
                     const std::string& address_path) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create CLI pipes");
  }

  const pid_t pid = ::fork();
  if (pid == -1) throw std::runtime_error("failed to fork CLI");
  if (pid == 0) {
    ::setpgid(0, 0);
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    ::execl(mdbg.c_str(), mdbg.c_str(), fixture.c_str(), address_path.c_str(), "sequence",
            nullptr);
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  CliProcess process;
  process.pid = pid;
  process.input = input_pipe[1];
  process.output = output_pipe[0];
  return process;
}

void write_all(int fd, const std::string& data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const auto written = ::write(fd, data.data() + offset, data.size() - offset);
    if (written == -1 && errno == EINTR) continue;
    if (written <= 0) throw std::runtime_error("failed to write CLI command script");
    offset += static_cast<std::size_t>(written);
  }
}

std::string read_to_eof(int fd) {
  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) throw std::runtime_error("timed out waiting for CLI exit");
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd descriptor{fd, POLLIN | POLLHUP, 0};
    const int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
    if (result == -1 && errno == EINTR) continue;
    if (result <= 0) throw std::runtime_error("timed out reading CLI output");

    char buffer[1024];
    const auto count = ::read(fd, buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed reading CLI output");
    if (count == 0) return output;
    output.append(buffer, static_cast<std::size_t>(count));
  }
}

void test_cli_memory_write_round_trip(const std::string& fixture, const std::string& mdbg) {
  const auto path = temp_path();
  auto cli = spawn_cli(mdbg, fixture, path);
  const std::string script =
      "continue\n"
      "set memory fixture_value 0x11 0x22 0x33 0x44 0x55 0x66 0x77 0x88\n"
      "x fixture_value 8\n"
      "set memory fixture_value 0x88 0x77 0x66 0x55 0x44 0x33 0x22 0x11\n"
      "continue\n";
  write_all(cli.input, script);
  ::close(cli.input);
  cli.input = -1;

  const auto output = read_to_eof(cli.output);
  ::close(cli.output);
  cli.output = -1;

  int status = 0;
  pid_t result;
  do {
    result = ::waitpid(cli.pid, &status, 0);
  } while (result == -1 && errno == EINTR);
  require(result == cli.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "mdbg CLI did not exit cleanly after memory mutation workflow");
  cli.pid = -1;
  std::remove(path.c_str());

  require(output.find("wrote 8 bytes at 0x") != std::string::npos,
          "CLI did not report the memory write");
  require(output.find(": 11 22 33 44 55 66 77 88") != std::string::npos,
          "CLI x command did not read back the mutated byte sequence");
  require(output.find("process exited with code 0") != std::string::npos,
          "CLI tracee did not exit cleanly after restoring original data");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: memory_mutation_integration <fixture> <mdbg>\n";
    return 2;
  }
  try {
    const std::string fixture = argv[1];
    const std::string mdbg = argv[2];
    test_bounded_data_write(fixture);
    test_installed_breakpoint_overlap(fixture);
    test_pending_displaced_step_overlap_is_rejected(fixture);
    test_cli_memory_write_round_trip(fixture, mdbg);
    std::cout << "memory mutation integration tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << "memory mutation integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

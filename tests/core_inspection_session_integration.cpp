#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr const char* kExpectedCaller = "inspect_entry_parameter";
constexpr const char* kExpectedValue = "0x458a30bf63ac1619";

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string generate_core(const std::string& fixture) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for core-session fixture");
  if (child == 0) {
    rlimit core_limit{};
    if (::getrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(120);
    core_limit.rlim_cur = core_limit.rlim_max;
    if (::setrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(121);
    ::execl(fixture.c_str(), fixture.c_str(), "--snapshot-crash", nullptr);
    _exit(127);
  }

  const auto core_path = "/tmp/mdbg-core-" + std::to_string(child);
  std::remove(core_path.c_str());
  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  require(waited == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
          "core-session fixture did not terminate from deterministic SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce the core-session snapshot");
  return core_path;
}

std::string run_core_cli(const std::string& cli, const std::string& core_path) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create core-session CLI pipes");
  }

  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for core-session CLI");
  if (child == 0) {
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    ::execl(cli.c_str(), cli.c_str(), core_path.c_str(), nullptr);
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  const std::string script =
      "bt\n"
      "frame 1\n"
      "print transformed\n"
      "continue\n"
      "frame 999\n"
      "quit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset, script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write core-session commands");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read core-session output");
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
          "mdbg-core did not exit cleanly");
  return output;
}

void test_core_session(const std::string& fixture, const std::string& cli) {
  const auto core_path = generate_core(fixture);
  try {
    const auto output = run_core_cli(cli, core_path);
    require(output.find("core signal 11 tid ") != std::string::npos,
            "core session did not report immutable crash identity");
    require(output.find("#0 0x") != std::string::npos &&
                output.find("#1 0x") != std::string::npos,
            "core session backtrace did not expose crash and caller frames");
    require(output.find(fixture + "!" + kExpectedCaller) != std::string::npos,
            "core session caller frame lost module-qualified symbol ownership");
    require(output.find("selected frame 1") != std::string::npos,
            "core session did not select the recovered caller frame");
    require(output.find("!transformed = " + std::string(kExpectedValue)) !=
                std::string::npos,
            "core session did not evaluate the historical caller source value");
    require(output.find("unsupported in core session: continue") != std::string::npos,
            "core session exposed a live execution command");
    require(output.find("error: core frame index is out of range") != std::string::npos,
            "core session did not reject invalid immutable frame selection");
  } catch (...) {
    std::remove(core_path.c_str());
    throw;
  }
  std::remove(core_path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_inspection_session_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    test_core_session(argv[1], argv[2]);
    std::cout << "core inspection session integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "core inspection session failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

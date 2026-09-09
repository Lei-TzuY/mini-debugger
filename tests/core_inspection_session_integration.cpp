#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kExpectedCaller = "inspect_entry_parameter";
constexpr const char* kExpectedValue = "0x458a30bf63ac1619";

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string temp_path() {
  char pattern[] = "/tmp/mdbg-core-thread-ready-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd == -1) throw std::runtime_error("mkstemp failed for core thread fixture");
  ::close(fd);
  ::unlink(pattern);
  return pattern;
}

struct GeneratedCore {
  std::string path;
  pid_t crash_tid;
  pid_t sibling_tid;
};

GeneratedCore generate_core(const std::string& fixture) {
  const auto ready_path = temp_path();
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for core-session fixture");
  if (child == 0) {
    rlimit core_limit{};
    if (::getrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(120);
    core_limit.rlim_cur = core_limit.rlim_max;
    if (::setrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(121);
    ::execl(fixture.c_str(), fixture.c_str(), "--snapshot-crash-threaded",
            ready_path.c_str(), nullptr);
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
  while ((!std::filesystem::exists(core_path) || !std::filesystem::exists(ready_path)) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce the core-session snapshot");
  require(std::filesystem::exists(ready_path),
          "threaded core fixture did not publish its sibling TID");

  std::ifstream ready(ready_path);
  long sibling = -1;
  ready >> sibling;
  std::remove(ready_path.c_str());
  require(ready && sibling > 0 && sibling != child,
          "threaded core fixture published an invalid sibling TID");
  return GeneratedCore{core_path, child, static_cast<pid_t>(sibling)};
}

struct CliResult {
  int exit_code;
  std::string output;
};

CliResult run_core_cli_process(const std::string& cli,
                               const std::vector<std::string>& arguments,
                               const std::string& script) {
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
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(cli.c_str()));
    for (const auto& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(cli.c_str(), argv.data());
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
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
  require(waited == child && WIFEXITED(status), "mdbg-core did not exit normally");
  return CliResult{WEXITSTATUS(status), std::move(output)};
}

std::string session_script(const GeneratedCore& core) {
  return "threads\n"
         "thread " + std::to_string(core.sibling_tid) + "\n"
         "bt\n"
         "thread " + std::to_string(core.crash_tid) + "\n"
         "bt\n"
         "frame 1\n"
         "print transformed\n"
         "continue\n"
         "thread 999999999\n"
         "frame 999\n"
         "quit\n";
}

std::string run_core_cli(const std::string& cli, const GeneratedCore& core) {
  const auto result = run_core_cli_process(cli, {core.path}, session_script(core));
  require(result.exit_code == 0, "mdbg-core did not exit cleanly");
  return result.output;
}

void test_core_session(const std::string& fixture, const std::string& cli) {
  const auto core = generate_core(fixture);
  try {
    const auto output = run_core_cli(cli, core);
    require(output.find("core signal 11 tid " + std::to_string(core.crash_tid)) !=
                std::string::npos,
            "core session did not report immutable crash identity");
    require(output.find("* tid " + std::to_string(core.crash_tid) + " crash") !=
                std::string::npos,
            "core thread catalogue did not mark the selected crash thread");
    require(output.find("tid " + std::to_string(core.sibling_tid)) != std::string::npos,
            "core thread catalogue lost the real sibling thread");
    require(output.find("selected thread " + std::to_string(core.sibling_tid)) !=
                std::string::npos,
            "core session did not select the immutable sibling thread");
    require(output.find("selected thread " + std::to_string(core.crash_tid)) !=
                std::string::npos,
            "core session did not restore the immutable crash thread");
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
    require(output.find("error: core thread TID is unavailable") != std::string::npos,
            "core session did not reject an unavailable immutable thread");
    require(output.find("error: core frame index is out of range") != std::string::npos,
            "core session did not reject invalid immutable frame selection");
  } catch (...) {
    std::remove(core.path.c_str());
    throw;
  }
  std::remove(core.path.c_str());
}

void test_relocated_core_module_mapping(const std::string& fixture,
                                        const std::string& cli) {
  const auto recorded_path = temp_path();
  std::filesystem::copy_file(fixture, recorded_path,
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::permissions(recorded_path,
                               std::filesystem::status(fixture).permissions());
  const auto core = generate_core(recorded_path);
  std::remove(recorded_path.c_str());

  try {
    const auto no_map = run_core_cli_process(cli, {core.path}, "quit\n");
    require(no_map.exit_code != 0,
            "relocated core unexpectedly resolved its deleted recorded module path");

    const auto mapped = run_core_cli_process(
        cli, {"--module-map", recorded_path, fixture, core.path},
        "bt\nframe 1\nprint transformed\nquit\n");
    require(mapped.exit_code == 0,
            "explicit core module mapping did not restore the read-only session\n" +
                mapped.output);
    require(mapped.output.find(recorded_path + "!" + kExpectedCaller) !=
                std::string::npos,
            "mapped core session lost the recorded NT_FILE module identity");
    require(mapped.output.find("!transformed = " + std::string(kExpectedValue)) !=
                std::string::npos,
            "mapped core session did not recover caller source-value inspection");
    require(mapped.output.find(fixture + "!") == std::string::npos,
            "mapped core session leaked the local backing path as historical module identity");
  } catch (...) {
    std::remove(core.path.c_str());
    throw;
  }
  std::remove(core.path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_inspection_session_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    test_core_session(argv[1], argv[2]);
    test_relocated_core_module_mapping(argv[1], argv[2]);
    std::cout << "core inspection session integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "core inspection session failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

#include "debugger/debugger.hpp"

#include <poll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
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

constexpr std::uint64_t kRegisterMutationSeed = 0x13579bdf2468ace0ULL;
constexpr std::uint64_t kRegisterMutationValue = 0xa5a55a5ac3c33c3cULL;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string temp_path() {
  char pattern[] = "/tmp/mdbg-thread-attach-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd == -1) throw std::runtime_error("mkstemp failed");
  ::close(fd);
  ::unlink(pattern);
  return pattern;
}

std::vector<pid_t> task_ids(pid_t leader) {
  std::vector<pid_t> result;
  const auto path = std::filesystem::path("/proc") / std::to_string(leader) / "task";
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    result.push_back(static_cast<pid_t>(std::stol(entry.path().filename().string())));
  }
  std::sort(result.begin(), result.end());
  return result;
}

pid_t tracer_pid(pid_t leader, pid_t tid) {
  std::ifstream input(std::filesystem::path("/proc") / std::to_string(leader) / "task" /
                      std::to_string(tid) / "status");
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("TracerPid:", 0) == 0) {
      return static_cast<pid_t>(std::stol(line.substr(std::string("TracerPid:").size())));
    }
  }
  throw std::runtime_error("task status did not contain TracerPid");
}

std::uintptr_t first_breakpoint_address(const std::string& path) {
  std::ifstream input(path);
  std::string one, two, value;
  input >> one >> two >> value;
  if (!input) throw std::runtime_error("fixture did not publish breakpoint addresses");
  return static_cast<std::uintptr_t>(std::stoull(one, nullptr, 0));
}

struct Child {
  pid_t pid{-1};
  std::string path;
};

Child spawn_threaded_fixture(const std::string& fixture, const char* mode) {
  Child child{-1, temp_path()};
  child.pid = ::fork();
  if (child.pid == -1) throw std::runtime_error("fork failed");
  if (child.pid == 0) {
    ::execl(fixture.c_str(), fixture.c_str(), child.path.c_str(), mode, nullptr);
    _exit(127);
  }

  for (int attempt = 0; attempt < 200; ++attempt) {
    if (std::filesystem::exists(child.path)) return child;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ::kill(child.pid, SIGKILL);
  int status = 0;
  while (::waitpid(child.pid, &status, 0) == -1 && errno == EINTR) {
  }
  std::remove(child.path.c_str());
  throw std::runtime_error("threaded fixture did not become ready");
}

void terminate_child(const Child& child) noexcept {
  if (child.pid > 0) {
    ::kill(child.pid, SIGKILL);
    int status = 0;
    while (::waitpid(child.pid, &status, 0) == -1 && errno == EINTR) {
    }
  }
  std::remove(child.path.c_str());
}

void release_and_require_clean_exit(const Child& child) {
  require(::kill(child.pid, SIGUSR1) == 0, "failed to release detached fixture");
  int status = 0;
  pid_t result;
  do {
    result = ::waitpid(child.pid, &status, 0);
  } while (result == -1 && errno == EINTR);
  require(result == child.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "detached threaded fixture did not exit cleanly");
  std::remove(child.path.c_str());
}

void release_register_fixture(const Child& child, pid_t worker) {
  require(::syscall(SYS_tgkill, child.pid, worker, SIGUSR2) == 0,
          "failed to release register-mutation worker");
  int status = 0;
  pid_t result;
  do {
    result = ::waitpid(child.pid, &status, 0);
  } while (result == -1 && errno == EINTR);
  require(result == child.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "register-mutation fixture did not observe the mutated worker register");
  std::remove(child.path.c_str());
}

void require_tracer_state(pid_t leader, const std::vector<pid_t>& tids, pid_t tracer,
                          const std::string& context) {
  for (const auto tid : tids) {
    require(tracer_pid(leader, tid) == tracer,
            context + " for TID " + std::to_string(tid));
  }
}

pid_t worker_tid(pid_t leader, const std::vector<pid_t>& tids) {
  for (const auto tid : tids) {
    if (tid != leader) return tid;
  }
  throw std::runtime_error("worker TID is unavailable");
}

struct CliProcess {
  pid_t pid{-1};
  int input{-1};
  int output{-1};
};

CliProcess spawn_cli(const std::string& mdbg, const std::string& fixture) {
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
    ::execl(mdbg.c_str(), mdbg.c_str(), fixture.c_str(), "thread-lifecycle", nullptr);
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  return CliProcess{pid, input_pipe[1], output_pipe[0]};
}

CliProcess spawn_attach_cli(const std::string& mdbg, pid_t tracee) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create attach CLI pipes");
  }

  const pid_t pid = ::fork();
  if (pid == -1) throw std::runtime_error("failed to fork attach CLI");
  if (pid == 0) {
    ::setpgid(0, 0);
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    const auto pid_text = std::to_string(tracee);
    ::execl(mdbg.c_str(), mdbg.c_str(), "--attach", pid_text.c_str(), nullptr);
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  return CliProcess{pid, input_pipe[1], output_pipe[0]};
}

void terminate_cli(CliProcess& cli) noexcept {
  if (cli.input != -1) {
    ::close(cli.input);
    cli.input = -1;
  }
  if (cli.output != -1) {
    ::close(cli.output);
    cli.output = -1;
  }
  if (cli.pid > 0) {
    ::kill(-cli.pid, SIGKILL);
    ::kill(cli.pid, SIGKILL);
    int status = 0;
    while (::waitpid(cli.pid, &status, 0) == -1 && errno == EINTR) {
    }
    cli.pid = -1;
  }
}

void write_cli(CliProcess& cli, const std::string& command) {
  std::size_t offset = 0;
  while (offset < command.size()) {
    const auto written = ::write(cli.input, command.data() + offset, command.size() - offset);
    if (written == -1 && errno == EINTR) continue;
    if (written <= 0) throw std::runtime_error("failed to write CLI command");
    offset += static_cast<std::size_t>(written);
  }
}

std::string read_cli_until(CliProcess& cli, const std::string& needle) {
  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (output.find(needle) == std::string::npos) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) throw std::runtime_error("timed out waiting for CLI output: " + needle);
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd descriptor{cli.output, POLLIN | POLLHUP, 0};
    const int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
    if (result == -1 && errno == EINTR) continue;
    if (result <= 0) throw std::runtime_error("timed out reading CLI output: " + needle);

    char buffer[512];
    const auto count = ::read(cli.output, buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("CLI exited before output: " + needle);
    output.append(buffer, static_cast<std::size_t>(count));
  }
  return output;
}

std::string read_cli_to_eof(CliProcess& cli) {
  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) throw std::runtime_error("timed out waiting for CLI exit");
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd descriptor{cli.output, POLLIN | POLLHUP, 0};
    const int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
    if (result == -1 && errno == EINTR) continue;
    if (result <= 0) throw std::runtime_error("timed out waiting for CLI EOF");

    char buffer[512];
    const auto count = ::read(cli.output, buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count == 0) break;
    if (count < 0) throw std::runtime_error("failed to read CLI output");
    output.append(buffer, static_cast<std::size_t>(count));
  }
  return output;
}

std::uint64_t register_output_value(const std::string& output, const std::string& name) {
  const auto marker = name + " = 0x";
  const auto begin = output.find(marker);
  if (begin == std::string::npos) {
    throw std::runtime_error("CLI did not print register " + name);
  }
  std::size_t consumed = 0;
  const auto value = std::stoull(output.substr(begin + marker.size()), &consumed, 16);
  if (consumed == 0) throw std::runtime_error("CLI register value is malformed");
  return value;
}

pid_t created_thread_tid(const std::string& output) {
  const std::string prefix = "thread ";
  const std::string suffix = " created and stopped";
  const auto begin = output.find(prefix);
  if (begin == std::string::npos) throw std::runtime_error("CLI did not report thread creation");
  const auto digits = begin + prefix.size();
  const auto end = output.find(suffix, digits);
  if (end == std::string::npos) throw std::runtime_error("CLI thread creation output is malformed");
  return static_cast<pid_t>(std::stol(output.substr(digits, end - digits)));
}

std::vector<pid_t> listed_thread_ids(const std::string& output) {
  std::vector<pid_t> result;
  std::istringstream lines(output);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    std::string first;
    fields >> first;
    if (first.empty()) continue;
    if (first == "*") fields >> first;
    try {
      std::size_t consumed = 0;
      const auto value = std::stoll(first, &consumed, 10);
      std::string state;
      fields >> state;
      if (consumed == first.size() && value > 0 && state == "stopped") {
        result.push_back(static_cast<pid_t>(value));
      }
    } catch (const std::exception&) {
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

void test_explicit_multithread_detach(const std::string& fixture) {
  const auto child = spawn_threaded_fixture(fixture, "attach-threaded");
  try {
    const auto before = task_ids(child.pid);
    require(before.size() >= 2, "fixture did not expose a pre-existing worker");
    const auto worker = worker_tid(child.pid, before);

    auto debugger = mdbg::Debugger::attach(child.pid);
    require(debugger.active_tid() == child.pid,
            "attach must select the thread-group leader as initial active TID");
    require_tracer_state(child.pid, before, ::getpid(), "attach did not trace every existing TID");

    bool invalid_rejected = false;
    try {
      debugger.select_thread(999999999);
    } catch (const std::invalid_argument&) {
      invalid_rejected = true;
    }
    require(invalid_rejected, "untracked TID selection was not rejected deterministically");

    debugger.select_thread(worker);
    require(debugger.active_tid() == worker,
            "worker did not remain active before explicit detach");
    debugger.detach();
    require(debugger.state() == mdbg::ProcessState::Detached,
            "explicit detach did not transition to Detached");
    require_tracer_state(child.pid, before, 0,
                         "explicit detach after thread selection left a TID traced");
    release_and_require_clean_exit(child);
  } catch (...) {
    terminate_child(child);
    throw;
  }
}

void test_destructor_multithread_detach(const std::string& fixture) {
  const auto child = spawn_threaded_fixture(fixture, "attach-threaded");
  try {
    const auto before = task_ids(child.pid);
    require(before.size() >= 2, "fixture did not expose a pre-existing worker");
    {
      auto debugger = mdbg::Debugger::attach(child.pid);
      require_tracer_state(child.pid, before, ::getpid(),
                           "destructor setup did not trace every existing TID");
      debugger.select_thread(worker_tid(child.pid, before));
    }
    require_tracer_state(child.pid, before, 0,
                         "debugger destructor after thread selection left a TID traced");
    release_and_require_clean_exit(child);
  } catch (...) {
    terminate_child(child);
    throw;
  }
}

void test_explicit_thread_selection_progress(const std::string& fixture) {
  const auto child = spawn_threaded_fixture(fixture, "attach-thread-select");
  try {
    const auto before = task_ids(child.pid);
    require(before.size() == 2, "selection fixture must expose one leader and one worker");
    const auto worker = worker_tid(child.pid, before);
    const auto breakpoint_address = first_breakpoint_address(child.path);

    auto debugger = mdbg::Debugger::attach(child.pid);
    const auto initial_threads = debugger.threads();
    require(initial_threads.size() == 2, "debugger thread registry size mismatch after attach");
    for (const auto& thread : initial_threads) {
      require(thread.state == mdbg::ProcessState::Stopped,
              "all attached threads must be stopped before selection");
      require(thread.active == (thread.tid == child.pid),
              "initial active-thread marker must identify the leader");
    }

    debugger.select_thread(worker);
    require(debugger.active_tid() == worker,
            "explicit selection did not make the worker active");
    require(debugger.registers().rip != 0,
            "register reads did not follow the selected worker");
    const auto breakpoint_id = debugger.add_breakpoint(breakpoint_address);

    require(::syscall(SYS_tgkill, child.pid, worker, SIGUSR2) == 0,
            "failed to queue targeted worker release signal");
    auto info = debugger.continue_execution();
    require(info.reason == mdbg::StopReason::Signal && info.value == SIGUSR2 &&
                info.tid == worker,
            "selected worker did not surface its targeted signal-delivery stop");

    debugger.select_thread(child.pid);
    require(debugger.active_tid() == child.pid,
            "selection could not switch back to the stopped leader");
    debugger.select_thread(worker);
    info = debugger.continue_execution(mdbg::SignalPolicy::Forward);
    require(info.reason == mdbg::StopReason::Breakpoint && info.tid == worker &&
                info.breakpoint_address == breakpoint_address,
            "forwarding the worker-owned signal did not release it to the managed breakpoint");

    bool switch_blocked = false;
    try {
      debugger.select_thread(child.pid);
    } catch (const std::logic_error&) {
      switch_blocked = true;
    }
    require(switch_blocked,
            "thread switch was allowed while the process-wide breakpoint byte was restored");

    info = debugger.continue_execution();
    require(info.reason == mdbg::StopReason::ThreadExited && info.tid == worker &&
                info.value == 0,
            "selected worker did not complete and exit after its displaced breakpoint step");
    require(debugger.active_tid() == child.pid,
            "stopped leader did not become active after the selected worker exited");

    bool exited_rejected = false;
    try {
      debugger.select_thread(worker);
    } catch (const std::invalid_argument&) {
      exited_rejected = true;
    }
    require(exited_rejected, "exited worker TID remained selectable");
    require(debugger.remove_breakpoint(breakpoint_id),
            "managed breakpoint could not be removed after worker exit");

    info = debugger.continue_execution();
    require(info.reason == mdbg::StopReason::Exited && info.tid == child.pid &&
                info.value == 0,
            "leader did not complete after worker scheduling resolved pthread_join");
    std::remove(child.path.c_str());
  } catch (...) {
    terminate_child(child);
    throw;
  }
}

void test_cli_thread_selection(const std::string& fixture, const std::string& mdbg) {
  auto cli = spawn_cli(mdbg, fixture);
  try {
    const auto initial = read_cli_until(cli, "(mdbg) ");
    require(initial.find("stopped after exec") != std::string::npos,
            "CLI did not reach the initial exec stop");

    write_cli(cli, "continue\n");
    const auto created = read_cli_until(cli, "(mdbg) ");
    const auto worker = created_thread_tid(created);

    write_cli(cli, "info threads\n");
    const auto initial_listing = read_cli_until(cli, "(mdbg) ");
    const auto tids = listed_thread_ids(initial_listing);
    require(tids.size() == 2, "CLI did not list both stopped threads");
    require(std::find(tids.begin(), tids.end(), worker) != tids.end(),
            "CLI thread list omitted the created worker");
    const auto leader = tids.front() == worker ? tids.back() : tids.front();

    write_cli(cli, "thread " + std::to_string(leader) + "\n");
    auto selected = read_cli_until(cli, "(mdbg) ");
    require(selected.find("selected thread " + std::to_string(leader)) != std::string::npos,
            "CLI did not acknowledge leader selection");

    write_cli(cli, "info threads\n");
    auto listing = read_cli_until(cli, "(mdbg) ");
    require(listing.find("* " + std::to_string(leader) + " stopped") != std::string::npos,
            "CLI did not mark the selected leader active");

    write_cli(cli, "thread " + std::to_string(worker) + "\n");
    selected = read_cli_until(cli, "(mdbg) ");
    require(selected.find("selected thread " + std::to_string(worker)) != std::string::npos,
            "CLI did not acknowledge worker selection");

    write_cli(cli, "info threads\n");
    listing = read_cli_until(cli, "(mdbg) ");
    require(listing.find("* " + std::to_string(worker) + " stopped") != std::string::npos,
            "CLI did not mark the selected worker active");

    write_cli(cli, "regs\n");
    const auto registers = read_cli_until(cli, "(mdbg) ");
    require(registers.find("rip") != std::string::npos,
            "CLI register read failed after worker selection");

    write_cli(cli, "continue\n");
    const auto worker_exit = read_cli_until(cli, "(mdbg) ");
    require(worker_exit.find("thread " + std::to_string(worker) + " exited with code 0") !=
                std::string::npos,
            "CLI execution did not follow the selected worker through exit");

    write_cli(cli, "info threads\n");
    const auto final_listing = read_cli_until(cli, "(mdbg) ");
    require(final_listing.find(std::to_string(worker) + " stopped") == std::string::npos,
            "CLI retained an exited worker in the thread list");
    require(final_listing.find("* " + std::to_string(leader) + " stopped") !=
                std::string::npos,
            "CLI did not return active ownership to the stopped leader");

    write_cli(cli, "continue\n");
    ::close(cli.input);
    cli.input = -1;
    const auto exited = read_cli_to_eof(cli);
    require(exited.find("process exited with code 0") != std::string::npos,
            "CLI tracee did not exit cleanly after worker scheduling");

    int status = 0;
    pid_t result;
    do {
      result = ::waitpid(cli.pid, &status, 0);
    } while (result == -1 && errno == EINTR);
    require(result == cli.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "mdbg CLI process did not exit cleanly");
    cli.pid = -1;
    ::close(cli.output);
    cli.output = -1;
  } catch (...) {
    terminate_cli(cli);
    throw;
  }
}

void test_cli_selected_register_mutation(const std::string& fixture, const std::string& mdbg) {
  const auto child = spawn_threaded_fixture(fixture, "attach-register-mutation");
  auto cli = CliProcess{};
  try {
    const auto tids = task_ids(child.pid);
    require(tids.size() == 2, "register fixture must expose one leader and one worker");
    const auto worker = worker_tid(child.pid, tids);

    cli = spawn_attach_cli(mdbg, child.pid);
    const auto attached = read_cli_until(cli, "(mdbg) ");
    require(attached.find("attached to process " + std::to_string(child.pid)) !=
                std::string::npos,
            "CLI did not attach to register-mutation fixture");

    write_cli(cli, "thread " + std::to_string(worker) + "\n");
    auto output = read_cli_until(cli, "(mdbg) ");
    require(output.find("selected thread " + std::to_string(worker)) != std::string::npos,
            "CLI did not select register-mutation worker");

    write_cli(cli, "reg r12\n");
    output = read_cli_until(cli, "(mdbg) ");
    require(register_output_value(output, "r12") == kRegisterMutationSeed,
            "worker did not expose deterministic r12 seed");

    write_cli(cli, "thread " + std::to_string(child.pid) + "\n");
    read_cli_until(cli, "(mdbg) ");
    write_cli(cli, "reg r12\n");
    output = read_cli_until(cli, "(mdbg) ");
    const auto leader_r12 = register_output_value(output, "r12");

    write_cli(cli, "thread " + std::to_string(worker) + "\n");
    read_cli_until(cli, "(mdbg) ");
    write_cli(cli, "set register r12 0xa5a55a5ac3c33c3c\n");
    output = read_cli_until(cli, "(mdbg) ");
    require(register_output_value(output, "r12") == kRegisterMutationValue,
            "CLI did not acknowledge selected worker register mutation");

    write_cli(cli, "reg r12\n");
    output = read_cli_until(cli, "(mdbg) ");
    require(register_output_value(output, "r12") == kRegisterMutationValue,
            "selected worker register mutation did not round-trip");

    write_cli(cli, "thread " + std::to_string(child.pid) + "\n");
    read_cli_until(cli, "(mdbg) ");
    write_cli(cli, "reg r12\n");
    output = read_cli_until(cli, "(mdbg) ");
    require(register_output_value(output, "r12") == leader_r12,
            "selected worker register mutation leaked into the leader");

    write_cli(cli, "detach\n");
    ::close(cli.input);
    cli.input = -1;
    const auto detached = read_cli_to_eof(cli);
    require(detached.find("detached") != std::string::npos,
            "CLI did not detach after register mutation");

    int status = 0;
    pid_t result;
    do {
      result = ::waitpid(cli.pid, &status, 0);
    } while (result == -1 && errno == EINTR);
    require(result == cli.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "register-mutation CLI did not exit cleanly");
    cli.pid = -1;
    ::close(cli.output);
    cli.output = -1;

    release_register_fixture(child, worker);
  } catch (...) {
    terminate_cli(cli);
    terminate_child(child);
    throw;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: multithread_attach_teardown_integration <fixture>\n";
    return 2;
  }
  try {
    const auto executable_dir = std::filesystem::path(argv[0]).parent_path();
    const auto mdbg = (executable_dir.empty() ? std::filesystem::current_path() : executable_dir) /
                      "mdbg";
    test_explicit_multithread_detach(argv[1]);
    test_destructor_multithread_detach(argv[1]);
    test_explicit_thread_selection_progress(argv[1]);
    test_cli_thread_selection(argv[1], mdbg.string());
    test_cli_selected_register_mutation(argv[1], mdbg.string());
  } catch (const std::exception& error) {
    std::cerr << "multi-thread attach/teardown integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

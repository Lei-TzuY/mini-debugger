#include "debugger/debugger.hpp"

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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: multithread_attach_teardown_integration <fixture>\n";
    return 2;
  }
  try {
    test_explicit_multithread_detach(argv[1]);
    test_destructor_multithread_detach(argv[1]);
    test_explicit_thread_selection_progress(argv[1]);
  } catch (const std::exception& error) {
    std::cerr << "multi-thread attach/teardown integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

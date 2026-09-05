#include "debugger/debugger.hpp"

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

struct Child {
  pid_t pid{-1};
  std::string path;
};

Child spawn_threaded_fixture(const std::string& fixture) {
  Child child{-1, temp_path()};
  child.pid = ::fork();
  if (child.pid == -1) throw std::runtime_error("fork failed");
  if (child.pid == 0) {
    ::execl(fixture.c_str(), fixture.c_str(), child.path.c_str(), "attach-threaded", nullptr);
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

void test_explicit_multithread_detach(const std::string& fixture) {
  const auto child = spawn_threaded_fixture(fixture);
  try {
    const auto before = task_ids(child.pid);
    require(before.size() >= 2, "fixture did not expose a pre-existing worker");

    auto debugger = mdbg::Debugger::attach(child.pid);
    require(debugger.active_tid() == child.pid,
            "attach must select the thread-group leader as initial active TID");
    require_tracer_state(child.pid, before, ::getpid(), "attach did not trace every existing TID");

    debugger.detach();
    require(debugger.state() == mdbg::ProcessState::Detached,
            "explicit detach did not transition to Detached");
    require_tracer_state(child.pid, before, 0, "explicit detach left a TID traced");
    release_and_require_clean_exit(child);
  } catch (...) {
    terminate_child(child);
    throw;
  }
}

void test_destructor_multithread_detach(const std::string& fixture) {
  const auto child = spawn_threaded_fixture(fixture);
  try {
    const auto before = task_ids(child.pid);
    require(before.size() >= 2, "fixture did not expose a pre-existing worker");
    {
      auto debugger = mdbg::Debugger::attach(child.pid);
      require_tracer_state(child.pid, before, ::getpid(),
                           "destructor setup did not trace every existing TID");
    }
    require_tracer_state(child.pid, before, 0, "debugger destructor left a TID traced");
    release_and_require_clean_exit(child);
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
  } catch (const std::exception& error) {
    std::cerr << "multi-thread attach/teardown integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

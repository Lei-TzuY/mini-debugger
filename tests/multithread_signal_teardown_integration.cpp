#include "debugger/debugger.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string temp_path() {
  char pattern[] = "/tmp/mdbg-thread-signal-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd == -1) throw std::runtime_error("mkstemp failed");
  ::close(fd);
  ::unlink(pattern);
  return pattern;
}

void require_tid_gone(pid_t tid, const std::string& message) {
  errno = 0;
  require(::kill(tid, 0) == -1 && errno == ESRCH, message);
}

void test_worker_signal_routing(const std::string& fixture) {
  const auto path = temp_path();
  try {
    auto debugger = mdbg::Debugger::launch(fixture, {path, "thread-signal"});
    const auto leader = debugger.pid();

    auto info = debugger.continue_execution();
    require(info.reason == mdbg::StopReason::Signal && info.value == SIGSTOP &&
                info.tid == leader,
            "fixture synchronization stop missing");

    info = debugger.continue_execution();
    require(info.reason == mdbg::StopReason::ThreadCreated && info.tid != leader,
            "worker creation was not surfaced");
    const auto worker = info.tid;

    info = debugger.continue_execution();
    require(info.reason == mdbg::StopReason::Signal && info.value == SIGUSR1 &&
                info.tid == worker,
            "first worker SIGUSR1 was not attributed to worker TID");

    info = debugger.continue_execution(mdbg::SignalPolicy::Suppress);
    require(info.reason == mdbg::StopReason::Signal && info.value == SIGUSR1 &&
                info.tid == worker,
            "suppression did not resume the same worker to its second signal");

    info = debugger.continue_execution(mdbg::SignalPolicy::Forward);
    require(info.reason == mdbg::StopReason::ThreadExited && info.value == 0 &&
                info.tid == worker,
            "forwarded worker signal did not return through the same worker");
    require(debugger.active_tid() == leader,
            "leader did not become active after signal worker exited");

    info = debugger.continue_execution();
    require(info.reason == mdbg::StopReason::Exited && info.value == 0 &&
                info.tid == leader,
            "fixture did not prove exactly one forwarded handler on worker TID");
  } catch (...) {
    std::remove(path.c_str());
    throw;
  }
  std::remove(path.c_str());
}

void test_launched_multithread_teardown(const std::string& fixture) {
  const auto path = temp_path();
  pid_t leader = -1;
  pid_t worker = -1;
  try {
    {
      auto debugger = mdbg::Debugger::launch(fixture, {path, "thread-cleanup"});
      leader = debugger.pid();
      auto info = debugger.continue_execution();
      require(info.reason == mdbg::StopReason::Signal && info.value == SIGSTOP,
              "cleanup fixture synchronization stop missing");
      info = debugger.continue_execution();
      require(info.reason == mdbg::StopReason::ThreadCreated && info.tid != leader,
              "cleanup fixture did not create a traced worker");
      worker = info.tid;
    }

    require_tid_gone(leader, "launched teardown left leader alive");
    require_tid_gone(worker, "launched teardown left worker alive");
  } catch (...) {
    if (leader > 0) ::kill(leader, SIGKILL);
    std::remove(path.c_str());
    throw;
  }
  std::remove(path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: multithread_signal_teardown_integration <fixture>\n";
    return 2;
  }
  try {
    test_worker_signal_routing(argv[1]);
    test_launched_multithread_teardown(argv[1]);
  } catch (const std::exception& error) {
    std::cerr << "multi-thread signal/teardown integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

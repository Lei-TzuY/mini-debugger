#include "debugger/debugger.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

struct FixtureAddresses {
  std::uintptr_t one;
  std::uintptr_t two;
};

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string temp_path() {
  char pattern[] = "/tmp/mdbg-attach-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd == -1) throw std::runtime_error("mkstemp failed");
  ::close(fd);
  ::unlink(pattern);
  return pattern;
}

FixtureAddresses wait_for_addresses(const std::string& path) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    std::ifstream input(path);
    std::string one, two, value;
    input >> one >> two >> value;
    if (input) {
      return {static_cast<std::uintptr_t>(std::stoull(one, nullptr, 0)),
              static_cast<std::uintptr_t>(std::stoull(two, nullptr, 0))};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  throw std::runtime_error("fixture did not publish addresses");
}

struct Child {
  pid_t pid;
  std::string path;
  FixtureAddresses addresses;
};

Child spawn_fixture(const std::string& fixture) {
  const std::string path = temp_path();
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed");
  if (child == 0) {
    ::execl(fixture.c_str(), fixture.c_str(), path.c_str(), "attach", nullptr);
    _exit(127);
  }
  return {child, path, wait_for_addresses(path)};
}

void release_and_require_clean_exit(const Child& child) {
  require(::kill(child.pid, 0) == 0, "detached process should remain alive");
  require(::kill(child.pid, SIGUSR1) == 0, "failed to release detached fixture");
  int status = 0;
  pid_t result;
  do {
    result = ::waitpid(child.pid, &status, 0);
  } while (result == -1 && errno == EINTR);
  require(result == child.pid, "waitpid failed for detached fixture");
  require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "detached fixture did not exit cleanly");
  std::remove(child.path.c_str());
}

void terminate_child(const Child& child) noexcept {
  ::kill(child.pid, SIGKILL);
  int status = 0;
  while (::waitpid(child.pid, &status, 0) == -1 && errno == EINTR) {
  }
  std::remove(child.path.c_str());
}

void install_and_hit_breakpoints(mdbg::Debugger& debugger,
                                 const FixtureAddresses& addresses) {
  require(debugger.stop_info().reason == mdbg::StopReason::Attached,
          "attach stop reason mismatch");
  require(debugger.state() == mdbg::ProcessState::Stopped,
          "attached tracee should be stopped");
  require(debugger.origin() == mdbg::ProcessOrigin::Attached,
          "attached process origin mismatch");
  require(debugger.registers().rip != 0, "attached registers should be readable");

  debugger.add_breakpoint(addresses.one);
  debugger.add_breakpoint(addresses.two);
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint &&
              hit.breakpoint_address == addresses.one,
          "attached breakpoint was not hit");
  require(debugger.registers().rip == addresses.one,
          "attached breakpoint RIP was not repaired");
}

void test_explicit_detach(const std::string& fixture) {
  const auto child = spawn_fixture(fixture);
  try {
    auto debugger = mdbg::Debugger::attach(child.pid);
    install_and_hit_breakpoints(debugger, child.addresses);
    debugger.detach();
    require(debugger.state() == mdbg::ProcessState::Detached,
            "detach should transition to Detached state");
    release_and_require_clean_exit(child);
  } catch (...) {
    terminate_child(child);
    throw;
  }
}

void test_destructor_detaches_without_killing(const std::string& fixture) {
  const auto child = spawn_fixture(fixture);
  try {
    {
      auto debugger = mdbg::Debugger::attach(child.pid);
      install_and_hit_breakpoints(debugger, child.addresses);
    }
    release_and_require_clean_exit(child);
  } catch (...) {
    terminate_child(child);
    throw;
  }
}

void test_invalid_pid() {
  bool failed = false;
  try {
    (void)mdbg::Debugger::attach(-1);
  } catch (const std::invalid_argument&) {
    failed = true;
  }
  require(failed, "invalid attach pid should be rejected before ptrace");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: attach_integration <fixture>\n";
    return 2;
  }
  try {
    test_explicit_detach(argv[1]);
    test_destructor_detaches_without_killing(argv[1]);
    test_invalid_pid();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "attach integration failure: " << error.what() << '\n';
    return 1;
  }
}

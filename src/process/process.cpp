#include "process/process.hpp"

#include "ptrace/ptrace.hpp"

#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace mdbg {
namespace {

std::runtime_error system_error(const char* operation) {
  return std::runtime_error(std::string(operation) + " failed: " + std::strerror(errno));
}

}  // namespace

Process::~Process() { cleanup(); }

Process::Process(Process&& other) noexcept
    : pid_(std::exchange(other.pid_, -1)),
      state_(other.state_),
      origin_(other.origin_),
      exit_code_(other.exit_code_),
      termination_signal_(other.termination_signal_) {}

Process& Process::operator=(Process&& other) noexcept {
  if (this != &other) {
    cleanup();
    pid_ = std::exchange(other.pid_, -1);
    state_ = other.state_;
    origin_ = other.origin_;
    exit_code_ = other.exit_code_;
    termination_signal_ = other.termination_signal_;
  }
  return *this;
}

Process Process::launch(const std::string& executable,
                        const std::vector<std::string>& arguments) {
  const pid_t child = ::fork();
  if (child == -1) {
    throw system_error("fork");
  }

  if (child == 0) {
    try {
      lowlevel::traceme();
    } catch (...) {
      _exit(126);
    }

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    _exit(127);
  }

  Process process(child, ProcessOrigin::Launched);
  const auto initial = process.wait();
  if (initial.kind != WaitEvent::Kind::Stopped || initial.value != SIGTRAP) {
    throw std::runtime_error("tracee did not stop with SIGTRAP after exec");
  }
  lowlevel::set_options(child, PTRACE_O_EXITKILL);
  return process;
}

Process Process::attach(pid_t pid) {
  if (pid <= 0) {
    throw std::invalid_argument("attach requires a positive pid");
  }

  lowlevel::attach(pid);
  Process process(pid, ProcessOrigin::Attached);
  try {
    const auto initial = process.wait();
    if (initial.kind != WaitEvent::Kind::Stopped || initial.value != SIGSTOP) {
      throw std::runtime_error("attached tracee did not stop with SIGSTOP");
    }
  } catch (...) {
    process.cleanup();
    throw;
  }
  return process;
}

WaitEvent Process::wait() {
  int status = 0;
  pid_t result;
  do {
    result = ::waitpid(pid_, &status, 0);
  } while (result == -1 && errno == EINTR);
  if (result == -1) {
    throw system_error("waitpid");
  }

  if (WIFEXITED(status)) {
    state_ = ProcessState::Exited;
    exit_code_ = WEXITSTATUS(status);
    return {WaitEvent::Kind::Exited, *exit_code_};
  }
  if (WIFSIGNALED(status)) {
    state_ = ProcessState::Signaled;
    termination_signal_ = WTERMSIG(status);
    return {WaitEvent::Kind::Signaled, *termination_signal_};
  }
  if (WIFSTOPPED(status)) {
    state_ = ProcessState::Stopped;
    return {WaitEvent::Kind::Stopped, WSTOPSIG(status)};
  }
  throw std::runtime_error("waitpid returned an unsupported process state");
}

void Process::detach(int signal) {
  if (origin_ != ProcessOrigin::Attached) {
    throw std::logic_error("detach is only valid for an attached process");
  }
  if (state_ != ProcessState::Stopped) {
    throw std::logic_error("detach requires a stopped process");
  }
  lowlevel::detach(pid_, signal);
  state_ = ProcessState::Detached;
  pid_ = -1;
}

void Process::cleanup() noexcept {
  if (pid_ <= 0 || state_ == ProcessState::Exited || state_ == ProcessState::Signaled ||
      state_ == ProcessState::Detached) {
    return;
  }

  if (origin_ == ProcessOrigin::Attached) {
    if (state_ == ProcessState::Stopped) {
      try {
        lowlevel::detach(pid_);
      } catch (...) {
      }
    }
    pid_ = -1;
    state_ = ProcessState::Detached;
    return;
  }

  ::kill(pid_, SIGKILL);
  int status = 0;
  while (::waitpid(pid_, &status, 0) == -1 && errno == EINTR) {
  }
  pid_ = -1;
}

}  // namespace mdbg

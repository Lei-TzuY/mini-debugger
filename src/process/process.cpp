#include "process/process.hpp"

#include "ptrace/ptrace.hpp"

#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

namespace mdbg {
namespace {
constexpr std::size_t kMaxAttachDiscoveryPasses = 64;
constexpr std::size_t kMaxAttachTasks = 4096;

std::runtime_error system_error(const char* operation) {
  return std::runtime_error(std::string(operation) + " failed: " + std::strerror(errno));
}

bool is_tracee_task(pid_t leader, pid_t tid) noexcept {
  std::error_code error;
  const auto task_path = std::filesystem::path("/proc") / std::to_string(leader) /
                         "task" / std::to_string(tid);
  return std::filesystem::exists(task_path, error) && !error;
}

std::vector<pid_t> thread_ids(pid_t leader) {
  std::vector<pid_t> result;
  std::error_code error;
  const auto task_path =
      std::filesystem::path("/proc") / std::to_string(leader) / "task";
  std::filesystem::directory_iterator it(task_path, error);
  if (error) {
    throw std::runtime_error("cannot enumerate tracee threads: " + error.message());
  }
  const std::filesystem::directory_iterator end;
  for (; it != end; it.increment(error)) {
    if (error) {
      throw std::runtime_error("cannot enumerate tracee threads: " + error.message());
    }
    const auto name = it->path().filename().string();
    try {
      std::size_t consumed = 0;
      const auto value = std::stoll(name, &consumed, 10);
      if (consumed != name.size() || value <= 0 ||
          value > static_cast<long long>(std::numeric_limits<pid_t>::max())) {
        continue;
      }
      result.push_back(static_cast<pid_t>(value));
    } catch (const std::exception&) {
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

}  // namespace

Process::~Process() { cleanup(); }

Process::Process(Process&& other) noexcept
    : pid_(std::exchange(other.pid_, -1)),
      current_tid_(std::exchange(other.current_tid_, -1)),
      state_(other.state_),
      origin_(other.origin_),
      task_states_(std::move(other.task_states_)),
      exit_code_(other.exit_code_),
      termination_signal_(other.termination_signal_) {}

Process& Process::operator=(Process&& other) noexcept {
  if (this != &other) {
    cleanup();
    pid_ = std::exchange(other.pid_, -1);
    current_tid_ = std::exchange(other.current_tid_, -1);
    state_ = other.state_;
    origin_ = other.origin_;
    task_states_ = std::move(other.task_states_);
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
  if (initial.kind != WaitEvent::Kind::Stopped || initial.value != SIGTRAP ||
      initial.tid != child) {
    throw std::runtime_error("tracee did not stop with SIGTRAP after exec");
  }
  lowlevel::set_options(child, PTRACE_O_EXITKILL | PTRACE_O_TRACECLONE);
  return process;
}

Process Process::attach(pid_t pid) {
  if (pid <= 0) {
    throw std::invalid_argument("attach requires a positive pid");
  }

  Process process(pid, ProcessOrigin::Attached);
  process.task_states_.clear();
  process.current_tid_ = -1;
  process.state_ = ProcessState::Running;

  try {
    for (std::size_t pass = 0; pass < kMaxAttachDiscoveryPasses; ++pass) {
      const auto observed = thread_ids(pid);
      if (observed.empty()) {
        throw std::runtime_error("attach target has no live threads");
      }

      for (const auto tid : observed) {
        if (process.task_states_.count(tid) != 0) continue;
        if (process.task_states_.size() >= kMaxAttachTasks) {
          throw std::runtime_error("attach thread count exceeds supported bound");
        }

        try {
          lowlevel::attach(tid);
        } catch (const lowlevel::PtraceError& error) {
          if (error.error_number() == ESRCH) continue;
          throw;
        }
        process.task_states_.emplace(tid, ProcessState::Running);

        const auto initial = process.wait_for(tid);
        if (initial.kind != WaitEvent::Kind::Stopped || initial.value != SIGSTOP ||
            initial.tid != tid) {
          throw std::runtime_error("attached tracee thread did not stop with SIGSTOP");
        }
        lowlevel::set_options(tid, PTRACE_O_TRACECLONE);
      }

      const auto after = thread_ids(pid);
      bool complete = !after.empty();
      for (const auto tid : after) {
        if (process.task_states_.count(tid) == 0) {
          complete = false;
          break;
        }
      }
      if (!complete) continue;
      if (process.task_states_.count(pid) == 0) {
        throw std::runtime_error("thread-group leader exited during attach");
      }

      process.current_tid_ = pid;
      process.update_aggregate_state();
      if (process.state_ != ProcessState::Stopped) {
        throw std::runtime_error("attach did not stop every traced thread");
      }
      return process;
    }
    throw std::runtime_error("attach thread discovery did not converge");
  } catch (...) {
    process.cleanup();
    throw;
  }
}

std::vector<pid_t> Process::tids() const {
  std::vector<pid_t> result;
  result.reserve(task_states_.size());
  for (const auto& [tid, state] : task_states_) {
    static_cast<void>(state);
    result.push_back(tid);
  }
  return result;
}

std::optional<ProcessState> Process::task_state(pid_t tid) const noexcept {
  const auto it = task_states_.find(tid);
  if (it == task_states_.end()) return std::nullopt;
  return it->second;
}

WaitEvent Process::wait() { return wait_for(-1); }

WaitEvent Process::wait_for(pid_t requested_tid) {
  int status = 0;
  pid_t result;
  do {
    result = ::waitpid(requested_tid, &status, __WALL);
  } while (result == -1 && errno == EINTR);
  if (result == -1) {
    throw system_error("waitpid");
  }

  auto task = task_states_.find(result);
  if (task == task_states_.end() && WIFSTOPPED(status) && is_tracee_task(pid_, result)) {
    task = task_states_.emplace(result, ProcessState::Running).first;
  }
  if (task == task_states_.end()) {
    throw std::runtime_error("waitpid returned an event for an untracked task");
  }

  current_tid_ = result;

  if (WIFEXITED(status)) {
    const int code = WEXITSTATUS(status);
    if (result == pid_) exit_code_ = code;
    task_states_.erase(task);
    if (task_states_.empty()) {
      current_tid_ = -1;
      state_ = ProcessState::Exited;
    } else {
      update_aggregate_state();
      select_current_task();
    }
    return {WaitEvent::Kind::Exited, code, result};
  }
  if (WIFSIGNALED(status)) {
    const int signal = WTERMSIG(status);
    if (result == pid_) termination_signal_ = signal;
    task_states_.erase(task);
    if (task_states_.empty()) {
      current_tid_ = -1;
      state_ = ProcessState::Signaled;
    } else {
      update_aggregate_state();
      select_current_task();
    }
    return {WaitEvent::Kind::Signaled, signal, result};
  }
  if (WIFSTOPPED(status)) {
    task->second = ProcessState::Stopped;
    state_ = ProcessState::Stopped;
    const auto ptrace_event = static_cast<unsigned int>(status >> 16);
    std::optional<pid_t> new_tid;
    if (ptrace_event == PTRACE_EVENT_CLONE) {
      const auto message = lowlevel::get_event_message(result);
      if (message == 0 ||
          message > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        throw std::runtime_error("PTRACE_EVENT_CLONE returned an invalid task id");
      }
      const auto child_tid = static_cast<pid_t>(message);
      task_states_.try_emplace(child_tid, ProcessState::Running);
      new_tid = child_tid;
    }
    return {WaitEvent::Kind::Stopped, WSTOPSIG(status), result, ptrace_event, new_tid};
  }
  throw std::runtime_error("waitpid returned an unsupported process state");
}

void Process::mark_running(pid_t tid) noexcept {
  const auto it = task_states_.find(tid);
  if (it == task_states_.end()) return;
  it->second = ProcessState::Running;
  update_aggregate_state();
}

void Process::update_aggregate_state() noexcept {
  if (task_states_.empty()) return;
  for (const auto& [tid, state] : task_states_) {
    static_cast<void>(tid);
    if (state == ProcessState::Stopped) {
      state_ = ProcessState::Stopped;
      return;
    }
  }
  state_ = ProcessState::Running;
}

void Process::select_current_task() noexcept {
  current_tid_ = -1;
  for (const auto& [tid, state] : task_states_) {
    if (state == ProcessState::Stopped) {
      current_tid_ = tid;
      return;
    }
  }
  if (!task_states_.empty()) current_tid_ = task_states_.begin()->first;
}

void Process::detach(int signal) {
  if (origin_ != ProcessOrigin::Attached) {
    throw std::logic_error("detach is only valid for an attached process");
  }
  if (state_ != ProcessState::Stopped) {
    throw std::logic_error("detach requires a stopped process");
  }
  for (const auto& [tid, task_state] : task_states_) {
    static_cast<void>(tid);
    if (task_state != ProcessState::Stopped) {
      throw std::logic_error("detach requires every traced thread to be stopped");
    }
  }

  const auto signal_tid = current_tid_;
  std::vector<pid_t> order;
  order.reserve(task_states_.size());
  for (const auto& [tid, task_state] : task_states_) {
    static_cast<void>(task_state);
    if (tid != signal_tid) order.push_back(tid);
  }
  if (task_states_.count(signal_tid) != 0) order.push_back(signal_tid);

  std::exception_ptr first_error;
  for (const auto tid : order) {
    try {
      lowlevel::detach(tid, tid == signal_tid ? signal : 0);
      task_states_.erase(tid);
    } catch (const lowlevel::PtraceError& error) {
      if (error.error_number() == ESRCH) {
        task_states_.erase(tid);
        continue;
      }
      if (!first_error) first_error = std::current_exception();
    } catch (...) {
      if (!first_error) first_error = std::current_exception();
    }
  }

  if (task_states_.empty()) {
    state_ = ProcessState::Detached;
    current_tid_ = -1;
    pid_ = -1;
  } else {
    update_aggregate_state();
    select_current_task();
  }
  if (first_error) std::rethrow_exception(first_error);
}

void Process::cleanup() noexcept {
  if (pid_ <= 0 || state_ == ProcessState::Exited || state_ == ProcessState::Signaled ||
      state_ == ProcessState::Detached) {
    return;
  }

  if (origin_ == ProcessOrigin::Attached) {
    if (state_ == ProcessState::Stopped) {
      try {
        detach();
        return;
      } catch (...) {
        for (auto it = task_states_.begin(); it != task_states_.end();) {
          if (it->second != ProcessState::Stopped) {
            ++it;
            continue;
          }
          try {
            lowlevel::detach(it->first);
            it = task_states_.erase(it);
          } catch (const lowlevel::PtraceError& error) {
            if (error.error_number() == ESRCH) {
              it = task_states_.erase(it);
            } else {
              ++it;
            }
          } catch (...) {
            ++it;
          }
        }
      }
    }
    task_states_.clear();
    current_tid_ = -1;
    pid_ = -1;
    state_ = ProcessState::Detached;
    return;
  }

  ::kill(pid_, SIGKILL);
  int status = 0;
  for (;;) {
    pid_t result;
    do {
      result = ::waitpid(-1, &status, __WALL);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
      if (errno == ECHILD) break;
      break;
    }
    task_states_.erase(result);
  }
  task_states_.clear();
  current_tid_ = -1;
  pid_ = -1;
  state_ = ProcessState::Exited;
}

}  // namespace mdbg

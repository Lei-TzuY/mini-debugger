#pragma once

#include <sys/types.h>

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mdbg {

enum class ProcessState { Stopped, Running, Exited, Signaled, Detached };
enum class ProcessOrigin { Launched, Attached };

struct WaitEvent {
  enum class Kind { Stopped, Exited, Signaled };
  Kind kind;
  int value;
  pid_t tid{-1};
  unsigned int ptrace_event{0};
  std::optional<pid_t> new_tid{};
};

class Process {
 public:
  Process() = default;
  ~Process();

  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  Process(Process&& other) noexcept;
  Process& operator=(Process&& other) noexcept;

  static Process launch(const std::string& executable,
                        const std::vector<std::string>& arguments = {});
  static Process attach(pid_t pid);
  static Process adopt_stopped(pid_t pid, ProcessOrigin origin);

  [[nodiscard]] pid_t pid() const noexcept { return pid_; }
  [[nodiscard]] pid_t current_tid() const noexcept { return current_tid_; }
  [[nodiscard]] ProcessState state() const noexcept { return state_; }
  [[nodiscard]] ProcessOrigin origin() const noexcept { return origin_; }
  [[nodiscard]] std::optional<int> exit_code() const noexcept { return exit_code_; }
  [[nodiscard]] std::optional<int> termination_signal() const noexcept {
    return termination_signal_;
  }
  [[nodiscard]] std::vector<pid_t> tids() const;
  [[nodiscard]] std::optional<ProcessState> task_state(pid_t tid) const noexcept;

  WaitEvent wait();
  WaitEvent wait_current();
  void mark_running() noexcept { mark_running(current_tid_); }
  void mark_running(pid_t tid) noexcept;
  void select_tid(pid_t tid);
  void swap(Process& other) noexcept;
  void collapse_after_exec(pid_t tid) {
    if (tid <= 0) throw std::invalid_argument("exec collapse requires a positive tid");
    task_states_.clear();
    task_states_.emplace(tid, ProcessState::Stopped);
    pid_ = tid;
    current_tid_ = tid;
    state_ = ProcessState::Stopped;
    exit_code_.reset();
    termination_signal_.reset();
  }
  void adopt_stopped_process(pid_t pid) {
    if (pid <= 0) throw std::invalid_argument("process adoption requires a positive pid");
    task_states_.clear();
    task_states_.emplace(pid, ProcessState::Stopped);
    pid_ = pid;
    current_tid_ = pid;
    state_ = ProcessState::Stopped;
    exit_code_.reset();
    termination_signal_.reset();
  }
  void detach(int signal = 0);

 private:
  Process(pid_t pid, ProcessOrigin origin)
      : pid_(pid),
        current_tid_(pid),
        state_(ProcessState::Running),
        origin_(origin) {
    task_states_.emplace(pid, ProcessState::Running);
  }

  WaitEvent wait_for(pid_t tid);
  void update_aggregate_state() noexcept;
  void select_current_task() noexcept;
  void cleanup() noexcept;

  pid_t pid_{-1};
  pid_t current_tid_{-1};
  ProcessState state_{ProcessState::Exited};
  ProcessOrigin origin_{ProcessOrigin::Launched};
  std::map<pid_t, ProcessState> task_states_;
  std::optional<int> exit_code_;
  std::optional<int> termination_signal_;
};

}  // namespace mdbg

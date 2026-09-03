#pragma once

#include <sys/types.h>

#include <optional>
#include <string>
#include <vector>

namespace mdbg {

enum class ProcessState { Stopped, Running, Exited, Signaled, Detached };
enum class ProcessOrigin { Launched, Attached };

struct WaitEvent {
  enum class Kind { Stopped, Exited, Signaled };
  Kind kind;
  int value;
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

  [[nodiscard]] pid_t pid() const noexcept { return pid_; }
  [[nodiscard]] ProcessState state() const noexcept { return state_; }
  [[nodiscard]] ProcessOrigin origin() const noexcept { return origin_; }
  [[nodiscard]] std::optional<int> exit_code() const noexcept { return exit_code_; }
  [[nodiscard]] std::optional<int> termination_signal() const noexcept {
    return termination_signal_;
  }

  WaitEvent wait();
  void mark_running() noexcept { state_ = ProcessState::Running; }
  void detach(int signal = 0);

 private:
  Process(pid_t pid, ProcessOrigin origin)
      : pid_(pid), state_(ProcessState::Running), origin_(origin) {}
  void cleanup() noexcept;

  pid_t pid_{-1};
  ProcessState state_{ProcessState::Exited};
  ProcessOrigin origin_{ProcessOrigin::Launched};
  std::optional<int> exit_code_;
  std::optional<int> termination_signal_;
};

}  // namespace mdbg

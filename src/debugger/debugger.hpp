#pragma once

#include "breakpoints/breakpoint.hpp"
#include "process/process.hpp"

#include <sys/user.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mdbg {

class DeferredBreakpoints;

enum class StopReason {
  InitialExec,
  Attached,
  Exec,
  ThreadCreated,
  ThreadExited,
  ThreadSignaled,
  ProcessExited,
  ProcessSignaled,
  Breakpoint,
  Watchpoint,
  SingleStep,
  Signal,
  Trap,
  Exited,
  Signaled
};
enum class SignalPolicy { Suppress, Forward };
enum class ProcessEventKind { None, Fork, Vfork };
enum class ForkFollowPolicy { Parent, Child, Both };

inline std::uint64_t next_stop_sequence() noexcept {
  static std::atomic<std::uint64_t> sequence{0};
  return sequence.fetch_add(1, std::memory_order_relaxed) + 1;
}

struct StopInfo {
  StopReason reason;
  int value{0};
  std::optional<std::uintptr_t> breakpoint_address{};
  std::optional<std::size_t> watchpoint_id{};
  std::optional<std::uintptr_t> watchpoint_address{};
  pid_t tid{-1};
  std::optional<pid_t> new_tid{};
  std::optional<pid_t> former_tid{};
  ProcessEventKind process_event{ProcessEventKind::None};
  std::optional<pid_t> parent_pid{};
  std::optional<pid_t> child_pid{};
  bool retains_child{false};
  std::uint64_t sequence{next_stop_sequence()};
};

struct Watchpoint {
  std::size_t id;
  std::uintptr_t address;
  std::size_t length;
};

struct ThreadInfo {
  pid_t tid;
  ProcessState state;
  bool active;
};

struct ProcessInfo {
  pid_t pid;
  ProcessState state;
  bool active;
};

class Debugger {
 public:
  ~Debugger();
  Debugger(const Debugger&) = delete;
  Debugger& operator=(const Debugger&) = delete;
  Debugger(Debugger&&) noexcept = default;
  Debugger& operator=(Debugger&&) = delete;

  static Debugger launch(const std::string& executable,
                         const std::vector<std::string>& arguments = {});
  static Debugger attach(pid_t pid);

  [[nodiscard]] pid_t pid() const noexcept { return process_.pid(); }
  [[nodiscard]] pid_t active_tid() const noexcept { return process_.current_tid(); }
  [[nodiscard]] ProcessState state() const noexcept { return process_.state(); }
  [[nodiscard]] ProcessOrigin origin() const noexcept { return process_.origin(); }
  [[nodiscard]] const StopInfo& stop_info() const noexcept { return stop_info_; }
  [[nodiscard]] const std::string& executable_path() const noexcept { return executable_path_; }
  [[nodiscard]] ForkFollowPolicy fork_follow_policy() const noexcept {
    return fork_follow_policy_;
  }
  void set_fork_follow_policy(ForkFollowPolicy policy);
  [[nodiscard]] std::vector<ProcessInfo> processes() const;
  void select_process(pid_t pid);
  [[nodiscard]] std::vector<ThreadInfo> threads() const;
  void select_thread(pid_t tid);

  StopInfo continue_execution(SignalPolicy policy = SignalPolicy::Suppress);
  StopInfo single_step(SignalPolicy policy = SignalPolicy::Suppress);
  void detach(SignalPolicy policy = SignalPolicy::Suppress);

  user_regs_struct registers() const;
  [[nodiscard]] std::optional<user_regs_struct> breakpoint_register_snapshot(
      pid_t tid, std::uintptr_t address) const;
  void set_register(std::string_view name, std::uint64_t value);
  std::vector<std::byte> read_memory(std::uintptr_t address, std::size_t length) const;
  void write_memory(std::uintptr_t address, const std::vector<std::byte>& bytes);

  std::size_t add_breakpoint(std::uintptr_t address);
  bool remove_breakpoint(std::size_t id);
  [[nodiscard]] std::vector<Breakpoint> breakpoints() const;

  std::size_t add_write_watchpoint(std::uintptr_t address, std::size_t length);
  bool remove_watchpoint(std::size_t id);
  [[nodiscard]] std::vector<Watchpoint> watchpoints() const;

 private:
  struct PendingBreakpointStep {
    std::uintptr_t address;
    pid_t tid;
  };

  struct DebugRegisterSnapshot {
    pid_t tid;
    std::uint64_t dr0;
    std::uint64_t dr6;
    std::uint64_t dr7;
  };

  struct RetainedProcessDomain {
    Process process;
    StopInfo stop_info;
    std::string executable_path;
    std::set<pid_t> pending_thread_starts;
    std::map<pid_t, int> pending_signals;
    std::map<std::uintptr_t, Breakpoint> breakpoints_by_address;
    std::map<std::size_t, std::uintptr_t> breakpoint_ids;
    std::map<std::pair<pid_t, std::uintptr_t>, user_regs_struct>
        breakpoint_register_snapshots;
    std::optional<PendingBreakpointStep> pending_breakpoint_step;
    std::optional<Watchpoint> watchpoint;
    std::optional<DebugRegisterSnapshot> watchpoint_register_snapshot;
  };

  friend class DeferredBreakpoints;

  Debugger(Process process, StopInfo initial_stop, std::string executable_path);

  [[nodiscard]] pid_t stopped_tid() const;
  [[nodiscard]] std::optional<pid_t> first_stopped_retained_process() const noexcept;
  void swap_active_process(pid_t pid);
  WaitEvent wait_active_process();
  StopInfo wait_and_classify(bool expected_single_step = false);
  int resume_signal(SignalPolicy policy, pid_t tid) const;
  void prepare_breakpoint_hit(std::uintptr_t address, user_regs_struct regs, pid_t tid);
  StopInfo step_over_pending_breakpoint(bool expose_single_step);
  void reinsert_breakpoint(std::uintptr_t address, pid_t tid);
  void restore_all_breakpoints();
  void restore_watchpoint_registers();
  void discard_image_state() noexcept;
  void discard_breakpoint_register_snapshots(pid_t tid) noexcept;
  [[nodiscard]] std::optional<StopInfo> classify_watchpoint_stop(pid_t tid);
  bool discard_breakpoint(std::size_t id) noexcept;

  Process process_;
  StopInfo stop_info_;
  std::string executable_path_;
  ForkFollowPolicy fork_follow_policy_{ForkFollowPolicy::Parent};
  std::map<std::uintptr_t, Breakpoint> breakpoints_by_address_;
  std::map<std::size_t, std::uintptr_t> breakpoint_ids_;
  std::map<std::pair<pid_t, std::uintptr_t>, user_regs_struct>
      breakpoint_register_snapshots_;
  std::size_t next_breakpoint_id_{1};
  std::optional<PendingBreakpointStep> pending_breakpoint_step_;
  std::optional<Watchpoint> watchpoint_;
  std::optional<DebugRegisterSnapshot> watchpoint_register_snapshot_;
  std::size_t next_watchpoint_id_{1};
  std::set<pid_t> pending_thread_starts_;
  std::map<pid_t, int> pending_signals_;
  std::map<pid_t, RetainedProcessDomain> retained_processes_;
};

}  // namespace mdbg

#pragma once

#include "breakpoints/breakpoint.hpp"
#include "process/process.hpp"

#include <sys/user.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mdbg {

enum class StopReason {
  InitialExec,
  Attached,
  Breakpoint,
  SingleStep,
  Signal,
  Trap,
  Exited,
  Signaled
};
enum class SignalPolicy { Suppress, Forward };

struct StopInfo {
  StopReason reason;
  int value{0};
  std::optional<std::uintptr_t> breakpoint_address;
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
  [[nodiscard]] ProcessState state() const noexcept { return process_.state(); }
  [[nodiscard]] ProcessOrigin origin() const noexcept { return process_.origin(); }
  [[nodiscard]] const StopInfo& stop_info() const noexcept { return stop_info_; }

  StopInfo continue_execution(SignalPolicy policy = SignalPolicy::Suppress);
  StopInfo single_step(SignalPolicy policy = SignalPolicy::Suppress);
  void detach(SignalPolicy policy = SignalPolicy::Suppress);

  user_regs_struct registers() const;
  std::vector<std::byte> read_memory(std::uintptr_t address, std::size_t length) const;

  std::size_t add_breakpoint(std::uintptr_t address);
  bool remove_breakpoint(std::size_t id);
  [[nodiscard]] std::vector<Breakpoint> breakpoints() const;

 private:
  Debugger(Process process, StopInfo initial_stop);

  StopInfo wait_and_classify(bool expected_single_step = false);
  int resume_signal(SignalPolicy policy) const;
  void prepare_breakpoint_hit(std::uintptr_t address, user_regs_struct regs);
  StopInfo step_over_pending_breakpoint(bool expose_single_step);
  void reinsert_breakpoint(std::uintptr_t address);
  void restore_all_breakpoints();

  Process process_;
  StopInfo stop_info_;
  std::map<std::uintptr_t, Breakpoint> breakpoints_by_address_;
  std::map<std::size_t, std::uintptr_t> breakpoint_ids_;
  std::size_t next_breakpoint_id_{1};
  std::optional<std::uintptr_t> pending_breakpoint_step_;
};

}  // namespace mdbg

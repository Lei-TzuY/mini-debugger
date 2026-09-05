#include "debugger/debugger.hpp"

#include "ptrace/ptrace.hpp"

#include <sys/ptrace.h>

#include <csignal>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

namespace mdbg {
namespace {
constexpr std::byte kInt3{0xcc};
constexpr std::uint64_t kDr6Breakpoint0{1};
constexpr std::uint64_t kDr7Slot0EnableMask{0x3};
constexpr std::uint64_t kDr7Slot0ControlMask{std::uint64_t{0xf} << 16U};

std::uint64_t watchpoint_length_encoding(std::size_t length) {
  switch (length) {
    case 1:
      return 0;
    case 2:
      return 1;
    case 4:
      return 3;
    case 8:
      return 2;
    default:
      throw std::invalid_argument("write watchpoint length must be 1, 2, 4, or 8 bytes");
  }
}

StopInfo make_stop(StopReason reason, int value, pid_t tid) {
  StopInfo info{reason, value};
  info.tid = tid;
  return info;
}

std::string process_executable(pid_t pid) {
  return std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe").string();
}

unsigned long tracing_options(ProcessOrigin origin) {
  unsigned long options = PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC;
  if (origin == ProcessOrigin::Launched) options |= PTRACE_O_EXITKILL;
  return options;
}
}  // namespace

Debugger Debugger::launch(const std::string& executable,
                          const std::vector<std::string>& arguments) {
  auto process = Process::launch(executable, arguments);
  lowlevel::set_options(process.current_tid(), tracing_options(ProcessOrigin::Launched));
  auto initial = make_stop(StopReason::InitialExec, SIGTRAP, process.current_tid());
  return Debugger(std::move(process), std::move(initial), process_executable(process.pid()));
}

Debugger Debugger::attach(pid_t pid) {
  auto process = Process::attach(pid);
  for (const auto tid : process.tids()) {
    lowlevel::set_options(tid, tracing_options(ProcessOrigin::Attached));
  }
  auto initial = make_stop(StopReason::Attached, SIGSTOP, process.current_tid());
  return Debugger(std::move(process), std::move(initial), process_executable(pid));
}

Debugger::Debugger(Process process, StopInfo initial_stop, std::string executable_path)
    : process_(std::move(process)),
      stop_info_(std::move(initial_stop)),
      executable_path_(std::move(executable_path)) {}

Debugger::~Debugger() {
  if (process_.origin() != ProcessOrigin::Attached ||
      process_.state() != ProcessState::Stopped) {
    return;
  }
  try {
    restore_all_breakpoints();
    restore_watchpoint_registers();
    process_.detach();
  } catch (...) {
  }
}

pid_t Debugger::stopped_tid() const {
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("operation requires a stopped tracee");
  }
  const auto tid = process_.current_tid();
  if (tid <= 0 || process_.task_state(tid) != ProcessState::Stopped) {
    throw std::logic_error("no active stopped tracee thread");
  }
  return tid;
}

std::vector<ThreadInfo> Debugger::threads() const {
  std::vector<ThreadInfo> result;
  for (const auto tid : process_.tids()) {
    const auto state = process_.task_state(tid);
    if (!state) continue;
    result.push_back(ThreadInfo{tid, *state, tid == process_.current_tid()});
  }
  return result;
}

void Debugger::select_thread(pid_t tid) {
  if (pending_breakpoint_step_ && pending_breakpoint_step_->tid != tid) {
    throw std::logic_error(
        "cannot switch threads while a breakpoint displaced step is pending");
  }
  process_.select_tid(tid);
}

user_regs_struct Debugger::registers() const {
  return lowlevel::get_registers(stopped_tid());
}

std::vector<std::byte> Debugger::read_memory(std::uintptr_t address,
                                             std::size_t length) const {
  return lowlevel::read_memory(stopped_tid(), address, length);
}

std::size_t Debugger::add_breakpoint(std::uintptr_t address) {
  const auto tid = stopped_tid();
  if (breakpoints_by_address_.count(address) != 0) {
    throw std::invalid_argument("a breakpoint already exists at that address");
  }

  const auto original = lowlevel::read_byte(tid, address);
  lowlevel::write_byte(tid, address, kInt3);
  const auto id = next_breakpoint_id_++;
  breakpoints_by_address_.emplace(address, Breakpoint{id, address, original, true});
  breakpoint_ids_.emplace(id, address);
  return id;
}

bool Debugger::remove_breakpoint(std::size_t id) {
  const auto id_it = breakpoint_ids_.find(id);
  if (id_it == breakpoint_ids_.end()) {
    return false;
  }
  const auto address = id_it->second;
  auto bp_it = breakpoints_by_address_.find(address);
  if (bp_it != breakpoints_by_address_.end() && bp_it->second.installed &&
      process_.state() == ProcessState::Stopped) {
    lowlevel::write_byte(stopped_tid(), address, bp_it->second.original_byte);
  }
  if (pending_breakpoint_step_ && pending_breakpoint_step_->address == address) {
    pending_breakpoint_step_.reset();
  }
  breakpoints_by_address_.erase(address);
  breakpoint_ids_.erase(id_it);
  return true;
}

bool Debugger::discard_breakpoint(std::size_t id) noexcept {
  const auto id_it = breakpoint_ids_.find(id);
  if (id_it == breakpoint_ids_.end()) return false;
  const auto address = id_it->second;
  if (pending_breakpoint_step_ && pending_breakpoint_step_->address == address) {
    pending_breakpoint_step_.reset();
  }
  breakpoints_by_address_.erase(address);
  breakpoint_ids_.erase(id_it);
  return true;
}

std::vector<Breakpoint> Debugger::breakpoints() const {
  std::vector<Breakpoint> result;
  result.reserve(breakpoints_by_address_.size());
  for (const auto& [address, breakpoint] : breakpoints_by_address_) {
    (void)address;
    result.push_back(breakpoint);
  }
  return result;
}

std::size_t Debugger::add_write_watchpoint(std::uintptr_t address, std::size_t length) {
  const auto tid = stopped_tid();
  if (watchpoint_) {
    throw std::invalid_argument("only one hardware watchpoint is currently supported");
  }
  const auto length_encoding = watchpoint_length_encoding(length);
  if (address % length != 0) {
    throw std::invalid_argument("write watchpoint address must be naturally aligned");
  }

  const DebugRegisterSnapshot snapshot{tid, lowlevel::get_debug_register(tid, 0),
                                       lowlevel::get_debug_register(tid, 6),
                                       lowlevel::get_debug_register(tid, 7)};
  if ((snapshot.dr7 & kDr7Slot0EnableMask) != 0) {
    throw std::runtime_error("hardware debug-register slot 0 is already in use");
  }

  const auto disabled_dr7 =
      snapshot.dr7 & ~(kDr7Slot0EnableMask | kDr7Slot0ControlMask);
  const auto configured_dr7 = disabled_dr7 | std::uint64_t{1} |
                              (std::uint64_t{1} << 16U) | (length_encoding << 18U);
  try {
    lowlevel::set_debug_register(tid, 7, disabled_dr7);
    lowlevel::set_debug_register(tid, 0, address);
    lowlevel::set_debug_register(tid, 6, snapshot.dr6 & ~kDr6Breakpoint0);
    lowlevel::set_debug_register(tid, 7, configured_dr7);
  } catch (...) {
    try {
      lowlevel::set_debug_register(tid, 7, disabled_dr7);
      lowlevel::set_debug_register(tid, 0, snapshot.dr0);
      lowlevel::set_debug_register(tid, 6, snapshot.dr6);
      lowlevel::set_debug_register(tid, 7, snapshot.dr7);
    } catch (...) {
    }
    throw;
  }

  const auto id = next_watchpoint_id_++;
  watchpoint_ = Watchpoint{id, address, length};
  watchpoint_register_snapshot_ = snapshot;
  return id;
}

bool Debugger::remove_watchpoint(std::size_t id) {
  if (!watchpoint_ || watchpoint_->id != id) return false;
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("watchpoints can only be modified while the tracee is stopped");
  }
  restore_watchpoint_registers();
  watchpoint_.reset();
  watchpoint_register_snapshot_.reset();
  return true;
}

std::vector<Watchpoint> Debugger::watchpoints() const {
  if (!watchpoint_) return {};
  return {*watchpoint_};
}

int Debugger::resume_signal(SignalPolicy policy, pid_t tid) const {
  if (policy != SignalPolicy::Forward) return 0;
  const auto pending = pending_signals_.find(tid);
  if (pending == pending_signals_.end()) return 0;
  return pending->second;
}

StopInfo Debugger::continue_execution(SignalPolicy policy) {
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("continue requires a stopped tracee");
  }

  if (pending_breakpoint_step_) {
    if (pending_breakpoint_step_->tid != stopped_tid()) {
      throw std::logic_error("pending breakpoint step belongs to a different thread");
    }
    const auto internal = step_over_pending_breakpoint(false);
    if (internal.reason != StopReason::SingleStep) return internal;
  }

  const auto tid = stopped_tid();
  const int signal = resume_signal(policy, tid);
  lowlevel::continue_process(tid, signal);
  pending_signals_.erase(tid);
  process_.mark_running(tid);
  return wait_and_classify(false);
}

StopInfo Debugger::single_step(SignalPolicy policy) {
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("single-step requires a stopped tracee");
  }
  if (pending_breakpoint_step_) {
    if (pending_breakpoint_step_->tid != stopped_tid()) {
      throw std::logic_error("pending breakpoint step belongs to a different thread");
    }
    return step_over_pending_breakpoint(true);
  }

  const auto tid = stopped_tid();
  const int signal = resume_signal(policy, tid);
  lowlevel::single_step(tid, signal);
  pending_signals_.erase(tid);
  process_.mark_running(tid);
  return wait_and_classify(true);
}

void Debugger::detach(SignalPolicy policy) {
  if (process_.origin() != ProcessOrigin::Attached) {
    throw std::logic_error("detach is only valid for an attached process");
  }
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("detach requires a stopped tracee");
  }

  const auto tid = stopped_tid();
  const int signal = resume_signal(policy, tid);
  restore_all_breakpoints();
  restore_watchpoint_registers();
  process_.detach(signal);
  breakpoints_by_address_.clear();
  breakpoint_ids_.clear();
  pending_breakpoint_step_.reset();
  pending_thread_starts_.clear();
  pending_signals_.clear();
  watchpoint_.reset();
  watchpoint_register_snapshot_.reset();
}

void Debugger::restore_all_breakpoints() {
  const auto tid = stopped_tid();
  for (auto& [address, breakpoint] : breakpoints_by_address_) {
    if (!breakpoint.installed) continue;
    lowlevel::write_byte(tid, address, breakpoint.original_byte);
    breakpoint.installed = false;
  }
  pending_breakpoint_step_.reset();
}

void Debugger::restore_watchpoint_registers() {
  if (!watchpoint_register_snapshot_) return;
  const auto tid = watchpoint_register_snapshot_->tid;
  if (process_.task_state(tid) != ProcessState::Stopped) {
    throw std::logic_error("watchpoint owner thread must be stopped before restore");
  }

  const auto current_dr7 = lowlevel::get_debug_register(tid, 7);
  const auto disabled_dr7 = current_dr7 & ~(kDr7Slot0EnableMask | kDr7Slot0ControlMask);
  lowlevel::set_debug_register(tid, 7, disabled_dr7);
  lowlevel::set_debug_register(tid, 0, watchpoint_register_snapshot_->dr0);
  lowlevel::set_debug_register(tid, 6, watchpoint_register_snapshot_->dr6);
  lowlevel::set_debug_register(tid, 7, watchpoint_register_snapshot_->dr7);
}

void Debugger::discard_image_state() noexcept {
  breakpoints_by_address_.clear();
  breakpoint_ids_.clear();
  pending_breakpoint_step_.reset();
  watchpoint_.reset();
  watchpoint_register_snapshot_.reset();
  pending_thread_starts_.clear();
  pending_signals_.clear();
}

std::optional<StopInfo> Debugger::classify_watchpoint_stop(pid_t tid) {
  if (!watchpoint_ || !watchpoint_register_snapshot_ ||
      watchpoint_register_snapshot_->tid != tid) {
    return std::nullopt;
  }
  const auto dr6 = lowlevel::get_debug_register(tid, 6);
  if ((dr6 & kDr6Breakpoint0) == 0) return std::nullopt;
  lowlevel::set_debug_register(tid, 6, dr6 & ~kDr6Breakpoint0);
  auto info = make_stop(StopReason::Watchpoint, SIGTRAP, tid);
  info.watchpoint_id = watchpoint_->id;
  info.watchpoint_address = watchpoint_->address;
  return info;
}

StopInfo Debugger::step_over_pending_breakpoint(bool expose_single_step) {
  const auto pending = *pending_breakpoint_step_;
  lowlevel::single_step(pending.tid, 0);
  process_.mark_running(pending.tid);
  auto info = wait_and_classify(true);
  if (info.reason == StopReason::Exec) return info;

  pid_t reinsert_tid = -1;
  if (process_.task_state(pending.tid) == ProcessState::Stopped) {
    reinsert_tid = pending.tid;
  } else if (process_.state() == ProcessState::Stopped) {
    reinsert_tid = stopped_tid();
  }
  if (reinsert_tid > 0) reinsert_breakpoint(pending.address, reinsert_tid);
  pending_breakpoint_step_.reset();

  if (expose_single_step && info.reason == StopReason::SingleStep) {
    stop_info_ = info;
  }
  return info;
}

void Debugger::reinsert_breakpoint(std::uintptr_t address, pid_t tid) {
  const auto it = breakpoints_by_address_.find(address);
  if (it == breakpoints_by_address_.end() || it->second.installed) {
    return;
  }
  lowlevel::write_byte(tid, address, kInt3);
  it->second.installed = true;
}

StopInfo Debugger::wait_and_classify(bool expected_single_step) {
  for (;;) {
    const auto event = process_.wait();
    if (event.kind == WaitEvent::Kind::Exited) {
      pending_thread_starts_.erase(event.tid);
      pending_signals_.erase(event.tid);
      if (watchpoint_register_snapshot_ && watchpoint_register_snapshot_->tid == event.tid) {
        watchpoint_.reset();
        watchpoint_register_snapshot_.reset();
      }
      if (process_.tids().empty()) {
        stop_info_ = make_stop(StopReason::Exited, event.value, event.tid);
        return stop_info_;
      }
      if (process_.state() != ProcessState::Stopped) continue;
      stop_info_ = make_stop(StopReason::ThreadExited, event.value, event.tid);
      return stop_info_;
    }
    if (event.kind == WaitEvent::Kind::Signaled) {
      pending_thread_starts_.erase(event.tid);
      pending_signals_.erase(event.tid);
      if (watchpoint_register_snapshot_ && watchpoint_register_snapshot_->tid == event.tid) {
        watchpoint_.reset();
        watchpoint_register_snapshot_.reset();
      }
      if (process_.tids().empty()) {
        stop_info_ = make_stop(StopReason::Signaled, event.value, event.tid);
        return stop_info_;
      }
      if (process_.state() != ProcessState::Stopped) continue;
      stop_info_ = make_stop(StopReason::ThreadSignaled, event.value, event.tid);
      return stop_info_;
    }

    if (event.ptrace_event == PTRACE_EVENT_EXEC) {
      const auto message = lowlevel::get_event_message(event.tid);
      std::optional<pid_t> former_tid;
      if (message != 0) {
        if (message > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
          throw std::runtime_error("PTRACE_EVENT_EXEC returned an invalid former task id");
        }
        former_tid = static_cast<pid_t>(message);
      }

      process_.collapse_after_exec(event.tid);
      discard_image_state();
      lowlevel::set_options(event.tid, tracing_options(process_.origin()));
      executable_path_ = process_executable(process_.pid());
      stop_info_ = make_stop(StopReason::Exec, SIGTRAP, event.tid);
      stop_info_.former_tid = former_tid;
      return stop_info_;
    }

    if (event.ptrace_event == PTRACE_EVENT_CLONE) {
      if (!event.new_tid) {
        throw std::runtime_error("clone event did not provide a new thread id");
      }
      if (watchpoint_) {
        throw std::runtime_error(
            "thread creation while a hardware watchpoint is active is unsupported");
      }
      pending_thread_starts_.insert(*event.new_tid);
      continue;
    }

    if (pending_thread_starts_.erase(event.tid) != 0) {
      stop_info_ = make_stop(StopReason::ThreadCreated, event.value, event.tid);
      stop_info_.new_tid = event.tid;
      return stop_info_;
    }

    const int signal = event.value;
    if (signal != SIGTRAP) {
      pending_signals_[event.tid] = signal;
      stop_info_ = make_stop(StopReason::Signal, signal, event.tid);
      return stop_info_;
    }
    if (const auto watchpoint = classify_watchpoint_stop(event.tid)) {
      stop_info_ = *watchpoint;
      return stop_info_;
    }
    if (expected_single_step) {
      stop_info_ = make_stop(StopReason::SingleStep, SIGTRAP, event.tid);
      return stop_info_;
    }

    auto regs = lowlevel::get_registers(event.tid);
    if (regs.rip > 0) {
      const auto candidate = static_cast<std::uintptr_t>(regs.rip - 1);
      const auto bp = breakpoints_by_address_.find(candidate);
      if (bp != breakpoints_by_address_.end() && bp->second.installed) {
        prepare_breakpoint_hit(candidate, regs, event.tid);
        auto info = make_stop(StopReason::Breakpoint, SIGTRAP, event.tid);
        info.breakpoint_address = candidate;
        stop_info_ = info;
        return stop_info_;
      }
    }

    stop_info_ = make_stop(StopReason::Trap, SIGTRAP, event.tid);
    return stop_info_;
  }
}

void Debugger::prepare_breakpoint_hit(std::uintptr_t address, user_regs_struct regs,
                                      pid_t tid) {
  auto it = breakpoints_by_address_.find(address);
  if (it == breakpoints_by_address_.end() || !it->second.installed) {
    throw std::logic_error("attempted to prepare an unknown breakpoint hit");
  }

  regs.rip = address;
  lowlevel::set_registers(tid, regs);
  lowlevel::write_byte(tid, address, it->second.original_byte);
  it->second.installed = false;
  pending_breakpoint_step_ = PendingBreakpointStep{address, tid};
}

}  // namespace mdbg

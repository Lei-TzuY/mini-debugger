#include "debugger/debugger.hpp"

#include "ptrace/ptrace.hpp"

#include <csignal>
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
}  // namespace

Debugger Debugger::launch(const std::string& executable,
                          const std::vector<std::string>& arguments) {
  return Debugger(Process::launch(executable, arguments),
                  {StopReason::InitialExec, SIGTRAP, std::nullopt});
}

Debugger Debugger::attach(pid_t pid) {
  return Debugger(Process::attach(pid), {StopReason::Attached, SIGSTOP, std::nullopt});
}

Debugger::Debugger(Process process, StopInfo initial_stop)
    : process_(std::move(process)), stop_info_(std::move(initial_stop)) {}

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

user_regs_struct Debugger::registers() const {
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("registers are only available while the tracee is stopped");
  }
  return lowlevel::get_registers(process_.pid());
}

std::vector<std::byte> Debugger::read_memory(std::uintptr_t address,
                                             std::size_t length) const {
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("memory is only available while the tracee is stopped");
  }
  return lowlevel::read_memory(process_.pid(), address, length);
}

std::size_t Debugger::add_breakpoint(std::uintptr_t address) {
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("breakpoints can only be modified while the tracee is stopped");
  }
  if (breakpoints_by_address_.count(address) != 0) {
    throw std::invalid_argument("a breakpoint already exists at that address");
  }

  const auto original = lowlevel::read_byte(process_.pid(), address);
  lowlevel::write_byte(process_.pid(), address, kInt3);
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
    lowlevel::write_byte(process_.pid(), address, bp_it->second.original_byte);
  }
  if (pending_breakpoint_step_ == address) {
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
  if (pending_breakpoint_step_ == address) pending_breakpoint_step_.reset();
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
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("watchpoints can only be modified while the tracee is stopped");
  }
  if (watchpoint_) {
    throw std::invalid_argument("only one hardware watchpoint is currently supported");
  }
  const auto length_encoding = watchpoint_length_encoding(length);
  if (address % length != 0) {
    throw std::invalid_argument("write watchpoint address must be naturally aligned");
  }

  const DebugRegisterSnapshot snapshot{lowlevel::get_debug_register(process_.pid(), 0),
                                       lowlevel::get_debug_register(process_.pid(), 6),
                                       lowlevel::get_debug_register(process_.pid(), 7)};
  if ((snapshot.dr7 & kDr7Slot0EnableMask) != 0) {
    throw std::runtime_error("hardware debug-register slot 0 is already in use");
  }

  const auto disabled_dr7 =
      snapshot.dr7 & ~(kDr7Slot0EnableMask | kDr7Slot0ControlMask);
  const auto configured_dr7 = disabled_dr7 | std::uint64_t{1} |
                              (std::uint64_t{1} << 16U) | (length_encoding << 18U);
  try {
    lowlevel::set_debug_register(process_.pid(), 7, disabled_dr7);
    lowlevel::set_debug_register(process_.pid(), 0, address);
    lowlevel::set_debug_register(process_.pid(), 6, snapshot.dr6 & ~kDr6Breakpoint0);
    lowlevel::set_debug_register(process_.pid(), 7, configured_dr7);
  } catch (...) {
    try {
      lowlevel::set_debug_register(process_.pid(), 7, disabled_dr7);
      lowlevel::set_debug_register(process_.pid(), 0, snapshot.dr0);
      lowlevel::set_debug_register(process_.pid(), 6, snapshot.dr6);
      lowlevel::set_debug_register(process_.pid(), 7, snapshot.dr7);
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

int Debugger::resume_signal(SignalPolicy policy) const {
  if (policy != SignalPolicy::Forward || stop_info_.reason != StopReason::Signal) {
    return 0;
  }
  return stop_info_.value;
}

StopInfo Debugger::continue_execution(SignalPolicy policy) {
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("continue requires a stopped tracee");
  }

  if (pending_breakpoint_step_) {
    const auto internal = step_over_pending_breakpoint(false);
    if (internal.reason != StopReason::SingleStep) return internal;
  }

  const int signal = resume_signal(policy);
  lowlevel::continue_process(process_.pid(), signal);
  process_.mark_running();
  return wait_and_classify(false);
}

StopInfo Debugger::single_step(SignalPolicy policy) {
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("single-step requires a stopped tracee");
  }
  if (pending_breakpoint_step_) {
    return step_over_pending_breakpoint(true);
  }

  const int signal = resume_signal(policy);
  lowlevel::single_step(process_.pid(), signal);
  process_.mark_running();
  return wait_and_classify(true);
}

void Debugger::detach(SignalPolicy policy) {
  if (process_.origin() != ProcessOrigin::Attached) {
    throw std::logic_error("detach is only valid for an attached process");
  }
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("detach requires a stopped tracee");
  }

  const int signal = resume_signal(policy);
  restore_all_breakpoints();
  restore_watchpoint_registers();
  process_.detach(signal);
  breakpoints_by_address_.clear();
  breakpoint_ids_.clear();
  pending_breakpoint_step_.reset();
  watchpoint_.reset();
  watchpoint_register_snapshot_.reset();
}

void Debugger::restore_all_breakpoints() {
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("breakpoints can only be restored while the tracee is stopped");
  }
  for (auto& [address, breakpoint] : breakpoints_by_address_) {
    if (!breakpoint.installed) continue;
    lowlevel::write_byte(process_.pid(), address, breakpoint.original_byte);
    breakpoint.installed = false;
  }
  pending_breakpoint_step_.reset();
}

void Debugger::restore_watchpoint_registers() {
  if (!watchpoint_register_snapshot_) return;
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("watchpoint registers can only be restored while stopped");
  }

  const auto current_dr7 = lowlevel::get_debug_register(process_.pid(), 7);
  const auto disabled_dr7 = current_dr7 & ~(kDr7Slot0EnableMask | kDr7Slot0ControlMask);
  lowlevel::set_debug_register(process_.pid(), 7, disabled_dr7);
  lowlevel::set_debug_register(process_.pid(), 0, watchpoint_register_snapshot_->dr0);
  lowlevel::set_debug_register(process_.pid(), 6, watchpoint_register_snapshot_->dr6);
  lowlevel::set_debug_register(process_.pid(), 7, watchpoint_register_snapshot_->dr7);
}

std::optional<StopInfo> Debugger::classify_watchpoint_stop() {
  if (!watchpoint_) return std::nullopt;
  const auto dr6 = lowlevel::get_debug_register(process_.pid(), 6);
  if ((dr6 & kDr6Breakpoint0) == 0) return std::nullopt;
  lowlevel::set_debug_register(process_.pid(), 6, dr6 & ~kDr6Breakpoint0);
  return StopInfo{StopReason::Watchpoint, SIGTRAP, std::nullopt, watchpoint_->id,
                  watchpoint_->address};
}

StopInfo Debugger::step_over_pending_breakpoint(bool expose_single_step) {
  const auto address = *pending_breakpoint_step_;
  lowlevel::single_step(process_.pid(), 0);
  process_.mark_running();
  auto info = wait_and_classify(true);
  if (process_.state() == ProcessState::Stopped) {
    reinsert_breakpoint(address);
  }
  pending_breakpoint_step_.reset();

  if (expose_single_step && info.reason == StopReason::SingleStep) {
    stop_info_ = info;
  }
  return info;
}

void Debugger::reinsert_breakpoint(std::uintptr_t address) {
  const auto it = breakpoints_by_address_.find(address);
  if (it == breakpoints_by_address_.end() || it->second.installed) {
    return;
  }
  lowlevel::write_byte(process_.pid(), address, kInt3);
  it->second.installed = true;
}

StopInfo Debugger::wait_and_classify(bool expected_single_step) {
  const auto event = process_.wait();
  if (event.kind == WaitEvent::Kind::Exited) {
    stop_info_ = {StopReason::Exited, event.value, std::nullopt};
    return stop_info_;
  }
  if (event.kind == WaitEvent::Kind::Signaled) {
    stop_info_ = {StopReason::Signaled, event.value, std::nullopt};
    return stop_info_;
  }

  const int signal = event.value;
  if (signal != SIGTRAP) {
    stop_info_ = {StopReason::Signal, signal, std::nullopt};
    return stop_info_;
  }
  if (const auto watchpoint = classify_watchpoint_stop()) {
    stop_info_ = *watchpoint;
    return stop_info_;
  }
  if (expected_single_step) {
    stop_info_ = {StopReason::SingleStep, SIGTRAP, std::nullopt};
    return stop_info_;
  }

  auto regs = lowlevel::get_registers(process_.pid());
  if (regs.rip > 0) {
    const auto candidate = static_cast<std::uintptr_t>(regs.rip - 1);
    const auto bp = breakpoints_by_address_.find(candidate);
    if (bp != breakpoints_by_address_.end() && bp->second.installed) {
      prepare_breakpoint_hit(candidate, regs);
      stop_info_ = {StopReason::Breakpoint, SIGTRAP, candidate};
      return stop_info_;
    }
  }

  stop_info_ = {StopReason::Trap, SIGTRAP, std::nullopt};
  return stop_info_;
}

void Debugger::prepare_breakpoint_hit(std::uintptr_t address, user_regs_struct regs) {
  auto it = breakpoints_by_address_.find(address);
  if (it == breakpoints_by_address_.end() || !it->second.installed) {
    throw std::logic_error("attempted to prepare an unknown breakpoint hit");
  }

  regs.rip = address;
  lowlevel::set_registers(process_.pid(), regs);
  lowlevel::write_byte(process_.pid(), address, it->second.original_byte);
  it->second.installed = false;
  pending_breakpoint_step_ = address;
}

}  // namespace mdbg

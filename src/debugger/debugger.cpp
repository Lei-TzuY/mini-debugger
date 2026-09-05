#include "debugger/debugger.hpp"

#include "ptrace/ptrace.hpp"

#include <sys/ptrace.h>
#include <sys/wait.h>

#include <cerrno>
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
constexpr std::size_t kMaxProcessDomains{3};

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
  unsigned long options = PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEFORK |
                          PTRACE_O_TRACEVFORK | PTRACE_O_TRACEVFORKDONE;
  if (origin == ProcessOrigin::Launched) options |= PTRACE_O_EXITKILL;
  return options;
}

unsigned long transient_vfork_child_options(ProcessOrigin origin) {
  unsigned long options = PTRACE_O_TRACEEXEC;
  if (origin == ProcessOrigin::Launched) options |= PTRACE_O_EXITKILL;
  return options;
}

void wait_for_unfollowed_child_stop(pid_t child) {
  int status = 0;
  pid_t result;
  do {
    result = ::waitpid(child, &status, __WALL);
  } while (result == -1 && errno == EINTR);
  if (result != child) {
    throw std::runtime_error("failed to consume process child's initial ptrace stop");
  }
  if (!WIFSTOPPED(status)) {
    throw std::runtime_error("process child did not enter an initial ptrace stop");
  }
}

int wait_for_transient_child(pid_t child) {
  int status = 0;
  pid_t result;
  do {
    result = ::waitpid(child, &status, __WALL);
  } while (result == -1 && errno == EINTR);
  if (result != child) {
    throw std::runtime_error("failed to wait for transient vfork child");
  }
  return status;
}

bool terminal_process_state(ProcessState state) {
  return state == ProcessState::Exited || state == ProcessState::Signaled ||
         state == ProcessState::Detached;
}
}  // namespace

Debugger Debugger::launch(const std::string& executable,
                          const std::vector<std::string>& arguments) {
  auto process = Process::launch(executable, arguments);
  lowlevel::set_options(process.current_tid(), tracing_options(ProcessOrigin::Launched));
  auto initial = make_stop(StopReason::InitialExec, SIGTRAP, process.current_tid());
  const auto executable_path = process_executable(process.pid());
  return Debugger(std::move(process), std::move(initial), executable_path);
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
  if (!retained_processes_.empty()) {
    if (!terminal_process_state(process_.state()) && process_.pid() > 0) {
      (void)::kill(process_.pid(), SIGKILL);
    }
    for (const auto& [pid, domain] : retained_processes_) {
      if (!terminal_process_state(domain.process.state()) && pid > 0) {
        (void)::kill(pid, SIGKILL);
      }
    }
    return;
  }

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

void Debugger::set_fork_follow_policy(ForkFollowPolicy policy) {
  if (!retained_processes_.empty()) {
    if (policy != ForkFollowPolicy::Both) {
      throw std::logic_error(
          "cannot change fork follow policy while process domains are retained");
    }
    fork_follow_policy_ = policy;
    return;
  }
  if (policy == ForkFollowPolicy::Both) {
    if (process_.origin() != ProcessOrigin::Launched) {
      throw std::logic_error(
          "simultaneous fork ownership is currently supported for launched tracees only");
    }
    const auto tids = process_.tids();
    if (tids.size() != 1 || tids.front() != process_.current_tid() ||
        process_.current_tid() != process_.pid()) {
      throw std::logic_error(
          "simultaneous fork ownership currently requires a single-thread parent");
    }
    if (!breakpoints_by_address_.empty() || pending_breakpoint_step_) {
      throw std::logic_error(
          "simultaneous fork ownership requires software breakpoints to be armed after fork");
    }
  }
  fork_follow_policy_ = policy;
}

std::vector<ProcessInfo> Debugger::processes() const {
  std::vector<ProcessInfo> result;
  if (process_.pid() > 0) {
    result.push_back(ProcessInfo{process_.pid(), process_.state(), true});
  }
  for (const auto& [pid, domain] : retained_processes_) {
    if (pid > 0) result.push_back(ProcessInfo{pid, domain.process.state(), false});
  }
  return result;
}

std::optional<pid_t> Debugger::first_stopped_retained_process() const noexcept {
  for (const auto& [pid, domain] : retained_processes_) {
    if (domain.process.state() == ProcessState::Stopped) return pid;
  }
  return std::nullopt;
}

void Debugger::swap_active_process(pid_t pid) {
  auto node = retained_processes_.extract(pid);
  if (node.empty()) {
    throw std::logic_error("no retained process is available for selection");
  }
  const auto old_active_pid = process_.pid();
  auto& domain = node.mapped();
  process_.swap(domain.process);
  std::swap(stop_info_, domain.stop_info);
  std::swap(executable_path_, domain.executable_path);
  pending_thread_starts_.swap(domain.pending_thread_starts);
  pending_signals_.swap(domain.pending_signals);
  breakpoints_by_address_.swap(domain.breakpoints_by_address);
  breakpoint_ids_.swap(domain.breakpoint_ids);
  breakpoint_register_snapshots_.swap(domain.breakpoint_register_snapshots);
  std::swap(pending_breakpoint_step_, domain.pending_breakpoint_step);
  std::swap(watchpoint_, domain.watchpoint);
  std::swap(watchpoint_register_snapshot_, domain.watchpoint_register_snapshot);
  node.key() = old_active_pid;
  const auto inserted = retained_processes_.insert(std::move(node));
  if (!inserted.inserted) {
    throw std::logic_error("active process identity collided with retained registry");
  }
}

void Debugger::select_process(pid_t pid) {
  if (pid == process_.pid()) return;
  const auto selected = retained_processes_.find(pid);
  if (selected == retained_processes_.end()) {
    throw std::invalid_argument("cannot select an untracked process");
  }
  if (process_.state() != ProcessState::Stopped) {
    throw std::logic_error("process selection requires the active process to be stopped");
  }
  if (selected->second.process.state() != ProcessState::Stopped) {
    throw std::logic_error("cannot select a process that is not stopped");
  }
  if (pending_breakpoint_step_) {
    throw std::logic_error("cannot switch processes while a breakpoint displaced step is pending");
  }
  swap_active_process(pid);
  stop_info_.sequence = next_stop_sequence();
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

std::optional<user_regs_struct> Debugger::breakpoint_register_snapshot(
    pid_t tid, std::uintptr_t address) const {
  const auto it = breakpoint_register_snapshots_.find({tid, address});
  if (it == breakpoint_register_snapshots_.end()) return std::nullopt;
  return it->second;
}

std::vector<std::byte> Debugger::read_memory(std::uintptr_t address,
                                             std::size_t length) const {
  return lowlevel::read_memory(stopped_tid(), address, length);
}

std::size_t Debugger::add_breakpoint(std::uintptr_t address) {
  if (fork_follow_policy_ == ForkFollowPolicy::Both && retained_processes_.empty()) {
    throw std::logic_error(
        "process-scoped managed breakpoints can only be armed after fork retention");
  }
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
  if (!retained_processes_.empty()) {
    throw std::logic_error("detach is not yet supported for a multi-process session");
  }
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
  breakpoint_register_snapshots_.clear();
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
  breakpoint_register_snapshots_.clear();
  pending_breakpoint_step_.reset();
  watchpoint_.reset();
  watchpoint_register_snapshot_.reset();
  pending_thread_starts_.clear();
  pending_signals_.clear();
}

void Debugger::discard_breakpoint_register_snapshots(pid_t tid) noexcept {
  for (auto it = breakpoint_register_snapshots_.begin();
       it != breakpoint_register_snapshots_.end();) {
    if (it->first.first == tid) {
      it = breakpoint_register_snapshots_.erase(it);
    } else {
      ++it;
    }
  }
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

WaitEvent Debugger::wait_active_process() {
  if (!retained_processes_.empty()) return process_.wait_current();
  return process_.wait();
}

StopInfo Debugger::wait_and_classify(bool expected_single_step) {
  for (;;) {
    const auto event = wait_active_process();
    if (event.kind == WaitEvent::Kind::Exited) {
      pending_thread_starts_.erase(event.tid);
      pending_signals_.erase(event.tid);
      discard_breakpoint_register_snapshots(event.tid);
      if (watchpoint_register_snapshot_ && watchpoint_register_snapshot_->tid == event.tid) {
        watchpoint_.reset();
        watchpoint_register_snapshot_.reset();
      }
      if (process_.tids().empty()) {
        if (const auto survivor = first_stopped_retained_process()) {
          const auto terminal_pid = process_.pid();
          const auto terminal = make_stop(StopReason::ProcessExited, event.value, event.tid);
          stop_info_ = terminal;
          swap_active_process(*survivor);
          retained_processes_.erase(terminal_pid);
          stop_info_ = terminal;
          return stop_info_;
        }
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
      discard_breakpoint_register_snapshots(event.tid);
      if (watchpoint_register_snapshot_ && watchpoint_register_snapshot_->tid == event.tid) {
        watchpoint_.reset();
        watchpoint_register_snapshot_.reset();
      }
      if (process_.tids().empty()) {
        if (const auto survivor = first_stopped_retained_process()) {
          const auto terminal_pid = process_.pid();
          const auto terminal = make_stop(StopReason::ProcessSignaled, event.value, event.tid);
          stop_info_ = terminal;
          swap_active_process(*survivor);
          retained_processes_.erase(terminal_pid);
          stop_info_ = terminal;
          return stop_info_;
        }
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

    if (event.ptrace_event == PTRACE_EVENT_VFORK) {
      const auto message = lowlevel::get_event_message(event.tid);
      if (message == 0 ||
          message > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        throw std::runtime_error("PTRACE_EVENT_VFORK returned an invalid child process id");
      }
      const auto child = static_cast<pid_t>(message);
      wait_for_unfollowed_child_stop(child);

      bool child_owned = true;
      try {
        if (watchpoint_register_snapshot_ && watchpoint_register_snapshot_->tid == event.tid) {
          const auto& snapshot = *watchpoint_register_snapshot_;
          const auto current_dr7 = lowlevel::get_debug_register(child, 7);
          const auto disabled_dr7 =
              current_dr7 & ~(kDr7Slot0EnableMask | kDr7Slot0ControlMask);
          lowlevel::set_debug_register(child, 7, disabled_dr7);
          lowlevel::set_debug_register(child, 0, snapshot.dr0);
          lowlevel::set_debug_register(child, 6, snapshot.dr6);
          lowlevel::set_debug_register(child, 7, snapshot.dr7);
        }

        lowlevel::set_options(child, transient_vfork_child_options(process_.origin()));
        lowlevel::continue_process(child);

        for (;;) {
          const int status = wait_for_transient_child(child);
          if (WIFEXITED(status) || WIFSIGNALED(status)) {
            child_owned = false;
            break;
          }
          if (!WIFSTOPPED(status)) {
            throw std::runtime_error("vfork child entered an unsupported wait state");
          }

          const auto ptrace_event = static_cast<unsigned int>(status >> 16);
          if (ptrace_event == PTRACE_EVENT_EXEC) {
            lowlevel::detach(child);
            child_owned = false;
            break;
          }
          if (ptrace_event != 0) {
            throw std::runtime_error("vfork child produced an unsupported ptrace event before VM release");
          }

          const int signal = WSTOPSIG(status);
          if (signal == SIGTRAP) {
            throw std::runtime_error("vfork child trapped before releasing the shared address space");
          }
          lowlevel::continue_process(child, signal);
        }

        lowlevel::continue_process(event.tid);
        process_.mark_running(event.tid);
        const auto done = process_.wait();
        if (done.kind != WaitEvent::Kind::Stopped || done.tid != event.tid ||
            done.ptrace_event != PTRACE_EVENT_VFORK_DONE) {
          throw std::runtime_error("followed parent did not stop at PTRACE_EVENT_VFORK_DONE");
        }
        const auto done_message = lowlevel::get_event_message(done.tid);
        if (done_message != static_cast<std::uint64_t>(child)) {
          throw std::runtime_error("PTRACE_EVENT_VFORK_DONE did not match the released child");
        }
      } catch (...) {
        if (child_owned) {
          try {
            lowlevel::detach(child, SIGKILL);
          } catch (...) {
            (void)::kill(child, SIGKILL);
          }
        }
        throw;
      }

      stop_info_ = make_stop(StopReason::Trap, SIGTRAP, event.tid);
      stop_info_.process_event = ProcessEventKind::Vfork;
      stop_info_.parent_pid = process_.pid();
      stop_info_.child_pid = child;
      return stop_info_;
    }

    if (event.ptrace_event == PTRACE_EVENT_VFORK_DONE) {
      throw std::runtime_error("unexpected standalone PTRACE_EVENT_VFORK_DONE");
    }

    if (event.ptrace_event == PTRACE_EVENT_FORK) {
      const auto message = lowlevel::get_event_message(event.tid);
      if (message == 0 ||
          message > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        throw std::runtime_error("PTRACE_EVENT_FORK returned an invalid child process id");
      }
      const auto child = static_cast<pid_t>(message);
      const auto parent = process_.pid();
      wait_for_unfollowed_child_stop(child);

      if (fork_follow_policy_ == ForkFollowPolicy::Both) {
        if (process_.origin() != ProcessOrigin::Launched) {
          try {
            lowlevel::detach(child, SIGKILL);
          } catch (...) {
            (void)::kill(child, SIGKILL);
          }
          throw std::runtime_error(
              "simultaneous fork ownership is currently supported for launched tracees only");
        }
        const auto parent_tids = process_.tids();
        if (parent_tids.size() != 1 || parent_tids.front() != event.tid || event.tid != parent) {
          try {
            lowlevel::detach(child, SIGKILL);
          } catch (...) {
            (void)::kill(child, SIGKILL);
          }
          throw std::runtime_error(
              "simultaneous fork ownership currently requires a single-thread active process");
        }
        if (1U + retained_processes_.size() >= kMaxProcessDomains) {
          try {
            lowlevel::detach(child, SIGKILL);
          } catch (...) {
            (void)::kill(child, SIGKILL);
          }
          throw std::runtime_error(
              "simultaneous fork ownership currently supports at most three process domains");
        }
        if (!breakpoints_by_address_.empty() || pending_breakpoint_step_) {
          try {
            lowlevel::detach(child, SIGKILL);
          } catch (...) {
            (void)::kill(child, SIGKILL);
          }
          throw std::logic_error(
              "simultaneous fork ownership requires software breakpoints to be armed after fork");
        }

        bool child_retained = false;
        try {
          lowlevel::set_options(child, tracing_options(process_.origin()));

          std::optional<Watchpoint> child_watchpoint;
          std::optional<DebugRegisterSnapshot> child_watchpoint_snapshot;
          if (watchpoint_) {
            if (!watchpoint_register_snapshot_ ||
                watchpoint_register_snapshot_->tid != event.tid) {
              throw std::logic_error(
                  "watchpoint ownership does not match the process that forked");
            }
            const auto& snapshot = *watchpoint_register_snapshot_;
            const auto armed_dr0 = lowlevel::get_debug_register(event.tid, 0);
            const auto armed_dr6 = lowlevel::get_debug_register(event.tid, 6);
            const auto armed_dr7 = lowlevel::get_debug_register(event.tid, 7);
            const auto child_dr7 = lowlevel::get_debug_register(child, 7);
            const auto disabled_child_dr7 =
                child_dr7 & ~(kDr7Slot0EnableMask | kDr7Slot0ControlMask);
            lowlevel::set_debug_register(child, 7, disabled_child_dr7);
            lowlevel::set_debug_register(child, 0, armed_dr0);
            lowlevel::set_debug_register(child, 6, armed_dr6 & ~kDr6Breakpoint0);
            lowlevel::set_debug_register(child, 7, armed_dr7);
            child_watchpoint = watchpoint_;
            child_watchpoint_snapshot = DebugRegisterSnapshot{
                child, snapshot.dr0, snapshot.dr6, snapshot.dr7};
          }

          auto child_stop = make_stop(StopReason::Trap, SIGSTOP, child);
          const auto inserted = retained_processes_.emplace(
              child, RetainedProcessDomain{
                         Process::adopt_stopped(child, process_.origin()), std::move(child_stop),
                         executable_path_, {}, {}, {}, {}, {}, std::nullopt,
                         child_watchpoint, child_watchpoint_snapshot});
          if (!inserted.second) {
            throw std::logic_error("fork child process identity already exists in registry");
          }
          child_retained = true;
        } catch (...) {
          if (!child_retained) {
            try {
              lowlevel::detach(child, SIGKILL);
            } catch (...) {
              (void)::kill(child, SIGKILL);
            }
          }
          throw;
        }

        stop_info_ = make_stop(StopReason::Trap, SIGTRAP, event.tid);
        stop_info_.process_event = ProcessEventKind::Fork;
        stop_info_.parent_pid = parent;
        stop_info_.child_pid = child;
        stop_info_.retains_child = true;
        return stop_info_;
      }

      if (fork_follow_policy_ == ForkFollowPolicy::Child) {
        if (process_.origin() != ProcessOrigin::Launched) {
          try {
            lowlevel::detach(child, SIGKILL);
          } catch (...) {
            (void)::kill(child, SIGKILL);
          }
          throw std::runtime_error(
              "follow-child fork handoff is currently supported for launched tracees only");
        }
        const auto parent_tids = process_.tids();
        if (parent_tids.size() != 1 || parent_tids.front() != event.tid || event.tid != parent) {
          try {
            lowlevel::detach(child, SIGKILL);
          } catch (...) {
            (void)::kill(child, SIGKILL);
          }
          throw std::runtime_error(
              "follow-child fork handoff currently requires a single-thread parent");
        }

        bool child_adopted = false;
        try {
          lowlevel::set_options(child, tracing_options(process_.origin()));

          for (const auto& [address, breakpoint] : breakpoints_by_address_) {
            if (!breakpoint.installed) continue;
            lowlevel::write_byte(event.tid, address, breakpoint.original_byte);
          }

          const bool transfer_watchpoint =
              watchpoint_register_snapshot_ && watchpoint_register_snapshot_->tid == event.tid;
          if (transfer_watchpoint) {
            const auto armed_dr0 = lowlevel::get_debug_register(event.tid, 0);
            const auto armed_dr6 = lowlevel::get_debug_register(event.tid, 6);
            const auto armed_dr7 = lowlevel::get_debug_register(event.tid, 7);
            const auto child_dr7 = lowlevel::get_debug_register(child, 7);
            const auto disabled_child_dr7 =
                child_dr7 & ~(kDr7Slot0EnableMask | kDr7Slot0ControlMask);
            lowlevel::set_debug_register(child, 7, disabled_child_dr7);
            lowlevel::set_debug_register(child, 0, armed_dr0);
            lowlevel::set_debug_register(child, 6, armed_dr6 & ~kDr6Breakpoint0);
            lowlevel::set_debug_register(child, 7, armed_dr7);
          }
          if (watchpoint_register_snapshot_) {
            restore_watchpoint_registers();
          }

          lowlevel::detach(event.tid);
          process_.adopt_stopped_process(child);
          child_adopted = true;

          if (transfer_watchpoint) {
            watchpoint_register_snapshot_->tid = child;
          } else {
            watchpoint_.reset();
            watchpoint_register_snapshot_.reset();
          }
          pending_thread_starts_.clear();
          pending_signals_.clear();
          breakpoint_register_snapshots_.clear();
        } catch (...) {
          if (!child_adopted) {
            try {
              lowlevel::detach(child, SIGKILL);
            } catch (...) {
              (void)::kill(child, SIGKILL);
            }
          }
          throw;
        }

        stop_info_ = make_stop(StopReason::Trap, SIGTRAP, child);
        stop_info_.process_event = ProcessEventKind::Fork;
        stop_info_.parent_pid = parent;
        stop_info_.child_pid = child;
        return stop_info_;
      }

      bool detached = false;
      try {
        for (const auto& [address, breakpoint] : breakpoints_by_address_) {
          if (!breakpoint.installed) continue;
          lowlevel::write_byte(child, address, breakpoint.original_byte);
        }

        if (watchpoint_register_snapshot_ && watchpoint_register_snapshot_->tid == event.tid) {
          const auto& snapshot = *watchpoint_register_snapshot_;
          const auto current_dr7 = lowlevel::get_debug_register(child, 7);
          const auto disabled_dr7 =
              current_dr7 & ~(kDr7Slot0EnableMask | kDr7Slot0ControlMask);
          lowlevel::set_debug_register(child, 7, disabled_dr7);
          lowlevel::set_debug_register(child, 0, snapshot.dr0);
          lowlevel::set_debug_register(child, 6, snapshot.dr6);
          lowlevel::set_debug_register(child, 7, snapshot.dr7);
        }

        lowlevel::detach(child);
        detached = true;
      } catch (...) {
        if (!detached) {
          try {
            lowlevel::detach(child, SIGKILL);
          } catch (...) {
          }
        }
        throw;
      }

      stop_info_ = make_stop(StopReason::Trap, SIGTRAP, event.tid);
      stop_info_.process_event = ProcessEventKind::Fork;
      stop_info_.parent_pid = parent;
      stop_info_.child_pid = child;
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
  breakpoint_register_snapshots_[{tid, address}] = regs;
  lowlevel::set_registers(tid, regs);
  lowlevel::write_byte(tid, address, it->second.original_byte);
  it->second.installed = false;
  pending_breakpoint_step_ = PendingBreakpointStep{address, tid};
}

}  // namespace mdbg
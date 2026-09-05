#include "debugger/debugger.hpp"
#include "elf/elf.hpp"
#include "process/process.hpp"
#include "ptrace/ptrace.hpp"

#include <sys/ptrace.h>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

struct FixtureAddresses {
  std::uintptr_t one;
  std::uintptr_t two;
  std::uintptr_t value;
};

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string temp_path() {
  char pattern[] = "/tmp/mdbg-fixture-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd == -1) throw std::runtime_error("mkstemp failed");
  ::close(fd);
  ::unlink(pattern);
  return pattern;
}

FixtureAddresses read_addresses(const std::string& path) {
  std::ifstream input(path);
  std::string one, two, value;
  input >> one >> two >> value;
  if (!input) throw std::runtime_error("fixture did not publish addresses");
  return {static_cast<std::uintptr_t>(std::stoull(one, nullptr, 0)),
          static_cast<std::uintptr_t>(std::stoull(two, nullptr, 0)),
          static_cast<std::uintptr_t>(std::stoull(value, nullptr, 0))};
}

bool contains_tid(const std::vector<pid_t>& tids, pid_t tid) {
  return std::find(tids.begin(), tids.end(), tid) != tids.end();
}

void resume_task(mdbg::Process& process, pid_t tid) {
  mdbg::lowlevel::continue_process(tid);
  process.mark_running(tid);
}

void test_multithread_process_lifecycle(const std::string& fixture) {
  auto process = mdbg::Process::launch(fixture, {"thread-lifecycle"});
  const auto leader = process.pid();

  require(process.current_tid() == leader,
          "initial exec stop must identify the thread-group leader");
  require(process.task_state(leader) == mdbg::ProcessState::Stopped,
          "leader must be tracked as stopped after exec");
  require(process.tids().size() == 1 && process.tids().front() == leader,
          "initial traced-task registry must contain only the leader");

  resume_task(process, leader);

  std::optional<pid_t> worker_tid;
  bool saw_clone = false;
  bool saw_worker_stop = false;
  bool saw_worker_exit = false;
  bool saw_leader_exit = false;

  for (int events = 0; events < 16 && !saw_leader_exit; ++events) {
    const auto event = process.wait();
    require(event.tid > 0, "wait event must identify a concrete TID");

    if (event.kind == mdbg::WaitEvent::Kind::Stopped) {
      require(process.current_tid() == event.tid,
              "current TID must follow the most recent stop event");
      require(process.task_state(event.tid) == mdbg::ProcessState::Stopped,
              "stopped TID must be tracked as stopped");

      if (event.ptrace_event == PTRACE_EVENT_CLONE) {
        require(!saw_clone, "fixture must create exactly one traced worker");
        require(event.new_tid.has_value(),
                "clone stop must expose the kernel-reported new TID");
        require(*event.new_tid != leader,
                "clone event must report a distinct worker TID");
        require(contains_tid(process.tids(), *event.new_tid),
                "new TID must enter the traced-task registry at clone stop");
        worker_tid = *event.new_tid;
        saw_clone = true;
      } else if (worker_tid && event.tid == *worker_tid) {
        saw_worker_stop = true;
      }

      resume_task(process, event.tid);
      continue;
    }

    if (event.kind == mdbg::WaitEvent::Kind::Exited) {
      if (worker_tid && event.tid == *worker_tid) {
        require(event.value == 0, "worker thread must exit cleanly");
        require(!contains_tid(process.tids(), *worker_tid),
                "exited worker TID must leave the traced-task registry");
        saw_worker_exit = true;
        continue;
      }
      if (event.tid == leader) {
        require(event.value == 0, "thread-group leader must exit cleanly");
        saw_leader_exit = true;
        continue;
      }
      throw std::runtime_error("unexpected traced task exited");
    }

    throw std::runtime_error("multithread fixture terminated by signal");
  }

  require(saw_clone, "PTRACE_O_TRACECLONE did not surface pthread creation");
  require(worker_tid.has_value(), "worker TID was never discovered");
  require(saw_worker_stop, "new worker TID never produced its initial ptrace stop");
  require(saw_worker_exit, "worker thread exit was not observed");
  require(saw_leader_exit, "leader exit was not observed");
  require(process.tids().empty(),
          "traced-task registry must be empty after the thread group exits");
  require(process.state() == mdbg::ProcessState::Exited,
          "process state must converge to exited after all traced tasks leave");
}

struct Session {
  std::string path;
  mdbg::Debugger debugger;
  FixtureAddresses addresses;

  Session(const std::string& fixture, const std::string& mode)
      : path(temp_path()), debugger(mdbg::Debugger::launch(fixture, {path, mode})),
        addresses{} {
    require(debugger.stop_info().reason == mdbg::StopReason::InitialExec,
            "launch should expose initial exec stop");
    require(debugger.stop_info().tid == debugger.pid(),
            "initial exec stop must identify the leader TID");
    require(debugger.state() == mdbg::ProcessState::Stopped,
            "tracee should be stopped after launch");
    const auto sync = debugger.continue_execution();
    require(sync.reason == mdbg::StopReason::Signal && sync.value == SIGSTOP,
            "fixture synchronization SIGSTOP was not observed");
    require(sync.tid == debugger.pid(),
            "fixture synchronization stop must identify the leader TID");
    addresses = read_addresses(path);
  }

  ~Session() { std::remove(path.c_str()); }
};

void test_worker_thread_breakpoint_execution(const std::string& fixture) {
  Session session(fixture, "thread-breakpoint");
  const auto leader = session.debugger.pid();
  const auto breakpoint_id = session.debugger.add_breakpoint(session.addresses.one);
  require(breakpoint_id == 1, "first managed breakpoint id must start at one");

  auto info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::ThreadCreated,
          "pthread clone must surface one thread-created stop");
  require(info.new_tid.has_value() && *info.new_tid == info.tid && info.tid != leader,
          "thread-created stop must identify the new worker TID");
  const auto worker = info.tid;
  require(session.debugger.active_tid() == worker,
          "new worker must become the active stopped TID");
  require(session.debugger.registers().rip != 0,
          "register access must target the active worker TID");

  info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint && info.tid == worker &&
              info.breakpoint_address == session.addresses.one,
          "worker did not hit the process-wide managed breakpoint");
  require(session.debugger.active_tid() == worker,
          "breakpoint stop must preserve worker as the active TID");
  require(session.debugger.registers().rip == session.addresses.one,
          "worker RIP was not repaired to the managed breakpoint address");

  info = session.debugger.single_step();
  require(info.reason == mdbg::StopReason::SingleStep && info.tid == worker,
          "displaced breakpoint step must execute on the worker TID");
  require(session.debugger.active_tid() == worker,
          "single-step stop must remain associated with the worker TID");
  require(session.debugger.registers().rip != session.addresses.one,
          "worker single-step did not advance RIP");

  info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint && info.tid == worker &&
              info.breakpoint_address == session.addresses.one,
          "process-wide breakpoint was not reinserted for the worker's second call");

  info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::ThreadExited && info.tid == worker &&
              info.value == 0,
          "worker exit must not be misreported as whole-process exit");
  require(session.debugger.active_tid() == leader,
          "leader must become active after the worker exits");
  require(session.debugger.state() == mdbg::ProcessState::Stopped,
          "leader must remain stopped while worker lifecycle events are surfaced");

  info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Exited && info.tid == leader && info.value == 0,
          "leader did not exit cleanly after worker completion");
}

void test_normal_exit(const std::string& fixture) {
  Session session(fixture, "exit");
  const auto info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Exited && info.value == 0,
          "normal exit was not reported correctly");
}

void test_elf_runtime_symbol_resolution(const std::string& fixture) {
  Session session(fixture, "sequence");
  const mdbg::ElfFile elf(fixture);
  const auto one = elf.find_symbol("breakpoint_one");
  const auto two = elf.find_symbol("breakpoint_two");
  require(one && two, "fixture ELF symbols should be present");
  require(elf.runtime_address(session.debugger.pid(), *one) == session.addresses.one,
          "symbol-to-runtime resolution mismatch");
  require(elf.runtime_address(session.debugger.pid(), *two) == session.addresses.two,
          "second symbol-to-runtime resolution mismatch");
  const auto reverse = elf.find_symbol_by_runtime_address(session.debugger.pid(),
                                                           session.addresses.one);
  require(reverse && reverse->symbol.name == "breakpoint_one" && reverse->offset == 0,
          "runtime address-to-symbol resolution mismatch");
}

void test_registers_and_memory(const std::string& fixture) {
  Session session(fixture, "sequence");
  const auto regs = session.debugger.registers();
  require(regs.rip != 0 && regs.rsp != 0, "x86-64 registers should be readable");
  const auto bytes = session.debugger.read_memory(session.addresses.value, sizeof(std::uint64_t));
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[i])) << (i * 8U);
  }
  require(value == 0x1122334455667788ULL, "fixture memory value did not match");
}

void test_repeated_breakpoint_and_clean_exit(const std::string& fixture) {
  Session session(fixture, "sequence");
  session.debugger.add_breakpoint(session.addresses.one);

  auto info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint &&
              info.breakpoint_address == session.addresses.one,
          "first breakpoint hit missing");
  require(session.debugger.registers().rip == session.addresses.one,
          "RIP was not rewound after INT3");
  const auto original = session.debugger.read_memory(session.addresses.one, 1);
  require(std::to_integer<unsigned>(original.front()) != 0xcc,
          "original byte should be restored while stopped at breakpoint");

  info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint &&
              info.breakpoint_address == session.addresses.one,
          "breakpoint was not reinserted for repeated hit");

  info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Exited && info.value == 0,
          "tracee did not exit cleanly with breakpoint still owned by debugger");
}

void test_single_step_after_breakpoint(const std::string& fixture) {
  Session session(fixture, "sequence");
  session.debugger.add_breakpoint(session.addresses.one);
  auto info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint, "expected breakpoint before stepi");
  info = session.debugger.single_step();
  require(info.reason == mdbg::StopReason::SingleStep,
          "stepi after breakpoint should expose the single-step SIGTRAP");
  require(session.debugger.registers().rip != session.addresses.one,
          "single step did not advance RIP");
  info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint &&
              info.breakpoint_address == session.addresses.one,
          "breakpoint was not reinserted after explicit stepi");
}

void test_multiple_and_delete_breakpoints(const std::string& fixture) {
  Session session(fixture, "sequence");
  const auto first_id = session.debugger.add_breakpoint(session.addresses.one);
  const auto second_id = session.debugger.add_breakpoint(session.addresses.two);
  (void)first_id;

  auto info = session.debugger.continue_execution();
  require(info.breakpoint_address == session.addresses.one, "first breakpoint order mismatch");
  info = session.debugger.continue_execution();
  require(info.breakpoint_address == session.addresses.two, "second breakpoint order mismatch");
  require(session.debugger.remove_breakpoint(second_id), "delete should remove existing breakpoint");
  require(!session.debugger.remove_breakpoint(second_id), "deleting breakpoint twice should fail");

  info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint &&
              info.breakpoint_address == session.addresses.one,
          "remaining breakpoint should still fire after deleting another");
}

void test_delete_installed_breakpoint(const std::string& fixture) {
  Session session(fixture, "sequence");
  const auto id = session.debugger.add_breakpoint(session.addresses.two);
  require(session.debugger.remove_breakpoint(id), "installed breakpoint delete failed");
  const auto info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Exited && info.value == 0,
          "deleted breakpoint should not trap");
}

void test_invalid_breakpoint_address(const std::string& fixture) {
  Session session(fixture, "sequence");
  bool failed = false;
  try {
    session.debugger.add_breakpoint(1);
  } catch (const mdbg::lowlevel::PtraceError&) {
    failed = true;
  }
  require(failed, "invalid breakpoint address should fail with ptrace error");
  require(session.debugger.breakpoints().empty(),
          "failed breakpoint insertion must not mutate debugger state");
}

void test_invalid_memory_address(const std::string& fixture) {
  Session session(fixture, "sequence");
  bool failed = false;
  try {
    (void)session.debugger.read_memory(1, 8);
  } catch (const mdbg::lowlevel::PtraceError&) {
    failed = true;
  }
  require(failed, "invalid memory read should fail with ptrace error");
}

void test_write_watchpoint_coexists_with_software_breakpoint(const std::string& fixture) {
  Session session(fixture, "watchpoint");
  const mdbg::ElfFile elf(fixture);
  const auto probe = elf.find_symbol("watchpoint_write_probe");
  require(probe.has_value(), "watchpoint store probe symbol is unavailable");
  const auto probe_address = static_cast<std::uintptr_t>(
      elf.runtime_address(session.debugger.pid(), *probe));

  bool invalid_length = false;
  try {
    (void)session.debugger.add_write_watchpoint(session.addresses.value, 3);
  } catch (const std::invalid_argument&) {
    invalid_length = true;
  }
  require(invalid_length, "unsupported watchpoint length was accepted");

  bool misaligned = false;
  try {
    (void)session.debugger.add_write_watchpoint(session.addresses.value + 1, 2);
  } catch (const std::invalid_argument&) {
    misaligned = true;
  }
  require(misaligned, "misaligned watchpoint address was accepted");

  const auto software_id = session.debugger.add_breakpoint(probe_address);
  const auto watchpoint_id =
      session.debugger.add_write_watchpoint(session.addresses.value, sizeof(std::uint64_t));
  const auto watchpoints = session.debugger.watchpoints();
  require(watchpoints.size() == 1 && watchpoints.front().id == watchpoint_id &&
              watchpoints.front().address == session.addresses.value &&
              watchpoints.front().length == sizeof(std::uint64_t),
          "write watchpoint ownership was not recorded");

  bool second_slot = false;
  try {
    (void)session.debugger.add_write_watchpoint(session.addresses.value, 1);
  } catch (const std::invalid_argument&) {
    second_slot = true;
  }
  require(second_slot, "single-slot watchpoint bound was not enforced");

  auto info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint &&
              info.breakpoint_address == probe_address,
          "software breakpoint did not stop on the watched store instruction");

  info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Watchpoint &&
              info.watchpoint_id == watchpoint_id &&
              info.watchpoint_address == session.addresses.value &&
              !info.breakpoint_address,
          "watchpoint was not surfaced from the software-breakpoint displaced step");

  const auto bytes =
      session.debugger.read_memory(session.addresses.value, sizeof(std::uint64_t));
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[i])) << (i * 8U);
  }
  require(value == 0x1122334455667789ULL,
          "watched store did not commit before the hardware trap");

  require(session.debugger.remove_breakpoint(software_id),
          "software breakpoint could not be removed after watchpoint stop");
  require(session.debugger.remove_watchpoint(watchpoint_id),
          "write watchpoint could not be removed");
  require(!session.debugger.remove_watchpoint(watchpoint_id),
          "deleting the same watchpoint twice should fail");
  require(session.debugger.watchpoints().empty(),
          "watchpoint ownership remained after deletion");

  info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Exited && info.value == 0,
          "deleted watchpoint trapped the second watched store or fixture failed to exit");
}

void test_unmanaged_sigtrap(const std::string& fixture) {
  Session session(fixture, "trap");
  auto info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Trap,
          "unmanaged SIGTRAP must not be reported as a breakpoint");
  info = session.debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Exited && info.value == 0,
          "tracee should continue after suppressed SIGTRAP");
}

void test_signal_suppression_and_forwarding(const std::string& fixture) {
  {
    Session session(fixture, "signal");
    auto info = session.debugger.continue_execution();
    require(info.reason == mdbg::StopReason::Signal && info.value == SIGUSR1,
            "SIGUSR1 stop missing");
    info = session.debugger.continue_execution(mdbg::SignalPolicy::Suppress);
    require(info.reason == mdbg::StopReason::Exited && info.value == 0,
            "suppressed signal should allow clean exit");
  }
  {
    Session session(fixture, "terminate");
    auto info = session.debugger.continue_execution();
    require(info.reason == mdbg::StopReason::Signal && info.value == SIGTERM,
            "SIGTERM stop missing");
    info = session.debugger.continue_execution(mdbg::SignalPolicy::Forward);
    require(info.reason == mdbg::StopReason::Signaled && info.value == SIGTERM,
            "forwarded SIGTERM should terminate tracee");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: debugger_integration <fixture>\n";
    return 2;
  }
  try {
    const std::string fixture = argv[1];
    test_multithread_process_lifecycle(fixture);
    test_worker_thread_breakpoint_execution(fixture);
    test_normal_exit(fixture);
    test_elf_runtime_symbol_resolution(fixture);
    test_registers_and_memory(fixture);
    test_repeated_breakpoint_and_clean_exit(fixture);
    test_single_step_after_breakpoint(fixture);
    test_multiple_and_delete_breakpoints(fixture);
    test_delete_installed_breakpoint(fixture);
    test_invalid_breakpoint_address(fixture);
    test_invalid_memory_address(fixture);
    test_write_watchpoint_coexists_with_software_breakpoint(fixture);
    test_unmanaged_sigtrap(fixture);
    test_signal_suppression_and_forwarding(fixture);
    std::cout << "all debugger integration tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << "integration test failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

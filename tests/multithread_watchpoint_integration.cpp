#include "debugger/debugger.hpp"
#include "elf/elf.hpp"
#include "ptrace/ptrace.hpp"

#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kInitialValue = 0x1122334455667788ULL;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string temp_path() {
  char pattern[] = "/tmp/mdbg-thread-watchpoint-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd == -1) throw std::runtime_error("mkstemp failed");
  ::close(fd);
  ::unlink(pattern);
  return pattern;
}

std::vector<pid_t> task_ids(pid_t leader) {
  std::vector<pid_t> result;
  const auto path = std::filesystem::path("/proc") / std::to_string(leader) / "task";
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    result.push_back(static_cast<pid_t>(std::stol(entry.path().filename().string())));
  }
  std::sort(result.begin(), result.end());
  return result;
}

pid_t worker_tid(pid_t leader, const std::vector<pid_t>& tids) {
  for (const auto tid : tids) {
    if (tid != leader) return tid;
  }
  throw std::runtime_error("worker TID is unavailable");
}

struct PublishedAddresses {
  std::uintptr_t breakpoint_one;
  std::uintptr_t breakpoint_two;
  std::uintptr_t value;
};

PublishedAddresses read_addresses(const std::string& path) {
  std::ifstream input(path);
  std::string one, two, value;
  input >> one >> two >> value;
  if (!input) throw std::runtime_error("fixture did not publish addresses");
  return PublishedAddresses{static_cast<std::uintptr_t>(std::stoull(one, nullptr, 0)),
                            static_cast<std::uintptr_t>(std::stoull(two, nullptr, 0)),
                            static_cast<std::uintptr_t>(std::stoull(value, nullptr, 0))};
}

struct DebugRegisters {
  std::uint64_t dr0;
  std::uint64_t dr6;
  std::uint64_t dr7;
};

DebugRegisters debug_registers(pid_t tid) {
  return DebugRegisters{mdbg::lowlevel::get_debug_register(tid, 0),
                        mdbg::lowlevel::get_debug_register(tid, 6),
                        mdbg::lowlevel::get_debug_register(tid, 7)};
}

void require_debug_registers(const DebugRegisters& actual, const DebugRegisters& expected,
                             const std::string& message) {
  require(actual.dr0 == expected.dr0 && actual.dr6 == expected.dr6 &&
              actual.dr7 == expected.dr7,
          message);
}

std::uint64_t read_value(const mdbg::Debugger& debugger, std::uintptr_t address) {
  const auto bytes = debugger.read_memory(address, sizeof(std::uint64_t));
  require(bytes.size() == sizeof(std::uint64_t), "failed to read watched value");
  std::uint64_t value = 0;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

struct Child {
  pid_t pid{-1};
  std::string path;
};

Child spawn_fixture(const std::string& fixture) {
  Child child{-1, temp_path()};
  child.pid = ::fork();
  if (child.pid == -1) throw std::runtime_error("fork failed");
  if (child.pid == 0) {
    ::execl(fixture.c_str(), fixture.c_str(), child.path.c_str(),
            "attach-thread-watchpoint", nullptr);
    _exit(127);
  }

  for (int attempt = 0; attempt < 200; ++attempt) {
    if (std::filesystem::exists(child.path)) return child;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ::kill(child.pid, SIGKILL);
  int status = 0;
  while (::waitpid(child.pid, &status, 0) == -1 && errno == EINTR) {
  }
  std::remove(child.path.c_str());
  throw std::runtime_error("thread watchpoint fixture did not become ready");
}

void terminate_child(const Child& child) noexcept {
  if (child.pid > 0) {
    ::kill(child.pid, SIGKILL);
    int status = 0;
    while (::waitpid(child.pid, &status, 0) == -1 && errno == EINTR) {
    }
  }
  std::remove(child.path.c_str());
}

void release_detached_child(const Child& child) {
  require(::kill(child.pid, SIGUSR2) == 0, "failed to release detached watchpoint fixture");
  int status = 0;
  pid_t result;
  do {
    result = ::waitpid(child.pid, &status, 0);
  } while (result == -1 && errno == EINTR);
  require(result == child.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "detached watchpoint fixture did not exit cleanly");
  std::remove(child.path.c_str());
}

std::uintptr_t watchpoint_probe(const std::string& fixture, pid_t pid) {
  const mdbg::ElfFile elf(fixture);
  const auto symbol = elf.find_symbol("watchpoint_write_probe");
  require(symbol.has_value(), "watchpoint_write_probe symbol is unavailable");
  return static_cast<std::uintptr_t>(elf.runtime_address(pid, *symbol));
}

void test_worker_owned_watchpoint(const std::string& fixture) {
  const auto child = spawn_fixture(fixture);
  try {
    const auto tids = task_ids(child.pid);
    require(tids.size() == 2, "watchpoint fixture must expose one leader and one worker");
    const auto worker = worker_tid(child.pid, tids);
    const auto addresses = read_addresses(child.path);

    auto debugger = mdbg::Debugger::attach(child.pid);
    const auto leader_before = debug_registers(child.pid);
    const auto worker_before = debug_registers(worker);
    debugger.select_thread(worker);

    const auto breakpoint_id = debugger.add_breakpoint(watchpoint_probe(fixture, child.pid));
    const auto watchpoint_id = debugger.add_write_watchpoint(addresses.value, sizeof(std::uint64_t));
    require(watchpoint_id != 0, "thread-scoped watchpoint id must be non-zero");
    require(debugger.watchpoints().size() == 1,
            "thread-scoped watchpoint did not remain visible through the debugger API");

    require_debug_registers(debug_registers(child.pid), leader_before,
                            "arming the worker watchpoint changed leader debug registers");
    const auto worker_armed = debug_registers(worker);
    require(worker_armed.dr0 == addresses.value,
            "worker DR0 did not receive the watched address");
    require((worker_armed.dr7 & 0x3U) != 0,
            "worker DR7 did not enable hardware slot 0");
    require(worker_armed.dr0 != worker_before.dr0 || worker_armed.dr7 != worker_before.dr7,
            "worker debug-register state did not change when watchpoint was armed");

    debugger.select_thread(child.pid);
    require(debugger.active_tid() == child.pid,
            "non-owner leader could not be selected while worker watchpoint was active");
    require_debug_registers(debug_registers(child.pid), leader_before,
                            "selecting the non-owner leader inherited worker debug registers");
    require_debug_registers(debug_registers(worker), worker_armed,
                            "selecting the non-owner leader changed owner debug registers");
    debugger.select_thread(worker);
    require(debugger.active_tid() == worker,
            "watchpoint owner could not be reselected after non-owner selection");

    require(::syscall(SYS_tgkill, child.pid, worker, SIGUSR2) == 0,
            "failed to queue targeted worker release signal");
    auto info = debugger.continue_execution();
    require(info.reason == mdbg::StopReason::Signal && info.value == SIGUSR2 &&
                info.tid == worker,
            "worker did not surface its targeted release signal");

    info = debugger.continue_execution(mdbg::SignalPolicy::Forward);
    require(info.reason == mdbg::StopReason::Breakpoint && info.tid == worker &&
                info.breakpoint_address == watchpoint_probe(fixture, child.pid),
            "worker did not hit the process-wide breakpoint on the watched store");

    info = debugger.continue_execution();
    require(info.reason == mdbg::StopReason::Watchpoint && info.tid == worker &&
                info.watchpoint_id == watchpoint_id &&
                info.watchpoint_address == addresses.value,
            "displaced watched store was not classified as the worker-owned watchpoint");
    require(read_value(debugger, addresses.value) == kInitialValue + 1,
            "worker watched store did not commit before the watchpoint stop");
    require(debugger.remove_breakpoint(breakpoint_id),
            "worker software breakpoint could not be removed after watchpoint stop");

    info = debugger.continue_execution();
    require(info.reason == mdbg::StopReason::ThreadExited && info.tid == worker,
            "worker exit was not surfaced after the watchpoint stop");
    require(debugger.watchpoints().empty(),
            "exited watchpoint owner left stale debugger watchpoint ownership");
    require_debug_registers(debug_registers(child.pid), leader_before,
                            "worker watchpoint lifetime changed leader debug registers");

    debugger.select_thread(child.pid);
    info = debugger.continue_execution();
    require(info.reason == mdbg::StopReason::Exited && info.value == 0,
            "non-owner leader did not execute the same watched store and exit cleanly");
    std::remove(child.path.c_str());
  } catch (...) {
    terminate_child(child);
    throw;
  }
}

void test_remove_restores_owner(const std::string& fixture) {
  const auto child = spawn_fixture(fixture);
  try {
    const auto tids = task_ids(child.pid);
    require(tids.size() == 2, "remove fixture must expose one leader and one worker");
    const auto worker = worker_tid(child.pid, tids);
    const auto addresses = read_addresses(child.path);

    auto debugger = mdbg::Debugger::attach(child.pid);
    debugger.select_thread(worker);
    const auto before = debug_registers(worker);
    const auto id = debugger.add_write_watchpoint(addresses.value, sizeof(std::uint64_t));
    require(debugger.remove_watchpoint(id), "thread-scoped watchpoint removal failed");
    require_debug_registers(debug_registers(worker), before,
                            "watchpoint removal did not restore exact owner debug registers");
    debugger.detach();
    release_detached_child(child);
  } catch (...) {
    terminate_child(child);
    throw;
  }
}

void test_explicit_detach_restores_owner(const std::string& fixture) {
  const auto child = spawn_fixture(fixture);
  try {
    const auto tids = task_ids(child.pid);
    require(tids.size() == 2, "detach fixture must expose one leader and one worker");
    const auto worker = worker_tid(child.pid, tids);
    const auto addresses = read_addresses(child.path);

    auto debugger = mdbg::Debugger::attach(child.pid);
    const auto leader_before = debug_registers(child.pid);
    debugger.select_thread(worker);
    const auto worker_before = debug_registers(worker);
    (void)debugger.add_write_watchpoint(addresses.value, sizeof(std::uint64_t));
    debugger.detach();

    auto verify = mdbg::Debugger::attach(child.pid);
    require_debug_registers(debug_registers(child.pid), leader_before,
                            "explicit detach changed non-owner leader debug registers");
    verify.select_thread(worker);
    require_debug_registers(debug_registers(worker), worker_before,
                            "explicit detach did not restore exact owner debug registers");
    verify.detach();
    release_detached_child(child);
  } catch (...) {
    terminate_child(child);
    throw;
  }
}

void test_destructor_restores_owner(const std::string& fixture) {
  const auto child = spawn_fixture(fixture);
  try {
    const auto tids = task_ids(child.pid);
    require(tids.size() == 2, "destructor fixture must expose one leader and one worker");
    const auto worker = worker_tid(child.pid, tids);
    const auto addresses = read_addresses(child.path);
    DebugRegisters leader_before{};
    DebugRegisters worker_before{};
    {
      auto debugger = mdbg::Debugger::attach(child.pid);
      leader_before = debug_registers(child.pid);
      debugger.select_thread(worker);
      worker_before = debug_registers(worker);
      (void)debugger.add_write_watchpoint(addresses.value, sizeof(std::uint64_t));
    }

    auto verify = mdbg::Debugger::attach(child.pid);
    require_debug_registers(debug_registers(child.pid), leader_before,
                            "debugger destructor changed non-owner leader debug registers");
    verify.select_thread(worker);
    require_debug_registers(debug_registers(worker), worker_before,
                            "debugger destructor did not restore exact owner debug registers");
    verify.detach();
    release_detached_child(child);
  } catch (...) {
    terminate_child(child);
    throw;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    test_worker_owned_watchpoint(argv[1]);
    test_remove_restores_owner(argv[1]);
    test_explicit_detach_restores_owner(argv[1]);
    test_destructor_restores_owner(argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "multithread watchpoint integration failure: %s\n", error.what());
    return 1;
  }
}

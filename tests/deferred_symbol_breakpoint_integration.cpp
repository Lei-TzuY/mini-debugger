#include "breakpoints/user_breakpoint_registry.hpp"
#include "debugger/debugger.hpp"
#include "elf/elf.hpp"
#include "loader/deferred_symbol_breakpoints.hpp"
#include "unwind/cfi.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void test_deferred_symbol_across_dlopen(const std::string& driver,
                                       const std::string& plugin) {
  auto debugger = mdbg::Debugger::launch(driver, {plugin});
  const mdbg::ElfFile executable(driver);
  mdbg::DeferredSymbolBreakpoints deferred(debugger, executable);

  require(!mdbg::find_module_symbol_by_name(debugger.pid(), "deferred_target", executable),
          "deferred target must not be loaded at the initial exec stop");

  const auto request_id = deferred.add("deferred_target");
  auto requests = deferred.breakpoints();
  require(requests.size() == 1 && requests.front().request_id == request_id,
          "deferred request was not recorded");
  require(!requests.front().breakpoint_id && !requests.front().address,
          "unloaded symbol must begin in the pending state");

  const auto hit = deferred.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address,
          "deferred symbol did not produce a managed breakpoint stop");

  requests = deferred.breakpoints();
  require(requests.size() == 1 && requests.front().breakpoint_id && requests.front().address,
          "loader event did not resolve the pending symbol request");
  require(*requests.front().address == *hit.breakpoint_address,
          "resolved deferred address does not match the breakpoint stop");

  const auto resolved =
      mdbg::find_module_symbol_by_name(debugger.pid(), "deferred_target", executable);
  require(resolved && resolved->address == *hit.breakpoint_address,
          "deferred stop does not belong to the loaded plugin symbol");

  const auto managed = debugger.breakpoints();
  require(managed.size() == 1,
          "internal loader/bootstrap breakpoints leaked after deferred resolution");
  require(managed.front().id == *requests.front().breakpoint_id &&
              managed.front().address == *requests.front().address,
          "remaining managed breakpoint is not the resolved deferred target");

  const auto done = deferred.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "dlopen fixture did not exit cleanly after the deferred breakpoint");
}

void test_user_breakpoint_registry(const std::string& driver, const std::string& plugin) {
  auto debugger = mdbg::Debugger::launch(driver, {plugin});
  const mdbg::ElfFile executable(driver);
  mdbg::UserBreakpointRegistry breakpoints(debugger, executable);

  const auto loaded_id = breakpoints.add_symbol("main");
  require(loaded_id == 1, "first user breakpoint id must start at one");
  const auto loaded = breakpoints.breakpoint(loaded_id);
  require(loaded && loaded->address && loaded->state == mdbg::UserBreakpointState::Enabled,
          "loaded symbol must become an enabled managed breakpoint");

  const auto deferred_id = breakpoints.add_symbol("deferred_target");
  require(deferred_id == 2, "user ids must remain monotonic across breakpoint backends");
  const auto pending = breakpoints.breakpoint(deferred_id);
  require(pending && !pending->address && pending->state == mdbg::UserBreakpointState::Pending,
          "unloaded symbol must be represented as pending");
  require(breakpoints.has_pending(), "registry must report its unresolved deferred request");

  require(breakpoints.remove(loaded_id), "loaded user breakpoint could not be removed");
  require(!breakpoints.breakpoint(loaded_id), "removed user breakpoint remained visible");

  const auto hit = breakpoints.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address,
          "registry deferred breakpoint did not stop after dlopen");

  const auto resolved = breakpoints.breakpoint(deferred_id);
  require(resolved && resolved->address == hit.breakpoint_address,
          "resolved registry address does not match the breakpoint stop");
  require(resolved->state == mdbg::UserBreakpointState::TemporarilyRestored,
          "breakpoint hit must expose displaced-execution ownership state");
  require(!breakpoints.has_pending(), "resolved user breakpoint must leave pending state");

  require(breakpoints.remove(deferred_id), "resolved user breakpoint could not be removed");
  require(breakpoints.breakpoints().empty(), "user registry did not become empty");
  require(debugger.breakpoints().empty(), "backend breakpoint leaked after user deletion");

  const auto done = breakpoints.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "tracee did not exit cleanly after deleting the resolved user breakpoint");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    test_deferred_symbol_across_dlopen(argv[1], argv[2]);
    test_user_breakpoint_registry(argv[1], argv[2]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "deferred symbol breakpoint integration failure: %s\n", error.what());
    return 1;
  }
}

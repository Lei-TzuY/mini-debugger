#include "breakpoints/user_breakpoint_registry.hpp"
#include "debugger/debugger.hpp"
#include "elf/elf.hpp"
#include "loader/deferred_symbol_breakpoints.hpp"
#include "unwind/cfi.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void test_deferred_symbol_across_reload(const std::string& driver,
                                        const std::string& plugin) {
  auto debugger = mdbg::Debugger::launch(driver, {plugin});
  const mdbg::ElfFile executable(driver);
  mdbg::DeferredSymbolBreakpoints deferred(debugger, executable);

  require(!mdbg::find_module_symbol_by_name(debugger.pid(), "deferred_target", executable),
          "deferred target must not be loaded at the initial exec stop");
  const auto barrier =
      mdbg::find_module_symbol_by_name(debugger.pid(), "reload_barrier", executable);
  require(barrier.has_value(), "reload barrier symbol is unavailable");
  const auto barrier_breakpoint_id = debugger.add_breakpoint(barrier->address);

  const auto request_id = deferred.add("deferred_target");
  auto requests = deferred.breakpoints();
  require(requests.size() == 1 && requests.front().request_id == request_id,
          "deferred request was not recorded");
  require(!requests.front().breakpoint_id && !requests.front().address,
          "unloaded symbol must begin in the pending state");

  const auto first_hit = deferred.continue_execution();
  require(first_hit.reason == mdbg::StopReason::Breakpoint && first_hit.breakpoint_address,
          "first plugin load did not reach the deferred symbol");

  requests = deferred.breakpoints();
  require(requests.size() == 1 && requests.front().breakpoint_id && requests.front().address,
          "first loader event did not resolve the pending symbol request");
  require(*requests.front().address == *first_hit.breakpoint_address,
          "first resolved deferred address does not match the breakpoint stop");
  const auto first_backend_id = *requests.front().breakpoint_id;

  auto managed = debugger.breakpoints();
  require(managed.size() == 2,
          "user target and reload barrier should be the only visible managed breakpoints");
  require(std::none_of(managed.begin(), managed.end(), [&](const mdbg::Breakpoint& breakpoint) {
            return breakpoint.address != barrier->address &&
                   breakpoint.address != *first_hit.breakpoint_address;
          }),
          "internal loader breakpoint leaked into a user-visible stop");

  const auto barrier_hit = deferred.continue_execution();
  require(barrier_hit.reason == mdbg::StopReason::Breakpoint &&
              barrier_hit.breakpoint_address == barrier->address,
          "tracee did not stop at the reload barrier after dlclose");

  requests = deferred.breakpoints();
  require(requests.size() == 1 && !requests.front().breakpoint_id && !requests.front().address,
          "unloaded module did not return the deferred request to pending state");
  require(!mdbg::find_module_symbol_by_name(debugger.pid(), "deferred_target", executable),
          "plugin symbol remained mapped after dlclose barrier");

  managed = debugger.breakpoints();
  require(managed.size() == 1 && managed.front().id == barrier_breakpoint_id &&
              managed.front().address == barrier->address,
          "stale plugin breakpoint ownership survived module unload");
  require(!managed.front().installed,
          "reload barrier must be temporarily restored at its breakpoint stop");
  require(debugger.remove_breakpoint(barrier_breakpoint_id),
          "reload barrier breakpoint could not be removed");

  const auto second_hit = deferred.continue_execution();
  require(second_hit.reason == mdbg::StopReason::Breakpoint && second_hit.breakpoint_address,
          "plugin reload did not re-arm the deferred symbol breakpoint");

  requests = deferred.breakpoints();
  require(requests.size() == 1 && requests.front().breakpoint_id && requests.front().address,
          "reloaded plugin did not resolve the pending request again");
  require(*requests.front().address == *second_hit.breakpoint_address,
          "re-armed deferred address does not match the second breakpoint stop");
  require(*requests.front().breakpoint_id != first_backend_id,
          "reload reused stale backend breakpoint ownership instead of re-arming");

  managed = debugger.breakpoints();
  require(managed.size() == 1 && managed.front().id == *requests.front().breakpoint_id,
          "internal loader breakpoint leaked after reload resolution");

  require(deferred.remove(request_id), "re-armed deferred request could not be removed");
  require(debugger.breakpoints().empty(),
          "managed breakpoint state leaked after removing the re-armed request");

  const auto done = deferred.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "reload fixture did not exit cleanly after the second breakpoint");
}

void test_user_breakpoint_registry_reload(const std::string& driver,
                                          const std::string& plugin) {
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

  const auto barrier_id = breakpoints.add_symbol("reload_barrier");
  require(barrier_id == 3, "reload barrier must share the same user id namespace");
  const auto barrier = breakpoints.breakpoint(barrier_id);
  require(barrier && barrier->address, "reload barrier did not resolve immediately");

  require(breakpoints.remove(loaded_id), "loaded user breakpoint could not be removed");

  const auto first_hit = breakpoints.continue_execution();
  require(first_hit.reason == mdbg::StopReason::Breakpoint && first_hit.breakpoint_address,
          "registry deferred breakpoint did not stop on first dlopen");
  const auto first_resolved = breakpoints.breakpoint(deferred_id);
  require(first_resolved && first_resolved->address == first_hit.breakpoint_address &&
              first_resolved->state == mdbg::UserBreakpointState::TemporarilyRestored,
          "user deferred breakpoint did not expose its first resolved stop");
  require(!breakpoints.has_pending(), "resolved user breakpoint unexpectedly remained pending");

  const auto barrier_hit = breakpoints.continue_execution();
  require(barrier_hit.reason == mdbg::StopReason::Breakpoint &&
              barrier_hit.breakpoint_address == barrier->address,
          "registry did not expose the post-dlclose reload barrier");

  const auto pending_again = breakpoints.breakpoint(deferred_id);
  require(pending_again && !pending_again->address &&
              pending_again->state == mdbg::UserBreakpointState::Pending,
          "same user breakpoint id did not return to pending after unload");
  require(breakpoints.has_pending(), "registry did not report the unloaded request as pending");
  require(breakpoints.remove(barrier_id), "reload barrier user breakpoint could not be removed");

  const auto second_hit = breakpoints.continue_execution();
  require(second_hit.reason == mdbg::StopReason::Breakpoint && second_hit.breakpoint_address,
          "registry deferred breakpoint did not re-arm after plugin reload");
  const auto second_resolved = breakpoints.breakpoint(deferred_id);
  require(second_resolved && second_resolved->id == deferred_id &&
              second_resolved->address == second_hit.breakpoint_address &&
              second_resolved->state == mdbg::UserBreakpointState::TemporarilyRestored,
          "user breakpoint identity was not preserved across unload and reload");
  require(!breakpoints.has_pending(), "re-armed user breakpoint remained pending");

  require(breakpoints.remove(deferred_id), "re-armed user breakpoint could not be removed");
  require(breakpoints.breakpoints().empty(), "user registry did not become empty");
  require(debugger.breakpoints().empty(), "backend breakpoint leaked after user deletion");

  const auto done = breakpoints.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "tracee did not exit cleanly after deleting the re-armed user breakpoint");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    test_deferred_symbol_across_reload(argv[1], argv[2]);
    test_user_breakpoint_registry_reload(argv[1], argv[2]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "deferred symbol breakpoint integration failure: %s\n", error.what());
    return 1;
  }
}

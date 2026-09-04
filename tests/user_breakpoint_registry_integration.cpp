#include "breakpoints/user_breakpoint_registry.hpp"
#include "debugger/debugger.hpp"
#include "elf/elf.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void test_registry_lifecycle(const std::string& driver, const std::string& plugin) {
  auto debugger = mdbg::Debugger::launch(driver, {plugin});
  const mdbg::ElfFile executable(driver);
  mdbg::UserBreakpointRegistry breakpoints(debugger, executable);

  const auto loaded_id = breakpoints.add_symbol("main");
  require(loaded_id == 1, "first user breakpoint id must start at one");
  const auto loaded = breakpoints.breakpoint(loaded_id);
  require(loaded && loaded->address && loaded->state == mdbg::UserBreakpointState::Enabled,
          "loaded symbol must become an enabled managed breakpoint");

  const auto deferred_id = breakpoints.add_symbol("deferred_target");
  require(deferred_id == 2, "user breakpoint ids must remain monotonic across backends");
  const auto pending = breakpoints.breakpoint(deferred_id);
  require(pending && !pending->address && pending->state == mdbg::UserBreakpointState::Pending,
          "unloaded symbol must be represented as pending");
  require(breakpoints.has_pending(), "registry must report an unresolved deferred breakpoint");

  require(breakpoints.remove(loaded_id), "loaded user breakpoint could not be removed");
  require(!breakpoints.breakpoint(loaded_id), "removed user breakpoint remained visible");

  const auto hit = breakpoints.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address,
          "deferred breakpoint did not stop after dlopen");

  const auto resolved = breakpoints.breakpoint(deferred_id);
  require(resolved && resolved->address == hit.breakpoint_address,
          "resolved user breakpoint address does not match the stop");
  require(resolved->state == mdbg::UserBreakpointState::TemporarilyRestored,
          "breakpoint hit must expose displaced-execution ownership state");
  require(!breakpoints.has_pending(), "resolved breakpoint must leave pending state");

  require(breakpoints.remove(deferred_id), "resolved deferred breakpoint could not be removed");
  require(breakpoints.breakpoints().empty(), "user breakpoint registry did not become empty");
  require(debugger.breakpoints().empty(), "backend breakpoint state leaked after user deletion");

  const auto done = breakpoints.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "tracee did not exit cleanly after deleting the resolved breakpoint");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    test_registry_lifecycle(argv[1], argv[2]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "user breakpoint registry integration failure: %s\n", error.what());
    return 1;
  }
}

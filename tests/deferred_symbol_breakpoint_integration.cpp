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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    test_deferred_symbol_across_dlopen(argv[1], argv[2]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "deferred symbol breakpoint integration failure: %s\n", error.what());
    return 1;
  }
}

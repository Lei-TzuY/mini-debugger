#include "debugger/debugger.hpp"
#include "dwarf/eh_frame.hpp"
#include "elf/elf.hpp"
#include "ptrace/ptrace.hpp"
#include "source/source_finish.hpp"
#include "unwind/cfi.hpp"

#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_frame_name(const mdbg::ElfFile& elf, pid_t pid, std::uintptr_t address,
                        const std::string& expected) {
  const auto resolved = elf.find_symbol_by_runtime_address(pid, address);
  require(resolved && resolved->symbol.name == expected,
          "expected frame " + expected + " at address " + std::to_string(address));
}

void test_shared_library_cfi(const std::string& driver, const std::string& library) {
  auto debugger = mdbg::Debugger::launch(driver, {});
  const auto sync = debugger.continue_execution();
  require(sync.reason == mdbg::StopReason::Signal && sync.value == SIGSTOP,
          "shared-CFI fixture synchronization SIGSTOP was not observed");

  const mdbg::ElfFile executable(driver);
  const mdbg::EhFrame executable_cfi(driver);
  const mdbg::ElfFile shared(library);
  const auto probe = shared.find_symbol("shared_cfi_probe");
  require(probe.has_value(), "shared_cfi_probe symbol is missing");
  const auto probe_address =
      static_cast<std::uintptr_t>(shared.runtime_address(debugger.pid(), *probe));

  debugger.add_breakpoint(probe_address);
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint &&
              hit.breakpoint_address == probe_address,
          "shared-library CFI probe breakpoint was not hit");

  auto regs = debugger.registers();
  regs.rbp = 3;
  mdbg::lowlevel::set_registers(debugger.pid(), regs);

  const auto trace = mdbg::unwind_eh_frame(debugger, executable, executable_cfi, 16);
  require(trace.frames.size() >= 4,
          "module-aware CFI unwind returned fewer than four frames");
  require(trace.frames[0].instruction_pointer == probe_address,
          "shared-library CFI top frame did not preserve repaired RIP");
  require_frame_name(shared, debugger.pid(), trace.frames[1].instruction_pointer,
                     "shared_inner");
  require_frame_name(shared, debugger.pid(), trace.frames[2].instruction_pointer,
                     "shared_outer");
  require_frame_name(executable, debugger.pid(), trace.frames[3].instruction_pointer,
                     "main");

  const auto limited = mdbg::unwind_eh_frame(debugger, executable, executable_cfi, 2);
  require(limited.frames.size() == 2 &&
              limited.stop_reason == mdbg::CfiUnwindStopReason::FrameLimit,
          "shared-library CFI frame limit must remain deterministic");

  const auto finish = mdbg::finish_frame(debugger);
  require(finish.reason == mdbg::FinishStopReason::Returned,
          "shared-library finish did not report a normal return");
  require(finish.stop.reason == mdbg::StopReason::Breakpoint &&
              finish.stop.breakpoint_address == finish.return_address,
          "shared-library finish did not stop on its return-address breakpoint");
  require_frame_name(shared, debugger.pid(), finish.return_address, "shared_inner");

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "shared-library CFI fixture did not exit cleanly");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    test_shared_library_cfi(argv[1], argv[2]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "shared-CFI backtrace integration failure: %s\n", error.what());
    return 1;
  }
}

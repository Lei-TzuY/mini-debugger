#include "debugger/debugger.hpp"
#include "dwarf/eh_frame.hpp"
#include "elf/elf.hpp"
#include "ptrace/ptrace.hpp"
#include "source/source_finish.hpp"
#include "unwind/cfi.hpp"

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_same_module(const std::string& actual, const std::string& expected,
                         const std::string& message) {
  std::error_code error;
  const bool equivalent = std::filesystem::equivalent(actual, expected, error);
  require(!error && equivalent, message);
}

void require_module_symbol(const mdbg::ElfFile& executable, pid_t pid, std::uintptr_t address,
                           const std::string& expected_module,
                           const std::string& expected_symbol) {
  const auto resolved =
      mdbg::find_module_symbol_by_runtime_address(pid, address, executable);
  require(resolved && resolved->name == expected_symbol,
          "expected module symbol " + expected_symbol + " at address " +
              std::to_string(address));
  require_same_module(resolved->module_path, expected_module,
                      "module symbol " + expected_symbol +
                          " resolved from the wrong ELF image");
}

void require_module_source(const mdbg::ElfFile& executable, pid_t pid, std::uintptr_t address,
                           const std::string& expected_module,
                           const std::string& expected_file) {
  const auto resolved =
      mdbg::find_module_source_by_runtime_address(pid, address, executable);
  require(resolved.has_value(),
          "expected module source at address " + std::to_string(address));
  require(std::filesystem::path(resolved->file).filename() == expected_file,
          "module source resolved to the wrong source file: " + resolved->file);
  require(resolved->line != 0, "module source must expose a non-zero source line");
  require_same_module(resolved->module_path, expected_module,
                      "module source " + expected_file +
                          " resolved from the wrong ELF image");
}

void test_shared_library_cfi(const std::string& driver, const std::string& library) {
  auto debugger = mdbg::Debugger::launch(driver, {});
  const auto sync = debugger.continue_execution();
  require(sync.reason == mdbg::StopReason::Signal && sync.value == SIGSTOP,
          "shared-CFI fixture synchronization SIGSTOP was not observed");

  const mdbg::ElfFile executable(driver);
  const mdbg::EhFrame executable_cfi(driver);
  const mdbg::ElfFile shared(library);

  const auto shared_source = mdbg::find_module_source_by_file_line(
      debugger.pid(), "shared_break_source.c", 700, executable);
  require(shared_source.has_value(),
          "loaded shared-object source line did not resolve to a runtime address");
  require(shared_source->line == 700 &&
              std::filesystem::path(shared_source->file).filename() ==
                  "shared_break_source.c",
          "shared-object reverse lookup resolved the wrong source row");
  require_same_module(shared_source->module_path, library,
                      "shared-object reverse lookup resolved the wrong module");

  const auto driver_source = mdbg::find_module_source_by_file_line(
      debugger.pid(), "driver_break_source.c", 800, executable);
  require(driver_source.has_value(),
          "main-executable source line did not resolve through module routing");
  require(driver_source->line == 800 &&
              std::filesystem::path(driver_source->file).filename() ==
                  "driver_break_source.c",
          "main-executable reverse lookup resolved the wrong source row");
  require_same_module(driver_source->module_path, driver,
                      "main-executable reverse lookup resolved the wrong module");

  bool ambiguous = false;
  try {
    (void)mdbg::find_module_source_by_file_line(
        debugger.pid(), "ambiguous_break_source.c", 900, executable);
  } catch (const std::runtime_error& error) {
    ambiguous = std::string(error.what()).find(
                    "ambiguous source location across loaded modules") != std::string::npos;
  }
  require(ambiguous,
          "source line present in multiple loaded modules must be rejected as ambiguous");

  require(!mdbg::find_module_source_by_file_line(
              debugger.pid(), "missing_break_source.c", 1, executable),
          "unknown source line must not resolve to a loaded module");

  const auto probe = shared.find_symbol("shared_cfi_probe");
  require(probe.has_value(), "shared_cfi_probe symbol is missing");
  const auto probe_address =
      static_cast<std::uintptr_t>(shared.runtime_address(debugger.pid(), *probe));

  const auto source_breakpoint_id = debugger.add_breakpoint(shared_source->address);
  debugger.add_breakpoint(probe_address);

  const auto source_hit = debugger.continue_execution();
  require(source_hit.reason == mdbg::StopReason::Breakpoint &&
              source_hit.breakpoint_address == shared_source->address,
          "module-aware source breakpoint in shared object was not hit");
  const auto hit_source = mdbg::find_module_source_by_runtime_address(
      debugger.pid(), *source_hit.breakpoint_address, executable);
  require(hit_source && hit_source->line == 700 &&
              std::filesystem::path(hit_source->file).filename() ==
                  "shared_break_source.c",
          "shared-object source breakpoint stopped on the wrong source row");

  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint &&
              hit.breakpoint_address == probe_address,
          "shared-library CFI probe breakpoint was not hit after source step-over");

  bool source_breakpoint_reinserted = false;
  for (const auto& breakpoint : debugger.breakpoints()) {
    if (breakpoint.id == source_breakpoint_id) {
      source_breakpoint_reinserted = breakpoint.installed;
      break;
    }
  }
  require(source_breakpoint_reinserted,
          "shared-object source breakpoint was not reinserted after displaced execution");

  auto regs = debugger.registers();
  regs.rbp = 3;
  mdbg::lowlevel::set_registers(debugger.pid(), regs);

  const auto trace = mdbg::unwind_eh_frame(debugger, executable, executable_cfi, 16);
  require(trace.frames.size() >= 4,
          "module-aware CFI unwind returned fewer than four frames");
  require(trace.frames[0].instruction_pointer == probe_address,
          "shared-library CFI top frame did not preserve repaired RIP");
  require_module_symbol(executable, debugger.pid(), trace.frames[0].instruction_pointer,
                        library, "shared_cfi_probe");
  require_module_symbol(executable, debugger.pid(), trace.frames[1].instruction_pointer,
                        library, "shared_inner");
  require_module_symbol(executable, debugger.pid(), trace.frames[2].instruction_pointer,
                        library, "shared_outer");
  require_module_symbol(executable, debugger.pid(), trace.frames[3].instruction_pointer,
                        driver, "main");
  require_module_source(executable, debugger.pid(), trace.frames[0].instruction_pointer,
                        library, "shared_cfi_library.c");
  require_module_source(executable, debugger.pid(), trace.frames[1].instruction_pointer,
                        library, "shared_cfi_library.c");
  require_module_source(executable, debugger.pid(), trace.frames[2].instruction_pointer,
                        library, "shared_cfi_library.c");
  require_module_source(executable, debugger.pid(), trace.frames[3].instruction_pointer,
                        driver, "shared_cfi_driver.c");
  require(!mdbg::find_module_symbol_by_runtime_address(
              debugger.pid(), trace.frames[0].stack_pointer, executable),
          "anonymous stack mapping must not be symbolized as a file-backed module");
  require(!mdbg::find_module_source_by_runtime_address(
              debugger.pid(), trace.frames[0].stack_pointer, executable),
          "anonymous stack mapping must not be source-mapped as a file-backed module");

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
  require_module_symbol(executable, debugger.pid(), finish.return_address,
                        library, "shared_inner");
  require_module_source(executable, debugger.pid(), finish.return_address,
                        library, "shared_cfi_library.c");

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "shared-library CFI fixture did not exit cleanly");
}

void test_unsupported_module_source(const std::string& dwarf5_fixture) {
  auto debugger = mdbg::Debugger::launch(dwarf5_fixture, {});
  const mdbg::ElfFile executable(dwarf5_fixture);
  const auto probe = executable.find_symbol("line_probe");
  require(probe.has_value(), "DWARF5 fixture line_probe symbol is missing");
  const auto address =
      static_cast<std::uintptr_t>(executable.runtime_address(debugger.pid(), *probe));
  require(!mdbg::find_module_source_by_runtime_address(
              debugger.pid(), address, executable),
          "unsupported DWARF module source must degrade to no source information");
  require(!mdbg::find_module_source_by_file_line(
              debugger.pid(), "mapped_source.c", 400, executable),
          "unsupported DWARF reverse lookup must degrade to no source information");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  try {
    test_shared_library_cfi(argv[1], argv[2]);
    test_unsupported_module_source(argv[3]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "shared-CFI backtrace integration failure: %s\n", error.what());
    return 1;
  }
}

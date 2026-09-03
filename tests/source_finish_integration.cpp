#include "debugger/debugger.hpp"
#include "dwarf/line_table.hpp"
#include "elf/elf.hpp"
#include "ptrace/ptrace.hpp"
#include "source/source_finish.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_source_line(const std::optional<mdbg::SourceLocation>& source,
                         std::uint64_t line) {
  require(source.has_value(), "expected a source location");
  require(std::filesystem::path(source->file).filename() == "next_source.c",
          "finish returned an unexpected source file: " + source->file);
  require(source->line == line,
          "finish returned line " + std::to_string(source->line) +
              " instead of " + std::to_string(line));
}

std::uintptr_t source_address(const mdbg::DwarfLineTable& lines, const mdbg::ElfFile& elf,
                              const mdbg::Debugger& debugger, std::uint64_t line) {
  const auto address = lines.find_runtime_source(debugger.pid(), "next_source.c", line, elf);
  require(address.has_value(), "finish fixture is missing line " + std::to_string(line));
  return static_cast<std::uintptr_t>(*address);
}

int marker_value(const mdbg::Debugger& debugger, const mdbg::ElfFile& elf) {
  const auto marker = elf.find_symbol("next_marker");
  require(marker.has_value(), "next_marker symbol is missing");
  const auto address = elf.runtime_address(debugger.pid(), *marker);
  const auto bytes = debugger.read_memory(static_cast<std::uintptr_t>(address), sizeof(int));
  require(bytes.size() == sizeof(int), "failed to read next_marker");
  int value = 0;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

bool has_breakpoint_at(const mdbg::Debugger& debugger, std::uintptr_t address) {
  for (const auto& breakpoint : debugger.breakpoints()) {
    if (breakpoint.address == address) return true;
  }
  return false;
}

void require_breakpoint_state(const mdbg::Debugger& debugger, std::size_t id, bool installed) {
  for (const auto& breakpoint : debugger.breakpoints()) {
    if (breakpoint.id == id) {
      require(breakpoint.installed == installed, "breakpoint installation state is wrong");
      return;
    }
  }
  throw std::runtime_error("expected breakpoint disappeared");
}

void test_finish_returns_to_caller(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line510 = source_address(lines, elf, debugger, 510);
  const auto line501 = source_address(lines, elf, debugger, 501);

  const auto start_id = debugger.add_breakpoint(line510);
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == line510,
          "finish start breakpoint was not hit");

  const auto result = mdbg::finish_frame(debugger);
  require(result.reason == mdbg::FinishStopReason::Returned,
          "finish did not stop at the current frame's return address");
  require(result.stop.reason == mdbg::StopReason::Breakpoint &&
              result.stop.breakpoint_address == line501,
          "finish return stop was not classified at caller line 501");
  require(result.return_address == line501 && debugger.registers().rip == line501,
          "finish did not stop at the exact saved return address");
  require_source_line(lines.find_runtime_address(debugger.pid(), debugger.registers().rip, elf),
                      501);
  require(marker_value(debugger, elf) == 1,
          "callee side effect must complete before finish returns to the caller");
  require_breakpoint_state(debugger, start_id, true);
  require(debugger.breakpoints().size() == 1,
          "temporary finish breakpoint leaked after normal return");

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "finish fixture did not exit cleanly after normal return");
}

void test_callee_breakpoint_interrupts_finish(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line510 = source_address(lines, elf, debugger, 510);
  const auto line511 = source_address(lines, elf, debugger, 511);
  const auto line501 = source_address(lines, elf, debugger, 501);

  debugger.add_breakpoint(line510);
  debugger.add_breakpoint(line511);
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == line510,
          "finish interruption start breakpoint was not hit");

  const auto result = mdbg::finish_frame(debugger);
  require(result.reason == mdbg::FinishStopReason::Interrupted,
          "callee user breakpoint must interrupt finish");
  require(result.stop.reason == mdbg::StopReason::Breakpoint &&
              result.stop.breakpoint_address == line511,
          "finish hid or misclassified the callee user breakpoint");
  require_source_line(lines.find_runtime_address(debugger.pid(), debugger.registers().rip, elf),
                      511);
  require(debugger.breakpoints().size() == 2,
          "temporary finish breakpoint leaked after interruption");
  require(!has_breakpoint_at(debugger, line501),
          "finish left its return-address breakpoint installed after interruption");

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "finish fixture did not exit cleanly after interruption");
}

void test_user_return_breakpoint_is_preserved(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line510 = source_address(lines, elf, debugger, 510);
  const auto line501 = source_address(lines, elf, debugger, 501);

  debugger.add_breakpoint(line510);
  const auto return_id = debugger.add_breakpoint(line501);
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == line510,
          "user-return-breakpoint finish start was not hit");

  const auto result = mdbg::finish_frame(debugger);
  require(result.reason == mdbg::FinishStopReason::Interrupted &&
              result.stop.reason == mdbg::StopReason::Breakpoint &&
              result.stop.breakpoint_address == line501,
          "existing user breakpoint at the return address must remain visible");
  require(debugger.breakpoints().size() == 2,
          "finish must not add a duplicate return-address breakpoint");
  require_breakpoint_state(debugger, return_id, false);

  const auto stepped = debugger.single_step();
  require(stepped.reason == mdbg::StopReason::SingleStep,
          "user return breakpoint should remain pending for normal displaced stepping");
  require_breakpoint_state(debugger, return_id, true);
  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "finish fixture did not exit after preserved return breakpoint");
}

void test_invalid_frame_pointer_is_fail_closed(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line510 = source_address(lines, elf, debugger, 510);
  debugger.add_breakpoint(line510);
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == line510,
          "invalid-frame finish start breakpoint was not hit");

  const auto original = debugger.registers();
  const auto original_rip = original.rip;
  const auto original_breakpoints = debugger.breakpoints().size();
  const std::uint64_t bad_frame_pointers[] = {0, 3, 8};
  for (const auto bad_rbp : bad_frame_pointers) {
    auto changed = original;
    changed.rbp = bad_rbp;
    mdbg::lowlevel::set_registers(debugger.pid(), changed);
    bool rejected = false;
    try {
      static_cast<void>(mdbg::finish_frame_pointer(debugger));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "invalid or unreadable frame pointer must reject frame-pointer finish");
    require(debugger.state() == mdbg::ProcessState::Stopped &&
                debugger.registers().rip == original_rip,
            "rejected frame-pointer finish must not move RIP");
    require(debugger.breakpoints().size() == original_breakpoints,
            "rejected frame-pointer finish must not mutate breakpoint state");
  }
  mdbg::lowlevel::set_registers(debugger.pid(), original);

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "tracee did not remain usable after rejected finish attempts");
}

void test_cfi_finish_without_frame_pointer(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line510 = source_address(lines, elf, debugger, 510);
  const auto line501 = source_address(lines, elf, debugger, 501);

  const auto start_id = debugger.add_breakpoint(line510);
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == line510,
          "CFI finish start breakpoint was not hit");

  auto regs = debugger.registers();
  regs.rbp = 3;
  mdbg::lowlevel::set_registers(debugger.pid(), regs);

  const auto result = mdbg::finish_frame(debugger);
  require(result.reason == mdbg::FinishStopReason::Returned &&
              result.return_address == line501 && debugger.registers().rip == line501,
          ".eh_frame finish did not recover the caller return address without RBP");
  require_source_line(lines.find_runtime_address(debugger.pid(), debugger.registers().rip, elf),
                      501);
  require(marker_value(debugger, elf) == 1,
          "CFI finish must execute the callee before returning to the caller");
  require_breakpoint_state(debugger, start_id, true);
  require(debugger.breakpoints().size() == 1,
          "CFI finish leaked a temporary return breakpoint");

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "CFI finish fixture did not exit cleanly");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    test_finish_returns_to_caller(argv[1]);
    test_callee_breakpoint_interrupts_finish(argv[1]);
    test_user_return_breakpoint_is_preserved(argv[1]);
    test_invalid_frame_pointer_is_fail_closed(argv[1]);
    test_cfi_finish_without_frame_pointer(argv[2]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "source-finish integration failure: %s\n", error.what());
    return 1;
  }
}

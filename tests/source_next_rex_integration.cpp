#include "debugger/debugger.hpp"
#include "dwarf/line_table.hpp"
#include "elf/elf.hpp"
#include "source/source_step.hpp"

#include <cstddef>
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

std::uint64_t source_address(const mdbg::DwarfLineTable& lines, const mdbg::ElfFile& elf,
                             const mdbg::Debugger& debugger, std::uint64_t line) {
  const auto address =
      lines.find_runtime_source(debugger.pid(), "next_source.c", line, elf);
  require(address.has_value(), "REX source-next fixture is missing line " +
                                   std::to_string(line));
  return *address;
}

void require_source_line(const std::optional<mdbg::SourceLocation>& source,
                         std::uint64_t line) {
  require(source.has_value(), "expected a REX source location");
  require(std::filesystem::path(source->file).filename() == "next_source.c",
          "REX source next returned an unexpected source file: " + source->file);
  require(source->line == line,
          "REX source next returned line " + std::to_string(source->line) +
              " instead of " + std::to_string(line));
}

std::uint64_t rex_call_address(const mdbg::Debugger& debugger, std::uint64_t line580,
                               std::uint64_t line581) {
  require(line581 > line580, "REX fixture rows are not in executable order");
  const auto span = line581 - line580;
  require(span <= 64, "REX fixture row unexpectedly grew beyond 64 bytes");
  const auto bytes = debugger.read_memory(static_cast<std::uintptr_t>(line580),
                                          static_cast<std::size_t>(span));
  for (std::size_t i = 0; i + 2 < bytes.size(); ++i) {
    if (std::to_integer<unsigned>(bytes[i]) == 0x41U &&
        std::to_integer<unsigned>(bytes[i + 1]) == 0xffU &&
        std::to_integer<unsigned>(bytes[i + 2]) == 0xd0U) {
      return line580 + i;
    }
  }
  throw std::runtime_error(
      "REX source-next fixture does not contain exact 41 ff d0");
}

std::uint64_t rexwb_call_address(const mdbg::Debugger& debugger,
                                 std::uint64_t line600,
                                 std::uint64_t line601) {
  require(line601 > line600, "REX.W+B fixture rows are not in executable order");
  const auto span = line601 - line600;
  require(span <= 64, "REX.W+B fixture row unexpectedly grew beyond 64 bytes");
  const auto bytes = debugger.read_memory(static_cast<std::uintptr_t>(line600),
                                          static_cast<std::size_t>(span));
  for (std::size_t i = 0; i + 2 < bytes.size(); ++i) {
    if (std::to_integer<unsigned>(bytes[i]) == 0x49U &&
        std::to_integer<unsigned>(bytes[i + 1]) == 0xffU &&
        std::to_integer<unsigned>(bytes[i + 2]) == 0xd0U) {
      return line600 + i;
    }
  }
  throw std::runtime_error(
      "REX.W+B source-next fixture does not contain exact 49 ff d0");
}

std::uint64_t rexw_call_address(const mdbg::Debugger& debugger,
                                std::uint64_t line620,
                                std::uint64_t line621) {
  require(line621 > line620, "REX.W fixture rows are not in executable order");
  const auto span = line621 - line620;
  require(span <= 64, "REX.W fixture row unexpectedly grew beyond 64 bytes");
  const auto bytes = debugger.read_memory(static_cast<std::uintptr_t>(line620),
                                          static_cast<std::size_t>(span));
  for (std::size_t i = 0; i + 2 < bytes.size(); ++i) {
    if (std::to_integer<unsigned>(bytes[i]) == 0x48U &&
        std::to_integer<unsigned>(bytes[i + 1]) == 0xffU &&
        std::to_integer<unsigned>(bytes[i + 2]) == 0xd0U) {
      return line620 + i;
    }
  }
  throw std::runtime_error(
      "REX.W source-next fixture does not contain exact 48 ff d0");
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

void require_breakpoint_installed(const mdbg::Debugger& debugger, std::size_t id) {
  for (const auto& breakpoint : debugger.breakpoints()) {
    if (breakpoint.id == id) {
      require(breakpoint.installed,
              "REX source next must reinsert the starting managed breakpoint");
      return;
    }
  }
  throw std::runtime_error("REX starting breakpoint disappeared during source next");
}

void test_rex_register_call_step_over(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line580 = source_address(lines, elf, debugger, 580);
  const auto line581 = source_address(lines, elf, debugger, 581);
  const auto call = rex_call_address(debugger, line580, line581);

  const auto start_id = debugger.add_breakpoint(static_cast<std::uintptr_t>(call));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == call,
          "REX source-next call breakpoint was not hit");
  require(lines.find_runtime_address(debugger.pid(), debugger.registers().rip, elf).has_value(),
          "REX call instruction has no source mapping");

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::LineChanged,
          "REX register-indirect call must step over its callee");
  require_source_line(result.source, 581);
  require(debugger.registers().rip == line581,
          "REX source next did not stop at caller line 581");
  require(marker_value(debugger, elf) == 511,
          "REX callee side effect must complete before next returns");
  require_breakpoint_installed(debugger, start_id);
  require(debugger.breakpoints().size() == 1,
          "temporary REX source-next breakpoint leaked into debugger state");

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "REX source-next fixture did not exit cleanly");
}

void test_rex_callee_breakpoint_interrupts_next(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line580 = source_address(lines, elf, debugger, 580);
  const auto line581 = source_address(lines, elf, debugger, 581);
  const auto line590 = source_address(lines, elf, debugger, 590);
  const auto call = rex_call_address(debugger, line580, line581);

  debugger.add_breakpoint(static_cast<std::uintptr_t>(call));
  debugger.add_breakpoint(static_cast<std::uintptr_t>(line590));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == call,
          "REX interruption call breakpoint was not hit");

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::Interrupted,
          "REX callee user breakpoint must interrupt source next");
  require(result.stop.reason == mdbg::StopReason::Breakpoint &&
              result.stop.breakpoint_address == line590,
          "source next hid the REX callee user breakpoint");
  require_source_line(result.source, 590);
  require(debugger.breakpoints().size() == 2,
          "temporary REX breakpoint leaked after interruption");
}

void test_rexwb_register_call_step_over(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line600 = source_address(lines, elf, debugger, 600);
  const auto line601 = source_address(lines, elf, debugger, 601);
  const auto call = rexwb_call_address(debugger, line600, line601);

  const auto start_id = debugger.add_breakpoint(static_cast<std::uintptr_t>(call));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == call,
          "REX.W+B source-next call breakpoint was not hit");
  require(lines.find_runtime_address(debugger.pid(), debugger.registers().rip, elf).has_value(),
          "REX.W+B call instruction has no source mapping");

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::LineChanged,
          "REX.W+B register-indirect call must step over its callee");
  require_source_line(result.source, 601);
  require(debugger.registers().rip == line601,
          "REX.W+B source next did not stop at caller line 601");
  require(marker_value(debugger, elf) == 2047,
          "REX.W+B callee side effect must complete before next returns");
  require_breakpoint_installed(debugger, start_id);
  require(debugger.breakpoints().size() == 1,
          "temporary REX.W+B source-next breakpoint leaked into debugger state");

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "REX.W+B source-next fixture did not exit cleanly");
}

void test_rexwb_callee_breakpoint_interrupts_next(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line600 = source_address(lines, elf, debugger, 600);
  const auto line601 = source_address(lines, elf, debugger, 601);
  const auto line610 = source_address(lines, elf, debugger, 610);
  const auto call = rexwb_call_address(debugger, line600, line601);

  debugger.add_breakpoint(static_cast<std::uintptr_t>(call));
  debugger.add_breakpoint(static_cast<std::uintptr_t>(line610));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == call,
          "REX.W+B interruption call breakpoint was not hit");

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::Interrupted,
          "REX.W+B callee user breakpoint must interrupt source next");
  require(result.stop.reason == mdbg::StopReason::Breakpoint &&
              result.stop.breakpoint_address == line610,
          "source next hid the REX.W+B callee user breakpoint");
  require_source_line(result.source, 610);
  require(debugger.breakpoints().size() == 2,
          "temporary REX.W+B breakpoint leaked after interruption");
}

void test_rexw_register_call_step_over(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line620 = source_address(lines, elf, debugger, 620);
  const auto line621 = source_address(lines, elf, debugger, 621);
  const auto call = rexw_call_address(debugger, line620, line621);

  const auto start_id = debugger.add_breakpoint(static_cast<std::uintptr_t>(call));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == call,
          "REX.W source-next call breakpoint was not hit");
  require(lines.find_runtime_address(debugger.pid(), debugger.registers().rip, elf).has_value(),
          "REX.W call instruction has no source mapping");

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::LineChanged,
          "REX.W register-indirect call must step over its callee");
  require_source_line(result.source, 621);
  require(debugger.registers().rip == line621,
          "REX.W source next did not stop at caller line 621");
  require(marker_value(debugger, elf) == 8191,
          "REX.W callee side effect must complete before next returns");
  require_breakpoint_installed(debugger, start_id);
  require(debugger.breakpoints().size() == 1,
          "temporary REX.W source-next breakpoint leaked into debugger state");

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "REX.W source-next fixture did not exit cleanly");
}

void test_rexw_callee_breakpoint_interrupts_next(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line620 = source_address(lines, elf, debugger, 620);
  const auto line621 = source_address(lines, elf, debugger, 621);
  const auto line630 = source_address(lines, elf, debugger, 630);
  const auto call = rexw_call_address(debugger, line620, line621);

  debugger.add_breakpoint(static_cast<std::uintptr_t>(call));
  debugger.add_breakpoint(static_cast<std::uintptr_t>(line630));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == call,
          "REX.W interruption call breakpoint was not hit");

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::Interrupted,
          "REX.W callee user breakpoint must interrupt source next");
  require(result.stop.reason == mdbg::StopReason::Breakpoint &&
              result.stop.breakpoint_address == line630,
          "source next hid the REX.W callee user breakpoint");
  require_source_line(result.source, 630);
  require(debugger.breakpoints().size() == 2,
          "temporary REX.W breakpoint leaked after interruption");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    test_rex_register_call_step_over(argv[1]);
    test_rex_callee_breakpoint_interrupts_next(argv[1]);
    test_rexwb_register_call_step_over(argv[1]);
    test_rexwb_callee_breakpoint_interrupts_next(argv[1]);
    test_rexw_register_call_step_over(argv[1]);
    test_rexw_callee_breakpoint_interrupts_next(argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "REX source-next integration failure: %s\n", error.what());
    return 1;
  }
}

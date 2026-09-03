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
  require(address.has_value(), "displacement fixture is missing line " +
                                   std::to_string(line));
  return *address;
}

void require_source_line(const std::optional<mdbg::SourceLocation>& source,
                         std::uint64_t line) {
  require(source.has_value(), "expected a source location");
  require(std::filesystem::path(source->file).filename() == "next_source.c",
          "source next returned an unexpected file: " + source->file);
  require(source->line == line,
          "source next returned line " + std::to_string(source->line) +
              " instead of " + std::to_string(line));
}

int marker_value(const mdbg::Debugger& debugger, const mdbg::ElfFile& elf,
                 const std::string& symbol) {
  const auto marker = elf.find_symbol(symbol);
  require(marker.has_value(), symbol + " symbol is missing");
  const auto address = elf.runtime_address(debugger.pid(), *marker);
  const auto bytes = debugger.read_memory(static_cast<std::uintptr_t>(address), sizeof(int));
  require(bytes.size() == sizeof(int), "failed to read " + symbol);
  int value = 0;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

std::uint64_t disp8_call_address(const mdbg::Debugger& debugger,
                                 std::uint64_t line580,
                                 std::uint64_t line581) {
  require(line581 > line580, "disp8 fixture rows are not in executable order");
  const auto span = line581 - line580;
  require(span <= 64, "disp8 fixture row unexpectedly grew beyond 64 bytes");
  const auto bytes = debugger.read_memory(static_cast<std::uintptr_t>(line580),
                                          static_cast<std::size_t>(span));
  for (std::size_t i = 0; i + 2 < bytes.size(); ++i) {
    if (std::to_integer<unsigned>(bytes[i]) == 0xffU &&
        std::to_integer<unsigned>(bytes[i + 1]) == 0x50U &&
        std::to_integer<unsigned>(bytes[i + 2]) == 0x08U) {
      return line580 + i;
    }
  }
  throw std::runtime_error(
      "disp8 source-next fixture does not contain unprefixed ff 50 08");
}

std::uint64_t disp32_call_address(const mdbg::Debugger& debugger,
                                  std::uint64_t line600,
                                  std::uint64_t line601) {
  require(line601 > line600, "disp32 fixture rows are not in executable order");
  const auto span = line601 - line600;
  require(span <= 64, "disp32 fixture row unexpectedly grew beyond 64 bytes");
  const auto bytes = debugger.read_memory(static_cast<std::uintptr_t>(line600),
                                          static_cast<std::size_t>(span));
  for (std::size_t i = 0; i + 5 < bytes.size(); ++i) {
    if (std::to_integer<unsigned>(bytes[i]) == 0xffU &&
        std::to_integer<unsigned>(bytes[i + 1]) == 0x90U &&
        std::to_integer<unsigned>(bytes[i + 2]) == 0x08U &&
        std::to_integer<unsigned>(bytes[i + 3]) == 0x00U &&
        std::to_integer<unsigned>(bytes[i + 4]) == 0x00U &&
        std::to_integer<unsigned>(bytes[i + 5]) == 0x00U) {
      return line600 + i;
    }
  }
  throw std::runtime_error(
      "disp32 source-next fixture does not contain unprefixed ff 90 08 00 00 00");
}

void require_breakpoint_installed(const mdbg::Debugger& debugger, std::size_t id) {
  for (const auto& breakpoint : debugger.breakpoints()) {
    if (breakpoint.id == id) {
      require(breakpoint.installed,
              "source next must reinsert the starting managed breakpoint");
      return;
    }
  }
  throw std::runtime_error("starting breakpoint disappeared during source next");
}

void test_disp8_call_step_over(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line580 = source_address(lines, elf, debugger, 580);
  const auto line581 = source_address(lines, elf, debugger, 581);
  const auto call = disp8_call_address(debugger, line580, line581);

  const auto start_id = debugger.add_breakpoint(static_cast<std::uintptr_t>(call));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == call,
          "disp8 source-next call breakpoint was not hit");
  require(lines.find_runtime_address(debugger.pid(), debugger.registers().rip, elf).has_value(),
          "disp8 call instruction has no source mapping");

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::LineChanged,
          "disp8 memory-indirect call must step over its callee");
  require_source_line(result.source, 581);
  require(debugger.registers().rip == line581,
          "disp8 source next did not stop at line 581");
  require(marker_value(debugger, elf, "disp8_marker") == 1,
          "disp8 callee side effect must complete before next returns");
  require_breakpoint_installed(debugger, start_id);
  require(debugger.breakpoints().size() == 1,
          "temporary disp8 source-next breakpoint leaked into debugger state");

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "displacement source-next fixture did not exit cleanly after disp8 next");
}

void test_disp8_callee_breakpoint_interrupts_next(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line580 = source_address(lines, elf, debugger, 580);
  const auto line581 = source_address(lines, elf, debugger, 581);
  const auto line590 = source_address(lines, elf, debugger, 590);
  const auto call = disp8_call_address(debugger, line580, line581);

  debugger.add_breakpoint(static_cast<std::uintptr_t>(call));
  debugger.add_breakpoint(static_cast<std::uintptr_t>(line590));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == call,
          "disp8 interruption call breakpoint was not hit");

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::Interrupted,
          "disp8 callee breakpoint must interrupt source next");
  require(result.stop.reason == mdbg::StopReason::Breakpoint &&
              result.stop.breakpoint_address == line590,
          "source next hid the disp8 callee breakpoint");
  require_source_line(result.source, 590);
  require(debugger.breakpoints().size() == 2,
          "temporary disp8 source-next breakpoint leaked after interruption");
}

void test_disp32_call_step_over(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line600 = source_address(lines, elf, debugger, 600);
  const auto line601 = source_address(lines, elf, debugger, 601);
  const auto call = disp32_call_address(debugger, line600, line601);

  const auto start_id = debugger.add_breakpoint(static_cast<std::uintptr_t>(call));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == call,
          "disp32 source-next call breakpoint was not hit");
  require(lines.find_runtime_address(debugger.pid(), debugger.registers().rip, elf).has_value(),
          "disp32 call instruction has no source mapping");

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::LineChanged,
          "disp32 memory-indirect call must step over its callee");
  require_source_line(result.source, 601);
  require(debugger.registers().rip == line601,
          "disp32 source next did not stop at line 601");
  require(marker_value(debugger, elf, "disp32_marker") == 1,
          "disp32 callee side effect must complete before next returns");
  require_breakpoint_installed(debugger, start_id);
  require(debugger.breakpoints().size() == 1,
          "temporary disp32 source-next breakpoint leaked into debugger state");

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "displacement source-next fixture did not exit cleanly after disp32 next");
}

void test_disp32_callee_breakpoint_interrupts_next(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line600 = source_address(lines, elf, debugger, 600);
  const auto line601 = source_address(lines, elf, debugger, 601);
  const auto line610 = source_address(lines, elf, debugger, 610);
  const auto call = disp32_call_address(debugger, line600, line601);

  debugger.add_breakpoint(static_cast<std::uintptr_t>(call));
  debugger.add_breakpoint(static_cast<std::uintptr_t>(line610));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == call,
          "disp32 interruption call breakpoint was not hit");

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::Interrupted,
          "disp32 callee breakpoint must interrupt source next");
  require(result.stop.reason == mdbg::StopReason::Breakpoint &&
              result.stop.breakpoint_address == line610,
          "source next hid the disp32 callee breakpoint");
  require_source_line(result.source, 610);
  require(debugger.breakpoints().size() == 2,
          "temporary disp32 source-next breakpoint leaked after interruption");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    test_disp8_call_step_over(argv[1]);
    test_disp8_callee_breakpoint_interrupts_next(argv[1]);
    test_disp32_call_step_over(argv[1]);
    test_disp32_callee_breakpoint_interrupts_next(argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "displacement source-next integration failure: %s\n",
                 error.what());
    return 1;
  }
}

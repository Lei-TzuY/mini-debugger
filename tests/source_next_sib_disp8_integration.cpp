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
  require(address.has_value(), "SIB disp8 fixture is missing line " +
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

int marker_value(const mdbg::Debugger& debugger, const mdbg::ElfFile& elf) {
  const auto marker = elf.find_symbol("sib_disp8_marker");
  require(marker.has_value(), "sib_disp8_marker symbol is missing");
  const auto address = elf.runtime_address(debugger.pid(), *marker);
  const auto bytes = debugger.read_memory(static_cast<std::uintptr_t>(address), sizeof(int));
  require(bytes.size() == sizeof(int), "failed to read sib_disp8_marker");
  int value = 0;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

std::uint64_t sib_disp8_call_address(const mdbg::Debugger& debugger,
                                     std::uint64_t line640,
                                     std::uint64_t line641) {
  require(line641 > line640, "SIB disp8 fixture rows are not in executable order");
  const auto span = line641 - line640;
  require(span <= 64, "SIB disp8 fixture row unexpectedly grew beyond 64 bytes");
  const auto bytes = debugger.read_memory(static_cast<std::uintptr_t>(line640),
                                          static_cast<std::size_t>(span));
  for (std::size_t i = 0; i + 3 < bytes.size(); ++i) {
    if (std::to_integer<unsigned>(bytes[i]) == 0xffU &&
        std::to_integer<unsigned>(bytes[i + 1]) == 0x54U &&
        std::to_integer<unsigned>(bytes[i + 2]) == 0x20U &&
        std::to_integer<unsigned>(bytes[i + 3]) == 0x08U) {
      return line640 + i;
    }
  }
  throw std::runtime_error(
      "SIB disp8 source-next fixture does not contain unprefixed ff 54 20 08");
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

void test_sib_disp8_call_step_over(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line640 = source_address(lines, elf, debugger, 640);
  const auto line641 = source_address(lines, elf, debugger, 641);
  const auto call = sib_disp8_call_address(debugger, line640, line641);

  const auto start_id = debugger.add_breakpoint(static_cast<std::uintptr_t>(call));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == call,
          "SIB disp8 source-next call breakpoint was not hit");
  require(lines.find_runtime_address(debugger.pid(), debugger.registers().rip, elf).has_value(),
          "SIB disp8 call instruction has no source mapping");

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::LineChanged,
          "SIB disp8 memory-indirect call must step over its callee");
  require_source_line(result.source, 641);
  require(debugger.registers().rip == line641,
          "SIB disp8 source next did not stop at line 641");
  require(marker_value(debugger, elf) == 1,
          "SIB disp8 callee side effect must complete before next returns");
  require_breakpoint_installed(debugger, start_id);
  require(debugger.breakpoints().size() == 1,
          "temporary SIB disp8 source-next breakpoint leaked into debugger state");

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "SIB disp8 source-next fixture did not exit cleanly after next");
}

void test_sib_disp8_callee_breakpoint_interrupts_next(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line640 = source_address(lines, elf, debugger, 640);
  const auto line641 = source_address(lines, elf, debugger, 641);
  const auto line650 = source_address(lines, elf, debugger, 650);
  const auto call = sib_disp8_call_address(debugger, line640, line641);

  debugger.add_breakpoint(static_cast<std::uintptr_t>(call));
  debugger.add_breakpoint(static_cast<std::uintptr_t>(line650));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == call,
          "SIB disp8 interruption call breakpoint was not hit");

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::Interrupted,
          "SIB disp8 callee breakpoint must interrupt source next");
  require(result.stop.reason == mdbg::StopReason::Breakpoint &&
              result.stop.breakpoint_address == line650,
          "source next hid the SIB disp8 callee breakpoint");
  require_source_line(result.source, 650);
  require(debugger.breakpoints().size() == 2,
          "temporary SIB disp8 source-next breakpoint leaked after interruption");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    test_sib_disp8_call_step_over(argv[1]);
    test_sib_disp8_callee_breakpoint_interrupts_next(argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "SIB disp8 source-next integration failure: %s\n", error.what());
    return 1;
  }
}

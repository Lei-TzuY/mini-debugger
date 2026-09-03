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

void require_source_line(const std::optional<mdbg::SourceLocation>& source,
                         std::uint64_t line) {
  require(source.has_value(), "expected a source location");
  require(std::filesystem::path(source->file).filename() == "next_source.c",
          "source next returned an unexpected source file: " + source->file);
  require(source->line == line,
          "source next returned line " + std::to_string(source->line) +
              " instead of " + std::to_string(line));
}

std::uint64_t source_address(const mdbg::DwarfLineTable& lines, const mdbg::ElfFile& elf,
                             const mdbg::Debugger& debugger, std::uint64_t line) {
  const auto address =
      lines.find_runtime_source(debugger.pid(), "next_source.c", line, elf);
  require(address.has_value(), "source-next fixture is missing line " + std::to_string(line));
  return *address;
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

void require_breakpoint_count(const mdbg::Debugger& debugger, std::size_t expected) {
  require(debugger.breakpoints().size() == expected,
          "temporary source-next breakpoint leaked into debugger state");
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

void require_register_indirect_call_encoding(const mdbg::Debugger& debugger,
                                             std::uint64_t line520,
                                             std::uint64_t line521) {
  require(line521 > line520,
          "register-indirect fixture rows are not in executable order");
  const auto span = line521 - line520;
  require(span <= 64, "register-indirect fixture row unexpectedly grew beyond 64 bytes");
  const auto bytes = debugger.read_memory(static_cast<std::uintptr_t>(line520),
                                          static_cast<std::size_t>(span));
  bool found = false;
  for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
    if (std::to_integer<unsigned>(bytes[i]) == 0xffU &&
        std::to_integer<unsigned>(bytes[i + 1]) == 0xd0U) {
      found = true;
      break;
    }
  }
  require(found,
          "register-indirect source-next fixture does not contain unprefixed ff d0");
}

void test_direct_call_step_over(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line500 = source_address(lines, elf, debugger, 500);
  const auto line501 = source_address(lines, elf, debugger, 501);

  const auto start_id = debugger.add_breakpoint(static_cast<std::uintptr_t>(line500));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == line500,
          "source-next start breakpoint was not hit");
  require_source_line(lines.find_runtime_address(debugger.pid(), debugger.registers().rip, elf),
                      500);

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::LineChanged,
          "source next did not reach the caller's following source line");
  require_source_line(result.source, 501);
  require(debugger.registers().rip == line501,
          "source next did not stop at the first executable row for line 501");
  require(marker_value(debugger, elf) == 1,
          "callee side effect must complete before source next returns to the caller");
  require_breakpoint_installed(debugger, start_id);
  require_breakpoint_count(debugger, 1);

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "source-next fixture did not exit cleanly");
}

void test_callee_breakpoint_interrupts_next(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line500 = source_address(lines, elf, debugger, 500);
  const auto line510 = source_address(lines, elf, debugger, 510);

  debugger.add_breakpoint(static_cast<std::uintptr_t>(line500));
  debugger.add_breakpoint(static_cast<std::uintptr_t>(line510));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == line500,
          "source-next interruption start breakpoint was not hit");

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::Interrupted,
          "callee user breakpoint must interrupt source next");
  require(result.stop.reason == mdbg::StopReason::Breakpoint &&
              result.stop.breakpoint_address == line510,
          "source next hid or misclassified the callee user breakpoint");
  require_source_line(result.source, 510);
  require_breakpoint_count(debugger, 2);
}

void test_register_indirect_call_step_over(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line520 = source_address(lines, elf, debugger, 520);
  const auto line521 = source_address(lines, elf, debugger, 521);

  const auto start_id = debugger.add_breakpoint(static_cast<std::uintptr_t>(line520));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == line520,
          "register-indirect source-next start breakpoint was not hit");
  require_source_line(lines.find_runtime_address(debugger.pid(), debugger.registers().rip, elf),
                      520);
  require_register_indirect_call_encoding(debugger, line520, line521);

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::LineChanged,
          "register-indirect call must step over its callee");
  require_source_line(result.source, 521);
  require(debugger.registers().rip == line521,
          "register-indirect source next did not stop at line 521");
  require(marker_value(debugger, elf) == 7,
          "register-indirect callee side effect must complete before next returns");
  require_breakpoint_installed(debugger, start_id);
  require_breakpoint_count(debugger, 1);

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "register-indirect source-next fixture did not exit cleanly");
}

void test_register_indirect_callee_breakpoint_interrupts_next(
    const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line520 = source_address(lines, elf, debugger, 520);
  const auto line521 = source_address(lines, elf, debugger, 521);
  const auto line530 = source_address(lines, elf, debugger, 530);

  debugger.add_breakpoint(static_cast<std::uintptr_t>(line520));
  debugger.add_breakpoint(static_cast<std::uintptr_t>(line530));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == line520,
          "register-indirect interruption start breakpoint was not hit");
  require_register_indirect_call_encoding(debugger, line520, line521);

  const auto result = mdbg::next_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::Interrupted,
          "register-indirect callee breakpoint must interrupt source next");
  require(result.stop.reason == mdbg::StopReason::Breakpoint &&
              result.stop.breakpoint_address == line530,
          "source next hid the register-indirect callee breakpoint");
  require_source_line(result.source, 530);
  require_breakpoint_count(debugger, 2);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    test_direct_call_step_over(argv[1]);
    test_callee_breakpoint_interrupts_next(argv[1]);
    test_register_indirect_call_step_over(argv[1]);
    test_register_indirect_callee_breakpoint_interrupts_next(argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "source-next integration failure: %s\n", error.what());
    return 1;
  }
}

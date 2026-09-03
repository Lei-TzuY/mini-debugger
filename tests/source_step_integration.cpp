#include "debugger/debugger.hpp"
#include "dwarf/line_table.hpp"
#include "elf/elf.hpp"
#include "source/source_step.hpp"

#include <cstdint>
#include <cstdio>
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
  require(std::filesystem::path(source->file).filename() == "mapped_source.c",
          "source step returned an unexpected source file: " + source->file);
  require(source->line == line,
          "source step returned line " + std::to_string(source->line) +
              " instead of " + std::to_string(line));
}

void require_breakpoint_installed(const mdbg::Debugger& debugger, std::size_t id) {
  for (const auto& breakpoint : debugger.breakpoints()) {
    if (breakpoint.id == id) {
      require(breakpoint.installed,
              "source step must reinsert the breakpoint after displaced instruction step");
      return;
    }
  }
  throw std::runtime_error("source breakpoint disappeared during source step");
}

void test_line_change(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);

  const auto line400 = lines.find_runtime_source(debugger.pid(), "mapped_source.c", 400, elf);
  const auto line401 = lines.find_runtime_source(debugger.pid(), "mapped_source.c", 401, elf);
  require(line400.has_value() && line401.has_value(),
          "source-step fixture must expose mapped_source.c:400 and :401");
  require(*line401 > *line400, "line 401 must follow line 400 in the fixture");

  const auto breakpoint_id =
      debugger.add_breakpoint(static_cast<std::uintptr_t>(*line400));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == *line400,
          "source-step start breakpoint was not hit");
  require_source_line(lines.find_runtime_address(debugger.pid(), debugger.registers().rip, elf),
                      400);

  const auto result = mdbg::step_source(debugger, lines, elf, 64);
  require(result.reason == mdbg::SourceStepStopReason::LineChanged,
          "source step did not stop when file:line changed");
  require(result.stop.reason == mdbg::StopReason::SingleStep,
          "source line transition must be exposed as a single-step stop");
  require(result.instructions > 0 && result.instructions <= 64,
          "source step instruction count is outside its bound");
  require_source_line(result.source, 401);
  require(debugger.registers().rip == *line401,
          "source step did not stop at the first executable row for line 401");
  require_breakpoint_installed(debugger, breakpoint_id);

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "fixture did not exit cleanly after source step");
}

void test_instruction_limit(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const auto line401 = lines.find_runtime_source(debugger.pid(), "mapped_source.c", 401, elf);
  require(line401.has_value(), "source-step fixture must expose mapped_source.c:401");

  const auto breakpoint_id =
      debugger.add_breakpoint(static_cast<std::uintptr_t>(*line401));
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == *line401,
          "line 401 breakpoint was not hit");

  const auto result = mdbg::step_source(debugger, lines, elf, 1);
  require(result.reason == mdbg::SourceStepStopReason::InstructionLimit,
          "one instruction inside a multi-instruction source row must hit the source-step limit");
  require(result.instructions == 1 && result.stop.reason == mdbg::StopReason::SingleStep,
          "instruction-limit result must report exactly one single-step");
  require_source_line(result.source, 401);
  require(debugger.state() == mdbg::ProcessState::Stopped,
          "instruction-limit source step must leave the tracee stopped");
  require_breakpoint_installed(debugger, breakpoint_id);
}

void test_unmapped_start(const std::string& stripped_fixture) {
  auto debugger = mdbg::Debugger::launch(stripped_fixture, {});
  const mdbg::ElfFile elf(stripped_fixture);
  const mdbg::DwarfLineTable lines(stripped_fixture);
  require(!lines.available(), "stripped fixture unexpectedly exposes source lines");

  const auto before = debugger.registers().rip;
  try {
    static_cast<void>(mdbg::step_source(debugger, lines, elf, 64));
  } catch (const std::invalid_argument& error) {
    require(std::string(error.what()).find("no source location") != std::string::npos,
            "unmapped source-step rejection should explain the missing source location");
    require(debugger.state() == mdbg::ProcessState::Stopped,
            "rejected source step must keep the tracee stopped");
    require(debugger.registers().rip == before,
            "rejected source step must not advance RIP");
    return;
  }
  throw std::runtime_error("source step from an unmapped RIP must be rejected");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    test_line_change(argv[1]);
    test_instruction_limit(argv[1]);
    test_unmapped_start(argv[2]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "source-step integration failure: %s\n", error.what());
    return 1;
  }
}

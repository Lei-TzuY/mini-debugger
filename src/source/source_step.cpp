#include "source/source_step.hpp"

#include <stdexcept>

namespace mdbg {
namespace {

bool same_source_line(const SourceLocation& left, const SourceLocation& right) {
  return left.file == right.file && left.line == right.line;
}

std::optional<SourceLocation> current_source(const Debugger& debugger,
                                             const DwarfLineTable& lines,
                                             const ElfFile& elf) {
  if (debugger.state() != ProcessState::Stopped) return std::nullopt;
  const auto rip = debugger.registers().rip;
  return lines.find_runtime_address(debugger.pid(), rip, elf);
}

}  // namespace

SourceStepResult step_source(Debugger& debugger, const DwarfLineTable& lines,
                             const ElfFile& elf, std::size_t instruction_limit) {
  if (instruction_limit == 0) {
    throw std::invalid_argument("source step instruction limit must be positive");
  }
  if (debugger.state() != ProcessState::Stopped) {
    throw std::logic_error("source step requires a stopped tracee");
  }

  const auto start = current_source(debugger, lines, elf);
  if (!start) {
    throw std::invalid_argument("current instruction has no source location");
  }

  for (std::size_t instructions = 1; instructions <= instruction_limit; ++instructions) {
    const auto stop = debugger.single_step();
    const auto source = current_source(debugger, lines, elf);

    if (stop.reason != StopReason::SingleStep) {
      return {SourceStepStopReason::Interrupted, stop, source, instructions};
    }
    if (source && !same_source_line(*source, *start)) {
      return {SourceStepStopReason::LineChanged, stop, source, instructions};
    }
  }

  return {SourceStepStopReason::InstructionLimit, debugger.stop_info(),
          current_source(debugger, lines, elf), instruction_limit};
}

}  // namespace mdbg

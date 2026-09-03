#include "source/source_step.hpp"

#include <limits>
#include <stdexcept>
#include <string>

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

std::optional<std::uintptr_t> direct_near_call_return_address(const Debugger& debugger) {
  const auto rip = static_cast<std::uintptr_t>(debugger.registers().rip);
  const auto opcode = debugger.read_memory(rip, 1);
  if (opcode.empty() || std::to_integer<unsigned>(opcode.front()) != 0xe8U) {
    return std::nullopt;
  }
  if (rip > std::numeric_limits<std::uintptr_t>::max() - 5) {
    throw std::runtime_error("direct call return address overflows address space");
  }
  return rip + 5;
}

bool has_breakpoint_at(const Debugger& debugger, std::uintptr_t address) {
  for (const auto& breakpoint : debugger.breakpoints()) {
    if (breakpoint.address == address) return true;
  }
  return false;
}

void remove_temporary_breakpoint(Debugger& debugger, std::optional<std::size_t>& id) {
  if (!id) return;
  if (!debugger.remove_breakpoint(*id)) {
    throw std::logic_error("temporary next breakpoint disappeared");
  }
  id.reset();
}

void validate_source_motion(const Debugger& debugger, std::size_t instruction_limit,
                            const char* operation) {
  if (instruction_limit == 0) {
    throw std::invalid_argument(std::string(operation) +
                                " instruction limit must be positive");
  }
  if (debugger.state() != ProcessState::Stopped) {
    throw std::logic_error(std::string(operation) + " requires a stopped tracee");
  }
}

}  // namespace

SourceStepResult step_source(Debugger& debugger, const DwarfLineTable& lines,
                             const ElfFile& elf, std::size_t instruction_limit) {
  validate_source_motion(debugger, instruction_limit, "source step");

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

SourceStepResult next_source(Debugger& debugger, const DwarfLineTable& lines,
                             const ElfFile& elf, std::size_t instruction_limit) {
  validate_source_motion(debugger, instruction_limit, "source next");

  const auto start = current_source(debugger, lines, elf);
  if (!start) {
    throw std::invalid_argument("current instruction has no source location");
  }

  for (std::size_t instructions = 1; instructions <= instruction_limit; ++instructions) {
    if (const auto return_address = direct_near_call_return_address(debugger)) {
      const bool user_breakpoint = has_breakpoint_at(debugger, *return_address);
      std::optional<std::size_t> temporary_breakpoint;
      if (!user_breakpoint) {
        temporary_breakpoint = debugger.add_breakpoint(*return_address);
      }

      StopInfo stop;
      try {
        stop = debugger.continue_execution();
      } catch (...) {
        if (temporary_breakpoint && debugger.state() != ProcessState::Running) {
          try {
            remove_temporary_breakpoint(debugger, temporary_breakpoint);
          } catch (...) {
          }
        }
        throw;
      }

      const bool returned = stop.reason == StopReason::Breakpoint &&
                            stop.breakpoint_address == return_address;
      if (temporary_breakpoint) {
        remove_temporary_breakpoint(debugger, temporary_breakpoint);
      }
      const auto source = current_source(debugger, lines, elf);

      if (!returned || user_breakpoint) {
        return {SourceStepStopReason::Interrupted, stop, source, instructions};
      }
      if (source && !same_source_line(*source, *start)) {
        return {SourceStepStopReason::LineChanged, stop, source, instructions};
      }
      continue;
    }

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

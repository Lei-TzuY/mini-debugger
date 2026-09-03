#include "source/source_step.hpp"

#include "unwind/cfi.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace mdbg {
namespace {

struct ModuleSourcePosition {
  std::string module_path;
  SourceLocation source;
};

bool same_source_line(const SourceLocation& left, const SourceLocation& right) {
  return left.file == right.file && left.line == right.line;
}

bool same_source_line(const ModuleSourcePosition& left,
                      const ModuleSourcePosition& right) {
  return left.module_path == right.module_path &&
         same_source_line(left.source, right.source);
}

std::optional<ModuleSourcePosition> current_module_source(
    const Debugger& debugger, const ElfFile& elf) {
  if (debugger.state() != ProcessState::Stopped) return std::nullopt;
  const auto rip = static_cast<std::uintptr_t>(debugger.registers().rip);
  const auto resolved =
      find_module_source_by_runtime_address(debugger.pid(), rip, elf);
  if (!resolved) return std::nullopt;
  return ModuleSourcePosition{
      resolved->module_path,
      SourceLocation{resolved->file, resolved->line, resolved->column}};
}

std::optional<SourceLocation> source_only(
    const std::optional<ModuleSourcePosition>& position) {
  if (!position) return std::nullopt;
  return position->source;
}

std::optional<std::uintptr_t> supported_call_return_address(
    const Debugger& debugger) {
  const auto rip = static_cast<std::uintptr_t>(debugger.registers().rip);
  const auto opcode = debugger.read_memory(rip, 1);
  if (opcode.empty()) return std::nullopt;

  const auto first = std::to_integer<unsigned>(opcode.front());
  std::size_t instruction_length = 0;
  if (first == 0xe8U) {
    instruction_length = 5;
  } else if (first == 0xffU) {
    if (rip == std::numeric_limits<std::uintptr_t>::max()) {
      return std::nullopt;
    }
    const auto modrm_bytes = debugger.read_memory(rip + 1, 1);
    if (modrm_bytes.empty()) return std::nullopt;
    const auto modrm = std::to_integer<unsigned>(modrm_bytes.front());
    const auto mod = (modrm >> 6U) & 0x3U;
    const auto reg = (modrm >> 3U) & 0x7U;
    const auto rm = modrm & 0x7U;
    if (reg == 2U) {
      if (mod == 3U) {
        instruction_length = 2;
      } else if (mod == 0U && rm == 5U) {
        instruction_length = 6;
      }
    }
  }

  if (instruction_length == 0) return std::nullopt;
  if (rip > std::numeric_limits<std::uintptr_t>::max() - instruction_length) {
    throw std::runtime_error("call return address overflows address space");
  }
  return rip + instruction_length;
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

SourceStepResult step_source(Debugger& debugger, const ElfFile& elf,
                             std::size_t instruction_limit) {
  validate_source_motion(debugger, instruction_limit, "source step");

  const auto start = current_module_source(debugger, elf);
  if (!start) {
    throw std::invalid_argument("current instruction has no source location");
  }

  for (std::size_t instructions = 1; instructions <= instruction_limit; ++instructions) {
    const auto stop = debugger.single_step();
    const auto source = current_module_source(debugger, elf);

    if (stop.reason != StopReason::SingleStep) {
      return {SourceStepStopReason::Interrupted, stop, source_only(source), instructions};
    }
    if (source && !same_source_line(*source, *start)) {
      return {SourceStepStopReason::LineChanged, stop, source_only(source), instructions};
    }
  }

  const auto source = current_module_source(debugger, elf);
  return {SourceStepStopReason::InstructionLimit, debugger.stop_info(),
          source_only(source), instruction_limit};
}

SourceStepResult step_source(Debugger& debugger, const DwarfLineTable& lines,
                             const ElfFile& elf, std::size_t instruction_limit) {
  static_cast<void>(lines);
  return step_source(debugger, elf, instruction_limit);
}

SourceStepResult next_source(Debugger& debugger, const ElfFile& elf,
                             std::size_t instruction_limit) {
  validate_source_motion(debugger, instruction_limit, "source next");

  const auto start = current_module_source(debugger, elf);
  if (!start) {
    throw std::invalid_argument("current instruction has no source location");
  }

  for (std::size_t instructions = 1; instructions <= instruction_limit; ++instructions) {
    if (const auto return_address = supported_call_return_address(debugger)) {
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
      const auto source = current_module_source(debugger, elf);

      if (!returned || user_breakpoint) {
        return {SourceStepStopReason::Interrupted, stop, source_only(source), instructions};
      }
      if (source && !same_source_line(*source, *start)) {
        return {SourceStepStopReason::LineChanged, stop, source_only(source), instructions};
      }
      continue;
    }

    const auto stop = debugger.single_step();
    const auto source = current_module_source(debugger, elf);
    if (stop.reason != StopReason::SingleStep) {
      return {SourceStepStopReason::Interrupted, stop, source_only(source), instructions};
    }
    if (source && !same_source_line(*source, *start)) {
      return {SourceStepStopReason::LineChanged, stop, source_only(source), instructions};
    }
  }

  const auto source = current_module_source(debugger, elf);
  return {SourceStepStopReason::InstructionLimit, debugger.stop_info(),
          source_only(source), instruction_limit};
}

SourceStepResult next_source(Debugger& debugger, const DwarfLineTable& lines,
                             const ElfFile& elf, std::size_t instruction_limit) {
  static_cast<void>(lines);
  return next_source(debugger, elf, instruction_limit);
}

}  // namespace mdbg

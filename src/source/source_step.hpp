#pragma once

#include "debugger/debugger.hpp"
#include "dwarf/line_table.hpp"
#include "elf/elf.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace mdbg {

enum class SourceStepStopReason { LineChanged, Interrupted, InstructionLimit };

struct SourceStepResult {
  SourceStepStopReason reason;
  StopInfo stop;
  std::optional<SourceLocation> source;
  std::size_t instructions;
  std::optional<std::string> source_module_path;
};

SourceStepResult step_source(Debugger& debugger, const ElfFile& elf,
                             std::size_t instruction_limit = 4096);
SourceStepResult step_source(Debugger& debugger, const DwarfLineTable& lines,
                             const ElfFile& elf, std::size_t instruction_limit = 4096);
SourceStepResult next_source(Debugger& debugger, const ElfFile& elf,
                             std::size_t instruction_limit = 4096);
SourceStepResult next_source(Debugger& debugger, const DwarfLineTable& lines,
                             const ElfFile& elf, std::size_t instruction_limit = 4096);

}  // namespace mdbg

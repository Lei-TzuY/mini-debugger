#pragma once

#include "debugger/debugger.hpp"
#include "dwarf/line_table.hpp"
#include "elf/elf.hpp"

#include <cstddef>
#include <optional>

namespace mdbg {

enum class SourceStepStopReason { LineChanged, Interrupted, InstructionLimit };

struct SourceStepResult {
  SourceStepStopReason reason;
  StopInfo stop;
  std::optional<SourceLocation> source;
  std::size_t instructions;
};

SourceStepResult step_source(Debugger& debugger, const DwarfLineTable& lines,
                             const ElfFile& elf, std::size_t instruction_limit = 4096);

}  // namespace mdbg

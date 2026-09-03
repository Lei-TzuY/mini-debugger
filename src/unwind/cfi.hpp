#pragma once

#include "dwarf/eh_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mdbg {

class Debugger;
class ElfFile;

enum class CfiUnwindStopReason {
  EndOfChain,
  FrameLimit,
  NoFrameInfo,
  InvalidFrameState,
};

struct CfiStackFrame {
  std::uintptr_t instruction_pointer;
  std::uintptr_t stack_pointer;
  std::optional<std::uintptr_t> frame_pointer;
};

struct CfiBacktrace {
  std::vector<CfiStackFrame> frames;
  CfiUnwindStopReason stop_reason;
};

std::optional<std::uintptr_t> module_caller_return_address(
    const Debugger& debugger, const ElfFile& preferred_elf, const EhFrame& preferred_cfi);
CfiBacktrace unwind_eh_frame(const Debugger& debugger, const ElfFile& elf,
                             const EhFrame& cfi, std::size_t max_frames = 64);
const char* cfi_unwind_stop_reason_name(CfiUnwindStopReason reason) noexcept;

}  // namespace mdbg

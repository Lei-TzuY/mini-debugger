#include "unwind/cfi.hpp"

#include "debugger/debugger.hpp"
#include "elf/elf.hpp"

#include <stdexcept>

namespace mdbg {

CfiBacktrace unwind_eh_frame(const Debugger& debugger, const ElfFile& elf,
                             const EhFrame& cfi, std::size_t max_frames) {
  if (max_frames == 0) {
    throw std::invalid_argument("CFI unwind requires a non-zero frame limit");
  }
  if (debugger.state() != ProcessState::Stopped) {
    throw std::logic_error("CFI unwind requires a stopped tracee");
  }

  const auto regs = debugger.registers();
  EhFrameCursor current{static_cast<std::uintptr_t>(regs.rip),
                        static_cast<std::uintptr_t>(regs.rsp),
                        static_cast<std::uintptr_t>(regs.rbp)};
  CfiBacktrace result{{CfiStackFrame{current.instruction_pointer, current.stack_pointer,
                                    current.frame_pointer}},
                      CfiUnwindStopReason::EndOfChain};
  if (max_frames == 1) {
    result.stop_reason = CfiUnwindStopReason::FrameLimit;
    return result;
  }

  while (result.frames.size() < max_frames) {
    std::optional<EhFrameCursor> caller;
    try {
      caller = cfi.caller_frame(debugger, elf, current);
    } catch (const std::exception&) {
      result.stop_reason = CfiUnwindStopReason::InvalidFrameState;
      return result;
    }

    if (!caller) {
      result.stop_reason = result.frames.size() == 1 ? CfiUnwindStopReason::NoFrameInfo
                                                     : CfiUnwindStopReason::EndOfChain;
      return result;
    }
    if (caller->instruction_pointer == current.instruction_pointer ||
        caller->stack_pointer <= current.stack_pointer) {
      result.stop_reason = CfiUnwindStopReason::InvalidFrameState;
      return result;
    }

    result.frames.push_back(CfiStackFrame{caller->instruction_pointer, caller->stack_pointer,
                                          caller->frame_pointer});
    current = *caller;
  }

  result.stop_reason = CfiUnwindStopReason::FrameLimit;
  return result;
}

const char* cfi_unwind_stop_reason_name(CfiUnwindStopReason reason) noexcept {
  switch (reason) {
    case CfiUnwindStopReason::EndOfChain:
      return "end of CFI chain";
    case CfiUnwindStopReason::FrameLimit:
      return "frame limit reached";
    case CfiUnwindStopReason::NoFrameInfo:
      return "no CFI frame information";
    case CfiUnwindStopReason::InvalidFrameState:
      return "invalid or unsupported CFI frame state";
  }
  return "unknown CFI unwind stop";
}

}  // namespace mdbg

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mdbg {

class Debugger;

enum class UnwindStopReason {
  EndOfChain,
  FrameLimit,
  InvalidFramePointer,
  MemoryReadError,
};

struct StackFrame {
  std::uintptr_t instruction_pointer;
  std::uintptr_t frame_pointer;
};

struct Backtrace {
  std::vector<StackFrame> frames;
  UnwindStopReason stop_reason;
};

Backtrace unwind_frame_pointers(const Debugger& debugger, std::size_t max_frames = 64,
                                std::uintptr_t max_frame_span = 16U * 1024U * 1024U);
const char* unwind_stop_reason_name(UnwindStopReason reason) noexcept;

}  // namespace mdbg

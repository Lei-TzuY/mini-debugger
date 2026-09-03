#include "unwind/frame_pointer.hpp"

#include "debugger/debugger.hpp"
#include "ptrace/ptrace.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace mdbg {
namespace {

std::uint64_t read_u64(const std::vector<std::byte>& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

}  // namespace

Backtrace unwind_frame_pointers(const Debugger& debugger, std::size_t max_frames,
                                std::uintptr_t max_frame_span) {
  if (max_frames == 0) {
    throw std::invalid_argument("frame-pointer unwind requires a non-zero frame limit");
  }
  if (max_frame_span == 0) {
    throw std::invalid_argument("frame-pointer unwind requires a non-zero frame span");
  }

  const auto regs = debugger.registers();
  auto frame_pointer = static_cast<std::uintptr_t>(regs.rbp);
  Backtrace result{{StackFrame{static_cast<std::uintptr_t>(regs.rip), frame_pointer}},
                   UnwindStopReason::EndOfChain};

  if (max_frames == 1) {
    result.stop_reason = UnwindStopReason::FrameLimit;
    return result;
  }

  while (frame_pointer != 0) {
    if ((frame_pointer % alignof(std::uint64_t)) != 0) {
      result.stop_reason = UnwindStopReason::InvalidFramePointer;
      return result;
    }

    std::vector<std::byte> frame_data;
    try {
      frame_data = debugger.read_memory(frame_pointer, 2 * sizeof(std::uint64_t));
    } catch (const lowlevel::PtraceError&) {
      result.stop_reason = UnwindStopReason::MemoryReadError;
      return result;
    }

    const auto next_frame_pointer = static_cast<std::uintptr_t>(read_u64(frame_data, 0));
    const auto return_address =
        static_cast<std::uintptr_t>(read_u64(frame_data, sizeof(std::uint64_t)));
    if (return_address == 0) {
      result.stop_reason = UnwindStopReason::EndOfChain;
      return result;
    }

    if (next_frame_pointer == 0) {
      result.frames.push_back(StackFrame{return_address, 0});
      result.stop_reason = UnwindStopReason::EndOfChain;
      return result;
    }
    if (next_frame_pointer <= frame_pointer ||
        next_frame_pointer - frame_pointer > max_frame_span) {
      result.stop_reason = UnwindStopReason::InvalidFramePointer;
      return result;
    }

    result.frames.push_back(StackFrame{return_address, next_frame_pointer});
    if (result.frames.size() >= max_frames) {
      result.stop_reason = UnwindStopReason::FrameLimit;
      return result;
    }
    frame_pointer = next_frame_pointer;
  }

  return result;
}

const char* unwind_stop_reason_name(UnwindStopReason reason) noexcept {
  switch (reason) {
    case UnwindStopReason::EndOfChain:
      return "end of frame chain";
    case UnwindStopReason::FrameLimit:
      return "frame limit reached";
    case UnwindStopReason::InvalidFramePointer:
      return "invalid frame pointer";
    case UnwindStopReason::MemoryReadError:
      return "frame memory is unreadable";
  }
  return "unknown unwind stop";
}

}  // namespace mdbg

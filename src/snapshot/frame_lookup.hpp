#pragma once

#include "snapshot/inspection.hpp"

#include <cstdint>
#include <stdexcept>

namespace mdbg {

inline std::uintptr_t snapshot_frame_lookup_pc(
    const SnapshotInspectionFrameContext& frame) {
  if (frame.pc_ownership == SnapshotFramePcOwnership::ExactInstruction) {
    return frame.runtime_pc;
  }
  if (frame.runtime_pc == 0) {
    throw std::underflow_error(
        "historical snapshot return PC cannot be normalized below zero");
  }
  return frame.runtime_pc - 1;
}

inline void validate_snapshot_frame_lookup_pc(
    const SnapshotInspectionFrameContext& frame) {
  const auto lookup_pc = snapshot_frame_lookup_pc(frame);
  if (frame.pc_ownership == SnapshotFramePcOwnership::ExactInstruction) {
    if (lookup_pc != frame.runtime_pc) {
      throw std::logic_error("exact snapshot lookup PC changed runtime identity");
    }
    return;
  }
  if (frame.index == 0) {
    throw std::logic_error("snapshot frame zero cannot own a return-address PC");
  }
  if (lookup_pc >= frame.runtime_pc || lookup_pc + 1 != frame.runtime_pc) {
    throw std::logic_error("historical snapshot lookup PC is not the return call site");
  }
}

}  // namespace mdbg

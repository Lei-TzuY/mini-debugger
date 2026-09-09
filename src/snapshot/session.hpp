#pragma once

#include "dwarf/local_value.hpp"
#include "snapshot/inspection.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mdbg {

class CoreInspectionSession {
 public:
  explicit CoreInspectionSession(std::string core_path, std::size_t max_frames = 64)
      : snapshot_(std::move(core_path)), trace_(build_snapshot_inspection_frames(snapshot_, max_frames)) {
    if (trace_.frames.empty()) {
      throw std::runtime_error("core inspection produced no snapshot frames");
    }
    validate_snapshot_inspection_frame(snapshot_, trace_.frames.front());
  }

  CoreInspectionSession(const CoreInspectionSession&) = delete;
  CoreInspectionSession& operator=(const CoreInspectionSession&) = delete;
  CoreInspectionSession(CoreInspectionSession&&) = delete;
  CoreInspectionSession& operator=(CoreInspectionSession&&) = delete;

  [[nodiscard]] const CoreSnapshot& snapshot() const noexcept { return snapshot_; }
  [[nodiscard]] const SnapshotInspectionTrace& trace() const noexcept { return trace_; }
  [[nodiscard]] std::size_t selected_frame_index() const noexcept { return selected_frame_; }

  [[nodiscard]] const SnapshotInspectionFrameContext& selected_frame() const {
    return trace_.frames.at(selected_frame_);
  }

  void select_frame(std::size_t index) {
    if (index >= trace_.frames.size()) {
      throw std::out_of_range("core frame index is out of range");
    }
    validate_snapshot_inspection_frame(snapshot_, trace_.frames[index]);
    selected_frame_ = index;
  }

  [[nodiscard]] LocalScalarValue inspect_value(std::string_view name) const {
    const auto& frame = selected_frame();
    validate_snapshot_inspection_frame(snapshot_, frame);
    return inspect_local_value(snapshot_, frame, name);
  }

 private:
  CoreSnapshot snapshot_;
  SnapshotInspectionTrace trace_;
  std::size_t selected_frame_{0};
};

}  // namespace mdbg

#pragma once

#include "dwarf/local_value.hpp"
#include "snapshot/inspection.hpp"
#include "snapshot/module_resolver.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mdbg {

class CoreInspectionSession {
 public:
  explicit CoreInspectionSession(std::string core_path, std::size_t max_frames = 64)
      : CoreInspectionSession(std::move(core_path), SnapshotModuleResolver{}, max_frames) {}

  CoreInspectionSession(std::string core_path, SnapshotModuleResolver module_resolver,
                        std::size_t max_frames = 64)
      : snapshot_(std::move(core_path)), module_resolver_(std::move(module_resolver)),
        max_frames_(max_frames), selected_thread_tid_(snapshot_.crashed_tid()),
        trace_(build_snapshot_inspection_frames(snapshot_, snapshot_.crashed_thread(),
                                                module_resolver_, max_frames_)) {
    validate_trace();
  }

  CoreInspectionSession(const CoreInspectionSession&) = delete;
  CoreInspectionSession& operator=(const CoreInspectionSession&) = delete;
  CoreInspectionSession(CoreInspectionSession&&) = delete;
  CoreInspectionSession& operator=(CoreInspectionSession&&) = delete;

  [[nodiscard]] const CoreSnapshot& snapshot() const noexcept { return snapshot_; }
  [[nodiscard]] const SnapshotModuleResolver& module_resolver() const noexcept {
    return module_resolver_;
  }
  [[nodiscard]] const SnapshotInspectionTrace& trace() const noexcept { return trace_; }
  [[nodiscard]] pid_t selected_thread_tid() const noexcept { return selected_thread_tid_; }
  [[nodiscard]] const std::vector<CoreThreadSnapshot>& threads() const noexcept {
    return snapshot_.threads();
  }
  [[nodiscard]] std::size_t selected_frame_index() const noexcept { return selected_frame_; }

  [[nodiscard]] const SnapshotInspectionFrameContext& selected_frame() const {
    return trace_.frames.at(selected_frame_);
  }

  void select_thread(pid_t tid) {
    const auto& thread = snapshot_.thread(tid);
    auto next_trace =
        build_snapshot_inspection_frames(snapshot_, thread, module_resolver_, max_frames_);
    if (next_trace.frames.empty()) {
      throw std::runtime_error("core thread inspection produced no snapshot frames");
    }
    validate_snapshot_inspection_frame(snapshot_, next_trace.frames.front());
    trace_ = std::move(next_trace);
    selected_thread_tid_ = tid;
    selected_frame_ = 0;
  }

  void select_frame(std::size_t index) {
    if (index >= trace_.frames.size()) {
      throw std::out_of_range("core frame index is out of range");
    }
    validate_snapshot_inspection_frame(snapshot_, trace_.frames[index]);
    if (trace_.frames[index].thread_tid != selected_thread_tid_) {
      throw std::logic_error("core frame belongs to a different selected thread");
    }
    selected_frame_ = index;
  }

  [[nodiscard]] LocalScalarValue inspect_value(std::string_view name) const {
    const auto& frame = selected_frame();
    validate_snapshot_inspection_frame(snapshot_, frame);
    if (frame.thread_tid != selected_thread_tid_) {
      throw std::logic_error("selected core frame belongs to a different thread");
    }
    return inspect_local_value(snapshot_, frame, module_resolver_, name);
  }

 private:
  void validate_trace() const {
    if (trace_.frames.empty()) {
      throw std::runtime_error("core inspection produced no snapshot frames");
    }
    validate_snapshot_inspection_frame(snapshot_, trace_.frames.front());
    if (trace_.frames.front().thread_tid != selected_thread_tid_) {
      throw std::logic_error("core inspection trace belongs to a different thread");
    }
  }

  CoreSnapshot snapshot_;
  SnapshotModuleResolver module_resolver_;
  std::size_t max_frames_{64};
  pid_t selected_thread_tid_{-1};
  SnapshotInspectionTrace trace_;
  std::size_t selected_frame_{0};
};

}  // namespace mdbg

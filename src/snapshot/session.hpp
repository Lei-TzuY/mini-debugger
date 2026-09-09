#pragma once

#include "dwarf/local_value.hpp"
#include "snapshot/inspection.hpp"
#include "snapshot/memory.hpp"
#include "snapshot/module_path.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mdbg {

class CoreInspectionSession {
 public:
  explicit CoreInspectionSession(std::string core_path, std::size_t max_frames = 64)
      : CoreInspectionSession(std::move(core_path), SnapshotModulePathResolver{}, max_frames) {}

  CoreInspectionSession(std::string core_path, SnapshotModulePathResolver module_paths,
                        std::size_t max_frames = 64)
      : snapshot_(std::move(core_path)), module_paths_(std::move(module_paths)),
        max_frames_(max_frames), selected_thread_tid_(snapshot_.crashed_tid()),
        trace_(build_snapshot_inspection_frames(snapshot_, snapshot_.crashed_thread(),
                                                max_frames_, module_paths_)) {
    validate_trace();
  }

  CoreInspectionSession(const CoreInspectionSession&) = delete;
  CoreInspectionSession& operator=(const CoreInspectionSession&) = delete;
  CoreInspectionSession(CoreInspectionSession&&) = delete;
  CoreInspectionSession& operator=(CoreInspectionSession&&) = delete;

  [[nodiscard]] const CoreSnapshot& snapshot() const noexcept { return snapshot_; }
  [[nodiscard]] const std::optional<CoreCrashInfo>& crash_info() const noexcept {
    return snapshot_.crash_info();
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

  [[nodiscard]] std::optional<SnapshotResolvedSymbol> find_symbol(
      std::uintptr_t runtime_pc) const {
    return find_snapshot_symbol_by_runtime_address(snapshot_, runtime_pc, module_paths_);
  }

  [[nodiscard]] std::optional<SnapshotResolvedSource> find_source(
      std::uintptr_t runtime_pc) const {
    return find_snapshot_source_by_runtime_address(snapshot_, runtime_pc, module_paths_);
  }

  [[nodiscard]] SnapshotMemoryRead read_memory(std::uintptr_t address,
                                               std::size_t length) const {
    return read_snapshot_memory(snapshot_, module_paths_, address, length);
  }

  void select_thread(pid_t tid) {
    const auto& thread = snapshot_.thread(tid);
    auto next_trace =
        build_snapshot_inspection_frames(snapshot_, thread, max_frames_, module_paths_);
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
    return inspect_local_value(snapshot_, frame, name, module_paths_);
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
  SnapshotModulePathResolver module_paths_;
  std::size_t max_frames_{64};
  pid_t selected_thread_tid_{-1};
  SnapshotInspectionTrace trace_;
  std::size_t selected_frame_{0};
};

}  // namespace mdbg

#pragma once

#include "dwarf/inline_context.hpp"
#include "dwarf/inline_member.hpp"
#include "dwarf/local_value.hpp"
#include "snapshot/frame_lookup.hpp"
#include "snapshot/inspection.hpp"
#include "snapshot/memory.hpp"
#include "snapshot/module_path.hpp"
#include "snapshot/startup.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mdbg {

class CoreInspectionSession {
 public:
  explicit CoreInspectionSession(std::string core_path, std::size_t max_frames = 64)
      : CoreInspectionSession(std::move(core_path), SnapshotModulePathResolver{}, max_frames) {}

  CoreInspectionSession(std::string core_path, SnapshotModulePathResolver module_paths,
                        std::size_t max_frames = 64)
      : snapshot_(std::move(core_path)), module_paths_(std::move(module_paths)),
        startup_info_(read_core_startup_info(snapshot_)), max_frames_(max_frames),
        selected_thread_tid_(snapshot_.crashed_tid()),
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
  [[nodiscard]] const std::optional<CoreProcessInfo>& process_info() const noexcept {
    return snapshot_.process_info();
  }
  [[nodiscard]] const std::optional<CoreStartupInfo>& startup_info() const noexcept {
    return startup_info_;
  }
  [[nodiscard]] const SnapshotInspectionTrace& trace() const noexcept { return trace_; }
  [[nodiscard]] pid_t selected_thread_tid() const noexcept { return selected_thread_tid_; }
  [[nodiscard]] const std::vector<CoreThreadSnapshot>& threads() const noexcept {
    return snapshot_.threads();
  }
  [[nodiscard]] std::size_t selected_frame_index() const noexcept { return selected_frame_; }
  [[nodiscard]] std::optional<std::size_t> selected_inline_context_index() const noexcept {
    return selected_inline_context_index_;
  }

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

  [[nodiscard]] std::optional<SnapshotResolvedSymbol> find_frame_symbol(
      const SnapshotInspectionFrameContext& frame) const {
    validate_selected_frame(frame);
    return find_symbol(snapshot_frame_lookup_pc(frame));
  }

  [[nodiscard]] std::optional<SnapshotResolvedSource> find_frame_source(
      const SnapshotInspectionFrameContext& frame) const {
    validate_selected_frame(frame);
    return find_source(snapshot_frame_lookup_pc(frame));
  }

  [[nodiscard]] SnapshotMemoryRead read_memory(std::uintptr_t address,
                                               std::size_t length) const {
    return read_snapshot_memory(snapshot_, module_paths_, address, length);
  }

  [[nodiscard]] std::vector<InlineCallsiteContext> inline_contexts() const {
    const auto& frame = selected_frame();
    validate_selected_frame(frame);
    return discover_inline_call_chain(snapshot_, frame, module_paths_);
  }

  void select_inline_context(std::size_t index) {
    const auto contexts = inline_contexts();
    if (index >= contexts.size()) {
      throw std::out_of_range("core inline-context index is out of range");
    }
    selected_inline_context_ = contexts[index];
    selected_inline_context_index_ = index;
  }

  void clear_inline_context() noexcept {
    selected_inline_context_.reset();
    selected_inline_context_index_.reset();
  }

  void select_thread(pid_t tid) {
    const auto& thread = snapshot_.thread(tid);
    auto next_trace =
        build_snapshot_inspection_frames(snapshot_, thread, max_frames_, module_paths_);
    if (next_trace.frames.empty()) {
      throw std::runtime_error("core thread inspection produced no snapshot frames");
    }
    validate_snapshot_inspection_frame(snapshot_, next_trace.frames.front());
    validate_snapshot_frame_lookup_pc(next_trace.frames.front());
    trace_ = std::move(next_trace);
    selected_thread_tid_ = tid;
    selected_frame_ = 0;
    clear_inline_context();
  }

  void select_frame(std::size_t index) {
    if (index >= trace_.frames.size()) {
      throw std::out_of_range("core frame index is out of range");
    }
    validate_snapshot_inspection_frame(snapshot_, trace_.frames[index]);
    validate_snapshot_frame_lookup_pc(trace_.frames[index]);
    if (trace_.frames[index].thread_tid != selected_thread_tid_) {
      throw std::logic_error("core frame belongs to a different selected thread");
    }
    selected_frame_ = index;
    clear_inline_context();
  }

  [[nodiscard]] std::vector<LocalDiscoveryEntry> locals() const {
    const auto& frame = selected_frame();
    validate_selected_frame(frame);
    if (selected_inline_context_) {
      if (selected_inline_context_->module_path != frame.module_path) {
        throw std::logic_error("selected inline context belongs to a different module");
      }
      return discover_inline_local_values(snapshot_, frame,
                                          selected_inline_context_->die_offset,
                                          module_paths_);
    }
    return discover_local_values(snapshot_, frame, module_paths_);
  }

  [[nodiscard]] LocalScalarValue inspect_value(std::string_view name) const {
    const auto& frame = selected_frame();
    validate_selected_frame(frame);
    if (selected_inline_context_) {
      if (selected_inline_context_->module_path != frame.module_path) {
        throw std::logic_error("selected inline context belongs to a different module");
      }
      return inspect_inline_local_value(snapshot_, frame,
                                        selected_inline_context_->die_offset,
                                        name, module_paths_);
    }
    return inspect_local_value(snapshot_, frame, name, module_paths_);
  }

  [[nodiscard]] LocalScalarValue dereference_value(std::string_view name) const {
    const auto& frame = selected_frame();
    validate_selected_frame(frame);
    if (selected_inline_context_) {
      if (selected_inline_context_->module_path != frame.module_path) {
        throw std::logic_error("selected inline context belongs to a different module");
      }
      return dereference_inline_local_pointer(
          snapshot_, frame, selected_inline_context_->die_offset, name, module_paths_);
    }
    return dereference_local_pointer(snapshot_, frame, name, module_paths_);
  }

  [[nodiscard]] LocalScalarValue inspect_pointer_member(
      std::string_view name, std::string_view member_name) const {
    const auto& frame = selected_frame();
    validate_selected_frame(frame);
    if (selected_inline_context_) {
      if (selected_inline_context_->module_path != frame.module_path) {
        throw std::logic_error("selected inline context belongs to a different module");
      }
      return inspect_inline_local_pointer_member(
          snapshot_, frame, selected_inline_context_->die_offset, name,
          member_name, module_paths_);
    }
    return inspect_local_pointer_member(
        snapshot_, frame, name, member_name, module_paths_);
  }

  [[nodiscard]] LocalScalarValue dereference_pointer_member(
      std::string_view name, std::string_view member_name) const {
    const auto& frame = selected_frame();
    validate_selected_frame(frame);
    if (selected_inline_context_) {
      if (selected_inline_context_->module_path != frame.module_path) {
        throw std::logic_error("selected inline context belongs to a different module");
      }
      return dereference_inline_local_pointer_member(
          snapshot_, frame, selected_inline_context_->die_offset, name,
          member_name, module_paths_);
    }
    return dereference_local_pointer_member(
        snapshot_, frame, name, member_name, module_paths_);
  }

  [[nodiscard]] LocalScalarValue inspect_aggregate_member(
      std::string_view name, std::string_view member_name) const {
    const auto& frame = selected_frame();
    validate_selected_frame(frame);
    if (!selected_inline_context_) {
      throw std::logic_error(
          "direct aggregate member traversal requires a selected inline context");
    }
    if (selected_inline_context_->module_path != frame.module_path) {
      throw std::logic_error("selected inline context belongs to a different module");
    }
    const auto aggregate = inspect_inline_local_value(
        snapshot_, frame, selected_inline_context_->die_offset, name, module_paths_);
    return inspect_inline_local_aggregate_member(aggregate, member_name);
  }

  [[nodiscard]] LocalScalarValue inspect_array_element(
    std::string_view name, std::size_t index) const {
  const auto& frame = selected_frame();
  validate_selected_frame(frame);
  if (!selected_inline_context_) {
    throw std::logic_error(
        "fixed-array indexing requires a selected inline context");
  }
  if (selected_inline_context_->module_path != frame.module_path) {
    throw std::logic_error("selected inline context belongs to a different module");
  }
  const auto array = inspect_inline_local_value(
      snapshot_, frame, selected_inline_context_->die_offset, name, module_paths_);
  if (array.kind != LocalValueKind::Array || !array.array_type) {
    throw std::logic_error(
        "selected-inline local value is not a bounded fixed array: " +
        std::string(name));
  }
  if (index >= array.array_type->element_count || index >= array.elements.size()) {
    throw std::out_of_range("array index is out of range");
  }
  const auto& element = array.elements[index];
  if (element.kind != LocalValueKind::Integer ||
      element.byte_size != array.array_type->element_byte_size ||
      element.is_signed != array.array_type->element_is_signed) {
    throw std::logic_error("bounded fixed-array element metadata is inconsistent");
  }
  LocalScalarValue result{array.module_path,
                          array.name + "[" + std::to_string(index) + "]",
                          element.raw_value, element.byte_size,
                          element.is_signed, element.kind};
  result.storage = array.storage;
  result.storage_module_path = array.storage_module_path;
  result.storage_file_path = array.storage_file_path;
  result.storage_file_offset = array.storage_file_offset;
  if (array.storage == LocalValueStorage::SnapshotRuntimeArtifact) {
    if (index > std::numeric_limits<std::uint64_t>::max() /
                    element.byte_size) {
      throw std::overflow_error("array element artifact offset overflows");
    }
    const auto offset = static_cast<std::uint64_t>(index * element.byte_size);
    if (offset > std::numeric_limits<std::uint64_t>::max() -
                     result.storage_file_offset) {
      throw std::overflow_error("array element artifact offset overflows");
    }
    result.storage_file_offset += offset;
  }
  return result;
}

[[nodiscard]] LocalScalarValue dereference_aggregate_member(
      std::string_view name, std::string_view member_name) const {
    const auto& frame = selected_frame();
    validate_selected_frame(frame);
    if (!selected_inline_context_) {
      throw std::logic_error(
          "direct aggregate member traversal requires a selected inline context");
    }
    if (selected_inline_context_->module_path != frame.module_path) {
      throw std::logic_error("selected inline context belongs to a different module");
    }
    const auto aggregate = inspect_inline_local_value(
        snapshot_, frame, selected_inline_context_->die_offset, name, module_paths_);
    return dereference_inline_local_aggregate_member(
        snapshot_, aggregate, member_name, module_paths_);
  }

 private:
  void validate_selected_frame(const SnapshotInspectionFrameContext& frame) const {
    validate_snapshot_inspection_frame(snapshot_, frame);
    validate_snapshot_frame_lookup_pc(frame);
    if (frame.thread_tid != selected_thread_tid_) {
      throw std::logic_error("selected core frame belongs to a different thread");
    }
  }

  void validate_trace() const {
    if (trace_.frames.empty()) {
      throw std::runtime_error("core inspection produced no snapshot frames");
    }
    for (const auto& frame : trace_.frames) {
      validate_snapshot_inspection_frame(snapshot_, frame);
      validate_snapshot_frame_lookup_pc(frame);
    }
    if (trace_.frames.front().thread_tid != selected_thread_tid_) {
      throw std::logic_error("core inspection trace belongs to a different thread");
    }
  }

  CoreSnapshot snapshot_;
  SnapshotModulePathResolver module_paths_;
  std::optional<CoreStartupInfo> startup_info_;
  std::size_t max_frames_{64};
  pid_t selected_thread_tid_{-1};
  SnapshotInspectionTrace trace_;
  std::size_t selected_frame_{0};
  std::optional<InlineCallsiteContext> selected_inline_context_;
  std::optional<std::size_t> selected_inline_context_index_;
};

}  // namespace mdbg

#pragma once

#include "dwarf/eh_frame.hpp"
#include "dwarf/eh_frame_signal.hpp"
#include "dwarf/line_table.hpp"
#include "elf/elf.hpp"
#include "snapshot/linux_x86_signal_frame.hpp"
#include "snapshot/module_path.hpp"
#include "unwind/cfi.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mdbg {

struct SnapshotModuleAddress {
  std::string module_path;
  std::string module_file_path;
  std::uint64_t virtual_address;
};

struct SnapshotResolvedSymbol {
  std::string module_path;
  std::string name;
  std::uint64_t offset;
};

struct SnapshotResolvedSource {
  std::string module_path;
  std::string file;
  std::uint64_t line;
  std::uint64_t column;
};

enum class SnapshotFramePcOwnership {
  ExactInstruction,
  ReturnAddress
};

struct SnapshotInspectionFrameContext {
  std::size_t index;
  const CoreSnapshot* owner_snapshot;
  pid_t thread_tid;
  int thread_signal_number;
  std::uintptr_t origin_runtime_pc;
  std::uintptr_t origin_stack_pointer;
  std::optional<std::uintptr_t> origin_frame_pointer;
  std::uintptr_t runtime_pc;
  std::uintptr_t stack_pointer;
  std::optional<std::uintptr_t> frame_pointer;
  SnapshotFramePcOwnership pc_ownership;
  std::string module_path;
  InspectionRegisterState registers;
};

struct SnapshotInspectionTrace {
  std::vector<SnapshotInspectionFrameContext> frames;
  CfiUnwindStopReason stop_reason;
};

inline const SnapshotModulePathResolver& identity_snapshot_module_paths() {
  static const SnapshotModulePathResolver resolver;
  return resolver;
}

inline SnapshotModuleAddress resolve_snapshot_module_address(
    const CoreSnapshot& snapshot, std::uintptr_t address,
    const SnapshotModulePathResolver& module_paths) {
  const auto mapping = snapshot.mapping_for_address(address);
  if (!mapping) {
    throw std::runtime_error("snapshot address is not covered by NT_FILE mapping");
  }
  if (mapping->path.empty() || mapping->path.front() != '/') {
    throw std::runtime_error("snapshot NT_FILE mapping lacks an absolute module path");
  }

  const auto module_file_path = module_paths.resolve(mapping->path);
  const ElfFile module(module_file_path);
  if (!module.is_pie()) {
    return SnapshotModuleAddress{mapping->path, module_file_path,
                                 static_cast<std::uint64_t>(address)};
  }

  const auto base = std::find_if(
      snapshot.file_mappings().begin(), snapshot.file_mappings().end(),
      [&](const CoreFileMapping& candidate) {
        return candidate.path == mapping->path && candidate.file_offset == 0;
      });
  if (base == snapshot.file_mappings().end()) {
    throw std::runtime_error("snapshot PIE module lacks offset-zero NT_FILE mapping");
  }
  if (base->start < module.load_virtual_base()) {
    throw std::runtime_error("snapshot PIE mapping is below ELF load virtual base");
  }

  const auto bias = base->start - module.load_virtual_base();
  const auto runtime = static_cast<std::uint64_t>(address);
  if (runtime < bias) {
    throw std::runtime_error("snapshot runtime address is below module load bias");
  }
  return SnapshotModuleAddress{mapping->path, module_file_path, runtime - bias};
}

inline SnapshotModuleAddress resolve_snapshot_module_address(
    const CoreSnapshot& snapshot, std::uintptr_t address) {
  return resolve_snapshot_module_address(snapshot, address,
                                         identity_snapshot_module_paths());
}

inline std::optional<SnapshotResolvedSymbol> find_snapshot_symbol_by_runtime_address(
    const CoreSnapshot& snapshot, std::uintptr_t address,
    const SnapshotModulePathResolver& module_paths) {
  const auto module_address =
      resolve_snapshot_module_address(snapshot, address, module_paths);
  const ElfFile module(module_paths.resolve_debug_file(module_address.module_path));
  const auto resolved = module.find_symbol_by_virtual_address(module_address.virtual_address);
  if (!resolved) return std::nullopt;
  return SnapshotResolvedSymbol{module_address.module_path, resolved->symbol.name,
                                resolved->offset};
}

inline std::optional<SnapshotResolvedSymbol> find_snapshot_symbol_by_runtime_address(
    const CoreSnapshot& snapshot, std::uintptr_t address) {
  return find_snapshot_symbol_by_runtime_address(
      snapshot, address, identity_snapshot_module_paths());
}

inline std::optional<SnapshotResolvedSource> find_snapshot_source_by_runtime_address(
    const CoreSnapshot& snapshot, std::uintptr_t address,
    const SnapshotModulePathResolver& module_paths) {
  const auto module_address =
      resolve_snapshot_module_address(snapshot, address, module_paths);
  const DwarfLineTable lines(module_paths.resolve_debug_file(module_address.module_path));
  if (!lines.available()) return std::nullopt;
  const auto source = lines.find_virtual_address(module_address.virtual_address);
  if (!source) return std::nullopt;
  return SnapshotResolvedSource{module_address.module_path, source->file, source->line,
                                source->column};
}

inline std::optional<SnapshotResolvedSource> find_snapshot_source_by_runtime_address(
    const CoreSnapshot& snapshot, std::uintptr_t address) {
  return find_snapshot_source_by_runtime_address(
      snapshot, address, identity_snapshot_module_paths());
}

inline void validate_snapshot_inspection_frame(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame) {
  if (frame.owner_snapshot != &snapshot) {
    throw std::logic_error(
        "snapshot inspection frame belongs to a different CoreSnapshot owner");
  }
  const auto& thread = snapshot.thread(frame.thread_tid);
  const auto& regs = thread.registers;
  if (frame.thread_signal_number != thread.signal_number ||
      frame.origin_runtime_pc != static_cast<std::uintptr_t>(regs.rip) ||
      frame.origin_stack_pointer != static_cast<std::uintptr_t>(regs.rsp) ||
      frame.origin_frame_pointer !=
          std::optional<std::uintptr_t>{static_cast<std::uintptr_t>(regs.rbp)}) {
    throw std::logic_error(
        "snapshot inspection frame thread identity does not match its owner");
  }
  if (frame.runtime_pc == 0 || frame.stack_pointer == 0 || frame.module_path.empty()) {
    throw std::invalid_argument("snapshot inspection frame is incomplete");
  }
  if (frame.index == 0 &&
      frame.pc_ownership != SnapshotFramePcOwnership::ExactInstruction) {
    throw std::logic_error("snapshot frame zero must own an exact instruction PC");
  }
}

inline std::string snapshot_frame_module_path(const CoreSnapshot& snapshot,
                                              std::uintptr_t address) {
  const auto mapping = snapshot.mapping_for_address(address);
  if (!mapping) {
    throw std::runtime_error("snapshot frame address is not covered by NT_FILE mapping");
  }
  if (mapping->path.empty() || mapping->path.front() != '/') {
    throw std::runtime_error("snapshot frame NT_FILE mapping lacks an absolute module path");
  }
  return mapping->path;
}

inline SnapshotInspectionFrameContext make_snapshot_inspection_frame(
    const CoreSnapshot& snapshot, const CoreThreadSnapshot& thread,
    std::size_t index, const EhFrameCursor& cursor,
    SnapshotFramePcOwnership pc_ownership,
    std::optional<std::uint64_t> restored_r12 = std::nullopt) {
  const auto& owned = snapshot.thread(thread.tid);
  if (&owned != &thread) {
    throw std::logic_error("snapshot thread context belongs to a different owner");
  }
  const auto& origin = thread.registers;
  InspectionRegisterState recovered{};
  recovered.rbx = cursor.rbx;
  recovered.rbp = cursor.frame_pointer;
  recovered.rsp = static_cast<std::uint64_t>(cursor.stack_pointer);
  recovered.r12 = restored_r12;
  return SnapshotInspectionFrameContext{
      index,
      &snapshot,
      thread.tid,
      thread.signal_number,
      static_cast<std::uintptr_t>(origin.rip),
      static_cast<std::uintptr_t>(origin.rsp),
      std::optional<std::uintptr_t>{static_cast<std::uintptr_t>(origin.rbp)},
      cursor.instruction_pointer,
      cursor.stack_pointer,
      cursor.frame_pointer,
      pc_ownership,
      snapshot_frame_module_path(snapshot, cursor.instruction_pointer),
      recovered};
}

inline SnapshotInspectionTrace build_snapshot_inspection_frames(
    const CoreSnapshot& snapshot, const CoreThreadSnapshot& thread,
    std::size_t max_frames, const SnapshotModulePathResolver& module_paths) {
  if (max_frames == 0) {
    throw std::invalid_argument(
        "snapshot inspection requires a non-zero frame limit");
  }
  const auto& owned = snapshot.thread(thread.tid);
  if (&owned != &thread) {
    throw std::logic_error("snapshot thread context belongs to a different owner");
  }

  const auto& regs = thread.registers;
  EhFrameCursor current{static_cast<std::uintptr_t>(regs.rip),
                        static_cast<std::uintptr_t>(regs.rsp),
                        static_cast<std::uintptr_t>(regs.rbp), regs.rbx};
  SnapshotInspectionTrace result{
      {make_snapshot_inspection_frame(snapshot, thread, 0, current,
                                      SnapshotFramePcOwnership::ExactInstruction)},
      CfiUnwindStopReason::EndOfChain};
  if (max_frames == 1) {
    result.stop_reason = CfiUnwindStopReason::FrameLimit;
    return result;
  }

  const CfiMemoryReader read_memory =
      [&snapshot](std::uintptr_t address, std::size_t length) {
        return snapshot.read_memory(address, length);
      };

  while (result.frames.size() < max_frames) {
    std::optional<EhFrameCursor> caller;
    std::optional<std::uint64_t> caller_r12;
    auto caller_pc_ownership = SnapshotFramePcOwnership::ReturnAddress;
    try {
      const auto module = resolve_snapshot_module_address(
          snapshot, current.instruction_pointer, module_paths);
      const EhFrame cfi(module.module_file_path);
      if (!cfi.available()) {
        result.stop_reason = result.frames.size() == 1
                                 ? CfiUnwindStopReason::NoFrameInfo
                                 : CfiUnwindStopReason::EndOfChain;
        return result;
      }

      const EhFrameSignalIndex signal_index(module.module_file_path);
      if (signal_index.available() && signal_index.is_signal_frame(module.virtual_address)) {
        const auto signal =
            recover_linux_x86_signal_frame(snapshot, current.stack_pointer);
        caller = signal.cursor;
        caller_r12 = signal.r12;
        caller_pc_ownership = SnapshotFramePcOwnership::ExactInstruction;
      } else {
        caller = cfi.caller_frame(read_memory, module.virtual_address, current);
      }

      if (!caller) {
        result.stop_reason = result.frames.size() == 1
                                 ? CfiUnwindStopReason::NoFrameInfo
                                 : CfiUnwindStopReason::EndOfChain;
        return result;
      }
      if (caller->instruction_pointer == current.instruction_pointer ||
          caller->stack_pointer <= current.stack_pointer) {
        result.stop_reason = CfiUnwindStopReason::InvalidFrameState;
        return result;
      }
      static_cast<void>(resolve_snapshot_module_address(
          snapshot, caller->instruction_pointer, module_paths));
    } catch (const std::exception&) {
      result.stop_reason = CfiUnwindStopReason::InvalidFrameState;
      return result;
    }

    result.frames.push_back(make_snapshot_inspection_frame(
        snapshot, thread, result.frames.size(), *caller, caller_pc_ownership,
        caller_r12));
    current = *caller;
  }

  result.stop_reason = CfiUnwindStopReason::FrameLimit;
  return result;
}

inline SnapshotInspectionTrace build_snapshot_inspection_frames(
    const CoreSnapshot& snapshot, const CoreThreadSnapshot& thread,
    std::size_t max_frames = 64) {
  return build_snapshot_inspection_frames(snapshot, thread, max_frames,
                                          identity_snapshot_module_paths());
}

inline SnapshotInspectionTrace build_snapshot_inspection_frames(
    const CoreSnapshot& snapshot, std::size_t max_frames,
    const SnapshotModulePathResolver& module_paths) {
  return build_snapshot_inspection_frames(snapshot, snapshot.crashed_thread(),
                                          max_frames, module_paths);
}

inline SnapshotInspectionTrace build_snapshot_inspection_frames(
    const CoreSnapshot& snapshot, std::size_t max_frames = 64) {
  return build_snapshot_inspection_frames(snapshot, snapshot.crashed_thread(), max_frames,
                                          identity_snapshot_module_paths());
}

inline CfiBacktrace unwind_eh_frame(const CoreSnapshot& snapshot,
                                    std::size_t max_frames = 64) {
  const auto inspection = build_snapshot_inspection_frames(snapshot, max_frames);
  CfiBacktrace result{{}, inspection.stop_reason};
  result.frames.reserve(inspection.frames.size());
  for (const auto& frame : inspection.frames) {
    result.frames.push_back(
        CfiStackFrame{frame.runtime_pc, frame.stack_pointer, frame.frame_pointer});
  }
  return result;
}

}  // namespace mdbg

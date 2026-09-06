#pragma once

#include "dwarf/eh_frame.hpp"
#include "dwarf/line_table.hpp"
#include "elf/elf.hpp"
#include "unwind/cfi.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace mdbg {

struct SnapshotModuleAddress {
  std::string module_path;
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

inline SnapshotModuleAddress resolve_snapshot_module_address(
    const CoreSnapshot& snapshot, std::uintptr_t address) {
  const auto mapping = snapshot.mapping_for_address(address);
  if (!mapping) {
    throw std::runtime_error("snapshot address is not covered by NT_FILE mapping");
  }
  if (mapping->path.empty() || mapping->path.front() != '/') {
    throw std::runtime_error("snapshot NT_FILE mapping lacks an absolute module path");
  }

  const ElfFile module(mapping->path);
  if (!module.is_pie()) {
    return SnapshotModuleAddress{mapping->path, static_cast<std::uint64_t>(address)};
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
  return SnapshotModuleAddress{mapping->path, runtime - bias};
}

inline std::optional<SnapshotResolvedSymbol> find_snapshot_symbol_by_runtime_address(
    const CoreSnapshot& snapshot, std::uintptr_t address) {
  const auto module_address = resolve_snapshot_module_address(snapshot, address);
  const ElfFile module(module_address.module_path);
  const auto resolved = module.find_symbol_by_virtual_address(module_address.virtual_address);
  if (!resolved) return std::nullopt;
  return SnapshotResolvedSymbol{module_address.module_path, resolved->symbol.name,
                                resolved->offset};
}

inline std::optional<SnapshotResolvedSource> find_snapshot_source_by_runtime_address(
    const CoreSnapshot& snapshot, std::uintptr_t address) {
  const auto module_address = resolve_snapshot_module_address(snapshot, address);
  const DwarfLineTable lines(module_address.module_path);
  if (!lines.available()) return std::nullopt;
  const auto source = lines.find_virtual_address(module_address.virtual_address);
  if (!source) return std::nullopt;
  return SnapshotResolvedSource{module_address.module_path, source->file, source->line,
                                source->column};
}

inline CfiBacktrace unwind_eh_frame(const CoreSnapshot& snapshot,
                                    std::size_t max_frames = 64) {
  if (max_frames == 0) {
    throw std::invalid_argument("snapshot CFI unwind requires a non-zero frame limit");
  }

  const auto& regs = snapshot.registers();
  EhFrameCursor current{static_cast<std::uintptr_t>(regs.rip),
                        static_cast<std::uintptr_t>(regs.rsp),
                        static_cast<std::uintptr_t>(regs.rbp), regs.rbx};
  CfiBacktrace result{{CfiStackFrame{current.instruction_pointer, current.stack_pointer,
                                    current.frame_pointer}},
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
    try {
      const auto module =
          resolve_snapshot_module_address(snapshot, current.instruction_pointer);
      const EhFrame cfi(module.module_path);
      if (!cfi.available()) {
        result.stop_reason = result.frames.size() == 1
                                 ? CfiUnwindStopReason::NoFrameInfo
                                 : CfiUnwindStopReason::EndOfChain;
        return result;
      }
      caller = cfi.caller_frame(read_memory, module.virtual_address, current);
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
      static_cast<void>(
          resolve_snapshot_module_address(snapshot, caller->instruction_pointer));
    } catch (const std::exception&) {
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

}  // namespace mdbg

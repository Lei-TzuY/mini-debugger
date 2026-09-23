#pragma once

#include "dwarf/line_table.hpp"
#include "dwarf/local_value.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mdbg {

class CoreSnapshot;
class Debugger;
class ElfFile;
class SnapshotModulePathResolver;
struct InspectionFrameContext;
struct SnapshotInspectionFrameContext;

struct InlineCallsiteContext {
  std::size_t die_offset;
  std::size_t depth;
  std::string module_path;
  std::string name;
  SourceLocation call_site;
};

std::vector<InlineCallsiteContext> discover_inline_call_chain(
    const Debugger& debugger, const ElfFile& preferred_elf,
    const InspectionFrameContext& frame);
std::vector<InlineCallsiteContext> discover_inline_call_chain(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    const SnapshotModulePathResolver& module_paths);

std::vector<LocalDiscoveryEntry> discover_inline_local_values(
    const Debugger& debugger, const ElfFile& preferred_elf,
    const InspectionFrameContext& frame, std::size_t inline_die_offset);
std::vector<LocalDiscoveryEntry> discover_inline_local_values(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::size_t inline_die_offset, const SnapshotModulePathResolver& module_paths);

LocalScalarValue inspect_inline_local_value(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::size_t inline_die_offset, std::string_view name,
    const SnapshotModulePathResolver& module_paths);
LocalScalarValue dereference_inline_local_pointer(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::size_t inline_die_offset, std::string_view name,
    const SnapshotModulePathResolver& module_paths);

}  // namespace mdbg

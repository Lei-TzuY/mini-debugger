#pragma once

#include "dwarf/line_table.hpp"
#include "dwarf/local_value.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace mdbg {

class CoreSnapshot;
class SnapshotModulePathResolver;
struct SnapshotInspectionFrameContext;

struct InlineCallsiteContext {
  std::size_t die_offset;
  std::size_t depth;
  std::string module_path;
  std::string name;
  SourceLocation call_site;
};

std::vector<InlineCallsiteContext> discover_inline_call_chain(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    const SnapshotModulePathResolver& module_paths);

std::vector<LocalDiscoveryEntry> discover_inline_local_values(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::size_t inline_die_offset, const SnapshotModulePathResolver& module_paths);

}  // namespace mdbg

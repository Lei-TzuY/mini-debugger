#include "dwarf/source_lookup_snapshot_impl.inc"

namespace mdbg {
namespace {

std::optional<std::vector<LocalDiscoveryEntry>> discover_snapshot_unit(
    const DebugSections& sections, std::uint64_t virtual_pc,
    std::size_t unit_start, std::size_t& next_unit) {
  std::uint16_t unit_version = 0;
  const auto dies = parse_unit_dies(sections, unit_start, next_unit, unit_version);
  (void)unit_version;

  std::optional<std::size_t> subprogram;
  for (std::size_t index = 0; index < dies.size(); ++index) {
    if (dies[index].tag != kDwTagSubprogram ||
        !die_contains_pc(dies[index], virtual_pc)) {
      continue;
    }
    if (subprogram) {
      throw std::runtime_error(
          "snapshot local discovery PC matches multiple DWARF subprograms");
    }
    subprogram = index;
  }
  if (!subprogram) return std::nullopt;

  struct Candidate {
    std::size_t depth;
    LocalDiscoveryKind kind;
  };

  constexpr std::size_t kMaxDiscoveredLocals = 64;
  std::map<std::string, Candidate> selected;
  for (std::size_t index = 0; index < dies.size(); ++index) {
    const bool variable = dies[index].tag == kDwTagVariable;
    const bool parameter = dies[index].tag == kDwTagFormalParameter;
    if (!variable && !parameter) continue;

    const auto* name = attribute_with_abstract_origin(dies, index, kDwAtName);
    if (name == nullptr || name->text.empty()) continue;
    const auto depth = active_scope_depth(dies, index, *subprogram, virtual_pc);
    if (!depth) continue;

    const auto kind = parameter ? LocalDiscoveryKind::FormalParameter
                                : LocalDiscoveryKind::Variable;
    const auto existing = selected.find(name->text);
    if (existing == selected.end()) {
      if (selected.size() >= kMaxDiscoveredLocals) {
        throw std::runtime_error(
            "active scoped-local catalogue exceeds the bounded 64-name limit");
      }
      selected.emplace(name->text, Candidate{*depth, kind});
      continue;
    }
    if (*depth > existing->second.depth) {
      existing->second = Candidate{*depth, kind};
      continue;
    }
    if (*depth == existing->second.depth) {
      throw std::runtime_error(
          "ambiguous active local name at equal lexical depth: " + name->text);
    }
  }

  std::vector<LocalDiscoveryEntry> result;
  result.reserve(selected.size());
  for (const auto& [name, candidate] : selected) {
    result.push_back(LocalDiscoveryEntry{name, candidate.kind});
  }
  return result;
}

}  // namespace

std::vector<LocalDiscoveryEntry> discover_local_values(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    const SnapshotModulePathResolver& module_paths) {
  validate_snapshot_inspection_frame(snapshot, frame);
  validate_snapshot_frame_lookup_pc(frame);

  const auto lookup_runtime_pc = snapshot_frame_lookup_pc(frame);
  const auto owner =
      resolve_snapshot_module_address(snapshot, lookup_runtime_pc, module_paths);
  if (owner.module_path != frame.module_path) {
    throw std::logic_error("snapshot local discovery module ownership changed");
  }
  const auto sections =
      read_debug_sections(module_paths.resolve_debug_file(owner.module_path));

  std::size_t unit = 0;
  while (unit < sections.info.size()) {
    std::size_t next = unit;
    const auto result =
        discover_snapshot_unit(sections, owner.virtual_address, unit, next);
    if (result) return *result;
    if (next <= unit) {
      throw std::runtime_error("DWARF parser did not advance to the next unit");
    }
    unit = next;
  }
  throw std::runtime_error(
      "snapshot local discovery PC is not covered by a supported DWARF4/5 subprogram");
}

}  // namespace mdbg

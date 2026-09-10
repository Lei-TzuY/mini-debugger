#include "dwarf/inline_context.hpp"
#include "dwarf/source_lookup_snapshot_impl.inc"

namespace mdbg {
namespace {

constexpr std::uint64_t kInlineDwAtRanges = 0x55;
constexpr std::uint64_t kInlineDwAtCallColumn = 0x57;
constexpr std::uint64_t kInlineDwAtCallFile = 0x58;
constexpr std::uint64_t kInlineDwAtCallLine = 0x59;
constexpr std::uint8_t kInlineDwOpRdx =
    static_cast<std::uint8_t>(kDwOpReg0 + 1U);
constexpr std::uint8_t kInlineDwOpRcx =
    static_cast<std::uint8_t>(kDwOpReg0 + 2U);
constexpr std::size_t kMaxInlineContexts = 8;
constexpr std::size_t kMaxInlineRangeEntries = 64;
constexpr std::size_t kMaxDiscoveredLocals = 64;

std::vector<std::byte> read_debug_ranges(const std::string& path) {
  const auto sections = read_named_sections(path, {".debug_ranges"});
  const auto ranges = sections.find(".debug_ranges");
  return ranges == sections.end() ? std::vector<std::byte>{} : ranges->second;
}

std::uint64_t compilation_unit_base(const std::vector<Die>& dies) {
  if (dies.empty()) return 0;
  const auto* low = attribute(dies.front(), kDwAtLowPc);
  if (low == nullptr) return 0;
  if (low->form != kDwFormAddr) {
    throw std::runtime_error("compilation-unit DW_AT_low_pc is not DW_FORM_addr");
  }
  return low->number;
}

bool bounded_range_list_contains_pc(const Die& die, std::uint64_t pc,
                                    const std::vector<std::byte>& ranges,
                                    std::uint64_t initial_base,
                                    std::uint16_t unit_version) {
  const auto* range = attribute(die, kInlineDwAtRanges);
  if (range == nullptr) return false;
  if (unit_version != 4) {
    throw std::runtime_error(
        "inline DW_AT_ranges is currently bounded to DWARF4 .debug_ranges");
  }
  if (range->form != kDwFormSecOffset && range->form != kDwFormData4) {
    throw std::runtime_error(
        "inline DW_AT_ranges does not use a supported section-offset form");
  }
  if (ranges.empty() || range->number > ranges.size()) {
    throw std::runtime_error("inline DW_AT_ranges references unavailable .debug_ranges data");
  }

  std::size_t cursor = static_cast<std::size_t>(range->number);
  std::uint64_t base = initial_base;
  for (std::size_t entry = 0; entry < kMaxInlineRangeEntries; ++entry) {
    const auto begin = read_scalar<std::uint64_t>(ranges, cursor, ranges.size(),
                                                  "DWARF4 range begin");
    const auto end = read_scalar<std::uint64_t>(ranges, cursor, ranges.size(),
                                                "DWARF4 range end");
    if (begin == 0 && end == 0) return false;
    if (begin == std::numeric_limits<std::uint64_t>::max()) {
      base = end;
      continue;
    }
    if (end < begin) {
      throw std::runtime_error("DWARF4 inline range has descending endpoints");
    }
    const auto absolute_begin = add_unsigned(base, begin, "DWARF4 range begin");
    const auto absolute_end = add_unsigned(base, end, "DWARF4 range end");
    if (pc >= absolute_begin && pc < absolute_end) return true;
  }
  throw std::runtime_error("DWARF4 inline range list exceeds the bounded 64-entry limit");
}

bool inline_scope_contains_pc(const Die& die, std::uint64_t pc,
                              const std::vector<std::byte>& ranges,
                              std::uint64_t unit_base, std::uint16_t unit_version) {
  const auto* low = attribute(die, kDwAtLowPc);
  const auto* high = attribute(die, kDwAtHighPc);
  if (low != nullptr || high != nullptr) {
    if (low == nullptr || high == nullptr || low->form != kDwFormAddr) {
      throw std::runtime_error(
          "inline scope has an incomplete DW_AT_low_pc/DW_AT_high_pc range");
    }
    return die_contains_pc(die, pc);
  }
  return bounded_range_list_contains_pc(die, pc, ranges, unit_base, unit_version);
}

std::optional<std::size_t> find_physical_subprogram(
    const std::vector<Die>& dies, std::uint64_t pc,
    const std::vector<std::byte>& ranges, std::uint64_t unit_base,
    std::uint16_t unit_version) {
  std::optional<std::size_t> result;
  for (std::size_t index = 0; index < dies.size(); ++index) {
    if (dies[index].tag != kDwTagSubprogram ||
        !inline_scope_contains_pc(dies[index], pc, ranges, unit_base, unit_version)) {
      continue;
    }
    if (result) {
      throw std::runtime_error(
          "snapshot inline discovery PC matches multiple physical DWARF subprograms");
    }
    result = index;
  }
  return result;
}

bool crosses_inline_scope(const std::vector<Die>& dies, std::size_t value_index,
                          std::size_t subprogram) {
  auto parent = dies[value_index].parent;
  while (parent && *parent != subprogram) {
    if (dies[*parent].tag == kDwTagInlinedSubroutine) return true;
    parent = dies[*parent].parent;
  }
  return false;
}

std::optional<std::size_t> inline_depth_from_physical(
    const std::vector<Die>& dies, std::size_t inline_index,
    std::size_t subprogram) {
  std::size_t depth = 1;
  auto parent = dies[inline_index].parent;
  while (parent) {
    if (*parent == subprogram) return depth;
    if (dies[*parent].tag == kDwTagInlinedSubroutine) ++depth;
    parent = dies[*parent].parent;
  }
  return std::nullopt;
}

std::optional<std::size_t> inline_local_scope_depth(
    const std::vector<Die>& dies, std::size_t value_index,
    std::size_t inline_index, std::uint64_t pc,
    const std::vector<std::byte>& ranges, std::uint64_t unit_base,
    std::uint16_t unit_version) {
  auto parent = dies[value_index].parent;
  std::size_t depth = 0;
  while (parent) {
    if (*parent == inline_index) return depth;
    const auto& scope = dies[*parent];
    if (scope.tag == kDwTagInlinedSubroutine) return std::nullopt;
    if (scope.tag != kDwTagLexicalBlock) return std::nullopt;
    if (!inline_scope_contains_pc(scope, pc, ranges, unit_base, unit_version)) {
      return std::nullopt;
    }
    ++depth;
    parent = scope.parent;
  }
  return std::nullopt;
}

std::optional<std::vector<InlineCallsiteContext>> discover_inline_unit(
    const DebugSections& sections, const std::vector<std::byte>& ranges,
    std::uint64_t virtual_pc, const std::string& module_path,
    const std::string& debug_path, std::size_t unit_start, std::size_t& next_unit) {
  std::uint16_t unit_version = 0;
  const auto dies = parse_unit_dies(sections, unit_start, next_unit, unit_version);
  const auto unit_base = compilation_unit_base(dies);
  const auto subprogram =
      find_physical_subprogram(dies, virtual_pc, ranges, unit_base, unit_version);
  if (!subprogram) return std::nullopt;

  struct Candidate {
    std::size_t index;
    std::size_t depth;
  };
  std::vector<Candidate> candidates;
  for (std::size_t index = 0; index < dies.size(); ++index) {
    if (dies[index].tag != kDwTagInlinedSubroutine ||
        !is_descendant_of(dies, index, *subprogram) ||
        !inline_scope_contains_pc(dies[index], virtual_pc, ranges, unit_base,
                                  unit_version)) {
      continue;
    }
    const auto depth = inline_depth_from_physical(dies, index, *subprogram);
    if (!depth) {
      throw std::logic_error("inline context lost physical-subprogram ownership");
    }
    if (*depth > kMaxInlineContexts) {
      throw std::runtime_error("inline call chain exceeds the bounded 8-context limit");
    }
    candidates.push_back(Candidate{index, *depth});
  }
  if (candidates.empty()) return std::vector<InlineCallsiteContext>{};
  if (candidates.size() > kMaxInlineContexts) {
    throw std::runtime_error("inline call chain exceeds the bounded 8-context limit");
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) {
              return left.depth < right.depth;
            });
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (candidates[index].depth != index + 1) {
      throw std::runtime_error("active inline contexts do not form one contiguous chain");
    }
    if (index > 0 &&
        !is_descendant_of(dies, candidates[index].index,
                          candidates[index - 1].index)) {
      throw std::runtime_error("active inline contexts are ambiguous sibling scopes");
    }
  }

  const DwarfLineTable line_table(debug_path);
  if (!line_table.find_virtual_address(virtual_pc)) {
    throw std::runtime_error("inline context has no supported source file ownership");
  }

  std::vector<InlineCallsiteContext> result;
  result.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    const auto* name =
        attribute_with_abstract_origin(dies, candidate.index, kDwAtName);
    const auto* call_file = attribute(dies[candidate.index], kInlineDwAtCallFile);
    const auto* call_line = attribute(dies[candidate.index], kInlineDwAtCallLine);
    const auto* call_column = attribute(dies[candidate.index], kInlineDwAtCallColumn);
    if (name == nullptr || name->text.empty() || call_file == nullptr ||
        call_line == nullptr || call_file->number == 0 || call_line->number == 0) {
      throw std::runtime_error(
          "active inline context lacks bounded name/call-site metadata");
    }
    const auto call_file_path =
        line_table.find_virtual_file(virtual_pc, call_file->number);
    if (!call_file_path) {
      throw std::runtime_error(
          "active inline context call-site file index is unavailable in the owning line table");
    }
    result.push_back(InlineCallsiteContext{
        dies[candidate.index].offset, candidate.depth - 1, module_path, name->text,
        SourceLocation{*call_file_path, call_line->number,
                       call_column == nullptr ? 0 : call_column->number}});
  }
  return result;
}

std::optional<std::vector<LocalDiscoveryEntry>> discover_snapshot_unit(
    const DebugSections& sections, const std::vector<std::byte>& ranges,
    std::uint64_t virtual_pc, std::size_t unit_start, std::size_t& next_unit) {
  std::uint16_t unit_version = 0;
  const auto dies = parse_unit_dies(sections, unit_start, next_unit, unit_version);
  const auto unit_base = compilation_unit_base(dies);
  const auto subprogram =
      find_physical_subprogram(dies, virtual_pc, ranges, unit_base, unit_version);
  if (!subprogram) return std::nullopt;

  struct Candidate {
    std::size_t depth;
    LocalDiscoveryKind kind;
  };

  std::map<std::string, Candidate> selected;
  for (std::size_t index = 0; index < dies.size(); ++index) {
    const bool variable = dies[index].tag == kDwTagVariable;
    const bool parameter = dies[index].tag == kDwTagFormalParameter;
    if (!variable && !parameter) continue;
    if (crosses_inline_scope(dies, index, *subprogram)) continue;

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

std::optional<std::vector<LocalDiscoveryEntry>> discover_inline_locals_unit(
    const DebugSections& sections, const std::vector<std::byte>& ranges,
    std::uint64_t virtual_pc, std::size_t inline_die_offset,
    std::size_t unit_start, std::size_t& next_unit) {
  std::uint16_t unit_version = 0;
  const auto dies = parse_unit_dies(sections, unit_start, next_unit, unit_version);
  const auto selected_inline = die_index_by_offset(dies, inline_die_offset);
  if (!selected_inline) return std::nullopt;
  if (dies[*selected_inline].tag != kDwTagInlinedSubroutine) {
    throw std::runtime_error("selected inline context does not reference an inline DIE");
  }
  const auto unit_base = compilation_unit_base(dies);
  if (!inline_scope_contains_pc(dies[*selected_inline], virtual_pc, ranges, unit_base,
                                unit_version)) {
    throw std::runtime_error("selected inline context no longer owns the physical frame PC");
  }
  const auto subprogram =
      find_physical_subprogram(dies, virtual_pc, ranges, unit_base, unit_version);
  if (!subprogram || !is_descendant_of(dies, *selected_inline, *subprogram)) {
    throw std::runtime_error("selected inline context lost physical-frame ownership");
  }

  struct Candidate {
    std::size_t depth;
    LocalDiscoveryKind kind;
  };
  std::map<std::string, Candidate> selected;
  for (std::size_t index = 0; index < dies.size(); ++index) {
    const bool variable = dies[index].tag == kDwTagVariable;
    const bool parameter = dies[index].tag == kDwTagFormalParameter;
    if (!variable && !parameter) continue;
    const auto depth = inline_local_scope_depth(
        dies, index, *selected_inline, virtual_pc, ranges, unit_base, unit_version);
    if (!depth) continue;
    const auto* name = attribute_with_abstract_origin(dies, index, kDwAtName);
    if (name == nullptr || name->text.empty()) continue;
    const auto kind = parameter ? LocalDiscoveryKind::FormalParameter
                                : LocalDiscoveryKind::Variable;
    const auto existing = selected.find(name->text);
    if (existing == selected.end()) {
      if (selected.size() >= kMaxDiscoveredLocals) {
        throw std::runtime_error(
            "inline scoped-local catalogue exceeds the bounded 64-name limit");
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
          "ambiguous inline local name at equal lexical depth: " + name->text);
    }
  }

  std::vector<LocalDiscoveryEntry> result;
  result.reserve(selected.size());
  for (const auto& [name, candidate] : selected) {
    result.push_back(LocalDiscoveryEntry{name, candidate.kind});
  }
  return result;
}

std::optional<LocalScalarValue> inspect_inline_scalar_unit(
    const DebugSections& sections, const std::vector<std::byte>& ranges,
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    const SnapshotModulePathResolver& module_paths,
    const SnapshotModuleAddress& owner, std::uint64_t virtual_pc,
    std::size_t inline_die_offset, std::string_view requested_name,
    std::size_t unit_start, std::size_t& next_unit) {
  std::uint16_t unit_version = 0;
  const auto dies = parse_unit_dies(sections, unit_start, next_unit, unit_version);
  const auto selected_inline = die_index_by_offset(dies, inline_die_offset);
  if (!selected_inline) return std::nullopt;
  if (dies[*selected_inline].tag != kDwTagInlinedSubroutine) {
    throw std::runtime_error("selected inline context does not reference an inline DIE");
  }

  const auto unit_base = compilation_unit_base(dies);
  if (!inline_scope_contains_pc(dies[*selected_inline], virtual_pc, ranges, unit_base,
                                unit_version)) {
    throw std::runtime_error("selected inline context no longer owns the physical frame PC");
  }
  const auto subprogram =
      find_physical_subprogram(dies, virtual_pc, ranges, unit_base, unit_version);
  if (!subprogram || !is_descendant_of(dies, *selected_inline, *subprogram)) {
    throw std::runtime_error("selected inline context lost physical-frame ownership");
  }

  std::optional<std::size_t> value_index;
  std::optional<std::size_t> best_depth;
  bool owned_name = false;
  for (std::size_t index = 0; index < dies.size(); ++index) {
    const bool variable = dies[index].tag == kDwTagVariable;
    const bool parameter = dies[index].tag == kDwTagFormalParameter;
    if (!variable && !parameter) continue;
    const auto* name = attribute_with_abstract_origin(dies, index, kDwAtName);
    if (name == nullptr || name->text != requested_name) continue;
    const auto depth = inline_local_scope_depth(
        dies, index, *selected_inline, virtual_pc, ranges, unit_base, unit_version);
    if (!depth) continue;
    owned_name = true;
    if (!value_index || *depth > *best_depth) {
      value_index = index;
      best_depth = *depth;
      continue;
    }
    if (*depth == *best_depth) {
      throw std::runtime_error(
          "ambiguous selected-inline local at equal lexical depth: " +
          std::string(requested_name));
    }
  }
  if (!value_index) {
    throw std::runtime_error(
        owned_name ? "selected-inline local has no active value binding: " +
                         std::string(requested_name)
                   : "local value is not owned by the selected inline context: " +
                         std::string(requested_name));
  }

  const auto& value_die = dies[*value_index];
  const auto* location = attribute(value_die, kDwAtLocation);
  const auto* type = attribute_with_abstract_origin(dies, *value_index, kDwAtType);
  if (location == nullptr) {
    throw std::runtime_error("selected-inline local has no DW_AT_location");
  }
  if (type == nullptr || type->form != kDwFormRef4) {
    throw std::runtime_error(
        "selected-inline local has no supported DW_FORM_ref4 type");
  }

  const auto value_type = resolve_value_type(dies, type->number);
  if (value_type.kind != LocalValueKind::Integer || value_type.byte_size == 0 ||
      value_type.byte_size > sizeof(std::uint64_t)) {
    throw std::runtime_error(
        "selected-inline scalar materialization requires a bounded integer value");
  }

  std::vector<std::byte> expression;
  if (location->form == kDwFormExprloc) {
    expression = location->expression;
  } else if (location->form == kDwFormSecOffset || location->form == kDwFormLoclistx) {
    expression = active_location_expression(sections, location->number, virtual_pc,
                                            unit_base, unit_version);
  } else {
    throw std::runtime_error(
        "selected-inline scalar has no supported DW_AT_location form");
  }
  if (expression.empty()) {
    throw std::runtime_error(
        "selected-inline scalar requires one exact compiler-proven location operation");
  }

  const auto opcode = std::to_integer<std::uint8_t>(expression.front());
  if (frame.index != 0) {
    if (opcode != kDwOpFbreg) {
      throw std::runtime_error(
          "caller-frame selected-inline scalar currently requires compiler-proven DW_OP_fbreg");
    }
    const auto base = snapshot_frame_base(
        dies, *subprogram, snapshot, frame, module_paths, owner);
    const auto runtime_address = add_signed(
        base, decode_snapshot_fbreg_offset(expression),
        "caller-frame selected-inline DW_OP_fbreg runtime address");
    if (runtime_address > std::numeric_limits<std::uintptr_t>::max()) {
      throw std::overflow_error(
          "caller-frame selected-inline DW_OP_fbreg address exceeds runtime address width");
    }
    const auto memory = read_snapshot_memory(
        snapshot, module_paths, static_cast<std::uintptr_t>(runtime_address),
        value_type.byte_size);
    return materialize_snapshot_memory_value(owner, requested_name, value_type, memory);
  }

  if (expression.size() != 1) {
    throw std::runtime_error(
        "selected-inline scalar requires one exact compiler-proven register operation");
  }
  const auto& regs = snapshot.thread(frame.thread_tid).registers;
  std::uint64_t raw = 0;
  if (opcode == kInlineDwOpRdx) {
    raw = regs.rdx;
  } else if (opcode == kInlineDwOpRcx) {
    raw = regs.rcx;
  } else {
    throw std::runtime_error(
        "selected-inline scalar currently requires compiler-proven DW_OP_reg1 (rdx) or "
        "DW_OP_reg2 (rcx)");
  }

  LocalScalarValue result{owner.module_path, std::string(requested_name),
                          truncate_integer(raw, value_type.byte_size),
                          value_type.byte_size, value_type.is_signed,
                          LocalValueKind::Integer};
  result.storage = LocalValueStorage::SnapshotCoreRegister;
  return result;
}

}  // namespace

std::vector<InlineCallsiteContext> discover_inline_call_chain(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    const SnapshotModulePathResolver& module_paths) {
  validate_snapshot_inspection_frame(snapshot, frame);
  validate_snapshot_frame_lookup_pc(frame);
  const auto lookup_runtime_pc = snapshot_frame_lookup_pc(frame);
  const auto owner =
      resolve_snapshot_module_address(snapshot, lookup_runtime_pc, module_paths);
  if (owner.module_path != frame.module_path) {
    throw std::logic_error("snapshot inline discovery module ownership changed");
  }
  const auto debug_path = module_paths.resolve_debug_file(owner.module_path);
  const auto sections = read_debug_sections(debug_path);
  const auto ranges = read_debug_ranges(debug_path);

  std::size_t unit = 0;
  while (unit < sections.info.size()) {
    std::size_t next = unit;
    const auto result = discover_inline_unit(sections, ranges, owner.virtual_address,
                                             owner.module_path, debug_path, unit, next);
    if (result) return *result;
    if (next <= unit) {
      throw std::runtime_error("DWARF inline parser did not advance to the next unit");
    }
    unit = next;
  }
  return {};
}

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
  const auto debug_path = module_paths.resolve_debug_file(owner.module_path);
  const auto sections = read_debug_sections(debug_path);
  const auto ranges = read_debug_ranges(debug_path);

  std::size_t unit = 0;
  while (unit < sections.info.size()) {
    std::size_t next = unit;
    const auto result =
        discover_snapshot_unit(sections, ranges, owner.virtual_address, unit, next);
    if (result) return *result;
    if (next <= unit) {
      throw std::runtime_error("DWARF parser did not advance to the next unit");
    }
    unit = next;
  }
  throw std::runtime_error(
      "snapshot local discovery PC is not covered by a supported DWARF4/5 subprogram");
}

std::vector<LocalDiscoveryEntry> discover_inline_local_values(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::size_t inline_die_offset, const SnapshotModulePathResolver& module_paths) {
  validate_snapshot_inspection_frame(snapshot, frame);
  validate_snapshot_frame_lookup_pc(frame);
  const auto lookup_runtime_pc = snapshot_frame_lookup_pc(frame);
  const auto owner =
      resolve_snapshot_module_address(snapshot, lookup_runtime_pc, module_paths);
  if (owner.module_path != frame.module_path) {
    throw std::logic_error("snapshot inline local discovery module ownership changed");
  }
  const auto debug_path = module_paths.resolve_debug_file(owner.module_path);
  const auto sections = read_debug_sections(debug_path);
  const auto ranges = read_debug_ranges(debug_path);

  std::size_t unit = 0;
  while (unit < sections.info.size()) {
    std::size_t next = unit;
    const auto result = discover_inline_locals_unit(
        sections, ranges, owner.virtual_address, inline_die_offset, unit, next);
    if (result) return *result;
    if (next <= unit) {
      throw std::runtime_error("DWARF inline-local parser did not advance to the next unit");
    }
    unit = next;
  }
  throw std::runtime_error("selected inline DIE is unavailable in the owning debug file");
}

LocalScalarValue inspect_inline_local_value(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::size_t inline_die_offset, std::string_view name,
    const SnapshotModulePathResolver& module_paths) {
  if (name.empty()) {
    throw std::invalid_argument("selected-inline local name must not be empty");
  }
  validate_snapshot_inspection_frame(snapshot, frame);
  validate_snapshot_frame_lookup_pc(frame);

  const auto lookup_runtime_pc = snapshot_frame_lookup_pc(frame);
  const auto owner =
      resolve_snapshot_module_address(snapshot, lookup_runtime_pc, module_paths);
  if (owner.module_path != frame.module_path) {
    throw std::logic_error("selected-inline scalar module ownership changed");
  }
  const auto debug_path = module_paths.resolve_debug_file(owner.module_path);
  const auto sections = read_debug_sections(debug_path);
  const auto ranges = read_debug_ranges(debug_path);

  std::size_t unit = 0;
  while (unit < sections.info.size()) {
    std::size_t next = unit;
    const auto result = inspect_inline_scalar_unit(
        sections, ranges, snapshot, frame, module_paths, owner,
        owner.virtual_address, inline_die_offset, name, unit, next);
    if (result) return *result;
    if (next <= unit) {
      throw std::runtime_error(
          "DWARF selected-inline scalar parser did not advance to the next unit");
    }
    unit = next;
  }
  throw std::runtime_error("selected inline DIE is unavailable in the owning debug file");
}

}  // namespace mdbg

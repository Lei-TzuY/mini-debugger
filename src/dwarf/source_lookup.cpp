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
constexpr std::uint64_t kInlineDwTagArrayType = 0x01;
constexpr std::uint64_t kInlineDwTagUnionType = 0x17;
constexpr std::uint64_t kInlineDwTagSubrangeType = 0x21;
constexpr std::uint64_t kInlineDwAtLowerBound = 0x22;
constexpr std::uint64_t kInlineDwAtUpperBound = 0x2f;
constexpr std::uint64_t kInlineDwAtCount = 0x37;
constexpr std::uint64_t kInlineDwAtBitOffset = 0x0c;
constexpr std::uint64_t kInlineDwAtBitSize = 0x0d;
constexpr std::uint64_t kInlineDwAtDataBitOffset = 0x6b;
constexpr std::size_t kMaxSelectedInlineArrayElements = 64;

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

std::optional<LocalBitSlice> selected_inline_bit_slice(
    const Die& member, const IntegerType& integer, std::size_t struct_size) {
  const auto* bit_size = attribute(member, kInlineDwAtBitSize);
  const auto* bit_offset = attribute(member, kInlineDwAtBitOffset);
  const auto* data_bit_offset = attribute(member, kInlineDwAtDataBitOffset);
  const auto* member_offset = attribute(member, kDwAtDataMemberLocation);
  if (bit_size == nullptr) {
    if (bit_offset != nullptr || data_bit_offset != nullptr) {
      throw std::runtime_error(
          "selected-inline structure member has bit location without bit size");
    }
    return std::nullopt;
  }
  if (!is_constant_member_offset_form(bit_size->form) || bit_size->number == 0 ||
      bit_size->number > integer.byte_size * 8U) {
    throw std::runtime_error(
        "selected-inline structure bit-field has unsupported compiler width");
  }
  if ((bit_offset == nullptr) == (data_bit_offset == nullptr)) {
    throw std::runtime_error(
        "selected-inline structure bit-field requires exactly one compiler bit location");
  }

  const auto width = static_cast<std::size_t>(bit_size->number);
  std::size_t absolute_bit_offset = 0;
  if (data_bit_offset != nullptr) {
    if (member_offset != nullptr ||
        !is_constant_member_offset_form(data_bit_offset->form) ||
        data_bit_offset->number > std::numeric_limits<std::size_t>::max()) {
      throw std::runtime_error(
          "selected-inline structure DW_AT_data_bit_offset is unsupported");
    }
    absolute_bit_offset = static_cast<std::size_t>(data_bit_offset->number);
  } else {
    if (member_offset == nullptr ||
        !is_constant_member_offset_form(member_offset->form) ||
        !is_constant_member_offset_form(bit_offset->form) ||
        member_offset->number > struct_size) {
      throw std::runtime_error(
          "selected-inline structure legacy bit-field has unsupported storage offset");
    }
    const auto storage_bits = integer.byte_size * 8U;
    if (bit_offset->number > storage_bits ||
        bit_size->number > storage_bits - bit_offset->number) {
      throw std::runtime_error(
          "selected-inline structure legacy bit-field exceeds its base storage unit");
    }
    const auto byte_offset = static_cast<std::size_t>(member_offset->number);
    absolute_bit_offset = byte_offset * 8U +
                          (storage_bits - static_cast<std::size_t>(bit_offset->number) - width);
  }

  const auto aggregate_bits = struct_size * 8U;
  if (absolute_bit_offset > aggregate_bits ||
      width > aggregate_bits - absolute_bit_offset) {
    throw std::runtime_error(
        "selected-inline structure bit-field exceeds aggregate storage");
  }
  return LocalBitSlice{absolute_bit_offset, width};
}

std::optional<LocalPointeeType> selected_inline_direct_structure_type(
    const std::vector<Die>& dies, std::uint64_t type_offset) {
  for (unsigned depth = 0; depth < 16; ++depth) {
    const auto index = die_index_by_offset(dies, type_offset);
    if (!index) {
      throw std::runtime_error("selected-inline local type references an unknown DIE");
    }
    const auto& die = dies[*index];
    if (die.tag == kDwTagTypedef || die.tag == kDwTagConstType) {
      const auto* wrapped = attribute(die, kDwAtType);
      if (wrapped == nullptr || wrapped->form != kDwFormRef4) {
        throw std::runtime_error(
            "selected-inline local type wrapper does not use DW_FORM_ref4");
      }
      type_offset = wrapped->number;
      continue;
    }
    if (die.tag != kDwTagStructureType) return std::nullopt;

    const auto* size = attribute(die, kDwAtByteSize);
    if (size == nullptr || size->number == 0 || size->number > kMaxLocalStructSize) {
      throw std::runtime_error("selected-inline structure has an unsupported byte size");
    }
    const auto struct_size = static_cast<std::size_t>(size->number);
    LocalPointeeType result{struct_size, false, LocalValueKind::Structure, {}};
    for (std::size_t child = 0; child < dies.size(); ++child) {
      if (dies[child].parent != *index) continue;
      if (dies[child].tag != kDwTagMember) {
        throw std::runtime_error(
            "selected-inline structure has an unsupported direct child DIE");
      }
      if (result.members.size() >= kMaxLocalStructMembers) {
        throw std::runtime_error("selected-inline structure has too many direct members");
      }

      const auto& member_die = dies[child];
      const bool bit_field = attribute(member_die, kInlineDwAtBitSize) != nullptr ||
                             attribute(member_die, kInlineDwAtBitOffset) != nullptr ||
                             attribute(member_die, kInlineDwAtDataBitOffset) != nullptr;
      LocalStructMemberType member;
      if (!bit_field) {
        member = resolve_snapshot_struct_member_type(dies, member_die, struct_size);
      } else {
        const auto* member_name = attribute(member_die, kDwAtName);
        const auto* member_type = attribute(member_die, kDwAtType);
        if (member_name == nullptr || member_name->text.empty()) {
          throw std::runtime_error("selected-inline structure bit-field has no supported name");
        }
        if (member_type == nullptr || member_type->form != kDwFormRef4) {
          throw std::runtime_error(
              "selected-inline structure bit-field has no supported DW_FORM_ref4 type");
        }
        const auto integer = resolve_integer_type(dies, member_type->number);
        const auto slice = selected_inline_bit_slice(member_die, integer, struct_size);
        if (!slice) {
          throw std::logic_error("selected-inline bit-field lost compiler bit-slice metadata");
        }
        member = LocalStructMemberType{member_name->text,
                                       slice->bit_offset / 8U,
                                       integer.byte_size,
                                       integer.is_signed,
                                       LocalValueKind::Integer,
                                       {},
                                       slice};
      }

      const auto duplicate = std::find_if(
          result.members.begin(), result.members.end(),
          [&member](const LocalStructMemberType& existing) {
            return existing.name == member.name;
          });
      if (duplicate != result.members.end()) {
        throw std::runtime_error("selected-inline structure has duplicate member names");
      }
      result.members.push_back(std::move(member));
    }
    if (result.members.empty()) {
      throw std::runtime_error("selected-inline structure has no supported direct members");
    }
    return result;
  }
  throw std::runtime_error("selected-inline local type chain is too deep");
}

LocalStructMemberType selected_inline_union_member_type(
    const std::vector<Die>& dies, const Die& member, std::size_t union_size) {
  const auto* member_name = attribute(member, kDwAtName);
  const auto* member_type = attribute(member, kDwAtType);
  const auto* member_offset = attribute(member, kDwAtDataMemberLocation);
  if (member_name == nullptr || member_name->text.empty()) {
    throw std::runtime_error("selected-inline union member has no supported name");
  }
  if (member_type == nullptr || member_type->form != kDwFormRef4) {
    throw std::runtime_error(
        "selected-inline union member has no supported DW_FORM_ref4 type");
  }
  if (member_offset != nullptr &&
      (!is_constant_member_offset_form(member_offset->form) ||
       member_offset->number != 0)) {
    throw std::runtime_error(
        "selected-inline union member does not share storage at offset zero");
  }

  std::uint64_t type_offset = member_type->number;
  for (unsigned depth = 0; depth < 16; ++depth) {
    const auto index = die_index_by_offset(dies, type_offset);
    if (!index) {
      throw std::runtime_error(
"selected-inline union member type references an unknown DIE");
    }
    const auto& type_die = dies[*index];
    if (type_die.tag == kDwTagTypedef || type_die.tag == kDwTagConstType) {
      const auto* wrapped = attribute(type_die, kDwAtType);
      if (wrapped == nullptr || wrapped->form != kDwFormRef4) {
        throw std::runtime_error(
  "selected-inline union member wrapper does not use DW_FORM_ref4");
      }
      type_offset = wrapped->number;
      continue;
    }
    if (type_die.tag != kDwTagBaseType) {
      throw std::runtime_error(
"selected-inline union member is not a bounded integer scalar");
    }
    const auto integer = resolve_integer_type(dies, type_offset);
    if (integer.byte_size == 0 || integer.byte_size > sizeof(std::uint64_t) ||
        integer.byte_size > union_size) {
      throw std::runtime_error(
"selected-inline union integer member exceeds bounded shared storage");
    }
    return LocalStructMemberType{member_name->text, 0, integer.byte_size,
                       integer.is_signed};
  }
  throw std::runtime_error("selected-inline union member type chain is too deep");
}

std::optional<LocalPointeeType> selected_inline_direct_union_type(
    const std::vector<Die>& dies, std::uint64_t type_offset) {
  for (unsigned depth = 0; depth < 16; ++depth) {
    const auto index = die_index_by_offset(dies, type_offset);
    if (!index) {
      throw std::runtime_error("selected-inline union type references an unknown DIE");
    }
    const auto& die = dies[*index];
    if (die.tag == kDwTagTypedef || die.tag == kDwTagConstType) {
      const auto* wrapped = attribute(die, kDwAtType);
      if (wrapped == nullptr || wrapped->form != kDwFormRef4) {
        throw std::runtime_error(
  "selected-inline union wrapper does not use DW_FORM_ref4");
      }
      type_offset = wrapped->number;
      continue;
    }
    if (die.tag != kInlineDwTagUnionType) return std::nullopt;

    const auto* size = attribute(die, kDwAtByteSize);
    if (size == nullptr || size->number == 0 || size->number > kMaxLocalStructSize) {
      throw std::runtime_error("selected-inline union has an unsupported byte size");
    }
    const auto union_size = static_cast<std::size_t>(size->number);
    LocalPointeeType result{union_size, false, LocalValueKind::Union, {}};
    for (std::size_t child = 0; child < dies.size(); ++child) {
      if (dies[child].parent != *index) continue;
      if (dies[child].tag != kDwTagMember) {
        throw std::runtime_error(
  "selected-inline union has an unsupported direct child DIE");
      }
      if (result.members.size() >= kMaxLocalStructMembers) {
        throw std::runtime_error("selected-inline union has too many direct members");
      }
      auto resolved = selected_inline_union_member_type(dies, dies[child], union_size);
      const auto duplicate = std::find_if(
result.members.begin(), result.members.end(),
[&resolved](const LocalStructMemberType& existing) {
  return existing.name == resolved.name;
});
      if (duplicate != result.members.end()) {
        throw std::runtime_error("selected-inline union has duplicate member names");
      }
      result.members.push_back(std::move(resolved));
    }
    if (result.members.empty()) {
      throw std::runtime_error("selected-inline union has no supported direct members");
    }
    return result;
  }
  throw std::runtime_error("selected-inline union type chain is too deep");
}

struct SelectedInlineFixedArrayType {
  std::size_t element_count;
  std::size_t element_byte_size;
  bool element_is_signed;
  std::size_t byte_size;
};

bool selected_inline_array_bound_form(std::uint64_t form) {
  return form == kDwFormData1 || form == kDwFormData2 ||
         form == kDwFormData4 || form == kDwFormData8 ||
         form == kDwFormUdata || form == kDwFormImplicitConst;
}

std::optional<SelectedInlineFixedArrayType> selected_inline_fixed_array_type(
    const std::vector<Die>& dies, std::uint64_t type_offset) {
  for (unsigned depth = 0; depth < 16; ++depth) {
    const auto index = die_index_by_offset(dies, type_offset);
    if (!index) {
      throw std::runtime_error(
          "selected-inline fixed-array type references an unknown DIE");
    }
    const auto& die = dies[*index];
    if (die.tag == kDwTagTypedef || die.tag == kDwTagConstType) {
      const auto* wrapped = attribute(die, kDwAtType);
      if (wrapped == nullptr || wrapped->form != kDwFormRef4) {
        throw std::runtime_error(
            "selected-inline fixed-array wrapper does not use DW_FORM_ref4");
      }
      type_offset = wrapped->number;
      continue;
    }
    if (die.tag != kInlineDwTagArrayType) return std::nullopt;

    const auto* element_ref = attribute(die, kDwAtType);
    if (element_ref == nullptr || element_ref->form != kDwFormRef4) {
      throw std::runtime_error(
          "selected-inline fixed array has no supported element type");
    }
    const auto element = resolve_integer_type(dies, element_ref->number);

    std::optional<std::size_t> subrange;
    for (std::size_t child = 0; child < dies.size(); ++child) {
      if (dies[child].parent != *index) continue;
      if (dies[child].tag != kInlineDwTagSubrangeType || subrange) {
        throw std::runtime_error(
            "selected-inline fixed array requires exactly one direct subrange");
      }
      subrange = child;
    }
    if (!subrange) {
      throw std::runtime_error(
          "selected-inline fixed array has no direct subrange");
    }

    const auto& range = dies[*subrange];
    const auto* lower = attribute(range, kInlineDwAtLowerBound);
    if (lower != nullptr &&
        (!selected_inline_array_bound_form(lower->form) || lower->number != 0)) {
      throw std::runtime_error(
          "selected-inline fixed array requires a zero lower bound");
    }
    const auto* count = attribute(range, kInlineDwAtCount);
    const auto* upper = attribute(range, kInlineDwAtUpperBound);
    if (count != nullptr && !selected_inline_array_bound_form(count->form)) {
      throw std::runtime_error(
          "selected-inline fixed-array count has an unsupported form");
    }
    if (upper != nullptr && !selected_inline_array_bound_form(upper->form)) {
      throw std::runtime_error(
          "selected-inline fixed-array upper bound has an unsupported form");
    }
    std::uint64_t element_count = 0;
    if (count != nullptr) {
      element_count = count->number;
      if (upper != nullptr &&
          (upper->number == std::numeric_limits<std::uint64_t>::max() ||
           upper->number + 1 != element_count)) {
        throw std::runtime_error(
            "selected-inline fixed-array count conflicts with upper bound");
      }
    } else if (upper != nullptr) {
      if (upper->number == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error(
            "selected-inline fixed-array upper bound overflows element count");
      }
      element_count = upper->number + 1;
    } else {
      throw std::runtime_error(
          "selected-inline fixed array has no bounded element count");
    }
    if (element_count == 0 ||
        element_count > kMaxSelectedInlineArrayElements) {
      throw std::runtime_error(
          "selected-inline fixed-array count exceeds the bounded 64-element model");
    }
    if (element.byte_size == 0 || element.byte_size > sizeof(std::uint64_t) ||
        element_count > kMaxLocalStructSize / element.byte_size) {
      throw std::runtime_error(
          "selected-inline fixed-array storage exceeds the bounded scalar-array model");
    }
    const auto byte_size = static_cast<std::size_t>(element_count) *
                           element.byte_size;
    const auto* declared_size = attribute(die, kDwAtByteSize);
    if (declared_size != nullptr && declared_size->number != byte_size) {
      throw std::runtime_error(
          "selected-inline fixed-array byte size conflicts with its element layout");
    }
    return SelectedInlineFixedArrayType{
        static_cast<std::size_t>(element_count), element.byte_size,
        element.is_signed, byte_size};
  }
  throw std::runtime_error("selected-inline fixed-array type chain is too deep");
}

LocalScalarValue materialize_selected_inline_array(
    const SnapshotModuleAddress& owner, std::string_view name,
    const SelectedInlineFixedArrayType& array,
    const SnapshotMemoryRead& memory) {
  if (array.element_count == 0 ||
      array.element_count > kMaxSelectedInlineArrayElements ||
      array.element_byte_size == 0 ||
      array.element_byte_size > sizeof(std::uint64_t) ||
      array.byte_size != array.element_count * array.element_byte_size ||
      memory.bytes.size() != array.byte_size) {
    throw std::runtime_error(
        "selected-inline fixed-array bytes exceed the bounded array model");
  }
  LocalScalarValue result{owner.module_path, std::string(name), 0,
                          array.byte_size, false, LocalValueKind::Array};
  result.array_type = LocalArrayType{array.element_count,
                                     array.element_byte_size,
                                     array.element_is_signed,
                                     LocalValueKind::Integer};
  result.elements.reserve(array.element_count);
  for (std::size_t index = 0; index < array.element_count; ++index) {
    result.elements.push_back(LocalArrayElement{
        decode_integer(memory.bytes, index * array.element_byte_size,
                       array.element_byte_size),
        array.element_byte_size, array.element_is_signed,
        LocalValueKind::Integer});
  }
  if (memory.provenance == SnapshotMemoryProvenance::Core) {
    result.storage = LocalValueStorage::SnapshotCoreMemory;
  } else {
    if (memory.module_path != owner.module_path) {
      throw std::logic_error(
          "selected-inline fixed-array artifact ownership changed during value read");
    }
    result.storage = LocalValueStorage::SnapshotRuntimeArtifact;
    result.storage_module_path = memory.module_path;
    result.storage_file_path = memory.module_file_path;
    result.storage_file_offset = memory.artifact_file_offset;
  }
  return result;
}

LocalScalarValue materialize_selected_inline_union(
    const SnapshotModuleAddress& owner, std::string_view name,
    const LocalPointeeType& union_type, const SnapshotMemoryRead& memory) {
  if (union_type.kind != LocalValueKind::Union || union_type.byte_size == 0 ||
      union_type.byte_size > kMaxLocalStructSize || union_type.members.empty() ||
      union_type.members.size() > kMaxLocalStructMembers ||
      memory.bytes.size() != union_type.byte_size) {
    throw std::runtime_error(
        "selected-inline union bytes exceed the bounded overlapping-storage model");
  }
  LocalScalarValue result{owner.module_path, std::string(name), 0,
                union_type.byte_size, false, LocalValueKind::Union};
  result.members.reserve(union_type.members.size());
  for (const auto& member : union_type.members) {
    if (member.offset != 0 || member.kind != LocalValueKind::Integer ||
        member.pointee_type || member.byte_size == 0 ||
        member.byte_size > sizeof(std::uint64_t) ||
        member.byte_size > union_type.byte_size) {
      throw std::logic_error(
"selected-inline union member violates bounded shared-storage semantics");
    }
    result.members.push_back(LocalStructMember{
        member.name, decode_integer(memory.bytes, 0, member.byte_size),
        member.byte_size, member.is_signed, LocalValueKind::Integer, {}});
  }
  if (memory.provenance == SnapshotMemoryProvenance::Core) {
    result.storage = LocalValueStorage::SnapshotCoreMemory;
  } else {
    if (memory.module_path != owner.module_path) {
      throw std::logic_error(
"selected-inline union artifact ownership changed during value read");
    }
    result.storage = LocalValueStorage::SnapshotRuntimeArtifact;
    result.storage_module_path = memory.module_path;
    result.storage_file_path = memory.module_file_path;
    result.storage_file_offset = memory.artifact_file_offset;
  }
  return result;
}

std::uint64_t selected_inline_bit_mask(std::size_t bits) {
  if (bits == 0 || bits > 64) {
    throw std::logic_error("selected-inline bit-field width is out of range");
  }
  return bits == 64 ? std::numeric_limits<std::uint64_t>::max()
                    : (std::uint64_t{1} << bits) - 1U;
}

std::uint64_t decode_selected_inline_bit_field(
    const std::vector<std::byte>& bytes, const LocalStructMemberType& member) {
  if (!member.bit_slice || member.kind != LocalValueKind::Integer ||
      member.pointee_type || member.byte_size == 0 ||
      member.byte_size > sizeof(std::uint64_t)) {
    throw std::logic_error(
        "selected-inline bit-field member lost bounded integer metadata");
  }
  const auto slice = *member.bit_slice;
  const auto storage_bits = member.byte_size * 8U;
  if (slice.bit_size == 0 || slice.bit_size > storage_bits ||
      slice.bit_size > 64 || slice.bit_offset > bytes.size() * 8U ||
      slice.bit_size > bytes.size() * 8U - slice.bit_offset) {
    throw std::logic_error(
        "selected-inline bit-field slice exceeds immutable aggregate bytes");
  }

  std::uint64_t raw = 0;
  for (std::size_t bit = 0; bit < slice.bit_size; ++bit) {
    const auto absolute = slice.bit_offset + bit;
    const auto byte = std::to_integer<unsigned int>(bytes[absolute / 8U]);
    if (((byte >> (absolute % 8U)) & 1U) != 0) {
      raw |= std::uint64_t{1} << bit;
    }
  }
  raw &= selected_inline_bit_mask(slice.bit_size);
  if (member.is_signed && slice.bit_size < storage_bits &&
      ((raw >> (slice.bit_size - 1U)) & 1U) != 0) {
    raw |= ~selected_inline_bit_mask(slice.bit_size);
    raw &= selected_inline_bit_mask(storage_bits);
  }
  return raw;
}

LocalScalarValue materialize_selected_inline_structure(
    const SnapshotModuleAddress& owner, std::string_view name,
    const LocalPointeeType& aggregate, const SnapshotMemoryRead& memory) {
  if (aggregate.kind != LocalValueKind::Structure || aggregate.byte_size == 0 ||
      aggregate.byte_size > kMaxLocalStructSize ||
      aggregate.members.size() > kMaxLocalStructMembers ||
      memory.bytes.size() != aggregate.byte_size) {
    throw std::runtime_error(
        "selected-inline aggregate bytes exceed the bounded structure model");
  }

  LocalScalarValue result{owner.module_path, std::string(name), 0,
                          aggregate.byte_size, false,
                          LocalValueKind::Structure};
  result.members.reserve(aggregate.members.size());
  for (const auto& member : aggregate.members) {
    std::uint64_t raw = 0;
    if (member.bit_slice) {
      raw = decode_selected_inline_bit_field(memory.bytes, member);
    } else {
      if (member.byte_size == 0 || member.byte_size > sizeof(std::uint64_t) ||
          member.offset > aggregate.byte_size ||
          member.byte_size > aggregate.byte_size - member.offset) {
        throw std::logic_error(
            "selected-inline aggregate member exceeds aggregate storage");
      }
      raw = decode_integer(memory.bytes, member.offset, member.byte_size);
    }
    result.members.push_back(LocalStructMember{member.name,
                                               raw,
                                               member.byte_size,
                                               member.is_signed,
                                               member.kind,
                                               member.pointee_type,
                                               member.bit_slice});
  }

  if (memory.provenance == SnapshotMemoryProvenance::Core) {
    result.storage = LocalValueStorage::SnapshotCoreMemory;
  } else {
    if (memory.module_path != owner.module_path) {
      throw std::logic_error(
          "selected-inline aggregate artifact ownership changed during value read");
    }
    result.storage = LocalValueStorage::SnapshotRuntimeArtifact;
    result.storage_module_path = memory.module_path;
    result.storage_file_path = memory.module_file_path;
    result.storage_file_offset = memory.artifact_file_offset;
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

  const auto pointee_type = resolve_pointer_pointee_type(dies, type->number);
  const auto direct_structure =
      pointee_type ? std::optional<LocalPointeeType>{}
         : selected_inline_direct_structure_type(dies, type->number);
  const auto direct_union =
      (pointee_type || direct_structure)
? std::optional<LocalPointeeType>{}
: selected_inline_direct_union_type(dies, type->number);
  const auto direct_array =
      (pointee_type || direct_structure || direct_union)
? std::optional<SelectedInlineFixedArrayType>{}
: selected_inline_fixed_array_type(dies, type->number);
  const auto value_type =
      pointee_type
? ValueType{sizeof(std::uintptr_t), false, LocalValueKind::Pointer, {}}
: direct_structure
      ? ValueType{direct_structure->byte_size, false,
                  LocalValueKind::Structure, {}}
      : direct_union
            ? ValueType{direct_union->byte_size, false,
                        LocalValueKind::Union, {}}
            : direct_array
                  ? ValueType{direct_array->byte_size, false,
                              LocalValueKind::Array, {}}
                  : resolve_value_type(dies, type->number);
  if (!direct_structure && !direct_union && !direct_array &&
      ((value_type.kind != LocalValueKind::Integer &&
        value_type.kind != LocalValueKind::Pointer) ||
       value_type.byte_size == 0 ||
       value_type.byte_size > sizeof(std::uint64_t))) {
    throw std::runtime_error(
        "selected-inline value materialization requires a bounded integer/pointer scalar");
  }

  std::vector<std::byte> expression;
  if (location->form == kDwFormExprloc) {
    expression = location->expression;
  } else if (location->form == kDwFormSecOffset || location->form == kDwFormLoclistx) {
    expression = active_location_expression(sections, location->number, virtual_pc,
                                            unit_base, unit_version);
  } else {
    throw std::runtime_error(
        "selected-inline value has no supported DW_AT_location form");
  }
  if (expression.empty()) {
    throw std::runtime_error(
        "selected-inline value requires one exact compiler-proven location operation");
  }

  const auto opcode = std::to_integer<std::uint8_t>(expression.front());
  if (frame.index != 0) {
    if (opcode != kDwOpFbreg) {
      throw std::runtime_error(
          "caller-frame selected-inline value currently requires compiler-proven DW_OP_fbreg");
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
        value_type.byte_size);    if (direct_structure) {
    return materialize_selected_inline_structure(owner, requested_name,
                                                 *direct_structure, memory);
  }
  if (direct_union) {
    return materialize_selected_inline_union(owner, requested_name,
                                             *direct_union, memory);
  }
  if (direct_array) {
    return materialize_selected_inline_array(owner, requested_name,
                                             *direct_array, memory);
  }
  auto result =
        materialize_snapshot_memory_value(owner, requested_name, value_type, memory);
    attach_pointer_metadata(result, pointee_type);
    return result;
  }

  if (direct_structure) {
    throw std::runtime_error(
        "frame-zero selected-inline aggregate materialization is outside current compiler evidence");
  }
  if (direct_union) {
    throw std::runtime_error(
        "frame-zero selected-inline union materialization is outside current compiler evidence");
  }
  if (direct_array) {
    throw std::runtime_error(
        "frame-zero selected-inline fixed-array materialization is outside current compiler evidence");
  }
  if (value_type.kind == LocalValueKind::Pointer) {
    throw std::runtime_error(
        "frame-zero selected-inline pointer materialization is outside current compiler evidence");
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

LocalScalarValue dereference_inline_local_pointer(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::size_t inline_die_offset, std::string_view name,
    const SnapshotModulePathResolver& module_paths) {
  const auto pointer = inspect_inline_local_value(
      snapshot, frame, inline_die_offset, name, module_paths);
  if (pointer.kind != LocalValueKind::Pointer) {
    throw std::logic_error(
        "selected-inline local value is not a pointer: " + std::string(name));
  }
  if (!pointer.pointee_type ||
      pointer.pointee_type->kind != LocalValueKind::Integer ||
      pointer.pointee_type->byte_size == 0 ||
      pointer.pointee_type->byte_size > sizeof(std::uint64_t) ||
      !pointer.pointee_type->members.empty()) {
    throw std::runtime_error(
        "selected-inline pointer does not have a bounded integer pointee type");
  }
  if (pointer.raw_value == 0) {
    throw std::runtime_error(
        "cannot dereference a null selected-inline core pointer: " +
        std::string(name));
  }
  if (pointer.raw_value > std::numeric_limits<std::uintptr_t>::max()) {
    throw std::runtime_error("selected-inline core pointer exceeds host address width");
  }

  const auto memory = read_snapshot_memory(
      snapshot, module_paths, static_cast<std::uintptr_t>(pointer.raw_value),
      pointer.pointee_type->byte_size);
  const ValueType value_type{pointer.pointee_type->byte_size,
                             pointer.pointee_type->is_signed,
                             LocalValueKind::Integer, {}};
  const SnapshotModuleAddress owner{pointer.module_path, {}, 0};
  return materialize_snapshot_memory_value(
      owner, "*" + pointer.name, value_type, memory, false);
}

}  // namespace mdbg
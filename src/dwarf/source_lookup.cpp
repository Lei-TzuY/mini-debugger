#include "dwarf/source_lookup_impl.inc"
#include "snapshot/inspection.hpp"

namespace mdbg {
namespace {

std::optional<LocalScalarValue> inspect_snapshot_caller_breg3_unit(
    const DebugSections& sections, const ElfFile& module,
    const SnapshotInspectionFrameContext& frame, std::string_view recorded_module_path,
    std::uint64_t virtual_pc, std::string_view name, std::size_t unit_start,
    std::size_t& next_unit) {
  std::uint16_t unit_version = 0;
  const auto dies = parse_unit_dies(sections, unit_start, next_unit, unit_version);
  std::uint64_t compilation_unit_base = 0;
  for (const auto& die : dies) {
    if (die.parent) continue;
    const auto* low_pc = attribute(die, kDwAtLowPc);
    if (low_pc != nullptr && low_pc->form == kDwFormAddr) {
      compilation_unit_base = low_pc->number;
      break;
    }
  }

  std::optional<std::size_t> subprogram;
  for (std::size_t index = 0; index < dies.size(); ++index) {
    if (dies[index].tag != kDwTagSubprogram || !die_contains_pc(dies[index], virtual_pc)) {
      continue;
    }
    if (subprogram) {
      throw std::runtime_error("snapshot inspection frame PC matches multiple DWARF subprograms");
    }
    subprogram = index;
  }
  if (!subprogram) return std::nullopt;

  std::optional<std::size_t> value_die_index;
  std::optional<std::size_t> best_depth;
  bool nested_name = false;
  for (std::size_t index = 0; index < dies.size(); ++index) {
    const bool supported_value_tag =
        dies[index].tag == kDwTagVariable || dies[index].tag == kDwTagFormalParameter;
    if (!supported_value_tag) continue;
    const auto* die_name = attribute_with_abstract_origin(dies, index, kDwAtName);
    if (die_name == nullptr || die_name->text != name) continue;
    const auto depth = active_scope_depth(dies, index, *subprogram, virtual_pc);
    if (!depth) {
      if (is_descendant_of(dies, index, *subprogram)) nested_name = true;
      continue;
    }
    if (!value_die_index || *depth > *best_depth) {
      value_die_index = index;
      best_depth = *depth;
      continue;
    }
    if (*depth == *best_depth) {
      throw std::runtime_error(
          "ambiguous local value in snapshot inspection-frame lexical scope: " +
          std::string(name));
    }
  }
  if (!value_die_index) {
    if (nested_name) {
      throw std::runtime_error(
          "local value is outside the snapshot inspection-frame lexical scope: " +
          std::string(name));
    }
    throw std::runtime_error(
        "local value is not in the snapshot inspection-frame subprogram: " +
        std::string(name));
  }

  const auto& value_die = dies[*value_die_index];
  const auto* location = attribute(value_die, kDwAtLocation);
  const auto* type = attribute_with_abstract_origin(dies, *value_die_index, kDwAtType);
  if (location == nullptr) throw std::runtime_error("local value has no DW_AT_location");
  if (type == nullptr || type->form != kDwFormRef4) {
    throw std::runtime_error("local value has no supported DW_FORM_ref4 type");
  }

  const auto value_type = resolve_value_type(dies, type->number);
  std::vector<std::byte> location_expression;
  if (location->form == kDwFormExprloc) {
    location_expression = location->expression;
  } else if (location->form == kDwFormSecOffset || location->form == kDwFormLoclistx) {
    location_expression = active_location_expression(
        sections, location->number, virtual_pc, compilation_unit_base, unit_version);
  } else {
    throw std::runtime_error("local value has no supported DW_AT_location form");
  }

  if (value_type.kind == LocalValueKind::Structure || location_expression.empty() ||
      std::to_integer<std::uint8_t>(location_expression.front()) != kDwOpBreg3) {
    throw std::runtime_error(
        "snapshot caller local requires the existing compiler-proven DW_OP_breg3 scalar form");
  }
  if (!frame.registers.rbx) {
    throw std::runtime_error(
        "snapshot caller DW_OP_breg3 requires CFI-recovered historical RBX");
  }

  const auto raw = truncate_integer(
      evaluate_breg3_xor_stack_value(location_expression, *frame.registers.rbx),
      value_type.byte_size);
  return LocalScalarValue{std::string(recorded_module_path), std::string(name), raw,
                          value_type.byte_size, value_type.is_signed,
                          value_type.kind};
}

}  // namespace

LocalScalarValue inspect_local_value(const CoreSnapshot& snapshot,
                                     const SnapshotInspectionFrameContext& frame,
                                     std::string_view name,
                                     const SnapshotModulePathResolver& module_paths) {
  if (name.empty()) throw std::invalid_argument("local variable name must not be empty");
  validate_snapshot_inspection_frame(snapshot, frame);
  if (frame.index == 0) {
    throw std::invalid_argument(
        "snapshot caller local-value inspection requires a recovered caller frame");
  }

  const auto owner =
      resolve_snapshot_module_address(snapshot, frame.runtime_pc, module_paths);
  if (owner.module_path != frame.module_path) {
    throw std::logic_error("snapshot inspection frame module ownership changed");
  }
  const ElfFile module(owner.module_file_path);
  const auto sections = read_debug_sections(module.path());

  std::size_t unit = 0;
  while (unit < sections.info.size()) {
    std::size_t next = unit;
    const auto result = inspect_snapshot_caller_breg3_unit(
        sections, module, frame, owner.module_path, owner.virtual_address, name, unit, next);
    if (result) return *result;
    if (next <= unit) {
      throw std::runtime_error("DWARF parser did not advance to the next unit");
    }
    unit = next;
  }
  throw std::runtime_error(
      "snapshot inspection-frame PC is not covered by a supported DWARF4/5 subprogram");
}

LocalScalarValue inspect_local_value(const CoreSnapshot& snapshot,
                                     const SnapshotInspectionFrameContext& frame,
                                     std::string_view name) {
  return inspect_local_value(snapshot, frame, name, identity_snapshot_module_paths());
}

LocalIntegerValue inspect_local_integer(const CoreSnapshot& snapshot,
                                        const SnapshotInspectionFrameContext& frame,
                                        std::string_view name,
                                        const SnapshotModulePathResolver& module_paths) {
  auto value = inspect_local_value(snapshot, frame, name, module_paths);
  if (value.kind != LocalValueKind::Integer) {
    throw std::runtime_error("local value is not an integer scalar: " + std::string(name));
  }
  return value;
}

LocalIntegerValue inspect_local_integer(const CoreSnapshot& snapshot,
                                        const SnapshotInspectionFrameContext& frame,
                                        std::string_view name) {
  return inspect_local_integer(snapshot, frame, name, identity_snapshot_module_paths());
}

}  // namespace mdbg

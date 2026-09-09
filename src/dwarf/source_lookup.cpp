#include "dwarf/source_lookup_impl.inc"
#include "snapshot/inspection.hpp"
#include "snapshot/memory.hpp"

namespace mdbg {
namespace {

constexpr std::uint8_t kDwOpAddr = 0x03;

std::uint64_t decode_snapshot_address(const std::vector<std::byte>& expression) {
  constexpr std::size_t kAddressSize = sizeof(std::uint64_t);
  if (expression.size() != 1 + kAddressSize) {
    throw std::runtime_error(
        "snapshot DW_OP_addr requires one exact x86-64 address operand");
  }
  std::uint64_t address = 0;
  for (std::size_t index = 0; index < kAddressSize; ++index) {
    address |= static_cast<std::uint64_t>(
                   std::to_integer<unsigned int>(expression[index + 1]))
               << (index * 8U);
  }
  return address;
}

std::uint64_t decode_snapshot_scalar(const SnapshotMemoryRead& memory,
                                     std::size_t byte_size) {
  if (byte_size == 0 || byte_size > sizeof(std::uint64_t) ||
      memory.bytes.size() != byte_size) {
    throw std::runtime_error("snapshot scalar memory width is unsupported");
  }
  std::uint64_t raw = 0;
  for (std::size_t index = 0; index < memory.bytes.size(); ++index) {
    raw |= static_cast<std::uint64_t>(
               std::to_integer<unsigned int>(memory.bytes[index]))
           << (index * 8U);
  }
  return raw;
}

LocalScalarValue materialize_snapshot_memory_value(
    const SnapshotModuleAddress& owner, std::string_view name,
    const ValueType& value_type, const SnapshotMemoryRead& memory) {
  LocalScalarValue result;
  if (value_type.kind == LocalValueKind::Structure) {
    if (value_type.byte_size == 0 || value_type.byte_size > kMaxLocalStructSize ||
        memory.bytes.size() != value_type.byte_size) {
      throw std::runtime_error("snapshot aggregate bytes exceed the bounded structure model");
    }
    result = LocalScalarValue{owner.module_path, std::string(name), 0,
                              value_type.byte_size, false,
                              LocalValueKind::Structure};
    result.members.reserve(value_type.members.size());
    for (const auto& member : value_type.members) {
      result.members.push_back(LocalStructMember{
          member.name,
          decode_integer(memory.bytes, member.offset, member.integer.byte_size),
          member.integer.byte_size, member.integer.is_signed});
    }
  } else {
    result = LocalScalarValue{
        owner.module_path, std::string(name),
        decode_snapshot_scalar(memory, value_type.byte_size), value_type.byte_size,
        value_type.is_signed, value_type.kind};
  }

  if (memory.provenance == SnapshotMemoryProvenance::Core) {
    result.storage = LocalValueStorage::SnapshotCoreMemory;
  } else {
    if (memory.module_path != owner.module_path) {
      throw std::logic_error("snapshot local artifact ownership changed during value read");
    }
    result.storage = LocalValueStorage::SnapshotRuntimeArtifact;
    result.storage_module_path = memory.module_path;
    result.storage_file_path = memory.module_file_path;
    result.storage_file_offset = memory.artifact_file_offset;
  }
  return result;
}

std::optional<LocalScalarValue> inspect_snapshot_caller_unit(
    const DebugSections& sections, const CoreSnapshot& snapshot,
    const SnapshotInspectionFrameContext& frame,
    const SnapshotModulePathResolver& module_paths,
    const SnapshotModuleAddress& owner, std::uint64_t virtual_pc,
    std::string_view name, std::size_t unit_start, std::size_t& next_unit) {
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

  if (location_expression.empty()) {
    throw std::runtime_error(
        "snapshot caller local requires a compiler-proven location form");
  }

  const auto opcode = std::to_integer<std::uint8_t>(location_expression.front());
  if (opcode == kDwOpBreg3) {
    if (value_type.kind == LocalValueKind::Structure) {
      throw std::runtime_error(
          "snapshot caller DW_OP_breg3 requires a scalar value");
    }
    if (!frame.registers.rbx) {
      throw std::runtime_error(
          "snapshot caller DW_OP_breg3 requires CFI-recovered historical RBX");
    }
    const auto raw = truncate_integer(
        evaluate_breg3_xor_stack_value(location_expression, *frame.registers.rbx),
        value_type.byte_size);
    return LocalScalarValue{owner.module_path, std::string(name), raw,
                            value_type.byte_size, value_type.is_signed,
                            value_type.kind};
  }

  if (opcode != kDwOpAddr) {
    throw std::runtime_error(
        "snapshot caller local requires a compiler-proven DW_OP_breg3 scalar or DW_OP_addr memory form");
  }
  if (value_type.kind != LocalValueKind::Structure &&
      (value_type.byte_size == 0 || value_type.byte_size > sizeof(std::uint64_t))) {
    throw std::runtime_error("snapshot DW_OP_addr scalar width exceeds the bounded reader");
  }
  if (value_type.kind == LocalValueKind::Structure &&
      (value_type.byte_size == 0 || value_type.byte_size > kMaxLocalStructSize)) {
    throw std::runtime_error("snapshot DW_OP_addr aggregate width exceeds the bounded reader");
  }
  if (frame.runtime_pc < owner.virtual_address) {
    throw std::runtime_error("snapshot frame runtime PC is below its module virtual address");
  }
  const auto load_bias =
      static_cast<std::uint64_t>(frame.runtime_pc) - owner.virtual_address;
  const auto virtual_address = decode_snapshot_address(location_expression);
  if (virtual_address > std::numeric_limits<std::uint64_t>::max() - load_bias) {
    throw std::overflow_error("snapshot DW_OP_addr runtime address overflow");
  }
  const auto runtime_address_u64 = virtual_address + load_bias;
  if (runtime_address_u64 > std::numeric_limits<std::uintptr_t>::max()) {
    throw std::overflow_error("snapshot DW_OP_addr exceeds runtime address width");
  }
  const auto memory = read_snapshot_memory(
      snapshot, module_paths, static_cast<std::uintptr_t>(runtime_address_u64),
      value_type.byte_size);
  return materialize_snapshot_memory_value(owner, name, value_type, memory);
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
  const auto sections = read_debug_sections(module_paths.resolve_debug_file(owner.module_path));

  std::size_t unit = 0;
  while (unit < sections.info.size()) {
    std::size_t next = unit;
    const auto result = inspect_snapshot_caller_unit(
        sections, snapshot, frame, module_paths, owner, owner.virtual_address,
        name, unit, next);
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

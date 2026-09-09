#include "dwarf/source_lookup_impl.inc"
#include "snapshot/frame_lookup.hpp"
#include "snapshot/inspection.hpp"
#include "snapshot/memory.hpp"

namespace mdbg {
namespace {

constexpr std::uint8_t kDwOpAddr = 0x03;
constexpr std::uint64_t kSnapshotDwAteFloat = 0x04;
constexpr std::uint8_t kSnapshotDwOpRsp =
    static_cast<std::uint8_t>(kDwOpReg0 + 7U);
constexpr std::uint8_t kSnapshotDwOpR12 =
    static_cast<std::uint8_t>(kDwOpReg0 + 12U);
constexpr std::uint8_t kSnapshotDwOpXmm0 =
    static_cast<std::uint8_t>(kDwOpReg0 + 17U);

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

std::int64_t decode_snapshot_fbreg_offset(
    const std::vector<std::byte>& expression) {
  if (expression.empty() ||
      std::to_integer<std::uint8_t>(expression.front()) != kDwOpFbreg) {
    throw std::runtime_error(
        "snapshot stack local requires one compiler-proven DW_OP_fbreg expression");
  }
  std::size_t cursor = 1;
  const auto offset =
      read_sleb(expression, cursor, expression.size(), "snapshot DW_OP_fbreg offset");
  if (cursor != expression.size()) {
    throw std::runtime_error(
        "snapshot DW_OP_fbreg does not support trailing operations");
  }
  return offset;
}

std::uint64_t snapshot_frame_base(
    const std::vector<Die>& dies, std::size_t subprogram,
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    const SnapshotModulePathResolver& module_paths,
    const SnapshotModuleAddress& owner) {
  const auto* frame_base = attribute(dies[subprogram], kDwAtFrameBase);
  if (frame_base == nullptr || frame_base->form != kDwFormExprloc ||
      frame_base->expression.size() != 1) {
    throw std::runtime_error(
        "snapshot DW_OP_fbreg requires one compiler-proven DW_AT_frame_base operation");
  }

  const auto op =
      std::to_integer<std::uint8_t>(frame_base->expression.front());
  if (op == kSnapshotDwOpRsp) {
    if (!frame.registers.rsp) {
      throw std::runtime_error(
          "snapshot DW_OP_reg7 frame base requires immutable RSP ownership");
    }
    if (*frame.registers.rsp != frame.stack_pointer) {
      throw std::logic_error(
          "snapshot inspection-frame RSP ownership does not match its stack pointer");
    }
    return *frame.registers.rsp;
  }
  if (op != kDwOpCallFrameCfa) {
    throw std::runtime_error(
        "snapshot DW_OP_fbreg has an unsupported DW_AT_frame_base operation");
  }

  const EhFrame cfi(owner.module_file_path);
  if (!cfi.available()) {
    throw std::runtime_error(
        "snapshot DW_OP_call_frame_cfa requires owner .eh_frame");
  }
  const CfiMemoryReader read_memory =
      [&snapshot, &module_paths](std::uintptr_t address, std::size_t length) {
        return read_snapshot_memory(snapshot, module_paths, address, length).bytes;
      };
  const EhFrameCursor current{frame.runtime_pc, frame.stack_pointer,
                              frame.frame_pointer, frame.registers.rbx};
  const auto caller = cfi.caller_frame(read_memory, owner.virtual_address, current);
  if (!caller) {
    throw std::runtime_error(
        "snapshot CFI did not cover compiler-proven DW_OP_call_frame_cfa");
  }
  return caller->stack_pointer;
}

std::optional<ValueType> resolve_snapshot_floating_type(
    const std::vector<Die>& dies, std::uint64_t type_offset) {
  for (unsigned depth = 0; depth < 16; ++depth) {
    const auto index = die_index_by_offset(dies, type_offset);
    if (!index) {
      throw std::runtime_error("snapshot local value type references an unknown DIE");
    }
    const auto& die = dies[*index];
    if (die.tag == kDwTagTypedef || die.tag == kDwTagConstType) {
      const auto* type = attribute(die, kDwAtType);
      if (type == nullptr || type->form != kDwFormRef4) {
        throw std::runtime_error(
            "snapshot floating type wrapper does not use DW_FORM_ref4");
      }
      type_offset = type->number;
      continue;
    }
    if (die.tag != kDwTagBaseType) return std::nullopt;

    const auto* size = attribute(die, kDwAtByteSize);
    const auto* encoding = attribute(die, kDwAtEncoding);
    if (encoding == nullptr || encoding->number != kSnapshotDwAteFloat) {
      return std::nullopt;
    }
    if (size == nullptr || (size->number != 4 && size->number != 8)) {
      throw std::runtime_error(
          "snapshot floating base type must be a compiler-proven 4/8-byte scalar");
    }
    return ValueType{static_cast<std::size_t>(size->number), false,
                     LocalValueKind::Floating, {}};
  }
  throw std::runtime_error("snapshot floating type chain is too deep");
}

std::optional<LocalPointeeType> resolve_pointer_pointee_type(
    const std::vector<Die>& dies, std::uint64_t type_offset) {
  for (unsigned depth = 0; depth < 16; ++depth) {
    const auto index = die_index_by_offset(dies, type_offset);
    if (!index) {
      throw std::runtime_error("snapshot pointer type references an unknown DIE");
    }
    const auto& die = dies[*index];
    if (die.tag == kDwTagTypedef || die.tag == kDwTagConstType) {
      const auto* type = attribute(die, kDwAtType);
      if (type == nullptr || type->form != kDwFormRef4) {
        throw std::runtime_error(
            "snapshot pointer type wrapper does not use DW_FORM_ref4");
      }
      type_offset = type->number;
      continue;
    }
    if (die.tag != kDwTagPointerType) return std::nullopt;
    const auto* size = attribute(die, kDwAtByteSize);
    const auto pointer_size = size == nullptr ? std::size_t{8}
                                              : static_cast<std::size_t>(size->number);
    if (pointer_size != 8) {
      throw std::runtime_error("snapshot pointer type has an unsupported byte size");
    }
    const auto* pointee = attribute(die, kDwAtType);
    if (pointee == nullptr || pointee->form != kDwFormRef4) {
      throw std::runtime_error("snapshot pointer has no supported DW_FORM_ref4 pointee");
    }

    const auto value_type = resolve_value_type(dies, pointee->number);
    if (value_type.kind == LocalValueKind::Integer) {
      return LocalPointeeType{value_type.byte_size, value_type.is_signed,
                              LocalValueKind::Integer, {}};
    }
    if (value_type.kind == LocalValueKind::Structure) {
      LocalPointeeType result{value_type.byte_size, false,
                              LocalValueKind::Structure, {}};
      result.members.reserve(value_type.members.size());
      for (const auto& member : value_type.members) {
        result.members.push_back(LocalStructMemberType{
            member.name, member.offset, member.integer.byte_size,
            member.integer.is_signed});
      }
      return result;
    }
    throw std::runtime_error(
        "snapshot pointer pointee is not a bounded integer or structure type");
  }
  throw std::runtime_error("snapshot pointer type chain is too deep");
}

std::size_t snapshot_xmm_index(const std::vector<std::byte>& expression) {
  if (expression.size() != 1) {
    throw std::runtime_error(
        "snapshot XMM local requires one exact compiler-proven DW_OP_reg expression");
  }
  const auto opcode = std::to_integer<std::uint8_t>(expression.front());
  if (opcode != kSnapshotDwOpXmm0) {
    throw std::runtime_error(
        "snapshot XMM local currently supports only compiler-proven DW_OP_reg17 (xmm0)");
  }
  return 0;
}

LocalScalarValue materialize_snapshot_xmm_value(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    const SnapshotModuleAddress& owner, std::string_view name,
    const ValueType& value_type, const std::vector<std::byte>& expression) {
  if (value_type.kind != LocalValueKind::Floating ||
      (value_type.byte_size != 4 && value_type.byte_size != 8)) {
    throw std::runtime_error(
        "snapshot XMM register location requires a bounded floating scalar type");
  }
  const auto index = snapshot_xmm_index(expression);
  std::array<std::byte, 16> xmm{};
  if (frame.index == 0) {
    const auto floating = snapshot.floating_point_state(frame.thread_tid);
    if (!floating) {
      throw std::runtime_error(
          "selected core thread has no NT_FPREGSET state for XMM source recovery");
    }
    if (index >= floating->xmm.size()) {
      throw std::runtime_error("snapshot XMM register number is outside the FPREGSET model");
    }
    xmm = floating->xmm[index];
  } else {
    if (index != 0 || !frame.registers.xmm0) {
      throw std::runtime_error(
          "historical snapshot frame has no restored XMM0 ownership");
    }
    xmm = *frame.registers.xmm0;
  }

  std::uint64_t raw = 0;
  for (std::size_t byte = 0; byte < value_type.byte_size; ++byte) {
    raw |= static_cast<std::uint64_t>(
               std::to_integer<unsigned int>(xmm[byte]))
           << (byte * 8U);
  }
  LocalScalarValue result{owner.module_path, std::string(name), raw,
                          value_type.byte_size, false,
                          LocalValueKind::Floating};
  result.storage = LocalValueStorage::SnapshotCoreRegister;
  return result;
}

LocalScalarValue materialize_snapshot_memory_value(
    const SnapshotModuleAddress& owner, std::string_view name,
    const ValueType& value_type, const SnapshotMemoryRead& memory,
    bool require_owner_match = true) {
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
    if (require_owner_match && memory.module_path != owner.module_path) {
      throw std::logic_error("snapshot local artifact ownership changed during value read");
    }
    result.storage = LocalValueStorage::SnapshotRuntimeArtifact;
    result.storage_module_path = memory.module_path;
    result.storage_file_path = memory.module_file_path;
    result.storage_file_offset = memory.artifact_file_offset;
  }
  return result;
}

void attach_pointer_metadata(LocalScalarValue& result,
                             const std::optional<LocalPointeeType>& pointee) {
  if (result.kind != LocalValueKind::Pointer) return;
  if (!pointee) {
    throw std::logic_error("bounded pointer value lost its pointee metadata");
  }
  if (pointee->kind == LocalValueKind::Integer) {
    if (pointee->byte_size == 0 ||
        pointee->byte_size > sizeof(std::uint64_t) || !pointee->members.empty()) {
      throw std::logic_error("bounded pointer value has invalid integer pointee metadata");
    }
  } else if (pointee->kind == LocalValueKind::Structure) {
    if (pointee->byte_size == 0 || pointee->byte_size > kMaxLocalStructSize ||
        pointee->members.size() > kMaxLocalStructMembers) {
      throw std::logic_error("bounded pointer value has invalid structure pointee metadata");
    }
    for (const auto& member : pointee->members) {
      if (member.byte_size == 0 || member.byte_size > sizeof(std::uint64_t) ||
          member.offset > pointee->byte_size ||
          member.byte_size > pointee->byte_size - member.offset) {
        throw std::logic_error(
            "bounded pointer value has an out-of-range structure member");
      }
    }
  } else {
    throw std::logic_error("bounded pointer value has an unsupported pointee kind");
  }
  result.pointee_type = *pointee;
}

std::optional<LocalScalarValue> inspect_snapshot_unit(
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

  const auto floating_type = resolve_snapshot_floating_type(dies, type->number);
  const auto pointee_type = floating_type
                                ? std::optional<LocalPointeeType>{}
                                : resolve_pointer_pointee_type(dies, type->number);
  const auto value_type = floating_type
                              ? *floating_type
                              : pointee_type
                                    ? ValueType{sizeof(std::uintptr_t), false,
                                                LocalValueKind::Pointer, {}}
                                    : resolve_value_type(dies, type->number);
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
        "snapshot local requires a compiler-proven location form");
  }

  const auto opcode = std::to_integer<std::uint8_t>(location_expression.front());
  if (opcode == kSnapshotDwOpXmm0) {
    return materialize_snapshot_xmm_value(snapshot, frame, owner, name,
                                          value_type, location_expression);
  }

  if (opcode == kSnapshotDwOpR12) {
    if (location_expression.size() != 1) {
      throw std::runtime_error(
          "snapshot DW_OP_reg12 requires one exact compiler-proven register operation");
    }
    if (value_type.kind == LocalValueKind::Structure ||
        value_type.kind == LocalValueKind::Floating || value_type.byte_size == 0 ||
        value_type.byte_size > sizeof(std::uint64_t)) {
      throw std::runtime_error(
          "snapshot DW_OP_reg12 requires a bounded integer/pointer scalar value");
    }
    if (!frame.registers.r12) {
      throw std::runtime_error(
          "snapshot DW_OP_reg12 requires immutable R12 ownership");
    }
    const auto raw = truncate_integer(*frame.registers.r12, value_type.byte_size);
    LocalScalarValue result{owner.module_path, std::string(name), raw,
                            value_type.byte_size, value_type.is_signed,
                            value_type.kind};
    result.storage = LocalValueStorage::SnapshotCoreRegister;
    attach_pointer_metadata(result, pointee_type);
    return result;
  }

  if (opcode == kDwOpFbreg) {
    if (value_type.kind != LocalValueKind::Integer || value_type.byte_size == 0 ||
        value_type.byte_size > sizeof(std::uint64_t)) {
      throw std::runtime_error(
          "snapshot DW_OP_fbreg currently requires the compiler-proven bounded integer local");
    }
    const auto base = snapshot_frame_base(
        dies, *subprogram, snapshot, frame, module_paths, owner);
    const auto runtime_address = add_signed(
        base, decode_snapshot_fbreg_offset(location_expression),
        "snapshot DW_OP_fbreg runtime address");
    if (runtime_address > std::numeric_limits<std::uintptr_t>::max()) {
      throw std::overflow_error(
          "snapshot DW_OP_fbreg address exceeds runtime address width");
    }
    const auto memory = read_snapshot_memory(
        snapshot, module_paths, static_cast<std::uintptr_t>(runtime_address),
        value_type.byte_size);
    return materialize_snapshot_memory_value(owner, name, value_type, memory);
  }
  if (opcode == kDwOpBreg3) {
    if (value_type.kind == LocalValueKind::Structure ||
        value_type.kind == LocalValueKind::Floating) {
      throw std::runtime_error(
          "snapshot caller DW_OP_breg3 requires an integer/pointer scalar value");
    }
    if (!frame.registers.rbx) {
      throw std::runtime_error(
          "snapshot caller DW_OP_breg3 requires CFI-recovered historical RBX");
    }
    const auto raw = truncate_integer(
        evaluate_breg3_xor_stack_value(location_expression, *frame.registers.rbx),
        value_type.byte_size);
    LocalScalarValue result{owner.module_path, std::string(name), raw,
                            value_type.byte_size, value_type.is_signed,
                            value_type.kind};
    attach_pointer_metadata(result, pointee_type);
    return result;
  }

  if (opcode != kDwOpAddr) {
    if (frame.index == 0) {
      throw std::runtime_error(
          "snapshot frame-zero local requires compiler-proven XMM0, DW_OP_fbreg, DW_OP_reg12, or DW_OP_addr ownership");
    }
    throw std::runtime_error(
        "snapshot caller local requires compiler-proven XMM0, DW_OP_fbreg, DW_OP_breg3, DW_OP_reg12, or DW_OP_addr ownership");
  }
  if (value_type.kind != LocalValueKind::Structure &&
      (value_type.byte_size == 0 || value_type.byte_size > sizeof(std::uint64_t))) {
    throw std::runtime_error("snapshot DW_OP_addr scalar width exceeds the bounded reader");
  }
  if (value_type.kind == LocalValueKind::Structure &&
      (value_type.byte_size == 0 || value_type.byte_size > kMaxLocalStructSize)) {
    throw std::runtime_error("snapshot DW_OP_addr aggregate width exceeds the bounded reader");
  }
  const auto lookup_runtime_pc = snapshot_frame_lookup_pc(frame);
  if (lookup_runtime_pc < owner.virtual_address) {
    throw std::runtime_error("snapshot frame lookup PC is below its module virtual address");
  }
  const auto load_bias =
      static_cast<std::uint64_t>(lookup_runtime_pc) - owner.virtual_address;
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
  auto result = materialize_snapshot_memory_value(owner, name, value_type, memory);
  attach_pointer_metadata(result, pointee_type);
  return result;
}

}  // namespace

LocalScalarValue inspect_local_value(const CoreSnapshot& snapshot,
                                     const SnapshotInspectionFrameContext& frame,
                                     std::string_view name,
                                     const SnapshotModulePathResolver& module_paths) {
  if (name.empty()) throw std::invalid_argument("local variable name must not be empty");
  validate_snapshot_inspection_frame(snapshot, frame);
  validate_snapshot_frame_lookup_pc(frame);

  const auto lookup_runtime_pc = snapshot_frame_lookup_pc(frame);
  const auto owner =
      resolve_snapshot_module_address(snapshot, lookup_runtime_pc, module_paths);
  if (owner.module_path != frame.module_path) {
    throw std::logic_error("snapshot inspection frame module ownership changed");
  }
  const auto sections = read_debug_sections(module_paths.resolve_debug_file(owner.module_path));

  std::size_t unit = 0;
  while (unit < sections.info.size()) {
    std::size_t next = unit;
    const auto result = inspect_snapshot_unit(
        sections, snapshot, frame, module_paths, owner, owner.virtual_address,
        name, unit, next);
    if (result) return *result;
    if (next <= unit) {
      throw std::runtime_error("DWARF parser did not advance to the next unit");
    }
    unit = next;
  }
  throw std::runtime_error(
      "snapshot inspection-frame lookup PC is not covered by a supported DWARF4/5 subprogram");
}

LocalScalarValue inspect_local_value(const CoreSnapshot& snapshot,
                                     const SnapshotInspectionFrameContext& frame,
                                     std::string_view name) {
  return inspect_local_value(snapshot, frame, name, identity_snapshot_module_paths());
}

LocalScalarValue dereference_local_pointer(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::string_view name, const SnapshotModulePathResolver& module_paths) {
  const auto pointer = inspect_local_value(snapshot, frame, name, module_paths);
  if (pointer.kind != LocalValueKind::Pointer) {
    throw std::runtime_error("local value is not a pointer: " + std::string(name));
  }
  if (!pointer.pointee_type) {
    throw std::runtime_error("pointer does not have bounded pointee metadata");
  }
  if (pointer.raw_value == 0) {
    throw std::runtime_error("cannot dereference a null core pointer: " + std::string(name));
  }
  if (pointer.raw_value > std::numeric_limits<std::uintptr_t>::max()) {
    throw std::runtime_error("core pointer exceeds host address width");
  }

  ValueType value_type{};
  if (pointer.pointee_type->kind == LocalValueKind::Integer) {
    if (pointer.pointee_type->byte_size == 0 ||
        pointer.pointee_type->byte_size > sizeof(std::uint64_t) ||
        !pointer.pointee_type->members.empty()) {
      throw std::runtime_error("pointer does not have a bounded integer pointee type");
    }
    value_type = ValueType{pointer.pointee_type->byte_size,
                           pointer.pointee_type->is_signed,
                           LocalValueKind::Integer, {}};
  } else if (pointer.pointee_type->kind == LocalValueKind::Structure) {
    if (pointer.pointee_type->byte_size == 0 ||
        pointer.pointee_type->byte_size > kMaxLocalStructSize ||
        pointer.pointee_type->members.size() > kMaxLocalStructMembers) {
      throw std::runtime_error("pointer does not have a bounded structure pointee type");
    }
    value_type = ValueType{pointer.pointee_type->byte_size, false,
                           LocalValueKind::Structure, {}};
    value_type.members.reserve(pointer.pointee_type->members.size());
    for (const auto& member : pointer.pointee_type->members) {
      if (member.byte_size == 0 || member.byte_size > sizeof(std::uint64_t) ||
          member.offset > pointer.pointee_type->byte_size ||
          member.byte_size > pointer.pointee_type->byte_size - member.offset) {
        throw std::runtime_error("pointer structure member exceeds bounded pointee layout");
      }
      value_type.members.push_back(AggregateMemberType{
          member.name, member.offset,
          IntegerType{member.byte_size, member.is_signed}});
    }
  } else {
    throw std::runtime_error("pointer pointee kind is unsupported for bounded dereference");
  }

  const auto memory = read_snapshot_memory(
      snapshot, module_paths, static_cast<std::uintptr_t>(pointer.raw_value),
      value_type.byte_size);
  const SnapshotModuleAddress owner{pointer.module_path, {}, 0};
  return materialize_snapshot_memory_value(owner, "*" + pointer.name,
                                           value_type, memory, false);
}

LocalScalarValue dereference_local_pointer(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::string_view name) {
  return dereference_local_pointer(snapshot, frame, name,
                                   identity_snapshot_module_paths());
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

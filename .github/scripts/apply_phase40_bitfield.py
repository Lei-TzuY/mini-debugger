from pathlib import Path

local = Path("src/dwarf/local_value.hpp")
text = local.read_text()
marker = """struct LocalPointerPointeeType {
  std::size_t byte_size;
  bool is_signed;
};

struct LocalStructMemberType {"""
replacement = """struct LocalPointerPointeeType {
  std::size_t byte_size;
  bool is_signed;
};

struct LocalBitSlice {
  std::size_t bit_offset;
  std::size_t bit_size;
};

struct LocalStructMemberType {"""
if marker not in text:
    raise SystemExit("local-value insertion marker missing")
text = text.replace(marker, replacement, 1)
marker = """  LocalValueKind kind{LocalValueKind::Integer};
  std::optional<LocalPointerPointeeType> pointee_type{};
};

struct LocalPointeeType {"""
replacement = """  LocalValueKind kind{LocalValueKind::Integer};
  std::optional<LocalPointerPointeeType> pointee_type{};
  std::optional<LocalBitSlice> bit_slice{};
};

struct LocalPointeeType {"""
if marker not in text:
    raise SystemExit("member-type marker missing")
text = text.replace(marker, replacement, 1)
marker = """  LocalValueKind kind{LocalValueKind::Integer};
  std::optional<LocalPointerPointeeType> pointee_type{};
};

struct LocalArrayElement {"""
replacement = """  LocalValueKind kind{LocalValueKind::Integer};
  std::optional<LocalPointerPointeeType> pointee_type{};
  std::optional<LocalBitSlice> bit_slice{};
};

struct LocalArrayElement {"""
if marker not in text:
    raise SystemExit("materialized-member marker missing")
text = text.replace(marker, replacement, 1)
local.write_text(text)

source = Path("src/dwarf/source_lookup.cpp")
text = source.read_text()
marker = """constexpr std::uint64_t kInlineDwAtCount = 0x37;
constexpr std::size_t kMaxSelectedInlineArrayElements = 64;"""
replacement = """constexpr std::uint64_t kInlineDwAtCount = 0x37;
constexpr std::uint64_t kInlineDwAtBitOffset = 0x0c;
constexpr std::uint64_t kInlineDwAtBitSize = 0x0d;
constexpr std::uint64_t kInlineDwAtDataBitOffset = 0x6b;
constexpr std::size_t kMaxSelectedInlineArrayElements = 64;"""
if marker not in text:
    raise SystemExit("inline bit attribute insertion marker missing")
text = text.replace(marker, replacement, 1)

start = text.index("std::optional<LocalPointeeType> selected_inline_direct_structure_type(")
end = text.index("\nLocalStructMemberType selected_inline_union_member_type(", start)
new_block = r'''std::optional<LocalBitSlice> selected_inline_bit_slice(
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
'''
text = text[:start] + new_block + text[end:]

start = text.index("LocalScalarValue materialize_selected_inline_structure(")
end = text.index("\nstd::optional<LocalScalarValue> inspect_inline_scalar_unit(", start)
new_block = r'''std::uint64_t selected_inline_bit_mask(std::size_t bits) {
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
'''
text = text[:start] + new_block + text[end:]
source.write_text(text)

#pragma once

#include "dwarf/inline_context.hpp"
#include "snapshot/memory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mdbg {
namespace inline_member_detail {

inline std::uint64_t decode_scalar(const SnapshotMemoryRead& memory,
                                   std::size_t byte_size) {
  if (byte_size == 0 || byte_size > sizeof(std::uint64_t) ||
      memory.bytes.size() != byte_size) {
    throw std::runtime_error("selected-inline member has an unsupported scalar width");
  }
  std::uint64_t raw = 0;
  for (std::size_t index = 0; index < byte_size; ++index) {
    raw |= static_cast<std::uint64_t>(
               std::to_integer<unsigned int>(memory.bytes[index]))
           << (index * 8U);
  }
  return raw;
}

inline void attach_storage(LocalScalarValue& result,
                           const SnapshotMemoryRead& memory) {
  if (memory.provenance == SnapshotMemoryProvenance::Core) {
    result.storage = LocalValueStorage::SnapshotCoreMemory;
    return;
  }
  result.storage = LocalValueStorage::SnapshotRuntimeArtifact;
  result.storage_module_path = memory.module_path;
  result.storage_file_path = memory.module_file_path;
  result.storage_file_offset = memory.artifact_file_offset;
}

inline std::uintptr_t checked_address(std::uint64_t base, std::size_t offset,
                                      const char* context) {
  if (base == 0) {
    throw std::runtime_error(std::string("cannot traverse a null ") + context);
  }
  if (base > std::numeric_limits<std::uintptr_t>::max()) {
    throw std::runtime_error(std::string(context) + " exceeds host address width");
  }
  const auto address = static_cast<std::uintptr_t>(base);
  if (offset > std::numeric_limits<std::uintptr_t>::max() - address) {
    throw std::overflow_error(std::string(context) + " member address overflows");
  }
  return address + offset;
}

}  // namespace inline_member_detail

inline LocalScalarValue inspect_inline_local_pointer_member(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::size_t inline_die_offset, std::string_view name,
    std::string_view member_name,
    const SnapshotModulePathResolver& module_paths) {
  if (member_name.empty()) {
    throw std::invalid_argument("selected-inline pointer member name must not be empty");
  }
  const auto pointer = inspect_inline_local_value(
      snapshot, frame, inline_die_offset, name, module_paths);
  if (pointer.kind != LocalValueKind::Pointer || !pointer.pointee_type ||
      pointer.pointee_type->kind != LocalValueKind::Structure) {
    throw std::runtime_error(
        "selected-inline local is not a pointer to a bounded structure: " +
        std::string(name));
  }
  if (pointer.pointee_type->byte_size == 0 ||
      pointer.pointee_type->members.empty()) {
    throw std::logic_error(
        "selected-inline structure pointer lost bounded member metadata");
  }

  const auto member = std::find_if(
      pointer.pointee_type->members.begin(), pointer.pointee_type->members.end(),
      [member_name](const LocalStructMemberType& candidate) {
        return candidate.name == member_name;
      });
  if (member == pointer.pointee_type->members.end()) {
    throw std::runtime_error("bounded selected-inline structure has no member named: " +
                             std::string(member_name));
  }
  if (member->byte_size == 0 || member->byte_size > sizeof(std::uint64_t) ||
      member->offset > pointer.pointee_type->byte_size ||
      member->byte_size > pointer.pointee_type->byte_size - member->offset) {
    throw std::logic_error(
        "selected-inline structure member exceeds bounded pointee layout");
  }
  if (member->kind == LocalValueKind::Integer) {
    if (member->pointee_type) {
      throw std::logic_error(
          "selected-inline integer member unexpectedly has pointee metadata");
    }
  } else if (member->kind == LocalValueKind::Pointer) {
    if (member->byte_size != sizeof(std::uintptr_t) || member->is_signed ||
        !member->pointee_type || member->pointee_type->byte_size == 0 ||
        member->pointee_type->byte_size > sizeof(std::uint64_t)) {
      throw std::logic_error(
          "selected-inline pointer member has invalid bounded pointee metadata");
    }
  } else {
    throw std::runtime_error(
        "selected-inline structure member kind is outside the bounded traversal model");
  }

  const auto address = inline_member_detail::checked_address(
      pointer.raw_value, member->offset, "selected-inline structure pointer");
  const auto memory =
      read_snapshot_memory(snapshot, module_paths, address, member->byte_size);
  LocalScalarValue result{
      pointer.module_path,
      pointer.name + "->" + std::string(member_name),
      inline_member_detail::decode_scalar(memory, member->byte_size),
      member->byte_size, member->is_signed, member->kind};
  inline_member_detail::attach_storage(result, memory);
  if (member->kind == LocalValueKind::Pointer) {
    result.pointee_type = LocalPointeeType{
        member->pointee_type->byte_size, member->pointee_type->is_signed,
        LocalValueKind::Integer, {}};
  }
  return result;
}

inline LocalScalarValue dereference_inline_local_pointer_member(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::size_t inline_die_offset, std::string_view name,
    std::string_view member_name,
    const SnapshotModulePathResolver& module_paths) {
  const auto member = inspect_inline_local_pointer_member(
      snapshot, frame, inline_die_offset, name, member_name, module_paths);
  if (member.kind != LocalValueKind::Pointer || !member.pointee_type ||
      member.pointee_type->kind != LocalValueKind::Integer ||
      member.pointee_type->byte_size == 0 ||
      member.pointee_type->byte_size > sizeof(std::uint64_t) ||
      !member.pointee_type->members.empty()) {
    throw std::runtime_error(
        "selected-inline pointer member does not have a bounded integer pointee");
  }
  const auto address = inline_member_detail::checked_address(
      member.raw_value, 0, "selected-inline pointer-valued member");
  const auto memory = read_snapshot_memory(
      snapshot, module_paths, address, member.pointee_type->byte_size);
  LocalScalarValue result{
      member.module_path, "*(" + member.name + ")",
      inline_member_detail::decode_scalar(memory, member.pointee_type->byte_size),
      member.pointee_type->byte_size, member.pointee_type->is_signed,
      LocalValueKind::Integer};
  inline_member_detail::attach_storage(result, memory);
  return result;
}

}  // namespace mdbg

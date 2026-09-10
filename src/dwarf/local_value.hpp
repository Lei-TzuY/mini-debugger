#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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

enum class LocalValueKind { Integer, Pointer, Floating, Structure, Union, Array, Enumeration };
enum class LocalDiscoveryKind { Variable, FormalParameter };
enum class LocalValueStorage {
  Computed,
  SnapshotCoreMemory,
  SnapshotCoreRegister,
  SnapshotRuntimeArtifact
};

struct LocalDiscoveryEntry {
  std::string name;
  LocalDiscoveryKind kind;
};

struct LocalPointerPointeeType {
  std::size_t byte_size;
  bool is_signed;
};

struct LocalBitSlice {
  std::size_t bit_offset;
  std::size_t bit_size;
};

struct LocalStructMemberType {
  std::string name;
  std::size_t offset;
  std::size_t byte_size;
  bool is_signed;
  LocalValueKind kind{LocalValueKind::Integer};
  std::optional<LocalPointerPointeeType> pointee_type{};
  std::optional<LocalBitSlice> bit_slice{};
};

struct LocalPointeeType {
  std::size_t byte_size;
  bool is_signed;
  LocalValueKind kind{LocalValueKind::Integer};
  std::vector<LocalStructMemberType> members{};
};

struct LocalStructMember {
  std::string name;
  std::uint64_t raw_value;
  std::size_t byte_size;
  bool is_signed;
  LocalValueKind kind{LocalValueKind::Integer};
  std::optional<LocalPointerPointeeType> pointee_type{};
  std::optional<LocalBitSlice> bit_slice{};
};

struct LocalArrayElement {
  std::uint64_t raw_value;
  std::size_t byte_size;
  bool is_signed;
  LocalValueKind kind{LocalValueKind::Integer};
};

struct LocalArrayType {
  std::size_t element_count;
  std::size_t element_byte_size;
  bool element_is_signed;
  LocalValueKind element_kind{LocalValueKind::Integer};
};

struct LocalEnumEntry {
  std::string name;
  std::uint64_t raw_value;
};

struct LocalEnumType {
  std::string name;
  std::size_t byte_size;
  bool is_signed;
  std::vector<LocalEnumEntry> enumerators{};
};

struct LocalIntegerValue {
  std::string module_path;
  std::string name;
  std::uint64_t raw_value;
  std::size_t byte_size;
  bool is_signed;
  LocalValueKind kind{LocalValueKind::Integer};
  std::vector<LocalStructMember> members{};
  LocalValueStorage storage{LocalValueStorage::Computed};
  std::string storage_module_path{};
  std::string storage_file_path{};
  std::uint64_t storage_file_offset{0};
  std::optional<LocalPointeeType> pointee_type{};
  std::vector<LocalArrayElement> elements{};
  std::optional<LocalArrayType> array_type{};
  std::optional<LocalEnumType> enum_type{};
};

using LocalScalarValue = LocalIntegerValue;

inline std::optional<std::string_view> local_enum_symbol(const LocalScalarValue& value) {
  if (value.kind != LocalValueKind::Enumeration || !value.enum_type) {
    return std::nullopt;
  }
  for (const auto& enumerator : value.enum_type->enumerators) {
    if (enumerator.raw_value == value.raw_value) return enumerator.name;
  }
  return std::nullopt;
}

LocalScalarValue inspect_local_value(const Debugger& debugger,
                                     const ElfFile& preferred_elf,
                                     std::string_view name);
LocalScalarValue inspect_local_value(const Debugger& debugger,
                                     const ElfFile& preferred_elf,
                                     const InspectionFrameContext& frame,
                                     std::string_view name);
LocalScalarValue inspect_local_value(const CoreSnapshot& snapshot,
                                     const SnapshotInspectionFrameContext& frame,
                                     std::string_view name);
LocalScalarValue inspect_local_value(const CoreSnapshot& snapshot,
                                     const SnapshotInspectionFrameContext& frame,
                                     std::string_view name,
                                     const SnapshotModulePathResolver& module_paths);
std::vector<LocalDiscoveryEntry> discover_local_values(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    const SnapshotModulePathResolver& module_paths);
LocalScalarValue dereference_local_pointer(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::string_view name, const SnapshotModulePathResolver& module_paths);
LocalScalarValue inspect_local_pointer_member(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::string_view name, std::string_view member_name,
    const SnapshotModulePathResolver& module_paths);
LocalScalarValue dereference_local_pointer_member(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::string_view name, std::string_view member_name,
    const SnapshotModulePathResolver& module_paths);

LocalIntegerValue inspect_local_integer(const Debugger& debugger,
                                        const ElfFile& preferred_elf,
                                        std::string_view name);
LocalIntegerValue inspect_local_integer(const Debugger& debugger,
                                        const ElfFile& preferred_elf,
                                        const InspectionFrameContext& frame,
                                        std::string_view name);
LocalIntegerValue inspect_local_integer(const CoreSnapshot& snapshot,
                                        const SnapshotInspectionFrameContext& frame,
                                        std::string_view name);
LocalIntegerValue inspect_local_integer(const CoreSnapshot& snapshot,
                                        const SnapshotInspectionFrameContext& frame,
                                        std::string_view name,
                                        const SnapshotModulePathResolver& module_paths);

}  // namespace mdbg

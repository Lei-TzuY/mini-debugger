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

enum class LocalValueKind { Integer, Pointer, Floating, Structure };
enum class LocalValueStorage {
  Computed,
  SnapshotCoreMemory,
  SnapshotCoreRegister,
  SnapshotRuntimeArtifact
};

struct LocalPointeeType {
  std::size_t byte_size;
  bool is_signed;
  LocalValueKind kind{LocalValueKind::Integer};
};

struct LocalStructMember {
  std::string name;
  std::uint64_t raw_value;
  std::size_t byte_size;
  bool is_signed;
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
};

using LocalScalarValue = LocalIntegerValue;

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
LocalScalarValue dereference_local_pointer(
    const CoreSnapshot& snapshot, const SnapshotInspectionFrameContext& frame,
    std::string_view name, const SnapshotModulePathResolver& module_paths);

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

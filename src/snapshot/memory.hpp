#pragma once

#include "elf/elf.hpp"
#include "snapshot/module_path.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace mdbg {

enum class SnapshotMemoryProvenance { Core, RuntimeArtifact };

struct SnapshotMemoryRead {
  std::vector<std::byte> bytes;
  SnapshotMemoryProvenance provenance;
  std::string module_path;
  std::string module_file_path;
  std::uint64_t artifact_file_offset{0};
};

inline SnapshotMemoryRead read_snapshot_memory(
    const CoreSnapshot& snapshot, const SnapshotModulePathResolver& module_paths,
    std::uintptr_t address, std::size_t length) {
  if (length == 0) {
    throw std::invalid_argument("snapshot memory read length must be non-zero");
  }
  constexpr std::size_t kMaxReadLength = 4096;
  if (length > kMaxReadLength) {
    throw std::invalid_argument("snapshot memory read length exceeds bounded limit");
  }
  const auto start = static_cast<std::uint64_t>(address);
  if (length > std::numeric_limits<std::uint64_t>::max() - start) {
    throw std::invalid_argument("snapshot memory read range overflows");
  }
  const auto end = start + static_cast<std::uint64_t>(length);

  std::vector<std::byte> core_bytes;
  core_bytes.reserve(length);
  bool any_captured = false;
  bool any_omitted = false;
  for (std::size_t index = 0; index < length; ++index) {
    try {
      const auto byte = snapshot.read_memory(address + index, 1);
      core_bytes.push_back(byte.front());
      any_captured = true;
    } catch (const std::runtime_error&) {
      core_bytes.push_back(std::byte{0});
      any_omitted = true;
    }
  }

  if (!any_omitted) {
    return SnapshotMemoryRead{std::move(core_bytes), SnapshotMemoryProvenance::Core, {}, {}, 0};
  }
  if (any_captured) {
    throw std::runtime_error(
        "snapshot memory range mixes captured and omitted bytes; artifact fallback refused");
  }

  const auto mapping = snapshot.mapping_for_address(address);
  if (!mapping || end > mapping->end) {
    throw std::runtime_error(
        "omitted snapshot memory range is not wholly owned by one NT_FILE mapping");
  }
  if (mapping->path.empty() || mapping->path.front() != '/') {
    throw std::runtime_error("omitted snapshot memory lacks an absolute NT_FILE owner");
  }

  const auto delta = start - mapping->start;
  if (delta > std::numeric_limits<std::uint64_t>::max() - mapping->file_offset) {
    throw std::runtime_error("snapshot artifact file offset overflows");
  }
  const auto file_offset = mapping->file_offset + delta;
  const auto module_file_path = module_paths.resolve(mapping->path);

  std::ifstream input(module_file_path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open snapshot runtime artifact: " + module_file_path);
  }
  input.seekg(0, std::ios::end);
  const auto file_size = input.tellg();
  if (file_size < 0 || file_offset > static_cast<std::uint64_t>(file_size) ||
      length > static_cast<std::uint64_t>(file_size) - file_offset) {
    throw std::runtime_error("snapshot runtime artifact range extends past end of file");
  }
  input.seekg(static_cast<std::streamoff>(file_offset), std::ios::beg);
  std::vector<std::byte> bytes(length);
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
  if (!input) throw std::runtime_error("failed to read snapshot runtime artifact bytes");

  return SnapshotMemoryRead{std::move(bytes), SnapshotMemoryProvenance::RuntimeArtifact,
                            mapping->path, module_file_path, file_offset};
}

}  // namespace mdbg

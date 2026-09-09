#pragma once

#include <elf.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mdbg {
namespace snapshot_artifact_detail {

inline std::vector<std::byte> read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open snapshot artifact: " + path.string());
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0) throw std::runtime_error("failed to determine snapshot artifact size");
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(length));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input) throw std::runtime_error("failed to read snapshot artifact");
  }
  return bytes;
}

template <typename T>
inline T read_struct(const std::vector<std::byte>& bytes, std::size_t offset,
                     const char* what) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    throw std::runtime_error(std::string(what) + " extends past end of file");
  }
  T result{};
  std::memcpy(&result, bytes.data() + offset, sizeof(result));
  return result;
}

inline void require_range(const std::vector<std::byte>& bytes, std::uint64_t offset,
                          std::uint64_t size, const char* what) {
  if (offset > bytes.size() || size > bytes.size() - static_cast<std::size_t>(offset)) {
    throw std::runtime_error(std::string(what) + " extends past end of file");
  }
}

inline std::string section_name(const std::vector<std::byte>& bytes,
                                const Elf64_Shdr& strings, std::uint32_t offset) {
  if (offset >= strings.sh_size) {
    throw std::runtime_error("ELF section name offset is out of range");
  }
  const auto begin = strings.sh_offset + offset;
  const auto end = strings.sh_offset + strings.sh_size;
  std::string result;
  for (auto cursor = begin; cursor < end; ++cursor) {
    const char character = static_cast<char>(
        std::to_integer<unsigned char>(bytes[static_cast<std::size_t>(cursor)]));
    if (character == '\0') return result;
    result.push_back(character);
  }
  throw std::runtime_error("ELF section name is not terminated");
}

struct GnuDebugLink {
  std::string filename;
  std::uint32_t crc32;
};

inline std::optional<GnuDebugLink> read_gnu_debuglink(
    const std::filesystem::path& runtime_module) {
  const auto bytes = read_file(runtime_module);
  const auto header = read_struct<Elf64_Ehdr>(bytes, 0, "ELF header");
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
      header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_X86_64) {
    throw std::runtime_error("snapshot runtime module is not supported x86-64 ELF64");
  }
  if (header.e_shnum == 0 || header.e_shstrndx == SHN_UNDEF) return std::nullopt;
  if (header.e_shentsize != sizeof(Elf64_Shdr) || header.e_shstrndx == SHN_XINDEX ||
      header.e_shstrndx >= header.e_shnum) {
    throw std::runtime_error("unsupported ELF section table for GNU debuglink");
  }
  const auto table_size = static_cast<std::uint64_t>(header.e_shnum) * sizeof(Elf64_Shdr);
  require_range(bytes, header.e_shoff, table_size, "ELF section table");

  std::vector<Elf64_Shdr> sections;
  sections.reserve(header.e_shnum);
  for (std::size_t index = 0; index < header.e_shnum; ++index) {
    sections.push_back(read_struct<Elf64_Shdr>(
        bytes, static_cast<std::size_t>(header.e_shoff) + index * sizeof(Elf64_Shdr),
        "ELF section header"));
  }
  const auto& names = sections[header.e_shstrndx];
  require_range(bytes, names.sh_offset, names.sh_size, "ELF section-name table");

  std::optional<GnuDebugLink> result;
  for (const auto& section : sections) {
    if (section_name(bytes, names, section.sh_name) != ".gnu_debuglink") continue;
    if (result) throw std::runtime_error("runtime ELF contains duplicate .gnu_debuglink sections");
    require_range(bytes, section.sh_offset, section.sh_size, ".gnu_debuglink");
    if (section.sh_size < 5) throw std::runtime_error(".gnu_debuglink section is truncated");

    std::string filename;
    std::size_t relative = 0;
    bool terminated = false;
    for (; relative < section.sh_size; ++relative) {
      const auto character = static_cast<char>(std::to_integer<unsigned char>(
          bytes[static_cast<std::size_t>(section.sh_offset) + relative]));
      if (character == '\0') {
        terminated = true;
        break;
      }
      filename.push_back(character);
    }
    if (!terminated || filename.empty() ||
        std::filesystem::path(filename).filename().string() != filename) {
      throw std::runtime_error(".gnu_debuglink contains an invalid filename");
    }

    const auto crc_relative = (relative + 1U + 3U) & ~std::size_t{3U};
    if (crc_relative > section.sh_size ||
        sizeof(std::uint32_t) != section.sh_size - crc_relative) {
      throw std::runtime_error(".gnu_debuglink has an invalid CRC field");
    }
    for (std::size_t padding = relative + 1U; padding < crc_relative; ++padding) {
      if (bytes[static_cast<std::size_t>(section.sh_offset) + padding] != std::byte{0}) {
        throw std::runtime_error(".gnu_debuglink contains non-zero padding");
      }
    }
    std::uint32_t crc = 0;
    for (std::size_t byte = 0; byte < sizeof(crc); ++byte) {
      crc |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(
                 bytes[static_cast<std::size_t>(section.sh_offset) + crc_relative + byte]))
             << (byte * 8U);
    }
    result = GnuDebugLink{std::move(filename), crc};
  }
  return result;
}

inline std::uint32_t gnu_debuglink_crc32(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open debug companion: " + path.string());
  std::uint32_t crc = 0xffffffffU;
  std::array<char, 8192> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    for (std::streamsize index = 0; index < count; ++index) {
      crc ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
      }
    }
  }
  if (!input.eof()) throw std::runtime_error("failed to read debug companion");
  return crc ^ 0xffffffffU;
}

}  // namespace snapshot_artifact_detail

class SnapshotModulePathResolver {
 public:
  void add_substitution(std::filesystem::path from, std::filesystem::path to) {
    from = normalized(std::move(from));
    to = normalized(std::move(to));
    if (from.empty() || from == ".") {
      throw std::invalid_argument("snapshot module substitution prefix must not be empty");
    }
    if (to.empty()) {
      throw std::invalid_argument("snapshot module substitution target must not be empty");
    }

    for (auto& substitution : substitutions_) {
      if (substitution.from == from) {
        substitution.to = std::move(to);
        return;
      }
    }
    substitutions_.push_back(Substitution{std::move(from), std::move(to)});
  }

  void add_debug_file(std::filesystem::path recorded_module,
                      std::filesystem::path local_debug_file) {
    recorded_module = normalized(std::move(recorded_module));
    local_debug_file = normalized(std::move(local_debug_file));
    if (recorded_module.empty() || !recorded_module.is_absolute()) {
      throw std::invalid_argument("snapshot debug-file module identity must be absolute");
    }
    if (local_debug_file.empty()) {
      throw std::invalid_argument("snapshot debug-file path must not be empty");
    }
    for (auto& mapping : debug_files_) {
      if (mapping.recorded_module == recorded_module) {
        mapping.local_debug_file = std::move(local_debug_file);
        return;
      }
    }
    debug_files_.push_back(DebugFile{std::move(recorded_module),
                                     std::move(local_debug_file)});
  }

  [[nodiscard]] std::string resolve(const std::string& recorded_module) const {
    if (recorded_module.empty()) {
      throw std::invalid_argument("snapshot module path must not be empty");
    }
    const std::filesystem::path recorded(recorded_module);
    if (!recorded.is_absolute()) {
      throw std::runtime_error("snapshot NT_FILE mapping lacks an absolute module path");
    }

    const auto normalized_recorded = normalized(recorded);
    if (const auto substituted = substituted_candidate(normalized_recorded)) {
      std::error_code error;
      if (std::filesystem::is_regular_file(*substituted, error)) {
        return substituted->string();
      }
      throw std::runtime_error("substituted snapshot module file is unavailable: " +
                               substituted->string());
    }

    std::error_code error;
    if (std::filesystem::is_regular_file(normalized_recorded, error)) {
      return normalized_recorded.string();
    }
    throw std::runtime_error("snapshot module file is unavailable: " + recorded_module);
  }

  [[nodiscard]] std::string resolve_debug_file(const std::string& recorded_module) const {
    const auto runtime_file = std::filesystem::path(resolve(recorded_module));
    const auto normalized_recorded = normalized(std::filesystem::path(recorded_module));
    const auto mapping = std::find_if(debug_files_.begin(), debug_files_.end(),
                                      [&](const DebugFile& candidate) {
                                        return candidate.recorded_module == normalized_recorded;
                                      });
    if (mapping == debug_files_.end()) return runtime_file.string();

    std::error_code error;
    if (!std::filesystem::is_regular_file(mapping->local_debug_file, error)) {
      throw std::runtime_error("snapshot debug companion is unavailable: " +
                               mapping->local_debug_file.string());
    }
    const auto link = snapshot_artifact_detail::read_gnu_debuglink(runtime_file);
    if (!link) {
      throw std::runtime_error("snapshot runtime module has no .gnu_debuglink: " +
                               runtime_file.string());
    }
    if (mapping->local_debug_file.filename().string() != link->filename) {
      throw std::runtime_error("debug companion filename does not match GNU debuglink: " +
                               mapping->local_debug_file.string());
    }
    if (snapshot_artifact_detail::gnu_debuglink_crc32(mapping->local_debug_file) !=
        link->crc32) {
      throw std::runtime_error("debug companion CRC mismatch: " +
                               mapping->local_debug_file.string());
    }
    return mapping->local_debug_file.string();
  }

 private:
  struct Substitution {
    std::filesystem::path from;
    std::filesystem::path to;
  };

  struct DebugFile {
    std::filesystem::path recorded_module;
    std::filesystem::path local_debug_file;
  };

  static std::filesystem::path normalized(std::filesystem::path path) {
    return path.lexically_normal();
  }

  static bool has_component_prefix(const std::filesystem::path& path,
                                   const std::filesystem::path& prefix) {
    auto path_it = path.begin();
    for (auto prefix_it = prefix.begin(); prefix_it != prefix.end(); ++prefix_it, ++path_it) {
      if (path_it == path.end() || *path_it != *prefix_it) return false;
    }
    return true;
  }

  static std::size_t component_count(const std::filesystem::path& path) {
    std::size_t count = 0;
    for (auto it = path.begin(); it != path.end(); ++it) ++count;
    return count;
  }

  static std::filesystem::path suffix_after(const std::filesystem::path& path,
                                            const std::filesystem::path& prefix) {
    auto path_it = path.begin();
    for (auto prefix_it = prefix.begin(); prefix_it != prefix.end(); ++prefix_it) ++path_it;
    std::filesystem::path suffix;
    for (; path_it != path.end(); ++path_it) suffix /= *path_it;
    return suffix;
  }

  [[nodiscard]] std::optional<std::filesystem::path> substituted_candidate(
      const std::filesystem::path& candidate) const {
    const Substitution* best = nullptr;
    std::size_t best_components = 0;
    for (const auto& substitution : substitutions_) {
      if (!has_component_prefix(candidate, substitution.from)) continue;
      const auto components = component_count(substitution.from);
      if (!best || components > best_components) {
        best = &substitution;
        best_components = components;
      }
    }
    if (!best) return std::nullopt;
    const auto suffix = suffix_after(candidate, best->from);
    if (suffix.empty()) return best->to;
    return normalized(best->to / suffix);
  }

  std::vector<Substitution> substitutions_;
  std::vector<DebugFile> debug_files_;
};

}  // namespace mdbg

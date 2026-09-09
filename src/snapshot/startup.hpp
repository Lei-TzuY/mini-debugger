#pragma once

#include "elf/elf.hpp"

#include <elf.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mdbg {

struct CoreAuxvEntry {
  std::uint64_t type;
  std::uint64_t value;
};

struct CoreStartupInfo {
  std::optional<std::uint64_t> entry_point;
  std::optional<std::uint64_t> program_headers;
  std::optional<std::uint64_t> program_header_count;
  std::optional<std::uint64_t> page_size;
  std::optional<std::uint64_t> interpreter_base;
  std::vector<CoreAuxvEntry> entries;
};

namespace startup_detail {

inline std::size_t align4(std::size_t value) {
  if (value > static_cast<std::size_t>(-1) - 3U) {
    throw std::runtime_error("core startup note alignment overflow");
  }
  return (value + 3U) & ~std::size_t{3U};
}

inline std::vector<std::byte> read_core_bytes(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open core file for startup metadata");
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0) throw std::runtime_error("failed to determine core startup file size");
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(length));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input) throw std::runtime_error("failed to read core startup metadata");
  }
  return bytes;
}

inline std::string owner(const std::vector<std::byte>& bytes, std::size_t offset,
                         std::size_t size) {
  if (offset > bytes.size() || size > bytes.size() - offset) {
    throw std::runtime_error("core startup note owner extends past file");
  }
  std::string result;
  for (std::size_t index = 0; index < size; ++index) {
    const char value = static_cast<char>(
        std::to_integer<unsigned char>(bytes[offset + index]));
    if (value == '\0') break;
    result.push_back(value);
  }
  return result;
}

inline void assign_once(std::optional<std::uint64_t>& target, std::uint64_t value,
                        const char* label) {
  if (target) throw std::runtime_error(std::string("duplicate NT_AUXV ") + label);
  target = value;
}

}  // namespace startup_detail

inline std::optional<CoreStartupInfo> read_core_startup_info(
    const CoreSnapshot& snapshot) {
  constexpr std::size_t kMaxAuxvBytes = 4096;
  const auto bytes = startup_detail::read_core_bytes(snapshot.path());
  if (bytes.size() < sizeof(Elf64_Ehdr)) {
    throw std::runtime_error("core startup metadata lacks ELF header");
  }

  Elf64_Ehdr header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  if (header.e_phentsize != sizeof(Elf64_Phdr)) {
    throw std::runtime_error("unsupported core startup program-header size");
  }

  std::optional<CoreStartupInfo> result;
  for (std::size_t ph_index = 0; ph_index < header.e_phnum; ++ph_index) {
    const auto ph_offset = static_cast<std::size_t>(header.e_phoff) +
                           ph_index * sizeof(Elf64_Phdr);
    if (ph_offset > bytes.size() || sizeof(Elf64_Phdr) > bytes.size() - ph_offset) {
      throw std::runtime_error("core startup program header extends past file");
    }
    Elf64_Phdr phdr{};
    std::memcpy(&phdr, bytes.data() + ph_offset, sizeof(phdr));
    if (phdr.p_type != PT_NOTE) continue;
    const auto begin = static_cast<std::size_t>(phdr.p_offset);
    const auto size = static_cast<std::size_t>(phdr.p_filesz);
    if (begin > bytes.size() || size > bytes.size() - begin) {
      throw std::runtime_error("core startup PT_NOTE extends past file");
    }
    const auto end = begin + size;
    auto cursor = begin;
    while (cursor < end) {
      if (sizeof(Elf64_Nhdr) > end - cursor) {
        throw std::runtime_error("truncated core startup note header");
      }
      Elf64_Nhdr note{};
      std::memcpy(&note, bytes.data() + cursor, sizeof(note));
      cursor += sizeof(note);
      const auto name_padded = startup_detail::align4(note.n_namesz);
      if (name_padded > end - cursor) {
        throw std::runtime_error("truncated core startup note name");
      }
      const auto note_owner = startup_detail::owner(bytes, cursor, note.n_namesz);
      cursor += name_padded;
      const auto desc_offset = cursor;
      const auto desc_padded = startup_detail::align4(note.n_descsz);
      if (desc_padded > end - cursor) {
        throw std::runtime_error("truncated core startup note payload");
      }
      cursor += desc_padded;
      if (note_owner != "CORE" || note.n_type != NT_AUXV) continue;
      if (result) throw std::runtime_error("duplicate NT_AUXV note");
      if (note.n_descsz == 0 || note.n_descsz > kMaxAuxvBytes ||
          note.n_descsz % sizeof(Elf64_auxv_t) != 0) {
        throw std::runtime_error("unsupported bounded x86-64 NT_AUXV payload");
      }
      if (desc_offset > bytes.size() || note.n_descsz > bytes.size() - desc_offset) {
        throw std::runtime_error("NT_AUXV payload extends past core file");
      }

      CoreStartupInfo info;
      bool saw_null = false;
      const auto count = note.n_descsz / sizeof(Elf64_auxv_t);
      info.entries.reserve(count);
      for (std::size_t index = 0; index < count; ++index) {
        Elf64_auxv_t entry{};
        std::memcpy(&entry, bytes.data() + desc_offset + index * sizeof(entry),
                    sizeof(entry));
        if (entry.a_type == AT_NULL) {
          saw_null = true;
          break;
        }
        info.entries.push_back(CoreAuxvEntry{entry.a_type, entry.a_un.a_val});
        switch (entry.a_type) {
          case AT_ENTRY:
            startup_detail::assign_once(info.entry_point, entry.a_un.a_val, "AT_ENTRY");
            break;
          case AT_PHDR:
            startup_detail::assign_once(info.program_headers, entry.a_un.a_val, "AT_PHDR");
            break;
          case AT_PHNUM:
            startup_detail::assign_once(info.program_header_count, entry.a_un.a_val,
                                        "AT_PHNUM");
            break;
          case AT_PAGESZ:
            startup_detail::assign_once(info.page_size, entry.a_un.a_val, "AT_PAGESZ");
            break;
          case AT_BASE:
            startup_detail::assign_once(info.interpreter_base, entry.a_un.a_val, "AT_BASE");
            break;
          default:
            break;
        }
      }
      if (!saw_null) throw std::runtime_error("NT_AUXV lacks AT_NULL terminator");
      if (info.page_size &&
          (*info.page_size == 0 || (*info.page_size & (*info.page_size - 1U)) != 0)) {
        throw std::runtime_error("NT_AUXV AT_PAGESZ is not a power of two");
      }
      if (info.program_header_count && *info.program_header_count == 0) {
        throw std::runtime_error("NT_AUXV AT_PHNUM is zero");
      }
      if (info.entry_point) {
        if (*info.entry_point == 0 || !snapshot.mapping_for_address(*info.entry_point)) {
          throw std::runtime_error("NT_AUXV AT_ENTRY is outside NT_FILE ownership");
        }
      }
      if (info.program_headers) {
        if (*info.program_headers == 0 || !snapshot.mapping_for_address(*info.program_headers)) {
          throw std::runtime_error("NT_AUXV AT_PHDR is outside NT_FILE ownership");
        }
      }
      if (info.interpreter_base && *info.interpreter_base != 0 &&
          !snapshot.mapping_for_address(*info.interpreter_base)) {
        throw std::runtime_error("NT_AUXV AT_BASE is outside NT_FILE ownership");
      }
      result = std::move(info);
    }
  }
  return result;
}

}  // namespace mdbg

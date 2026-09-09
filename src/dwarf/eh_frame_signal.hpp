#pragma once

#include <elf.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mdbg {
namespace eh_signal_detail {

constexpr std::uint8_t kPcrelSdata4 = 0x1b;
constexpr std::uint8_t kPcrelSdata8 = 0x1c;
constexpr std::uint8_t kPcrelIndirectSdata4 = 0x9b;
constexpr std::uint8_t kPcrelIndirectSdata8 = 0x9c;

struct CieMetadata {
  std::uint8_t fde_encoding{0};
  bool signal_frame{false};
};

template <typename T>
inline T read_at(const std::vector<std::byte>& bytes, std::size_t offset,
                 const char* what) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    throw std::runtime_error(std::string(what) + " extends past end of data");
  }
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

template <typename T>
inline T read_scalar(const std::vector<std::byte>& bytes, std::size_t& cursor,
                     std::size_t limit, const char* what) {
  if (cursor > limit || sizeof(T) > limit - cursor) {
    throw std::runtime_error(std::string(what) + " extends past CFI entry boundary");
  }
  const auto value = read_at<T>(bytes, cursor, what);
  cursor += sizeof(T);
  return value;
}

inline std::uint64_t read_uleb(const std::vector<std::byte>& bytes,
                               std::size_t& cursor, std::size_t limit) {
  std::uint64_t result = 0;
  unsigned shift = 0;
  for (unsigned count = 0; count < 10; ++count) {
    const auto byte = read_scalar<std::uint8_t>(bytes, cursor, limit, "ULEB128");
    const auto payload = static_cast<std::uint64_t>(byte & 0x7fU);
    if (shift > 63 || (shift == 63 && payload > 1)) {
      throw std::runtime_error("ULEB128 overflows 64 bits");
    }
    result |= payload << shift;
    if ((byte & 0x80U) == 0) return result;
    shift += 7;
  }
  throw std::runtime_error("ULEB128 is too long");
}

inline std::int64_t read_sleb(const std::vector<std::byte>& bytes,
                              std::size_t& cursor, std::size_t limit) {
  std::uint64_t result = 0;
  unsigned shift = 0;
  for (unsigned count = 0; count < 10; ++count) {
    const auto byte = read_scalar<std::uint8_t>(bytes, cursor, limit, "SLEB128");
    result |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
    shift += 7;
    if ((byte & 0x80U) == 0) {
      if ((byte & 0x40U) != 0 && shift < 64) {
        result |= (~std::uint64_t{0}) << shift;
      }
      return static_cast<std::int64_t>(result);
    }
  }
  throw std::runtime_error("SLEB128 is too long");
}

inline std::string read_c_string(const std::vector<std::byte>& bytes,
                                 std::size_t& cursor, std::size_t limit,
                                 const char* what) {
  std::string result;
  while (cursor < limit) {
    const auto value = std::to_integer<unsigned char>(bytes[cursor++]);
    if (value == 0) return result;
    result.push_back(static_cast<char>(value));
  }
  throw std::runtime_error(std::string("unterminated ") + what);
}

inline bool signed_pcrel_encoding(std::uint8_t encoding) {
  return encoding == kPcrelSdata4 || encoding == kPcrelSdata8;
}

inline bool indirect_signed_pcrel_encoding(std::uint8_t encoding) {
  return encoding == kPcrelIndirectSdata4 || encoding == kPcrelIndirectSdata8;
}

inline std::int64_t read_signed_pointer(const std::vector<std::byte>& bytes,
                                        std::size_t& cursor, std::size_t limit,
                                        std::uint8_t encoding, const char* what) {
  switch (encoding) {
    case kPcrelSdata4:
    case kPcrelIndirectSdata4:
      return read_scalar<std::int32_t>(bytes, cursor, limit, what);
    case kPcrelSdata8:
    case kPcrelIndirectSdata8:
      return read_scalar<std::int64_t>(bytes, cursor, limit, what);
    default:
      throw std::runtime_error("unsupported signal-frame CFI pointer encoding");
  }
}

inline std::uint64_t add_signed(std::uint64_t base, std::int64_t offset,
                                const char* what) {
  if (offset >= 0) {
    const auto value = static_cast<std::uint64_t>(offset);
    if (value > std::numeric_limits<std::uint64_t>::max() - base) {
      throw std::runtime_error(std::string(what) + " overflows address space");
    }
    return base + value;
  }
  const auto value = static_cast<std::uint64_t>(-(offset + 1)) + 1;
  if (value > base) {
    throw std::runtime_error(std::string(what) + " underflows address space");
  }
  return base - value;
}

inline std::vector<std::byte> read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open ELF file: " + path);
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0) throw std::runtime_error("failed to determine ELF file size");
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(length));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input) throw std::runtime_error("failed to read ELF file");
  }
  return bytes;
}

inline std::optional<std::pair<std::uint64_t, std::vector<std::byte>>> find_eh_frame(
    const std::vector<std::byte>& bytes) {
  const auto header = read_at<Elf64_Ehdr>(bytes, 0, "ELF header");
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
      header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_X86_64) {
    throw std::runtime_error("signal-frame metadata requires little-endian x86-64 ELF64");
  }
  if (header.e_shnum == 0 || header.e_shstrndx == SHN_UNDEF) return std::nullopt;
  if (header.e_shentsize != sizeof(Elf64_Shdr) || header.e_shstrndx == SHN_XINDEX ||
      header.e_shstrndx >= header.e_shnum) {
    throw std::runtime_error("unsupported ELF section layout for signal-frame metadata");
  }

  std::vector<Elf64_Shdr> sections;
  sections.reserve(header.e_shnum);
  for (std::size_t index = 0; index < header.e_shnum; ++index) {
    sections.push_back(read_at<Elf64_Shdr>(
        bytes, static_cast<std::size_t>(header.e_shoff) + index * sizeof(Elf64_Shdr),
        "ELF section header"));
  }
  const auto& names = sections[header.e_shstrndx];
  if (names.sh_offset > bytes.size() || names.sh_size > bytes.size() - names.sh_offset) {
    throw std::runtime_error("ELF section-name table extends past end of file");
  }
  const auto names_begin = static_cast<std::size_t>(names.sh_offset);
  const auto names_end = names_begin + static_cast<std::size_t>(names.sh_size);

  for (const auto& section : sections) {
    if (section.sh_name >= names.sh_size) continue;
    auto name_cursor = names_begin + static_cast<std::size_t>(section.sh_name);
    if (read_c_string(bytes, name_cursor, names_end, "ELF section name") != ".eh_frame") {
      continue;
    }
    if ((section.sh_flags & SHF_COMPRESSED) != 0) {
      throw std::runtime_error("compressed .eh_frame is unsupported");
    }
    if (section.sh_offset > bytes.size() || section.sh_size > bytes.size() - section.sh_offset) {
      throw std::runtime_error(".eh_frame extends past end of file");
    }
    const auto begin = static_cast<std::size_t>(section.sh_offset);
    const auto end = begin + static_cast<std::size_t>(section.sh_size);
    return std::pair<std::uint64_t, std::vector<std::byte>>{
        section.sh_addr, {bytes.begin() + begin, bytes.begin() + end}};
  }
  return std::nullopt;
}

inline CieMetadata parse_cie_metadata(const std::vector<std::byte>& bytes,
                                      std::size_t cursor, std::size_t end) {
  const auto version = read_scalar<std::uint8_t>(bytes, cursor, end, "CIE version");
  if (version != 1) {
    throw std::runtime_error("only .eh_frame CIE version 1 is supported");
  }
  const auto augmentation = read_c_string(bytes, cursor, end, "CIE augmentation");
  if (augmentation.empty() || augmentation.front() != 'z') {
    throw std::runtime_error("only z-prefixed .eh_frame augmentation is supported");
  }

  static_cast<void>(read_uleb(bytes, cursor, end));
  static_cast<void>(read_sleb(bytes, cursor, end));
  static_cast<void>(read_scalar<std::uint8_t>(bytes, cursor, end, "CIE return register"));
  const auto augmentation_size = read_uleb(bytes, cursor, end);
  if (augmentation_size > end - cursor) {
    throw std::runtime_error("CIE augmentation extends past entry boundary");
  }
  const auto augmentation_end = cursor + static_cast<std::size_t>(augmentation_size);

  CieMetadata metadata;
  bool has_fde_encoding = false;
  for (std::size_t index = 1; index < augmentation.size(); ++index) {
    switch (augmentation[index]) {
      case 'P': {
        const auto encoding = read_scalar<std::uint8_t>(
            bytes, cursor, augmentation_end, "personality encoding");
        if (!indirect_signed_pcrel_encoding(encoding)) {
          throw std::runtime_error("unsupported .eh_frame personality pointer encoding");
        }
        static_cast<void>(read_signed_pointer(bytes, cursor, augmentation_end, encoding,
                                              "personality pointer"));
        break;
      }
      case 'L': {
        const auto encoding =
            read_scalar<std::uint8_t>(bytes, cursor, augmentation_end, "LSDA encoding");
        if (!signed_pcrel_encoding(encoding)) {
          throw std::runtime_error("unsupported .eh_frame LSDA pointer encoding");
        }
        break;
      }
      case 'R':
        metadata.fde_encoding =
            read_scalar<std::uint8_t>(bytes, cursor, augmentation_end, "FDE encoding");
        if (!signed_pcrel_encoding(metadata.fde_encoding)) {
          throw std::runtime_error("unsupported .eh_frame FDE pointer encoding");
        }
        has_fde_encoding = true;
        break;
      case 'S':
        metadata.signal_frame = true;
        break;
      default:
        throw std::runtime_error("unsupported .eh_frame CIE augmentation descriptor");
    }
  }
  if (!has_fde_encoding) {
    throw std::runtime_error("CIE augmentation does not define an FDE pointer encoding");
  }
  if (cursor != augmentation_end) {
    throw std::runtime_error("unexpected trailing CIE augmentation data");
  }
  return metadata;
}

}  // namespace eh_signal_detail

class EhFrameSignalIndex {
 public:
  explicit EhFrameSignalIndex(const std::string& path) {
    const auto section = eh_signal_detail::find_eh_frame(eh_signal_detail::read_file(path));
    if (!section) return;
    section_virtual_address_ = section->first;
    section_ = section->second;
    available_ = true;
  }

  [[nodiscard]] bool available() const noexcept { return available_; }

  [[nodiscard]] bool is_signal_frame(std::uint64_t target) const {
    if (!available_) return false;
    std::map<std::size_t, eh_signal_detail::CieMetadata> cies;
    std::size_t entry = 0;
    while (entry < section_.size()) {
      const auto length =
          eh_signal_detail::read_at<std::uint32_t>(section_, entry, ".eh_frame length");
      if (length == 0) break;
      if (length == 0xffffffffU) {
        throw std::runtime_error("DWARF64 .eh_frame is unsupported");
      }
      const auto content = entry + sizeof(std::uint32_t);
      if (length > section_.size() - content) {
        throw std::runtime_error(".eh_frame entry extends past section boundary");
      }
      const auto end = content + static_cast<std::size_t>(length);
      std::size_t cursor = content;
      const auto cie_pointer = eh_signal_detail::read_scalar<std::uint32_t>(
          section_, cursor, end, "CIE pointer");
      if (cie_pointer == 0) {
        cies.emplace(entry, eh_signal_detail::parse_cie_metadata(section_, cursor, end));
        entry = end;
        continue;
      }
      if (cie_pointer > content) {
        throw std::runtime_error("FDE CIE pointer underflows section");
      }
      const auto cie_it = cies.find(content - static_cast<std::size_t>(cie_pointer));
      if (cie_it == cies.end()) {
        throw std::runtime_error("FDE references an unknown CIE");
      }
      const auto& cie = cie_it->second;
      const auto field_virtual_address = section_virtual_address_ + cursor;
      const auto relative = eh_signal_detail::read_signed_pointer(
          section_, cursor, end, cie.fde_encoding, "FDE initial location");
      const auto initial_location = eh_signal_detail::add_signed(
          field_virtual_address, relative, "FDE start");
      const auto signed_range = eh_signal_detail::read_signed_pointer(
          section_, cursor, end, cie.fde_encoding, "FDE address range");
      if (signed_range < 0) {
        throw std::runtime_error("negative FDE address range");
      }
      const auto range = static_cast<std::uint64_t>(signed_range);
      if (range > std::numeric_limits<std::uint64_t>::max() - initial_location) {
        throw std::runtime_error("FDE address range overflows address space");
      }
      if (target >= initial_location && target < initial_location + range) {
        return cie.signal_frame;
      }
      entry = end;
    }
    return false;
  }

 private:
  std::vector<std::byte> section_;
  std::uint64_t section_virtual_address_{0};
  bool available_{false};
};

}  // namespace mdbg

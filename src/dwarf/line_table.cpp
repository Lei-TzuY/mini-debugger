#include "dwarf/line_table.hpp"

#include <elf.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace mdbg {
namespace {

constexpr std::uint8_t kLnsCopy = 1;
constexpr std::uint8_t kLnsAdvancePc = 2;
constexpr std::uint8_t kLnsAdvanceLine = 3;
constexpr std::uint8_t kLnsSetFile = 4;
constexpr std::uint8_t kLnsSetColumn = 5;
constexpr std::uint8_t kLnsNegateStmt = 6;
constexpr std::uint8_t kLnsSetBasicBlock = 7;
constexpr std::uint8_t kLnsConstAddPc = 8;
constexpr std::uint8_t kLnsFixedAdvancePc = 9;
constexpr std::uint8_t kLnsSetPrologueEnd = 10;
constexpr std::uint8_t kLnsSetEpilogueBegin = 11;
constexpr std::uint8_t kLnsSetIsa = 12;

constexpr std::uint8_t kLneEndSequence = 1;
constexpr std::uint8_t kLneSetAddress = 2;
constexpr std::uint8_t kLneDefineFile = 3;
constexpr std::uint8_t kLneSetDiscriminator = 4;

constexpr std::uint64_t kLnctPath = 0x01;
constexpr std::uint64_t kLnctDirectoryIndex = 0x02;

constexpr std::uint64_t kFormData2 = 0x05;
constexpr std::uint64_t kFormData4 = 0x06;
constexpr std::uint64_t kFormData8 = 0x07;
constexpr std::uint64_t kFormString = 0x08;
constexpr std::uint64_t kFormData1 = 0x0b;
constexpr std::uint64_t kFormSdata = 0x0d;
constexpr std::uint64_t kFormUdata = 0x0f;
constexpr std::uint64_t kFormData16 = 0x1e;
constexpr std::uint64_t kFormLineStrp = 0x1f;

struct FileEntry {
  std::string name;
  std::uint64_t directory_index;
};

struct EntryFormat {
  std::uint64_t content_type;
  std::uint64_t form;
};

struct SectionRange {
  std::size_t offset;
  std::size_t size;
};

struct Row {
  std::uint64_t address;
  std::optional<SourceLocation> location;
};

struct State {
  std::uint64_t address{0};
  std::uint64_t file{1};
  std::int64_t line{1};
  std::uint64_t column{0};
};

template <typename T>
T read_at(const std::vector<std::byte>& bytes, std::size_t offset, const char* what) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    throw std::runtime_error(std::string(what) + " extends past end of file");
  }
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

template <typename T>
T read_scalar(const std::vector<std::byte>& bytes, std::size_t& cursor,
              std::size_t limit, const char* what) {
  if (cursor > limit || sizeof(T) > limit - cursor) {
    throw std::runtime_error(std::string(what) + " extends past DWARF unit boundary");
  }
  const auto value = read_at<T>(bytes, cursor, what);
  cursor += sizeof(T);
  return value;
}

void require_range(const std::vector<std::byte>& bytes, std::uint64_t offset,
                   std::uint64_t size, const char* what) {
  if (offset > bytes.size() || size > bytes.size() - static_cast<std::size_t>(offset)) {
    throw std::runtime_error(std::string(what) + " extends past end of file");
  }
}

std::string read_c_string(const std::vector<std::byte>& bytes, std::size_t& cursor,
                          std::size_t limit, const char* what) {
  std::string result;
  while (cursor < limit) {
    const auto value = std::to_integer<unsigned char>(bytes[cursor++]);
    if (value == 0) return result;
    result.push_back(static_cast<char>(value));
  }
  throw std::runtime_error(std::string("unterminated ") + what);
}

std::uint64_t read_uleb(const std::vector<std::byte>& bytes, std::size_t& cursor,
                        std::size_t limit) {
  std::uint64_t result = 0;
  unsigned shift = 0;
  for (unsigned count = 0; count < 10; ++count) {
    const auto byte = read_scalar<std::uint8_t>(bytes, cursor, limit, "ULEB128 operand");
    const auto payload = static_cast<std::uint64_t>(byte & 0x7fU);
    if (shift > 63 || (shift == 63 && payload > 1)) {
      throw std::runtime_error("ULEB128 operand overflows 64 bits");
    }
    result |= payload << shift;
    if ((byte & 0x80U) == 0) return result;
    shift += 7;
  }
  throw std::runtime_error("ULEB128 operand is too long");
}

std::int64_t read_sleb(const std::vector<std::byte>& bytes, std::size_t& cursor,
                       std::size_t limit) {
  std::uint64_t result = 0;
  unsigned shift = 0;
  std::uint8_t byte = 0;
  for (unsigned count = 0; count < 10; ++count) {
    byte = read_scalar<std::uint8_t>(bytes, cursor, limit, "SLEB128 operand");
    const auto payload = static_cast<std::uint64_t>(byte & 0x7fU);
    if (shift > 63 || (shift == 63 && payload != 0 && payload != 0x7fU)) {
      throw std::runtime_error("SLEB128 operand overflows 64 bits");
    }
    result |= payload << shift;
    shift += 7;
    if ((byte & 0x80U) == 0) {
      if ((byte & 0x40U) != 0 && shift < 64) result |= (~std::uint64_t{0}) << shift;
      return static_cast<std::int64_t>(result);
    }
  }
  throw std::runtime_error("SLEB128 operand is too long");
}

std::vector<std::byte> read_file(const std::string& path) {
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

std::optional<SectionRange> find_debug_section(const std::vector<std::byte>& bytes,
                                               std::string_view wanted_name) {
  const auto header = read_at<Elf64_Ehdr>(bytes, 0, "ELF header");
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0) {
    throw std::runtime_error("file is not ELF");
  }
  if (header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB) {
    throw std::runtime_error("DWARF line parsing requires little-endian ELF64");
  }
  if (header.e_machine != EM_X86_64) {
    throw std::runtime_error("DWARF line parsing currently supports x86-64 ELF only");
  }
  if (header.e_shnum == 0) return std::nullopt;
  if (header.e_shentsize != sizeof(Elf64_Shdr)) {
    throw std::runtime_error("unsupported ELF section-header entry size");
  }
  if (header.e_shstrndx == SHN_XINDEX) {
    throw std::runtime_error("ELF extended section numbering is unsupported");
  }
  if (header.e_shstrndx == SHN_UNDEF || header.e_shstrndx >= header.e_shnum) {
    return std::nullopt;
  }

  std::vector<Elf64_Shdr> sections;
  sections.reserve(header.e_shnum);
  for (std::size_t index = 0; index < header.e_shnum; ++index) {
    const auto offset = static_cast<std::size_t>(header.e_shoff) + index * sizeof(Elf64_Shdr);
    sections.push_back(read_at<Elf64_Shdr>(bytes, offset, "ELF section header"));
  }

  const auto& names = sections[header.e_shstrndx];
  require_range(bytes, names.sh_offset, names.sh_size, "ELF section-name table");
  const auto names_begin = static_cast<std::size_t>(names.sh_offset);
  const auto names_end = names_begin + static_cast<std::size_t>(names.sh_size);

  for (const auto& section : sections) {
    if (section.sh_name >= names.sh_size) continue;
    auto cursor = names_begin + static_cast<std::size_t>(section.sh_name);
    const auto name = read_c_string(bytes, cursor, names_end, "ELF section name");
    if (name != wanted_name) continue;
    if ((section.sh_flags & SHF_COMPRESSED) != 0) {
      throw std::runtime_error("compressed " + std::string(wanted_name) +
                               " sections are unsupported");
    }
    require_range(bytes, section.sh_offset, section.sh_size,
                  std::string(wanted_name).c_str());
    return SectionRange{static_cast<std::size_t>(section.sh_offset),
                        static_cast<std::size_t>(section.sh_size)};
  }
  return std::nullopt;
}

std::string read_section_string(const std::vector<std::byte>& bytes,
                                const std::optional<SectionRange>& section,
                                std::uint64_t offset, const char* what) {
  if (!section) throw std::runtime_error(std::string(what) + " section is missing");
  if (offset >= section->size) {
    throw std::runtime_error(std::string(what) + " offset is outside its string section");
  }
  auto cursor = section->offset + static_cast<std::size_t>(offset);
  const auto limit = section->offset + section->size;
  return read_c_string(bytes, cursor, limit, what);
}

FileEntry read_v4_file_entry(const std::vector<std::byte>& bytes, std::size_t& cursor,
                             std::size_t limit) {
  auto name = read_c_string(bytes, cursor, limit, "DWARF file name");
  const auto directory_index = read_uleb(bytes, cursor, limit);
  static_cast<void>(read_uleb(bytes, cursor, limit));
  static_cast<void>(read_uleb(bytes, cursor, limit));
  return {std::move(name), directory_index};
}

std::vector<EntryFormat> read_v5_entry_formats(const std::vector<std::byte>& bytes,
                                                std::size_t& cursor,
                                                std::size_t header_end,
                                                const char* what) {
  const auto count = read_scalar<std::uint8_t>(bytes, cursor, header_end, what);
  std::vector<EntryFormat> formats;
  formats.reserve(count);
  for (std::uint16_t index = 0; index < count; ++index) {
    formats.push_back({read_uleb(bytes, cursor, header_end),
                       read_uleb(bytes, cursor, header_end)});
  }
  return formats;
}

std::uint64_t read_v5_unsigned_form(const std::vector<std::byte>& bytes,
                                    std::size_t& cursor, std::size_t limit,
                                    std::uint64_t form) {
  switch (form) {
    case kFormUdata:
      return read_uleb(bytes, cursor, limit);
    case kFormData1:
      return read_scalar<std::uint8_t>(bytes, cursor, limit, "DWARF5 data1 form");
    case kFormData2:
      return read_scalar<std::uint16_t>(bytes, cursor, limit, "DWARF5 data2 form");
    case kFormData4:
      return read_scalar<std::uint32_t>(bytes, cursor, limit, "DWARF5 data4 form");
    case kFormData8:
      return read_scalar<std::uint64_t>(bytes, cursor, limit, "DWARF5 data8 form");
    default:
      throw std::runtime_error("unsupported DWARF5 unsigned line-table form " +
                               std::to_string(form));
  }
}

std::string read_v5_path_form(const std::vector<std::byte>& bytes,
                              std::size_t& cursor, std::size_t limit,
                              std::uint64_t form,
                              const std::optional<SectionRange>& line_strings) {
  switch (form) {
    case kFormString:
      return read_c_string(bytes, cursor, limit, "DWARF5 inline path");
    case kFormLineStrp: {
      const auto offset =
          read_scalar<std::uint32_t>(bytes, cursor, limit, "DWARF5 line_strp offset");
      return read_section_string(bytes, line_strings, offset, "DWARF5 line string");
    }
    default:
      throw std::runtime_error("unsupported DWARF5 path line-table form " +
                               std::to_string(form));
  }
}

void skip_v5_form(const std::vector<std::byte>& bytes, std::size_t& cursor,
                  std::size_t limit, std::uint64_t form) {
  switch (form) {
    case kFormString:
      static_cast<void>(read_c_string(bytes, cursor, limit, "DWARF5 string form"));
      return;
    case kFormLineStrp:
      static_cast<void>(read_scalar<std::uint32_t>(bytes, cursor, limit,
                                                   "DWARF5 line_strp offset"));
      return;
    case kFormUdata:
      static_cast<void>(read_uleb(bytes, cursor, limit));
      return;
    case kFormSdata:
      static_cast<void>(read_sleb(bytes, cursor, limit));
      return;
    case kFormData1:
      static_cast<void>(read_scalar<std::uint8_t>(bytes, cursor, limit,
                                                  "DWARF5 data1 form"));
      return;
    case kFormData2:
      static_cast<void>(read_scalar<std::uint16_t>(bytes, cursor, limit,
                                                   "DWARF5 data2 form"));
      return;
    case kFormData4:
      static_cast<void>(read_scalar<std::uint32_t>(bytes, cursor, limit,
                                                   "DWARF5 data4 form"));
      return;
    case kFormData8:
      static_cast<void>(read_scalar<std::uint64_t>(bytes, cursor, limit,
                                                   "DWARF5 data8 form"));
      return;
    case kFormData16:
      if (cursor > limit || 16 > limit - cursor) {
        throw std::runtime_error("DWARF5 data16 form extends past header boundary");
      }
      cursor += 16;
      return;
    default:
      throw std::runtime_error("unsupported DWARF5 line-table form " +
                               std::to_string(form));
  }
}

FileEntry read_v5_entry(const std::vector<std::byte>& bytes, std::size_t& cursor,
                        std::size_t limit, const std::vector<EntryFormat>& formats,
                        const std::optional<SectionRange>& line_strings) {
  std::optional<std::string> path;
  std::uint64_t directory_index = 0;
  for (const auto& format : formats) {
    if (format.content_type == kLnctPath) {
      if (path) throw std::runtime_error("duplicate DWARF5 path column");
      path = read_v5_path_form(bytes, cursor, limit, format.form, line_strings);
    } else if (format.content_type == kLnctDirectoryIndex) {
      directory_index = read_v5_unsigned_form(bytes, cursor, limit, format.form);
    } else {
      skip_v5_form(bytes, cursor, limit, format.form);
    }
  }
  if (!path) throw std::runtime_error("DWARF5 line-table entry has no path column");
  return {std::move(*path), directory_index};
}

void read_v5_directory_and_file_tables(
    const std::vector<std::byte>& bytes, std::size_t& cursor,
    std::size_t header_end, const std::optional<SectionRange>& line_strings,
    std::vector<std::string>& directories, std::vector<FileEntry>& files) {
  const auto directory_formats =
      read_v5_entry_formats(bytes, cursor, header_end, "directory format count");
  const auto directory_count = read_uleb(bytes, cursor, header_end);
  if (directory_count > header_end - cursor) {
    throw std::runtime_error("DWARF5 directory count exceeds header bounds");
  }
  directories.reserve(static_cast<std::size_t>(directory_count));
  for (std::uint64_t index = 0; index < directory_count; ++index) {
    directories.push_back(
        read_v5_entry(bytes, cursor, header_end, directory_formats, line_strings).name);
  }

  const auto file_formats =
      read_v5_entry_formats(bytes, cursor, header_end, "file format count");
  const auto file_count = read_uleb(bytes, cursor, header_end);
  if (file_count > header_end - cursor) {
    throw std::runtime_error("DWARF5 file count exceeds header bounds");
  }
  files.reserve(static_cast<std::size_t>(file_count));
  for (std::uint64_t index = 0; index < file_count; ++index) {
    files.push_back(read_v5_entry(bytes, cursor, header_end, file_formats, line_strings));
  }
}

std::optional<SourceLocation> source_location(const State& state,
                                              const std::vector<std::string>& directories,
                                              const std::vector<FileEntry>& files,
                                              bool dwarf5) {
  if (state.line <= 0) return std::nullopt;

  std::size_t file_index = 0;
  if (dwarf5) {
    if (state.file >= files.size()) return std::nullopt;
    file_index = static_cast<std::size_t>(state.file);
  } else {
    if (state.file == 0 || state.file > files.size()) return std::nullopt;
    file_index = static_cast<std::size_t>(state.file - 1);
  }

  const auto& file = files[file_index];
  std::filesystem::path path(file.name);
  if (!path.is_absolute()) {
    if (dwarf5) {
      if (file.directory_index < directories.size()) {
        path = std::filesystem::path(
                   directories[static_cast<std::size_t>(file.directory_index)]) /
               path;
      }
    } else if (file.directory_index != 0 && file.directory_index <= directories.size()) {
      path = std::filesystem::path(
                 directories[static_cast<std::size_t>(file.directory_index - 1)]) /
             path;
    }
  }
  return SourceLocation{path.string(), static_cast<std::uint64_t>(state.line), state.column};
}

}  // namespace

DwarfLineTable::DwarfLineTable(std::string path) : path_(std::move(path)) { parse(); }

void DwarfLineTable::parse() {
  const auto bytes = read_file(path_);
  const auto section = find_debug_section(bytes, ".debug_line");
  if (!section) return;
  const auto line_strings = find_debug_section(bytes, ".debug_line_str");

  std::size_t cursor = section->offset;
  const auto section_end = section->offset + section->size;
  while (cursor < section_end) {
    const auto unit_length = read_scalar<std::uint32_t>(bytes, cursor, section_end,
                                                        "DWARF unit length");
    if (unit_length == 0xffffffffU) {
      throw std::runtime_error("64-bit DWARF line-table format is unsupported");
    }
    if (unit_length == 0) throw std::runtime_error("zero-length DWARF line-table unit");
    if (unit_length > section_end - cursor) {
      throw std::runtime_error("DWARF line-table unit extends past .debug_line");
    }
    const auto unit_end = cursor + static_cast<std::size_t>(unit_length);

    const auto version = read_scalar<std::uint16_t>(bytes, cursor, unit_end,
                                                     "DWARF line-table version");
    if (version != 4 && version != 5) {
      throw std::runtime_error("DWARF line table version " + std::to_string(version) +
                               " is unsupported; versions 4 and 5 are supported");
    }
    const bool dwarf5 = version == 5;
    std::uint8_t address_size = sizeof(std::uint64_t);
    if (dwarf5) {
      address_size = read_scalar<std::uint8_t>(bytes, cursor, unit_end,
                                               "DWARF5 address size");
      const auto segment_selector_size = read_scalar<std::uint8_t>(
          bytes, cursor, unit_end, "DWARF5 segment selector size");
      if (address_size != sizeof(std::uint64_t)) {
        throw std::runtime_error("DWARF5 line table address size does not match ELF64");
      }
      if (segment_selector_size != 0) {
        throw std::runtime_error("DWARF5 segmented addresses are unsupported");
      }
    }

    const auto header_length = read_scalar<std::uint32_t>(bytes, cursor, unit_end,
                                                           "DWARF line header length");
    if (header_length > unit_end - cursor) {
      throw std::runtime_error("DWARF line-table header extends past unit boundary");
    }
    const auto header_end = cursor + static_cast<std::size_t>(header_length);

    const auto minimum_instruction_length =
        read_scalar<std::uint8_t>(bytes, cursor, header_end, "minimum instruction length");
    const auto maximum_operations_per_instruction = read_scalar<std::uint8_t>(
        bytes, cursor, header_end, "maximum operations per instruction");
    if (minimum_instruction_length == 0 || maximum_operations_per_instruction != 1) {
      throw std::runtime_error(
          "DWARF line tables with multi-operation instructions are unsupported");
    }
    static_cast<void>(
        read_scalar<std::uint8_t>(bytes, cursor, header_end, "default is_stmt"));
    const auto line_base = static_cast<std::int8_t>(
        read_scalar<std::uint8_t>(bytes, cursor, header_end, "line base"));
    const auto line_range =
        read_scalar<std::uint8_t>(bytes, cursor, header_end, "line range");
    const auto opcode_base =
        read_scalar<std::uint8_t>(bytes, cursor, header_end, "opcode base");
    if (line_range == 0 || opcode_base == 0) {
      throw std::runtime_error("invalid DWARF line-table opcode parameters");
    }

    std::vector<std::uint8_t> standard_opcode_lengths;
    standard_opcode_lengths.reserve(static_cast<std::size_t>(opcode_base - 1));
    for (std::uint16_t opcode = 1; opcode < opcode_base; ++opcode) {
      standard_opcode_lengths.push_back(
          read_scalar<std::uint8_t>(bytes, cursor, header_end,
                                    "standard opcode operand count"));
    }

    std::vector<std::string> directories;
    std::vector<FileEntry> files;
    if (dwarf5) {
      read_v5_directory_and_file_tables(bytes, cursor, header_end, line_strings,
                                        directories, files);
      if (cursor != header_end) {
        throw std::runtime_error("DWARF5 line-table header has trailing bytes");
      }
    } else {
      while (cursor < header_end) {
        auto directory = read_c_string(bytes, cursor, header_end,
                                       "DWARF include directory");
        if (directory.empty()) break;
        directories.push_back(std::move(directory));
      }

      while (cursor < header_end) {
        const auto before = cursor;
        auto name = read_c_string(bytes, cursor, header_end, "DWARF file name");
        if (name.empty()) break;
        cursor = before;
        files.push_back(read_v4_file_entry(bytes, cursor, header_end));
      }
    }
    cursor = header_end;

    State state;
    std::optional<Row> previous;
    auto emit = [&](bool end_sequence) {
      if (previous && state.address > previous->address && previous->location) {
        ranges_.push_back({previous->address, state.address, *previous->location});
      }
      if (end_sequence) {
        previous.reset();
      } else {
        previous = Row{state.address, source_location(state, directories, files, dwarf5)};
      }
    };
    auto reset_state = [&]() { state = State{}; };

    while (cursor < unit_end) {
      const auto opcode =
          read_scalar<std::uint8_t>(bytes, cursor, unit_end, "DWARF line opcode");
      if (opcode == 0) {
        const auto payload_length = read_uleb(bytes, cursor, unit_end);
        if (payload_length == 0 || payload_length > unit_end - cursor) {
          throw std::runtime_error("invalid DWARF extended line opcode length");
        }
        const auto payload_end = cursor + static_cast<std::size_t>(payload_length);
        const auto extended_opcode = read_scalar<std::uint8_t>(
            bytes, cursor, payload_end, "DWARF extended line opcode");
        switch (extended_opcode) {
          case kLneEndSequence:
            emit(true);
            reset_state();
            break;
          case kLneSetAddress:
            if (payload_end - cursor != address_size) {
              throw std::runtime_error("DWARF set_address width does not match line header");
            }
            state.address = read_scalar<std::uint64_t>(bytes, cursor, payload_end,
                                                       "DWARF set_address operand");
            break;
          case kLneDefineFile:
            if (dwarf5) {
              throw std::runtime_error("DWARF5 define_file opcode is unsupported");
            }
            files.push_back(read_v4_file_entry(bytes, cursor, payload_end));
            break;
          case kLneSetDiscriminator:
            static_cast<void>(read_uleb(bytes, cursor, payload_end));
            break;
          default:
            break;
        }
        cursor = payload_end;
        continue;
      }

      if (opcode < opcode_base) {
        switch (opcode) {
          case kLnsCopy:
            emit(false);
            break;
          case kLnsAdvancePc:
            state.address +=
                read_uleb(bytes, cursor, unit_end) * minimum_instruction_length;
            break;
          case kLnsAdvanceLine:
            state.line += read_sleb(bytes, cursor, unit_end);
            break;
          case kLnsSetFile:
            state.file = read_uleb(bytes, cursor, unit_end);
            break;
          case kLnsSetColumn:
            state.column = read_uleb(bytes, cursor, unit_end);
            break;
          case kLnsNegateStmt:
          case kLnsSetBasicBlock:
          case kLnsSetPrologueEnd:
          case kLnsSetEpilogueBegin:
            break;
          case kLnsConstAddPc: {
            const auto adjusted = static_cast<std::uint16_t>(255U - opcode_base);
            state.address +=
                static_cast<std::uint64_t>(adjusted / line_range) *
                minimum_instruction_length;
            break;
          }
          case kLnsFixedAdvancePc:
            state.address += read_scalar<std::uint16_t>(bytes, cursor, unit_end,
                                                        "fixed advance PC operand");
            break;
          case kLnsSetIsa:
            static_cast<void>(read_uleb(bytes, cursor, unit_end));
            break;
          default: {
            const auto operand_count = standard_opcode_lengths[opcode - 1];
            for (std::uint8_t operand = 0; operand < operand_count; ++operand) {
              static_cast<void>(read_uleb(bytes, cursor, unit_end));
            }
            break;
          }
        }
        continue;
      }

      const auto adjusted = static_cast<std::uint16_t>(opcode - opcode_base);
      state.address += static_cast<std::uint64_t>(adjusted / line_range) *
                       minimum_instruction_length;
      state.line += static_cast<std::int64_t>(line_base) +
                    static_cast<std::int64_t>(adjusted % line_range);
      emit(false);
    }
  }

  std::sort(ranges_.begin(), ranges_.end(), [](const Range& left, const Range& right) {
    if (left.begin != right.begin) return left.begin < right.begin;
    return left.end < right.end;
  });
}

std::optional<SourceLocation> DwarfLineTable::find_virtual_address(
    std::uint64_t address) const {
  const auto candidate = std::upper_bound(
      ranges_.begin(), ranges_.end(), address,
      [](std::uint64_t value, const Range& range) { return value < range.begin; });
  if (candidate == ranges_.begin()) return std::nullopt;
  const auto& range = *std::prev(candidate);
  if (address < range.begin || address >= range.end) return std::nullopt;
  return range.location;
}

std::optional<SourceLocation> DwarfLineTable::find_runtime_address(
    pid_t pid, std::uint64_t address, const ElfFile& elf) const {
  const auto bias = elf.load_bias(pid);
  if (address < bias) return std::nullopt;
  return find_virtual_address(address - bias);
}

}  // namespace mdbg

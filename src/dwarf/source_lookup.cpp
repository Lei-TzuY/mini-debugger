#include "dwarf/line_table.hpp"
#include "dwarf/local_value.hpp"

#include "debugger/debugger.hpp"
#include "dwarf/eh_frame.hpp"
#include "elf/elf.hpp"
#include "unwind/cfi.hpp"

#include <elf.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mdbg {
namespace {

constexpr std::uint64_t kDwTagFormalParameter = 0x05;
constexpr std::uint64_t kDwTagMember = 0x0d;
constexpr std::uint64_t kDwTagLexicalBlock = 0x0b;
constexpr std::uint64_t kDwTagPointerType = 0x0f;
constexpr std::uint64_t kDwTagStructureType = 0x13;
constexpr std::uint64_t kDwTagTypedef = 0x16;
constexpr std::uint64_t kDwTagInlinedSubroutine = 0x1d;
constexpr std::uint64_t kDwTagBaseType = 0x24;
constexpr std::uint64_t kDwTagConstType = 0x26;
constexpr std::uint64_t kDwTagSubprogram = 0x2e;
constexpr std::uint64_t kDwTagVariable = 0x34;

constexpr std::uint64_t kDwAtLocation = 0x02;
constexpr std::uint64_t kDwAtName = 0x03;
constexpr std::uint64_t kDwAtByteSize = 0x0b;
constexpr std::uint64_t kDwAtLowPc = 0x11;
constexpr std::uint64_t kDwAtHighPc = 0x12;
constexpr std::uint64_t kDwAtAbstractOrigin = 0x31;
constexpr std::uint64_t kDwAtDataMemberLocation = 0x38;
constexpr std::uint64_t kDwAtEncoding = 0x3e;
constexpr std::uint64_t kDwAtFrameBase = 0x40;
constexpr std::uint64_t kDwAtType = 0x49;
constexpr std::uint64_t kDwAtStrOffsetsBase = 0x72;
constexpr std::uint64_t kDwAtAddrBase = 0x73;
constexpr std::uint64_t kDwAtLoclistsBase = 0x8c;

constexpr std::uint64_t kDwFormAddr = 0x01;
constexpr std::uint64_t kDwFormData2 = 0x05;
constexpr std::uint64_t kDwFormData4 = 0x06;
constexpr std::uint64_t kDwFormData8 = 0x07;
constexpr std::uint64_t kDwFormString = 0x08;
constexpr std::uint64_t kDwFormData1 = 0x0b;
constexpr std::uint64_t kDwFormStrp = 0x0e;
constexpr std::uint64_t kDwFormRef4 = 0x13;
constexpr std::uint64_t kDwFormSecOffset = 0x17;
constexpr std::uint64_t kDwFormExprloc = 0x18;
constexpr std::uint64_t kDwFormFlagPresent = 0x19;
constexpr std::uint64_t kDwFormAddrx = 0x1b;
constexpr std::uint64_t kDwFormLineStrp = 0x1f;
constexpr std::uint64_t kDwFormImplicitConst = 0x21;
constexpr std::uint64_t kDwFormLoclistx = 0x22;
constexpr std::uint64_t kDwFormStrx1 = 0x25;

constexpr std::uint8_t kDwUtCompile = 0x01;
constexpr std::uint8_t kDwLleEndOfList = 0x00;
constexpr std::uint8_t kDwLleOffsetPair = 0x04;
constexpr std::uint8_t kDwLleBaseAddress = 0x06;
constexpr std::uint8_t kDwOpDeref = 0x06;
constexpr std::uint8_t kDwOpConst1u = 0x08;
constexpr std::uint8_t kDwOpConstu = 0x10;
constexpr std::uint8_t kDwOpShl = 0x24;
constexpr std::uint8_t kDwOpXor = 0x27;
constexpr std::uint8_t kDwOpLit8 = 0x38;
constexpr std::uint8_t kDwOpLit16 = 0x40;
constexpr std::uint8_t kDwOpLit24 = 0x48;
constexpr std::uint8_t kDwOpReg0 = 0x50;
constexpr std::uint8_t kDwOpReg4 = 0x54;
constexpr std::uint8_t kDwOpReg5 = 0x55;
constexpr std::uint8_t kDwOpReg6 = 0x56;
constexpr std::uint8_t kDwOpBreg1 = 0x71;
constexpr std::uint8_t kDwOpBreg2 = 0x72;
constexpr std::uint8_t kDwOpBreg3 = 0x73;
constexpr std::uint8_t kDwOpBreg4 = 0x74;
constexpr std::uint8_t kDwOpBreg5 = 0x75;
constexpr std::uint8_t kDwOpBreg8 = 0x78;
constexpr std::uint8_t kDwOpBreg9 = 0x79;
constexpr std::uint8_t kDwOpFbreg = 0x91;
constexpr std::uint8_t kDwOpPiece = 0x93;
constexpr std::uint8_t kDwOpStackValue = 0x9f;
constexpr std::uint8_t kDwOpEntryValue = 0xa3;
constexpr std::uint8_t kDwOpCallFrameCfa = 0x9c;
constexpr std::uint64_t kDwAteSigned = 0x05;
constexpr std::uint64_t kDwAteUnsigned = 0x07;
constexpr std::size_t kMaxLocalStructSize = 256;
constexpr std::size_t kMaxLocalStructMembers = 32;
constexpr unsigned kMaxAbstractOriginDepth = 8;

struct DebugSections {
  std::vector<std::byte> info;
  std::vector<std::byte> abbrev;
  std::vector<std::byte> strings;
  std::vector<std::byte> line_strings;
  std::vector<std::byte> locations;
  std::vector<std::byte> location_lists;
  std::vector<std::byte> addresses;
  std::vector<std::byte> string_offsets;
};

struct AttributeSpec {
  std::uint64_t name;
  std::uint64_t form;
  std::int64_t implicit_constant{0};
};

struct Abbreviation {
  std::uint64_t tag;
  bool has_children;
  std::vector<AttributeSpec> attributes;
};

struct AttributeValue {
  std::uint64_t name;
  std::uint64_t form;
  std::uint64_t number{0};
  std::string text;
  std::vector<std::byte> expression;
};

struct Die {
  std::size_t offset;
  std::uint64_t tag;
  std::optional<std::size_t> parent;
  std::vector<AttributeValue> attributes;
};

struct IntegerType {
  std::size_t byte_size;
  bool is_signed;
};

struct AggregateMemberType {
  std::string name;
  std::size_t offset;
  IntegerType integer;
};

struct ValueType {
  std::size_t byte_size;
  bool is_signed;
  LocalValueKind kind;
  std::vector<AggregateMemberType> members;
};

struct LoclistsContribution {
  std::size_t start;
  std::size_t offsets_base;
  std::size_t end;
  std::uint32_t offset_count;
};

std::string normalized_source_path(const std::filesystem::path& path) {
  return path.lexically_normal().generic_string();
}

template <typename T>
T read_at(const std::vector<std::byte>& bytes, std::size_t offset, const char* what) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    throw std::runtime_error(std::string(what) + " extends past end of data");
  }
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

template <typename T>
T read_scalar(const std::vector<std::byte>& bytes, std::size_t& cursor,
              std::size_t limit, const char* what) {
  if (cursor > limit || sizeof(T) > limit - cursor) {
    throw std::runtime_error(std::string(what) + " extends past DWARF boundary");
  }
  const auto value = read_at<T>(bytes, cursor, what);
  cursor += sizeof(T);
  return value;
}

std::uint64_t read_uleb(const std::vector<std::byte>& bytes, std::size_t& cursor,
                        std::size_t limit, const char* what) {
  std::uint64_t result = 0;
  unsigned shift = 0;
  for (unsigned count = 0; count < 10; ++count) {
    const auto byte = read_scalar<std::uint8_t>(bytes, cursor, limit, what);
    const auto payload = static_cast<std::uint64_t>(byte & 0x7fU);
    if (shift > 63 || (shift == 63 && payload > 1)) {
      throw std::runtime_error(std::string(what) + " overflows 64 bits");
    }
    result |= payload << shift;
    if ((byte & 0x80U) == 0) return result;
    shift += 7;
  }
  throw std::runtime_error(std::string(what) + " is too long");
}

std::int64_t read_sleb(const std::vector<std::byte>& bytes, std::size_t& cursor,
                       std::size_t limit, const char* what) {
  std::uint64_t result = 0;
  unsigned shift = 0;
  std::uint8_t byte = 0;
  for (unsigned count = 0; count < 10; ++count) {
    byte = read_scalar<std::uint8_t>(bytes, cursor, limit, what);
    result |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
    shift += 7;
    if ((byte & 0x80U) == 0) {
      if ((byte & 0x40U) != 0 && shift < 64) result |= (~std::uint64_t{0}) << shift;
      return static_cast<std::int64_t>(result);
    }
  }
  throw std::runtime_error(std::string(what) + " is too long");
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

std::map<std::string, std::vector<std::byte>> read_named_sections(
    const std::string& path, const std::vector<std::string>& wanted) {
  const auto bytes = read_file(path);
  const auto header = read_at<Elf64_Ehdr>(bytes, 0, "ELF header");
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
      header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_X86_64) {
    throw std::runtime_error("local-value inspection requires little-endian x86-64 ELF64");
  }
  if (header.e_shnum == 0 || header.e_shstrndx == SHN_UNDEF ||
      header.e_shentsize != sizeof(Elf64_Shdr) || header.e_shstrndx == SHN_XINDEX ||
      header.e_shstrndx >= header.e_shnum) {
    throw std::runtime_error("unsupported ELF section layout for local-value inspection");
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

  std::map<std::string, std::vector<std::byte>> result;
  for (const auto& section : sections) {
    if (section.sh_name >= names.sh_size) continue;
    auto name_cursor = names_begin + static_cast<std::size_t>(section.sh_name);
    const auto name = read_c_string(bytes, name_cursor, names_end, "ELF section name");
    if (std::find(wanted.begin(), wanted.end(), name) == wanted.end()) continue;
    if ((section.sh_flags & SHF_COMPRESSED) != 0) {
      throw std::runtime_error("compressed " + name + " is unsupported");
    }
    if (section.sh_offset > bytes.size() || section.sh_size > bytes.size() - section.sh_offset) {
      throw std::runtime_error(name + " extends past end of file");
    }
    const auto begin = static_cast<std::size_t>(section.sh_offset);
    const auto end = begin + static_cast<std::size_t>(section.sh_size);
    result.emplace(name, std::vector<std::byte>(bytes.begin() + begin, bytes.begin() + end));
  }
  return result;
}

DebugSections read_debug_sections(const std::string& path) {
  const auto sections = read_named_sections(
      path, {".debug_info", ".debug_abbrev", ".debug_str", ".debug_line_str",
             ".debug_loc", ".debug_loclists", ".debug_addr", ".debug_str_offsets"});
  const auto info = sections.find(".debug_info");
  const auto abbrev = sections.find(".debug_abbrev");
  const auto strings = sections.find(".debug_str");
  if (info == sections.end() || abbrev == sections.end() || strings == sections.end()) {
    throw std::runtime_error(
        "local-value inspection requires .debug_info, .debug_abbrev, and .debug_str");
  }
  const auto section_or_empty = [&](std::string_view name) {
    const auto it = sections.find(std::string(name));
    return it == sections.end() ? std::vector<std::byte>{} : it->second;
  };
  return DebugSections{info->second,
                       abbrev->second,
                       strings->second,
                       section_or_empty(".debug_line_str"),
                       section_or_empty(".debug_loc"),
                       section_or_empty(".debug_loclists"),
                       section_or_empty(".debug_addr"),
                       section_or_empty(".debug_str_offsets")};
}

std::map<std::uint64_t, Abbreviation> parse_abbreviations(
    const std::vector<std::byte>& bytes, std::uint64_t offset) {
  if (offset > bytes.size()) throw std::runtime_error("DWARF abbreviation offset is out of range");
  std::size_t cursor = static_cast<std::size_t>(offset);
  std::map<std::uint64_t, Abbreviation> result;
  while (cursor < bytes.size()) {
    const auto code = read_uleb(bytes, cursor, bytes.size(), "abbreviation code");
    if (code == 0) break;
    const auto tag = read_uleb(bytes, cursor, bytes.size(), "abbreviation tag");
    const auto children = read_scalar<std::uint8_t>(bytes, cursor, bytes.size(),
                                                    "abbreviation children flag");
    if (children > 1) throw std::runtime_error("invalid DWARF abbreviation children flag");
    Abbreviation entry{tag, children != 0, {}};
    for (;;) {
      const auto name = read_uleb(bytes, cursor, bytes.size(), "abbreviation attribute");
      const auto form = read_uleb(bytes, cursor, bytes.size(), "abbreviation form");
      if (name == 0 && form == 0) break;
      if (name == 0 || form == 0) {
        throw std::runtime_error("malformed DWARF abbreviation attribute/form pair");
      }
      std::int64_t implicit_constant = 0;
      if (form == kDwFormImplicitConst) {
        implicit_constant =
            read_sleb(bytes, cursor, bytes.size(), "DW_FORM_implicit_const value");
      }
      entry.attributes.push_back(AttributeSpec{name, form, implicit_constant});
    }
    if (!result.emplace(code, std::move(entry)).second) {
      throw std::runtime_error("duplicate DWARF abbreviation code");
    }
  }
  return result;
}

AttributeValue read_attribute(const AttributeSpec& spec, const DebugSections& sections,
                              std::size_t& cursor, std::size_t unit_end,
                              std::size_t unit_start, std::uint8_t address_size) {
  AttributeValue value{spec.name, spec.form, 0, {}, {}};
  switch (spec.form) {
    case kDwFormAddr:
      if (address_size != 8) throw std::runtime_error("only 8-byte DWARF addresses are supported");
      value.number = read_scalar<std::uint64_t>(sections.info, cursor, unit_end, "DW_FORM_addr");
      break;
    case kDwFormData1:
      value.number = read_scalar<std::uint8_t>(sections.info, cursor, unit_end, "DW_FORM_data1");
      break;
    case kDwFormData2:
      value.number = read_scalar<std::uint16_t>(sections.info, cursor, unit_end, "DW_FORM_data2");
      break;
    case kDwFormData4:
      value.number = read_scalar<std::uint32_t>(sections.info, cursor, unit_end, "DW_FORM_data4");
      break;
    case kDwFormData8:
      value.number = read_scalar<std::uint64_t>(sections.info, cursor, unit_end, "DW_FORM_data8");
      break;
    case kDwFormString:
      value.text = read_c_string(sections.info, cursor, unit_end, "DW_FORM_string");
      break;
    case kDwFormStrp: {
      const auto offset = read_scalar<std::uint32_t>(sections.info, cursor, unit_end,
                                                     "DW_FORM_strp");
      if (offset >= sections.strings.size()) {
        throw std::runtime_error("DW_FORM_strp offset is out of range");
      }
      auto string_cursor = static_cast<std::size_t>(offset);
      value.text = read_c_string(sections.strings, string_cursor, sections.strings.size(),
                                 "DW_FORM_strp string");
      break;
    }
    case kDwFormLineStrp: {
      const auto offset = read_scalar<std::uint32_t>(sections.info, cursor, unit_end,
                                                     "DW_FORM_line_strp");
      if (sections.line_strings.empty() || offset >= sections.line_strings.size()) {
        throw std::runtime_error("DW_FORM_line_strp offset is out of range");
      }
      auto string_cursor = static_cast<std::size_t>(offset);
      value.text = read_c_string(sections.line_strings, string_cursor,
                                 sections.line_strings.size(), "DW_FORM_line_strp string");
      break;
    }
    case kDwFormRef4: {
      const auto offset = read_scalar<std::uint32_t>(sections.info, cursor, unit_end,
                                                     "DW_FORM_ref4");
      if (offset > std::numeric_limits<std::size_t>::max() - unit_start) {
        throw std::runtime_error("DW_FORM_ref4 overflows debug-info offset");
      }
      value.number = unit_start + static_cast<std::size_t>(offset);
      break;
    }
    case kDwFormSecOffset:
      value.number = read_scalar<std::uint32_t>(sections.info, cursor, unit_end,
                                                "DW_FORM_sec_offset");
      break;
    case kDwFormExprloc: {
      const auto length = read_uleb(sections.info, cursor, unit_end, "DW_FORM_exprloc length");
      if (length > unit_end - cursor) {
        throw std::runtime_error("DW_FORM_exprloc extends past unit");
      }
      const auto end = cursor + static_cast<std::size_t>(length);
      value.expression.assign(sections.info.begin() + cursor, sections.info.begin() + end);
      cursor = end;
      break;
    }
    case kDwFormFlagPresent:
      value.number = 1;
      break;
    case kDwFormAddrx:
    case kDwFormLoclistx:
      value.number = read_uleb(sections.info, cursor, unit_end,
                               spec.form == kDwFormAddrx ? "DW_FORM_addrx"
                                                        : "DW_FORM_loclistx");
      break;
    case kDwFormStrx1:
      value.number = read_scalar<std::uint8_t>(sections.info, cursor, unit_end,
                                               "DW_FORM_strx1");
      break;
    case kDwFormImplicitConst:
      value.number = static_cast<std::uint64_t>(spec.implicit_constant);
      break;
    default:
      throw std::runtime_error("unsupported DWARF form in local-value inspection: " +
                               std::to_string(spec.form));
  }
  return value;
}

const AttributeValue* attribute(const Die& die, std::uint64_t name) {
  const auto it = std::find_if(die.attributes.begin(), die.attributes.end(),
                               [name](const AttributeValue& value) {
                                 return value.name == name;
                               });
  return it == die.attributes.end() ? nullptr : &*it;
}

std::optional<std::size_t> die_index_by_offset(const std::vector<Die>& dies,
                                               std::uint64_t offset) {
  for (std::size_t index = 0; index < dies.size(); ++index) {
    if (dies[index].offset == offset) return index;
  }
  return std::nullopt;
}

const AttributeValue* attribute_with_abstract_origin(const std::vector<Die>& dies,
                                                     std::size_t die_index,
                                                     std::uint64_t name) {
  for (unsigned depth = 0; depth < kMaxAbstractOriginDepth; ++depth) {
    if (const auto* direct = attribute(dies[die_index], name)) return direct;
    const auto* origin = attribute(dies[die_index], kDwAtAbstractOrigin);
    if (origin == nullptr) return nullptr;
    if (origin->form != kDwFormRef4) {
      throw std::runtime_error(
          "inlined local abstract origin does not use supported DW_FORM_ref4");
    }
    const auto next = die_index_by_offset(dies, origin->number);
    if (!next) {
      throw std::runtime_error("inlined local abstract origin references an unknown DIE");
    }
    if (*next == die_index) {
      throw std::runtime_error("inlined local abstract origin is self-referential");
    }
    die_index = *next;
  }
  throw std::runtime_error("inlined local abstract-origin chain is too deep");
}

bool is_descendant_of(const std::vector<Die>& dies, std::size_t child,
                      std::size_t ancestor) {
  auto parent = dies[child].parent;
  while (parent) {
    if (*parent == ancestor) return true;
    parent = dies[*parent].parent;
  }
  return false;
}

std::uint64_t add_unsigned(std::uint64_t base, std::uint64_t offset,
                           const char* what) {
  if (offset > std::numeric_limits<std::uint64_t>::max() - base) {
    throw std::runtime_error(std::string(what) + " overflows address space");
  }
  return base + offset;
}

std::uint64_t add_signed(std::uint64_t base, std::int64_t offset, const char* what) {
  if (offset >= 0) return add_unsigned(base, static_cast<std::uint64_t>(offset), what);
  const auto magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1;
  if (magnitude > base) {
    throw std::runtime_error(std::string(what) + " underflows address space");
  }
  return base - magnitude;
}

std::string indexed_string(const DebugSections& sections, std::uint64_t base,
                           std::uint64_t index) {
  if (sections.string_offsets.empty()) {
    throw std::runtime_error("DW_FORM_strx1 requires .debug_str_offsets");
  }
  if (base < 8 || base > sections.string_offsets.size()) {
    throw std::runtime_error("DW_AT_str_offsets_base is out of range");
  }
  const auto contribution_start = static_cast<std::size_t>(base - 8);
  const auto length = read_at<std::uint32_t>(sections.string_offsets, contribution_start,
                                             ".debug_str_offsets length");
  if (length == 0xffffffffU || length < 4 ||
      length > sections.string_offsets.size() - contribution_start - 4) {
    throw std::runtime_error("unsupported .debug_str_offsets contribution");
  }
  const auto contribution_end = contribution_start + 4 + static_cast<std::size_t>(length);
  if (read_at<std::uint16_t>(sections.string_offsets, contribution_start + 4,
                             ".debug_str_offsets version") != 5 ||
      read_at<std::uint16_t>(sections.string_offsets, contribution_start + 6,
                             ".debug_str_offsets padding") != 0) {
    throw std::runtime_error("unsupported .debug_str_offsets header");
  }
  if (index > (std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(base)) / 4) {
    throw std::runtime_error("DW_FORM_strx1 index overflows offset table");
  }
  const auto entry = static_cast<std::size_t>(base) + static_cast<std::size_t>(index) * 4;
  if (entry > contribution_end || 4 > contribution_end - entry) {
    throw std::runtime_error("DW_FORM_strx1 index is out of range");
  }
  const auto string_offset = read_at<std::uint32_t>(sections.string_offsets, entry,
                                                     "DW_FORM_strx1 offset");
  if (string_offset >= sections.strings.size()) {
    throw std::runtime_error("DW_FORM_strx1 string offset is out of range");
  }
  auto cursor = static_cast<std::size_t>(string_offset);
  return read_c_string(sections.strings, cursor, sections.strings.size(),
                       "DW_FORM_strx1 string");
}

std::uint64_t indexed_address(const DebugSections& sections, std::uint64_t base,
                              std::uint64_t index) {
  if (sections.addresses.empty()) throw std::runtime_error("DW_FORM_addrx requires .debug_addr");
  if (base < 8 || base > sections.addresses.size()) {
    throw std::runtime_error("DW_AT_addr_base is out of range");
  }
  const auto contribution_start = static_cast<std::size_t>(base - 8);
  const auto length = read_at<std::uint32_t>(sections.addresses, contribution_start,
                                             ".debug_addr length");
  if (length == 0xffffffffU || length < 4 ||
      length > sections.addresses.size() - contribution_start - 4) {
    throw std::runtime_error("unsupported .debug_addr contribution");
  }
  const auto contribution_end = contribution_start + 4 + static_cast<std::size_t>(length);
  if (read_at<std::uint16_t>(sections.addresses, contribution_start + 4,
                             ".debug_addr version") != 5 ||
      read_at<std::uint8_t>(sections.addresses, contribution_start + 6,
                            ".debug_addr address size") != 8 ||
      read_at<std::uint8_t>(sections.addresses, contribution_start + 7,
                            ".debug_addr segment size") != 0) {
    throw std::runtime_error("unsupported .debug_addr header");
  }
  if (index > (std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(base)) / 8) {
    throw std::runtime_error("DW_FORM_addrx index overflows address table");
  }
  const auto entry = static_cast<std::size_t>(base) + static_cast<std::size_t>(index) * 8;
  if (entry > contribution_end || 8 > contribution_end - entry) {
    throw std::runtime_error("DW_FORM_addrx index is out of range");
  }
  return read_at<std::uint64_t>(sections.addresses, entry, "DW_FORM_addrx address");
}

LoclistsContribution loclists_contribution_at(const DebugSections& sections,
                                               std::size_t start) {
  if (sections.location_lists.empty()) {
    throw std::runtime_error("DWARF5 local value location list requires .debug_loclists");
  }
  if (start > sections.location_lists.size() ||
      12 > sections.location_lists.size() - start) {
    throw std::runtime_error(".debug_loclists header is truncated");
  }
  const auto length = read_at<std::uint32_t>(sections.location_lists, start,
                                             ".debug_loclists length");
  if (length == 0xffffffffU || length < 8 ||
      length > sections.location_lists.size() - start - 4) {
    throw std::runtime_error("unsupported .debug_loclists contribution");
  }
  const auto end = start + 4 + static_cast<std::size_t>(length);
  if (read_at<std::uint16_t>(sections.location_lists, start + 4,
                             ".debug_loclists version") != 5 ||
      read_at<std::uint8_t>(sections.location_lists, start + 6,
                            ".debug_loclists address size") != 8 ||
      read_at<std::uint8_t>(sections.location_lists, start + 7,
                            ".debug_loclists segment size") != 0) {
    throw std::runtime_error("unsupported .debug_loclists header");
  }
  const auto offset_count = read_at<std::uint32_t>(sections.location_lists, start + 8,
                                                   ".debug_loclists offset count");
  const auto offsets_base = start + 12;
  if (offset_count > (end - offsets_base) / 4) {
    throw std::runtime_error(".debug_loclists offset table extends past contribution");
  }
  return LoclistsContribution{start, offsets_base, end, offset_count};
}

LoclistsContribution loclists_contribution_for_offset(const DebugSections& sections,
                                                      std::uint64_t offset) {
  if (offset > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error("DWARF5 location-list offset is too large");
  }
  const auto wanted = static_cast<std::size_t>(offset);
  std::size_t start = 0;
  while (start < sections.location_lists.size()) {
    const auto contribution = loclists_contribution_at(sections, start);
    if (wanted >= contribution.offsets_base && wanted < contribution.end) {
      return contribution;
    }
    if (contribution.end <= start) {
      throw std::runtime_error(".debug_loclists parser did not advance");
    }
    start = contribution.end;
  }
  throw std::runtime_error("DWARF5 location-list offset is outside all contributions");
}

std::uint64_t indexed_loclist_offset(const DebugSections& sections, std::uint64_t base,
                                     std::uint64_t index) {
  if (base < 12 || base > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error("DW_AT_loclists_base is out of range");
  }
  const auto contribution =
      loclists_contribution_at(sections, static_cast<std::size_t>(base) - 12);
  if (contribution.offsets_base != base) {
    throw std::runtime_error("DW_AT_loclists_base does not point at the offset table");
  }
  if (index >= contribution.offset_count) {
    throw std::runtime_error("DW_FORM_loclistx index is out of range");
  }
  const auto entry = contribution.offsets_base + static_cast<std::size_t>(index) * 4;
  const auto relative = read_at<std::uint32_t>(sections.location_lists, entry,
                                               "DW_FORM_loclistx offset");
  const auto absolute = add_unsigned(base, relative, "DW_FORM_loclistx offset");
  if (absolute >= contribution.end) {
    throw std::runtime_error("DW_FORM_loclistx resolves outside its contribution");
  }
  return absolute;
}

void resolve_dwarf5_indexes(const DebugSections& sections, std::vector<Die>& dies) {
  if (dies.empty() || dies.front().parent) {
    throw std::runtime_error("DWARF5 unit has no root DIE");
  }
  const auto* str_base = attribute(dies.front(), kDwAtStrOffsetsBase);
  const auto* addr_base = attribute(dies.front(), kDwAtAddrBase);
  const auto* loclists_base = attribute(dies.front(), kDwAtLoclistsBase);

  for (auto& die : dies) {
    for (auto& value : die.attributes) {
      if (value.form == kDwFormStrx1) {
        if (str_base == nullptr || str_base->form != kDwFormSecOffset) {
          throw std::runtime_error("DW_FORM_strx1 requires DW_AT_str_offsets_base");
        }
        value.text = indexed_string(sections, str_base->number, value.number);
      } else if (value.form == kDwFormAddrx) {
        if (addr_base == nullptr || addr_base->form != kDwFormSecOffset) {
          throw std::runtime_error("DW_FORM_addrx requires DW_AT_addr_base");
        }
        value.number = indexed_address(sections, addr_base->number, value.number);
        value.form = kDwFormAddr;
      } else if (value.form == kDwFormLoclistx) {
        if (loclists_base == nullptr || loclists_base->form != kDwFormSecOffset) {
          throw std::runtime_error("DW_FORM_loclistx requires DW_AT_loclists_base");
        }
        value.number = indexed_loclist_offset(sections, loclists_base->number, value.number);
      }
    }
  }
}

std::vector<Die> parse_unit_dies(const DebugSections& sections, std::size_t unit_start,
                                 std::size_t& next_unit, std::uint16_t& unit_version) {
  std::size_t cursor = unit_start;
  const auto unit_length = read_scalar<std::uint32_t>(sections.info, cursor,
                                                      sections.info.size(),
                                                      "DWARF unit length");
  if (unit_length == 0xffffffffU) throw std::runtime_error("DWARF64 debug info is unsupported");
  if (unit_length > sections.info.size() - cursor) {
    throw std::runtime_error("DWARF compilation unit extends past .debug_info");
  }
  const auto unit_end = cursor + static_cast<std::size_t>(unit_length);
  next_unit = unit_end;
  unit_version = read_scalar<std::uint16_t>(sections.info, cursor, unit_end,
                                            "DWARF unit version");

  std::uint32_t abbrev_offset = 0;
  std::uint8_t address_size = 0;
  if (unit_version == 4) {
    abbrev_offset = read_scalar<std::uint32_t>(sections.info, cursor, unit_end,
                                               "DWARF abbreviation offset");
    address_size = read_scalar<std::uint8_t>(sections.info, cursor, unit_end,
                                             "DWARF address size");
  } else if (unit_version == 5) {
    const auto unit_type = read_scalar<std::uint8_t>(sections.info, cursor, unit_end,
                                                     "DWARF5 unit type");
    if (unit_type != kDwUtCompile) {
      throw std::runtime_error("only DWARF5 compile units are supported for local values");
    }
    address_size = read_scalar<std::uint8_t>(sections.info, cursor, unit_end,
                                             "DWARF5 address size");
    abbrev_offset = read_scalar<std::uint32_t>(sections.info, cursor, unit_end,
                                               "DWARF5 abbreviation offset");
  } else {
    throw std::runtime_error("only DWARF4/5 local-value units are supported");
  }
  if (address_size != 8) {
    throw std::runtime_error("only 8-byte DWARF local-value addresses are supported");
  }
  const auto abbreviations = parse_abbreviations(sections.abbrev, abbrev_offset);

  std::vector<Die> dies;
  std::vector<std::size_t> parents;
  while (cursor < unit_end) {
    const auto die_offset = cursor;
    const auto code = read_uleb(sections.info, cursor, unit_end, "DIE abbreviation code");
    if (code == 0) {
      if (parents.empty()) break;
      parents.pop_back();
      continue;
    }
    const auto abbreviation = abbreviations.find(code);
    if (abbreviation == abbreviations.end()) {
      throw std::runtime_error("DIE references an unknown abbreviation code");
    }
    Die die{die_offset, abbreviation->second.tag,
            parents.empty() ? std::optional<std::size_t>{} : parents.back(), {}};
    for (const auto& spec : abbreviation->second.attributes) {
      die.attributes.push_back(read_attribute(spec, sections, cursor, unit_end, unit_start,
                                              address_size));
    }
    dies.push_back(std::move(die));
    if (abbreviation->second.has_children) parents.push_back(dies.size() - 1);
  }
  if (cursor != unit_end) {
    throw std::runtime_error("unexpected trailing data in DWARF compilation unit");
  }
  if (unit_version == 5) resolve_dwarf5_indexes(sections, dies);
  return dies;
}

bool die_contains_pc(const Die& die, std::uint64_t pc) {
  const auto* low = attribute(die, kDwAtLowPc);
  const auto* high = attribute(die, kDwAtHighPc);
  if (low == nullptr || high == nullptr || low->form != kDwFormAddr) return false;
  const auto high_pc = high->form == kDwFormAddr
                           ? high->number
                           : add_unsigned(low->number, high->number, "DWARF high_pc");
  return pc >= low->number && pc < high_pc;
}

std::optional<std::size_t> active_scope_depth(const std::vector<Die>& dies,
                                              std::size_t value_index,
                                              std::size_t subprogram,
                                              std::uint64_t pc) {
  auto parent = dies[value_index].parent;
  if (!parent) return std::nullopt;
  std::size_t depth = 0;
  while (*parent != subprogram) {
    const auto& scope = dies[*parent];
    if (scope.tag != kDwTagLexicalBlock && scope.tag != kDwTagInlinedSubroutine) {
      return std::nullopt;
    }
    const auto* low = attribute(scope, kDwAtLowPc);
    const auto* high = attribute(scope, kDwAtHighPc);
    if (low == nullptr || high == nullptr || low->form != kDwFormAddr) {
      throw std::runtime_error(
          "local scope does not use the supported DW_AT_low_pc/DW_AT_high_pc range");
    }
    if (!die_contains_pc(scope, pc)) return std::nullopt;
    ++depth;
    parent = scope.parent;
    if (!parent) return std::nullopt;
  }
  return depth;
}

IntegerType resolve_integer_type(const std::vector<Die>& dies, std::uint64_t type_offset) {
  for (unsigned depth = 0; depth < 16; ++depth) {
    const auto index = die_index_by_offset(dies, type_offset);
    if (!index) throw std::runtime_error("local variable type references an unknown DIE");
    const auto& die = dies[*index];
    if (die.tag == kDwTagTypedef || die.tag == kDwTagConstType) {
      const auto* type = attribute(die, kDwAtType);
      if (type == nullptr || type->form != kDwFormRef4) {
        throw std::runtime_error(
            "local type wrapper does not use the supported DW_FORM_ref4 type link");
      }
      type_offset = type->number;
      continue;
    }
    if (die.tag != kDwTagBaseType) {
      throw std::runtime_error("local variable type is not a supported typedef/const/base-type chain");
    }
    const auto* size = attribute(die, kDwAtByteSize);
    const auto* encoding = attribute(die, kDwAtEncoding);
    if (size == nullptr || encoding == nullptr || size->number == 0 || size->number > 8) {
      throw std::runtime_error("local integer base type has an unsupported byte size");
    }
    if (encoding->number != kDwAteSigned && encoding->number != kDwAteUnsigned) {
      throw std::runtime_error("local base type is not a supported signed/unsigned integer");
    }
    return IntegerType{static_cast<std::size_t>(size->number),
                       encoding->number == kDwAteSigned};
  }
  throw std::runtime_error("local variable typedef/const chain is too deep");
}

bool is_constant_member_offset_form(std::uint64_t form) {
  return form == kDwFormData1 || form == kDwFormData2 || form == kDwFormData4 ||
         form == kDwFormData8;
}

ValueType resolve_value_type(const std::vector<Die>& dies, std::uint64_t type_offset) {
  for (unsigned depth = 0; depth < 16; ++depth) {
    const auto index = die_index_by_offset(dies, type_offset);
    if (!index) throw std::runtime_error("local variable type references an unknown DIE");
    const auto& die = dies[*index];
    if (die.tag == kDwTagTypedef || die.tag == kDwTagConstType) {
      const auto* type = attribute(die, kDwAtType);
      if (type == nullptr || type->form != kDwFormRef4) {
        throw std::runtime_error(
            "local type wrapper does not use the supported DW_FORM_ref4 type link");
      }
      type_offset = type->number;
      continue;
    }
    if (die.tag == kDwTagPointerType) {
      const auto* size = attribute(die, kDwAtByteSize);
      const auto pointer_size = size == nullptr ? std::size_t{8}
                                                : static_cast<std::size_t>(size->number);
      if (pointer_size != 8) {
        throw std::runtime_error("local pointer type has an unsupported byte size");
      }
      const auto* pointee = attribute(die, kDwAtType);
      if (pointee == nullptr || pointee->form != kDwFormRef4) {
        throw std::runtime_error("local pointer type has no supported DW_FORM_ref4 pointee");
      }
      (void)resolve_integer_type(dies, pointee->number);
      return ValueType{pointer_size, false, LocalValueKind::Pointer, {}};
    }
    if (die.tag == kDwTagBaseType) {
      const auto integer = resolve_integer_type(dies, type_offset);
      return ValueType{integer.byte_size, integer.is_signed, LocalValueKind::Integer, {}};
    }
    if (die.tag == kDwTagStructureType) {
      const auto* size = attribute(die, kDwAtByteSize);
      if (size == nullptr || size->number == 0 || size->number > kMaxLocalStructSize) {
        throw std::runtime_error("local struct has an unsupported byte size");
      }
      const auto struct_size = static_cast<std::size_t>(size->number);
      std::vector<AggregateMemberType> members;
      for (std::size_t child_index = 0; child_index < dies.size(); ++child_index) {
        if (dies[child_index].parent != *index) continue;
        const auto& member = dies[child_index];
        if (member.tag != kDwTagMember) {
          throw std::runtime_error("local struct has an unsupported direct child DIE");
        }
        if (members.size() >= kMaxLocalStructMembers) {
          throw std::runtime_error("local struct has too many direct members");
        }
        const auto* member_name = attribute(member, kDwAtName);
        const auto* member_type = attribute(member, kDwAtType);
        const auto* member_offset = attribute(member, kDwAtDataMemberLocation);
        if (member_name == nullptr || member_name->text.empty()) {
          throw std::runtime_error("local struct member has no supported name");
        }
        if (member_type == nullptr || member_type->form != kDwFormRef4) {
          throw std::runtime_error("local struct member has no supported DW_FORM_ref4 type");
        }
        if (member_offset == nullptr || !is_constant_member_offset_form(member_offset->form)) {
          throw std::runtime_error("local struct member has no supported constant offset");
        }
        if (member_offset->number > std::numeric_limits<std::size_t>::max()) {
          throw std::runtime_error("local struct member offset is too large");
        }
        const auto offset = static_cast<std::size_t>(member_offset->number);
        const auto integer = resolve_integer_type(dies, member_type->number);
        if (offset > struct_size || integer.byte_size > struct_size - offset) {
          throw std::runtime_error("local struct member extends past aggregate storage");
        }
        members.push_back(AggregateMemberType{member_name->text, offset, integer});
      }
      if (members.empty()) {
        throw std::runtime_error("local struct has no supported direct members");
      }
      return ValueType{struct_size, false, LocalValueKind::Structure, std::move(members)};
    }
    throw std::runtime_error(
        "local variable type is not a supported typedef/const/base-type/pointer/structure chain");
  }
  throw std::runtime_error("local variable type chain is too deep");
}

std::int64_t fbreg_offset(const std::vector<std::byte>& expression) {
  if (expression.empty() || std::to_integer<std::uint8_t>(expression.front()) != kDwOpFbreg) {
    throw std::runtime_error("local variable location is not the supported DW_OP_fbreg form");
  }
  std::size_t cursor = 1;
  const auto offset = read_sleb(expression, cursor, expression.size(), "DW_OP_fbreg offset");
  if (cursor != expression.size()) {
    throw std::runtime_error("unsupported trailing operations in local variable location");
  }
  return offset;
}

std::vector<std::byte> active_dwarf4_location_expression(
    const DebugSections& sections, std::uint64_t offset, std::uint64_t virtual_pc,
    std::uint64_t initial_base) {
  if (sections.locations.empty()) {
    throw std::runtime_error("local value location list requires .debug_loc");
  }
  if (offset >= sections.locations.size()) {
    throw std::runtime_error("local value location-list offset is out of range");
  }

  std::size_t cursor = static_cast<std::size_t>(offset);
  std::uint64_t base_address = initial_base;
  while (cursor < sections.locations.size()) {
    const auto begin = read_scalar<std::uint64_t>(sections.locations, cursor,
                                                  sections.locations.size(),
                                                  "DWARF4 location-list begin");
    const auto end = read_scalar<std::uint64_t>(sections.locations, cursor,
                                                sections.locations.size(),
                                                "DWARF4 location-list end");
    if (begin == 0 && end == 0) break;
    if (begin == std::numeric_limits<std::uint64_t>::max()) {
      base_address = end;
      continue;
    }

    const auto length = read_scalar<std::uint16_t>(sections.locations, cursor,
                                                   sections.locations.size(),
                                                   "DWARF4 location expression length");
    if (length > sections.locations.size() - cursor) {
      throw std::runtime_error("DWARF4 location expression extends past .debug_loc");
    }
    const auto expression_end = cursor + static_cast<std::size_t>(length);
    const auto range_begin = add_unsigned(base_address, begin, "location-list range begin");
    const auto range_end = add_unsigned(base_address, end, "location-list range end");
    if (range_end < range_begin) {
      throw std::runtime_error("DWARF4 location-list range is reversed");
    }
    if (virtual_pc >= range_begin && virtual_pc < range_end) {
      return std::vector<std::byte>(sections.locations.begin() + cursor,
                                    sections.locations.begin() + expression_end);
    }
    cursor = expression_end;
  }
  throw std::runtime_error("local value has no location for the current PC");
}

std::vector<std::byte> active_dwarf5_location_expression(
    const DebugSections& sections, std::uint64_t offset, std::uint64_t virtual_pc,
    std::uint64_t initial_base) {
  const auto contribution = loclists_contribution_for_offset(sections, offset);
  std::size_t cursor = static_cast<std::size_t>(offset);
  std::uint64_t base_address = initial_base;
  while (cursor < contribution.end) {
    const auto entry = read_scalar<std::uint8_t>(sections.location_lists, cursor,
                                                 contribution.end,
                                                 "DWARF5 location-list entry");
    if (entry == kDwLleEndOfList) break;
    if (entry == kDwLleBaseAddress) {
      base_address = read_scalar<std::uint64_t>(sections.location_lists, cursor,
                                                contribution.end,
                                                "DW_LLE_base_address");
      continue;
    }
    if (entry != kDwLleOffsetPair) {
      throw std::runtime_error("unsupported DWARF5 location-list entry: " +
                               std::to_string(entry));
    }

    const auto begin = read_uleb(sections.location_lists, cursor, contribution.end,
                                 "DW_LLE_offset_pair begin");
    const auto end = read_uleb(sections.location_lists, cursor, contribution.end,
                               "DW_LLE_offset_pair end");
    const auto length = read_uleb(sections.location_lists, cursor, contribution.end,
                                  "DWARF5 location expression length");
    if (length > contribution.end - cursor) {
      throw std::runtime_error("DWARF5 location expression extends past contribution");
    }
    const auto expression_end = cursor + static_cast<std::size_t>(length);
    const auto range_begin = add_unsigned(base_address, begin, "DWARF5 location range begin");
    const auto range_end = add_unsigned(base_address, end, "DWARF5 location range end");
    if (range_end < range_begin) {
      throw std::runtime_error("DWARF5 location-list range is reversed");
    }
    if (virtual_pc >= range_begin && virtual_pc < range_end) {
      return std::vector<std::byte>(sections.location_lists.begin() + cursor,
                                    sections.location_lists.begin() + expression_end);
    }
    cursor = expression_end;
  }
  throw std::runtime_error("local value has no location for the current PC");
}

std::vector<std::byte> active_location_expression(const DebugSections& sections,
                                                  std::uint64_t offset,
                                                  std::uint64_t virtual_pc,
                                                  std::uint64_t initial_base,
                                                  std::uint16_t unit_version) {
  if (unit_version == 4) {
    return active_dwarf4_location_expression(sections, offset, virtual_pc, initial_base);
  }
  if (unit_version == 5) {
    return active_dwarf5_location_expression(sections, offset, virtual_pc, initial_base);
  }
  throw std::runtime_error("unsupported DWARF unit version for location list");
}

std::uint64_t frame_base(const Debugger& debugger, const ElfFile& module,
                         const std::vector<std::byte>& expression) {
  const auto regs = debugger.registers();
  if (expression.size() != 1) {
    throw std::runtime_error("unsupported compound DW_AT_frame_base expression");
  }
  const auto op = std::to_integer<std::uint8_t>(expression.front());
  if (op == kDwOpReg6) return regs.rbp;
  if (op != kDwOpCallFrameCfa) {
    throw std::runtime_error("unsupported DW_AT_frame_base operation");
  }

  const EhFrame cfi(module.path());
  if (!cfi.available()) throw std::runtime_error("DW_OP_call_frame_cfa requires .eh_frame");
  const EhFrameCursor current{static_cast<std::uintptr_t>(regs.rip),
                              static_cast<std::uintptr_t>(regs.rsp),
                              static_cast<std::uintptr_t>(regs.rbp)};
  const auto caller = cfi.caller_frame(debugger, module, current);
  if (!caller) throw std::runtime_error("CFI did not cover the current source frame");
  return caller->stack_pointer;
}

std::uint64_t decode_integer(const std::vector<std::byte>& bytes, std::size_t offset,
                             std::size_t byte_size) {
  if (byte_size == 0 || byte_size > 8 || offset > bytes.size() ||
      byte_size > bytes.size() - offset) {
    throw std::runtime_error("local integer decode exceeds owned storage");
  }
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < byte_size; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

LocalScalarValue decode_structure(const ElfFile& module, std::string_view name,
                                  const ValueType& value_type,
                                  const std::vector<std::byte>& bytes) {
  if (value_type.kind != LocalValueKind::Structure || bytes.size() != value_type.byte_size) {
    throw std::runtime_error("local struct bytes do not match owned aggregate storage");
  }
  LocalScalarValue result{module.path(), std::string(name), 0, value_type.byte_size,
                          false, LocalValueKind::Structure};
  result.members.reserve(value_type.members.size());
  for (const auto& member : value_type.members) {
    result.members.push_back(LocalStructMember{
        member.name, decode_integer(bytes, member.offset, member.integer.byte_size),
        member.integer.byte_size, member.integer.is_signed});
  }
  return result;
}

std::vector<std::byte> evaluate_register_piece_structure(
    const std::vector<std::byte>& expression, const Debugger& debugger,
    const ValueType& value_type) {
  if (value_type.kind != LocalValueKind::Structure || value_type.byte_size != 16) {
    throw std::runtime_error(
        "register-piece local requires the compiler-proven 16-byte structure shape");
  }
  std::size_t cursor = 0;
  const auto expect_op = [&](std::uint8_t expected, const char* what) {
    if (cursor >= expression.size() ||
        std::to_integer<std::uint8_t>(expression[cursor]) != expected) {
      throw std::runtime_error(std::string("register-piece local requires ") + what);
    }
    ++cursor;
  };
  expect_op(kDwOpReg5, "DW_OP_reg5 for the first piece");
  expect_op(kDwOpPiece, "DW_OP_piece after the first register");
  if (read_uleb(expression, cursor, expression.size(), "first DW_OP_piece size") != 8) {
    throw std::runtime_error("register-piece local requires an 8-byte first piece");
  }
  expect_op(kDwOpReg4, "DW_OP_reg4 for the second piece");
  expect_op(kDwOpPiece, "DW_OP_piece after the second register");
  if (read_uleb(expression, cursor, expression.size(), "second DW_OP_piece size") != 8) {
    throw std::runtime_error("register-piece local requires an 8-byte second piece");
  }
  if (cursor != expression.size()) {
    throw std::runtime_error("unsupported trailing operations after register-piece local");
  }

  const auto regs = debugger.registers();
  std::vector<std::byte> bytes(16);
  const auto store = [&](std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
      bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
  };
  store(0, regs.rdi);
  store(8, regs.rsi);
  return bytes;
}

std::uint64_t read_integer(const Debugger& debugger, std::uint64_t address,
                           std::size_t byte_size) {
  const auto bytes = debugger.read_memory(static_cast<std::uintptr_t>(address), byte_size);
  if (bytes.size() != byte_size) throw std::runtime_error("short local-variable memory read");
  return decode_integer(bytes, 0, byte_size);
}

std::uint64_t truncate_integer(std::uint64_t value, std::size_t byte_size) {
  if (byte_size == 8) return value;
  const auto bits = static_cast<unsigned>(byte_size * 8U);
  return value & ((std::uint64_t{1} << bits) - 1U);
}

std::uint64_t evaluate_composite_stack_value(const std::vector<std::byte>& expression,
                                             const Debugger& debugger) {
  if (expression.empty() ||
      std::to_integer<std::uint8_t>(expression.front()) != kDwOpBreg8) {
    throw std::runtime_error(
        "local value location is not the compiler-proven composite stack form");
  }

  const auto regs = debugger.registers();
  const auto register_value = [&](std::uint8_t op) -> std::uint64_t {
    switch (op) {
      case kDwOpBreg1:
        return regs.rdx;
      case kDwOpBreg2:
        return regs.rcx;
      case kDwOpBreg4:
        return regs.rsi;
      case kDwOpBreg5:
        return regs.rdi;
      case kDwOpBreg8:
        return regs.r8;
      case kDwOpBreg9:
        return regs.r9;
      default:
        throw std::runtime_error("unsupported register in composite local-value expression");
    }
  };

  std::vector<std::uint64_t> stack;
  std::size_t cursor = 0;
  while (cursor < expression.size()) {
    const auto op = std::to_integer<std::uint8_t>(expression[cursor++]);
    if (op == kDwOpBreg1 || op == kDwOpBreg2 || op == kDwOpBreg4 ||
        op == kDwOpBreg5 || op == kDwOpBreg8 || op == kDwOpBreg9) {
      const auto offset = read_sleb(expression, cursor, expression.size(),
                                    "composite DW_OP_breg offset");
      if (offset != 0) {
        throw std::runtime_error(
            "composite local-value expression only supports compiler-proven zero breg offsets");
      }
      stack.push_back(register_value(op));
      continue;
    }
    if (op == kDwOpConst1u) {
      if (cursor >= expression.size()) {
        throw std::runtime_error("truncated DW_OP_const1u in composite local-value expression");
      }
      stack.push_back(std::to_integer<std::uint8_t>(expression[cursor++]));
      continue;
    }
    if (op == kDwOpLit8 || op == kDwOpLit16 || op == kDwOpLit24) {
      stack.push_back(op - 0x30U);
      continue;
    }
    if (op == kDwOpShl || op == kDwOpXor) {
      if (stack.size() < 2) {
        throw std::runtime_error("composite local-value binary operation underflows stack");
      }
      const auto rhs = stack.back();
      stack.pop_back();
      const auto lhs = stack.back();
      stack.pop_back();
      if (op == kDwOpShl) {
        if (rhs >= 64) {
          throw std::runtime_error("composite local-value shift count is out of range");
        }
        stack.push_back(lhs << static_cast<unsigned>(rhs));
      } else {
        stack.push_back(lhs ^ rhs);
      }
      continue;
    }
    if (op == kDwOpStackValue) {
      if (cursor != expression.size() || stack.size() != 1) {
        throw std::runtime_error(
            "composite local-value stack_value requires one final stack result");
      }
      return stack.back();
    }
    throw std::runtime_error("unsupported operation in compiler-proven composite local value");
  }
  throw std::runtime_error("composite local-value expression is missing DW_OP_stack_value");
}

std::uint64_t evaluate_breg3_xor_stack_value(
    const std::vector<std::byte>& expression, const Debugger& debugger) {
  if (expression.empty() ||
      std::to_integer<std::uint8_t>(expression.front()) != kDwOpBreg3) {
    throw std::runtime_error(
        "local value location is not the compiler-proven DW_OP_breg3 XOR form");
  }
  std::size_t cursor = 1;
  const auto offset =
      read_sleb(expression, cursor, expression.size(), "DW_OP_breg3 offset");
  if (cursor >= expression.size() ||
      std::to_integer<std::uint8_t>(expression[cursor]) != kDwOpConstu) {
    throw std::runtime_error(
        "DW_OP_breg3 XOR value requires the compiler-proven DW_OP_constu");
  }
  ++cursor;
  const auto constant =
      read_uleb(expression, cursor, expression.size(), "DW_OP_constu value");
  if (cursor >= expression.size() ||
      std::to_integer<std::uint8_t>(expression[cursor]) != kDwOpXor) {
    throw std::runtime_error(
        "DW_OP_breg3 XOR value requires the compiler-proven DW_OP_xor");
  }
  ++cursor;
  if (cursor >= expression.size() ||
      std::to_integer<std::uint8_t>(expression[cursor]) != kDwOpStackValue) {
    throw std::runtime_error(
        "DW_OP_breg3 XOR value requires the compiler-proven trailing DW_OP_stack_value");
  }
  ++cursor;
  if (cursor != expression.size()) {
    throw std::runtime_error(
        "unsupported trailing operations after DW_OP_breg3 XOR value");
  }
  const auto base = add_signed(debugger.registers().rbx, offset,
                               "DW_OP_breg3 XOR base");
  return base ^ constant;
}

std::uint64_t evaluate_breg5_stack_value(const std::vector<std::byte>& expression,
                                         const Debugger& debugger) {
  if (expression.empty() ||
      std::to_integer<std::uint8_t>(expression.front()) != kDwOpBreg5) {
    throw std::runtime_error(
        "local value location is not the compiler-proven DW_OP_breg5 form");
  }
  std::size_t cursor = 1;
  const auto offset =
      read_sleb(expression, cursor, expression.size(), "DW_OP_breg5 offset");
  if (cursor >= expression.size()) {
    throw std::runtime_error("DW_OP_breg5 value is missing a compiler-proven operation");
  }

  const auto next = std::to_integer<std::uint8_t>(expression[cursor]);
  if (next == kDwOpStackValue) {
    ++cursor;
    if (cursor != expression.size()) {
      throw std::runtime_error("unsupported trailing operations after DW_OP_breg5 value");
    }
    return add_signed(debugger.registers().rdi, offset, "DW_OP_breg5 value");
  }

  if (next != kDwOpDeref || offset != 0) {
    throw std::runtime_error(
        "DW_OP_breg5 value is not the compiler-proven stack/dereference form");
  }

  auto value = add_signed(debugger.registers().rdi, offset,
                          "DW_OP_breg5 dereference address");
  std::size_t dereference_count = 0;
  while (cursor < expression.size() &&
         std::to_integer<std::uint8_t>(expression[cursor]) == kDwOpDeref) {
    if (dereference_count >= 2) {
      throw std::runtime_error(
          "DW_OP_breg5 dereference chain exceeds the compiler-proven two-level bound");
    }
    ++cursor;
    value = read_integer(debugger, value, sizeof(std::uint64_t));
    ++dereference_count;
  }
  if (dereference_count == 0) {
    throw std::runtime_error("DW_OP_breg5 dereference value is missing DW_OP_deref");
  }
  if (cursor >= expression.size() ||
      std::to_integer<std::uint8_t>(expression[cursor]) != kDwOpConstu) {
    throw std::runtime_error(
        "DW_OP_breg5 dereference value requires the compiler-proven DW_OP_constu");
  }
  ++cursor;
  const auto constant =
      read_uleb(expression, cursor, expression.size(), "DW_OP_constu value");
  if (cursor >= expression.size() ||
      std::to_integer<std::uint8_t>(expression[cursor]) != kDwOpXor) {
    throw std::runtime_error(
        "DW_OP_breg5 dereference value requires the compiler-proven DW_OP_xor");
  }
  ++cursor;
  if (cursor >= expression.size() ||
      std::to_integer<std::uint8_t>(expression[cursor]) != kDwOpStackValue) {
    throw std::runtime_error(
        "DW_OP_breg5 dereference value requires the compiler-proven trailing DW_OP_stack_value");
  }
  ++cursor;
  if (cursor != expression.size()) {
    throw std::runtime_error(
        "unsupported trailing operations after DW_OP_breg5 dereference value");
  }
  return value ^ constant;
}

std::uint64_t evaluate_breg5_memory_address(
    const std::vector<std::byte>& expression, const Debugger& debugger) {
  if (expression.empty() ||
      std::to_integer<std::uint8_t>(expression.front()) != kDwOpBreg5) {
    throw std::runtime_error(
        "local memory location is not the compiler-proven DW_OP_breg5 form");
  }
  std::size_t cursor = 1;
  const auto offset =
      read_sleb(expression, cursor, expression.size(), "DW_OP_breg5 memory offset");
  if (cursor != expression.size()) {
    throw std::runtime_error(
        "unsupported trailing operations after DW_OP_breg5 memory location");
  }
  return add_signed(debugger.registers().rdi, offset,
                    "DW_OP_breg5 memory address");
}

std::uint64_t evaluate_entry_rdi(
    const std::vector<std::byte>& expression, const Debugger& debugger,
    const ElfFile& module, const Die& subprogram_die) {
  std::size_t cursor = 1;
  const auto nested_length =
      read_uleb(expression, cursor, expression.size(), "DW_OP_entry_value length");
  if (nested_length != 1 || cursor >= expression.size() ||
      std::to_integer<std::uint8_t>(expression[cursor]) != kDwOpReg5) {
    throw std::runtime_error(
        "DW_OP_entry_value only supports the compiler-proven nested DW_OP_reg5 form");
  }
  ++cursor;
  if (cursor >= expression.size() ||
      std::to_integer<std::uint8_t>(expression[cursor]) != kDwOpStackValue) {
    throw std::runtime_error(
        "DW_OP_entry_value requires the compiler-proven trailing DW_OP_stack_value");
  }
  ++cursor;
  if (cursor != expression.size()) {
    throw std::runtime_error(
        "unsupported trailing operations after DW_OP_entry_value");
  }

  const auto* low_pc = attribute(subprogram_die, kDwAtLowPc);
  if (low_pc == nullptr || low_pc->form != kDwFormAddr) {
    throw std::runtime_error(
        "DW_OP_entry_value requires a subprogram with supported DW_AT_low_pc");
  }
  const auto bias = module.load_bias(debugger.pid());
  if (low_pc->number > std::numeric_limits<std::uint64_t>::max() - bias) {
    throw std::runtime_error("subprogram entry runtime address overflows");
  }
  const auto runtime_entry = static_cast<std::uintptr_t>(low_pc->number + bias);
  const auto snapshot = debugger.breakpoint_register_snapshot(
      debugger.active_tid(), runtime_entry);
  if (!snapshot) {
    throw std::runtime_error(
        "DW_OP_entry_value requires an observed function-entry breakpoint snapshot");
  }
  return snapshot->rdi;
}

std::optional<LocalScalarValue> inspect_unit(const DebugSections& sections,
                                             const Debugger& debugger,
                                             const ElfFile& module,
                                             std::uint64_t virtual_pc,
                                             std::string_view name,
                                             std::size_t unit_start,
                                             std::size_t& next_unit) {
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
    if (subprogram) throw std::runtime_error("current PC matches multiple DWARF subprograms");
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
      throw std::runtime_error("ambiguous local value in current lexical scope: " +
                               std::string(name));
    }
  }
  if (!value_die_index) {
    if (nested_name) {
      throw std::runtime_error("local value is outside the current lexical scope: " +
                               std::string(name));
    }
    throw std::runtime_error("local value is not in the current subprogram: " +
                             std::string(name));
  }

  const auto& subprogram_die = dies[*subprogram];
  const auto& value_die = dies[*value_die_index];
  const auto* location = attribute(value_die, kDwAtLocation);
  const auto* type =
      attribute_with_abstract_origin(dies, *value_die_index, kDwAtType);
  if (location == nullptr) {
    throw std::runtime_error("local value has no DW_AT_location");
  }
  if (type == nullptr || type->form != kDwFormRef4) {
    throw std::runtime_error("local value has no supported DW_FORM_ref4 type");
  }

  const auto value_type = resolve_value_type(dies, type->number);
  std::vector<std::byte> location_expression;
  if (location->form == kDwFormExprloc) {
    location_expression = location->expression;
  } else if (location->form == kDwFormSecOffset || location->form == kDwFormLoclistx) {
    location_expression = active_location_expression(
        sections, location->number, virtual_pc, compilation_unit_base, unit_version);
  } else {
    throw std::runtime_error("local value has no supported DW_AT_location form");
  }

  if (value_type.kind != LocalValueKind::Structure && location_expression.size() == 1) {
    const auto op = std::to_integer<std::uint8_t>(location_expression.front());
    const auto regs = debugger.registers();
    if (op == kDwOpReg0) {
      const auto raw = truncate_integer(regs.rax, value_type.byte_size);
      return LocalScalarValue{module.path(), std::string(name), raw,
                              value_type.byte_size, value_type.is_signed,
                              value_type.kind};
    }
    if (op == kDwOpReg5) {
      const auto raw = truncate_integer(regs.rdi, value_type.byte_size);
      return LocalScalarValue{module.path(), std::string(name), raw,
                              value_type.byte_size, value_type.is_signed,
                              value_type.kind};
    }
  }

  if (value_type.kind != LocalValueKind::Structure && !location_expression.empty() &&
      std::to_integer<std::uint8_t>(location_expression.front()) == kDwOpEntryValue) {
    const auto raw = truncate_integer(
        evaluate_entry_rdi(location_expression, debugger, module, subprogram_die),
        value_type.byte_size);
    return LocalScalarValue{module.path(), std::string(name), raw,
                            value_type.byte_size, value_type.is_signed,
                            value_type.kind};
  }

  if (value_type.kind != LocalValueKind::Structure && !location_expression.empty() &&
      std::to_integer<std::uint8_t>(location_expression.front()) == kDwOpBreg8) {
    const auto raw = truncate_integer(
        evaluate_composite_stack_value(location_expression, debugger),
        value_type.byte_size);
    return LocalScalarValue{module.path(), std::string(name), raw,
                            value_type.byte_size, value_type.is_signed,
                            value_type.kind};
  }

  if (value_type.kind != LocalValueKind::Structure && !location_expression.empty() &&
      std::to_integer<std::uint8_t>(location_expression.front()) == kDwOpBreg3) {
    const auto raw = truncate_integer(
        evaluate_breg3_xor_stack_value(location_expression, debugger),
        value_type.byte_size);
    return LocalScalarValue{module.path(), std::string(name), raw,
                            value_type.byte_size, value_type.is_signed,
                            value_type.kind};
  }

  if (value_type.kind != LocalValueKind::Structure && !location_expression.empty() &&
      std::to_integer<std::uint8_t>(location_expression.front()) == kDwOpBreg5) {
    const bool stack_value =
        std::to_integer<std::uint8_t>(location_expression.back()) == kDwOpStackValue;
    const auto raw = stack_value
                         ? truncate_integer(
                               evaluate_breg5_stack_value(location_expression, debugger),
                               value_type.byte_size)
                         : read_integer(
                               debugger,
                               evaluate_breg5_memory_address(location_expression, debugger),
                               value_type.byte_size);
    return LocalScalarValue{module.path(), std::string(name), raw,
                            value_type.byte_size, value_type.is_signed,
                            value_type.kind};
  }

  if (value_type.kind == LocalValueKind::Structure && !location_expression.empty() &&
      std::to_integer<std::uint8_t>(location_expression.front()) == kDwOpReg5) {
    return decode_structure(module, name, value_type,
                            evaluate_register_piece_structure(location_expression, debugger,
                                                              value_type));
  }

  if (location_expression.empty() ||
      std::to_integer<std::uint8_t>(location_expression.front()) != kDwOpFbreg) {
    if (value_type.kind == LocalValueKind::Structure) {
      throw std::runtime_error(
          "local struct is not in a supported compiler-proven fbreg/register-piece form");
    }
    throw std::runtime_error(
        "local value location is not a supported compiler-proven register/stack/fbreg form");
  }
  const auto* base_expression = attribute(subprogram_die, kDwAtFrameBase);
  if (base_expression == nullptr || base_expression->form != kDwFormExprloc) {
    throw std::runtime_error("current subprogram has no supported DW_AT_frame_base expression");
  }
  const auto base = frame_base(debugger, module, base_expression->expression);
  const auto address = add_signed(base, fbreg_offset(location_expression),
                                  "local variable address");
  if (value_type.kind == LocalValueKind::Structure) {
    const auto bytes = debugger.read_memory(static_cast<std::uintptr_t>(address),
                                            value_type.byte_size);
    if (bytes.size() != value_type.byte_size) {
      throw std::runtime_error("short local-struct memory read");
    }
    return decode_structure(module, name, value_type, bytes);
  }
  return LocalScalarValue{module.path(), std::string(name),
                          read_integer(debugger, address, value_type.byte_size),
                          value_type.byte_size, value_type.is_signed,
                          value_type.kind};
}

std::uint64_t caller_frame_base(const Debugger& debugger, const ElfFile& module,
                                const InspectionFrameContext& frame,
                                const std::vector<std::byte>& expression) {
  if (expression.size() != 1) {
    throw std::runtime_error("caller-frame inspection does not support compound DW_AT_frame_base");
  }
  const auto op = std::to_integer<std::uint8_t>(expression.front());
  if (op == kDwOpReg6) {
    if (!frame.registers.rbp) {
      throw std::runtime_error("caller-frame DW_OP_reg6 requires recovered RBP");
    }
    return *frame.registers.rbp;
  }
  if (op != kDwOpCallFrameCfa) {
    throw std::runtime_error("unsupported caller-frame DW_AT_frame_base operation");
  }
  const EhFrame cfi(module.path());
  if (!cfi.available()) {
    throw std::runtime_error("caller-frame DW_OP_call_frame_cfa requires .eh_frame");
  }
  const EhFrameCursor current{frame.runtime_pc, frame.stack_pointer, frame.frame_pointer};
  const auto caller = cfi.caller_frame(debugger, module, current);
  if (!caller) {
    throw std::runtime_error("CFI did not recover caller-frame CFA");
  }
  return caller->stack_pointer;
}

std::optional<LocalScalarValue> inspect_caller_stack_unit(
    const DebugSections& sections, const Debugger& debugger, const ElfFile& module,
    const InspectionFrameContext& frame, std::uint64_t virtual_pc,
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
      throw std::runtime_error("inspection frame PC matches multiple DWARF subprograms");
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
      throw std::runtime_error("ambiguous local value in inspection-frame lexical scope: " +
                               std::string(name));
    }
  }
  if (!value_die_index) {
    if (nested_name) {
      throw std::runtime_error("local value is outside the inspection-frame lexical scope: " +
                               std::string(name));
    }
    throw std::runtime_error("local value is not in the inspection-frame subprogram: " +
                             std::string(name));
  }

  const auto& subprogram_die = dies[*subprogram];
  const auto& value_die = dies[*value_die_index];
  const auto* location = attribute(value_die, kDwAtLocation);
  const auto* type = attribute_with_abstract_origin(dies, *value_die_index, kDwAtType);
  if (location == nullptr) throw std::runtime_error("local value has no DW_AT_location");
  if (type == nullptr || type->form != kDwFormRef4) {
    throw std::runtime_error("local value has no supported DW_FORM_ref4 type");
  }

  const auto value_type = resolve_value_type(dies, type->number);
  std::vector<std::byte> location_expression;
  if (location->form == kDwFormExprloc) {
    location_expression = location->expression;
  } else if (location->form == kDwFormSecOffset || location->form == kDwFormLoclistx) {
    location_expression = active_location_expression(
        sections, location->number, virtual_pc, compilation_unit_base, unit_version);
  } else {
    throw std::runtime_error("local value has no supported DW_AT_location form");
  }

  if (location_expression.empty() ||
      std::to_integer<std::uint8_t>(location_expression.front()) != kDwOpFbreg) {
    throw std::runtime_error(
        "caller-frame local requires compiler-proven DW_OP_fbreg stack storage; "
        "unrecovered historical registers are unavailable");
  }
  const auto* base_expression = attribute(subprogram_die, kDwAtFrameBase);
  if (base_expression == nullptr || base_expression->form != kDwFormExprloc) {
    throw std::runtime_error(
        "caller-frame subprogram has no supported DW_AT_frame_base expression");
  }
  const auto base = caller_frame_base(debugger, module, frame, base_expression->expression);
  const auto address = add_signed(base, fbreg_offset(location_expression),
                                  "caller-frame local variable address");
  if (value_type.kind == LocalValueKind::Structure) {
    const auto bytes = debugger.read_memory(static_cast<std::uintptr_t>(address),
                                            value_type.byte_size);
    if (bytes.size() != value_type.byte_size) {
      throw std::runtime_error("short caller-frame local-struct memory read");
    }
    return decode_structure(module, name, value_type, bytes);
  }
  return LocalScalarValue{module.path(), std::string(name),
                          read_integer(debugger, address, value_type.byte_size),
                          value_type.byte_size, value_type.is_signed,
                          value_type.kind};
}

void validate_inspection_frame(const Debugger& debugger,
                               const InspectionFrameContext& frame) {
  if (debugger.state() != ProcessState::Stopped) {
    throw std::logic_error("inspection-frame local-value lookup requires a stopped tracee");
  }
  if (frame.process_pid != debugger.pid() || frame.tid != debugger.active_tid()) {
    throw std::logic_error("inspection frame belongs to a different process/TID domain");
  }
  const auto live = debugger.registers();
  if (static_cast<std::uintptr_t>(live.rip) != frame.origin_runtime_pc ||
      static_cast<std::uintptr_t>(live.rsp) != frame.origin_stack_pointer ||
      (frame.origin_frame_pointer &&
       static_cast<std::uintptr_t>(live.rbp) != *frame.origin_frame_pointer)) {
    throw std::logic_error("inspection frame is stale after execution state changed");
  }
  if (frame.runtime_pc == 0 || frame.stack_pointer == 0 || frame.module_path.empty()) {
    throw std::invalid_argument("inspection frame is incomplete");
  }
}

}  // namespace

LocalScalarValue inspect_local_value(const Debugger& debugger,
                                     const ElfFile& preferred_elf,
                                     std::string_view name) {
  if (name.empty()) throw std::invalid_argument("local variable name must not be empty");
  if (debugger.state() != ProcessState::Stopped) {
    throw std::logic_error("local-value inspection requires a stopped tracee");
  }

  const auto regs = debugger.registers();
  const auto runtime_pc = static_cast<std::uintptr_t>(regs.rip);
  const auto owner = find_module_symbol_by_runtime_address(debugger.pid(), runtime_pc,
                                                           preferred_elf);
  if (!owner) throw std::runtime_error("current PC is not owned by a file-backed symbol module");
  const ElfFile module(owner->module_path);
  const auto bias = module.load_bias(debugger.pid());
  if (runtime_pc < bias) throw std::runtime_error("current PC is below module load bias");
  const auto virtual_pc = static_cast<std::uint64_t>(runtime_pc - bias);
  const auto sections = read_debug_sections(module.path());

  std::size_t unit = 0;
  while (unit < sections.info.size()) {
    std::size_t next = unit;
    const auto result = inspect_unit(sections, debugger, module, virtual_pc, name, unit, next);
    if (result) return *result;
    if (next <= unit) throw std::runtime_error("DWARF parser did not advance to the next unit");
    unit = next;
  }
  throw std::runtime_error("current PC is not covered by a supported DWARF4/5 subprogram");
}

LocalScalarValue inspect_local_value(const Debugger& debugger,
                                     const ElfFile& preferred_elf,
                                     const InspectionFrameContext& frame,
                                     std::string_view name) {
  if (name.empty()) throw std::invalid_argument("local variable name must not be empty");
  validate_inspection_frame(debugger, frame);
  if (frame.index == 0) {
    return inspect_local_value(debugger, preferred_elf, name);
  }

  const ElfFile module(frame.module_path);
  const auto bias = module.load_bias(frame.process_pid);
  if (frame.runtime_pc < bias) {
    throw std::runtime_error("inspection-frame PC is below module load bias");
  }
  const auto virtual_pc = static_cast<std::uint64_t>(frame.runtime_pc - bias);
  const auto sections = read_debug_sections(module.path());
  std::size_t unit = 0;
  while (unit < sections.info.size()) {
    std::size_t next = unit;
    const auto result = inspect_caller_stack_unit(sections, debugger, module, frame,
                                                  virtual_pc, name, unit, next);
    if (result) return *result;
    if (next <= unit) {
      throw std::runtime_error("DWARF parser did not advance to the next unit");
    }
    unit = next;
  }
  throw std::runtime_error(
      "inspection-frame PC is not covered by a supported DWARF4/5 subprogram");
}

LocalIntegerValue inspect_local_integer(const Debugger& debugger,
                                        const ElfFile& preferred_elf,
                                        std::string_view name) {
  auto value = inspect_local_value(debugger, preferred_elf, name);
  if (value.kind != LocalValueKind::Integer) {
    throw std::runtime_error("local value is not an integer scalar: " + std::string(name));
  }
  return value;
}

LocalIntegerValue inspect_local_integer(const Debugger& debugger,
                                        const ElfFile& preferred_elf,
                                        const InspectionFrameContext& frame,
                                        std::string_view name) {
  auto value = inspect_local_value(debugger, preferred_elf, frame, name);
  if (value.kind != LocalValueKind::Integer) {
    throw std::runtime_error("local value is not an integer scalar: " + std::string(name));
  }
  return value;
}

std::optional<std::uint64_t> DwarfLineTable::find_virtual_source(
    std::string_view file, std::uint64_t line) const {
  if (file.empty() || line == 0) return std::nullopt;

  const std::filesystem::path query_path{std::string(file)};
  const auto normalized_query = query_path.lexically_normal();
  const bool qualified_query = query_path.is_absolute() || query_path.has_parent_path();

  std::optional<std::uint64_t> best_address;
  std::optional<std::string> matched_file;

  for (const auto& range : ranges_) {
    if (range.location.line != line) continue;

    const std::filesystem::path candidate_path(range.location.file);
    const bool matches = qualified_query
                             ? candidate_path.lexically_normal() == normalized_query
                             : candidate_path.filename() == normalized_query.filename();
    if (!matches) continue;

    if (!qualified_query) {
      const auto candidate_identity = normalized_source_path(candidate_path);
      if (matched_file && *matched_file != candidate_identity) {
        throw std::runtime_error("ambiguous source file for line lookup: " +
                                 std::string(file));
      }
      matched_file = candidate_identity;
    }

    if (!best_address || range.begin < *best_address) best_address = range.begin;
  }

  return best_address;
}

std::optional<std::uint64_t> DwarfLineTable::find_runtime_source(
    pid_t pid, std::string_view file, std::uint64_t line, const ElfFile& elf) const {
  const auto virtual_address = find_virtual_source(file, line);
  if (!virtual_address) return std::nullopt;
  const auto bias = elf.load_bias(pid);
  if (*virtual_address > std::numeric_limits<std::uint64_t>::max() - bias) {
    throw std::runtime_error("source address overflows runtime address space");
  }
  return *virtual_address + bias;
}

}  // namespace mdbg

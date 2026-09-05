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
constexpr std::uint64_t kDwTagTypedef = 0x16;
constexpr std::uint64_t kDwTagBaseType = 0x24;
constexpr std::uint64_t kDwTagSubprogram = 0x2e;
constexpr std::uint64_t kDwTagVariable = 0x34;

constexpr std::uint64_t kDwAtLocation = 0x02;
constexpr std::uint64_t kDwAtName = 0x03;
constexpr std::uint64_t kDwAtByteSize = 0x0b;
constexpr std::uint64_t kDwAtLowPc = 0x11;
constexpr std::uint64_t kDwAtHighPc = 0x12;
constexpr std::uint64_t kDwAtEncoding = 0x3e;
constexpr std::uint64_t kDwAtFrameBase = 0x40;
constexpr std::uint64_t kDwAtType = 0x49;

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

constexpr std::uint8_t kDwOpReg5 = 0x55;
constexpr std::uint8_t kDwOpReg6 = 0x56;
constexpr std::uint8_t kDwOpFbreg = 0x91;
constexpr std::uint8_t kDwOpCallFrameCfa = 0x9c;
constexpr std::uint64_t kDwAteSigned = 0x05;
constexpr std::uint64_t kDwAteUnsigned = 0x07;

struct DebugSections {
  std::vector<std::byte> info;
  std::vector<std::byte> abbrev;
  std::vector<std::byte> strings;
  std::vector<std::byte> locations;
};

struct AttributeSpec {
  std::uint64_t name;
  std::uint64_t form;
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
      path, {".debug_info", ".debug_abbrev", ".debug_str", ".debug_loc"});
  const auto info = sections.find(".debug_info");
  const auto abbrev = sections.find(".debug_abbrev");
  const auto strings = sections.find(".debug_str");
  if (info == sections.end() || abbrev == sections.end() || strings == sections.end()) {
    throw std::runtime_error("local-value inspection requires .debug_info, .debug_abbrev, and .debug_str");
  }
  const auto locations = sections.find(".debug_loc");
  return DebugSections{info->second, abbrev->second, strings->second,
                       locations == sections.end() ? std::vector<std::byte>{}
                                                   : locations->second};
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
      entry.attributes.push_back(AttributeSpec{name, form});
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
      const auto offset = read_scalar<std::uint32_t>(sections.info, cursor, unit_end, "DW_FORM_strp");
      if (offset >= sections.strings.size()) throw std::runtime_error("DW_FORM_strp offset is out of range");
      auto string_cursor = static_cast<std::size_t>(offset);
      value.text = read_c_string(sections.strings, string_cursor, sections.strings.size(),
                                 "DW_FORM_strp string");
      break;
    }
    case kDwFormRef4: {
      const auto offset = read_scalar<std::uint32_t>(sections.info, cursor, unit_end, "DW_FORM_ref4");
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
      if (length > unit_end - cursor) throw std::runtime_error("DW_FORM_exprloc extends past unit");
      const auto end = cursor + static_cast<std::size_t>(length);
      value.expression.assign(sections.info.begin() + cursor, sections.info.begin() + end);
      cursor = end;
      break;
    }
    case kDwFormFlagPresent:
      value.number = 1;
      break;
    default:
      throw std::runtime_error("unsupported DWARF form in local-value inspection: " +
                               std::to_string(spec.form));
  }
  return value;
}

const AttributeValue* attribute(const Die& die, std::uint64_t name) {
  const auto it = std::find_if(die.attributes.begin(), die.attributes.end(),
                               [name](const AttributeValue& value) { return value.name == name; });
  return it == die.attributes.end() ? nullptr : &*it;
}

std::optional<std::size_t> die_index_by_offset(const std::vector<Die>& dies,
                                               std::uint64_t offset) {
  for (std::size_t index = 0; index < dies.size(); ++index) {
    if (dies[index].offset == offset) return index;
  }
  return std::nullopt;
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

std::uint64_t add_unsigned(std::uint64_t base, std::uint64_t offset, const char* what) {
  if (offset > std::numeric_limits<std::uint64_t>::max() - base) {
    throw std::runtime_error(std::string(what) + " overflows address space");
  }
  return base + offset;
}

std::uint64_t add_signed(std::uint64_t base, std::int64_t offset, const char* what) {
  if (offset >= 0) return add_unsigned(base, static_cast<std::uint64_t>(offset), what);
  const auto magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1;
  if (magnitude > base) throw std::runtime_error(std::string(what) + " underflows address space");
  return base - magnitude;
}

std::vector<Die> parse_unit_dies(const DebugSections& sections, std::size_t unit_start,
                                 std::size_t& next_unit) {
  std::size_t cursor = unit_start;
  const auto unit_length = read_scalar<std::uint32_t>(sections.info, cursor, sections.info.size(),
                                                      "DWARF unit length");
  if (unit_length == 0xffffffffU) throw std::runtime_error("DWARF64 debug info is unsupported");
  if (unit_length > sections.info.size() - cursor) {
    throw std::runtime_error("DWARF compilation unit extends past .debug_info");
  }
  const auto unit_end = cursor + static_cast<std::size_t>(unit_length);
  next_unit = unit_end;
  const auto version = read_scalar<std::uint16_t>(sections.info, cursor, unit_end,
                                                  "DWARF unit version");
  if (version != 4) throw std::runtime_error("only DWARF4 local-value units are supported");
  const auto abbrev_offset = read_scalar<std::uint32_t>(sections.info, cursor, unit_end,
                                                        "DWARF abbreviation offset");
  const auto address_size = read_scalar<std::uint8_t>(sections.info, cursor, unit_end,
                                                      "DWARF address size");
  if (address_size != 8) throw std::runtime_error("only 8-byte DWARF4 addresses are supported");
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
  return dies;
}

bool subprogram_contains_pc(const Die& die, std::uint64_t pc) {
  const auto* low = attribute(die, kDwAtLowPc);
  const auto* high = attribute(die, kDwAtHighPc);
  if (low == nullptr || high == nullptr || low->form != kDwFormAddr) return false;
  const auto high_pc = high->form == kDwFormAddr
                           ? high->number
                           : add_unsigned(low->number, high->number, "DWARF high_pc");
  return pc >= low->number && pc < high_pc;
}

IntegerType resolve_integer_type(const std::vector<Die>& dies, std::uint64_t type_offset) {
  for (unsigned depth = 0; depth < 16; ++depth) {
    const auto index = die_index_by_offset(dies, type_offset);
    if (!index) throw std::runtime_error("local variable type references an unknown DIE");
    const auto& die = dies[*index];
    if (die.tag == kDwTagTypedef) {
      const auto* type = attribute(die, kDwAtType);
      if (type == nullptr || type->form != kDwFormRef4) {
        throw std::runtime_error("typedef does not use the supported DW_FORM_ref4 type link");
      }
      type_offset = type->number;
      continue;
    }
    if (die.tag != kDwTagBaseType) {
      throw std::runtime_error("local variable type is not a supported typedef/base-type chain");
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
  throw std::runtime_error("local variable typedef chain is too deep");
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

std::vector<std::byte> active_location_expression(const DebugSections& sections,
                                                  std::uint64_t offset,
                                                  std::uint64_t virtual_pc) {
  if (sections.locations.empty()) {
    throw std::runtime_error("formal parameter location list requires .debug_loc");
  }
  if (offset >= sections.locations.size()) {
    throw std::runtime_error("formal parameter location-list offset is out of range");
  }

  std::size_t cursor = static_cast<std::size_t>(offset);
  std::optional<std::uint64_t> base_address;
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
    const auto range_begin = base_address ? add_unsigned(*base_address, begin,
                                                         "location-list range begin")
                                          : begin;
    const auto range_end = base_address ? add_unsigned(*base_address, end,
                                                       "location-list range end")
                                        : end;
    if (range_end < range_begin) {
      throw std::runtime_error("DWARF4 location-list range is reversed");
    }
    if (virtual_pc >= range_begin && virtual_pc < range_end) {
      return std::vector<std::byte>(sections.locations.begin() + cursor,
                                    sections.locations.begin() + expression_end);
    }
    cursor = expression_end;
  }
  throw std::runtime_error("formal parameter has no location for the current PC");
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

std::uint64_t read_integer(const Debugger& debugger, std::uint64_t address,
                           std::size_t byte_size) {
  const auto bytes = debugger.read_memory(static_cast<std::uintptr_t>(address), byte_size);
  if (bytes.size() != byte_size) throw std::runtime_error("short local-variable memory read");
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < byte_size; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[index])) << (index * 8U);
  }
  return value;
}

std::uint64_t truncate_integer(std::uint64_t value, std::size_t byte_size) {
  if (byte_size == 8) return value;
  const auto bits = static_cast<unsigned>(byte_size * 8U);
  return value & ((std::uint64_t{1} << bits) - 1U);
}

std::optional<LocalIntegerValue> inspect_unit(const DebugSections& sections,
                                              const Debugger& debugger,
                                              const ElfFile& module,
                                              std::uint64_t virtual_pc,
                                              std::string_view name,
                                              std::size_t unit_start,
                                              std::size_t& next_unit) {
  const auto dies = parse_unit_dies(sections, unit_start, next_unit);
  std::optional<std::size_t> subprogram;
  for (std::size_t index = 0; index < dies.size(); ++index) {
    if (dies[index].tag != kDwTagSubprogram || !subprogram_contains_pc(dies[index], virtual_pc)) {
      continue;
    }
    if (subprogram) throw std::runtime_error("current PC matches multiple DWARF subprograms");
    subprogram = index;
  }
  if (!subprogram) return std::nullopt;

  std::optional<std::size_t> value_die_index;
  bool nested_name = false;
  for (std::size_t index = 0; index < dies.size(); ++index) {
    const bool supported_value_tag =
        dies[index].tag == kDwTagVariable || dies[index].tag == kDwTagFormalParameter;
    if (!supported_value_tag) continue;
    const auto* die_name = attribute(dies[index], kDwAtName);
    if (die_name == nullptr || die_name->text != name) continue;
    if (dies[index].parent == *subprogram) {
      if (value_die_index) {
        throw std::runtime_error("ambiguous local value in current subprogram: " +
                                 std::string(name));
      }
      value_die_index = index;
    } else if (dies[index].tag == kDwTagVariable &&
               is_descendant_of(dies, index, *subprogram)) {
      nested_name = true;
    }
  }
  if (!value_die_index) {
    if (nested_name) {
      throw std::runtime_error("nested lexical-scope locals are unsupported in this milestone");
    }
    throw std::runtime_error("local value is not in the current subprogram: " +
                             std::string(name));
  }

  const auto& subprogram_die = dies[*subprogram];
  const auto& value_die = dies[*value_die_index];
  const bool is_formal_parameter = value_die.tag == kDwTagFormalParameter;
  const auto* location = attribute(value_die, kDwAtLocation);
  const auto* type = attribute(value_die, kDwAtType);
  if (location == nullptr) {
    throw std::runtime_error("local value has no DW_AT_location");
  }
  if (type == nullptr || type->form != kDwFormRef4) {
    throw std::runtime_error("local value has no supported DW_FORM_ref4 type");
  }

  const auto integer_type = resolve_integer_type(dies, type->number);
  std::vector<std::byte> location_expression;
  if (location->form == kDwFormExprloc) {
    location_expression = location->expression;
  } else if (is_formal_parameter && location->form == kDwFormSecOffset) {
    location_expression = active_location_expression(sections, location->number, virtual_pc);
  } else if (location->form == kDwFormSecOffset) {
    throw std::runtime_error(
        "optimized local-variable location lists are outside this milestone");
  } else {
    throw std::runtime_error("local value has no supported DW_AT_location form");
  }

  if (location_expression.size() == 1 &&
      std::to_integer<std::uint8_t>(location_expression.front()) == kDwOpReg5) {
    const auto raw = truncate_integer(debugger.registers().rdi, integer_type.byte_size);
    return LocalIntegerValue{module.path(), std::string(name), raw,
                             integer_type.byte_size, integer_type.is_signed};
  }

  if (location_expression.empty() ||
      std::to_integer<std::uint8_t>(location_expression.front()) != kDwOpFbreg) {
    throw std::runtime_error("local value location is not a supported DW_OP_reg5/DW_OP_fbreg form");
  }
  const auto* base_expression = attribute(subprogram_die, kDwAtFrameBase);
  if (base_expression == nullptr || base_expression->form != kDwFormExprloc) {
    throw std::runtime_error("current subprogram has no supported DW_AT_frame_base expression");
  }
  const auto base = frame_base(debugger, module, base_expression->expression);
  const auto address = add_signed(base, fbreg_offset(location_expression),
                                  "local variable address");
  return LocalIntegerValue{module.path(), std::string(name),
                           read_integer(debugger, address, integer_type.byte_size),
                           integer_type.byte_size, integer_type.is_signed};
}

}  // namespace

LocalIntegerValue inspect_local_integer(const Debugger& debugger,
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
  throw std::runtime_error("current PC is not covered by a supported DWARF4 subprogram");
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

#include "dwarf/eh_frame.hpp"

#include "debugger/debugger.hpp"
#include "elf/elf.hpp"
#include "ptrace/ptrace.hpp"

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
namespace {

constexpr std::uint8_t kPcrelSdata4 = 0x1b;

struct Cie {
  std::uint64_t code_alignment{0};
  std::int64_t data_alignment{0};
  std::uint64_t return_register{0};
  std::uint8_t fde_encoding{0};
  std::size_t instructions_begin{0};
  std::size_t instructions_end{0};
};

enum class RuleKind { Undefined, SameValue, Offset };
struct RegisterRule {
  RuleKind kind{RuleKind::Undefined};
  std::int64_t offset{0};
};
struct CfiState {
  bool cfa_defined{false};
  std::uint64_t cfa_register{0};
  std::int64_t cfa_offset{0};
  std::map<std::uint64_t, RegisterRule> rules;
};

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
    throw std::runtime_error(std::string(what) + " extends past CFI entry boundary");
  }
  const auto value = read_at<T>(bytes, cursor, what);
  cursor += sizeof(T);
  return value;
}

std::uint64_t read_uleb(const std::vector<std::byte>& bytes, std::size_t& cursor,
                        std::size_t limit) {
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

std::int64_t read_sleb(const std::vector<std::byte>& bytes, std::size_t& cursor,
                       std::size_t limit) {
  std::uint64_t result = 0;
  unsigned shift = 0;
  std::uint8_t byte = 0;
  for (unsigned count = 0; count < 10; ++count) {
    byte = read_scalar<std::uint8_t>(bytes, cursor, limit, "SLEB128");
    result |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
    shift += 7;
    if ((byte & 0x80U) == 0) {
      if ((byte & 0x40U) != 0 && shift < 64) result |= (~std::uint64_t{0}) << shift;
      return static_cast<std::int64_t>(result);
    }
  }
  throw std::runtime_error("SLEB128 is too long");
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

std::optional<std::pair<std::uint64_t, std::vector<std::byte>>> find_eh_frame(
    const std::vector<std::byte>& bytes) {
  const auto header = read_at<Elf64_Ehdr>(bytes, 0, "ELF header");
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
      header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_X86_64) {
    throw std::runtime_error(".eh_frame parsing requires little-endian x86-64 ELF64");
  }
  if (header.e_shnum == 0 || header.e_shstrndx == SHN_UNDEF) return std::nullopt;
  if (header.e_shentsize != sizeof(Elf64_Shdr) || header.e_shstrndx == SHN_XINDEX ||
      header.e_shstrndx >= header.e_shnum) {
    throw std::runtime_error("unsupported ELF section layout for .eh_frame");
  }

  std::vector<Elf64_Shdr> sections;
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
    const auto name = read_c_string(bytes, name_cursor, names_end, "ELF section name");
    if (name != ".eh_frame") continue;
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

std::int64_t scaled_offset(std::int64_t alignment, std::uint64_t factor) {
  if (factor > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::runtime_error("CFI offset factor is too large");
  }
  const auto signed_factor = static_cast<std::int64_t>(factor);
  if (alignment < 0 && signed_factor != 0 &&
      alignment < std::numeric_limits<std::int64_t>::min() / signed_factor) {
    throw std::runtime_error("CFI offset overflows 64 bits");
  }
  if (alignment > 0 && signed_factor != 0 &&
      alignment > std::numeric_limits<std::int64_t>::max() / signed_factor) {
    throw std::runtime_error("CFI offset overflows 64 bits");
  }
  return alignment * signed_factor;
}

std::uint64_t add_signed(std::uint64_t base, std::int64_t offset, const char* what) {
  if (offset >= 0) {
    const auto value = static_cast<std::uint64_t>(offset);
    if (value > std::numeric_limits<std::uint64_t>::max() - base) {
      throw std::runtime_error(std::string(what) + " overflows address space");
    }
    return base + value;
  }
  const auto value =
      static_cast<std::uint64_t>(-(offset + 1)) + static_cast<std::uint64_t>(1);
  if (value > base) throw std::runtime_error(std::string(what) + " underflows address space");
  return base - value;
}

Cie parse_cie(const std::vector<std::byte>& bytes, std::size_t cursor, std::size_t end) {
  const auto version = read_scalar<std::uint8_t>(bytes, cursor, end, "CIE version");
  if (version != 1) throw std::runtime_error("only .eh_frame CIE version 1 is supported");
  if (read_c_string(bytes, cursor, end, "CIE augmentation") != "zR") {
    throw std::runtime_error("only zR .eh_frame augmentation is supported");
  }

  Cie cie;
  cie.code_alignment = read_uleb(bytes, cursor, end);
  cie.data_alignment = read_sleb(bytes, cursor, end);
  cie.return_register = read_scalar<std::uint8_t>(bytes, cursor, end, "CIE return register");
  const auto augmentation_size = read_uleb(bytes, cursor, end);
  if (augmentation_size != 1 || cursor >= end) {
    throw std::runtime_error("unsupported zR CIE augmentation data");
  }
  cie.fde_encoding = read_scalar<std::uint8_t>(bytes, cursor, end, "FDE encoding");
  if (cie.fde_encoding != kPcrelSdata4) {
    throw std::runtime_error("only pcrel/sdata4 FDE pointers are supported");
  }
  cie.instructions_begin = cursor;
  cie.instructions_end = end;
  return cie;
}

void restore_rule(CfiState& state, const CfiState* initial, std::uint64_t reg) {
  if (initial == nullptr) throw std::runtime_error("CIE uses an invalid restore opcode");
  const auto it = initial->rules.find(reg);
  if (it == initial->rules.end()) state.rules.erase(reg);
  else state.rules[reg] = it->second;
}

bool advance(std::uint64_t& location, std::uint64_t units, const Cie& cie,
             std::uint64_t target) {
  if (cie.code_alignment == 0 ||
      units > std::numeric_limits<std::uint64_t>::max() / cie.code_alignment) {
    throw std::runtime_error("invalid CFI location advance");
  }
  const auto delta = units * cie.code_alignment;
  if (delta > std::numeric_limits<std::uint64_t>::max() - location) {
    throw std::runtime_error("CFI location overflows address space");
  }
  const auto next = location + delta;
  if (next > target) return false;
  location = next;
  return true;
}

CfiState execute(const std::vector<std::byte>& bytes, std::size_t begin, std::size_t end,
                 const Cie& cie, CfiState state, const CfiState* initial,
                 std::uint64_t location, std::uint64_t target) {
  std::size_t cursor = begin;
  std::vector<CfiState> stack;
  while (cursor < end) {
    const auto opcode = read_scalar<std::uint8_t>(bytes, cursor, end, "CFI opcode");
    const auto primary = static_cast<std::uint8_t>(opcode & 0xc0U);
    if (primary == 0x40U) {
      if (!advance(location, opcode & 0x3fU, cie, target)) return state;
      continue;
    }
    if (primary == 0x80U) {
      state.rules[opcode & 0x3fU] =
          {RuleKind::Offset, scaled_offset(cie.data_alignment, read_uleb(bytes, cursor, end))};
      continue;
    }
    if (primary == 0xc0U) {
      restore_rule(state, initial, opcode & 0x3fU);
      continue;
    }

    switch (opcode) {
      case 0x00:
        break;
      case 0x02:
        if (!advance(location, read_scalar<std::uint8_t>(bytes, cursor, end, "advance_loc1"),
                     cie, target)) return state;
        break;
      case 0x03:
        if (!advance(location, read_scalar<std::uint16_t>(bytes, cursor, end, "advance_loc2"),
                     cie, target)) return state;
        break;
      case 0x04:
        if (!advance(location, read_scalar<std::uint32_t>(bytes, cursor, end, "advance_loc4"),
                     cie, target)) return state;
        break;
      case 0x05: {
        const auto reg = read_uleb(bytes, cursor, end);
        state.rules[reg] =
            {RuleKind::Offset, scaled_offset(cie.data_alignment, read_uleb(bytes, cursor, end))};
        break;
      }
      case 0x06:
        restore_rule(state, initial, read_uleb(bytes, cursor, end));
        break;
      case 0x07:
        state.rules[read_uleb(bytes, cursor, end)] = {RuleKind::Undefined, 0};
        break;
      case 0x08:
        state.rules[read_uleb(bytes, cursor, end)] = {RuleKind::SameValue, 0};
        break;
      case 0x0a:
        stack.push_back(state);
        break;
      case 0x0b:
        if (stack.empty()) throw std::runtime_error("unbalanced DW_CFA_restore_state");
        state = stack.back();
        stack.pop_back();
        break;
      case 0x0c: {
        state.cfa_register = read_uleb(bytes, cursor, end);
        const auto offset = read_uleb(bytes, cursor, end);
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          throw std::runtime_error("CFA offset is too large");
        }
        state.cfa_offset = static_cast<std::int64_t>(offset);
        state.cfa_defined = true;
        break;
      }
      case 0x0d:
        if (!state.cfa_defined) throw std::runtime_error("CFA register is undefined");
        state.cfa_register = read_uleb(bytes, cursor, end);
        break;
      case 0x0e: {
        if (!state.cfa_defined) throw std::runtime_error("CFA register is undefined");
        const auto offset = read_uleb(bytes, cursor, end);
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          throw std::runtime_error("CFA offset is too large");
        }
        state.cfa_offset = static_cast<std::int64_t>(offset);
        break;
      }
      case 0x2e:
        static_cast<void>(read_uleb(bytes, cursor, end));
        break;
      default:
        throw std::runtime_error("unsupported DWARF CFI opcode");
    }
  }
  return state;
}

std::uint64_t dwarf_register(const user_regs_struct& regs, std::uint64_t reg) {
  switch (reg) {
    case 0: return regs.rax;
    case 1: return regs.rdx;
    case 2: return regs.rcx;
    case 3: return regs.rbx;
    case 4: return regs.rsi;
    case 5: return regs.rdi;
    case 6: return regs.rbp;
    case 7: return regs.rsp;
    case 8: return regs.r8;
    case 9: return regs.r9;
    case 10: return regs.r10;
    case 11: return regs.r11;
    case 12: return regs.r12;
    case 13: return regs.r13;
    case 14: return regs.r14;
    case 15: return regs.r15;
    case 16: return regs.rip;
    default: throw std::runtime_error("unsupported x86-64 DWARF register");
  }
}

std::uint64_t read_u64(const std::vector<std::byte>& bytes) {
  if (bytes.size() != sizeof(std::uint64_t)) {
    throw std::runtime_error("unexpected CFI return-address read size");
  }
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[index])) << (index * 8U);
  }
  return value;
}

}  // namespace

EhFrame::EhFrame(std::string path) : path_(std::move(path)) {
  const auto section = find_eh_frame(read_file(path_));
  if (!section) return;
  section_virtual_address_ = section->first;
  section_ = section->second;
  available_ = true;
}

std::optional<std::uintptr_t> EhFrame::caller_return_address(
    const Debugger& debugger, const ElfFile& elf) const {
  if (!available_) return std::nullopt;
  if (debugger.state() != ProcessState::Stopped) {
    throw std::logic_error("CFI evaluation requires a stopped tracee");
  }

  const auto regs = debugger.registers();
  const auto bias = elf.load_bias(debugger.pid());
  if (regs.rip < bias) throw std::runtime_error("runtime RIP is below executable load bias");
  const auto target = static_cast<std::uint64_t>(regs.rip - bias);

  std::map<std::size_t, Cie> cies;
  std::size_t entry = 0;
  while (entry < section_.size()) {
    const auto length = read_at<std::uint32_t>(section_, entry, ".eh_frame length");
    if (length == 0) break;
    if (length == 0xffffffffU) throw std::runtime_error("DWARF64 .eh_frame is unsupported");

    const auto content = entry + sizeof(std::uint32_t);
    if (length > section_.size() - content) {
      throw std::runtime_error(".eh_frame entry extends past section boundary");
    }
    const auto end = content + static_cast<std::size_t>(length);
    std::size_t cursor = content;
    const auto cie_pointer = read_scalar<std::uint32_t>(section_, cursor, end, "CIE pointer");

    if (cie_pointer == 0) {
      cies.emplace(entry, parse_cie(section_, cursor, end));
      entry = end;
      continue;
    }
    if (cie_pointer > content) throw std::runtime_error("FDE CIE pointer underflows section");
    const auto cie_it = cies.find(content - static_cast<std::size_t>(cie_pointer));
    if (cie_it == cies.end()) throw std::runtime_error("FDE references an unknown CIE");
    const auto& cie = cie_it->second;

    const auto field_virtual_address = section_virtual_address_ + cursor;
    const auto relative = read_scalar<std::int32_t>(section_, cursor, end, "FDE initial location");
    const auto initial_location = add_signed(field_virtual_address, relative, "FDE start");
    const auto signed_range = read_scalar<std::int32_t>(section_, cursor, end, "FDE address range");
    if (signed_range < 0) throw std::runtime_error("negative FDE address range");
    const auto range = static_cast<std::uint64_t>(signed_range);
    if (range > std::numeric_limits<std::uint64_t>::max() - initial_location) {
      throw std::runtime_error("FDE address range overflows address space");
    }

    const auto augmentation_size = read_uleb(section_, cursor, end);
    if (augmentation_size > end - cursor) {
      throw std::runtime_error("FDE augmentation extends past entry boundary");
    }
    cursor += static_cast<std::size_t>(augmentation_size);

    if (target >= initial_location && target < initial_location + range) {
      const auto cie_state =
          execute(section_, cie.instructions_begin, cie.instructions_end, cie, {}, nullptr,
                  initial_location, std::numeric_limits<std::uint64_t>::max());
      const auto state =
          execute(section_, cursor, end, cie, cie_state, &cie_state, initial_location, target);
      if (!state.cfa_defined) throw std::runtime_error("CFI did not define a CFA");

      const auto cfa = add_signed(dwarf_register(regs, state.cfa_register), state.cfa_offset,
                                  "CFI CFA");
      const auto rule = state.rules.find(cie.return_register);
      if (rule == state.rules.end() || rule->second.kind != RuleKind::Offset) {
        throw std::runtime_error("CFI return-address rule is not a supported memory offset");
      }
      const auto slot = add_signed(cfa, rule->second.offset, "CFI return slot");
      try {
        const auto return_address =
            read_u64(debugger.read_memory(static_cast<std::uintptr_t>(slot), sizeof(std::uint64_t)));
        if (return_address == 0) throw std::runtime_error("CFI resolved a zero return address");
        return static_cast<std::uintptr_t>(return_address);
      } catch (const lowlevel::PtraceError&) {
        throw std::runtime_error("CFI return-address slot is unreadable");
      }
    }

    entry = end;
  }
  return std::nullopt;
}

}  // namespace mdbg

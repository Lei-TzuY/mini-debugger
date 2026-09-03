#include "elf/elf.hpp"

#include <elf.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace mdbg {
namespace {

template <typename T>
T read_struct(const std::vector<std::byte>& bytes, std::size_t offset) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    throw std::runtime_error("ELF structure extends past end of file");
  }
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

void require_range(const std::vector<std::byte>& bytes, std::uint64_t offset,
                   std::uint64_t size, const char* what) {
  if (offset > bytes.size() || size > bytes.size() - static_cast<std::size_t>(offset)) {
    throw std::runtime_error(std::string(what) + " extends past end of file");
  }
}

std::string read_string(const std::vector<std::byte>& bytes, std::uint64_t table_offset,
                        std::uint64_t table_size, std::uint32_t string_offset) {
  if (string_offset >= table_size) return {};
  const auto begin = table_offset + string_offset;
  const auto end = table_offset + table_size;
  std::string result;
  for (auto cursor = begin; cursor < end; ++cursor) {
    const char character = static_cast<char>(std::to_integer<unsigned char>(bytes[cursor]));
    if (character == '\0') return result;
    result.push_back(character);
  }
  return {};
}

std::string trim_left(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });
  value.erase(value.begin(), first);
  return value;
}

std::string mapped_identity_path(const std::string& path) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  return error ? path : canonical.string();
}

}  // namespace

ElfFile::ElfFile(std::string path) : path_(std::move(path)) { parse(); }

bool ElfFile::is_pie() const noexcept { return elf_type_ == ET_DYN; }

void ElfFile::parse() {
  std::ifstream input(path_, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open ELF file: " + path_);
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0) throw std::runtime_error("failed to determine ELF file size");
  input.seekg(0, std::ios::beg);
  bytes_.resize(static_cast<std::size_t>(length));
  if (!bytes_.empty()) {
    input.read(reinterpret_cast<char*>(bytes_.data()), length);
    if (!input) throw std::runtime_error("failed to read ELF file");
  }

  const auto header = read_struct<Elf64_Ehdr>(bytes_, 0);
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0) {
    throw std::runtime_error("file is not ELF");
  }
  if (header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB) {
    throw std::runtime_error("only little-endian ELF64 is supported");
  }
  if (header.e_machine != EM_X86_64) {
    throw std::runtime_error("only x86-64 ELF files are supported");
  }
  if (header.e_phentsize != sizeof(Elf64_Phdr) && header.e_phnum != 0) {
    throw std::runtime_error("unsupported ELF program-header entry size");
  }
  if (header.e_shentsize != sizeof(Elf64_Shdr) && header.e_shnum != 0) {
    throw std::runtime_error("unsupported ELF section-header entry size");
  }
  elf_type_ = header.e_type;

  zero_offset_load_vaddr_ = std::numeric_limits<std::uint64_t>::max();
  const auto page_size = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
  for (std::size_t index = 0; index < header.e_phnum; ++index) {
    const auto offset = static_cast<std::size_t>(header.e_phoff) + index * sizeof(Elf64_Phdr);
    const auto phdr = read_struct<Elf64_Phdr>(bytes_, offset);
    if (phdr.p_type != PT_LOAD) continue;
    if ((phdr.p_offset / page_size) != 0) continue;
    const auto aligned_vaddr = phdr.p_vaddr - (phdr.p_vaddr % page_size);
    zero_offset_load_vaddr_ = std::min(zero_offset_load_vaddr_, aligned_vaddr);
  }
  if (zero_offset_load_vaddr_ == std::numeric_limits<std::uint64_t>::max()) {
    zero_offset_load_vaddr_ = 0;
  }

  std::vector<Elf64_Shdr> sections;
  sections.reserve(header.e_shnum);
  for (std::size_t index = 0; index < header.e_shnum; ++index) {
    const auto offset = static_cast<std::size_t>(header.e_shoff) + index * sizeof(Elf64_Shdr);
    sections.push_back(read_struct<Elf64_Shdr>(bytes_, offset));
  }

  for (const auto& section : sections) {
    if (section.sh_type != SHT_SYMTAB && section.sh_type != SHT_DYNSYM) continue;
    if (section.sh_entsize != sizeof(Elf64_Sym) || section.sh_link >= sections.size()) continue;
    require_range(bytes_, section.sh_offset, section.sh_size, "ELF symbol table");
    const auto& strings = sections[section.sh_link];
    require_range(bytes_, strings.sh_offset, strings.sh_size, "ELF string table");

    const auto count = section.sh_size / sizeof(Elf64_Sym);
    for (std::size_t index = 0; index < count; ++index) {
      const auto offset = static_cast<std::size_t>(section.sh_offset) + index * sizeof(Elf64_Sym);
      const auto symbol = read_struct<Elf64_Sym>(bytes_, offset);
      if (symbol.st_name == 0 || symbol.st_shndx == SHN_UNDEF) continue;
      const auto type = static_cast<unsigned char>(ELF64_ST_TYPE(symbol.st_info));
      if (type == STT_SECTION || type == STT_FILE) continue;
      auto name = read_string(bytes_, strings.sh_offset, strings.sh_size, symbol.st_name);
      if (name.empty()) continue;
      symbols_.push_back({std::move(name), symbol.st_value, symbol.st_size, type,
                          static_cast<unsigned char>(ELF64_ST_BIND(symbol.st_info))});
    }
  }

  std::sort(symbols_.begin(), symbols_.end(), [](const ElfSymbol& left, const ElfSymbol& right) {
    if (left.value != right.value) return left.value < right.value;
    if (left.name != right.name) return left.name < right.name;
    return left.type < right.type;
  });
  symbols_.erase(std::unique(symbols_.begin(), symbols_.end(), [](const ElfSymbol& left,
                                                                  const ElfSymbol& right) {
                   return left.value == right.value && left.name == right.name;
                 }),
                 symbols_.end());
}

std::optional<ElfSymbol> ElfFile::find_symbol(std::string_view name) const {
  std::optional<ElfSymbol> best;
  for (const auto& symbol : symbols_) {
    if (symbol.name != name) continue;
    if (!best || (symbol.type == STT_FUNC && best->type != STT_FUNC) ||
        (symbol.binding == STB_GLOBAL && best->binding != STB_GLOBAL)) {
      best = symbol;
    }
  }
  return best;
}

std::optional<ResolvedSymbol> ElfFile::find_symbol_by_virtual_address(
    std::uint64_t address) const {
  const ElfSymbol* best = nullptr;
  for (const auto& symbol : symbols_) {
    if (symbol.value > address) break;
    if (symbol.type != STT_FUNC && symbol.type != STT_NOTYPE) continue;
    if (symbol.size != 0 && address >= symbol.value + symbol.size) continue;
    if (best == nullptr || symbol.value > best->value ||
        (symbol.value == best->value && symbol.type == STT_FUNC && best->type != STT_FUNC)) {
      best = &symbol;
    }
  }
  if (best == nullptr) return std::nullopt;
  return ResolvedSymbol{*best, address - best->value};
}

std::uint64_t ElfFile::load_bias(pid_t pid) const {
  if (elf_type_ == ET_EXEC) return 0;
  if (elf_type_ != ET_DYN) throw std::runtime_error("unsupported ELF executable type");

  const auto mapped_file = mapped_identity_path(path_);
  std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
  if (!maps) throw std::runtime_error("failed to open tracee memory map");

  std::string line;
  while (std::getline(maps, line)) {
    std::istringstream fields(line);
    std::string range, permissions, offset_text, device, inode;
    if (!(fields >> range >> permissions >> offset_text >> device >> inode)) continue;
    std::string mapped_path;
    std::getline(fields, mapped_path);
    mapped_path = trim_left(std::move(mapped_path));
    if (mapped_path != mapped_file) continue;

    const auto offset = std::stoull(offset_text, nullptr, 16);
    if (offset != 0) continue;
    const auto dash = range.find('-');
    if (dash == std::string::npos) continue;
    const auto start = std::stoull(range.substr(0, dash), nullptr, 16);
    if (start < zero_offset_load_vaddr_) {
      throw std::runtime_error("invalid ELF mapping below load virtual address");
    }
    return start - zero_offset_load_vaddr_;
  }
  throw std::runtime_error("could not locate ELF mapping for load bias: " + mapped_file);
}

std::uint64_t ElfFile::runtime_address(pid_t pid, const ElfSymbol& symbol) const {
  return symbol.value + load_bias(pid);
}

std::optional<ResolvedSymbol> ElfFile::find_symbol_by_runtime_address(
    pid_t pid, std::uint64_t address) const {
  const auto bias = load_bias(pid);
  if (address < bias) return std::nullopt;
  return find_symbol_by_virtual_address(address - bias);
}

}  // namespace mdbg

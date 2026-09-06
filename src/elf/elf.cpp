#include "elf/elf.hpp"

#include <elf.h>
#include <sys/procfs.h>
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

std::uint64_t checked_add(std::uint64_t left, std::uint64_t right, const char* what) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    throw std::runtime_error(std::string(what) + " overflows");
  }
  return left + right;
}

std::uint64_t checked_mul(std::uint64_t left, std::uint64_t right, const char* what) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    throw std::runtime_error(std::string(what) + " overflows");
  }
  return left * right;
}

std::uint64_t align4(std::uint64_t value, const char* what) {
  return checked_add(value, 3, what) & ~std::uint64_t{3};
}

std::uint64_t read_u64(const std::vector<std::byte>& bytes, std::uint64_t offset,
                       std::uint64_t limit, const char* what) {
  if (offset > limit || sizeof(std::uint64_t) > limit - offset) {
    throw std::runtime_error(std::string(what) + " extends past note payload");
  }
  require_range(bytes, offset, sizeof(std::uint64_t), what);
  std::uint64_t value = 0;
  std::memcpy(&value, bytes.data() + static_cast<std::size_t>(offset), sizeof(value));
  return value;
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

std::vector<std::byte> read_binary_file(const std::string& path) {
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

std::string note_owner(const std::vector<std::byte>& bytes, std::uint64_t offset,
                       std::uint32_t size) {
  require_range(bytes, offset, size, "ELF note name");
  std::string result;
  for (std::uint32_t index = 0; index < size; ++index) {
    const auto character = static_cast<char>(
        std::to_integer<unsigned char>(bytes[static_cast<std::size_t>(offset + index)]));
    if (character == '\0') break;
    result.push_back(character);
  }
  return result;
}

}  // namespace

ElfFile::ElfFile(std::string path) : path_(std::move(path)) { parse(); }

bool ElfFile::is_pie() const noexcept { return elf_type_ == ET_DYN; }

void ElfFile::parse() {
  bytes_ = read_binary_file(path_);

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

CoreSnapshot::CoreSnapshot(std::string path) : path_(std::move(path)) { parse(); }

void CoreSnapshot::parse() {
  bytes_ = read_binary_file(path_);
  const auto header = read_struct<Elf64_Ehdr>(bytes_, 0);
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0) {
    throw std::runtime_error("core file is not ELF");
  }
  if (header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
      header.e_ident[EI_VERSION] != EV_CURRENT || header.e_version != EV_CURRENT) {
    throw std::runtime_error("only current little-endian ELF64 core files are supported");
  }
  if (header.e_type != ET_CORE) throw std::runtime_error("ELF file is not ET_CORE");
  if (header.e_machine != EM_X86_64) {
    throw std::runtime_error("only x86-64 ELF core files are supported");
  }
  if (header.e_phnum == 0 || header.e_phnum == PN_XNUM ||
      header.e_phentsize != sizeof(Elf64_Phdr)) {
    throw std::runtime_error("unsupported ELF core program-header table");
  }
  const auto phdr_bytes = checked_mul(header.e_phnum, sizeof(Elf64_Phdr),
                                      "ELF core program-header table");
  require_range(bytes_, header.e_phoff, phdr_bytes, "ELF core program-header table");

  for (std::size_t index = 0; index < header.e_phnum; ++index) {
    const auto offset = static_cast<std::size_t>(header.e_phoff) + index * sizeof(Elf64_Phdr);
    const auto phdr = read_struct<Elf64_Phdr>(bytes_, offset);
    if (phdr.p_type == PT_LOAD) {
      if (phdr.p_memsz < phdr.p_filesz) {
        throw std::runtime_error("ELF core PT_LOAD file size exceeds memory size");
      }
      (void)checked_add(phdr.p_vaddr, phdr.p_memsz, "ELF core PT_LOAD address range");
      require_range(bytes_, phdr.p_offset, phdr.p_filesz, "ELF core PT_LOAD bytes");
      load_segments_.push_back(
          LoadSegment{phdr.p_vaddr, phdr.p_offset, phdr.p_filesz, phdr.p_memsz});
    } else if (phdr.p_type == PT_NOTE) {
      require_range(bytes_, phdr.p_offset, phdr.p_filesz, "ELF core PT_NOTE");
      parse_notes(phdr.p_offset, phdr.p_filesz);
    }
  }

  if (!has_registers_) throw std::runtime_error("ELF core lacks NT_PRSTATUS registers");
  if (load_segments_.empty()) throw std::runtime_error("ELF core lacks PT_LOAD memory");
  if (file_mappings_.empty()) throw std::runtime_error("ELF core lacks NT_FILE mappings");
  if (!mapping_for_address(static_cast<std::uintptr_t>(registers_.rip))) {
    throw std::runtime_error("ELF core crash RIP is not covered by NT_FILE mapping");
  }
}

void CoreSnapshot::parse_notes(std::uint64_t offset, std::uint64_t size) {
  const auto end = checked_add(offset, size, "ELF core PT_NOTE range");
  auto cursor = offset;
  while (cursor < end) {
    if (sizeof(Elf64_Nhdr) > end - cursor) {
      throw std::runtime_error("truncated ELF core note header");
    }
    const auto note = read_struct<Elf64_Nhdr>(bytes_, static_cast<std::size_t>(cursor));
    cursor += sizeof(Elf64_Nhdr);

    const auto name_padded = align4(note.n_namesz, "ELF core note name");
    if (name_padded > end - cursor) throw std::runtime_error("truncated ELF core note name");
    const auto owner = note_owner(bytes_, cursor, note.n_namesz);
    cursor += name_padded;

    const auto desc_offset = cursor;
    const auto desc_padded = align4(note.n_descsz, "ELF core note payload");
    if (desc_padded > end - cursor) throw std::runtime_error("truncated ELF core note payload");
    cursor += desc_padded;

    if (owner != "CORE") continue;
    if (note.n_type == NT_PRSTATUS) {
      if (note.n_descsz != sizeof(elf_prstatus)) {
        throw std::runtime_error("unsupported x86-64 NT_PRSTATUS size");
      }
      const auto status = read_struct<elf_prstatus>(bytes_, static_cast<std::size_t>(desc_offset));
      static_assert(sizeof(status.pr_reg) == sizeof(user_regs_struct));
      if (!has_registers_) {
        std::memcpy(&registers_, status.pr_reg, sizeof(registers_));
        crashed_tid_ = status.pr_pid;
        signal_number_ = status.pr_cursig != 0 ? status.pr_cursig : status.pr_info.si_signo;
        if (crashed_tid_ <= 0) throw std::runtime_error("NT_PRSTATUS has invalid TID");
        has_registers_ = true;
      }
      continue;
    }

    if (note.n_type != NT_FILE) continue;
    if (!file_mappings_.empty()) throw std::runtime_error("duplicate NT_FILE note");
    if (note.n_descsz < 2 * sizeof(std::uint64_t)) {
      throw std::runtime_error("truncated NT_FILE header");
    }
    const auto desc_end = checked_add(desc_offset, note.n_descsz, "NT_FILE payload");
    const auto count = read_u64(bytes_, desc_offset, desc_end, "NT_FILE mapping count");
    const auto page_size =
        read_u64(bytes_, desc_offset + sizeof(std::uint64_t), desc_end, "NT_FILE page size");
    if (page_size == 0) throw std::runtime_error("NT_FILE page size is zero");
    const auto triples_size = checked_mul(count, 3 * sizeof(std::uint64_t),
                                          "NT_FILE mapping table");
    const auto triples_offset = desc_offset + 2 * sizeof(std::uint64_t);
    const auto strings_offset = checked_add(triples_offset, triples_size,
                                            "NT_FILE mapping table");
    if (strings_offset > desc_end) throw std::runtime_error("truncated NT_FILE mapping table");

    struct RawMapping {
      std::uint64_t start;
      std::uint64_t end;
      std::uint64_t page_offset;
    };
    std::vector<RawMapping> raw;
    raw.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
      const auto entry = triples_offset + index * 3 * sizeof(std::uint64_t);
      const auto start = read_u64(bytes_, entry, desc_end, "NT_FILE mapping start");
      const auto mapping_end =
          read_u64(bytes_, entry + sizeof(std::uint64_t), desc_end, "NT_FILE mapping end");
      const auto page_offset = read_u64(bytes_, entry + 2 * sizeof(std::uint64_t), desc_end,
                                        "NT_FILE mapping file offset");
      if (mapping_end <= start) throw std::runtime_error("invalid NT_FILE mapping range");
      raw.push_back(RawMapping{start, mapping_end, page_offset});
    }

    auto string_cursor = strings_offset;
    for (const auto& mapping : raw) {
      if (string_cursor >= desc_end) throw std::runtime_error("missing NT_FILE path");
      std::string path;
      bool terminated = false;
      while (string_cursor < desc_end) {
        const auto character = static_cast<char>(std::to_integer<unsigned char>(
            bytes_[static_cast<std::size_t>(string_cursor++)]));
        if (character == '\0') {
          terminated = true;
          break;
        }
        path.push_back(character);
      }
      if (!terminated || path.empty()) throw std::runtime_error("invalid NT_FILE path");
      const auto file_offset =
          checked_mul(mapping.page_offset, page_size, "NT_FILE file offset");
      file_mappings_.push_back(
          CoreFileMapping{mapping.start, mapping.end, file_offset, std::move(path)});
    }
  }

  std::sort(file_mappings_.begin(), file_mappings_.end(),
            [](const CoreFileMapping& left, const CoreFileMapping& right) {
              if (left.start != right.start) return left.start < right.start;
              return left.end < right.end;
            });
}

std::vector<std::byte> CoreSnapshot::read_memory(std::uintptr_t address,
                                                 std::size_t length) const {
  if (length == 0) return {};
  const auto start = static_cast<std::uint64_t>(address);
  const auto end = checked_add(start, static_cast<std::uint64_t>(length),
                               "core memory read range");
  for (const auto& segment : load_segments_) {
    const auto memory_end = checked_add(segment.virtual_address, segment.memory_size,
                                        "core PT_LOAD memory range");
    if (start < segment.virtual_address || start >= memory_end) continue;
    if (end > memory_end) throw std::runtime_error("core memory read spans PT_LOAD boundary");
    const auto captured_end = checked_add(segment.virtual_address, segment.file_size,
                                          "core PT_LOAD captured range");
    if (end > captured_end) {
      throw std::runtime_error("core memory range is not captured in PT_LOAD file bytes");
    }
    const auto delta = start - segment.virtual_address;
    const auto file_offset = checked_add(segment.file_offset, delta, "core memory file offset");
    require_range(bytes_, file_offset, length, "core memory bytes");
    std::vector<std::byte> result(length);
    std::copy_n(bytes_.data() + static_cast<std::size_t>(file_offset), length, result.data());
    return result;
  }
  throw std::runtime_error("core memory range is unmapped");
}

std::optional<CoreFileMapping> CoreSnapshot::mapping_for_address(
    std::uintptr_t address) const {
  const auto value = static_cast<std::uint64_t>(address);
  for (const auto& mapping : file_mappings_) {
    if (mapping.start > value) break;
    if (value >= mapping.start && value < mapping.end) return mapping;
  }
  return std::nullopt;
}

}  // namespace mdbg

#pragma once

#include <elf.h>
#include <sys/procfs.h>
#include <sys/types.h>
#include <sys/user.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace mdbg {

struct CoreFloatingPointState {
  std::uint32_t mxcsr;
  std::uint32_t mxcsr_mask;
  std::array<std::array<std::byte, 16>, 16> xmm;
};

namespace detail {

inline std::size_t core_align4(std::size_t value) {
  if (value > static_cast<std::size_t>(-1) - 3U) {
    throw std::runtime_error("ELF core note alignment overflows");
  }
  return (value + 3U) & ~std::size_t{3U};
}

inline std::vector<std::byte> read_core_bytes(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open ELF core file: " + path);
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0) throw std::runtime_error("failed to determine ELF core file size");
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(length));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input) throw std::runtime_error("failed to read ELF core file");
  }
  return bytes;
}

template <typename T>
inline T core_read_struct(const std::vector<std::byte>& bytes, std::size_t offset,
                          const char* what) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    throw std::runtime_error(std::string(what) + " extends past end of core file");
  }
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

inline std::string core_note_owner(const std::vector<std::byte>& bytes,
                                   std::size_t offset, std::size_t size) {
  if (offset > bytes.size() || size > bytes.size() - offset) {
    throw std::runtime_error("ELF core note owner extends past end of file");
  }
  std::string owner;
  for (std::size_t index = 0; index < size; ++index) {
    const auto value = static_cast<char>(std::to_integer<unsigned char>(bytes[offset + index]));
    if (value == '\0') break;
    owner.push_back(value);
  }
  return owner;
}

inline CoreFloatingPointState decode_core_fpregset(const user_fpregs_struct& registers) {
  static_assert(sizeof(user_fpregs_struct) == 512,
                "Phase 16 evidence is specific to Linux x86-64 512-byte FPREGSET");
  static_assert(sizeof(registers.xmm_space) == 16U * 16U,
                "unexpected x86-64 XMM save area size");
  CoreFloatingPointState result{};
  result.mxcsr = registers.mxcsr;
  result.mxcsr_mask = registers.mxcr_mask;
  std::memcpy(result.xmm.data(), registers.xmm_space, sizeof(registers.xmm_space));
  return result;
}

}  // namespace detail

inline std::optional<CoreFloatingPointState> read_core_floating_point_state(
    const std::string& path, pid_t target_tid) {
  if (target_tid <= 0) throw std::invalid_argument("core floating-point TID must be positive");
  const auto bytes = detail::read_core_bytes(path);
  if (bytes.size() < sizeof(Elf64_Ehdr)) throw std::runtime_error("core is smaller than ELF header");
  const auto header = detail::core_read_struct<Elf64_Ehdr>(bytes, 0, "ELF core header");
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
      header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
      header.e_type != ET_CORE || header.e_machine != EM_X86_64) {
    throw std::runtime_error("floating-point state requires little-endian x86-64 ET_CORE");
  }
  if (header.e_phnum == 0 || header.e_phnum == PN_XNUM ||
      header.e_phentsize != sizeof(Elf64_Phdr)) {
    throw std::runtime_error("unsupported ELF core program-header table");
  }

  std::optional<pid_t> current_tid;
  std::optional<CoreFloatingPointState> target_state;
  std::set<pid_t> fpregset_tids;
  for (std::size_t index = 0; index < header.e_phnum; ++index) {
    const auto phoff = static_cast<std::size_t>(header.e_phoff) + index * sizeof(Elf64_Phdr);
    const auto phdr = detail::core_read_struct<Elf64_Phdr>(bytes, phoff, "ELF core program header");
    if (phdr.p_type != PT_NOTE) continue;
    if (phdr.p_offset > bytes.size() || phdr.p_filesz > bytes.size() - phdr.p_offset) {
      throw std::runtime_error("ELF core PT_NOTE extends past end of file");
    }
    auto cursor = static_cast<std::size_t>(phdr.p_offset);
    const auto end = cursor + static_cast<std::size_t>(phdr.p_filesz);
    while (cursor < end) {
      if (sizeof(Elf64_Nhdr) > end - cursor) {
        throw std::runtime_error("truncated ELF core note header");
      }
      const auto note = detail::core_read_struct<Elf64_Nhdr>(bytes, cursor, "ELF core note");
      cursor += sizeof(Elf64_Nhdr);
      const auto name_size = detail::core_align4(note.n_namesz);
      if (name_size > end - cursor) throw std::runtime_error("truncated ELF core note name");
      const auto owner = detail::core_note_owner(bytes, cursor, note.n_namesz);
      cursor += name_size;
      const auto desc_offset = cursor;
      const auto desc_size = detail::core_align4(note.n_descsz);
      if (desc_size > end - cursor) throw std::runtime_error("truncated ELF core note payload");
      cursor += desc_size;
      if (owner != "CORE") continue;

      if (note.n_type == NT_PRSTATUS) {
        if (note.n_descsz != sizeof(elf_prstatus)) {
          throw std::runtime_error("unsupported x86-64 NT_PRSTATUS size while associating FPREGSET");
        }
        const auto status = detail::core_read_struct<elf_prstatus>(
            bytes, desc_offset, "NT_PRSTATUS while associating FPREGSET");
        if (status.pr_pid <= 0) throw std::runtime_error("NT_PRSTATUS has invalid TID");
        current_tid = status.pr_pid;
        continue;
      }

      if (note.n_type != NT_FPREGSET) continue;
      if (!current_tid) {
        throw std::runtime_error("NT_FPREGSET appeared before any NT_PRSTATUS thread context");
      }
      if (note.n_descsz != sizeof(user_fpregs_struct)) {
        throw std::runtime_error("unsupported x86-64 NT_FPREGSET size");
      }
      if (!fpregset_tids.insert(*current_tid).second) {
        throw std::runtime_error("duplicate NT_FPREGSET for one core thread");
      }
      const auto registers = detail::core_read_struct<user_fpregs_struct>(
          bytes, desc_offset, "NT_FPREGSET");
      if (*current_tid == target_tid) {
        target_state = detail::decode_core_fpregset(registers);
      }
    }
  }
  return target_state;
}

}  // namespace mdbg

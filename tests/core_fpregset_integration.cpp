#include "elf/elf.hpp"

#include <elf.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

using XmmValue = std::array<std::byte, 16>;

constexpr XmmValue kCrashXmm15{
    std::byte{0xef}, std::byte{0xcd}, std::byte{0xab}, std::byte{0x89},
    std::byte{0x67}, std::byte{0x45}, std::byte{0x23}, std::byte{0x01},
    std::byte{0x10}, std::byte{0x32}, std::byte{0x54}, std::byte{0x76},
    std::byte{0x98}, std::byte{0xba}, std::byte{0xdc}, std::byte{0xfe}};
constexpr XmmValue kSiblingXmm15{
    std::byte{0x78}, std::byte{0x69}, std::byte{0x5a}, std::byte{0x4b},
    std::byte{0x3c}, std::byte{0x2d}, std::byte{0x1e}, std::byte{0x0f},
    std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
    std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88}};

std::size_t align4(std::size_t value) {
  if (value > static_cast<std::size_t>(-1) - 3U) {
    throw std::runtime_error("core note alignment overflow");
  }
  return (value + 3U) & ~std::size_t{3U};
}

std::vector<std::byte> read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open genuine core");
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0) throw std::runtime_error("failed to determine core size");
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(length));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input) throw std::runtime_error("failed to read genuine core");
  }
  return bytes;
}

std::string write_variant(const std::vector<std::byte>& bytes) {
  char pattern[] = "/tmp/mdbg-fpregset-variant-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd == -1) throw std::runtime_error("mkstemp failed for FPREGSET variant");
  ::close(fd);
  std::ofstream output(pattern, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("failed to write FPREGSET variant");
  return pattern;
}

struct NoteOffsets {
  std::size_t first_prstatus_header;
  std::size_t first_fpregset_header;
};

std::string note_owner(const std::vector<std::byte>& bytes, std::size_t offset,
                       std::size_t size) {
  require(offset <= bytes.size() && size <= bytes.size() - offset,
          "core note owner extends past file");
  std::string owner;
  for (std::size_t index = 0; index < size; ++index) {
    const char value =
        static_cast<char>(std::to_integer<unsigned char>(bytes[offset + index]));
    if (value == '\0') break;
    owner.push_back(value);
  }
  return owner;
}

NoteOffsets find_first_thread_notes(const std::vector<std::byte>& bytes) {
  require(bytes.size() >= sizeof(Elf64_Ehdr), "core is smaller than ELF header");
  Elf64_Ehdr ehdr{};
  std::memcpy(&ehdr, bytes.data(), sizeof(ehdr));
  require(ehdr.e_phentsize == sizeof(Elf64_Phdr),
          "core uses unexpected program-header size");

  std::size_t prstatus = static_cast<std::size_t>(-1);
  std::size_t fpregset = static_cast<std::size_t>(-1);
  for (std::size_t index = 0; index < ehdr.e_phnum; ++index) {
    const auto phoff = static_cast<std::size_t>(ehdr.e_phoff) + index * sizeof(Elf64_Phdr);
    require(phoff <= bytes.size() && sizeof(Elf64_Phdr) <= bytes.size() - phoff,
            "core program header extends past file");
    Elf64_Phdr phdr{};
    std::memcpy(&phdr, bytes.data() + phoff, sizeof(phdr));
    if (phdr.p_type != PT_NOTE) continue;
    const auto begin = static_cast<std::size_t>(phdr.p_offset);
    const auto size = static_cast<std::size_t>(phdr.p_filesz);
    require(begin <= bytes.size() && size <= bytes.size() - begin,
            "core PT_NOTE extends past file");
    auto cursor = begin;
    const auto end = begin + size;
    while (cursor < end) {
      require(sizeof(Elf64_Nhdr) <= end - cursor, "truncated note header");
      const auto header_offset = cursor;
      Elf64_Nhdr note{};
      std::memcpy(&note, bytes.data() + cursor, sizeof(note));
      cursor += sizeof(note);
      const auto name_size = align4(note.n_namesz);
      require(name_size <= end - cursor, "truncated note name");
      const auto owner = note_owner(bytes, cursor, note.n_namesz);
      cursor += name_size;
      const auto desc_size = align4(note.n_descsz);
      require(desc_size <= end - cursor, "truncated note descriptor");
      cursor += desc_size;
      if (owner != "CORE") continue;
      if (note.n_type == NT_PRSTATUS && prstatus == static_cast<std::size_t>(-1)) {
        prstatus = header_offset;
      }
      if (note.n_type == NT_FPREGSET && fpregset == static_cast<std::size_t>(-1)) {
        fpregset = header_offset;
      }
    }
  }
  require(prstatus != static_cast<std::size_t>(-1),
          "genuine core lacks first NT_PRSTATUS note");
  require(fpregset != static_cast<std::size_t>(-1),
          "genuine core lacks first NT_FPREGSET note");
  return NoteOffsets{prstatus, fpregset};
}

void expect_fpregset_failure(const std::vector<std::byte>& bytes, pid_t tid,
                             const std::string& context) {
  const auto path = write_variant(bytes);
  bool failed = false;
  try {
    (void)mdbg::read_core_floating_point_state(path, tid);
  } catch (const std::exception&) {
    failed = true;
  }
  std::remove(path.c_str());
  require(failed, context + " was accepted");
}

void test_fail_closed_variants(const std::string& core_path, pid_t crash_tid) {
  const auto original = read_file(core_path);
  const auto notes = find_first_thread_notes(original);

  auto wrong_size = original;
  Elf64_Nhdr fp_header{};
  std::memcpy(&fp_header, wrong_size.data() + notes.first_fpregset_header,
              sizeof(fp_header));
  require(fp_header.n_type == NT_FPREGSET && fp_header.n_descsz == 512,
          "genuine FPREGSET evidence changed unexpectedly");
  fp_header.n_descsz = 511;
  std::memcpy(wrong_size.data() + notes.first_fpregset_header, &fp_header,
              sizeof(fp_header));
  expect_fpregset_failure(wrong_size, crash_tid,
                          "non-512-byte x86-64 NT_FPREGSET descriptor");

  auto orphan = original;
  Elf64_Nhdr pr_header{};
  std::memcpy(&pr_header, orphan.data() + notes.first_prstatus_header,
              sizeof(pr_header));
  require(pr_header.n_type == NT_PRSTATUS,
          "genuine first thread note is not NT_PRSTATUS");
  pr_header.n_type = 0x7fffffffU;
  std::memcpy(orphan.data() + notes.first_prstatus_header, &pr_header,
              sizeof(pr_header));
  expect_fpregset_failure(orphan, crash_tid,
                          "NT_FPREGSET without preceding NT_PRSTATUS context");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_fpregset_integration <core> <sibling-tid>\n";
    return 2;
  }
  try {
    const auto sibling_tid = static_cast<pid_t>(std::stol(argv[2]));
    const mdbg::CoreSnapshot snapshot(argv[1]);
    const auto crash = snapshot.floating_point_state(snapshot.crashed_tid());
    const auto sibling = snapshot.floating_point_state(sibling_tid);
    require(crash.has_value(), "crashed thread lost its genuine NT_FPREGSET state");
    require(sibling.has_value(), "sibling thread lost its genuine NT_FPREGSET state");
    require(crash->xmm[15] == kCrashXmm15,
            "crashed thread XMM15 did not match the seeded kernel-core evidence");
    require(sibling->xmm[15] == kSiblingXmm15,
            "sibling thread XMM15 did not match the seeded kernel-core evidence");
    require(crash->xmm[15] != sibling->xmm[15],
            "per-thread FPREGSET association collapsed distinct SSE state");
    test_fail_closed_variants(argv[1], snapshot.crashed_tid());
    std::cout << "core FPREGSET integration passed; crash mxcsr=0x" << std::hex
              << crash->mxcsr << " sibling mxcsr=0x" << sibling->mxcsr << std::dec << '\n';
  } catch (const std::exception& error) {
    std::cerr << "core FPREGSET integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

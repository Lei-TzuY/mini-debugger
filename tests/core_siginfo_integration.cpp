#include "elf/elf.hpp"
#include "snapshot/session.hpp"

#include <elf.h>
#include <signal.h>
#include <sys/procfs.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<std::byte> read_file_bytes(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open core file: " + path);
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0) throw std::runtime_error("failed to determine core file size");
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(length));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input) throw std::runtime_error("failed to read core file");
  }
  return bytes;
}

std::string write_variant(const std::vector<std::byte>& bytes) {
  char pattern[] = "/tmp/mdbg-siginfo-variant-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd == -1) throw std::runtime_error("mkstemp failed for core variant");
  ::close(fd);
  std::ofstream output(pattern, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("failed to write core variant");
  return pattern;
}

std::size_t align4(std::size_t value) {
  if (value > static_cast<std::size_t>(-1) - 3) {
    throw std::runtime_error("core note alignment overflow");
  }
  return (value + 3U) & ~std::size_t{3U};
}

struct NoteRef {
  std::size_t header_offset;
  std::size_t desc_offset;
  Elf64_Nhdr header;
};

std::string note_owner(const std::vector<std::byte>& bytes, std::size_t offset,
                       std::size_t size) {
  require(offset <= bytes.size() && size <= bytes.size() - offset,
          "core note owner extends past file");
  std::string owner;
  for (std::size_t index = 0; index < size; ++index) {
    const char value = static_cast<char>(
        std::to_integer<unsigned char>(bytes[offset + index]));
    if (value == '\0') break;
    owner.push_back(value);
  }
  return owner;
}

NoteRef find_core_note(const std::vector<std::byte>& bytes, std::uint32_t type,
                       const char* label) {
  require(bytes.size() >= sizeof(Elf64_Ehdr), "core is smaller than ELF header");
  Elf64_Ehdr ehdr{};
  std::memcpy(&ehdr, bytes.data(), sizeof(ehdr));
  require(ehdr.e_phentsize == sizeof(Elf64_Phdr),
          "core uses unsupported program-header size in test");

  for (std::size_t index = 0; index < ehdr.e_phnum; ++index) {
    const auto phoff = static_cast<std::size_t>(ehdr.e_phoff) + index * sizeof(Elf64_Phdr);
    require(phoff <= bytes.size() && sizeof(Elf64_Phdr) <= bytes.size() - phoff,
            "core program header extends past file");
    Elf64_Phdr phdr{};
    std::memcpy(&phdr, bytes.data() + phoff, sizeof(phdr));
    if (phdr.p_type != PT_NOTE) continue;

    const auto note_begin = static_cast<std::size_t>(phdr.p_offset);
    const auto note_size = static_cast<std::size_t>(phdr.p_filesz);
    require(note_begin <= bytes.size() && note_size <= bytes.size() - note_begin,
            "core PT_NOTE extends past file");
    const auto note_end = note_begin + note_size;
    auto cursor = note_begin;
    while (cursor < note_end) {
      require(sizeof(Elf64_Nhdr) <= note_end - cursor,
              "truncated core note header in test");
      const auto header_offset = cursor;
      Elf64_Nhdr note{};
      std::memcpy(&note, bytes.data() + cursor, sizeof(note));
      cursor += sizeof(note);
      const auto name_size = align4(note.n_namesz);
      require(name_size <= note_end - cursor, "truncated core note name in test");
      const auto owner = note_owner(bytes, cursor, note.n_namesz);
      cursor += name_size;
      const auto desc_offset = cursor;
      const auto desc_size = align4(note.n_descsz);
      require(desc_size <= note_end - cursor, "truncated core note payload in test");
      cursor += desc_size;
      if (owner == "CORE" && note.n_type == type) {
        return NoteRef{header_offset, desc_offset, note};
      }
    }
  }
  throw std::runtime_error(std::string("genuine Linux core did not contain ") + label);
}

std::string bounded_text(const char* data, std::size_t size) {
  std::size_t length = 0;
  while (length < size && data[length] != '\0') ++length;
  std::string result(data, length);
  while (!result.empty() && result.back() == ' ') result.pop_back();
  return result;
}

struct AuxvEvidence {
  std::uint64_t entry_point;
  std::uint64_t program_headers;
  std::uint64_t program_header_count;
  std::uint64_t page_size;
  std::optional<std::uint64_t> interpreter_base;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
  std::size_t entry_offset;
  std::size_t phdr_offset;
  std::optional<std::size_t> base_offset;
  std::size_t null_offset;
};

AuxvEvidence parse_auxv_evidence(const std::vector<std::byte>& bytes,
                                 const NoteRef& note) {
  constexpr std::size_t kMaxAuxvBytes = 4096;
  require(note.header.n_descsz != 0 && note.header.n_descsz <= kMaxAuxvBytes,
          "genuine NT_AUXV descriptor is outside the bounded evidence window");
  require(note.header.n_descsz % sizeof(Elf64_auxv_t) == 0,
          "genuine NT_AUXV is not a native x86-64 auxiliary-vector array");
  require(note.desc_offset <= bytes.size() &&
              note.header.n_descsz <= bytes.size() - note.desc_offset,
          "genuine NT_AUXV descriptor extends past the core file");

  std::optional<std::uint64_t> entry;
  std::optional<std::uint64_t> phdr;
  std::optional<std::uint64_t> phnum;
  std::optional<std::uint64_t> page_size;
  std::optional<std::uint64_t> base;
  std::optional<std::size_t> entry_offset;
  std::optional<std::size_t> phdr_offset;
  std::optional<std::size_t> base_offset;
  std::optional<std::size_t> null_offset;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
  const auto count = note.header.n_descsz / sizeof(Elf64_auxv_t);
  for (std::size_t index = 0; index < count; ++index) {
    const auto offset = note.desc_offset + index * sizeof(Elf64_auxv_t);
    Elf64_auxv_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    if (value.a_type == AT_NULL) {
      null_offset = offset;
      break;
    }
    entries.emplace_back(value.a_type, value.a_un.a_val);
    auto assign_once = [&](std::optional<std::uint64_t>& target, const char* label) {
      require(!target.has_value(), std::string("genuine NT_AUXV duplicated ") + label);
      target = value.a_un.a_val;
    };
    switch (value.a_type) {
      case AT_ENTRY:
        assign_once(entry, "AT_ENTRY");
        entry_offset = offset;
        break;
      case AT_PHDR:
        assign_once(phdr, "AT_PHDR");
        phdr_offset = offset;
        break;
      case AT_PHNUM:
        assign_once(phnum, "AT_PHNUM");
        break;
      case AT_PAGESZ:
        assign_once(page_size, "AT_PAGESZ");
        break;
      case AT_BASE:
        assign_once(base, "AT_BASE");
        base_offset = offset;
        break;
      default:
        break;
    }
  }

  require(null_offset.has_value(), "genuine NT_AUXV did not terminate with AT_NULL");
  require(entry.has_value() && *entry != 0 && entry_offset.has_value(),
          "genuine NT_AUXV did not provide a nonzero AT_ENTRY");
  require(phdr.has_value() && *phdr != 0 && phdr_offset.has_value(),
          "genuine NT_AUXV did not provide a nonzero AT_PHDR");
  require(phnum.has_value() && *phnum != 0,
          "genuine NT_AUXV did not provide a nonzero AT_PHNUM");
  require(page_size.has_value() && *page_size != 0 &&
              (*page_size & (*page_size - 1)) == 0,
          "genuine NT_AUXV did not provide a power-of-two AT_PAGESZ");
  return AuxvEvidence{*entry, *phdr, *phnum, *page_size, base, std::move(entries),
                      *entry_offset, *phdr_offset, base_offset, *null_offset};
}

std::string generate_core(const std::string& fixture, pid_t* recorded_pid = nullptr) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for siginfo core fixture");
  if (recorded_pid != nullptr) *recorded_pid = child;
  if (child == 0) {
    rlimit core_limit{};
    if (::getrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(120);
    core_limit.rlim_cur = core_limit.rlim_max;
    if (::setrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(121);
    ::execl(fixture.c_str(), fixture.c_str(), "--snapshot-crash", nullptr);
    _exit(127);
  }

  const auto core_path = "/tmp/mdbg-core-" + std::to_string(child);
  std::remove(core_path.c_str());
  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  require(waited == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
          "siginfo fixture did not terminate from deterministic SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce the siginfo core file");
  return core_path;
}

void expect_core_failure(const std::vector<std::byte>& bytes,
                         const std::string& context) {
  const auto path = write_variant(bytes);
  bool failed = false;
  try {
    const mdbg::CoreSnapshot snapshot(path);
    (void)snapshot;
  } catch (const std::exception&) {
    failed = true;
  }
  std::remove(path.c_str());
  require(failed, context + " was accepted");
}

void expect_startup_failure(const std::vector<std::byte>& bytes,
                            const std::string& context) {
  const auto path = write_variant(bytes);
  bool failed = false;
  try {
    const mdbg::CoreInspectionSession session(path);
    (void)session.startup_info();
  } catch (const std::exception&) {
    failed = true;
  }
  std::remove(path.c_str());
  require(failed, context + " was accepted");
}

std::string run_core_cli(const std::string& cli, const std::string& core_path) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create core CLI pipes");
  }
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for core CLI");
  if (child == 0) {
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    ::execl(cli.c_str(), cli.c_str(), core_path.c_str(), nullptr);
    _exit(127);
  }
  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  const std::string script = "crash\nprocess\nstartup\nquit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset,
                               script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write core CLI commands");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read core CLI output");
    if (count == 0) break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(output_pipe[0]);

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "mdbg-core crash/process/startup commands failed");
  return output;
}

void test_real_siginfo(const std::string& fixture, const std::string& cli) {
  pid_t recorded_pid = -1;
  const auto core_path = generate_core(fixture, &recorded_pid);
  try {
    const mdbg::CoreSnapshot snapshot(core_path);
    const auto& crash = snapshot.crash_info();
    require(crash.has_value(), "genuine core lost NT_SIGINFO crash metadata");
    require(crash->signal_number == SIGSEGV,
            "NT_SIGINFO did not preserve SIGSEGV identity");
    require(crash->signal_number == snapshot.signal_number(),
            "NT_SIGINFO and crashed NT_PRSTATUS disagree on signal identity");
    require(crash->signal_code == SEGV_MAPERR,
            "deterministic null write did not record SEGV_MAPERR");
    require(crash->fault_address.has_value() && *crash->fault_address == 0,
            "deterministic null write did not record si_addr=0");

    const auto original = read_file_bytes(core_path);
    const auto note = find_core_note(original, NT_SIGINFO, "CORE/NT_SIGINFO");
    require(note.header.n_descsz == sizeof(siginfo_t),
            "genuine x86-64 Linux NT_SIGINFO size is outside the bounded contract");
    const auto auxv_note = find_core_note(original, NT_AUXV, "CORE/NT_AUXV");
    const auto auxv = parse_auxv_evidence(original, auxv_note);

    const auto entry_mapping = snapshot.mapping_for_address(auxv.entry_point);
    require(entry_mapping.has_value(),
            "genuine AT_ENTRY is not owned by any NT_FILE mapping");
    require(std::filesystem::weakly_canonical(entry_mapping->path) ==
                std::filesystem::weakly_canonical(fixture),
            "genuine AT_ENTRY is not owned by the crashed executable mapping");
    const auto phdr_mapping = snapshot.mapping_for_address(auxv.program_headers);
    require(phdr_mapping.has_value(),
            "genuine AT_PHDR is not owned by any NT_FILE mapping");
    require(std::filesystem::weakly_canonical(phdr_mapping->path) ==
                std::filesystem::weakly_canonical(fixture),
            "genuine AT_PHDR is not owned by the crashed executable mapping");

    const mdbg::CoreInspectionSession session(core_path);
    require(session.crash_info().has_value() &&
                session.crash_info()->signal_number == SIGSEGV,
            "core inspection session did not expose immutable crash metadata");
    const auto& startup = session.startup_info();
    require(startup.has_value(),
            "core inspection session did not expose genuine NT_AUXV startup metadata");
    require(startup->entry_point == std::optional<std::uint64_t>{auxv.entry_point} &&
                startup->program_headers == std::optional<std::uint64_t>{auxv.program_headers} &&
                startup->program_header_count ==
                    std::optional<std::uint64_t>{auxv.program_header_count} &&
                startup->page_size == std::optional<std::uint64_t>{auxv.page_size} &&
                startup->interpreter_base == auxv.interpreter_base,
            "core inspection session changed kernel-recorded startup metadata");
    require(startup->entries.size() == auxv.entries.size(),
            "core startup consumer did not preserve every non-null AUXV entry");
    for (std::size_t index = 0; index < auxv.entries.size(); ++index) {
      require(startup->entries[index].type == auxv.entries[index].first &&
                  startup->entries[index].value == auxv.entries[index].second,
              "core startup consumer changed an unknown or recognized AUXV entry");
    }

    const auto output = run_core_cli(cli, core_path);
    const auto expected = std::string("crash signal ") + std::to_string(SIGSEGV) +
                          " code " + std::to_string(SEGV_MAPERR) + " address 0x0";
    require(output.find(expected) != std::string::npos,
            "mdbg-core did not render kernel-recorded crash metadata");
    std::ostringstream expected_startup;
    expected_startup << "startup entry 0x" << std::hex << auxv.entry_point << " phdr 0x"
                     << auxv.program_headers << std::dec << " phnum "
                     << auxv.program_header_count << " pagesz " << auxv.page_size;
    if (auxv.interpreter_base) {
      expected_startup << " base 0x" << std::hex << *auxv.interpreter_base << std::dec;
    }
    require(output.find(expected_startup.str()) != std::string::npos,
            "mdbg-core did not render kernel-recorded startup metadata");

    const auto process_note =
        find_core_note(original, NT_PRPSINFO, "CORE/NT_PRPSINFO");
    require(process_note.header.n_descsz == sizeof(elf_prpsinfo),
            "genuine x86-64 Linux NT_PRPSINFO size is outside the bounded contract");
    elf_prpsinfo process{};
    std::memcpy(&process, original.data() + process_note.desc_offset, sizeof(process));
    require(process.pr_pid == recorded_pid,
            "kernel NT_PRPSINFO did not preserve the crashed process PID");
    require(process.pr_ppid == ::getpid(),
            "kernel NT_PRPSINFO did not preserve the parent process PID");
    const auto process_name = bounded_text(process.pr_fname, sizeof(process.pr_fname));
    const auto process_command = bounded_text(process.pr_psargs, sizeof(process.pr_psargs));
    require(!process_name.empty(),
            "kernel NT_PRPSINFO did not preserve a process filename");
    require(!process_command.empty(),
            "kernel NT_PRPSINFO did not preserve bounded process command text");

    const auto& immutable_process = snapshot.process_info();
    require(immutable_process.has_value(),
            "CoreSnapshot did not expose genuine NT_PRPSINFO process identity");
    require(immutable_process->pid == process.pr_pid &&
                immutable_process->parent_pid == process.pr_ppid &&
                immutable_process->process_group_id == process.pr_pgrp &&
                immutable_process->session_id == process.pr_sid,
            "CoreSnapshot changed kernel-recorded process relationships");
    require(immutable_process->file_name == process_name &&
                immutable_process->command == process_command,
            "CoreSnapshot changed bounded kernel-recorded process text");
    require(session.process_info().has_value() &&
                session.process_info()->pid == recorded_pid,
            "core inspection session did not expose immutable process identity");

    const auto expected_process =
        std::string("process pid ") + std::to_string(process.pr_pid) + " parent " +
        std::to_string(process.pr_ppid) + " pgrp " + std::to_string(process.pr_pgrp) +
        " sid " + std::to_string(process.pr_sid) + " file " + process_name + " command " +
        process_command;
    require(output.find(expected_process) != std::string::npos,
            "mdbg-core did not render kernel-recorded process identity");

    auto auxv_duplicate = original;
    Elf64_auxv_t duplicate_entry{};
    std::memcpy(&duplicate_entry, auxv_duplicate.data() + auxv.phdr_offset,
                sizeof(duplicate_entry));
    duplicate_entry.a_type = AT_ENTRY;
    std::memcpy(auxv_duplicate.data() + auxv.phdr_offset, &duplicate_entry,
                sizeof(duplicate_entry));
    expect_startup_failure(auxv_duplicate, "duplicate recognized NT_AUXV key");

    auto auxv_no_null = original;
    Elf64_auxv_t missing_null{};
    std::memcpy(&missing_null, auxv_no_null.data() + auxv.null_offset,
                sizeof(missing_null));
    missing_null.a_type = 0x7ffffffbU;
    missing_null.a_un.a_val = 0;
    std::memcpy(auxv_no_null.data() + auxv.null_offset, &missing_null,
                sizeof(missing_null));
    expect_startup_failure(auxv_no_null, "NT_AUXV without AT_NULL terminator");

    auto auxv_bad_entry = original;
    Elf64_auxv_t bad_entry{};
    std::memcpy(&bad_entry, auxv_bad_entry.data() + auxv.entry_offset, sizeof(bad_entry));
    bad_entry.a_un.a_val = 1;
    std::memcpy(auxv_bad_entry.data() + auxv.entry_offset, &bad_entry, sizeof(bad_entry));
    expect_startup_failure(auxv_bad_entry, "NT_AUXV AT_ENTRY outside NT_FILE ownership");

    if (auxv.base_offset) {
      auto auxv_bad_base = original;
      Elf64_auxv_t bad_base{};
      std::memcpy(&bad_base, auxv_bad_base.data() + *auxv.base_offset, sizeof(bad_base));
      bad_base.a_un.a_val = 1;
      std::memcpy(auxv_bad_base.data() + *auxv.base_offset, &bad_base, sizeof(bad_base));
      expect_startup_failure(auxv_bad_base, "NT_AUXV AT_BASE outside NT_FILE ownership");
    }

    auto auxv_absent = original;
    Elf64_Nhdr absent_auxv_header = auxv_note.header;
    absent_auxv_header.n_type = 0x7ffffffcU;
    std::memcpy(auxv_absent.data() + auxv_note.header_offset,
                &absent_auxv_header, sizeof(absent_auxv_header));
    const auto auxv_absent_path = write_variant(auxv_absent);
    const mdbg::CoreInspectionSession auxv_absent_session(auxv_absent_path);
    require(!auxv_absent_session.startup_info().has_value(),
            "core without NT_AUXV fabricated startup metadata");
    require(auxv_absent_session.crash_info().has_value(),
            "core without NT_AUXV lost independent crash metadata");
    require(auxv_absent_session.process_info().has_value(),
            "core without NT_AUXV lost independent process identity");
    std::remove(auxv_absent_path.c_str());

    auto contradictory = original;
    siginfo_t info{};
    std::memcpy(&info, contradictory.data() + note.desc_offset, sizeof(info));
    info.si_signo = SIGBUS;
    std::memcpy(contradictory.data() + note.desc_offset, &info, sizeof(info));
    expect_core_failure(contradictory,
                        "NT_SIGINFO/NT_PRSTATUS signal contradiction");

    auto malformed = original;
    Elf64_Nhdr malformed_header = note.header;
    malformed_header.n_descsz = sizeof(siginfo_t) - 1;
    std::memcpy(malformed.data() + note.header_offset, &malformed_header,
                sizeof(malformed_header));
    expect_core_failure(malformed, "malformed NT_SIGINFO descriptor size");

    auto process_contradiction = original;
    elf_prpsinfo contradictory_process = process;
    contradictory_process.pr_pid = process.pr_pid == 1 ? 2 : 1;
    std::memcpy(process_contradiction.data() + process_note.desc_offset,
                &contradictory_process, sizeof(contradictory_process));
    expect_core_failure(process_contradiction,
                        "NT_PRPSINFO PID outside NT_PRSTATUS thread catalogue");

    auto process_malformed = original;
    Elf64_Nhdr malformed_process_header = process_note.header;
    malformed_process_header.n_descsz = sizeof(elf_prpsinfo) - 1;
    std::memcpy(process_malformed.data() + process_note.header_offset,
                &malformed_process_header, sizeof(malformed_process_header));
    expect_core_failure(process_malformed, "malformed NT_PRPSINFO descriptor size");

    auto process_absent = original;
    Elf64_Nhdr absent_process_header = process_note.header;
    absent_process_header.n_type = 0x7ffffffdU;
    std::memcpy(process_absent.data() + process_note.header_offset,
                &absent_process_header, sizeof(absent_process_header));
    const auto process_absent_path = write_variant(process_absent);
    const mdbg::CoreSnapshot process_absent_snapshot(process_absent_path);
    require(!process_absent_snapshot.process_info().has_value(),
            "core without NT_PRPSINFO fabricated process identity");
    require(process_absent_snapshot.crash_info().has_value(),
            "core without NT_PRPSINFO lost independent NT_SIGINFO evidence");
    std::remove(process_absent_path.c_str());

    auto absent = original;
    Elf64_Nhdr absent_header = note.header;
    absent_header.n_type = 0x7ffffffeU;
    std::memcpy(absent.data() + note.header_offset, &absent_header,
                sizeof(absent_header));
    const auto absent_path = write_variant(absent);
    const mdbg::CoreSnapshot absent_snapshot(absent_path);
    require(!absent_snapshot.crash_info().has_value(),
            "core without NT_SIGINFO fabricated crash metadata");
    require(absent_snapshot.signal_number() == SIGSEGV,
            "core without NT_SIGINFO lost NT_PRSTATUS signal evidence");
    require(absent_snapshot.process_info().has_value(),
            "removing NT_SIGINFO also removed independent process identity");
    std::remove(absent_path.c_str());
  } catch (...) {
    std::remove(core_path.c_str());
    throw;
  }
  std::remove(core_path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_siginfo_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    test_real_siginfo(argv[1], argv[2]);
    std::cout << "core siginfo integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "core siginfo integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

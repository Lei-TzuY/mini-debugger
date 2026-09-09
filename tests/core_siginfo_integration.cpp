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
#include <stdexcept>
#include <string>
#include <thread>
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
  const std::string script = "crash\nquit\n";
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
          "mdbg-core crash command failed");
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

    const mdbg::CoreInspectionSession session(core_path);
    require(session.crash_info().has_value() &&
                session.crash_info()->signal_number == SIGSEGV,
            "core inspection session did not expose immutable crash metadata");

    const auto output = run_core_cli(cli, core_path);
    const auto expected = std::string("crash signal ") + std::to_string(SIGSEGV) +
                          " code " + std::to_string(SEGV_MAPERR) + " address 0x0";
    require(output.find(expected) != std::string::npos,
            "mdbg-core did not render kernel-recorded crash metadata");

    const auto original = read_file_bytes(core_path);
    const auto note = find_core_note(original, NT_SIGINFO, "CORE/NT_SIGINFO");
    require(note.header.n_descsz == sizeof(siginfo_t),
            "genuine x86-64 Linux NT_SIGINFO size is outside the bounded contract");

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
    require(!bounded_text(process.pr_fname, sizeof(process.pr_fname)).empty(),
            "kernel NT_PRPSINFO did not preserve a process filename");
    require(bounded_text(process.pr_psargs, sizeof(process.pr_psargs)).find("--snapshot-crash") !=
                std::string::npos,
            "kernel NT_PRPSINFO did not preserve the controlled crash command text");

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

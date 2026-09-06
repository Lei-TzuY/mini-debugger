#include "elf/elf.hpp"
#include "snapshot/inspection.hpp"
#include "unwind/cfi.hpp"

#include <elf.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kCoreRegisterMarker = 0x13579bdf2468ace0ULL;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void test_symbols(const std::string& path, bool expect_pie) {
  const mdbg::ElfFile elf(path);
  require(elf.is_pie() == expect_pie, "ELF PIE classification mismatch for " + path);
  const auto one = elf.find_symbol("breakpoint_one");
  const auto two = elf.find_symbol("breakpoint_two");
  require(one && two, "expected fixture function symbols");
  require(one->type == STT_FUNC && two->type == STT_FUNC, "fixture symbols should be functions");
  const auto resolved = elf.find_symbol_by_virtual_address(one->value);
  require(resolved && resolved->symbol.name == "breakpoint_one" && resolved->offset == 0,
          "address-to-symbol lookup failed");
}

std::string temp_path() {
  char pattern[] = "/tmp/mdbg-core-ready-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd == -1) throw std::runtime_error("mkstemp failed");
  ::close(fd);
  ::unlink(pattern);
  return pattern;
}

std::vector<pid_t> task_ids(pid_t leader) {
  std::vector<pid_t> result;
  const auto path = std::filesystem::path("/proc") / std::to_string(leader) / "task";
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    result.push_back(static_cast<pid_t>(std::stol(entry.path().filename().string())));
  }
  std::sort(result.begin(), result.end());
  return result;
}

pid_t wait_for_worker(pid_t leader, const std::string& ready_path) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::filesystem::exists(ready_path)) {
      const auto tids = task_ids(leader);
      for (const auto tid : tids) {
        if (tid != leader) return tid;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  throw std::runtime_error("core fixture worker did not become ready");
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
  char pattern[] = "/tmp/mdbg-core-variant-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd == -1) throw std::runtime_error("mkstemp failed for core variant");
  ::close(fd);
  std::ofstream output(pattern, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("failed to write core variant");
  return pattern;
}

void expect_core_failure(const std::vector<std::byte>& bytes, const std::string& context) {
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

std::vector<std::byte> replace_all_ascii(std::vector<std::byte> bytes,
                                         const std::string& from,
                                         const std::string& to) {
  require(!from.empty() && from.size() == to.size(),
          "core path replacement must preserve non-empty width");
  std::size_t replacements = 0;
  for (std::size_t offset = 0; offset + from.size() <= bytes.size(); ++offset) {
    bool match = true;
    for (std::size_t index = 0; index < from.size(); ++index) {
      if (std::to_integer<unsigned char>(bytes[offset + index]) !=
          static_cast<unsigned char>(from[index])) {
        match = false;
        break;
      }
    }
    if (!match) continue;
    for (std::size_t index = 0; index < to.size(); ++index) {
      bytes[offset + index] = static_cast<std::byte>(static_cast<unsigned char>(to[index]));
    }
    ++replacements;
    offset += from.size() - 1;
  }
  require(replacements != 0, "real core did not contain the mapped fixture path");
  return bytes;
}

std::string unavailable_peer_path(const std::string& path) {
  auto candidate = path;
  for (std::size_t offset = candidate.size(); offset > 0; --offset) {
    const auto index = offset - 1;
    if (candidate[index] == '/') continue;
    const char original = candidate[index];
    candidate[index] = original == 'x' ? 'y' : 'x';
    if (!std::filesystem::exists(candidate)) return candidate;
    candidate[index] = original;
  }
  throw std::runtime_error("could not derive unavailable same-width module path");
}

void test_malformed_core_boundaries(const std::vector<std::byte>& original) {
  require(original.size() >= sizeof(Elf64_Ehdr), "real core is smaller than ELF header");

  std::vector<std::byte> truncated(original.begin(),
                                   original.begin() + sizeof(Elf64_Ehdr) - 1);
  expect_core_failure(truncated, "truncated ELF core header");

  auto wrong_type = original;
  Elf64_Ehdr header{};
  std::memcpy(&header, wrong_type.data(), sizeof(header));
  header.e_type = ET_EXEC;
  std::memcpy(wrong_type.data(), &header, sizeof(header));
  expect_core_failure(wrong_type, "non-ET_CORE file");

  auto bad_phdr_table = original;
  std::memcpy(&header, bad_phdr_table.data(), sizeof(header));
  header.e_phoff = std::numeric_limits<Elf64_Off>::max() - 8;
  std::memcpy(bad_phdr_table.data(), &header, sizeof(header));
  expect_core_failure(bad_phdr_table, "out-of-range program-header table");

  std::memcpy(&header, original.data(), sizeof(header));
  bool mutated_note = false;
  auto bad_note = original;
  for (std::size_t index = 0; index < header.e_phnum; ++index) {
    const auto offset = static_cast<std::size_t>(header.e_phoff) + index * sizeof(Elf64_Phdr);
    if (offset > bad_note.size() || sizeof(Elf64_Phdr) > bad_note.size() - offset) break;
    Elf64_Phdr phdr{};
    std::memcpy(&phdr, bad_note.data() + offset, sizeof(phdr));
    if (phdr.p_type != PT_NOTE) continue;
    phdr.p_offset = static_cast<Elf64_Off>(bad_note.size() + 1);
    std::memcpy(bad_note.data() + offset, &phdr, sizeof(phdr));
    mutated_note = true;
    break;
  }
  require(mutated_note, "real core did not contain PT_NOTE");
  expect_core_failure(bad_note, "out-of-range PT_NOTE");
}

void test_snapshot_inspection(const mdbg::CoreSnapshot& snapshot,
                              const std::string& fixture) {
  const auto rip = static_cast<std::uintptr_t>(snapshot.registers().rip);
  const auto module = mdbg::resolve_snapshot_module_address(snapshot, rip);

  std::error_code left_error;
  std::error_code right_error;
  const auto mapped = std::filesystem::weakly_canonical(module.module_path, left_error);
  const auto expected = std::filesystem::weakly_canonical(fixture, right_error);
  require(!left_error && !right_error && mapped == expected,
          "snapshot module inspection selected the wrong executable");

  const mdbg::ElfFile elf(fixture);
  const auto worker = elf.find_symbol("register_mutation_worker");
  require(worker.has_value(), "fixture lacks register_mutation_worker symbol");
  require(module.virtual_address >= worker->value &&
              (worker->size == 0 || module.virtual_address < worker->value + worker->size),
          "snapshot PIE/non-PIE module virtual address missed the crashing function");

  const auto symbol = mdbg::find_snapshot_symbol_by_runtime_address(snapshot, rip);
  require(symbol && symbol->name == "register_mutation_worker" &&
              symbol->module_path == module.module_path,
          "snapshot crash symbol was not module-qualified correctly");

  const auto source = mdbg::find_snapshot_source_by_runtime_address(snapshot, rip);
  require(source && source->module_path == module.module_path && source->line != 0 &&
              source->file.find("debugger_fixture.c") != std::string::npos,
          "snapshot crash source was not resolved from compiler DWARF");

  bool outside_failed = false;
  try {
    (void)mdbg::resolve_snapshot_module_address(snapshot, 1);
  } catch (const std::exception&) {
    outside_failed = true;
  }
  require(outside_failed, "snapshot inspection accepted an address outside NT_FILE mappings");
}

void test_snapshot_cfi_unwind(const mdbg::CoreSnapshot& snapshot) {
  const auto& regs = snapshot.registers();
  const auto trace = mdbg::unwind_eh_frame(snapshot, 2);
  require(trace.frames.size() == 2,
          "snapshot CFI did not recover one compiler caller frame");
  require(trace.stop_reason == mdbg::CfiUnwindStopReason::FrameLimit,
          "bounded two-frame snapshot unwind did not stop at its frame limit");
  require(trace.frames[0].instruction_pointer == static_cast<std::uintptr_t>(regs.rip) &&
              trace.frames[0].stack_pointer == static_cast<std::uintptr_t>(regs.rsp),
          "snapshot CFI top frame did not preserve crash RIP/RSP evidence");
  require(trace.frames[0].frame_pointer == static_cast<std::uintptr_t>(regs.rbp),
          "snapshot CFI top frame did not preserve crash RBP evidence");
  require(trace.frames[1].instruction_pointer != 0 &&
              trace.frames[1].instruction_pointer != trace.frames[0].instruction_pointer,
          "snapshot CFI caller PC is invalid or unchanged");
  require(trace.frames[1].stack_pointer > trace.frames[0].stack_pointer,
          "snapshot CFI caller stack pointer did not move toward the caller");
  const auto caller_mapping =
      snapshot.mapping_for_address(trace.frames[1].instruction_pointer);
  require(caller_mapping.has_value() && !caller_mapping->path.empty(),
          "snapshot CFI caller is not owned by recorded NT_FILE evidence");
}

void test_missing_snapshot_module(const std::vector<std::byte>& original,
                                  const std::string& fixture,
                                  std::uintptr_t rip) {
  const auto missing = unavailable_peer_path(fixture);
  const auto bytes = replace_all_ascii(original, fixture, missing);
  const auto path = write_variant(bytes);
  const mdbg::CoreSnapshot snapshot(path);
  bool failed = false;
  try {
    (void)mdbg::find_snapshot_symbol_by_runtime_address(snapshot, rip);
  } catch (const std::exception&) {
    failed = true;
  }
  require(failed, "snapshot inspection guessed through an unavailable NT_FILE module path");

  const auto trace = mdbg::unwind_eh_frame(snapshot, 2);
  require(trace.frames.size() == 1 &&
              trace.stop_reason == mdbg::CfiUnwindStopReason::InvalidFrameState,
          "snapshot CFI did not fail closed on unavailable owning module evidence");
  std::remove(path.c_str());
}

void test_core_snapshot(const std::string& fixture, bool exercise_malformed) {
  const auto ready_path = temp_path();
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed");
  if (child == 0) {
    rlimit core_limit{};
    if (::getrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(120);
    core_limit.rlim_cur = core_limit.rlim_max;
    if (::setrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(121);
    ::execl(fixture.c_str(), fixture.c_str(), ready_path.c_str(),
            "attach-register-mutation", nullptr);
    _exit(127);
  }

  const auto core_path = "/tmp/mdbg-core-" + std::to_string(child);
  std::remove(core_path.c_str());
  pid_t worker = -1;
  try {
    worker = wait_for_worker(child, ready_path);
    require(::syscall(SYS_tgkill, child, worker, SIGSEGV) == 0,
            "failed to crash the deterministic worker thread");
  } catch (...) {
    ::kill(child, SIGKILL);
    int status = 0;
    while (::waitpid(child, &status, 0) == -1 && errno == EINTR) {
    }
    std::remove(ready_path.c_str());
    std::remove(core_path.c_str());
    throw;
  }

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  require(waited == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
          "core fixture did not terminate from SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce the expected deterministic core file");

  const mdbg::CoreSnapshot snapshot(core_path);
  require(snapshot.crashed_tid() == worker,
          "NT_PRSTATUS did not identify the crashing worker TID");
  require(snapshot.signal_number() == SIGSEGV,
          "core snapshot did not retain the fatal signal");
  const auto& regs = snapshot.registers();
  require(regs.rip != 0 && regs.rsp != 0 && regs.rbp != 0,
          "core snapshot did not recover crash control registers");
  require(regs.r12 == kCoreRegisterMarker,
          "core snapshot did not recover deterministic worker r12");

  const auto stack = snapshot.read_memory(static_cast<std::uintptr_t>(regs.rsp), 16);
  require(stack.size() == 16, "core snapshot could not read captured stack bytes");

  const auto mapping = snapshot.mapping_for_address(static_cast<std::uintptr_t>(regs.rip));
  require(mapping.has_value(), "core snapshot could not map the crash RIP to a file");
  std::error_code left_error;
  std::error_code right_error;
  const auto mapped = std::filesystem::weakly_canonical(mapping->path, left_error);
  const auto expected = std::filesystem::weakly_canonical(fixture, right_error);
  require(!left_error && !right_error && mapped == expected,
          "core NT_FILE mapping did not identify the crashing executable");

  test_snapshot_inspection(snapshot, fixture);
  test_snapshot_cfi_unwind(snapshot);

  bool unmapped_failed = false;
  try {
    (void)snapshot.read_memory(1, 8);
  } catch (const std::exception&) {
    unmapped_failed = true;
  }
  require(unmapped_failed, "unmapped core-memory read was accepted");

  if (exercise_malformed) {
    const auto original = read_file_bytes(core_path);
    test_malformed_core_boundaries(original);
    test_missing_snapshot_module(original, fixture, static_cast<std::uintptr_t>(regs.rip));
  }

  std::remove(ready_path.c_str());
  std::remove(core_path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  try {
    test_symbols(argv[1], true);
    test_symbols(argv[2], false);
    const mdbg::ElfFile stripped(argv[3]);
    require(!stripped.find_symbol("breakpoint_one"),
            "fully stripped fixture should not claim local function symbols");
    test_core_snapshot(argv[1], true);
    test_core_snapshot(argv[2], false);
    std::cout << "all ELF tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << "ELF test failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
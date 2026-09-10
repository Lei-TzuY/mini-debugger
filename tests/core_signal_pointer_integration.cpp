#include "elf/elf.hpp"
#include "snapshot/session.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uintptr_t kLinuxX86UcontextMcontextOffset = 0x28;
constexpr std::uintptr_t kLinuxX86RdiIndex = 8;
constexpr std::uintptr_t kLinuxX86RspIndex = 15;
constexpr std::uintptr_t kLinuxX86RipIndex = 16;
constexpr std::uint64_t kTargetValue = UINT64_C(0x7a6b5c4d3e2f1908);
constexpr std::uint64_t kCrashRdi = UINT64_C(0x0ddc0ffeebadf00d);
constexpr const char* kPointerExpression = "DW_OP_reg5 (rdi)";

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string shell_quote(const std::string& text) {
  std::string result{"'"};
  for (const char character : text) {
    if (character == '\'') {
      result += "'\\''";
    } else {
      result.push_back(character);
    }
  }
  result.push_back('\'');
  return result;
}

std::string read_command(const std::string& command, const std::string& context) {
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) throw std::runtime_error("failed to launch " + context);
  std::string output;
  char buffer[4096];
  while (const auto count = std::fread(buffer, 1, sizeof(buffer), pipe)) {
    output.append(buffer, count);
  }
  const int status = ::pclose(pipe);
  require(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          context + " did not exit cleanly");
  return output;
}

bool pure_hex(std::string token) {
  if (token.rfind("0x", 0) == 0 || token.rfind("0X", 0) == 0) token.erase(0, 2);
  return !token.empty() &&
         std::all_of(token.begin(), token.end(), [](unsigned char value) {
           return std::isxdigit(value) != 0;
         });
}

std::uint64_t parse_hex(std::string token) {
  if (token.rfind("0x", 0) == 0 || token.rfind("0X", 0) == 0) token.erase(0, 2);
  return std::stoull(token, nullptr, 16);
}

void require_pointer_location_covers_probe(const std::string& executable) {
  const mdbg::ElfFile elf(executable);
  const auto probe = elf.find_symbol("signal_pointer_interrupted_probe");
  require(probe.has_value(), "signal-pointer probe symbol is unavailable");
  const auto probe_virtual = probe->value;

  const auto info = read_command("readelf --wide --debug-dump=info " +
                                     shell_quote(executable) + " 2>/dev/null",
                                 "readelf info oracle");
  require(info.find("interrupted_pointer") != std::string::npos,
          "compiler DWARF did not retain the interrupted pointer formal");
  if (info.find(kPointerExpression) != std::string::npos) return;

  const auto locations = read_command("readelf --wide --debug-dump=loc " +
                                          shell_quote(executable) + " 2>/dev/null",
                                      "readelf location oracle");
  std::istringstream input(locations);
  std::string line;
  bool saw_rdi = false;
  bool covered = false;
  while (std::getline(input, line)) {
    const auto expression = line.find(kPointerExpression);
    if (expression == std::string::npos) continue;
    saw_rdi = true;
    std::istringstream prefix(line.substr(0, expression));
    std::vector<std::uint64_t> numbers;
    std::string token;
    while (prefix >> token) {
      if (pure_hex(token)) numbers.push_back(parse_hex(token));
    }
    if (numbers.size() < 2) continue;
    const auto begin = numbers[numbers.size() - 2];
    const auto end = numbers[numbers.size() - 1];
    if (begin <= probe_virtual && probe_virtual < end) covered = true;
  }
  require(saw_rdi, "compiler DWARF did not emit an RDI-backed pointer location");
  require(covered,
          "compiler DWARF RDI pointer location does not cover the synchronous fault PC");
}

std::uintptr_t runtime_symbol_address(const mdbg::CoreSnapshot& snapshot,
                                      const std::string& executable,
                                      const std::string& symbol_name) {
  const auto canonical = std::filesystem::canonical(executable).string();
  const mdbg::ElfFile elf(canonical);
  const auto symbol = elf.find_symbol(symbol_name);
  require(symbol.has_value(), "signal-pointer symbol is unavailable: " + symbol_name);
  if (!elf.is_pie()) return static_cast<std::uintptr_t>(symbol->value);

  for (const auto& mapping : snapshot.file_mappings()) {
    if (mapping.path != canonical || mapping.file_offset != 0) continue;
    require(mapping.start >= elf.load_virtual_base(),
            "signal-pointer PIE mapping is below ELF virtual base");
    return static_cast<std::uintptr_t>(mapping.start - elf.load_virtual_base() +
                                       symbol->value);
  }
  throw std::runtime_error("signal-pointer PIE offset-zero mapping is unavailable");
}

std::uint64_t read_u64(const mdbg::CoreSnapshot& snapshot, std::uintptr_t address) {
  const auto bytes = snapshot.read_memory(address, sizeof(std::uint64_t));
  require(bytes.size() == sizeof(std::uint64_t), "signal-pointer oracle read was truncated");
  std::uint64_t value = 0;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

std::uint64_t signal_greg(const mdbg::CoreSnapshot& snapshot,
                          std::uintptr_t ucontext_address,
                          std::uintptr_t greg_index) {
  return read_u64(snapshot, ucontext_address + kLinuxX86UcontextMcontextOffset +
                                greg_index * sizeof(std::uint64_t));
}

const mdbg::SnapshotInspectionFrameContext* trace_context(
    const mdbg::CoreInspectionSession& session, std::uintptr_t instruction_pointer,
    std::uintptr_t stack_pointer) {
  for (const auto& frame : session.trace().frames) {
    if (frame.runtime_pc == instruction_pointer && frame.stack_pointer == stack_pointer) {
      return &frame;
    }
  }
  return nullptr;
}

std::string run_core_cli(const std::string& cli, const std::string& core,
                         std::size_t frame_index) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create mdbg-core signal-pointer pipes");
  }
  const pid_t pid = ::fork();
  if (pid == -1) throw std::runtime_error("failed to fork mdbg-core signal-pointer session");
  if (pid == 0) {
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    ::execl(cli.c_str(), cli.c_str(), core.c_str(), nullptr);
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  const std::string commands = "frame " + std::to_string(frame_index) +
                               "\nprint interrupted_pointer\n"
                               "deref interrupted_pointer\nquit\n";
  std::size_t offset = 0;
  while (offset < commands.size()) {
    const auto written =
        ::write(input_pipe[1], commands.data() + offset, commands.size() - offset);
    if (written == -1 && errno == EINTR) continue;
    if (written <= 0) {
      throw std::runtime_error("failed to write mdbg-core signal-pointer command");
    }
    offset += static_cast<std::size_t>(written);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read mdbg-core signal-pointer output");
    if (count == 0) break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(output_pipe[0]);

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(pid, &status, 0);
  } while (waited == -1 && errno == EINTR);
  require(waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "mdbg-core signal-pointer session did not exit cleanly");
  return output;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_signal_pointer_integration <core> <fixture>\n";
    return 2;
  }

  try {
    require_pointer_location_covers_probe(argv[2]);
    mdbg::CoreInspectionSession session(argv[1]);
    const auto& snapshot = session.snapshot();
    const auto ucontext_address = read_u64(
        snapshot, runtime_symbol_address(snapshot, argv[2],
                                         "signal_pointer_ucontext_address"));
    require(ucontext_address != 0,
            "signal-pointer handler did not publish ucontext ownership");

    const auto target_address =
        runtime_symbol_address(snapshot, argv[2], "signal_pointer_target");
    const auto raw_rdi = signal_greg(snapshot, ucontext_address, kLinuxX86RdiIndex);
    const auto saved_rsp = signal_greg(snapshot, ucontext_address, kLinuxX86RspIndex);
    const auto saved_rip = signal_greg(snapshot, ucontext_address, kLinuxX86RipIndex);
    require(raw_rdi == target_address,
            "kernel ucontext did not preserve the compiler-owned pointer in RDI");
    require(saved_rip == runtime_symbol_address(snapshot, argv[2],
                                                "signal_pointer_interrupted_probe"),
            "synchronous SIGILL did not save the exact compiler-proven pointer PC");
    require(read_u64(snapshot, target_address) == kTargetValue,
            "genuine core bytes do not contain the runtime-mutated pointer target");

    const auto& crashed = snapshot.crashed_thread().registers;
    require(crashed.rdi == kCrashRdi,
            "crash-time PRSTATUS did not preserve the distinct handler RDI marker");
    require(crashed.rdi != raw_rdi,
            "signal-pointer evidence accidentally reused crash-time PRSTATUS ownership");

    const auto* restored = trace_context(session, saved_rip, saved_rsp);
    require(restored != nullptr,
            "snapshot unwind did not restore the synchronous interrupted pointer frame");
    require(restored->pc_ownership == mdbg::SnapshotFramePcOwnership::ExactInstruction,
            "signal-restored pointer frame lost exact-PC ownership");
    require(restored->registers.rdi == target_address,
            "signal-restored frame did not own the kernel-saved pointer RDI");
    for (const auto& frame : session.trace().frames) {
      if (frame.index <= restored->index) continue;
      require(!frame.registers.rdi.has_value(),
              "ordinary CFI frame inherited signal-restored pointer RDI ownership");
    }

    session.select_frame(restored->index);
    const auto pointer = session.inspect_value("interrupted_pointer");
    require(pointer.kind == mdbg::LocalValueKind::Pointer &&
                pointer.raw_value == target_address &&
                pointer.byte_size == sizeof(std::uintptr_t),
            "signal-restored pointer has the wrong value/type metadata");
    require(pointer.storage == mdbg::LocalValueStorage::SnapshotCoreRegister,
            "signal-restored pointer lost register provenance");
    require(pointer.pointee_type.has_value() &&
                pointer.pointee_type->kind == mdbg::LocalValueKind::Integer &&
                pointer.pointee_type->byte_size == sizeof(std::uint64_t),
            "signal-restored pointer lost bounded integer pointee metadata");

    const auto pointee = session.dereference_value("interrupted_pointer");
    require(pointee.kind == mdbg::LocalValueKind::Integer &&
                pointee.raw_value == kTargetValue &&
                pointee.byte_size == sizeof(std::uint64_t),
            "signal-restored pointer did not dereference the runtime-mutated target");
    require(pointee.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
            "signal-restored pointer dereference did not preserve genuine core-memory provenance");

    const auto cli = (std::filesystem::path(argv[0]).parent_path() / "mdbg-core").string();
    const auto output = run_core_cli(cli, argv[1], restored->index);
    require(output.find("interrupted_pointer = 0x") != std::string::npos,
            "mdbg-core did not print the signal-restored pointer");
    require(output.find("*interrupted_pointer = 0x7a6b5c4d3e2f1908") !=
                std::string::npos,
            "mdbg-core did not dereference the signal-restored pointer");

    std::cout << "signal-restored pointer provenance integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "signal-restored pointer provenance integration failure: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}

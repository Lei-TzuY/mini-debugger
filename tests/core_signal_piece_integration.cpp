#include "elf/elf.hpp"
#include "snapshot/session.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
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
constexpr std::uintptr_t kLinuxX86RsiIndex = 9;
constexpr std::uintptr_t kLinuxX86RspIndex = 15;
constexpr std::uintptr_t kLinuxX86RipIndex = 16;
constexpr std::uint64_t kPairFirst = UINT64_C(0x1122334455667788);
constexpr std::uint64_t kPairSecond = UINT64_C(0x99aabbccddeeff00);
constexpr std::uint64_t kCrashRdi = UINT64_C(0x0badf00d11223344);
constexpr std::uint64_t kCrashRsi = UINT64_C(0x55667788cafef00d);
constexpr const char* kPieceExpression =
    "DW_OP_reg5 (rdi); DW_OP_piece: 8; DW_OP_reg4 (rsi); DW_OP_piece: 8";

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

std::string readelf_locations(const std::string& executable) {
  const auto command = "readelf --wide --debug-dump=loc " + shell_quote(executable) +
                       " 2>/dev/null";
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) throw std::runtime_error("failed to launch readelf location oracle");
  std::string output;
  char buffer[4096];
  while (const auto count = std::fread(buffer, 1, sizeof(buffer), pipe)) {
    output.append(buffer, count);
  }
  const int status = ::pclose(pipe);
  require(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "readelf location oracle did not exit cleanly");
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

void require_piece_range_covers_probe(const std::string& executable) {
  const mdbg::ElfFile elf(executable);
  const auto probe = elf.find_symbol("signal_piece_interrupted_probe");
  require(probe.has_value(), "signal-piece probe symbol is unavailable");
  const auto probe_virtual = probe->value;

  std::istringstream input(readelf_locations(executable));
  std::string line;
  bool saw_piece = false;
  bool covered = false;
  while (std::getline(input, line)) {
    const auto expression = line.find(kPieceExpression);
    if (expression == std::string::npos) continue;
    saw_piece = true;
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
  require(saw_piece,
          "compiler DWARF did not emit the bounded RDI/RSI register-piece expression");
  require(covered,
          "compiler DWARF register-piece location does not cover the synchronous fault PC");
}

std::uintptr_t runtime_symbol_address(const mdbg::CoreSnapshot& snapshot,
                                      const std::string& executable,
                                      const std::string& symbol_name) {
  const auto canonical = std::filesystem::canonical(executable).string();
  const mdbg::ElfFile elf(canonical);
  const auto symbol = elf.find_symbol(symbol_name);
  require(symbol.has_value(), "signal-piece symbol is unavailable: " + symbol_name);
  if (!elf.is_pie()) return static_cast<std::uintptr_t>(symbol->value);

  for (const auto& mapping : snapshot.file_mappings()) {
    if (mapping.path != canonical || mapping.file_offset != 0) continue;
    require(mapping.start >= elf.load_virtual_base(),
            "signal-piece PIE mapping is below ELF virtual base");
    return static_cast<std::uintptr_t>(mapping.start - elf.load_virtual_base() +
                                       symbol->value);
  }
  throw std::runtime_error("signal-piece PIE offset-zero mapping is unavailable");
}

std::uint64_t read_u64(const mdbg::CoreSnapshot& snapshot, std::uintptr_t address) {
  const auto bytes = snapshot.read_memory(address, sizeof(std::uint64_t));
  require(bytes.size() == sizeof(std::uint64_t), "signal-piece oracle read was truncated");
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
    throw std::runtime_error("failed to create mdbg-core signal-piece pipes");
  }
  const pid_t pid = ::fork();
  if (pid == -1) throw std::runtime_error("failed to fork mdbg-core signal-piece session");
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
                               "\nprint interrupted_pair\nquit\n";
  std::size_t offset = 0;
  while (offset < commands.size()) {
    const auto written =
        ::write(input_pipe[1], commands.data() + offset, commands.size() - offset);
    if (written == -1 && errno == EINTR) continue;
    if (written <= 0) throw std::runtime_error("failed to write mdbg-core signal-piece command");
    offset += static_cast<std::size_t>(written);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read mdbg-core signal-piece output");
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
          "mdbg-core signal-piece session did not exit cleanly");
  return output;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_signal_piece_integration <core> <fixture>\n";
    return 2;
  }

  try {
    require_piece_range_covers_probe(argv[2]);
    mdbg::CoreInspectionSession session(argv[1]);
    const auto& snapshot = session.snapshot();
    const auto ucontext_address = read_u64(
        snapshot, runtime_symbol_address(snapshot, argv[2],
                                         "signal_piece_ucontext_address"));
    require(ucontext_address != 0, "signal-piece handler did not publish ucontext ownership");

    const auto raw_rdi = signal_greg(snapshot, ucontext_address, kLinuxX86RdiIndex);
    const auto raw_rsi = signal_greg(snapshot, ucontext_address, kLinuxX86RsiIndex);
    const auto saved_rsp = signal_greg(snapshot, ucontext_address, kLinuxX86RspIndex);
    const auto saved_rip = signal_greg(snapshot, ucontext_address, kLinuxX86RipIndex);
    require(raw_rdi == kPairFirst && raw_rsi == kPairSecond,
            "kernel ucontext did not preserve the compiler-owned RDI/RSI pieces");
    require(saved_rip == runtime_symbol_address(snapshot, argv[2],
                                                "signal_piece_interrupted_probe"),
            "synchronous SIGILL did not save the exact compiler-proven piece PC");

    const auto& crashed = snapshot.crashed_thread().registers;
    require(crashed.rdi == kCrashRdi && crashed.rsi == kCrashRsi,
            "crash-time PRSTATUS did not preserve the distinct handler marker registers");
    require(crashed.rdi != raw_rdi && crashed.rsi != raw_rsi,
            "signal-piece evidence accidentally reused crash-time PRSTATUS ownership");

    const auto* restored = trace_context(session, saved_rip, saved_rsp);
    require(restored != nullptr,
            "snapshot unwind did not restore the synchronous interrupted frame");
    require(restored->pc_ownership == mdbg::SnapshotFramePcOwnership::ExactInstruction,
            "signal-restored piece frame lost exact-PC ownership");
    require(restored->registers.rdi == kPairFirst && restored->registers.rsi == kPairSecond,
            "signal-restored frame did not own the kernel-saved RDI/RSI pieces");
    for (const auto& frame : session.trace().frames) {
      if (frame.index <= restored->index) continue;
      require(!frame.registers.rdi.has_value() && !frame.registers.rsi.has_value(),
              "ordinary CFI frame inherited signal-restored register-piece ownership");
    }

    session.select_frame(restored->index);
    const auto pair = session.inspect_value("interrupted_pair");
    require(pair.kind == mdbg::LocalValueKind::Structure && pair.byte_size == 16,
            "signal-restored register-piece aggregate lost its structure type");
    require(pair.members.size() == 2 && pair.members[0].name == "first" &&
                pair.members[0].raw_value == kPairFirst &&
                pair.members[1].name == "second" &&
                pair.members[1].raw_value == kPairSecond,
            "signal-restored register-piece aggregate has the wrong member values");
    require(pair.storage == mdbg::LocalValueStorage::SnapshotCoreRegister,
            "signal-restored register-piece aggregate lost register provenance");

    const auto cli = (std::filesystem::path(argv[0]).parent_path() / "mdbg-core").string();
    const auto output = run_core_cli(cli, argv[1], restored->index);
    require(output.find("interrupted_pair = { first=0x1122334455667788, second=0x99aabbccddeeff00 }") !=
                std::string::npos,
            "mdbg-core did not print the signal-restored register-piece aggregate");

    std::cout << "signal-restored register-piece integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "signal-restored register-piece integration failure: " << error.what()
              << '\n';
    return 1;
  }
  return 0;
}

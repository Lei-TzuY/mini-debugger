#include "elf/elf.hpp"
#include "snapshot/session.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uintptr_t kLinuxX86UcontextMcontextOffset = 0x28;
constexpr std::uintptr_t kLinuxX86GregR12 = 4;
constexpr std::uintptr_t kLinuxX86GregRbp = 10;
constexpr std::uintptr_t kLinuxX86GregRbx = 11;
constexpr std::uintptr_t kLinuxX86GregRsp = 15;
constexpr std::uintptr_t kLinuxX86GregRip = 16;
constexpr std::uintptr_t kGregSize = sizeof(std::uint64_t);
constexpr std::uint64_t kInterruptedRegisterValue = UINT64_C(0xb5856a1a93a34cbc);

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::uintptr_t runtime_symbol_address(const mdbg::CoreSnapshot& snapshot,
                                      const std::string& executable,
                                      const std::string& symbol_name) {
  const auto canonical = std::filesystem::canonical(executable).string();
  const mdbg::ElfFile elf(canonical);
  const auto symbol = elf.find_symbol(symbol_name);
  require(symbol.has_value(), "signal fixture symbol is unavailable: " + symbol_name);
  if (!elf.is_pie()) return static_cast<std::uintptr_t>(symbol->value);

  for (const auto& mapping : snapshot.file_mappings()) {
    if (mapping.path != canonical || mapping.file_offset != 0) continue;
    require(mapping.start >= elf.load_virtual_base(),
            "signal fixture PIE mapping is below ELF virtual base");
    const auto bias = mapping.start - elf.load_virtual_base();
    return static_cast<std::uintptr_t>(bias + symbol->value);
  }
  throw std::runtime_error("signal fixture PIE offset-zero mapping is unavailable");
}

std::uintptr_t read_pointer(const mdbg::CoreSnapshot& snapshot,
                            std::uintptr_t address) {
  const auto bytes = snapshot.read_memory(address, sizeof(std::uintptr_t));
  require(bytes.size() == sizeof(std::uintptr_t),
          "signal-frame oracle pointer read returned a short value");
  std::uintptr_t value = 0;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

std::uintptr_t read_signal_greg(const mdbg::CoreSnapshot& snapshot,
                                std::uintptr_t ucontext_address,
                                std::uintptr_t greg_index) {
  return read_pointer(snapshot, ucontext_address + kLinuxX86UcontextMcontextOffset +
                                    greg_index * kGregSize);
}

bool trace_has_symbol(const mdbg::CoreInspectionSession& session,
                      const std::string& expected) {
  for (const auto& frame : session.trace().frames) {
    try {
      const auto symbol = session.find_frame_symbol(frame);
      if (symbol && symbol->name == expected) return true;
    } catch (const std::exception&) {
    }
  }
  return false;
}

const mdbg::SnapshotInspectionFrameContext* trace_context(
    const mdbg::CoreInspectionSession& session, std::uintptr_t instruction_pointer,
    std::uintptr_t stack_pointer) {
  for (const auto& frame : session.trace().frames) {
    if (frame.runtime_pc == instruction_pointer &&
        frame.stack_pointer == stack_pointer) {
      return &frame;
    }
  }
  return nullptr;
}

const mdbg::SnapshotInspectionFrameContext* trace_stack_boundary(
    const mdbg::CoreInspectionSession& session, std::uintptr_t stack_pointer) {
  for (const auto& frame : session.trace().frames) {
    if (frame.stack_pointer == stack_pointer) return &frame;
  }
  return nullptr;
}

std::string run_core_cli(const std::string& cli, const std::string& core,
                         std::size_t frame_index) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create mdbg-core integration pipes");
  }

  const pid_t pid = ::fork();
  if (pid == -1) throw std::runtime_error("failed to fork mdbg-core integration");
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
                               "\nprint interrupted_register_local\nquit\n";
  std::size_t offset = 0;
  while (offset < commands.size()) {
    const auto written =
        ::write(input_pipe[1], commands.data() + offset, commands.size() - offset);
    if (written == -1 && errno == EINTR) continue;
    if (written <= 0) {
      ::close(input_pipe[1]);
      ::close(output_pipe[0]);
      ::kill(pid, SIGKILL);
      int status = 0;
      while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {
      }
      throw std::runtime_error("failed to write mdbg-core integration commands");
    }
    offset += static_cast<std::size_t>(written);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) {
      ::close(output_pipe[0]);
      ::kill(pid, SIGKILL);
      int status = 0;
      while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {
      }
      throw std::runtime_error("failed to read mdbg-core integration output");
    }
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
          "mdbg-core signal-restored source-value session did not exit cleanly");
  return output;
}

void print_evidence(const mdbg::CoreInspectionSession& session,
                    std::uintptr_t ucontext_address,
                    std::uintptr_t saved_rip,
                    std::uintptr_t saved_rsp,
                    std::uintptr_t saved_rbp,
                    std::uintptr_t saved_rbx,
                    std::uintptr_t saved_r12,
                    std::uintptr_t interrupted_begin,
                    std::uintptr_t interrupted_end) {
  std::cout << "signal-frame evidence: ucontext=0x" << std::hex << ucontext_address
            << " saved-rip=0x" << saved_rip << " saved-rsp=0x" << saved_rsp
            << " saved-rbp=0x" << saved_rbp << " saved-rbx=0x" << saved_rbx
            << " saved-r12=0x" << saved_r12
            << " interrupted=[0x" << interrupted_begin << ",0x" << interrupted_end
            << ")" << std::dec
            << " stop-reason=" << static_cast<int>(session.trace().stop_reason)
            << '\n';

  for (const auto& frame : session.trace().frames) {
    std::string symbol_name{"<unresolved>"};
    try {
      const auto symbol = session.find_frame_symbol(frame);
      if (symbol) symbol_name = symbol->name;
    } catch (const std::exception&) {
    }

    const auto delta = static_cast<std::int64_t>(ucontext_address) -
                       static_cast<std::int64_t>(frame.stack_pointer);
    std::cout << "  frame #" << frame.index << " rip=0x" << std::hex
              << frame.runtime_pc << " rsp=0x" << frame.stack_pointer << std::dec
              << " ucontext-rsp=" << delta
              << " pc-owner="
              << (frame.pc_ownership == mdbg::SnapshotFramePcOwnership::ExactInstruction
                      ? "exact"
                      : "return")
              << " module=" << frame.module_path << " symbol=" << symbol_name << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_signal_frame_integration <core> <fixture>\n";
    return 2;
  }

  try {
    mdbg::CoreInspectionSession session(argv[1]);
    const auto& snapshot = session.snapshot();
    require(snapshot.signal_number() == SIGSEGV,
            "signal-handler fixture core did not terminate with SIGSEGV");

    const auto ucontext_address = read_pointer(
        snapshot, runtime_symbol_address(snapshot, argv[2],
                                         "signal_core_ucontext_address"));
    const auto saved_rip = read_pointer(
        snapshot,
        runtime_symbol_address(snapshot, argv[2], "signal_core_saved_rip"));
    const auto saved_rsp = read_pointer(
        snapshot,
        runtime_symbol_address(snapshot, argv[2], "signal_core_saved_rsp"));
    const auto saved_rbp = read_pointer(
        snapshot,
        runtime_symbol_address(snapshot, argv[2], "signal_core_saved_rbp"));
    const auto saved_rbx = read_pointer(
        snapshot,
        runtime_symbol_address(snapshot, argv[2], "signal_core_saved_rbx"));
    const auto saved_r12 = read_pointer(
        snapshot,
        runtime_symbol_address(snapshot, argv[2], "signal_core_saved_r12"));
    require(ucontext_address != 0 && saved_rip != 0 && saved_rsp != 0,
            "kernel-provided signal ucontext oracle was not captured");

    require(read_signal_greg(snapshot, ucontext_address, kLinuxX86GregRip) == saved_rip,
            "Linux x86-64 signal ucontext RIP slot does not match the kernel oracle");
    require(read_signal_greg(snapshot, ucontext_address, kLinuxX86GregRsp) == saved_rsp,
            "Linux x86-64 signal ucontext RSP slot does not match the kernel oracle");
    require(read_signal_greg(snapshot, ucontext_address, kLinuxX86GregRbp) == saved_rbp,
            "Linux x86-64 signal ucontext RBP slot does not match the kernel oracle");
    require(read_signal_greg(snapshot, ucontext_address, kLinuxX86GregRbx) == saved_rbx,
            "Linux x86-64 signal ucontext RBX slot does not match the kernel oracle");
    require(read_signal_greg(snapshot, ucontext_address, kLinuxX86GregR12) == saved_r12,
            "Linux x86-64 signal ucontext R12 slot does not match the kernel oracle");
    require(saved_r12 == kInterruptedRegisterValue,
            "compiler-proven interrupted R12 local does not match the expected value");

    const auto interrupted_begin = runtime_symbol_address(
        snapshot, argv[2], "signal_core_interrupted_probe");
    const auto interrupted_end = runtime_symbol_address(
        snapshot, argv[2], "signal_core_interrupted_probe_end");

    print_evidence(session, ucontext_address, saved_rip, saved_rsp, saved_rbp,
                   saved_rbx, saved_r12, interrupted_begin, interrupted_end);

    require(interrupted_begin < interrupted_end && saved_rip >= interrupted_begin &&
                saved_rip < interrupted_end,
            "saved signal RIP is outside the explicit interrupted application probe range");
    require(trace_has_symbol(session, "signal_core_crash_probe"),
            "snapshot unwind lost the signal-handler crash probe");
    require(trace_has_symbol(session, "signal_core_handler_probe"),
            "snapshot unwind lost the signal handler probe");

    const auto* boundary = trace_stack_boundary(session, ucontext_address);
    require(boundary != nullptr,
            "snapshot unwind lost the kernel-provided signal ucontext boundary");
    require(boundary->pc_ownership == mdbg::SnapshotFramePcOwnership::ReturnAddress,
            "signal-restorer boundary unexpectedly owns an exact instruction PC");
    require(boundary->index + 1 < session.trace().frames.size(),
            "snapshot unwind stopped before restoring the interrupted application context");

    const auto* restored = trace_context(session, saved_rip, saved_rsp);
    require(restored != nullptr,
            "snapshot unwind did not cross the genuine signal frame to the interrupted application context");
    require(restored->index == boundary->index + 1,
            "restored interrupted context does not immediately follow the signal-frame boundary");
    require(restored->pc_ownership == mdbg::SnapshotFramePcOwnership::ExactInstruction,
            "signal-restored interrupted PC is incorrectly owned as a return address");
    require(mdbg::snapshot_frame_lookup_pc(*restored) == saved_rip,
            "signal-restored interrupted PC was incorrectly normalized as a historical return address");
    require(restored->frame_pointer.has_value() && *restored->frame_pointer == saved_rbp,
            "signal-restored RBP does not match the kernel ucontext oracle");
    require(restored->registers.rbx.has_value() && *restored->registers.rbx == saved_rbx,
            "signal-restored RBX does not match the kernel ucontext oracle");
    require(restored->registers.r12.has_value() && *restored->registers.r12 == saved_r12,
            "signal-restored frame did not retain kernel-owned R12 state");

    const auto restored_symbol = session.find_frame_symbol(*restored);
    require(restored_symbol && restored_symbol->name == "signal_core_interrupted_probe",
            "signal-restored exact PC does not resolve to the interrupted application probe");
    require(restored->index + 1 < session.trace().frames.size(),
            "ordinary CFI did not resume beyond the signal-restored frame");
    for (std::size_t index = restored->index + 1;
         index < session.trace().frames.size(); ++index) {
      require(!session.trace().frames[index].registers.r12.has_value(),
              "signal-restored R12 leaked into a later ordinary-CFI frame");
    }
    require(trace_has_symbol(session, "main"),
            "ordinary CFI did not resume beyond the signal-restored application context");

    session.select_frame(restored->index);
    const auto value = session.inspect_value("interrupted_register_local");
    require(value.raw_value == kInterruptedRegisterValue,
            "signal-restored register local has the wrong source value");
    require(value.storage == mdbg::LocalValueStorage::SnapshotCoreRegister,
            "signal-restored register local did not retain core-register provenance");

    const auto cli_path =
        (std::filesystem::path(argv[0]).parent_path() / "mdbg-core").string();
    const auto cli_output = run_core_cli(cli_path, argv[1], restored->index);
    require(cli_output.find("interrupted_register_local") != std::string::npos,
            "mdbg-core did not render the signal-restored local name");
    require(cli_output.find("0xb5856a1a93a34cbc") != std::string::npos,
            "mdbg-core did not render the signal-restored R12 local value");

    std::cout << "genuine signal-frame core integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "genuine signal-frame core integration failure: " << error.what()
              << '\n';
    return 1;
  }
  return 0;
}

#include "debugger/debugger.hpp"
#include "dwarf/eh_frame.hpp"
#include "dwarf/local_value.hpp"
#include "elf/elf.hpp"
#include "unwind/cfi.hpp"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string run_cli(const std::string& mdbg_path,
                    const std::string& fixture) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create historical frame CLI pipes");
  }

  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("failed to fork historical frame CLI");
  if (child == 0) {
    (void)::setpgid(0, 0);
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    ::execl(mdbg_path.c_str(), mdbg_path.c_str(), fixture.c_str(), nullptr);
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  const std::string script =
      "break historical_pointer_callee_probe\n"
      "continue\n"
      "bt\n"
      "locals\n"
      "frame 1\n"
      "locals\n"
      "print historical_pointer\n"
      "deref historical_pointer\n"
      "continue\n";
  std::size_t written = 0;
  while (written < script.size()) {
    const auto count =
        ::write(input_pipe[1], script.data() + written, script.size() - written);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) {
      throw std::runtime_error("failed to write historical frame CLI script");
    }
    written += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      ::kill(-child, SIGKILL);
      ::kill(child, SIGKILL);
      throw std::runtime_error("timed out waiting for historical frame CLI");
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count();
    pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
    const int polled =
        ::poll(&descriptor, 1, static_cast<int>(remaining));
    if (polled == -1 && errno == EINTR) continue;
    if (polled <= 0) {
      throw std::runtime_error(
          "timed out reading historical frame CLI output");
    }
    char buffer[512];
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) {
      throw std::runtime_error("failed to read historical frame CLI output");
    }
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
          "historical frame CLI tracee did not exit cleanly\n" + output);
  return output;
}

void verify_historical_pointer_cli(const std::string& fixture,
                                   const std::string& mdbg_path) {
  const auto output = run_cli(mdbg_path, fixture);
  require(output.find("#0 ") != std::string::npos &&
              output.find("#1 ") != std::string::npos,
          "live CLI backtrace did not expose a caller frame\n" + output);
  const auto selected_marker = output.find("selected inspection frame 1");
  require(selected_marker != std::string::npos,
          "live CLI did not select historical inspection frame 1\n" + output);
  const auto current_catalogue = output.substr(0, selected_marker);
  const auto historical_catalogue = output.substr(selected_marker);
  require(current_catalogue.find("parameter input") != std::string::npos,
          "live CLI current-frame locals did not expose callee parameter input\n" +
              output);
  require(historical_catalogue.find("parameter historical_pointer") !=
              std::string::npos,
          "live CLI historical-frame locals did not expose historical_pointer\n" +
              output);
  require(historical_catalogue.find("parameter input") == std::string::npos,
          "live CLI historical-frame locals leaked the callee-only input binding\n" +
              output);
  require(output.find("historical_pointer = 0x") != std::string::npos,
          "live CLI did not inspect the historical pointer\n" + output);
  require(output.find("*historical_pointer = 324508639") !=
              std::string::npos,
          "live CLI did not dereference the historical pointer\n" + output);
}

std::int32_t read_i32(const mdbg::Debugger& debugger, std::uintptr_t address) {
  const auto bytes = debugger.read_memory(address, sizeof(std::int32_t));
  require(bytes.size() == sizeof(std::int32_t),
          "historical pointer raw target read was truncated");
  std::int32_t value = 0;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

void verify_historical_pointer(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::EhFrame cfi(fixture);
  require(cfi.available(), "historical pointer fixture is missing .eh_frame");

  const auto callee_probe = elf.find_symbol("historical_pointer_callee_probe");
  const auto after_probe = elf.find_symbol("historical_pointer_after_probe");
  const auto target = elf.find_symbol("historical_pointer_target");
  require(callee_probe && after_probe && target,
          "historical pointer fixture symbols are missing");

  const auto callee_address = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *callee_probe));
  const auto after_address = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *after_probe));
  const auto target_address = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *target));
  debugger.add_breakpoint(callee_address);
  debugger.add_breakpoint(after_address);

  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == callee_address,
          "historical pointer fixture did not stop in the callee");

  // Independent target oracle before source-value inspection.
  require(read_i32(debugger, target_address) == INT32_C(0x13579bdf),
          "historical pointer target does not contain the expected live value");

  const auto frames = mdbg::build_inspection_frames(debugger, elf, cfi, 3);
  require(frames.size() >= 2,
          "CFI did not recover the historical pointer caller frame");

  const auto current_locals =
      mdbg::discover_local_values(debugger, elf, frames[0]);
  const auto historical_locals =
      mdbg::discover_local_values(debugger, elf, frames[1]);
  const auto has_local =
      [](const std::vector<mdbg::LocalDiscoveryEntry>& entries,
         const std::string& name, mdbg::LocalDiscoveryKind kind) {
        for (const auto& entry : entries) {
          if (entry.name == name && entry.kind == kind) return true;
        }
        return false;
      };
  require(has_local(current_locals, "input",
                    mdbg::LocalDiscoveryKind::FormalParameter),
          "live current-frame discovery did not expose callee parameter input");
  require(has_local(historical_locals, "historical_pointer",
                    mdbg::LocalDiscoveryKind::FormalParameter),
          "live historical-frame discovery did not expose historical_pointer");
  require(!has_local(historical_locals, "input",
                     mdbg::LocalDiscoveryKind::FormalParameter),
          "live historical-frame discovery leaked the callee-only input binding");
  const auto caller_function = elf.find_symbol("historical_pointer_caller");
  require(caller_function.has_value() && caller_function->size != 0,
          "historical pointer caller symbol is missing or has zero size");
  const auto caller_begin = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *caller_function));
  require(caller_function->size <=
              std::numeric_limits<std::uintptr_t>::max() - caller_begin,
          "historical pointer caller symbol range overflows");
  const auto caller_end = caller_begin + caller_function->size;
  require(frames[1].runtime_pc >= caller_begin &&
              frames[1].runtime_pc < caller_end,
          "historical pointer frame 1 PC is outside the caller function range");

  const auto current_regs = debugger.registers();
  require(current_regs.rbx == UINT64_C(0x1122334455667788),
          "callee live RBX does not contain the deliberate clobber sentinel");
  require(frames[1].registers.rbx.has_value() &&
              *frames[1].registers.rbx == target_address,
          "CFI did not recover caller-owned historical RBX pointer state");
  require(*frames[1].registers.rbx != current_regs.rbx,
          "historical pointer ownership accidentally reused current callee RBX");

  const auto pointer =
      mdbg::inspect_local_value(debugger, elf, frames[1], "historical_pointer");
  require(pointer.kind == mdbg::LocalValueKind::Pointer &&
              pointer.byte_size == sizeof(std::uintptr_t) &&
              !pointer.is_signed &&
              pointer.raw_value == target_address,
          "historical pointer lost compiler-owned pointer identity");
  require(pointer.pointee_type.has_value() &&
              pointer.pointee_type->kind == mdbg::LocalValueKind::Integer &&
              pointer.pointee_type->byte_size == sizeof(std::int32_t) &&
              pointer.pointee_type->is_signed,
          "historical pointer lost bounded signed-int32 pointee metadata");

  const auto pointee = mdbg::dereference_local_pointer(
      debugger, elf, frames[1], "historical_pointer");
  require(pointee.name == "*historical_pointer" &&
              pointee.kind == mdbg::LocalValueKind::Integer &&
              pointee.byte_size == sizeof(std::int32_t) &&
              pointee.is_signed &&
              pointee.raw_value == UINT64_C(0x13579bdf),
          "historical pointer one-hop dereference did not recover the live pointee");

  const auto stale_frame = frames[1];
  const auto next_stop = debugger.continue_execution();
  require(next_stop.reason == mdbg::StopReason::Breakpoint &&
              next_stop.breakpoint_address == after_address,
          "historical pointer fixture did not reach a new caller stop");

  bool stale_rejected = false;
  try {
    (void)mdbg::inspect_local_value(
        debugger, elf, stale_frame, "historical_pointer");
  } catch (const std::logic_error&) {
    stale_rejected = true;
  }
  require(stale_rejected,
          "historical pointer accepted an inspection frame from an older stop");

  bool stale_catalogue_rejected = false;
  try {
    (void)mdbg::discover_local_values(debugger, elf, stale_frame);
  } catch (const std::logic_error&) {
    stale_catalogue_rejected = true;
  }
  require(stale_catalogue_rejected,
          "historical local discovery accepted a catalogue from an older stop");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "historical pointer fixture did not exit cleanly");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    verify_historical_pointer(argv[1]);
    verify_historical_pointer_cli(argv[1], argv[2]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "historical live pointer integration failure: %s\n",
                 error.what());
    return 1;
  }
}

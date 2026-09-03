#include "source/source_finish.hpp"

#include "dwarf/eh_frame.hpp"
#include "elf/elf.hpp"
#include "ptrace/ptrace.hpp"
#include "unwind/cfi.hpp"

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mdbg {
namespace {

constexpr std::uintptr_t kMaxFrameSpan = 16U * 1024U * 1024U;

std::uint64_t read_u64(const std::vector<std::byte>& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

std::string process_executable(pid_t pid) {
  const std::string link = "/proc/" + std::to_string(pid) + "/exe";
  std::vector<char> buffer(4096);
  const auto length = ::readlink(link.c_str(), buffer.data(), buffer.size() - 1);
  if (length < 0) throw std::runtime_error("failed to resolve tracee executable for finish");
  buffer[static_cast<std::size_t>(length)] = '\0';
  return std::string(buffer.data());
}

std::uintptr_t frame_pointer_return_address(const Debugger& debugger) {
  if (debugger.state() != ProcessState::Stopped) {
    throw std::logic_error("finish requires a stopped tracee");
  }

  const auto regs = debugger.registers();
  const auto frame_pointer = static_cast<std::uintptr_t>(regs.rbp);
  constexpr auto frame_record_size = 2 * sizeof(std::uint64_t);
  if (frame_pointer == 0) {
    throw std::invalid_argument("finish requires a non-zero frame pointer");
  }
  if ((frame_pointer % alignof(std::uint64_t)) != 0) {
    throw std::invalid_argument("finish requires an aligned frame pointer");
  }
  if (frame_pointer > std::numeric_limits<std::uintptr_t>::max() - frame_record_size) {
    throw std::invalid_argument("finish frame record overflows address space");
  }

  std::vector<std::byte> frame_data;
  try {
    frame_data = debugger.read_memory(frame_pointer, frame_record_size);
  } catch (const lowlevel::PtraceError&) {
    throw std::invalid_argument("finish frame record is unreadable");
  }

  const auto previous_frame = static_cast<std::uintptr_t>(read_u64(frame_data, 0));
  const auto return_address =
      static_cast<std::uintptr_t>(read_u64(frame_data, sizeof(std::uint64_t)));
  if (return_address == 0) {
    throw std::invalid_argument("finish frame has no return address");
  }
  if (previous_frame != 0) {
    if ((previous_frame % alignof(std::uint64_t)) != 0 || previous_frame <= frame_pointer ||
        previous_frame - frame_pointer > kMaxFrameSpan) {
      throw std::invalid_argument("finish encountered an invalid frame-pointer chain");
    }
  }
  return return_address;
}

std::uintptr_t automatic_return_address(const Debugger& debugger) {
  if (debugger.state() != ProcessState::Stopped) {
    throw std::logic_error("finish requires a stopped tracee");
  }

  const auto executable = process_executable(debugger.pid());
  const ElfFile elf(executable);
  const EhFrame cfi(executable);
  if (const auto return_address = module_caller_return_address(debugger, elf, cfi)) {
    return *return_address;
  }
  return frame_pointer_return_address(debugger);
}

bool has_breakpoint_at(const Debugger& debugger, std::uintptr_t address) {
  for (const auto& breakpoint : debugger.breakpoints()) {
    if (breakpoint.address == address) return true;
  }
  return false;
}

void remove_temporary_breakpoint(Debugger& debugger, std::optional<std::size_t>& id) {
  if (!id) return;
  if (!debugger.remove_breakpoint(*id)) {
    throw std::logic_error("temporary finish breakpoint disappeared");
  }
  id.reset();
}

FinishResult run_to_return_address(Debugger& debugger, std::uintptr_t return_address) {
  const bool user_breakpoint = has_breakpoint_at(debugger, return_address);
  std::optional<std::size_t> temporary_breakpoint;
  if (!user_breakpoint) {
    temporary_breakpoint = debugger.add_breakpoint(return_address);
  }

  StopInfo stop;
  try {
    stop = debugger.continue_execution();
  } catch (...) {
    if (temporary_breakpoint && debugger.state() != ProcessState::Running) {
      try {
        remove_temporary_breakpoint(debugger, temporary_breakpoint);
      } catch (...) {
      }
    }
    throw;
  }

  const bool returned = stop.reason == StopReason::Breakpoint &&
                        stop.breakpoint_address == return_address;
  if (temporary_breakpoint) {
    remove_temporary_breakpoint(debugger, temporary_breakpoint);
  }
  return {returned && !user_breakpoint ? FinishStopReason::Returned
                                      : FinishStopReason::Interrupted,
          stop, return_address};
}

}  // namespace

FinishResult finish_frame(Debugger& debugger) {
  return run_to_return_address(debugger, automatic_return_address(debugger));
}

FinishResult finish_frame_pointer(Debugger& debugger) {
  return run_to_return_address(debugger, frame_pointer_return_address(debugger));
}

}  // namespace mdbg

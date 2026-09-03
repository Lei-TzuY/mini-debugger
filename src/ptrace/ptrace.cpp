#include "ptrace/ptrace.hpp"

#include <sys/ptrace.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>

namespace mdbg::lowlevel {
namespace {

[[noreturn]] void throw_errno(const char* operation) {
  throw PtraceError(operation, errno);
}

}  // namespace

PtraceError::PtraceError(std::string operation, int error_number)
    : std::runtime_error([&] {
        std::ostringstream stream;
        stream << operation << " failed: " << std::strerror(error_number)
               << " (errno=" << error_number << ')';
        return stream.str();
      }()),
      error_number_(error_number) {}

void traceme() {
  if (::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == -1) {
    throw_errno("PTRACE_TRACEME");
  }
}

void attach(pid_t pid) {
  if (::ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) == -1) {
    throw_errno("PTRACE_ATTACH");
  }
}

void detach(pid_t pid, int signal) {
  if (::ptrace(PTRACE_DETACH, pid, nullptr,
               reinterpret_cast<void*>(static_cast<intptr_t>(signal))) == -1) {
    throw_errno("PTRACE_DETACH");
  }
}

void set_options(pid_t pid, unsigned long options) {
  if (::ptrace(PTRACE_SETOPTIONS, pid, nullptr,
               reinterpret_cast<void*>(options)) == -1) {
    throw_errno("PTRACE_SETOPTIONS");
  }
}

void continue_process(pid_t pid, int signal) {
  if (::ptrace(PTRACE_CONT, pid, nullptr,
               reinterpret_cast<void*>(static_cast<intptr_t>(signal))) == -1) {
    throw_errno("PTRACE_CONT");
  }
}

void single_step(pid_t pid, int signal) {
  if (::ptrace(PTRACE_SINGLESTEP, pid, nullptr,
               reinterpret_cast<void*>(static_cast<intptr_t>(signal))) == -1) {
    throw_errno("PTRACE_SINGLESTEP");
  }
}

user_regs_struct get_registers(pid_t pid) {
  user_regs_struct regs{};
  if (::ptrace(PTRACE_GETREGS, pid, nullptr, &regs) == -1) {
    throw_errno("PTRACE_GETREGS");
  }
  return regs;
}

void set_registers(pid_t pid, const user_regs_struct& regs) {
  if (::ptrace(PTRACE_SETREGS, pid, nullptr,
               const_cast<user_regs_struct*>(&regs)) == -1) {
    throw_errno("PTRACE_SETREGS");
  }
}

std::uint64_t peek_word(pid_t pid, std::uintptr_t address) {
  errno = 0;
  const long value = ::ptrace(PTRACE_PEEKDATA, pid,
                              reinterpret_cast<void*>(address), nullptr);
  if (value == -1 && errno != 0) {
    throw_errno("PTRACE_PEEKDATA");
  }
  return static_cast<std::uint64_t>(static_cast<unsigned long>(value));
}

void poke_word(pid_t pid, std::uintptr_t address, std::uint64_t value) {
  if (::ptrace(PTRACE_POKEDATA, pid, reinterpret_cast<void*>(address),
               reinterpret_cast<void*>(static_cast<uintptr_t>(value))) == -1) {
    throw_errno("PTRACE_POKEDATA");
  }
}

std::vector<std::byte> read_memory(pid_t pid, std::uintptr_t address,
                                   std::size_t length) {
  std::vector<std::byte> result;
  result.reserve(length);
  constexpr std::size_t kWordSize = sizeof(long);

  for (std::size_t offset = 0; offset < length; offset += kWordSize) {
    const auto word = peek_word(pid, address + offset);
    const auto chunk = std::min(kWordSize, length - offset);
    for (std::size_t byte = 0; byte < chunk; ++byte) {
      result.push_back(static_cast<std::byte>((word >> (byte * 8U)) & 0xffU));
    }
  }
  return result;
}

std::byte read_byte(pid_t pid, std::uintptr_t address) {
  return static_cast<std::byte>(peek_word(pid, address) & 0xffU);
}

void write_byte(pid_t pid, std::uintptr_t address, std::byte value) {
  const auto current = peek_word(pid, address);
  const auto replacement = (current & ~std::uint64_t{0xff}) |
                           static_cast<std::uint64_t>(std::to_integer<unsigned>(value));
  poke_word(pid, address, replacement);
}

}  // namespace mdbg::lowlevel

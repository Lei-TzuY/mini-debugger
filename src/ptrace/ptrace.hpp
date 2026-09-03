#pragma once

#include <sys/types.h>
#include <sys/user.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace mdbg::lowlevel {

class PtraceError : public std::runtime_error {
 public:
  PtraceError(std::string operation, int error_number);
  [[nodiscard]] int error_number() const noexcept { return error_number_; }

 private:
  int error_number_;
};

void traceme();
void attach(pid_t pid);
void detach(pid_t pid, int signal = 0);
void set_options(pid_t pid, unsigned long options);
void continue_process(pid_t pid, int signal = 0);
void single_step(pid_t pid, int signal = 0);
user_regs_struct get_registers(pid_t pid);
void set_registers(pid_t pid, const user_regs_struct& regs);
std::uint64_t peek_word(pid_t pid, std::uintptr_t address);
void poke_word(pid_t pid, std::uintptr_t address, std::uint64_t value);
std::vector<std::byte> read_memory(pid_t pid, std::uintptr_t address, std::size_t length);
std::byte read_byte(pid_t pid, std::uintptr_t address);
void write_byte(pid_t pid, std::uintptr_t address, std::byte value);

}  // namespace mdbg::lowlevel

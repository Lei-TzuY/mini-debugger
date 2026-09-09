#pragma once

#include "dwarf/eh_frame.hpp"
#include "elf/elf.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace mdbg {

inline std::uint64_t read_linux_x86_signal_slot(const CoreSnapshot& snapshot,
                                                std::uintptr_t ucontext_address,
                                                std::uintptr_t greg_index) {
  constexpr std::uintptr_t kMcontextOffset = 0x28;
  constexpr std::uintptr_t kGregSize = sizeof(std::uint64_t);
  const auto address = ucontext_address + kMcontextOffset + greg_index * kGregSize;
  const auto bytes = snapshot.read_memory(address, sizeof(std::uint64_t));
  if (bytes.size() != sizeof(std::uint64_t)) {
    throw std::runtime_error("Linux x86-64 signal context register is truncated");
  }
  std::uint64_t value = 0;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

inline EhFrameCursor recover_linux_x86_signal_frame(const CoreSnapshot& snapshot,
                                                    std::uintptr_t ucontext_address) {
  constexpr std::uintptr_t kRbp = 10;
  constexpr std::uintptr_t kRbx = 11;
  constexpr std::uintptr_t kRsp = 15;
  constexpr std::uintptr_t kRip = 16;

  const auto rip = read_linux_x86_signal_slot(snapshot, ucontext_address, kRip);
  const auto rsp = read_linux_x86_signal_slot(snapshot, ucontext_address, kRsp);
  const auto rbp = read_linux_x86_signal_slot(snapshot, ucontext_address, kRbp);
  const auto rbx = read_linux_x86_signal_slot(snapshot, ucontext_address, kRbx);
  if (rip == 0 || rsp == 0) {
    throw std::runtime_error("Linux x86-64 signal context contains a zero RIP or RSP");
  }
  if (rsp <= ucontext_address) {
    throw std::runtime_error(
        "Linux x86-64 signal context is outside the evidence-proven same-stack layout");
  }

  return EhFrameCursor{static_cast<std::uintptr_t>(rip),
                       static_cast<std::uintptr_t>(rsp),
                       std::optional<std::uintptr_t>{static_cast<std::uintptr_t>(rbp)},
                       std::optional<std::uint64_t>{rbx}};
}

}  // namespace mdbg

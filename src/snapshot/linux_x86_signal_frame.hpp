#pragma once

#include "dwarf/eh_frame.hpp"
#include "elf/elf.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mdbg {

struct LinuxX86SignalFrameRecovery {
  EhFrameCursor cursor;
  std::uint64_t r12;
  std::uint64_t rdi;
  std::uint64_t rsi;
  std::array<std::byte, 16> xmm0;
};

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

inline std::array<std::byte, 16> read_linux_x86_signal_xmm0(
    const CoreSnapshot& snapshot, std::uintptr_t ucontext_address,
    std::uintptr_t restored_rsp) {
  constexpr std::uintptr_t kMcontextOffset = 0x28;
  constexpr std::uintptr_t kGregCount = 23;
  constexpr std::uintptr_t kFpregsPointerOffset =
      kMcontextOffset + kGregCount * sizeof(std::uint64_t);
  constexpr std::uintptr_t kFxsaveXmm0Offset = 160;
  constexpr std::size_t kXmmSize = 16;

  if (ucontext_address > std::numeric_limits<std::uintptr_t>::max() -
                             kFpregsPointerOffset) {
    throw std::overflow_error("Linux x86-64 signal fpregs pointer address overflows");
  }
  const auto pointer_bytes =
      snapshot.read_memory(ucontext_address + kFpregsPointerOffset,
                           sizeof(std::uint64_t));
  if (pointer_bytes.size() != sizeof(std::uint64_t)) {
    throw std::runtime_error("Linux x86-64 signal fpregs pointer is truncated");
  }
  std::uint64_t fpstate_u64 = 0;
  std::memcpy(&fpstate_u64, pointer_bytes.data(), sizeof(fpstate_u64));
  if (fpstate_u64 == 0 ||
      fpstate_u64 > std::numeric_limits<std::uintptr_t>::max()) {
    throw std::runtime_error("Linux x86-64 signal fpregs pointer is invalid");
  }
  const auto fpstate_address = static_cast<std::uintptr_t>(fpstate_u64);
  if (fpstate_address <= ucontext_address || fpstate_address >= restored_rsp) {
    throw std::runtime_error(
        "Linux x86-64 signal fpstate is outside the evidence-proven same-stack layout");
  }
  if (fpstate_address > std::numeric_limits<std::uintptr_t>::max() -
                            kFxsaveXmm0Offset) {
    throw std::overflow_error("Linux x86-64 signal XMM0 address overflows");
  }
  const auto xmm0_address = fpstate_address + kFxsaveXmm0Offset;
  if (xmm0_address > std::numeric_limits<std::uintptr_t>::max() - kXmmSize) {
    throw std::overflow_error("Linux x86-64 signal XMM0 range overflows");
  }
  if (xmm0_address + kXmmSize > restored_rsp) {
    throw std::runtime_error(
        "Linux x86-64 signal XMM0 state extends outside the evidence-proven same-stack layout");
  }
  const auto bytes = snapshot.read_memory(xmm0_address, kXmmSize);
  if (bytes.size() != kXmmSize) {
    throw std::runtime_error("Linux x86-64 signal XMM0 state is truncated");
  }
  std::array<std::byte, kXmmSize> xmm0{};
  std::copy(bytes.begin(), bytes.end(), xmm0.begin());
  return xmm0;
}

inline LinuxX86SignalFrameRecovery recover_linux_x86_signal_frame(
    const CoreSnapshot& snapshot, std::uintptr_t ucontext_address) {
  constexpr std::uintptr_t kR12 = 4;
  constexpr std::uintptr_t kRdi = 8;
  constexpr std::uintptr_t kRsi = 9;
  constexpr std::uintptr_t kRbp = 10;
  constexpr std::uintptr_t kRbx = 11;
  constexpr std::uintptr_t kRsp = 15;
  constexpr std::uintptr_t kRip = 16;

  const auto r12 = read_linux_x86_signal_slot(snapshot, ucontext_address, kR12);
  const auto rdi = read_linux_x86_signal_slot(snapshot, ucontext_address, kRdi);
  const auto rsi = read_linux_x86_signal_slot(snapshot, ucontext_address, kRsi);
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
  const auto restored_rsp = static_cast<std::uintptr_t>(rsp);
  const auto xmm0 =
      read_linux_x86_signal_xmm0(snapshot, ucontext_address, restored_rsp);

  return LinuxX86SignalFrameRecovery{
      EhFrameCursor{static_cast<std::uintptr_t>(rip), restored_rsp,
                    std::optional<std::uintptr_t>{static_cast<std::uintptr_t>(rbp)},
                    std::optional<std::uint64_t>{rbx}},
      r12,
      rdi,
      rsi,
      xmm0};
}

}  // namespace mdbg

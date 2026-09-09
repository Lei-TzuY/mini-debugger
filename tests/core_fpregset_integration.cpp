#include "elf/elf.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

using XmmValue = std::array<std::byte, 16>;

constexpr XmmValue kCrashXmm15{
    std::byte{0xef}, std::byte{0xcd}, std::byte{0xab}, std::byte{0x89},
    std::byte{0x67}, std::byte{0x45}, std::byte{0x23}, std::byte{0x01},
    std::byte{0x10}, std::byte{0x32}, std::byte{0x54}, std::byte{0x76},
    std::byte{0x98}, std::byte{0xba}, std::byte{0xdc}, std::byte{0xfe}};
constexpr XmmValue kSiblingXmm15{
    std::byte{0x78}, std::byte{0x69}, std::byte{0x5a}, std::byte{0x4b},
    std::byte{0x3c}, std::byte{0x2d}, std::byte{0x1e}, std::byte{0x0f},
    std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
    std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88}};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_fpregset_integration <core> <sibling-tid>\n";
    return 2;
  }
  try {
    const auto sibling_tid = static_cast<pid_t>(std::stol(argv[2]));
    const mdbg::CoreSnapshot snapshot(argv[1]);
    const auto crash = snapshot.floating_point_state(snapshot.crashed_tid());
    const auto sibling = snapshot.floating_point_state(sibling_tid);
    require(crash.has_value(), "crashed thread lost its genuine NT_FPREGSET state");
    require(sibling.has_value(), "sibling thread lost its genuine NT_FPREGSET state");
    require(crash->xmm[15] == kCrashXmm15,
            "crashed thread XMM15 did not match the seeded kernel-core evidence");
    require(sibling->xmm[15] == kSiblingXmm15,
            "sibling thread XMM15 did not match the seeded kernel-core evidence");
    require(crash->xmm[15] != sibling->xmm[15],
            "per-thread FPREGSET association collapsed distinct SSE state");
    std::cout << "core FPREGSET integration passed; crash mxcsr=0x" << std::hex
              << crash->mxcsr << " sibling mxcsr=0x" << sibling->mxcsr << std::dec << '\n';
  } catch (const std::exception& error) {
    std::cerr << "core FPREGSET integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

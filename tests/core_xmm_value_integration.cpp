#include "snapshot/session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr double kCrashValue = 1234.25;
constexpr double kSiblingValue = 9876.5;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::uint64_t raw_bits(double value) {
  static_assert(sizeof(value) == sizeof(std::uint64_t));
  std::uint64_t raw = 0;
  std::memcpy(&raw, &value, sizeof(raw));
  return raw;
}

std::uint64_t xmm_low_u64(const mdbg::CoreFloatingPointState& state,
                          std::size_t index) {
  require(index < state.xmm.size(), "XMM register index is out of range");
  std::uint64_t raw = 0;
  std::memcpy(&raw, state.xmm[index].data(), sizeof(raw));
  return raw;
}

void require_source_value(const mdbg::LocalScalarValue& value, double expected,
                          const std::string& context) {
  require(value.name == "xmm_value", context + " changed the source-value name");
  require(value.byte_size == sizeof(double), context + " changed the double width");
  require(value.raw_value == raw_bits(expected),
          context + " did not recover the compiler-owned XMM value");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_xmm_value_integration <core> <sibling-tid>\n";
    return 2;
  }

  try {
    const auto sibling_tid = static_cast<pid_t>(std::stol(argv[2]));
    mdbg::CoreInspectionSession session(argv[1]);
    const auto crash_tid = session.snapshot().crashed_tid();
    require(session.selected_thread_tid() == crash_tid,
            "core session did not start on the crashed thread");
    require(session.selected_frame_index() == 0,
            "core session did not start on the current frame");

    const auto crash_fp = session.snapshot().floating_point_state(crash_tid);
    const auto sibling_fp = session.snapshot().floating_point_state(sibling_tid);
    require(crash_fp.has_value(), "crashed thread lost its FPREGSET state");
    require(sibling_fp.has_value(), "sibling thread lost its FPREGSET state");
    require(xmm_low_u64(*crash_fp, 0) == raw_bits(kCrashValue),
            "crashed thread XMM0 does not contain the compiler-proven source value");
    require(xmm_low_u64(*sibling_fp, 0) == raw_bits(kSiblingValue),
            "sibling thread XMM0 does not contain the compiler-proven source value");

    require_source_value(session.inspect_value("xmm_value"), kCrashValue,
                         "crashed-thread frame 0");

    session.select_thread(sibling_tid);
    require(session.selected_thread_tid() == sibling_tid,
            "core thread selection did not select the sibling TID");
    require(session.selected_frame_index() == 0,
            "core thread selection did not reset to sibling frame 0");
    require_source_value(session.inspect_value("xmm_value"), kSiblingValue,
                         "sibling-thread frame 0");

    std::cout << "core XMM source-value integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "core XMM source-value integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

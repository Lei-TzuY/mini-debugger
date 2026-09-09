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
constexpr std::uint64_t kPointerPointeeValue = UINT64_C(0x8877665544332211);

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

void require_pointer_dereference(const mdbg::CoreInspectionSession& session) {
  const auto pointer = session.inspect_value("scalar_pointer");
  require(pointer.name == "scalar_pointer", "pointer lookup returned the wrong source name");
  require(pointer.kind == mdbg::LocalValueKind::Pointer,
          "core pointer source value lost pointer type identity");
  require(pointer.byte_size == sizeof(std::uintptr_t),
          "core pointer source value has the wrong pointer width");
  require(pointer.raw_value != 0, "compiler-produced core pointer unexpectedly resolved to null");
  require(pointer.pointee_type.has_value(),
          "core pointer source value lost its bounded pointee metadata");
  require(pointer.pointee_type->kind == mdbg::LocalValueKind::Integer &&
              pointer.pointee_type->byte_size == sizeof(std::uint64_t) &&
              !pointer.pointee_type->is_signed,
          "core pointer pointee metadata does not describe uint64_t");

  const auto dereferenced = session.dereference_value("scalar_pointer");
  require(dereferenced.name == "*scalar_pointer",
          "core pointer dereference changed the bounded source-value name");
  require(dereferenced.kind == mdbg::LocalValueKind::Integer &&
              dereferenced.byte_size == sizeof(std::uint64_t) &&
              !dereferenced.is_signed,
          "core pointer dereference lost the uint64_t pointee type");
  require(dereferenced.raw_value == kPointerPointeeValue,
          "core pointer dereference did not recover the genuine pointee value");
  require(dereferenced.storage == mdbg::LocalValueStorage::SnapshotCoreMemory ||
              dereferenced.storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact,
          "core pointer dereference bypassed snapshot-memory provenance");
  if (dereferenced.storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact) {
    require(!dereferenced.storage_module_path.empty() &&
                !dereferenced.storage_file_path.empty(),
            "artifact-backed pointer dereference lost runtime-artifact provenance");
  }
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
    require_pointer_dereference(session);

    session.select_thread(sibling_tid);
    require(session.selected_thread_tid() == sibling_tid,
            "core thread selection did not select the sibling TID");
    require(session.selected_frame_index() == 0,
            "core thread selection did not reset to sibling frame 0");
    require_source_value(session.inspect_value("xmm_value"), kSiblingValue,
                         "sibling-thread frame 0");

    std::cout << "core XMM/pointer source-value integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "core XMM/pointer source-value integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

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
constexpr std::uint64_t kStackLocalValue = UINT64_C(0x4f3e2d1c0b9a8877);
constexpr std::uint64_t kPointerPointeeValue = UINT64_C(0x8877665544332211);
constexpr std::uint64_t kAggregateFirst = UINT64_C(0x0123456789abcdef);
constexpr std::uint64_t kAggregateSecond = UINT64_C(0xfedcba9876543210);

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

void require_stack_local(const mdbg::CoreInspectionSession& session) {
  const auto value = session.inspect_value("stack_local");
  require(value.name == "stack_local", "stack-local lookup changed the source name");
  require(value.kind == mdbg::LocalValueKind::Integer &&
              value.byte_size == sizeof(std::uint64_t) && !value.is_signed,
          "stack-local lookup lost uint64_t type identity");
  require(value.raw_value == kStackLocalValue,
          "stack-local lookup did not recover the genuine core stack value");
  require(value.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
          "stack-local lookup did not preserve immutable core-memory provenance");
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

void require_aggregate_pointer_dereference(const mdbg::CoreInspectionSession& session) {
  const auto pointer = session.inspect_value("aggregate_pointer");
  require(pointer.name == "aggregate_pointer",
          "aggregate pointer lookup returned the wrong source name");
  require(pointer.kind == mdbg::LocalValueKind::Pointer,
          "aggregate pointer lost pointer type identity");
  require(pointer.byte_size == sizeof(std::uintptr_t) && pointer.raw_value != 0,
          "aggregate pointer did not recover a concrete x86-64 address");
  require(pointer.pointee_type.has_value(),
          "aggregate pointer lost bounded pointee metadata");
  require(pointer.pointee_type->kind == mdbg::LocalValueKind::Structure &&
              pointer.pointee_type->byte_size == 2 * sizeof(std::uint64_t),
          "aggregate pointer metadata does not describe the compiler-produced structure");

  const auto dereferenced = session.dereference_value("aggregate_pointer");
  require(dereferenced.name == "*aggregate_pointer",
          "aggregate pointer dereference changed the bounded source-value name");
  require(dereferenced.kind == mdbg::LocalValueKind::Structure &&
              dereferenced.byte_size == 2 * sizeof(std::uint64_t),
          "aggregate pointer dereference lost structure type identity");
  require(dereferenced.members.size() == 2,
          "aggregate pointer dereference did not reuse the bounded member decoder");
  require(dereferenced.members[0].name == "first" &&
              dereferenced.members[0].raw_value == kAggregateFirst &&
              dereferenced.members[0].byte_size == sizeof(std::uint64_t) &&
              !dereferenced.members[0].is_signed,
          "aggregate pointer first member was not recovered exactly");
  require(dereferenced.members[1].name == "second" &&
              dereferenced.members[1].raw_value == kAggregateSecond &&
              dereferenced.members[1].byte_size == sizeof(std::uint64_t) &&
              !dereferenced.members[1].is_signed,
          "aggregate pointer second member was not recovered exactly");
  require(dereferenced.storage == mdbg::LocalValueStorage::SnapshotCoreMemory ||
              dereferenced.storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact,
          "aggregate pointer dereference bypassed snapshot-memory provenance");
  if (dereferenced.storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact) {
    require(!dereferenced.storage_module_path.empty() &&
                !dereferenced.storage_file_path.empty(),
            "artifact-backed aggregate dereference lost runtime-artifact provenance");
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
    require_stack_local(session);
    require_pointer_dereference(session);
    require_aggregate_pointer_dereference(session);

    session.select_thread(sibling_tid);
    require(session.selected_thread_tid() == sibling_tid,
            "core thread selection did not select the sibling TID");
    require(session.selected_frame_index() == 0,
            "core thread selection did not reset to sibling frame 0");
    require_source_value(session.inspect_value("xmm_value"), kSiblingValue,
                         "sibling-thread frame 0");

    std::cout << "core XMM/stack/pointer source-value integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "core XMM/stack/pointer source-value integration failure: " << error.what()
              << '\n';
    return 1;
  }
  return 0;
}

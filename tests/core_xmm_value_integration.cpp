#include "snapshot/session.hpp"

#include <sys/wait.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr double kCrashValue = 1234.25;
constexpr double kSiblingValue = 9876.5;
constexpr std::uint64_t kStackLocalValue = UINT64_C(0x4f3e2d1c0b9a8877);
constexpr std::uint64_t kCallerStackLocalValue = UINT64_C(0xcafebabedeadbeef);
constexpr std::uint64_t kPointerPointeeValue = UINT64_C(0x8877665544332211);
constexpr std::uint64_t kAggregateFirst = UINT64_C(0x0123456789abcdef);
constexpr std::uint64_t kAggregateSecond = UINT64_C(0xfedcba9876543210);
constexpr std::uint64_t kTypedMarker = UINT64_C(0x13579bdf2468ace0);

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

std::string shell_quote(const std::string& text) {
  std::string result{"'"};
  for (const char ch : text) {
    if (ch == '\'') {
      result += "'\\''";
    } else {
      result += ch;
    }
  }
  result += '\'';
  return result;
}

std::string run_command(const std::string& command, const std::string& context) {
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) throw std::runtime_error("failed to launch " + context);
  std::string output;
  std::array<char, 512> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }
  const int status = ::pclose(pipe);
  require(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          context + " did not exit cleanly: " + output);
  return output;
}

void require_typed_object_oracle(const std::string& executable) {
  const auto output = run_command(
      "python3 tests/core_typed_object_dwarf_oracle.py " + shell_quote(executable) +
          " 2>&1",
      "typed-object DWARF oracle");
  require(output.find("typed-object DWARF oracle passed") != std::string::npos,
          "typed-object DWARF oracle did not report compiler-proven evidence");
}

std::string run_core_cli(const std::string& executable,
                         const std::string& core_path) {
  const std::string command =
      "printf 'print typed_pointer\\nderef typed_pointer\\nmember typed_pointer payload\\nderef-member typed_pointer payload\\nmember typed_pointer marker\\nbt\\nframe 1\\nlist\\nprint caller_stack_local\\nquit\\n' | " +
      shell_quote(executable) + " " + shell_quote(core_path) + " 2>&1";
  return run_command(command, "mdbg-core subprocess");
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

std::uintptr_t require_caller_stack_local(mdbg::CoreInspectionSession& session) {
  require(session.trace().frames.size() > 1,
          "genuine core did not recover the historical caller frame");
  session.select_frame(1);
  require(session.selected_frame_index() == 1,
          "core session did not select the historical caller frame");

  const auto& frame = session.selected_frame();
  const auto resume_pc = frame.runtime_pc;
  const auto lookup_pc = mdbg::snapshot_frame_lookup_pc(frame);
  require(lookup_pc + 1 == resume_pc,
          "historical lookup PC is not the compiler-proven caller call site");
  require(lookup_pc != resume_pc,
          "historical lookup PC did not remain distinct from immutable resume PC");
  const auto symbol = session.find_frame_symbol(frame);
  require(symbol.has_value() && symbol->name == "caller_with_stack_local",
          "frame-aware historical symbol lookup did not recover caller ownership");
  const auto source = session.find_frame_source(frame);
  require(source.has_value() && source->file.find("xmm_core_value_fixture.c") != std::string::npos,
          "frame-aware historical source lookup did not recover caller ownership");

  const auto value = session.inspect_value("caller_stack_local");
  require(value.name == "caller_stack_local",
          "caller stack-local lookup changed the source name");
  require(value.kind == mdbg::LocalValueKind::Integer &&
              value.byte_size == sizeof(std::uint64_t) && !value.is_signed,
          "caller stack-local lookup lost uint64_t type identity");
  require(value.raw_value == kCallerStackLocalValue,
          "caller stack-local lookup did not recover historical stack ownership");
  require(value.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
          "caller stack-local lookup did not preserve immutable core-memory provenance");
  return resume_pc;
}

void require_value_unavailable(const mdbg::CoreInspectionSession& session,
                               const std::string& name,
                               const std::string& context) {
  bool unavailable = false;
  try {
    (void)session.inspect_value(name);
  } catch (const std::exception&) {
    unavailable = true;
  }
  require(unavailable, context + " unexpectedly revived stale local ownership");
}

void require_stale_frame_rejected(const mdbg::CoreInspectionSession& session,
                                  const mdbg::SnapshotInspectionFrameContext& stale) {
  bool rejected = false;
  try {
    (void)session.find_frame_symbol(stale);
  } catch (const std::exception&) {
    rejected = true;
  }
  require(rejected, "frame-aware lookup accepted a stale frame from another thread selection");
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

void require_typed_pointer_evidence(const mdbg::CoreInspectionSession& session) {
  const auto pointer = session.inspect_value("typed_pointer");
  require(pointer.name == "typed_pointer",
          "typed pointer lookup returned the wrong source name");
  require(pointer.kind == mdbg::LocalValueKind::Pointer &&
              pointer.byte_size == sizeof(std::uintptr_t) && pointer.raw_value != 0,
          "typed pointer did not retain bounded pointer identity");
  require(pointer.pointee_type.has_value() &&
              pointer.pointee_type->kind == mdbg::LocalValueKind::Structure &&
              pointer.pointee_type->byte_size == 2 * sizeof(std::uint64_t),
          "typed pointer did not retain its compiler-proven structure pointee");
  require(pointer.pointee_type->members.size() == 2 &&
              pointer.pointee_type->members[0].name == "payload" &&
              pointer.pointee_type->members[0].kind == mdbg::LocalValueKind::Pointer &&
              pointer.pointee_type->members[0].pointee_type.has_value() &&
              pointer.pointee_type->members[0].pointee_type->byte_size ==
                  sizeof(std::uint64_t) &&
              pointer.pointee_type->members[1].name == "marker" &&
              pointer.pointee_type->members[1].kind == mdbg::LocalValueKind::Integer,
          "typed pointer did not retain the compiler-proven pointer-member graph");

  const auto object = session.dereference_value("typed_pointer");
  require(object.kind == mdbg::LocalValueKind::Structure &&
              object.byte_size == 2 * sizeof(std::uint64_t) && object.members.size() == 2,
          "typed pointer did not materialize the bounded containing structure");
  require(object.members[0].name == "payload" &&
              object.members[0].kind == mdbg::LocalValueKind::Pointer &&
              object.members[0].raw_value != 0 &&
              object.members[0].pointee_type.has_value(),
          "typed structure did not preserve its pointer-valued payload member");
  require(object.members[1].name == "marker" &&
              object.members[1].kind == mdbg::LocalValueKind::Integer &&
              object.members[1].raw_value == kTypedMarker,
          "typed structure did not recover its deterministic marker member");
  require(object.storage == mdbg::LocalValueStorage::SnapshotCoreMemory ||
              object.storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact,
          "typed structure object bypassed snapshot-memory provenance");
}

void require_typed_pointer_member_traversal(const mdbg::CoreInspectionSession& session) {
  const auto object = session.dereference_value("typed_pointer");
  const auto member = session.inspect_pointer_member("typed_pointer", "payload");
  require(member.name == "typed_pointer->payload",
          "pointer-member hop changed the bounded source-value name");
  require(member.kind == mdbg::LocalValueKind::Pointer &&
              member.byte_size == sizeof(std::uintptr_t) && !member.is_signed &&
              member.raw_value != 0,
          "pointer-member hop lost x86-64 pointer identity");
  require(member.pointee_type.has_value() &&
              member.pointee_type->kind == mdbg::LocalValueKind::Integer &&
              member.pointee_type->byte_size == sizeof(std::uint64_t) &&
              !member.pointee_type->is_signed,
          "pointer-member hop lost bounded uint64_t pointee metadata");
  require(member.storage == object.storage,
          "pointer-member hop lost containing-object provenance");
  if (member.storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact) {
    require(!member.storage_module_path.empty() && !member.storage_file_path.empty(),
            "artifact-backed pointer member lost runtime-artifact provenance");
  }

  const auto dereferenced =
      session.dereference_pointer_member("typed_pointer", "payload");
  require(dereferenced.name == "*(typed_pointer->payload)",
          "second bounded dereference changed the source-value name");
  require(dereferenced.kind == mdbg::LocalValueKind::Integer &&
              dereferenced.byte_size == sizeof(std::uint64_t) &&
              !dereferenced.is_signed,
          "second bounded dereference lost uint64_t type identity");
  require(dereferenced.raw_value == kPointerPointeeValue,
          "second bounded dereference did not recover the deterministic pointee");
  require(dereferenced.storage == mdbg::LocalValueStorage::SnapshotCoreMemory ||
              dereferenced.storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact,
          "second bounded dereference bypassed snapshot-memory provenance");
  if (dereferenced.storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact) {
    require(!dereferenced.storage_module_path.empty() &&
                !dereferenced.storage_file_path.empty(),
            "artifact-backed second dereference lost runtime-artifact provenance");
  }

  bool non_pointer_rejected = false;
  try {
    (void)session.inspect_pointer_member("typed_pointer", "marker");
  } catch (const std::exception&) {
    non_pointer_rejected = true;
  }
  require(non_pointer_rejected,
          "typed member traversal accepted a non-pointer direct member");

  bool unknown_member_rejected = false;
  try {
    (void)session.inspect_pointer_member("typed_pointer", "missing");
  } catch (const std::exception&) {
    unknown_member_rejected = true;
  }
  require(unknown_member_rejected,
          "typed member traversal accepted an unknown direct member");
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
    require(mdbg::snapshot_frame_lookup_pc(session.selected_frame()) ==
                session.selected_frame().runtime_pc,
            "frame-zero lookup PC was incorrectly normalized");
    require_typed_object_oracle(session.selected_frame().module_path);

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
    require_typed_pointer_evidence(session);
    require_typed_pointer_member_traversal(session);
    const auto caller_resume_pc = require_caller_stack_local(session);
    const auto stale_caller_frame = session.selected_frame();

    session.select_thread(sibling_tid);
    require(session.selected_thread_tid() == sibling_tid,
            "core thread selection did not select the sibling TID");
    require(session.selected_frame_index() == 0,
            "core thread selection did not reset to sibling frame 0");
    require_stale_frame_rejected(session, stale_caller_frame);
    require_value_unavailable(session, "caller_stack_local",
                              "sibling-thread frame selection");
    require_source_value(session.inspect_value("xmm_value"), kSiblingValue,
                         "sibling-thread frame 0");

    session.select_thread(crash_tid);
    require(session.selected_thread_tid() == crash_tid &&
                session.selected_frame_index() == 0,
            "returning to the crash thread did not reset historical frame selection");
    require_value_unavailable(session, "caller_stack_local",
                              "crash-thread frame-zero selection");
    const auto recovered_resume_pc = require_caller_stack_local(session);
    require(recovered_resume_pc == caller_resume_pc,
            "lookup-PC normalization silently changed immutable unwind sequencing");

    const auto core_cli =
        (std::filesystem::path(argv[0]).parent_path() / "mdbg-core").string();
    const auto cli_output = run_core_cli(core_cli, argv[1]);
    require(cli_output.find("typed_pointer->payload = 0x") != std::string::npos,
            "mdbg-core did not expose the explicit typed pointer-member hop");
    require(cli_output.find("*(typed_pointer->payload) = 0x8877665544332211") !=
                std::string::npos,
            "mdbg-core did not expose the second bounded typed dereference");
    require(cli_output.find("bounded structure member is not a supported pointer: marker") !=
                std::string::npos,
            "mdbg-core did not reject a non-pointer direct member deterministically");
    require(cli_output.find("selected frame 1") != std::string::npos,
            "mdbg-core did not expose historical frame selection");
    require(cli_output.find("caller_with_stack_local") != std::string::npos,
            "mdbg-core did not render frame-aware caller symbol ownership");
    require(cli_output.find("xmm_core_value_fixture.c") != std::string::npos,
            "mdbg-core did not render frame-aware caller source ownership");
    require(cli_output.find("caller_stack_local = 0xcafebabedeadbeef") !=
                std::string::npos,
            "mdbg-core did not render the historical caller stack local");
    require(cli_output.find("[value-core]") != std::string::npos,
            "mdbg-core lost immutable core-memory provenance for caller local");

    std::cout << "core XMM/stack/pointer/caller source-value integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "core XMM/stack/pointer/caller source-value integration failure: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}

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
constexpr std::uint64_t kCallerAggregateFirst = UINT64_C(0x1021324354657687);
constexpr std::uint64_t kCallerAggregateSecond = UINT64_C(0x89abcdef01234567);
constexpr std::int32_t kCallerNestedPrefix = INT32_C(0x11223344);
constexpr std::int32_t kCallerNestedTerminal = INT32_C(0x55667788);
constexpr std::int32_t kCallerArrayFirst = INT32_C(0x10203040);
constexpr std::int32_t kCallerArraySecond = INT32_C(0x22334455);
constexpr std::int32_t kCallerArrayThird = INT32_C(0x33445566);
constexpr std::uint32_t kCallerUnionValue = UINT32_C(0x44556677);
constexpr std::uint64_t kCallerTypedPayload = UINT64_C(0x7766554433221100);
constexpr std::uint64_t kCallerTypedMarker = UINT64_C(0x0badf00dcafed00d);
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

void require_physical_stack_aggregate_oracle(const std::string& executable) {
  const auto output = run_command(
      "python3 tests/core_physical_aggregate_dwarf_oracle.py " +
          shell_quote(executable) + " 2>&1",
      "physical stack aggregate DWARF oracle");
  require(output.find("physical stack aggregate DWARF oracle passed") !=
              std::string::npos,
          "physical stack aggregate oracle did not report compiler-proven evidence");
}

void require_physical_typed_aggregate_oracle(const std::string& executable) {
  const auto output = run_command(
      "python3 tests/core_physical_typed_aggregate_dwarf_oracle.py " +
          shell_quote(executable) + " 2>&1",
      "physical typed aggregate DWARF oracle");
  require(output.find("physical typed aggregate DWARF oracle passed") !=
              std::string::npos,
          "physical typed aggregate oracle did not report compiler-proven evidence");
}

void require_physical_nested_aggregate_oracle(const std::string& executable) {
  const auto output = run_command(
      "python3 tests/core_physical_nested_aggregate_dwarf_oracle.py " +
          shell_quote(executable) + " 2>&1",
      "physical nested aggregate DWARF oracle");
  require(output.find("physical nested aggregate DWARF oracle passed") !=
              std::string::npos,
          "physical nested aggregate oracle did not report compiler-proven evidence");
}

void require_physical_fixed_array_oracle(const std::string& executable) {
  const auto output = run_command(
      "python3 tests/core_physical_array_dwarf_oracle.py " +
          shell_quote(executable) + " 2>&1",
      "physical fixed-array DWARF oracle");
  require(output.find("physical fixed-array DWARF oracle passed") !=
              std::string::npos,
          "physical fixed-array oracle did not report compiler-proven evidence");
}

void require_physical_union_oracle(const std::string& executable) {
  const auto output = run_command(
      "python3 tests/core_physical_union_dwarf_oracle.py " +
          shell_quote(executable) + " 2>&1",
      "physical union DWARF oracle");
  require(output.find("physical union DWARF oracle passed") !=
              std::string::npos,
          "physical union oracle did not report compiler-proven evidence");
}

void require_physical_bitfield_oracle(const std::string& executable) {
  const auto output = run_command(
      "python3 tests/core_physical_bitfield_dwarf_oracle.py " +
          shell_quote(executable) + " 2>&1",
      "physical bit-field DWARF oracle");
  require(output.find("physical bit-field DWARF oracle passed") !=
              std::string::npos,
          "physical bit-field oracle did not report compiler-proven evidence");
}

void require_physical_enum_oracle(const std::string& executable) {
  const auto output = run_command(
      "python3 tests/core_physical_enum_dwarf_oracle.py " +
          shell_quote(executable) + " 2>&1",
      "physical enum DWARF oracle");
  require(output.find("physical enum DWARF oracle passed") !=
              std::string::npos,
          "physical enum oracle did not report compiler-proven evidence");
}

void require_physical_enum_aggregate_oracle(const std::string& executable) {
  const auto output = run_command(
      "python3 tests/core_physical_enum_aggregate_dwarf_oracle.py " +
          shell_quote(executable) + " 2>&1",
      "physical enum aggregate DWARF oracle");
  require(output.find("physical enum aggregate DWARF oracle passed") !=
              std::string::npos,
          "physical enum aggregate oracle did not report compiler-proven evidence");
}

std::string run_core_cli(const std::string& executable,
                         const std::string& core_path) {
  const std::string command =
      "printf 'print typed_pointer\\nderef typed_pointer\\nmember typed_pointer payload\\nderef-member typed_pointer payload\\nmember typed_pointer marker\\nbt\\nframe 1\\nlist\\nprint caller_stack_local\\nprint caller_stack_aggregate\\naggregate-member caller_stack_aggregate second\\nprint caller_nested_aggregate\\nnested-aggregate-member caller_nested_aggregate inner terminal\\nprint caller_fixed_array\\narray-element caller_fixed_array 1\\narray-element caller_fixed_array 3\\nprint caller_union\\nunion-member caller_union signed_value\\nunion-member caller_union unsigned_value\\nunion-member caller_union missing\\nprint caller_bit_fields\\naggregate-member caller_bit_fields signed_bits\\naggregate-member caller_bit_fields unsigned_bits\\nprint caller_mode\\nprint caller_enum_aggregate\\naggregate-member caller_enum_aggregate mode\\nprint caller_typed_aggregate\\naggregate-member caller_typed_aggregate payload\\nderef-aggregate-member caller_typed_aggregate payload\\nquit\\n' | " +
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

void require_caller_stack_aggregate(const mdbg::CoreInspectionSession& session) {
  require(session.selected_frame_index() == 1,
          "physical stack aggregate requires the historical caller frame");
  const auto aggregate = session.inspect_value("caller_stack_aggregate");
  require(aggregate.name == "caller_stack_aggregate" &&
              aggregate.kind == mdbg::LocalValueKind::Structure &&
              aggregate.byte_size == 2 * sizeof(std::uint64_t),
          "historical physical aggregate lost bounded structure identity");
  require(aggregate.members.size() == 2,
          "historical physical aggregate changed its direct member count");
  require(aggregate.members[0].name == "first" &&
              aggregate.members[0].raw_value == kCallerAggregateFirst &&
              aggregate.members[0].byte_size == sizeof(std::uint64_t) &&
              !aggregate.members[0].is_signed && aggregate.members[0].offset == 0,
          "historical physical aggregate first member was not recovered exactly");
  require(aggregate.members[1].name == "second" &&
              aggregate.members[1].raw_value == kCallerAggregateSecond &&
              aggregate.members[1].byte_size == sizeof(std::uint64_t) &&
              !aggregate.members[1].is_signed &&
              aggregate.members[1].offset == sizeof(std::uint64_t),
          "historical physical aggregate second member was not recovered exactly");
  require(aggregate.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
          "historical physical aggregate lost immutable core-memory provenance");

  const auto member =
      session.inspect_aggregate_member("caller_stack_aggregate", "second");
  require(member.name == "caller_stack_aggregate.second" &&
              member.kind == mdbg::LocalValueKind::Integer &&
              member.raw_value == kCallerAggregateSecond &&
              member.byte_size == sizeof(std::uint64_t) && !member.is_signed,
          "physical aggregate member selection did not recover the second member");
  require(member.storage == aggregate.storage,
          "physical aggregate member selection changed immutable provenance");
}

void require_caller_nested_aggregate(
    const mdbg::CoreInspectionSession& session) {
  require(session.selected_frame_index() == 1,
          "physical nested aggregate requires the historical caller frame");
  const auto aggregate = session.inspect_value("caller_nested_aggregate");
  require(aggregate.name == "caller_nested_aggregate" &&
              aggregate.kind == mdbg::LocalValueKind::Structure &&
              aggregate.byte_size == 8 && aggregate.members.size() == 2,
          "historical physical nested aggregate lost outer structure identity");

  const auto& prefix = aggregate.members[0];
  require(prefix.name == "prefix" &&
              prefix.kind == mdbg::LocalValueKind::Integer &&
              prefix.byte_size == sizeof(std::int32_t) && prefix.is_signed &&
              prefix.raw_value == static_cast<std::uint32_t>(kCallerNestedPrefix) &&
              prefix.offset == 0,
          "physical nested aggregate prefix was not recovered exactly");

  const auto& inner = aggregate.members[1];
  require(inner.name == "inner" &&
              inner.kind == mdbg::LocalValueKind::Structure &&
              inner.byte_size == sizeof(std::int32_t) && !inner.is_signed &&
              inner.offset == sizeof(std::int32_t) &&
              inner.members.size() == 1,
          "physical nested aggregate lost bounded inner structure metadata");
  const auto& terminal = inner.members.front();
  require(terminal.name == "terminal" &&
              terminal.kind == mdbg::LocalValueKind::Integer &&
              terminal.byte_size == sizeof(std::int32_t) &&
              terminal.is_signed && terminal.offset == 0 &&
              terminal.raw_value ==
                  static_cast<std::uint32_t>(kCallerNestedTerminal),
          "physical nested aggregate terminal was not recovered exactly");
  require(aggregate.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
          "physical nested aggregate lost immutable core provenance");

  const auto selected = session.inspect_nested_aggregate_member(
      "caller_nested_aggregate", "inner", "terminal");
  require(selected.name == "caller_nested_aggregate.inner.terminal" &&
              selected.kind == mdbg::LocalValueKind::Integer &&
              selected.byte_size == sizeof(std::int32_t) &&
              selected.is_signed &&
              selected.raw_value ==
                  static_cast<std::uint32_t>(kCallerNestedTerminal),
          "physical nested aggregate traversal did not recover terminal");
  require(selected.storage == aggregate.storage,
          "physical nested aggregate traversal changed immutable provenance");
}

void require_caller_fixed_array(
    const mdbg::CoreInspectionSession& session) {
  require(session.selected_frame_index() == 1,
          "physical fixed array requires the historical caller frame");
  const auto array = session.inspect_value("caller_fixed_array");
  require(array.name == "caller_fixed_array" &&
              array.kind == mdbg::LocalValueKind::Array &&
              array.byte_size == 3 * sizeof(std::int32_t) &&
              array.array_type.has_value(),
          "historical physical fixed array lost bounded array identity");
  require(array.array_type->element_count == 3 &&
              array.array_type->element_byte_size == sizeof(std::int32_t) &&
              array.array_type->element_is_signed &&
              array.array_type->element_kind == mdbg::LocalValueKind::Integer,
          "historical physical fixed array metadata does not match compiler evidence");
  require(array.elements.size() == 3 &&
              array.elements[0].raw_value ==
                  static_cast<std::uint32_t>(kCallerArrayFirst) &&
              array.elements[1].raw_value ==
                  static_cast<std::uint32_t>(kCallerArraySecond) &&
              array.elements[2].raw_value ==
                  static_cast<std::uint32_t>(kCallerArrayThird),
          "historical physical fixed array elements were not recovered exactly");
  require(array.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
          "historical physical fixed array lost immutable core provenance");

  const auto middle = session.inspect_array_element("caller_fixed_array", 1);
  require(middle.name == "caller_fixed_array[1]" &&
              middle.kind == mdbg::LocalValueKind::Integer &&
              middle.byte_size == sizeof(std::int32_t) &&
              middle.is_signed &&
              middle.raw_value ==
                  static_cast<std::uint32_t>(kCallerArraySecond),
          "physical fixed-array index 1 was not materialized exactly");
  require(middle.storage == array.storage,
          "physical fixed-array indexing changed immutable provenance");

  bool rejected = false;
  try {
    (void)session.inspect_array_element("caller_fixed_array", 3);
  } catch (const std::out_of_range&) {
    rejected = true;
  }
  require(rejected, "physical fixed-array out-of-range index was accepted");
}

void require_caller_union(
    const mdbg::CoreInspectionSession& session) {
  require(session.selected_frame_index() == 1,
          "physical union requires the historical caller frame");
  const auto value = session.inspect_value("caller_union");
  require(value.name == "caller_union" &&
              value.kind == mdbg::LocalValueKind::Union &&
              value.byte_size == sizeof(std::uint32_t) &&
              value.members.size() == 2,
          "historical physical union lost bounded overlapping identity");
  require(value.members[0].name == "signed_value" &&
              value.members[0].offset == 0 &&
              value.members[0].byte_size == sizeof(std::int32_t) &&
              value.members[0].is_signed &&
              value.members[1].name == "unsigned_value" &&
              value.members[1].offset == 0 &&
              value.members[1].byte_size == sizeof(std::uint32_t) &&
              !value.members[1].is_signed,
          "historical physical union member metadata does not match compiler evidence");
  require(value.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
          "historical physical union lost immutable core provenance");

  const auto signed_view =
      session.inspect_union_member("caller_union", "signed_value");
  require(signed_view.name == "caller_union.signed_value" &&
              signed_view.kind == mdbg::LocalValueKind::Integer &&
              signed_view.raw_value == kCallerUnionValue &&
              signed_view.byte_size == sizeof(std::int32_t) &&
              signed_view.is_signed,
          "physical signed union member view was not recovered exactly");
  require(signed_view.storage == value.storage,
          "physical signed union member changed immutable provenance");

  const auto unsigned_view =
      session.inspect_union_member("caller_union", "unsigned_value");
  require(unsigned_view.name == "caller_union.unsigned_value" &&
              unsigned_view.kind == mdbg::LocalValueKind::Integer &&
              unsigned_view.raw_value == kCallerUnionValue &&
              unsigned_view.byte_size == sizeof(std::uint32_t) &&
              !unsigned_view.is_signed,
          "physical unsigned union member view was not recovered exactly");
  require(unsigned_view.storage == value.storage,
          "physical unsigned union member changed immutable provenance");

  bool missing_rejected = false;
  try {
    (void)session.inspect_union_member("caller_union", "missing");
  } catch (const std::runtime_error&) {
    missing_rejected = true;
  }
  require(missing_rejected, "physical missing union member was accepted");

  bool aggregate_alias_rejected = false;
  try {
    (void)session.inspect_aggregate_member("caller_union", "signed_value");
  } catch (const std::runtime_error&) {
    aggregate_alias_rejected = true;
  }
  require(aggregate_alias_rejected,
          "physical union leaked through the structure-only aggregate selector");
}

void require_caller_bit_fields(
    const mdbg::CoreInspectionSession& session) {
  require(session.selected_frame_index() == 1,
          "physical bit fields require the historical caller frame");
  const auto aggregate = session.inspect_value("caller_bit_fields");
  require(aggregate.name == "caller_bit_fields" &&
              aggregate.kind == mdbg::LocalValueKind::Structure &&
              aggregate.byte_size == sizeof(std::uint32_t) &&
              aggregate.members.size() == 2,
          "historical physical bit-field aggregate lost bounded structure identity");
  require(aggregate.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
          "historical physical bit-field aggregate lost immutable core provenance");

  const auto& signed_member = aggregate.members[0];
  require(signed_member.name == "signed_bits" &&
              signed_member.kind == mdbg::LocalValueKind::Integer &&
              signed_member.byte_size == sizeof(std::int32_t) &&
              signed_member.is_signed &&
              signed_member.bit_slice.has_value() &&
              signed_member.bit_slice->bit_size == 5 &&
              signed_member.raw_value == UINT64_C(0xfffffff9),
          "historical physical signed bit field was not normalized to int32 -7");
  const auto& unsigned_member = aggregate.members[1];
  require(unsigned_member.name == "unsigned_bits" &&
              unsigned_member.kind == mdbg::LocalValueKind::Integer &&
              unsigned_member.byte_size == sizeof(std::uint32_t) &&
              !unsigned_member.is_signed &&
              unsigned_member.bit_slice.has_value() &&
              unsigned_member.bit_slice->bit_size == 6 &&
              unsigned_member.raw_value == UINT64_C(0x29),
          "historical physical unsigned bit field was not normalized to uint32 41");

  const auto selected_signed =
      session.inspect_aggregate_member("caller_bit_fields", "signed_bits");
  require(selected_signed.name == "caller_bit_fields.signed_bits" &&
              selected_signed.raw_value == UINT64_C(0xfffffff9) &&
              selected_signed.is_signed &&
              selected_signed.byte_size == sizeof(std::int32_t) &&
              selected_signed.storage == aggregate.storage,
          "physical signed bit-field selection lost value/provenance");

  const auto selected_unsigned =
      session.inspect_aggregate_member("caller_bit_fields", "unsigned_bits");
  require(selected_unsigned.name == "caller_bit_fields.unsigned_bits" &&
              selected_unsigned.raw_value == UINT64_C(0x29) &&
              !selected_unsigned.is_signed &&
              selected_unsigned.byte_size == sizeof(std::uint32_t) &&
              selected_unsigned.storage == aggregate.storage,
          "physical unsigned bit-field selection lost value/provenance");
}

void require_caller_mode(
    const mdbg::CoreInspectionSession& session) {
  require(session.selected_frame_index() == 1,
          "physical enum requires the historical caller frame");
  const auto value = session.inspect_value("caller_mode");
  require(value.name == "caller_mode" &&
              value.kind == mdbg::LocalValueKind::Enumeration &&
              value.byte_size == sizeof(std::uint32_t) &&
              !value.is_signed && value.raw_value == UINT64_C(42),
          "historical physical enum lost compiler-described value identity");
  require(value.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
          "historical physical enum lost immutable core provenance");
  require(value.enum_type.has_value(),
          "historical physical enum lost bounded type metadata");
  require(value.enum_type->name == "CallerPhysicalMode" &&
              value.enum_type->byte_size == sizeof(std::uint32_t) &&
              !value.enum_type->is_signed &&
              value.enum_type->enumerators.size() == 3,
          "historical physical enum representation metadata is incorrect");

  const auto has_entry = [&](const std::string& name, std::uint64_t raw) {
    for (const auto& entry : value.enum_type->enumerators) {
      if (entry.name == name && entry.raw_value == raw) return true;
    }
    return false;
  };
  require(has_entry("CallerPhysicalIdle", 3) &&
              has_entry("CallerPhysicalReady", 7) &&
              has_entry("CallerPhysicalBusy", 42),
          "historical physical enum lost the compiler enumerator table");
  const auto symbol = mdbg::local_enum_symbol(value);
  require(symbol && *symbol == "CallerPhysicalBusy",
          "historical physical enum raw value did not map to its exact symbol");

  auto unknown = value;
  unknown.raw_value = 11;
  require(!mdbg::local_enum_symbol(unknown),
          "unknown physical enum value must remain numeric");
  auto ambiguous = value;
  ambiguous.enum_type->enumerators.push_back({"CallerPhysicalBusyAlias", 42});
  require(!mdbg::local_enum_symbol(ambiguous),
          "duplicate physical enum aliases must remain symbolically ambiguous");
}

void require_caller_enum_aggregate(
    const mdbg::CoreInspectionSession& session) {
  require(session.selected_frame_index() == 1,
          "physical enum aggregate requires the historical caller frame");
  const auto aggregate = session.inspect_value("caller_enum_aggregate");
  require(aggregate.name == "caller_enum_aggregate" &&
              aggregate.kind == mdbg::LocalValueKind::Structure &&
              aggregate.byte_size == 2 * sizeof(std::uint32_t) &&
              aggregate.members.size() == 2,
          "historical physical enum aggregate lost bounded structure identity");
  require(aggregate.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
          "historical physical enum aggregate lost immutable core provenance");

  const auto& direct = aggregate.members[0];
  require(direct.name == "direct" && direct.offset == 0 &&
              direct.kind == mdbg::LocalValueKind::Integer &&
              direct.byte_size == sizeof(std::int32_t) && direct.is_signed &&
              direct.raw_value == UINT64_C(0x31415926),
          "physical enum aggregate integer member changed unexpectedly");

  const auto& mode = aggregate.members[1];
  require(mode.name == "mode" && mode.offset == sizeof(std::int32_t) &&
              mode.kind == mdbg::LocalValueKind::Enumeration &&
              mode.byte_size == sizeof(std::uint32_t) && !mode.is_signed &&
              mode.raw_value == UINT64_C(42) && mode.enum_type.has_value(),
          "physical enum aggregate member lost enum identity");
  require(mode.enum_type->name == "CallerPhysicalMode" &&
              mode.enum_type->enumerators.size() == 3,
          "physical enum aggregate member lost bounded enum metadata");

  const auto selected =
      session.inspect_aggregate_member("caller_enum_aggregate", "mode");
  require(selected.name == "caller_enum_aggregate.mode" &&
              selected.kind == mdbg::LocalValueKind::Enumeration &&
              selected.raw_value == UINT64_C(42) &&
              selected.enum_type.has_value() &&
              selected.storage == aggregate.storage,
          "physical enum aggregate selection lost value/provenance");
  const auto symbol = mdbg::local_enum_symbol(selected);
  require(symbol && *symbol == "CallerPhysicalBusy",
          "physical enum aggregate selected member lost exact symbolic identity");
}

void require_caller_typed_aggregate(
    const mdbg::CoreInspectionSession& session) {
  require(session.selected_frame_index() == 1,
          "physical typed aggregate requires the historical caller frame");
  const auto aggregate = session.inspect_value("caller_typed_aggregate");
  require(aggregate.name == "caller_typed_aggregate" &&
              aggregate.kind == mdbg::LocalValueKind::Structure &&
              aggregate.byte_size == 2 * sizeof(std::uint64_t) &&
              aggregate.members.size() == 2,
          "historical physical typed aggregate lost bounded structure identity");

  const auto& payload = aggregate.members[0];
  require(payload.name == "payload" &&
              payload.kind == mdbg::LocalValueKind::Pointer &&
              payload.byte_size == sizeof(std::uintptr_t) &&
              payload.raw_value != 0 && payload.offset == 0 &&
              payload.pointee_type.has_value() &&
              payload.pointee_type->byte_size == sizeof(std::uint64_t) &&
              !payload.pointee_type->is_signed,
          "physical typed aggregate lost pointer-member metadata");
  const auto& marker = aggregate.members[1];
  require(marker.name == "marker" &&
              marker.kind == mdbg::LocalValueKind::Integer &&
              marker.raw_value == kCallerTypedMarker &&
              marker.byte_size == sizeof(std::uint64_t) &&
              !marker.is_signed && marker.offset == sizeof(std::uint64_t),
          "physical typed aggregate marker was not recovered exactly");
  require(aggregate.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
          "physical typed aggregate lost immutable core provenance");

  const auto selected =
      session.inspect_aggregate_member("caller_typed_aggregate", "payload");
  require(selected.name == "caller_typed_aggregate.payload" &&
              selected.kind == mdbg::LocalValueKind::Pointer &&
              selected.raw_value == payload.raw_value &&
              selected.pointee_type.has_value(),
          "context-neutral aggregate selection lost pointer-member identity");
  require(selected.storage == aggregate.storage,
          "context-neutral aggregate selection changed member provenance");

  const auto dereferenced =
      session.dereference_aggregate_member("caller_typed_aggregate", "payload");
  require(dereferenced.name == "*(caller_typed_aggregate.payload)" &&
              dereferenced.kind == mdbg::LocalValueKind::Integer &&
              dereferenced.byte_size == sizeof(std::uint64_t) &&
              !dereferenced.is_signed &&
              dereferenced.raw_value == kCallerTypedPayload,
          "context-neutral aggregate dereference lost the historical pointee");
  require(dereferenced.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
          "context-neutral aggregate dereference bypassed immutable core memory");
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
    require_physical_stack_aggregate_oracle(
        session.selected_frame().module_path);
    require_physical_typed_aggregate_oracle(
        session.selected_frame().module_path);
    require_physical_nested_aggregate_oracle(
        session.selected_frame().module_path);
    require_physical_fixed_array_oracle(
        session.selected_frame().module_path);
    require_physical_union_oracle(
        session.selected_frame().module_path);
    require_physical_bitfield_oracle(
        session.selected_frame().module_path);
    require_physical_enum_oracle(
        session.selected_frame().module_path);
    require_physical_enum_aggregate_oracle(
        session.selected_frame().module_path);

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
    require_caller_stack_aggregate(session);
    require_caller_nested_aggregate(session);
    require_caller_fixed_array(session);
    require_caller_union(session);
    require_caller_bit_fields(session);
    require_caller_mode(session);
    require_caller_enum_aggregate(session);
    require_caller_typed_aggregate(session);
    const auto stale_caller_frame = session.selected_frame();

    session.select_thread(sibling_tid);
    require(session.selected_thread_tid() == sibling_tid,
            "core thread selection did not select the sibling TID");
    require(session.selected_frame_index() == 0,
            "core thread selection did not reset to sibling frame 0");
    require_stale_frame_rejected(session, stale_caller_frame);
    require_value_unavailable(session, "caller_stack_local",
                              "sibling-thread frame selection");
    require_value_unavailable(session, "caller_stack_aggregate",
                              "sibling-thread physical aggregate selection");
    require_value_unavailable(session, "caller_nested_aggregate",
                              "sibling-thread nested aggregate selection");
    require_value_unavailable(session, "caller_fixed_array",
                              "sibling-thread fixed-array selection");
    require_value_unavailable(session, "caller_union",
                              "sibling-thread union selection");
    require_value_unavailable(session, "caller_bit_fields",
                              "sibling-thread bit-field selection");
    require_value_unavailable(session, "caller_mode",
                              "sibling-thread enum selection");
    require_value_unavailable(session, "caller_enum_aggregate",
                              "sibling-thread enum aggregate selection");
    require_value_unavailable(session, "caller_typed_aggregate",
                              "sibling-thread typed aggregate selection");
    require_source_value(session.inspect_value("xmm_value"), kSiblingValue,
                         "sibling-thread frame 0");

    session.select_thread(crash_tid);
    require(session.selected_thread_tid() == crash_tid &&
                session.selected_frame_index() == 0,
            "returning to the crash thread did not reset historical frame selection");
    require_value_unavailable(session, "caller_stack_local",
                              "crash-thread frame-zero selection");
    require_value_unavailable(session, "caller_stack_aggregate",
                              "crash-thread frame-zero aggregate selection");
    require_value_unavailable(session, "caller_nested_aggregate",
                              "crash-thread frame-zero nested aggregate selection");
    require_value_unavailable(session, "caller_fixed_array",
                              "crash-thread frame-zero fixed-array selection");
    require_value_unavailable(session, "caller_union",
                              "crash-thread frame-zero union selection");
    require_value_unavailable(session, "caller_bit_fields",
                              "crash-thread frame-zero bit-field selection");
    require_value_unavailable(session, "caller_mode",
                              "crash-thread frame-zero enum selection");
    require_value_unavailable(session, "caller_enum_aggregate",
                              "crash-thread frame-zero enum aggregate selection");
    require_value_unavailable(session, "caller_typed_aggregate",
                              "crash-thread frame-zero typed aggregate selection");
    const auto recovered_resume_pc = require_caller_stack_local(session);
    require_caller_stack_aggregate(session);
    require_caller_nested_aggregate(session);
    require_caller_fixed_array(session);
    require_caller_union(session);
    require_caller_bit_fields(session);
    require_caller_mode(session);
    require_caller_enum_aggregate(session);
    require_caller_typed_aggregate(session);
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
    require(cli_output.find(
                "caller_stack_aggregate = { first=0x1021324354657687, second=0x89abcdef01234567 }") !=
                std::string::npos,
            "mdbg-core did not render the historical physical stack aggregate");
    require(cli_output.find(
                "caller_stack_aggregate.second = 0x89abcdef01234567") !=
                std::string::npos,
            "mdbg-core did not expose physical aggregate member selection");
    require(cli_output.find(
                "caller_nested_aggregate.inner.terminal = 0x55667788") !=
                std::string::npos,
            "mdbg-core did not expose physical nested aggregate traversal");
    require(cli_output.find(
                "caller_fixed_array = [0x10203040, 0x22334455, 0x33445566]") !=
                std::string::npos,
            "mdbg-core did not render the historical physical fixed array");
    require(cli_output.find("caller_fixed_array[1] = 0x22334455") !=
                std::string::npos,
            "mdbg-core did not render physical fixed-array index 1");
    require(cli_output.find("array index is out of range") != std::string::npos,
            "mdbg-core did not reject the physical fixed-array out-of-range index");
    require(cli_output.find(
                "caller_union = union{signed_value, unsigned_value}") !=
                std::string::npos,
            "mdbg-core did not render the historical physical union");
    require(cli_output.find("caller_union.signed_value = 0x44556677") !=
                std::string::npos,
            "mdbg-core did not render the physical signed union member");
    require(cli_output.find("caller_union.unsigned_value = 0x44556677") !=
                std::string::npos,
            "mdbg-core did not render the physical unsigned union member");
    require(cli_output.find("has no member named: missing") != std::string::npos,
            "mdbg-core did not deterministically reject a missing physical union member");
    require(cli_output.find(
                "caller_bit_fields = { signed_bits=0xfffffff9, unsigned_bits=0x29 }") !=
                std::string::npos,
            "mdbg-core did not render historical physical bit fields");
    require(cli_output.find("caller_bit_fields.signed_bits = 0xfffffff9") !=
                std::string::npos,
            "mdbg-core did not select the physical signed bit field");
    require(cli_output.find("caller_bit_fields.unsigned_bits = 0x29") !=
                std::string::npos,
            "mdbg-core did not select the physical unsigned bit field");
    require(cli_output.find(
                "caller_mode = CallerPhysicalMode::CallerPhysicalBusy (0x2a)") !=
                std::string::npos,
            "mdbg-core did not render the historical physical enum symbol");
    require(cli_output.find("[4-byte enum unsigned]") != std::string::npos,
            "mdbg-core did not render the physical enum representation");
    require(cli_output.find(
                "caller_enum_aggregate = { direct=0x31415926, mode=0x2a }") !=
                std::string::npos,
            "mdbg-core did not render the historical physical enum aggregate");
    require(cli_output.find(
                "caller_enum_aggregate.mode = CallerPhysicalMode::CallerPhysicalBusy (0x2a)") !=
                std::string::npos,
            "mdbg-core did not render selected physical enum aggregate member identity");
    require(cli_output.find("caller_typed_aggregate = { payload=0x") !=
                std::string::npos,
            "mdbg-core did not render the physical typed aggregate");
    require(cli_output.find("caller_typed_aggregate.payload = 0x") !=
                std::string::npos,
            "mdbg-core did not expose context-neutral pointer-member selection");
    require(cli_output.find(
                "*(caller_typed_aggregate.payload) = 0x7766554433221100") !=
                std::string::npos,
            "mdbg-core did not expose context-neutral aggregate dereference");
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

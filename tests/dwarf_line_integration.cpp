#include "debugger/debugger.hpp"
#include "dwarf/inline_member.hpp"
#include "dwarf/line_table.hpp"
#include "dwarf/local_value.hpp"
#include "elf/elf.hpp"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint64_t kExpectedLocalRawValue = 0x1020304050607080ULL;
constexpr std::uint64_t kExpectedOuterLocalRawValue = 0xe1c2e384e5c6e708ULL;
constexpr std::uint64_t kExpectedOptimizedLocalRawValue = 0x1e3c1e781e3c1ef0ULL;
constexpr const char* kExpectedLocalValue = "local_value = 1161981756646125696";
constexpr const char* kExpectedOuterLocalValue =
    "local_value = 16267814963945858824";
constexpr const char* kExpectedParameterValue = "parameter = 1161981756646125696";
constexpr const char* kExpectedOptimizedLocalValue =
    "optimized_local = 2178649820992642800";
constexpr const char* kExpectedLiveEnumValue =
    "live_mode = LiveMode::LiveBusy (0x2a)";

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_mapped_source(const mdbg::SourceLocation& location) {
  require(std::filesystem::path(location.file).filename() == "mapped_source.c",
          "line table returned an unexpected source file: " + location.file);
  require(location.line == 400, "line_probe must map to synthetic source line 400");
}

void test_runtime_mapping(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  require(lines.available(), "DWARF fixture should expose line ranges");

  const auto probe = elf.find_symbol("line_probe");
  require(probe.has_value(), "line_probe symbol missing from fixture");

  const auto virtual_location = lines.find_virtual_address(probe->value);
  require(virtual_location.has_value(), "virtual line lookup failed for line_probe");
  require_mapped_source(*virtual_location);

  const auto reverse_virtual = lines.find_virtual_source("mapped_source.c", 400);
  require(reverse_virtual.has_value(), "reverse source lookup failed for mapped_source.c:400");
  const auto reverse_virtual_location = lines.find_virtual_address(*reverse_virtual);
  require(reverse_virtual_location.has_value(),
          "reverse source address did not map back to a source location");
  require_mapped_source(*reverse_virtual_location);
  const auto reverse_symbol = elf.find_symbol_by_virtual_address(*reverse_virtual);
  require(reverse_symbol.has_value() && reverse_symbol->symbol.name == "line_probe",
          "reverse source lookup did not select an address inside line_probe");
  require(!lines.find_virtual_source("mapped_source.c", 9999).has_value(),
          "missing source line must not resolve to an address");

  const auto runtime_address = elf.runtime_address(debugger.pid(), *probe);
  const auto runtime_location =
      lines.find_runtime_address(debugger.pid(), runtime_address, elf);
  require(runtime_location.has_value(), "runtime line lookup failed for line_probe");
  require_mapped_source(*runtime_location);

  const auto reverse_runtime =
      lines.find_runtime_source(debugger.pid(), "mapped_source.c", 400, elf);
  require(reverse_runtime.has_value(),
          "runtime reverse source lookup failed for mapped_source.c:400");
  const auto reverse_runtime_location =
      lines.find_runtime_address(debugger.pid(), *reverse_runtime, elf);
  require(reverse_runtime_location.has_value(),
          "runtime reverse source address did not map back to a source location");
  require_mapped_source(*reverse_runtime_location);
  const auto reverse_runtime_symbol =
      elf.find_symbol_by_runtime_address(debugger.pid(), *reverse_runtime);
  require(reverse_runtime_symbol.has_value() &&
              reverse_runtime_symbol->symbol.name == "line_probe",
          "runtime reverse source lookup did not select line_probe");

  debugger.add_breakpoint(static_cast<std::uintptr_t>(*reverse_runtime));
  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == *reverse_runtime,
          "source-derived breakpoint was not hit");
  const auto repaired_rip = debugger.registers().rip;
  require(repaired_rip == *reverse_runtime,
          "source-derived managed breakpoint did not expose repaired RIP");
  const auto stopped_location =
      lines.find_runtime_address(debugger.pid(), repaired_rip, elf);
  require(stopped_location.has_value(), "stopped RIP did not resolve to a source line");
  require_mapped_source(*stopped_location);
}

void test_local_value_api(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {"value"});
  const mdbg::ElfFile elf(fixture);
  const auto outer_before_probe = elf.find_symbol("outer_local_before_probe");
  const auto inner_probe = elf.find_symbol("local_value_probe");
  const auto outer_after_probe = elf.find_symbol("outer_local_after_probe");
  require(outer_before_probe && inner_probe && outer_after_probe,
          "lexical-scope probe symbols are missing from fixture");

  const auto outer_before = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *outer_before_probe));
  const auto inner = static_cast<std::uintptr_t>(elf.runtime_address(debugger.pid(), *inner_probe));
  const auto outer_after = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *outer_after_probe));
  require(outer_before < inner && inner < outer_after,
          "lexical-scope probes must execute outer-before, inner, outer-after");

  debugger.add_breakpoint(outer_before);
  debugger.add_breakpoint(inner);
  debugger.add_breakpoint(outer_after);

  auto require_local_value = [&](std::uint64_t expected, const char* context) {
    const auto value = mdbg::inspect_local_integer(debugger, elf, "local_value");
    require(value.name == "local_value", std::string(context) + " returned the wrong name");
    require(value.raw_value == expected, std::string(context) + " returned the wrong value");
    require(value.byte_size == sizeof(std::uint64_t) && !value.is_signed,
            std::string(context) + " returned the wrong uint64_t type metadata");
    require(std::filesystem::equivalent(value.module_path, fixture),
            std::string(context) + " lost owning module identity");
  };

  auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == outer_before,
          "fixture did not stop in the outer scope before shadowing");
  require_local_value(kExpectedOuterLocalRawValue, "outer-before local lookup");

  stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint && stop.breakpoint_address == inner,
          "fixture did not stop in the nested shadowing scope");
  require_local_value(kExpectedLocalRawValue, "inner local lookup");

  bool missing_failed = false;
  try {
    (void)mdbg::inspect_local_integer(debugger, elf, "missing_local");
  } catch (const std::exception&) {
    missing_failed = true;
  }
  require(missing_failed, "missing local variable did not fail explicitly");

  stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == outer_after,
          "fixture did not stop in the outer scope after shadowing");
  require_local_value(kExpectedOuterLocalRawValue, "outer-after local lookup");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "local-value fixture did not exit cleanly after lexical-scope inspection");
}

void test_formal_parameter_api(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const auto probe = elf.find_symbol("formal_parameter_probe");
  require(probe.has_value(), "formal_parameter_probe symbol missing from optimized fixture");
  const auto address = static_cast<std::uintptr_t>(elf.runtime_address(debugger.pid(), *probe));
  debugger.add_breakpoint(address);
  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint && stop.breakpoint_address == address,
          "formal-parameter fixture did not stop while the parameter was live");

  const auto value = mdbg::inspect_local_integer(debugger, elf, "parameter");
  require(value.name == "parameter", "formal-parameter API returned the wrong name");
  require(value.raw_value == kExpectedLocalRawValue,
          "formal-parameter API returned the wrong register-resident value");
  require(value.byte_size == sizeof(std::uint64_t) && !value.is_signed,
          "formal-parameter API returned the wrong uint64_t type metadata");
  require(std::filesystem::equivalent(value.module_path, fixture),
          "formal-parameter API did not preserve the owning module identity");

  bool missing_failed = false;
  try {
    (void)mdbg::inspect_local_integer(debugger, elf, "missing_parameter");
  } catch (const std::exception&) {
    missing_failed = true;
  }
  require(missing_failed, "missing formal parameter did not fail explicitly");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "formal-parameter fixture did not exit cleanly after inspection");
}

void test_optimized_local_api(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const auto probe = elf.find_symbol("optimized_local_probe");
  require(probe.has_value(), "optimized_local_probe symbol missing from optimized fixture");
  const auto address = static_cast<std::uintptr_t>(elf.runtime_address(debugger.pid(), *probe));
  debugger.add_breakpoint(address);
  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint && stop.breakpoint_address == address,
          "optimized-local fixture did not stop while the local was live");

  const auto value = mdbg::inspect_local_integer(debugger, elf, "optimized_local");
  require(value.name == "optimized_local", "optimized-local API returned the wrong name");
  require(value.raw_value == kExpectedOptimizedLocalRawValue,
          "optimized-local API returned the wrong register-resident value");
  require(value.byte_size == sizeof(std::uint64_t) && !value.is_signed,
          "optimized-local API returned the wrong uint64_t type metadata");
  require(std::filesystem::equivalent(value.module_path, fixture),
          "optimized-local API did not preserve the owning module identity");

  bool missing_failed = false;
  try {
    (void)mdbg::inspect_local_integer(debugger, elf, "missing_optimized_local");
  } catch (const std::exception&) {
    missing_failed = true;
  }
  require(missing_failed, "missing optimized local did not fail explicitly");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "optimized-local fixture did not exit cleanly after inspection");
}

void test_live_enum_api(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const auto probe = elf.find_symbol("live_enum_probe");
  require(probe.has_value(), "live_enum_probe symbol missing from optimized fixture");
  const auto address =
      static_cast<std::uintptr_t>(elf.runtime_address(debugger.pid(), *probe));
  debugger.add_breakpoint(address);
  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == address,
          "live-enum fixture did not stop while the enum local was active");

  const auto value = mdbg::inspect_local_value(debugger, elf, "live_mode");
  require(value.name == "live_mode" &&
              value.kind == mdbg::LocalValueKind::Enumeration &&
              value.byte_size == sizeof(std::uint32_t) &&
              !value.is_signed && value.raw_value == UINT64_C(42),
          "live enum did not preserve compiler-described raw/type identity");
  require(value.enum_type.has_value(),
          "live enum lost canonical enum metadata");
  require(value.enum_type->name == "LiveMode" &&
              value.enum_type->byte_size == sizeof(std::uint32_t) &&
              !value.enum_type->is_signed &&
              value.enum_type->enumerators.size() == 3,
          "live enum representation metadata is incorrect");

  const auto has_entry = [&](const char* name, std::uint64_t raw) {
    for (const auto& entry : value.enum_type->enumerators) {
      if (entry.name == name && entry.raw_value == raw) return true;
    }
    return false;
  };
  require(has_entry("LiveIdle", 3) &&
              has_entry("LiveReady", 7) &&
              has_entry("LiveBusy", 42),
          "live enum lost the compiler enumerator table");
  const auto symbol = mdbg::local_enum_symbol(value);
  require(symbol && *symbol == "LiveBusy",
          "live enum raw value did not resolve to its exact symbol");

  auto unknown = value;
  unknown.raw_value = 11;
  require(!mdbg::local_enum_symbol(unknown),
          "unknown live enum value must remain numeric");
  auto ambiguous = value;
  ambiguous.enum_type->enumerators.push_back({"LiveBusyAlias", 42});
  require(!mdbg::local_enum_symbol(ambiguous),
          "duplicate live enum aliases must remain symbolically ambiguous");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "live-enum fixture did not exit cleanly after inspection");
}

void test_live_array_api(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const auto probe = elf.find_symbol("live_array_probe");
  require(probe.has_value(),
          "live_array_probe symbol missing from optimized fixture");
  const auto address =
      static_cast<std::uintptr_t>(elf.runtime_address(debugger.pid(), *probe));
  debugger.add_breakpoint(address);
  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == address,
          "live-array fixture did not stop while the fixed array was active");

  const auto value = mdbg::inspect_local_value(debugger, elf, "live_array");
  require(value.name == "live_array" &&
              value.kind == mdbg::LocalValueKind::Array &&
              value.byte_size == 3 * sizeof(std::int32_t) &&
              value.array_type.has_value(),
          "live fixed array lost canonical root identity");
  require(value.array_type->element_count == 3 &&
              value.array_type->element_byte_size == sizeof(std::int32_t) &&
              value.array_type->element_is_signed &&
              value.array_type->element_kind == mdbg::LocalValueKind::Integer,
          "live fixed-array compiler metadata is incorrect");
  require(value.elements.size() == 3 &&
              value.elements[0].raw_value == UINT64_C(0x10203040) &&
              value.elements[1].raw_value == UINT64_C(0x22334455) &&
              value.elements[2].raw_value == UINT64_C(0x33445566),
          "live fixed-array bytes were not materialized exactly");

  const auto middle = mdbg::inspect_local_array_element(value, 1);
  require(middle.name == "live_array[1]" &&
              middle.kind == mdbg::LocalValueKind::Integer &&
              middle.byte_size == sizeof(std::int32_t) &&
              middle.is_signed &&
              middle.raw_value == UINT64_C(0x22334455),
          "live fixed-array checked index 1 was not recovered exactly");

  bool rejected = false;
  try {
    (void)mdbg::inspect_local_array_element(value, 3);
  } catch (const std::out_of_range&) {
    rejected = true;
  }
  require(rejected, "live fixed-array out-of-range index was accepted");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "live-array fixture did not exit cleanly after inspection");
}

void test_live_union_api(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const auto probe = elf.find_symbol("live_union_probe");
  require(probe.has_value(),
          "live_union_probe symbol missing from optimized fixture");
  const auto address =
      static_cast<std::uintptr_t>(elf.runtime_address(debugger.pid(), *probe));
  debugger.add_breakpoint(address);
  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == address,
          "live-union fixture did not stop while the union was active");

  const auto value = mdbg::inspect_local_value(debugger, elf, "live_union");
  require(value.name == "live_union" &&
              value.kind == mdbg::LocalValueKind::Union &&
              value.byte_size == sizeof(std::uint32_t) &&
              value.members.size() == 2,
          "live union lost canonical root identity");
  require(value.members[0].name == "signed_value" &&
              value.members[0].offset == 0 &&
              value.members[0].byte_size == sizeof(std::int32_t) &&
              value.members[0].is_signed &&
              value.members[1].name == "unsigned_value" &&
              value.members[1].offset == 0 &&
              value.members[1].byte_size == sizeof(std::uint32_t) &&
              !value.members[1].is_signed,
          "live union member metadata does not match compiler evidence");

  const auto signed_view =
      mdbg::inspect_local_union_member(value, "signed_value");
  require(signed_view.name == "live_union.signed_value" &&
              signed_view.kind == mdbg::LocalValueKind::Integer &&
              signed_view.raw_value == UINT64_C(0x44556677) &&
              signed_view.byte_size == sizeof(std::int32_t) &&
              signed_view.is_signed,
          "live signed union member view was not recovered exactly");

  const auto unsigned_view =
      mdbg::inspect_local_union_member(value, "unsigned_value");
  require(unsigned_view.name == "live_union.unsigned_value" &&
              unsigned_view.kind == mdbg::LocalValueKind::Integer &&
              unsigned_view.raw_value == UINT64_C(0x44556677) &&
              unsigned_view.byte_size == sizeof(std::uint32_t) &&
              !unsigned_view.is_signed,
          "live unsigned union member view was not recovered exactly");

  bool missing = false;
  try {
    (void)mdbg::inspect_local_union_member(value, "missing");
  } catch (const std::runtime_error&) {
    missing = true;
  }
  require(missing, "live union accepted an unknown member");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "live-union fixture did not exit cleanly after inspection");
}

void test_live_bit_field_api(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const auto probe = elf.find_symbol("live_bit_field_probe");
  require(probe.has_value(),
          "live_bit_field_probe symbol missing from optimized fixture");
  const auto address =
      static_cast<std::uintptr_t>(elf.runtime_address(debugger.pid(), *probe));
  debugger.add_breakpoint(address);
  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == address,
          "live bit-field fixture did not stop while the structure was active");

  const auto value = mdbg::inspect_local_value(debugger, elf, "live_bit_fields");
  require(value.name == "live_bit_fields" &&
              value.kind == mdbg::LocalValueKind::Structure &&
              value.byte_size == sizeof(std::uint32_t) &&
              value.members.size() == 2,
          "live bit-field structure lost canonical root identity");
  require(value.members[0].name == "signed_bits" &&
              value.members[0].kind == mdbg::LocalValueKind::Integer &&
              value.members[0].byte_size == sizeof(std::int32_t) &&
              value.members[0].is_signed &&
              value.members[0].bit_slice.has_value() &&
              value.members[0].bit_slice->bit_size == 5 &&
              value.members[0].raw_value == UINT64_C(0xfffffff9),
          "live signed bit field was not normalized to int32 -7");
  require(value.members[1].name == "unsigned_bits" &&
              value.members[1].kind == mdbg::LocalValueKind::Integer &&
              value.members[1].byte_size == sizeof(std::uint32_t) &&
              !value.members[1].is_signed &&
              value.members[1].bit_slice.has_value() &&
              value.members[1].bit_slice->bit_size == 6 &&
              value.members[1].raw_value == UINT64_C(41),
          "live unsigned bit field was not normalized to uint32 41");

  const auto signed_view =
      mdbg::inspect_local_aggregate_member(value, "signed_bits");
  require(signed_view.name == "live_bit_fields.signed_bits" &&
              signed_view.raw_value == UINT64_C(0xfffffff9) &&
              signed_view.byte_size == sizeof(std::int32_t) &&
              signed_view.is_signed,
          "live signed bit-field member selection lost exact value identity");
  const auto unsigned_view =
      mdbg::inspect_local_aggregate_member(value, "unsigned_bits");
  require(unsigned_view.name == "live_bit_fields.unsigned_bits" &&
              unsigned_view.raw_value == UINT64_C(41) &&
              unsigned_view.byte_size == sizeof(std::uint32_t) &&
              !unsigned_view.is_signed,
          "live unsigned bit-field member selection lost exact value identity");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "live bit-field fixture did not exit cleanly after inspection");
}

std::string run_cli_script(const std::string& integration_path, const std::string& fixture,
                           const char* mode, const std::string& script,
                           const char* context) {
  const auto mdbg_path =
      (std::filesystem::absolute(integration_path).parent_path() / "mdbg").string();
  require(std::filesystem::exists(mdbg_path), "mdbg executable is missing beside integration test");

  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create CLI pipes");
  }

  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("failed to fork CLI");
  if (child == 0) {
    ::setpgid(0, 0);
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    if (mode != nullptr) {
      ::execl(mdbg_path.c_str(), mdbg_path.c_str(), fixture.c_str(), mode, nullptr);
    } else {
      ::execl(mdbg_path.c_str(), mdbg_path.c_str(), fixture.c_str(), nullptr);
    }
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);

  std::size_t written = 0;
  while (written < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + written, script.size() - written);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write CLI script");
    written += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  bool eof = false;
  while (!eof) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      ::kill(-child, SIGKILL);
      ::kill(child, SIGKILL);
      throw std::runtime_error(std::string("timed out waiting for ") + context);
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
    const int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
    if (result == -1 && errno == EINTR) continue;
    if (result <= 0) {
      ::kill(-child, SIGKILL);
      ::kill(child, SIGKILL);
      throw std::runtime_error(std::string("timed out reading ") + context + " output");
    }

    char buffer[512];
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error(std::string("failed to read ") + context + " output");
    if (count == 0) {
      eof = true;
    } else {
      output.append(buffer, static_cast<std::size_t>(count));
    }
  }
  ::close(output_pipe[0]);

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          std::string(context) + " did not exit cleanly\n" + output);
  return output;
}

void test_cli_local_value(const std::string& integration_path, const std::string& fixture) {
  const auto output = run_cli_script(
      integration_path, fixture, "value",
      "break outer_local_before_probe\n"
      "break local_value_probe\n"
      "break outer_local_after_probe\n"
      "continue\n"
      "print local_value\n"
      "continue\n"
      "print local_value\n"
      "continue\n"
      "print local_value\n"
      "continue\n",
      "lexical-scope value-inspection CLI");
  require(output.find("Breakpoint 3") != std::string::npos,
          "CLI did not install all lexical-scope probe breakpoints\n" + output);
  const auto first_outer = output.find(kExpectedOuterLocalValue);
  const auto inner = first_outer == std::string::npos
                         ? std::string::npos
                         : output.find(kExpectedLocalValue, first_outer + 1);
  const auto second_outer = inner == std::string::npos
                                ? std::string::npos
                                : output.find(kExpectedOuterLocalValue, inner + 1);
  require(first_outer != std::string::npos && inner != std::string::npos &&
              second_outer != std::string::npos,
          "CLI did not render outer/inner/outer shadow ownership in order\n" + output);
}

void test_cli_formal_parameter(const std::string& integration_path,
                               const std::string& fixture) {
  const auto output = run_cli_script(
      integration_path, fixture, nullptr,
      "break formal_parameter_probe\n"
      "continue\n"
      "print parameter\n"
      "print missing_parameter\n"
      "continue\n",
      "formal-parameter CLI");
  require(output.find("Breakpoint 1") != std::string::npos,
          "CLI did not install the formal-parameter probe breakpoint\n" + output);
  require(output.find(kExpectedParameterValue) != std::string::npos,
          "CLI did not render the register-resident formal parameter\n" + output);
  require(output.find("print failed:") != std::string::npos,
          "CLI did not report a missing formal parameter explicitly\n" + output);
}

void test_cli_optimized_local(const std::string& integration_path,
                              const std::string& fixture) {
  const auto output = run_cli_script(
      integration_path, fixture, nullptr,
      "break optimized_local_probe\n"
      "continue\n"
      "print optimized_local\n"
      "print missing_optimized_local\n"
      "continue\n",
      "optimized-local CLI");
  require(output.find("Breakpoint 1") != std::string::npos,
          "CLI did not install the optimized-local probe breakpoint\n" + output);
  require(output.find(kExpectedOptimizedLocalValue) != std::string::npos,
          "CLI did not render the optimized local integer value\n" + output);
  require(output.find("print failed:") != std::string::npos,
          "CLI did not report a missing optimized local explicitly\n" + output);
}

void test_cli_live_enum(const std::string& integration_path,
                        const std::string& fixture) {
  const auto output = run_cli_script(
      integration_path, fixture, nullptr,
      "break live_enum_probe\n"
      "continue\n"
      "print live_mode\n"
      "continue\n",
      "live-enum CLI");
  require(output.find("Breakpoint 1") != std::string::npos,
          "CLI did not install the live-enum probe breakpoint\n" + output);
  require(output.find(kExpectedLiveEnumValue) != std::string::npos,
          "CLI did not render symbolic + numeric live enum identity\n" + output);
}

void test_cli_live_array(const std::string& integration_path,
                         const std::string& fixture) {
  const auto output = run_cli_script(
      integration_path, fixture, nullptr,
      "break live_array_probe\n"
      "continue\n"
      "print live_array\n"
      "array-element live_array 1\n"
      "array-element live_array 3\n"
      "continue\n",
      "live fixed-array CLI");
  require(output.find("Breakpoint 1") != std::string::npos,
          "CLI did not install the live-array probe breakpoint\n" + output);
  require(output.find(
              "live_array = [0x10203040, 0x22334455, 0x33445566]") !=
              std::string::npos,
          "CLI did not render the bounded live fixed array\n" + output);
  require(output.find("live_array[1] = 0x22334455") != std::string::npos,
          "CLI did not render checked live array index 1\n" + output);
  require(output.find("array-element failed: array index is out of range") !=
              std::string::npos,
          "CLI did not reject an out-of-range live array index\n" + output);
}

void test_cli_live_union(const std::string& integration_path,
                         const std::string& fixture) {
  const auto output = run_cli_script(
      integration_path, fixture, nullptr,
      "break live_union_probe\n"
      "continue\n"
      "print live_union\n"
      "union-member live_union signed_value\n"
      "union-member live_union unsigned_value\n"
      "union-member live_union missing\n"
      "continue\n",
      "live union CLI");
  require(output.find("Breakpoint 1") != std::string::npos,
          "CLI did not install the live-union probe breakpoint\n" + output);
  require(output.find("live_union = union{signed_value, unsigned_value}") !=
              std::string::npos,
          "CLI did not render explicit live union member choices\n" + output);
  require(output.find("live_union.signed_value = 0x44556677") !=
              std::string::npos,
          "CLI did not render signed live union member selection\n" + output);
  require(output.find("live_union.unsigned_value = 0x44556677") !=
              std::string::npos,
          "CLI did not render unsigned live union member selection\n" + output);
  require(output.find(
              "union-member failed: bounded selected-inline union has no member named: missing") !=
              std::string::npos,
          "CLI did not deterministically reject missing live union member\n" + output);
}

void test_cli_live_bit_fields(const std::string& integration_path,
                              const std::string& fixture) {
  const auto output = run_cli_script(
      integration_path, fixture, nullptr,
      "break live_bit_field_probe\n"
      "continue\n"
      "print live_bit_fields\n"
      "aggregate-member live_bit_fields signed_bits\n"
      "aggregate-member live_bit_fields unsigned_bits\n"
      "continue\n",
      "live bit-field CLI");
  require(output.find("Breakpoint 1") != std::string::npos,
          "CLI did not install the live bit-field probe breakpoint\n" + output);
  require(output.find(
              "live_bit_fields = { signed_bits = -7, unsigned_bits = 41 }") !=
              std::string::npos,
          "CLI did not render compiler-described live bit fields\n" + output);
  require(output.find("live_bit_fields.signed_bits = -7") != std::string::npos,
          "CLI did not select the signed live bit field\n" + output);
  require(output.find("live_bit_fields.unsigned_bits = 41") !=
              std::string::npos,
          "CLI did not select the unsigned live bit field\n" + output);
}

void test_missing_debug_line(const std::string& stripped_fixture) {
  const mdbg::DwarfLineTable lines(stripped_fixture);
  require(!lines.available(), "stripped fixture must not claim DWARF line coverage");
  require(!lines.find_virtual_address(0).has_value(),
          "empty line table must not resolve arbitrary addresses");
  require(!lines.find_virtual_source("mapped_source.c", 400).has_value(),
          "empty line table must not resolve source locations");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) return 2;
  try {
    test_runtime_mapping(argv[1]);
    test_local_value_api(argv[1]);
    test_cli_local_value(argv[0], argv[1]);
    test_missing_debug_line(argv[2]);
    test_runtime_mapping(argv[3]);
    test_formal_parameter_api(argv[4]);
    test_cli_formal_parameter(argv[0], argv[4]);
    test_optimized_local_api(argv[4]);
    test_cli_optimized_local(argv[0], argv[4]);
    test_live_enum_api(argv[4]);
    test_cli_live_enum(argv[0], argv[4]);
    test_live_array_api(argv[4]);
    test_cli_live_array(argv[0], argv[4]);
    test_live_union_api(argv[4]);
    test_cli_live_union(argv[0], argv[4]);
    test_live_bit_field_api(argv[4]);
    test_cli_live_bit_fields(argv[0], argv[4]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "DWARF line integration failure: %s\n", error.what());
    return 1;
  }
}

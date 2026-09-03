#include "debugger/debugger.hpp"
#include "dwarf/line_table.hpp"
#include "elf/elf.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

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
  require(lines.available(), "DWARF v4 fixture should expose line ranges");

  const auto probe = elf.find_symbol("line_probe");
  require(probe.has_value(), "line_probe symbol missing from fixture");

  const auto virtual_location = lines.find_virtual_address(probe->value);
  require(virtual_location.has_value(), "virtual line lookup failed for line_probe");
  require_mapped_source(*virtual_location);

  const auto runtime_address = elf.runtime_address(debugger.pid(), *probe);
  const auto runtime_location =
      lines.find_runtime_address(debugger.pid(), runtime_address, elf);
  require(runtime_location.has_value(), "runtime line lookup failed for line_probe");
  require_mapped_source(*runtime_location);

  debugger.add_breakpoint(static_cast<std::uintptr_t>(runtime_address));
  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == runtime_address,
          "line_probe breakpoint was not hit");
  const auto repaired_rip = debugger.registers().rip;
  require(repaired_rip == runtime_address,
          "managed breakpoint did not expose the repaired instruction pointer");
  const auto stopped_location =
      lines.find_runtime_address(debugger.pid(), repaired_rip, elf);
  require(stopped_location.has_value(), "stopped RIP did not resolve to a source line");
  require_mapped_source(*stopped_location);
}

void test_missing_debug_line(const std::string& stripped_fixture) {
  const mdbg::DwarfLineTable lines(stripped_fixture);
  require(!lines.available(), "stripped fixture must not claim DWARF line coverage");
  require(!lines.find_virtual_address(0).has_value(),
          "empty line table must not resolve arbitrary addresses");
}

void test_unsupported_version(const std::string& dwarf5_fixture) {
  try {
    const mdbg::DwarfLineTable lines(dwarf5_fixture);
    static_cast<void>(lines);
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    require(message.find("version 5") != std::string::npos &&
                message.find("unsupported") != std::string::npos,
            "DWARF5 rejection should identify the unsupported version");
    return;
  }
  throw std::runtime_error("DWARF5 fixture must be rejected explicitly");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  try {
    test_runtime_mapping(argv[1]);
    test_missing_debug_line(argv[2]);
    test_unsupported_version(argv[3]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "DWARF line integration failure: %s\n", error.what());
    return 1;
  }
}

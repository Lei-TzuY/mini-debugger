#include "debugger/debugger.hpp"
#include "elf/elf.hpp"
#include "unwind/cfi.hpp"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_same_module(const std::string& actual, const std::string& expected,
                         const std::string& message) {
  std::error_code error;
  const bool equivalent = std::filesystem::equivalent(actual, expected, error);
  require(!error && equivalent, message);
}

void require_breakpoint_installed(const mdbg::Debugger& debugger, std::size_t id) {
  for (const auto& breakpoint : debugger.breakpoints()) {
    if (breakpoint.id == id) {
      require(breakpoint.installed,
              "module symbol breakpoint was not reinserted after displaced execution");
      return;
    }
  }
  throw std::runtime_error("module symbol breakpoint disappeared");
}

int read_int_symbol(const mdbg::Debugger& debugger, const mdbg::ElfFile& elf,
                    const std::string& name) {
  const auto symbol = elf.find_symbol(name);
  require(symbol.has_value(), name + " symbol is missing");
  const auto address = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *symbol));
  const auto bytes = debugger.read_memory(address, sizeof(int));
  require(bytes.size() == sizeof(int), "failed to read " + name);
  int value = 0;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

void test_module_symbol_breakpoint(const std::string& driver,
                                   const std::string& library) {
  auto debugger = mdbg::Debugger::launch(driver, {});
  const auto sync = debugger.continue_execution();
  require(sync.reason == mdbg::StopReason::Signal && sync.value == SIGSTOP,
          "module-symbol fixture synchronization SIGSTOP was not observed");

  const mdbg::ElfFile executable(driver);
  const mdbg::ElfFile shared(library);

  const auto shared_symbol = mdbg::find_module_symbol_by_name(
      debugger.pid(), "shared_symbol_break_target", executable);
  require(shared_symbol.has_value(),
          "loaded shared-object symbol did not resolve by name");
  require_same_module(shared_symbol->module_path, library,
                      "shared-object symbol resolved from the wrong module");
  require(shared_symbol->name == "shared_symbol_break_target",
          "shared-object symbol resolver returned the wrong name");

  const auto expected_shared = shared.find_symbol("shared_symbol_break_target");
  require(expected_shared.has_value(),
          "shared_symbol_break_target definition is missing from shared ELF");
  require(shared_symbol->address ==
              static_cast<std::uintptr_t>(
                  shared.runtime_address(debugger.pid(), *expected_shared)),
          "shared-object symbol resolver returned the wrong runtime address");

  const auto driver_symbol = mdbg::find_module_symbol_by_name(
      debugger.pid(), "driver_break_target", executable);
  require(driver_symbol.has_value(),
          "main-executable symbol did not resolve through module routing");
  require_same_module(driver_symbol->module_path, driver,
                      "main-executable symbol resolved from the wrong module");

  bool ambiguous = false;
  try {
    (void)mdbg::find_module_symbol_by_name(
        debugger.pid(), "module_symbol_ambiguous", executable);
  } catch (const std::runtime_error& error) {
    ambiguous = std::string(error.what()).find(
                    "ambiguous symbol across loaded modules") != std::string::npos;
  }
  require(ambiguous,
          "symbol defined in multiple loaded modules must be rejected as ambiguous");

  require(!mdbg::find_module_symbol_by_name(
              debugger.pid(), "definitely_missing_module_symbol", executable),
          "unknown symbol must not resolve to a loaded module");

  const auto followup = shared.find_symbol("shared_break_target");
  require(followup.has_value(), "shared_break_target symbol is missing");
  const auto followup_address = static_cast<std::uintptr_t>(
      shared.runtime_address(debugger.pid(), *followup));

  const auto symbol_breakpoint_id = debugger.add_breakpoint(shared_symbol->address);
  debugger.add_breakpoint(followup_address);

  const auto symbol_hit = debugger.continue_execution();
  require(symbol_hit.reason == mdbg::StopReason::Breakpoint &&
              symbol_hit.breakpoint_address == shared_symbol->address,
          "managed breakpoint resolved from shared symbol was not hit");

  const auto followup_hit = debugger.continue_execution();
  require(followup_hit.reason == mdbg::StopReason::Breakpoint &&
              followup_hit.breakpoint_address == followup_address,
          "follow-up shared breakpoint was not hit after symbol breakpoint");
  require_breakpoint_installed(debugger, symbol_breakpoint_id);
  require(read_int_symbol(debugger, shared, "shared_symbol_marker") == 1,
          "shared symbol breakpoint target did not execute its displaced instruction path");
  require(debugger.breakpoints().size() == 2,
          "module symbol breakpoint test leaked debugger-visible breakpoints");

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "module-symbol fixture did not exit cleanly");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    test_module_symbol_breakpoint(argv[1], argv[2]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "module-symbol breakpoint integration failure: %s\n",
                 error.what());
    return 1;
  }
}

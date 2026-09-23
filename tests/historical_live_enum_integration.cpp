#include "debugger/debugger.hpp"
#include "dwarf/eh_frame.hpp"
#include "dwarf/local_value.hpp"
#include "elf/elf.hpp"
#include "unwind/cfi.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void verify_historical_enum(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::EhFrame cfi(fixture);
  require(cfi.available(), "historical enum fixture is missing .eh_frame");

  const auto callee_probe = elf.find_symbol("historical_enum_callee_probe");
  const auto after_probe = elf.find_symbol("historical_enum_after_probe");
  require(callee_probe && after_probe,
          "historical enum probe symbols are missing");

  const auto callee_address = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *callee_probe));
  const auto after_address = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *after_probe));
  debugger.add_breakpoint(callee_address);
  debugger.add_breakpoint(after_address);

  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == callee_address,
          "historical enum fixture did not stop in the callee");

  const auto frames = mdbg::build_inspection_frames(debugger, elf, cfi, 3);
  require(frames.size() >= 2,
          "CFI did not recover the historical enum caller frame");
  const auto caller_function = elf.find_symbol("historical_enum_caller");
  require(caller_function.has_value() && caller_function->size != 0,
          "historical enum caller symbol is missing or has zero size");
  const auto caller_begin = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *caller_function));
  require(caller_function->size <=
              std::numeric_limits<std::uintptr_t>::max() - caller_begin,
          "historical enum caller symbol range overflows");
  const auto caller_end = caller_begin + caller_function->size;
  require(frames[1].runtime_pc >= caller_begin &&
              frames[1].runtime_pc < caller_end,
          "historical enum frame 1 PC is outside the caller function range");
  require(frames[1].registers.rbx.has_value(),
          "historical enum frame is missing CFI-recovered RBX ownership");
  require(static_cast<std::uint32_t>(*frames[1].registers.rbx) ==
              UINT32_C(42),
          "historical enum recovered RBX does not carry the compiler-owned enum value");

  const auto value =
      mdbg::inspect_local_value(debugger, elf, frames[1], "historical_mode");
  require(value.kind == mdbg::LocalValueKind::Enumeration &&
              value.byte_size == sizeof(std::uint32_t) &&
              !value.is_signed && value.raw_value == UINT64_C(42),
          "historical enum lost compiler-owned scalar identity");
  require(value.enum_type && value.enum_type->name == "HistoricalLiveMode" &&
              value.enum_type->enumerators.size() == 3,
          "historical enum lost canonical type metadata");
  const auto symbol = mdbg::local_enum_symbol(value);
  require(symbol && *symbol == "HistoricalBusy",
          "historical enum did not preserve its unique symbolic value");

  const auto stale_frame = frames[1];
  const auto next_stop = debugger.continue_execution();
  require(next_stop.reason == mdbg::StopReason::Breakpoint &&
              next_stop.breakpoint_address == after_address,
          "historical enum fixture did not reach a new caller stop");

  bool stale_rejected = false;
  try {
    (void)mdbg::inspect_local_value(
        debugger, elf, stale_frame, "historical_mode");
  } catch (const std::logic_error&) {
    stale_rejected = true;
  }
  require(stale_rejected,
          "historical enum accepted an inspection frame from an older stop");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "historical enum fixture did not exit cleanly");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    verify_historical_enum(argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "historical live enum integration failure: %s\n",
                 error.what());
    return 1;
  }
}

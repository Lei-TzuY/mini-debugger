#include "debugger/debugger.hpp"
#include "dwarf/eh_frame.hpp"
#include "dwarf/local_value.hpp"
#include "elf/elf.hpp"
#include "unwind/cfi.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::int32_t read_i32(const mdbg::Debugger& debugger, std::uintptr_t address) {
  const auto bytes = debugger.read_memory(address, sizeof(std::int32_t));
  require(bytes.size() == sizeof(std::int32_t),
          "historical pointer raw target read was truncated");
  std::int32_t value = 0;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

void verify_historical_pointer(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::EhFrame cfi(fixture);
  require(cfi.available(), "historical pointer fixture is missing .eh_frame");

  const auto callee_probe = elf.find_symbol("historical_pointer_callee_probe");
  const auto after_probe = elf.find_symbol("historical_pointer_after_probe");
  const auto target = elf.find_symbol("historical_pointer_target");
  require(callee_probe && after_probe && target,
          "historical pointer fixture symbols are missing");

  const auto callee_address = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *callee_probe));
  const auto after_address = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *after_probe));
  const auto target_address = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *target));
  debugger.add_breakpoint(callee_address);
  debugger.add_breakpoint(after_address);

  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == callee_address,
          "historical pointer fixture did not stop in the callee");

  // Independent target oracle before source-value inspection.
  require(read_i32(debugger, target_address) == INT32_C(0x13579bdf),
          "historical pointer target does not contain the expected live value");

  const auto frames = mdbg::build_inspection_frames(debugger, elf, cfi, 3);
  require(frames.size() >= 2,
          "CFI did not recover the historical pointer caller frame");
  const auto caller_function = elf.find_symbol("historical_pointer_caller");
  require(caller_function.has_value() && caller_function->size != 0,
          "historical pointer caller symbol is missing or has zero size");
  const auto caller_begin = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *caller_function));
  require(caller_function->size <=
              std::numeric_limits<std::uintptr_t>::max() - caller_begin,
          "historical pointer caller symbol range overflows");
  const auto caller_end = caller_begin + caller_function->size;
  require(frames[1].runtime_pc >= caller_begin &&
              frames[1].runtime_pc < caller_end,
          "historical pointer frame 1 PC is outside the caller function range");

  const auto current_regs = debugger.registers();
  require(current_regs.rbx == UINT64_C(0x1122334455667788),
          "callee live RBX does not contain the deliberate clobber sentinel");
  require(frames[1].registers.rbx.has_value() &&
              *frames[1].registers.rbx == target_address,
          "CFI did not recover caller-owned historical RBX pointer state");
  require(*frames[1].registers.rbx != current_regs.rbx,
          "historical pointer ownership accidentally reused current callee RBX");

  const auto pointer =
      mdbg::inspect_local_value(debugger, elf, frames[1], "historical_pointer");
  require(pointer.kind == mdbg::LocalValueKind::Pointer &&
              pointer.byte_size == sizeof(std::uintptr_t) &&
              !pointer.is_signed &&
              pointer.raw_value == target_address,
          "historical pointer lost compiler-owned pointer identity");
  require(pointer.pointee_type.has_value() &&
              pointer.pointee_type->kind == mdbg::LocalValueKind::Integer &&
              pointer.pointee_type->byte_size == sizeof(std::int32_t) &&
              pointer.pointee_type->is_signed,
          "historical pointer lost bounded signed-int32 pointee metadata");

  const auto pointee = mdbg::dereference_local_pointer(
      debugger, elf, frames[1], "historical_pointer");
  require(pointee.name == "*historical_pointer" &&
              pointee.kind == mdbg::LocalValueKind::Integer &&
              pointee.byte_size == sizeof(std::int32_t) &&
              pointee.is_signed &&
              pointee.raw_value == UINT64_C(0x13579bdf),
          "historical pointer one-hop dereference did not recover the live pointee");

  const auto stale_frame = frames[1];
  const auto next_stop = debugger.continue_execution();
  require(next_stop.reason == mdbg::StopReason::Breakpoint &&
              next_stop.breakpoint_address == after_address,
          "historical pointer fixture did not reach a new caller stop");

  bool stale_rejected = false;
  try {
    (void)mdbg::inspect_local_value(
        debugger, elf, stale_frame, "historical_pointer");
  } catch (const std::logic_error&) {
    stale_rejected = true;
  }
  require(stale_rejected,
          "historical pointer accepted an inspection frame from an older stop");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "historical pointer fixture did not exit cleanly");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    verify_historical_pointer(argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "historical live pointer integration failure: %s\n",
                 error.what());
    return 1;
  }
}

#include "debugger/debugger.hpp"
#include "dwarf/eh_frame.hpp"
#include "dwarf/local_value.hpp"
#include "elf/elf.hpp"
#include "unwind/cfi.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void verify(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::EhFrame cfi(fixture);
  require(cfi.available(), "historical structure-pointer fixture is missing .eh_frame");

  const auto callee_probe = elf.find_symbol("historical_structure_callee_probe");
  const auto after_probe = elf.find_symbol("historical_structure_after_probe");
  const auto target = elf.find_symbol("historical_structure_target");
  const auto caller = elf.find_symbol("historical_structure_caller");
  require(callee_probe && after_probe && target && caller && caller->size != 0,
          "historical structure-pointer fixture symbols are missing");

  const auto callee_address = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *callee_probe));
  const auto after_address = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *after_probe));
  const auto target_address = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *target));
  const auto caller_begin = static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(), *caller));
  require(caller->size <= std::numeric_limits<std::uintptr_t>::max() - caller_begin,
          "historical structure caller range overflows");
  const auto caller_end = caller_begin + caller->size;

  debugger.add_breakpoint(callee_address);
  debugger.add_breakpoint(after_address);
  const auto stop = debugger.continue_execution();
  require(stop.reason == mdbg::StopReason::Breakpoint &&
              stop.breakpoint_address == callee_address,
          "historical structure-pointer fixture did not stop in callee");

  const auto frames = mdbg::build_inspection_frames(debugger, elf, cfi, 3);
  require(frames.size() >= 2 &&
              frames[1].runtime_pc >= caller_begin &&
              frames[1].runtime_pc < caller_end,
          "CFI did not recover the historical structure-pointer caller frame");

  const auto current = debugger.registers();
  require(current.rbx == UINT64_C(0x1122334455667788),
          "callee live RBX lost deliberate sentinel");

  const auto pointer = mdbg::inspect_local_value(
      debugger, elf, frames[1], "historical_structure_pointer");
  require(pointer.kind == mdbg::LocalValueKind::Pointer &&
              pointer.byte_size == sizeof(std::uintptr_t) &&
              pointer.raw_value == target_address &&
              pointer.pointee_type.has_value(),
          "historical structure pointer lost pointer identity");
  const auto& pointee_type = *pointer.pointee_type;
  require(pointee_type.kind == mdbg::LocalValueKind::Structure &&
              pointee_type.byte_size == 16 &&
              pointee_type.members.size() == 2,
          "historical structure pointer lost bounded structure pointee metadata");
  require(pointee_type.members[0].name == "count" &&
              pointee_type.members[0].offset == 0 &&
              pointee_type.members[0].byte_size == 4 &&
              !pointee_type.members[0].is_signed &&
              pointee_type.members[1].name == "delta" &&
              pointee_type.members[1].offset == 8 &&
              pointee_type.members[1].byte_size == 8 &&
              pointee_type.members[1].is_signed,
          "historical structure pointee layout metadata changed");

  const auto object = mdbg::dereference_local_pointer(
      debugger, elf, frames[1], "historical_structure_pointer");
  require(object.name == "*historical_structure_pointer" &&
              object.kind == mdbg::LocalValueKind::Structure &&
              object.byte_size == 16 &&
              object.members.size() == 2,
          "historical structure pointer did not materialize bounded object");
  require(object.members[0].name == "count" &&
              object.members[0].raw_value == UINT64_C(0x11223344) &&
              object.members[1].name == "delta" &&
              object.members[1].raw_value ==
                  static_cast<std::uint64_t>(INT64_C(-123456789)),
          "historical structure pointer decoded object members incorrectly");

  const auto stale = frames[1];
  const auto next = debugger.continue_execution();
  require(next.reason == mdbg::StopReason::Breakpoint &&
              next.breakpoint_address == after_address,
          "historical structure-pointer fixture did not reach next caller stop");
  bool rejected = false;
  try {
    (void)mdbg::dereference_local_pointer(
        debugger, elf, stale, "historical_structure_pointer");
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "historical structure pointer accepted stale inspection frame");

  const auto exit = debugger.continue_execution();
  require(exit.reason == mdbg::StopReason::Exited && exit.value == 0,
          "historical structure-pointer fixture did not exit cleanly");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    verify(argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "historical live structure pointer integration failure: %s\n",
                 error.what());
    return 1;
  }
}

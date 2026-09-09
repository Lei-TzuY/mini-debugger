#include "elf/elf.hpp"
#include "snapshot/session.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uintptr_t kLinuxX86UcontextMcontextOffset = 0x28;
constexpr std::uintptr_t kLinuxX86GregCount = 23;
constexpr std::uintptr_t kLinuxX86FpregsPointerOffset =
    kLinuxX86UcontextMcontextOffset + kLinuxX86GregCount * sizeof(std::uint64_t);
constexpr std::uintptr_t kLinuxX86Xmm0Offset = 160;
constexpr std::uint64_t kInterruptedXmm0Low = UINT64_C(0x40934a0000000000);
constexpr std::uint64_t kHandlerXmm0Low = UINT64_C(0xc0b0e14000000000);

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::uintptr_t runtime_symbol_address(const mdbg::CoreSnapshot& snapshot,
                                      const std::string& executable,
                                      const std::string& symbol_name) {
  const auto canonical = std::filesystem::canonical(executable).string();
  const mdbg::ElfFile elf(canonical);
  const auto symbol = elf.find_symbol(symbol_name);
  require(symbol.has_value(), "signal fixture symbol is unavailable: " + symbol_name);
  if (!elf.is_pie()) return static_cast<std::uintptr_t>(symbol->value);

  for (const auto& mapping : snapshot.file_mappings()) {
    if (mapping.path != canonical || mapping.file_offset != 0) continue;
    require(mapping.start >= elf.load_virtual_base(),
            "signal fixture PIE mapping is below ELF virtual base");
    return static_cast<std::uintptr_t>(mapping.start - elf.load_virtual_base() +
                                       symbol->value);
  }
  throw std::runtime_error("signal fixture PIE offset-zero mapping is unavailable");
}

std::uint64_t read_u64(const mdbg::CoreSnapshot& snapshot, std::uintptr_t address) {
  const auto bytes = snapshot.read_memory(address, sizeof(std::uint64_t));
  require(bytes.size() == sizeof(std::uint64_t), "signal FP oracle read was truncated");
  std::uint64_t value = 0;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

std::uint64_t xmm0_low(const mdbg::CoreFloatingPointState& state) {
  std::uint64_t value = 0;
  std::memcpy(&value, state.xmm[0].data(), sizeof(value));
  return value;
}

const mdbg::SnapshotInspectionFrameContext* trace_context(
    const mdbg::CoreInspectionSession& session, std::uintptr_t instruction_pointer,
    std::uintptr_t stack_pointer) {
  for (const auto& frame : session.trace().frames) {
    if (frame.runtime_pc == instruction_pointer && frame.stack_pointer == stack_pointer) {
      return &frame;
    }
  }
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_signal_fpstate_integration <core> <fixture>\n";
    return 2;
  }

  try {
    mdbg::CoreInspectionSession session(argv[1]);
    const auto& snapshot = session.snapshot();

    const auto ucontext_address = read_u64(
        snapshot, runtime_symbol_address(snapshot, argv[2],
                                         "signal_core_ucontext_address"));
    const auto saved_rip = read_u64(
        snapshot, runtime_symbol_address(snapshot, argv[2], "signal_core_saved_rip"));
    const auto saved_rsp = read_u64(
        snapshot, runtime_symbol_address(snapshot, argv[2], "signal_core_saved_rsp"));
    const auto fixture_fpstate = read_u64(
        snapshot, runtime_symbol_address(snapshot, argv[2], "signal_core_saved_fpstate"));
    const auto fixture_xmm0 = read_u64(
        snapshot, runtime_symbol_address(snapshot, argv[2],
                                         "signal_core_saved_xmm0_low"));

    require(ucontext_address != 0 && fixture_fpstate != 0,
            "signal fixture did not capture a kernel fpstate pointer");
    const auto raw_fpstate = read_u64(snapshot, ucontext_address + kLinuxX86FpregsPointerOffset);
    require(raw_fpstate == fixture_fpstate,
            "raw Linux x86-64 ucontext fpregs pointer disagrees with handler oracle");
    require(raw_fpstate > ucontext_address && raw_fpstate < saved_rsp,
            "signal fpstate is outside the evidence-proven signal-stack interval");

    const auto raw_saved_xmm0 = read_u64(snapshot, raw_fpstate + kLinuxX86Xmm0Offset);
    require(raw_saved_xmm0 == fixture_xmm0,
            "raw signal fpstate XMM0 disagrees with handler ucontext oracle");
    require(raw_saved_xmm0 == kInterruptedXmm0Low,
            "compiler-proven interrupted floating local is not saved in XMM0");

    const auto top_fp = snapshot.floating_point_state(snapshot.crashed_tid());
    require(top_fp.has_value(), "crashed handler thread has no NT_FPREGSET evidence");
    const auto top_xmm0 = xmm0_low(*top_fp);
    require(top_xmm0 == kHandlerXmm0Low,
            "handler crash-frame XMM0 marker was not captured by NT_FPREGSET");
    require(top_xmm0 != raw_saved_xmm0,
            "top-frame NT_FPREGSET was not distinguished from interrupted signal XMM0");

    const auto* restored = trace_context(session, saved_rip, saved_rsp);
    require(restored != nullptr,
            "snapshot unwind did not restore the interrupted application frame");
    session.select_frame(restored->index);
    const auto value = session.inspect_value("interrupted_fp_local");
    require(value.kind == mdbg::LocalValueKind::Floating && value.byte_size == 8,
            "signal-restored interrupted floating local lost its scalar type");
    require(value.raw_value == kInterruptedXmm0Low,
            "signal-restored interrupted floating local has the wrong value");
    require(value.storage == mdbg::LocalValueStorage::SnapshotCoreRegister,
            "signal-restored interrupted floating local lost register provenance");

    std::cout << "signal-restored XMM evidence integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "signal-restored XMM evidence integration failure: " << error.what()
              << '\n';
    return 1;
  }
  return 0;
}

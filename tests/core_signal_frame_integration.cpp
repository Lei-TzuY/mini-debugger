#include "elf/elf.hpp"
#include "snapshot/session.hpp"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

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
    const auto bias = mapping.start - elf.load_virtual_base();
    return static_cast<std::uintptr_t>(bias + symbol->value);
  }
  throw std::runtime_error("signal fixture PIE offset-zero mapping is unavailable");
}

std::uintptr_t read_pointer(const mdbg::CoreSnapshot& snapshot,
                            std::uintptr_t address) {
  const auto bytes = snapshot.read_memory(address, sizeof(std::uintptr_t));
  require(bytes.size() == sizeof(std::uintptr_t),
          "signal-frame oracle pointer read returned a short value");
  std::uintptr_t value = 0;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

bool trace_has_symbol(const mdbg::CoreInspectionSession& session,
                      const std::string& expected) {
  for (const auto& frame : session.trace().frames) {
    try {
      const auto symbol = session.find_frame_symbol(frame);
      if (symbol && symbol->name == expected) return true;
    } catch (const std::exception&) {
    }
  }
  return false;
}

void print_evidence(const mdbg::CoreInspectionSession& session,
                    std::uintptr_t ucontext_address,
                    std::uintptr_t saved_rip,
                    std::uintptr_t saved_rsp) {
  std::cout << "signal-frame evidence: ucontext=0x" << std::hex << ucontext_address
            << " saved-rip=0x" << saved_rip << " saved-rsp=0x" << saved_rsp
            << std::dec << " stop-reason=" << static_cast<int>(session.trace().stop_reason)
            << '\n';

  for (const auto& frame : session.trace().frames) {
    std::string symbol_name{"<unresolved>"};
    try {
      const auto symbol = session.find_frame_symbol(frame);
      if (symbol) symbol_name = symbol->name;
    } catch (const std::exception&) {
    }

    const auto delta = static_cast<std::int64_t>(ucontext_address) -
                       static_cast<std::int64_t>(frame.stack_pointer);
    std::cout << "  frame #" << frame.index << " rip=0x" << std::hex
              << frame.runtime_pc << " rsp=0x" << frame.stack_pointer << std::dec
              << " ucontext-rsp=" << delta << " module=" << frame.module_path
              << " symbol=" << symbol_name << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_signal_frame_integration <core> <fixture>\n";
    return 2;
  }

  try {
    mdbg::CoreInspectionSession session(argv[1]);
    const auto& snapshot = session.snapshot();
    require(snapshot.signal_number() == SIGSEGV,
            "signal-handler fixture core did not terminate with SIGSEGV");

    const auto ucontext_address = read_pointer(
        snapshot, runtime_symbol_address(snapshot, argv[2],
                                         "signal_core_ucontext_address"));
    const auto saved_rip = read_pointer(
        snapshot,
        runtime_symbol_address(snapshot, argv[2], "signal_core_saved_rip"));
    const auto saved_rsp = read_pointer(
        snapshot,
        runtime_symbol_address(snapshot, argv[2], "signal_core_saved_rsp"));
    require(ucontext_address != 0 && saved_rip != 0 && saved_rsp != 0,
            "kernel-provided signal ucontext oracle was not captured");

    const auto interrupted = session.find_symbol(saved_rip);
    require(interrupted.has_value() &&
                interrupted->name == "signal_core_interrupted_application",
            "saved signal RIP is not owned by the interrupted application frame");

    require(trace_has_symbol(session, "signal_core_crash_from_handler"),
            "ordinary core unwind lost the signal-handler crash frame");
    require(trace_has_symbol(session, "signal_core_handler"),
            "ordinary core unwind lost the signal handler frame");

    print_evidence(session, ucontext_address, saved_rip, saved_rsp);

    require(trace_has_symbol(session, "signal_core_interrupted_application"),
            "ordinary CFI did not cross the genuine signal frame to the interrupted application context");

    std::cout << "genuine signal-frame core integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "genuine signal-frame core integration failure: " << error.what()
              << '\n';
    return 1;
  }
  return 0;
}

#include "debugger/debugger.hpp"
#include "elf/elf.hpp"
#include "ptrace/ptrace.hpp"
#include "unwind/frame_pointer.hpp"

#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string temp_path() {
  char pattern[] = "/tmp/mdbg-backtrace-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd == -1) throw std::runtime_error("mkstemp failed");
  ::close(fd);
  ::unlink(pattern);
  return pattern;
}

void require_frame_name(const mdbg::ElfFile& elf, pid_t pid, std::uintptr_t address,
                        const std::string& expected) {
  const auto resolved = elf.find_symbol_by_runtime_address(pid, address);
  require(resolved && resolved->symbol.name == expected,
          "expected frame " + expected + " at address " + std::to_string(address));
}

void test_frame_pointer_backtrace(const std::string& fixture) {
  const std::string path = temp_path();
  try {
    auto debugger = mdbg::Debugger::launch(fixture, {path, "backtrace"});
    const auto sync = debugger.continue_execution();
    require(sync.reason == mdbg::StopReason::Signal && sync.value == SIGSTOP,
            "fixture synchronization SIGSTOP was not observed");

    const mdbg::ElfFile elf(fixture);
    const auto probe = elf.find_symbol("backtrace_probe");
    require(probe.has_value(), "backtrace_probe symbol missing from fixture");
    const auto probe_address =
        static_cast<std::uintptr_t>(elf.runtime_address(debugger.pid(), *probe));
    debugger.add_breakpoint(probe_address);

    const auto hit = debugger.continue_execution();
    require(hit.reason == mdbg::StopReason::Breakpoint &&
                hit.breakpoint_address == probe_address,
            "backtrace probe breakpoint was not hit");

    const auto trace = mdbg::unwind_frame_pointers(debugger, 16);
    require(trace.frames.size() >= 4, "frame-pointer unwind returned fewer than four frames");
    require(trace.frames[0].instruction_pointer == probe_address,
            "top backtrace frame did not preserve repaired RIP");
    require_frame_name(elf, debugger.pid(), trace.frames[1].instruction_pointer,
                       "backtrace_inner");
    require_frame_name(elf, debugger.pid(), trace.frames[2].instruction_pointer,
                       "backtrace_outer");
    require_frame_name(elf, debugger.pid(), trace.frames[3].instruction_pointer, "main");

    const auto limited = mdbg::unwind_frame_pointers(debugger, 2);
    require(limited.frames.size() == 2 &&
                limited.stop_reason == mdbg::UnwindStopReason::FrameLimit,
            "frame limit must bound the unwind deterministically");

    const auto regs = debugger.registers();
    require(regs.rbp != 0, "fixture should expose a frame pointer");
    const auto original_saved_rbp =
        mdbg::lowlevel::peek_word(debugger.pid(), static_cast<std::uintptr_t>(regs.rbp));
    mdbg::lowlevel::poke_word(debugger.pid(), static_cast<std::uintptr_t>(regs.rbp),
                              static_cast<std::uint64_t>(regs.rbp));
    const auto invalid = mdbg::unwind_frame_pointers(debugger, 16);
    mdbg::lowlevel::poke_word(debugger.pid(), static_cast<std::uintptr_t>(regs.rbp),
                              original_saved_rbp);
    require(invalid.frames.size() == 1 &&
                invalid.stop_reason == mdbg::UnwindStopReason::InvalidFramePointer,
            "self-referential RBP must stop instead of looping");
  } catch (...) {
    std::remove(path.c_str());
    throw;
  }
  std::remove(path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    test_frame_pointer_backtrace(argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "backtrace integration failure: %s\n", error.what());
    return 1;
  }
}

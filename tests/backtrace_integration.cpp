#include "debugger/debugger.hpp"
#include "dwarf/eh_frame.hpp"
#include "dwarf/line_table.hpp"
#include "dwarf/local_value.hpp"
#include "elf/elf.hpp"
#include "ptrace/ptrace.hpp"
#include "unwind/cfi.hpp"
#include "unwind/frame_pointer.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint64_t kCallerFrameLocalExpected = UINT64_C(0x6a5b4c3d2e1f9081);
volatile int exception_fixture_value = 0;

extern "C" __attribute__((noinline)) void cfi_exception_throw_helper(bool should_throw) {
  if (should_throw) throw std::runtime_error("CFI fixture exception");
  exception_fixture_value += 1;
}

extern "C" __attribute__((noinline)) void cfi_exception_middle() {
  try {
    cfi_exception_throw_helper(false);
    exception_fixture_value += 1;
  } catch (const std::exception&) {
    exception_fixture_value += 100;
  }
}

extern "C" __attribute__((noinline)) void cfi_exception_outer() {
  cfi_exception_middle();
  exception_fixture_value += 1;
}

int run_exception_fixture() {
  exception_fixture_value = 0;
  cfi_exception_outer();
  return exception_fixture_value == 3 ? 0 : 97;
}

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

void write_all(int fd, const std::string& text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto written = ::write(fd, text.data() + offset, text.size() - offset);
    if (written == -1 && errno == EINTR) continue;
    if (written <= 0) throw std::runtime_error("failed to write CLI command stream");
    offset += static_cast<std::size_t>(written);
  }
}

std::string read_all(int fd) {
  std::string output;
  char buffer[4096];
  for (;;) {
    const auto count = ::read(fd, buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count == 0) return output;
    if (count < 0) throw std::runtime_error("failed to read CLI output");
    output.append(buffer, static_cast<std::size_t>(count));
  }
}

std::size_t occurrence_count(const std::string& text, const std::string& needle) {
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

void require_frame_name(const mdbg::ElfFile& elf, pid_t pid, std::uintptr_t address,
                        const std::string& expected) {
  const auto resolved = elf.find_symbol_by_runtime_address(pid, address);
  require(resolved && resolved->symbol.name == expected,
          "expected frame " + expected + " at address " + std::to_string(address));
}

std::uintptr_t source_address(const mdbg::DwarfLineTable& lines, const mdbg::ElfFile& elf,
                              const mdbg::Debugger& debugger, std::uint64_t line) {
  const auto address = lines.find_runtime_source(debugger.pid(), "next_source.c", line, elf);
  require(address.has_value(), "backtrace fixture is missing source line " +
                                   std::to_string(line));
  return static_cast<std::uintptr_t>(*address);
}

void test_frame_pointer_backtrace(const std::string& fixture) {
  const std::string path = temp_path();
  try {
    auto debugger = mdbg::Debugger::launch(fixture, {path, "backtrace-repeat"});
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

    const mdbg::EhFrame cfi(fixture);
    require(cfi.available(), "backtrace fixture is missing .eh_frame for inspection frames");
    const auto live_before = debugger.registers();
    const auto inspection_frames = mdbg::build_inspection_frames(debugger, elf, cfi, 4);
    require(inspection_frames.size() >= 2,
            "inspection-frame unwind did not recover the immediate caller");
    require(inspection_frames[0].index == 0 &&
                inspection_frames[0].process_pid == debugger.pid() &&
                inspection_frames[0].tid == debugger.active_tid() &&
                inspection_frames[0].runtime_pc == probe_address,
            "frame 0 does not preserve live execution ownership");
    require(inspection_frames[1].index == 1 &&
                inspection_frames[1].process_pid == debugger.pid() &&
                inspection_frames[1].tid == debugger.active_tid(),
            "caller frame does not preserve process/TID ownership");
    require(inspection_frames[0].origin_stop_sequence == debugger.stop_info().sequence &&
                inspection_frames[1].origin_stop_sequence == debugger.stop_info().sequence,
            "inspection frames did not bind to the originating stop identity");
    require_frame_name(elf, debugger.pid(), inspection_frames[1].runtime_pc,
                       "backtrace_inner");
    require(inspection_frames[1].registers.rbp.has_value(),
            "caller frame did not retain CFI-recovered frame-pointer state");
    require(!inspection_frames[1].registers.rdi.has_value(),
            "caller frame must not invent an unrecovered historical argument register");

    bool frame_zero_rejected_caller_local = false;
    try {
      (void)mdbg::inspect_local_value(debugger, elf, inspection_frames[0],
                                      "caller_frame_local");
    } catch (const std::runtime_error&) {
      frame_zero_rejected_caller_local = true;
    }
    require(frame_zero_rejected_caller_local,
            "frame 0 incorrectly resolved a caller-owned local variable");

    const auto caller_value =
        mdbg::inspect_local_value(debugger, elf, inspection_frames[1], "caller_frame_local");
    require(caller_value.kind == mdbg::LocalValueKind::Integer &&
                caller_value.raw_value == kCallerFrameLocalExpected &&
                caller_value.byte_size == sizeof(std::uint64_t),
            "caller-frame source-value inspection returned the wrong stack local");

    const auto live_after = debugger.registers();
    require(live_after.rip == live_before.rip && live_after.rsp == live_before.rsp &&
                live_after.rbp == live_before.rbp && live_after.rdi == live_before.rdi,
            "caller-frame inspection mutated or redirected live execution registers");

    const auto second_hit = debugger.continue_execution();
    require(second_hit.reason == mdbg::StopReason::Breakpoint &&
                second_hit.breakpoint_address == probe_address,
            "repeated backtrace probe breakpoint was not hit");
    const auto repeated_live = debugger.registers();
    require(repeated_live.rip == live_before.rip && repeated_live.rsp == live_before.rsp &&
                repeated_live.rbp == live_before.rbp,
            "repeat fixture did not recreate the identical RIP/RSP/RBP stop fingerprint");
    require(debugger.stop_info().sequence != inspection_frames[1].origin_stop_sequence,
            "new breakpoint stop did not receive a distinct stop identity");

    bool stale_rejected = false;
    try {
      (void)mdbg::inspect_local_value(debugger, elf, inspection_frames[1],
                                      "caller_frame_local");
    } catch (const std::logic_error&) {
      stale_rejected = true;
    }
    require(stale_rejected,
            "caller inspection frame survived a new stop with an identical register fingerprint");

    const auto fresh_frames = mdbg::build_inspection_frames(debugger, elf, cfi, 4);
    require(fresh_frames.size() >= 2 &&
                fresh_frames[1].origin_stop_sequence == debugger.stop_info().sequence,
            "fresh caller frame did not bind to the repeated stop identity");
    const auto fresh_value =
        mdbg::inspect_local_value(debugger, elf, fresh_frames[1], "caller_frame_local");
    require(fresh_value.kind == mdbg::LocalValueKind::Integer &&
                fresh_value.raw_value == kCallerFrameLocalExpected,
            "fresh caller-frame source-value inspection failed after repeated stop");

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

void test_cfi_backtrace_without_frame_pointer(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {});
  const mdbg::ElfFile elf(fixture);
  const mdbg::DwarfLineTable lines(fixture);
  const mdbg::EhFrame cfi(fixture);
  require(cfi.available(), "omit-frame-pointer fixture is missing .eh_frame");

  const auto line510 = source_address(lines, elf, debugger, 510);
  debugger.add_breakpoint(line510);
  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint && hit.breakpoint_address == line510,
          "CFI backtrace start breakpoint was not hit");

  auto regs = debugger.registers();
  regs.rbp = 3;
  mdbg::lowlevel::set_registers(debugger.pid(), regs);

  const auto trace = mdbg::unwind_eh_frame(debugger, elf, cfi, 16);
  require(trace.frames.size() >= 4, "CFI unwind returned fewer than four frames");
  require(trace.frames[0].instruction_pointer == line510,
          "CFI top frame did not preserve repaired RIP");
  require_frame_name(elf, debugger.pid(), trace.frames[0].instruction_pointer, "next_callee");
  require_frame_name(elf, debugger.pid(), trace.frames[1].instruction_pointer, "next_caller");
  require_frame_name(elf, debugger.pid(), trace.frames[2].instruction_pointer, "next_outer");
  require_frame_name(elf, debugger.pid(), trace.frames[3].instruction_pointer, "main");

  const auto limited = mdbg::unwind_eh_frame(debugger, elf, cfi, 2);
  require(limited.frames.size() == 2 &&
              limited.stop_reason == mdbg::CfiUnwindStopReason::FrameLimit,
          "CFI frame limit must bound the unwind deterministically");

  const auto original = debugger.registers();
  auto broken = original;
  broken.rsp = 8;
  mdbg::lowlevel::set_registers(debugger.pid(), broken);
  const auto invalid = mdbg::unwind_eh_frame(debugger, elf, cfi, 16);
  mdbg::lowlevel::set_registers(debugger.pid(), original);
  require(invalid.frames.size() == 1 &&
              invalid.stop_reason == mdbg::CfiUnwindStopReason::InvalidFrameState,
          "unreadable CFI stack state must return a bounded partial trace");
}

void test_compiler_exception_cfi(const std::string& fixture) {
  auto debugger = mdbg::Debugger::launch(fixture, {"--exception-fixture"});
  const mdbg::ElfFile elf(fixture);
  const mdbg::EhFrame cfi(fixture);
  require(cfi.available(), "C++ exception fixture is missing .eh_frame");

  const auto middle = elf.find_symbol("cfi_exception_middle");
  require(middle.has_value(), "cfi_exception_middle symbol missing from fixture");
  const auto middle_address =
      static_cast<std::uintptr_t>(elf.runtime_address(debugger.pid(), *middle));
  debugger.add_breakpoint(middle_address);

  const auto hit = debugger.continue_execution();
  require(hit.reason == mdbg::StopReason::Breakpoint &&
              hit.breakpoint_address == middle_address,
          "exception CFI breakpoint was not hit");

  const auto caller = cfi.caller_return_address(debugger, elf);
  require(caller.has_value(), "exception CFI did not recover a caller return address");
  require_frame_name(elf, debugger.pid(), *caller, "cfi_exception_outer");

  const auto done = debugger.continue_execution();
  require(done.reason == mdbg::StopReason::Exited && done.value == 0,
          "exception CFI fixture did not exit cleanly");
}

void test_cli_source_context(const std::string& integration_path,
                             const std::string& fixture) {
  const auto mdbg_path =
      (std::filesystem::absolute(integration_path).parent_path() / "mdbg").string();
  require(std::filesystem::exists(mdbg_path), "mdbg executable is missing beside integration test");

  const std::string path = temp_path();
  int input_pipe[2] = {-1, -1};
  int output_pipe[2] = {-1, -1};
  require(::pipe(input_pipe) == 0, "failed to create CLI stdin pipe");
  require(::pipe(output_pipe) == 0, "failed to create CLI stdout pipe");

  const pid_t child = ::fork();
  require(child != -1, "failed to fork CLI integration child");
  if (child == 0) {
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    ::execl(mdbg_path.c_str(), mdbg_path.c_str(), fixture.c_str(), path.c_str(),
            "backtrace", nullptr);
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  try {
    write_all(input_pipe[1],
              "break backtrace_leaf\n"
              "continue\n"
              "continue\n"
              "list\n"
              "step\n"
              "quit\n");
    ::close(input_pipe[1]);
    input_pipe[1] = -1;

    const auto output = read_all(output_pipe[0]);
    ::close(output_pipe[0]);
    output_pipe[0] = -1;

    int status = 0;
    pid_t waited;
    do {
      waited = ::waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);
    require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "mdbg CLI source-context workflow did not exit cleanly\n" + output);
    require(output.find("Breakpoint 1") != std::string::npos,
            "CLI did not install the source-context probe breakpoint\n" + output);
    require(output.find("breakpoint at") != std::string::npos,
            "CLI did not reach the source-context probe breakpoint\n" + output);
    require(output.find("backtrace_leaf(void)") != std::string::npos,
            "CLI source context did not render real source text\n" + output);
    require(output.find("fixture_value += 1;") != std::string::npos,
            "source-step context did not render the advanced source line\n" + output);
    require(occurrence_count(output, "=> ") >= 2,
            "manual list and source step did not share current-line context rendering\n" + output);
  } catch (...) {
    if (input_pipe[1] != -1) ::close(input_pipe[1]);
    if (output_pipe[0] != -1) ::close(output_pipe[0]);
    ::kill(child, SIGKILL);
    int status = 0;
    while (::waitpid(child, &status, 0) == -1 && errno == EINTR) {
    }
    std::remove(path.c_str());
    throw;
  }
  std::remove(path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--exception-fixture") {
    return run_exception_fixture();
  }
  if (argc != 3) return 2;
  try {
    test_frame_pointer_backtrace(argv[1]);
    test_cfi_backtrace_without_frame_pointer(argv[2]);
    test_compiler_exception_cfi(argv[0]);
    test_cli_source_context(argv[0], argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "backtrace integration failure: %s\n", error.what());
    return 1;
  }
}

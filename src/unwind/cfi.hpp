#pragma once

#include "dwarf/eh_frame.hpp"

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mdbg {

class Debugger;
class ElfFile;

enum class CfiUnwindStopReason {
  EndOfChain,
  FrameLimit,
  NoFrameInfo,
  InvalidFrameState,
};

struct CfiStackFrame {
  std::uintptr_t instruction_pointer;
  std::uintptr_t stack_pointer;
  std::optional<std::uintptr_t> frame_pointer;
};

struct CfiBacktrace {
  std::vector<CfiStackFrame> frames;
  CfiUnwindStopReason stop_reason;
};

struct InspectionRegisterState {
  std::optional<std::uint64_t> rax;
  std::optional<std::uint64_t> rbx;
  std::optional<std::uint64_t> rcx;
  std::optional<std::uint64_t> rdx;
  std::optional<std::uint64_t> rsi;
  std::optional<std::uint64_t> rdi;
  std::optional<std::uint64_t> rbp;
  std::optional<std::uint64_t> rsp;
  std::optional<std::uint64_t> r8;
  std::optional<std::uint64_t> r9;
  std::optional<std::uint64_t> r10;
  std::optional<std::uint64_t> r11;
  std::optional<std::uint64_t> r12;
  std::optional<std::uint64_t> r13;
  std::optional<std::uint64_t> r14;
  std::optional<std::uint64_t> r15;
};

struct InspectionFrameContext {
  std::size_t index;
  pid_t process_pid;
  pid_t tid;
  std::uintptr_t origin_runtime_pc;
  std::uintptr_t origin_stack_pointer;
  std::optional<std::uintptr_t> origin_frame_pointer;
  std::uintptr_t runtime_pc;
  std::uintptr_t stack_pointer;
  std::optional<std::uintptr_t> frame_pointer;
  std::string module_path;
  InspectionRegisterState registers;
};

struct ModuleResolvedSymbol {
  std::string module_path;
  std::string name;
  std::uint64_t offset;
};

struct ModuleResolvedSymbolAddress {
  std::string module_path;
  std::string name;
  std::uintptr_t address;
};

struct ModuleResolvedSource {
  std::string module_path;
  std::string file;
  std::uint64_t line;
  std::uint64_t column;
};

struct ModuleResolvedSourceAddress {
  std::string module_path;
  std::string file;
  std::uint64_t line;
  std::uintptr_t address;
};

std::optional<ModuleResolvedSymbol> find_module_symbol_by_runtime_address(
    pid_t pid, std::uintptr_t address, const ElfFile& preferred_elf);
std::optional<ModuleResolvedSymbolAddress> find_module_symbol_by_name(
    pid_t pid, std::string_view name, const ElfFile& preferred_elf);
std::optional<ModuleResolvedSource> find_module_source_by_runtime_address(
    pid_t pid, std::uintptr_t address, const ElfFile& preferred_elf);
std::optional<ModuleResolvedSourceAddress> find_module_source_by_file_line(
    pid_t pid, std::string_view file, std::uint64_t line, const ElfFile& preferred_elf);
std::optional<std::uintptr_t> module_caller_return_address(
    const Debugger& debugger, const ElfFile& preferred_elf, const EhFrame& preferred_cfi);
InspectionFrameContext current_inspection_frame(const Debugger& debugger,
                                                const ElfFile& preferred_elf);
std::vector<InspectionFrameContext> build_inspection_frames(
    const Debugger& debugger, const ElfFile& preferred_elf,
    const EhFrame& preferred_cfi, std::size_t max_frames = 64);
CfiBacktrace unwind_eh_frame(const Debugger& debugger, const ElfFile& elf,
                             const EhFrame& cfi, std::size_t max_frames = 64);
const char* cfi_unwind_stop_reason_name(CfiUnwindStopReason reason) noexcept;

}  // namespace mdbg

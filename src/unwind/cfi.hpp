#pragma once

#include "dwarf/eh_frame.hpp"

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
CfiBacktrace unwind_eh_frame(const Debugger& debugger, const ElfFile& elf,
                             const EhFrame& cfi, std::size_t max_frames = 64);
const char* cfi_unwind_stop_reason_name(CfiUnwindStopReason reason) noexcept;

}  // namespace mdbg

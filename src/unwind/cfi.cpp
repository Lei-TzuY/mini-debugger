#include "unwind/cfi.hpp"

#include "debugger/debugger.hpp"
#include "elf/elf.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mdbg {
namespace {

std::string trim_left(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });
  value.erase(value.begin(), first);
  return value;
}

std::optional<std::string> mapped_module_path(pid_t pid, std::uintptr_t address) {
  std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
  if (!maps) throw std::runtime_error("failed to open tracee memory map for CFI unwind");

  std::string line;
  while (std::getline(maps, line)) {
    std::istringstream fields(line);
    std::string range, permissions, offset, device, inode;
    if (!(fields >> range >> permissions >> offset >> device >> inode)) continue;
    const auto dash = range.find('-');
    if (dash == std::string::npos) continue;

    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    try {
      begin = static_cast<std::uintptr_t>(std::stoull(range.substr(0, dash), nullptr, 16));
      end = static_cast<std::uintptr_t>(std::stoull(range.substr(dash + 1), nullptr, 16));
    } catch (const std::exception&) {
      continue;
    }
    if (address < begin || address >= end) continue;

    std::string path;
    std::getline(fields, path);
    path = trim_left(std::move(path));
    if (path.empty() || path.front() != '/') return std::nullopt;
    constexpr const char* deleted_suffix = " (deleted)";
    if (path.size() >= 10 && path.compare(path.size() - 10, 10, deleted_suffix) == 0) {
      return std::nullopt;
    }
    return path;
  }
  return std::nullopt;
}

bool same_file(const std::string& left, const std::string& right) {
  std::error_code error;
  const bool equivalent = std::filesystem::equivalent(left, right, error);
  if (!error) return equivalent;
  return left == right;
}

struct ModuleCfi {
  explicit ModuleCfi(std::string module_path)
      : path(std::move(module_path)), elf(path), cfi(path) {}

  std::string path;
  ElfFile elf;
  EhFrame cfi;
};

ModuleCfi& cached_module(std::vector<ModuleCfi>& modules, const std::string& path) {
  for (auto& module : modules) {
    if (same_file(module.path, path)) return module;
  }
  modules.emplace_back(path);
  return modules.back();
}

std::optional<EhFrameCursor> caller_for_cursor(
    const Debugger& debugger, const ElfFile& preferred_elf, const EhFrame& preferred_cfi,
    const EhFrameCursor& current, std::vector<ModuleCfi>& modules) {
  const auto path = mapped_module_path(debugger.pid(), current.instruction_pointer);
  if (!path) return std::nullopt;

  if (same_file(*path, preferred_elf.path())) {
    if (!preferred_cfi.available()) return std::nullopt;
    return preferred_cfi.caller_frame(debugger, preferred_elf, current);
  }

  auto& module = cached_module(modules, *path);
  if (!module.cfi.available()) return std::nullopt;
  return module.cfi.caller_frame(debugger, module.elf, current);
}

}  // namespace

std::optional<ModuleResolvedSymbol> find_module_symbol_by_runtime_address(
    pid_t pid, std::uintptr_t address, const ElfFile& preferred_elf) {
  const auto path = mapped_module_path(pid, address);
  if (!path) return std::nullopt;

  const auto resolve = [&](const ElfFile& elf) -> std::optional<ModuleResolvedSymbol> {
    const auto symbol = elf.find_symbol_by_runtime_address(pid, address);
    if (!symbol) return std::nullopt;
    return ModuleResolvedSymbol{*path, symbol->symbol.name, symbol->offset};
  };

  if (same_file(*path, preferred_elf.path())) return resolve(preferred_elf);
  const ElfFile module(*path);
  return resolve(module);
}

std::optional<std::uintptr_t> module_caller_return_address(
    const Debugger& debugger, const ElfFile& preferred_elf, const EhFrame& preferred_cfi) {
  if (debugger.state() != ProcessState::Stopped) {
    throw std::logic_error("CFI return-address recovery requires a stopped tracee");
  }

  const auto regs = debugger.registers();
  const auto path = mapped_module_path(debugger.pid(), static_cast<std::uintptr_t>(regs.rip));
  if (!path) return std::nullopt;

  if (same_file(*path, preferred_elf.path())) {
    if (!preferred_cfi.available()) return std::nullopt;
    return preferred_cfi.caller_return_address(debugger, preferred_elf);
  }

  ModuleCfi module(*path);
  if (!module.cfi.available()) return std::nullopt;
  return module.cfi.caller_return_address(debugger, module.elf);
}

CfiBacktrace unwind_eh_frame(const Debugger& debugger, const ElfFile& elf,
                             const EhFrame& cfi, std::size_t max_frames) {
  if (max_frames == 0) {
    throw std::invalid_argument("CFI unwind requires a non-zero frame limit");
  }
  if (debugger.state() != ProcessState::Stopped) {
    throw std::logic_error("CFI unwind requires a stopped tracee");
  }

  const auto regs = debugger.registers();
  EhFrameCursor current{static_cast<std::uintptr_t>(regs.rip),
                        static_cast<std::uintptr_t>(regs.rsp),
                        static_cast<std::uintptr_t>(regs.rbp)};
  CfiBacktrace result{{CfiStackFrame{current.instruction_pointer, current.stack_pointer,
                                    current.frame_pointer}},
                      CfiUnwindStopReason::EndOfChain};
  if (max_frames == 1) {
    result.stop_reason = CfiUnwindStopReason::FrameLimit;
    return result;
  }

  std::vector<ModuleCfi> modules;
  while (result.frames.size() < max_frames) {
    std::optional<EhFrameCursor> caller;
    try {
      caller = caller_for_cursor(debugger, elf, cfi, current, modules);
    } catch (const std::exception&) {
      result.stop_reason = CfiUnwindStopReason::InvalidFrameState;
      return result;
    }

    if (!caller) {
      result.stop_reason = result.frames.size() == 1 ? CfiUnwindStopReason::NoFrameInfo
                                                     : CfiUnwindStopReason::EndOfChain;
      return result;
    }
    if (caller->instruction_pointer == current.instruction_pointer ||
        caller->stack_pointer <= current.stack_pointer) {
      result.stop_reason = CfiUnwindStopReason::InvalidFrameState;
      return result;
    }

    result.frames.push_back(CfiStackFrame{caller->instruction_pointer, caller->stack_pointer,
                                          caller->frame_pointer});
    current = *caller;
  }

  result.stop_reason = CfiUnwindStopReason::FrameLimit;
  return result;
}

const char* cfi_unwind_stop_reason_name(CfiUnwindStopReason reason) noexcept {
  switch (reason) {
    case CfiUnwindStopReason::EndOfChain:
      return "end of CFI chain";
    case CfiUnwindStopReason::FrameLimit:
      return "frame limit reached";
    case CfiUnwindStopReason::NoFrameInfo:
      return "no CFI frame information";
    case CfiUnwindStopReason::InvalidFrameState:
      return "invalid or unsupported CFI frame state";
  }
  return "unknown CFI unwind stop";
}

}  // namespace mdbg

#include "debugger/debugger.hpp"
#include "dwarf/eh_frame.hpp"
#include "dwarf/line_table.hpp"
#include "elf/elf.hpp"
#include "registers/registers.hpp"
#include "source/source_finish.hpp"
#include "source/source_step.hpp"
#include "unwind/cfi.hpp"
#include "unwind/frame_pointer.hpp"

#include <elf.h>

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct SourceSpec {
  std::string file;
  std::uint64_t line;
};

std::optional<std::uintptr_t> try_parse_address(const std::string& text) {
  try {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed, 0);
    if (consumed == text.size()) return static_cast<std::uintptr_t>(value);
  } catch (const std::exception&) {
  }
  return std::nullopt;
}

std::optional<SourceSpec> try_parse_source_spec(const std::string& text) {
  const auto separator = text.rfind(':');
  if (separator == std::string::npos || separator == 0 || separator + 1 >= text.size()) {
    return std::nullopt;
  }
  try {
    std::size_t consumed = 0;
    const auto line = std::stoull(text.substr(separator + 1), &consumed, 10);
    if (consumed != text.size() - separator - 1 || line == 0) return std::nullopt;
    return SourceSpec{text.substr(0, separator), line};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

pid_t parse_pid(const std::string& text) {
  std::size_t consumed = 0;
  const auto value = std::stoll(text, &consumed, 10);
  if (consumed != text.size() || value <= 0 ||
      value > static_cast<long long>(std::numeric_limits<pid_t>::max())) {
    throw std::invalid_argument("invalid pid: " + text);
  }
  return static_cast<pid_t>(value);
}

std::string process_executable(pid_t pid) {
  return std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe").string();
}

std::uintptr_t resolve_location(const std::string& text, const mdbg::ElfFile& elf, pid_t pid) {
  if (const auto address = try_parse_address(text)) return *address;
  const auto symbol = elf.find_symbol(text);
  if (!symbol) throw std::invalid_argument("unknown symbol: " + text);
  return static_cast<std::uintptr_t>(elf.runtime_address(pid, *symbol));
}

std::uintptr_t resolve_break_location(const std::string& text, const std::string& executable,
                                      const mdbg::ElfFile& elf, pid_t pid) {
  if (const auto address = try_parse_address(text)) return *address;
  if (const auto symbol = elf.find_symbol(text)) {
    return static_cast<std::uintptr_t>(elf.runtime_address(pid, *symbol));
  }
  if (const auto source = try_parse_source_spec(text)) {
    const mdbg::DwarfLineTable lines(executable);
    const auto address = lines.find_runtime_source(pid, source->file, source->line, elf);
    if (!address) {
      throw std::invalid_argument("no executable address for source location: " + text);
    }
    return static_cast<std::uintptr_t>(*address);
  }
  throw std::invalid_argument("unknown symbol or source location: " + text);
}

void print_stop(const mdbg::StopInfo& info, const mdbg::ElfFile& elf, pid_t pid) {
  using mdbg::StopReason;
  switch (info.reason) {
    case StopReason::InitialExec:
      std::cout << "stopped after exec\n";
      break;
    case StopReason::Attached:
      std::cout << "attached to process " << pid << '\n';
      break;
    case StopReason::Breakpoint: {
      std::cout << "breakpoint at 0x" << std::hex << *info.breakpoint_address << std::dec;
      if (const auto symbol = elf.find_symbol_by_runtime_address(pid, *info.breakpoint_address)) {
        std::cout << " (" << symbol->symbol.name;
        if (symbol->offset != 0) std::cout << "+0x" << std::hex << symbol->offset << std::dec;
        std::cout << ')';
      }
      std::cout << '\n';
      break;
    }
    case StopReason::SingleStep:
      std::cout << "single-step trap\n";
      break;
    case StopReason::Signal:
      std::cout << "stopped by signal " << info.value << '\n';
      break;
    case StopReason::Trap:
      std::cout << "SIGTRAP (not a managed breakpoint)\n";
      break;
    case StopReason::Exited:
      std::cout << "process exited with code " << info.value << '\n';
      break;
    case StopReason::Signaled:
      std::cout << "process terminated by signal " << info.value << '\n';
      break;
  }
}

void print_symbolized_frame(std::size_t index, std::uintptr_t address,
                            const mdbg::Debugger& debugger, const mdbg::ElfFile& elf) {
  std::cout << '#' << index << " 0x" << std::hex << address << std::dec;
  try {
    if (const auto symbol =
            mdbg::find_module_symbol_by_runtime_address(debugger.pid(), address, elf)) {
      std::cout << ' ' << symbol->name;
      if (symbol->offset != 0) {
        std::cout << "+0x" << std::hex << symbol->offset << std::dec;
      }
    }
  } catch (const std::exception&) {
  }
  std::cout << '\n';
}

void print_backtrace(const mdbg::Debugger& debugger, const mdbg::ElfFile& elf) {
  try {
    const mdbg::EhFrame cfi(elf.path());
    if (cfi.available()) {
      const auto trace = mdbg::unwind_eh_frame(debugger, elf, cfi);
      if (trace.stop_reason != mdbg::CfiUnwindStopReason::NoFrameInfo) {
        for (std::size_t index = 0; index < trace.frames.size(); ++index) {
          print_symbolized_frame(index, trace.frames[index].instruction_pointer, debugger, elf);
        }
        if (trace.stop_reason != mdbg::CfiUnwindStopReason::EndOfChain) {
          std::cout << "backtrace stopped: "
                    << mdbg::cfi_unwind_stop_reason_name(trace.stop_reason) << '\n';
        }
        return;
      }
    }
  } catch (const std::exception& error) {
    std::cout << "backtrace stopped: CFI unavailable: " << error.what() << '\n';
    return;
  }

  const auto trace = mdbg::unwind_frame_pointers(debugger);
  for (std::size_t index = 0; index < trace.frames.size(); ++index) {
    print_symbolized_frame(index, trace.frames[index].instruction_pointer, debugger, elf);
  }
  if (trace.stop_reason != mdbg::UnwindStopReason::EndOfChain) {
    std::cout << "backtrace stopped: " << mdbg::unwind_stop_reason_name(trace.stop_reason) << '\n';
  }
}

void print_source_position(std::uintptr_t address, const mdbg::SourceLocation& source) {
  std::cout << "0x" << std::hex << address << std::dec << ' ' << source.file << ':'
            << source.line;
  if (source.column != 0) std::cout << ':' << source.column;
  std::cout << '\n';
}

void print_source_motion_result(const char* operation, const mdbg::SourceStepResult& result,
                                const mdbg::Debugger& debugger, const mdbg::ElfFile& elf) {
  if (result.reason == mdbg::SourceStepStopReason::LineChanged) {
    print_source_position(static_cast<std::uintptr_t>(debugger.registers().rip), *result.source);
    return;
  }
  if (result.reason == mdbg::SourceStepStopReason::Interrupted) {
    print_stop(result.stop, elf, debugger.pid());
    if (debugger.state() == mdbg::ProcessState::Stopped && result.source) {
      print_source_position(static_cast<std::uintptr_t>(debugger.registers().rip), *result.source);
    }
    return;
  }

  std::cout << operation << " stopped after " << result.instructions
            << " instructions: instruction limit reached\n";
  if (result.source) {
    print_source_position(static_cast<std::uintptr_t>(debugger.registers().rip), *result.source);
  }
}

void print_source_location(const std::string& executable, std::uintptr_t address,
                           const mdbg::Debugger& debugger, const mdbg::ElfFile& elf) {
  try {
    const mdbg::DwarfLineTable lines(executable);
    if (!lines.available()) {
      std::cout << "no DWARF line table\n";
      return;
    }
    const auto source = lines.find_runtime_address(debugger.pid(), address, elf);
    if (!source) {
      std::cout << "no source location for 0x" << std::hex << address << std::dec << '\n';
      return;
    }
    print_source_position(address, *source);
  } catch (const std::exception& error) {
    std::cout << "line info unavailable: " << error.what() << '\n';
  }
}

void print_usage() {
  std::cerr << "usage: mdbg <program> [args...]\n"
               "       mdbg --attach <pid>\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || (std::string(argv[1]) == "--attach" && argc != 3)) {
    print_usage();
    return 2;
  }

  try {
    const bool attach_mode = std::string(argv[1]) == "--attach";
    std::string executable;
    std::vector<std::string> args;

    auto debugger = [&]() -> mdbg::Debugger {
      if (attach_mode) {
        const pid_t pid = parse_pid(argv[2]);
        executable = process_executable(pid);
        return mdbg::Debugger::attach(pid);
      }

      executable = argv[1];
      for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);
      return mdbg::Debugger::launch(executable, args);
    }();

    const mdbg::ElfFile elf(executable);
    print_stop(debugger.stop_info(), elf, debugger.pid());

    std::string line;
    while (debugger.state() == mdbg::ProcessState::Stopped &&
           std::cout << "(mdbg) " && std::getline(std::cin, line)) {
      std::istringstream input(line);
      std::string command;
      input >> command;
      if (command.empty()) continue;
      if (command == "quit" || command == "q") {
        if (debugger.origin() == mdbg::ProcessOrigin::Attached) {
          debugger.detach();
          std::cout << "detached\n";
        }
        break;
      }
      if (command == "detach") {
        if (debugger.origin() != mdbg::ProcessOrigin::Attached) {
          std::cout << "detach is only valid for an attached process\n";
          continue;
        }
        debugger.detach();
        std::cout << "detached\n";
        break;
      }
      if (command == "continue" || command == "c") {
        print_stop(debugger.continue_execution(), elf, debugger.pid());
      } else if (command == "stepi" || command == "si") {
        print_stop(debugger.single_step(), elf, debugger.pid());
      } else if (command == "step" || command == "s" || command == "next" || command == "n") {
        const bool next = command == "next" || command == "n";
        try {
          const mdbg::DwarfLineTable lines(executable);
          if (!lines.available()) {
            std::cout << "no DWARF line table\n";
            continue;
          }
          const auto result = next ? mdbg::next_source(debugger, lines, elf)
                                   : mdbg::step_source(debugger, lines, elf);
          print_source_motion_result(next ? "source next" : "source step", result, debugger, elf);
        } catch (const std::exception& error) {
          std::cout << (next ? "source next unavailable: " : "source step unavailable: ")
                    << error.what() << '\n';
        }
      } else if (command == "finish" || command == "fin") {
        try {
          const auto result = mdbg::finish_frame(debugger);
          if (result.reason == mdbg::FinishStopReason::Returned) {
            if (debugger.state() == mdbg::ProcessState::Stopped) {
              print_source_location(executable,
                                    static_cast<std::uintptr_t>(debugger.registers().rip),
                                    debugger, elf);
            } else {
              print_stop(result.stop, elf, debugger.pid());
            }
          } else {
            print_stop(result.stop, elf, debugger.pid());
            if (debugger.state() == mdbg::ProcessState::Stopped) {
              print_source_location(executable,
                                    static_cast<std::uintptr_t>(debugger.registers().rip),
                                    debugger, elf);
            }
          }
        } catch (const std::exception& error) {
          std::cout << "finish unavailable: " << error.what() << '\n';
        }
      } else if (command == "regs") {
        for (const auto& [name, value] : mdbg::general_purpose_registers(debugger.registers())) {
          std::cout << std::setw(6) << name << " 0x" << std::hex << value << std::dec << '\n';
        }
      } else if (command == "bt") {
        print_backtrace(debugger, elf);
      } else if (command == "line") {
        std::string location;
        input >> location;
        if (location.empty()) {
          std::cout << "usage: line <address|symbol>\n";
          continue;
        }
        const auto address = resolve_location(location, elf, debugger.pid());
        print_source_location(executable, address, debugger, elf);
      } else if (command == "reg") {
        std::string name;
        input >> name;
        const auto value = mdbg::register_value(debugger.registers(), name);
        if (!value) std::cout << "unknown register\n";
        else std::cout << name << " = 0x" << std::hex << *value << std::dec << '\n';
      } else if (command == "x") {
        std::string address_text;
        std::size_t length = 8;
        input >> address_text >> length;
        const auto address = resolve_location(address_text, elf, debugger.pid());
        const auto bytes = debugger.read_memory(address, length);
        std::cout << "0x" << std::hex << address << ":";
        for (const auto byte : bytes) {
          std::cout << ' ' << std::setw(2) << std::setfill('0')
                    << std::to_integer<unsigned>(byte);
        }
        std::cout << std::setfill(' ') << std::dec << '\n';
      } else if (command == "break" || command == "b") {
        std::string location;
        input >> location;
        if (location.empty()) {
          std::cout << "usage: break <address|symbol|file:line>\n";
          continue;
        }
        const auto address = resolve_break_location(location, executable, elf, debugger.pid());
        const auto id = debugger.add_breakpoint(address);
        std::cout << "Breakpoint " << id << " at 0x" << std::hex << address << std::dec;
        if (!try_parse_address(location)) std::cout << " (" << location << ')';
        std::cout << '\n';
      } else if (command == "delete") {
        std::size_t id = 0;
        input >> id;
        if (!debugger.remove_breakpoint(id)) std::cout << "no such breakpoint\n";
      } else if (command == "info") {
        std::string topic;
        input >> topic;
        if (topic != "breakpoints") {
          std::cout << "usage: info breakpoints\n";
          continue;
        }
        for (const auto& bp : debugger.breakpoints()) {
          std::cout << bp.id << " 0x" << std::hex << bp.address << std::dec
                    << (bp.installed ? " enabled" : " temporarily-restored") << '\n';
        }
      } else if (command == "symbols") {
        std::string filter;
        input >> filter;
        for (const auto& symbol : elf.symbols()) {
          if (symbol.type != STT_FUNC) continue;
          if (!filter.empty() && symbol.name.find(filter) == std::string::npos) continue;
          std::cout << "0x" << std::hex << elf.runtime_address(debugger.pid(), symbol)
                    << std::dec << ' ' << symbol.name << '\n';
        }
      } else {
        std::cout << "commands: continue, step, next, finish, stepi, regs, bt, "
                     "line <addr|symbol>, reg <name>, x <addr|symbol> [len], "
                     "break <addr|symbol|file:line>, delete <id>, info breakpoints, "
                     "symbols [filter], detach, quit\n";
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "mdbg: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

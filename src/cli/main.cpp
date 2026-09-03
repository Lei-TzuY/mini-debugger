#include "debugger/debugger.hpp"
#include "elf/elf.hpp"
#include "registers/registers.hpp"
#include "unwind/frame_pointer.hpp"

#include <elf.h>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::optional<std::uintptr_t> try_parse_address(const std::string& text) {
  try {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed, 0);
    if (consumed == text.size()) return static_cast<std::uintptr_t>(value);
  } catch (const std::exception&) {
  }
  return std::nullopt;
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

void print_backtrace(const mdbg::Debugger& debugger, const mdbg::ElfFile& elf) {
  const auto trace = mdbg::unwind_frame_pointers(debugger);
  for (std::size_t index = 0; index < trace.frames.size(); ++index) {
    const auto address = trace.frames[index].instruction_pointer;
    std::cout << '#' << index << " 0x" << std::hex << address << std::dec;
    if (const auto symbol = elf.find_symbol_by_runtime_address(debugger.pid(), address)) {
      std::cout << ' ' << symbol->symbol.name;
      if (symbol->offset != 0) {
        std::cout << "+0x" << std::hex << symbol->offset << std::dec;
      }
    }
    std::cout << '\n';
  }
  if (trace.stop_reason != mdbg::UnwindStopReason::EndOfChain) {
    std::cout << "backtrace stopped: " << mdbg::unwind_stop_reason_name(trace.stop_reason) << '\n';
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
      } else if (command == "regs") {
        for (const auto& [name, value] : mdbg::general_purpose_registers(debugger.registers())) {
          std::cout << std::setw(6) << name << " 0x" << std::hex << value << std::dec << '\n';
        }
      } else if (command == "bt") {
        print_backtrace(debugger, elf);
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
        const auto address = resolve_location(location, elf, debugger.pid());
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
        std::cout << "commands: continue, stepi, regs, bt, reg <name>, x <addr|symbol> [len], "
                     "break <addr|symbol>, delete <id>, info breakpoints, symbols [filter], "
                     "detach, quit\n";
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "mdbg: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

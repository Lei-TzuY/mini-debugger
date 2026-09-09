#include "snapshot/session.hpp"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void print_frame(const mdbg::CoreInspectionSession& session,
                 const mdbg::SnapshotInspectionFrameContext& frame) {
  std::cout << (frame.index == session.selected_frame_index() ? "* " : "  ") << '#'
            << frame.index << " 0x" << std::hex << frame.runtime_pc << std::dec << ' '
            << frame.module_path;
  try {
    if (const auto symbol = session.find_symbol(frame.runtime_pc)) {
      std::cout << '!' << symbol->name;
      if (symbol->offset != 0) {
        std::cout << "+0x" << std::hex << symbol->offset << std::dec;
      }
    }
  } catch (const std::exception&) {
  }
  try {
    if (const auto source = session.find_source(frame.runtime_pc)) {
      std::cout << ' ' << source->module_path << '!' << source->file << ':' << source->line;
      if (source->column != 0) std::cout << ':' << source->column;
    }
  } catch (const std::exception&) {
  }
  std::cout << '\n';
}

void print_backtrace(const mdbg::CoreInspectionSession& session) {
  for (const auto& frame : session.trace().frames) print_frame(session, frame);
  if (session.trace().stop_reason != mdbg::CfiUnwindStopReason::EndOfChain) {
    std::cout << "backtrace stopped: "
              << mdbg::cfi_unwind_stop_reason_name(session.trace().stop_reason) << '\n';
  }
}

void print_threads(const mdbg::CoreInspectionSession& session) {
  for (const auto& thread : session.threads()) {
    std::cout << (thread.tid == session.selected_thread_tid() ? "* " : "  ")
              << "tid " << thread.tid;
    if (thread.is_crashed) std::cout << " crash";
    std::cout << " signal " << thread.signal_number << " rip 0x" << std::hex
              << thread.registers.rip << std::dec << '\n';
  }
}

void print_value(const mdbg::LocalScalarValue& value) {
  std::cout << value.module_path << '!' << value.name << " = ";
  if (value.kind == mdbg::LocalValueKind::Structure) {
    std::cout << "{ ";
    for (std::size_t index = 0; index < value.members.size(); ++index) {
      if (index != 0) std::cout << ", ";
      std::cout << value.members[index].name << "=0x" << std::hex
                << value.members[index].raw_value << std::dec;
    }
    std::cout << " }";
  } else {
    std::cout << "0x" << std::hex << value.raw_value << std::dec;
  }
  std::cout << " [" << value.byte_size << "-byte "
            << (value.is_signed ? "signed" : "unsigned") << "]\n";
}

std::size_t parse_frame_index(const std::string& text) {
  std::size_t consumed = 0;
  const auto value = std::stoull(text, &consumed, 10);
  if (consumed != text.size()) throw std::invalid_argument("invalid core frame index: " + text);
  return static_cast<std::size_t>(value);
}

pid_t parse_thread_tid(const std::string& text) {
  std::size_t consumed = 0;
  const auto value = std::stoll(text, &consumed, 10);
  if (consumed != text.size() || value <= 0 ||
      value > static_cast<long long>(std::numeric_limits<pid_t>::max())) {
    throw std::invalid_argument("invalid core thread TID: " + text);
  }
  return static_cast<pid_t>(value);
}

void print_help() {
  std::cout << "read-only core commands:\n"
               "  threads              show immutable core thread contexts\n"
               "  thread <tid>         select an immutable core thread\n"
               "  bt | backtrace       show immutable snapshot frames\n"
               "  frame <index>        select an immutable snapshot frame\n"
               "  print <name> | p <name>  inspect a source value in the selected frame\n"
               "  help                 show this help\n"
               "  quit | q             exit the core session\n";
}

int run_session(const std::string& core_path, mdbg::SnapshotModulePathResolver module_paths) {
  mdbg::CoreInspectionSession session(core_path, std::move(module_paths));
  std::cout << "core signal " << session.snapshot().signal_number() << " tid "
            << session.snapshot().crashed_tid() << " frames " << session.trace().frames.size()
            << '\n';
  print_frame(session, session.selected_frame());

  std::string line;
  while (true) {
    std::cout << "core> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    std::istringstream input(line);
    std::string command;
    input >> command;
    if (command.empty()) continue;

    try {
      if (command == "q" || command == "quit") break;
      if (command == "help") {
        print_help();
        continue;
      }
      if (command == "threads") {
        std::string extra;
        if (input >> extra) throw std::invalid_argument("usage: threads");
        print_threads(session);
        continue;
      }
      if (command == "thread") {
        std::string tid_text;
        std::string extra;
        if (!(input >> tid_text) || (input >> extra)) {
          throw std::invalid_argument("usage: thread <tid>");
        }
        const auto tid = parse_thread_tid(tid_text);
        session.select_thread(tid);
        std::cout << "selected thread " << tid << '\n';
        print_frame(session, session.selected_frame());
        continue;
      }
      if (command == "bt" || command == "backtrace") {
        std::string extra;
        if (input >> extra) throw std::invalid_argument("usage: bt");
        print_backtrace(session);
        continue;
      }
      if (command == "frame") {
        std::string index_text;
        std::string extra;
        if (!(input >> index_text) || (input >> extra)) {
          throw std::invalid_argument("usage: frame <index>");
        }
        const auto index = parse_frame_index(index_text);
        session.select_frame(index);
        std::cout << "selected frame " << index << '\n';
        print_frame(session, session.selected_frame());
        continue;
      }
      if (command == "print" || command == "p") {
        std::string name;
        std::string extra;
        if (!(input >> name) || (input >> extra)) {
          throw std::invalid_argument("usage: print <name>");
        }
        print_value(session.inspect_value(name));
        continue;
      }

      std::cout << "unsupported in core session: " << command << '\n';
    } catch (const std::exception& error) {
      std::cout << "error: " << error.what() << '\n';
    }
  }
  return 0;
}

void print_usage() {
  std::cerr << "usage: mdbg-core [--substitute-module-path <recorded-prefix> <local-prefix>]... "
               "<core-file>\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    mdbg::SnapshotModulePathResolver module_paths;
    int argument = 1;
    while (argument < argc && std::string(argv[argument]) == "--substitute-module-path") {
      if (argument + 2 >= argc) {
        print_usage();
        return 2;
      }
      module_paths.add_substitution(argv[argument + 1], argv[argument + 2]);
      argument += 3;
    }
    if (argument + 1 != argc) {
      print_usage();
      return 2;
    }
    return run_session(argv[argument], std::move(module_paths));
  } catch (const std::exception& error) {
    std::cerr << "mdbg-core: " << error.what() << '\n';
    return 1;
  }
}

#include "snapshot/session.hpp"
#include "source/source_path.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr std::uint64_t kSourceContextRadius = 4;

void print_frame(const mdbg::CoreInspectionSession& session,
                 const mdbg::SnapshotInspectionFrameContext& frame) {
  std::cout << (frame.index == session.selected_frame_index() ? "* " : "  ") << '#'
            << frame.index << " 0x" << std::hex << frame.runtime_pc << std::dec << ' '
            << frame.module_path;
  try {
    if (const auto symbol = session.find_frame_symbol(frame)) {
      std::cout << '!' << symbol->name;
      if (symbol->offset != 0) {
        std::cout << "+0x" << std::hex << symbol->offset << std::dec;
      }
    }
  } catch (const std::exception&) {
  }
  try {
    if (const auto source = session.find_frame_source(frame)) {
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

void print_crash(const mdbg::CoreInspectionSession& session) {
  const auto& crash = session.crash_info();
  if (!crash) {
    std::cout << "crash metadata unavailable\n";
    return;
  }
  std::cout << "crash signal " << crash->signal_number << " code " << crash->signal_code;
  if (crash->fault_address) {
    std::cout << " address 0x" << std::hex << *crash->fault_address << std::dec;
  }
  std::cout << '\n';
}

void print_process(const mdbg::CoreInspectionSession& session) {
  const auto& process = session.process_info();
  if (!process) {
    std::cout << "process metadata unavailable\n";
    return;
  }
  std::cout << "process pid " << process->pid << " parent " << process->parent_pid
            << " pgrp " << process->process_group_id << " sid " << process->session_id
            << " file " << process->file_name << " command " << process->command << '\n';
}

void print_startup(const mdbg::CoreInspectionSession& session) {
  const auto& startup = session.startup_info();
  if (!startup) {
    std::cout << "startup metadata unavailable\n";
    return;
  }
  std::cout << "startup";
  if (startup->entry_point) {
    std::cout << " entry 0x" << std::hex << *startup->entry_point << std::dec;
  }
  if (startup->program_headers) {
    std::cout << " phdr 0x" << std::hex << *startup->program_headers << std::dec;
  }
  if (startup->program_header_count) {
    std::cout << " phnum " << *startup->program_header_count;
  }
  if (startup->page_size) {
    std::cout << " pagesz " << *startup->page_size;
  }
  if (startup->interpreter_base) {
    std::cout << " base 0x" << std::hex << *startup->interpreter_base << std::dec;
  }
  std::cout << '\n';
}

void print_source_excerpt(const mdbg::SourcePathResolver& source_paths,
                          const std::string& file, std::uint64_t line,
                          const std::string& module_path) {
  const auto path = source_paths.resolve(file, module_path);
  if (!path) {
    std::cout << "source unavailable: " << file << '\n';
    return;
  }

  std::ifstream input(*path);
  if (!input) {
    std::cout << "source unavailable: " << path->string() << '\n';
    return;
  }

  const auto first = line > kSourceContextRadius ? line - kSourceContextRadius : 1;
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto last = line > maximum - kSourceContextRadius
                        ? maximum
                        : line + kSourceContextRadius;

  std::string text;
  std::uint64_t current = 0;
  bool found_current = false;
  while (current < last && std::getline(input, text)) {
    ++current;
    if (current < first) continue;
    const bool active = current == line;
    if (active) found_current = true;
    std::cout << (active ? "=> " : "   ") << std::setw(5) << current << " | " << text << '\n';
  }

  if (!found_current) {
    std::cout << "source unavailable: " << path->string() << ':' << line
              << " is outside the file\n";
  }
}

void print_selected_source_context(const mdbg::CoreInspectionSession& session,
                                   const mdbg::SourcePathResolver& source_paths) {
  const auto& frame = session.selected_frame();
  try {
    if (const auto source = session.find_frame_source(frame)) {
      std::cout << source->module_path << '!' << source->file << ':' << source->line;
      if (source->column != 0) std::cout << ':' << source->column;
      std::cout << '\n';
      print_source_excerpt(source_paths, source->file, source->line, source->module_path);
      return;
    }
    std::cout << "no source location for selected frame\n";
  } catch (const std::exception& error) {
    std::cout << "source unavailable: " << error.what() << '\n';
  }
}

void print_locals(const mdbg::CoreInspectionSession& session) {
  for (const auto& entry : session.locals()) {
    std::cout << (entry.kind == mdbg::LocalDiscoveryKind::FormalParameter ? "parameter "
                                                                          : "variable ")
              << entry.name << '\n';
  }
}

void print_inline_contexts(const mdbg::CoreInspectionSession& session) {
  const auto contexts = session.inline_contexts();
  if (contexts.empty()) {
    std::cout << "no inline contexts for selected physical frame\n";
    return;
  }
  const auto selected = session.selected_inline_context_index();
  for (std::size_t index = 0; index < contexts.size(); ++index) {
    const auto& context = contexts[index];
    std::cout << (selected && *selected == index ? "* " : "  ") << "inline " << index
              << ' ' << context.module_path << '!' << context.name << " called at "
              << context.call_site.file << ':' << context.call_site.line;
    if (context.call_site.column != 0) std::cout << ':' << context.call_site.column;
    std::cout << '\n';
  }
}

void print_floating_value(const mdbg::LocalScalarValue& value) {
  if (value.byte_size == sizeof(float)) {
    const auto bits = static_cast<std::uint32_t>(value.raw_value);
    float decoded = 0.0F;
    std::memcpy(&decoded, &bits, sizeof(decoded));
    std::cout << std::setprecision(std::numeric_limits<float>::max_digits10) << decoded;
    return;
  }
  if (value.byte_size == sizeof(double)) {
    const auto bits = value.raw_value;
    double decoded = 0.0;
    std::memcpy(&decoded, &bits, sizeof(decoded));
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10) << decoded;
    return;
  }
  throw std::logic_error("floating local value has an unsupported scalar width");
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
  } else if (value.kind == mdbg::LocalValueKind::Array) {
    if (!value.array_type || value.elements.size() != value.array_type->element_count) {
      throw std::logic_error("bounded fixed-array value lost its element metadata");
    }
    std::cout << '[';
    for (std::size_t index = 0; index < value.elements.size(); ++index) {
      if (index != 0) std::cout << ", ";
      std::cout << "0x" << std::hex << value.elements[index].raw_value << std::dec;
    }
    std::cout << ']';
  } else if (value.kind == mdbg::LocalValueKind::Floating) {
    print_floating_value(value);
  } else {
    std::cout << "0x" << std::hex << value.raw_value << std::dec;
  }
  std::cout << " [" << value.byte_size << "-byte ";
  if (value.kind == mdbg::LocalValueKind::Floating) {
    std::cout << "floating";
  } else if (value.kind == mdbg::LocalValueKind::Array) {
    if (!value.array_type) {
      throw std::logic_error("bounded fixed-array value lost its type metadata");
    }
    std::cout << "array " << value.array_type->element_count << " x "
              << value.array_type->element_byte_size << "-byte "
              << (value.array_type->element_is_signed ? "signed" : "unsigned");
  } else {
    std::cout << (value.is_signed ? "signed" : "unsigned");
  }
  std::cout << ']';
  if (value.storage == mdbg::LocalValueStorage::SnapshotCoreMemory) {
    std::cout << " [value-core]";
  } else if (value.storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact) {
    std::cout << " [value-artifact:" << value.storage_module_path << " file+0x"
              << std::hex << value.storage_file_offset << std::dec << ']';
  }
  std::cout << '\n';
}

void print_memory(std::uintptr_t address, const mdbg::SnapshotMemoryRead& memory) {
  std::cout << "0x" << std::hex << address << ":";
  for (const auto byte : memory.bytes) {
    std::cout << ' ' << std::setw(2) << std::setfill('0')
              << std::to_integer<unsigned int>(byte);
  }
  std::cout << std::setfill(' ') << std::dec << " [";
  if (memory.provenance == mdbg::SnapshotMemoryProvenance::Core) {
    std::cout << "core";
  } else {
    std::cout << "artifact:" << memory.module_path << " file+0x" << std::hex
              << memory.artifact_file_offset << std::dec;
  }
  std::cout << "]\n";
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

std::uintptr_t parse_memory_address(const std::string& text) {
  std::size_t consumed = 0;
  const auto value = std::stoull(text, &consumed, 0);
  if (consumed != text.size() || value > std::numeric_limits<std::uintptr_t>::max()) {
    throw std::invalid_argument("invalid core memory address: " + text);
  }
  return static_cast<std::uintptr_t>(value);
}

std::size_t parse_memory_length(const std::string& text) {
  std::size_t consumed = 0;
  const auto value = std::stoull(text, &consumed, 0);
  if (consumed != text.size() || value == 0 || value > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("invalid core memory length: " + text);
  }
  return static_cast<std::size_t>(value);
}

void print_help() {
  std::cout << "read-only core commands:\n"
               "  crash                show kernel-recorded crash metadata\n"
               "  process              show kernel-recorded process identity\n"
               "  startup              show kernel-recorded process startup metadata\n"
               "  threads              show immutable core thread contexts\n"
               "  thread <tid>         select an immutable core thread\n"
               "  bt | backtrace       show immutable snapshot frames\n"
               "  frame <index>        select an immutable snapshot frame\n"
               "  inline               show inline source contexts for the physical frame\n"
               "  inline <index>       select one inline source context\n"
               "  inline physical      return local scope to the physical frame\n"
               "  list | l             show source context for the selected frame\n"
               "  locals               list active parameter/local names without reading values\n"
               "  print <name> | p <name>  inspect a source value in the selected frame\n"
               "  deref <name>         dereference one bounded pointer value\n"
               "  member <name> <member>  inspect one bounded pointer-valued direct member\n"
               "  deref-member <name> <member>  dereference that member once\n"
               "  aggregate-member <name> <member>  select one by-value aggregate member\n"
               "  deref-aggregate-member <name> <member>  dereference that member once\n"
               "  array-element <name> <index>  select one bounded fixed-array element\n"
               "  x <address> <length> inspect immutable snapshot memory\n"
               "  help                 show this help\n"
               "  quit | q             exit the core session\n";
}

int run_session(const std::string& core_path, mdbg::SnapshotModulePathResolver module_paths,
                mdbg::SourcePathResolver source_paths) {
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
      if (command == "crash") {
        std::string extra;
        if (input >> extra) throw std::invalid_argument("usage: crash");
        print_crash(session);
        continue;
      }
      if (command == "process") {
        std::string extra;
        if (input >> extra) throw std::invalid_argument("usage: process");
        print_process(session);
        continue;
      }
      if (command == "startup") {
        std::string extra;
        if (input >> extra) throw std::invalid_argument("usage: startup");
        print_startup(session);
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
      if (command == "inline") {
        std::string selection;
        std::string extra;
        if (!(input >> selection)) {
          print_inline_contexts(session);
          continue;
        }
        if (input >> extra) {
          throw std::invalid_argument("usage: inline [<index>|physical]");
        }
        if (selection == "physical") {
          session.clear_inline_context();
          std::cout << "selected physical frame " << session.selected_frame_index() << '\n';
          continue;
        }
        const auto index = parse_frame_index(selection);
        session.select_inline_context(index);
        std::cout << "selected inline " << index << '\n';
        print_inline_contexts(session);
        continue;
      }
      if (command == "list" || command == "l") {
        std::string extra;
        if (input >> extra) throw std::invalid_argument("usage: list");
        print_selected_source_context(session, source_paths);
        continue;
      }
      if (command == "locals") {
        std::string extra;
        if (input >> extra) throw std::invalid_argument("usage: locals");
        print_locals(session);
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
      if (command == "deref") {
        std::string name;
        std::string extra;
        if (!(input >> name) || (input >> extra)) {
          throw std::invalid_argument("usage: deref <name>");
        }
        print_value(session.dereference_value(name));
        continue;
      }
      if (command == "member") {
        std::string name;
        std::string member;
        std::string extra;
        if (!(input >> name >> member) || (input >> extra)) {
          throw std::invalid_argument("usage: member <name> <member>");
        }
        print_value(session.inspect_pointer_member(name, member));
        continue;
      }
      if (command == "deref-member") {
        std::string name;
        std::string member;
        std::string extra;
        if (!(input >> name >> member) || (input >> extra)) {
          throw std::invalid_argument("usage: deref-member <name> <member>");
        }
        print_value(session.dereference_pointer_member(name, member));
        continue;
      }
      if (command == "aggregate-member") {
        std::string name;
        std::string member;
        std::string extra;
        if (!(input >> name >> member) || (input >> extra)) {
          throw std::invalid_argument("usage: aggregate-member <name> <member>");
        }
        print_value(session.inspect_aggregate_member(name, member));
        continue;
      }
      if (command == "deref-aggregate-member") {
        std::string name;
        std::string member;
        std::string extra;
        if (!(input >> name >> member) || (input >> extra)) {
          throw std::invalid_argument(
              "usage: deref-aggregate-member <name> <member>");
        }
        print_value(session.dereference_aggregate_member(name, member));
        continue;
      }
      if (command == "array-element") {
      std::string name;
      std::string index_text;
      std::string extra;
      if (!(input >> name >> index_text) || (input >> extra)) {
        throw std::invalid_argument("usage: array-element <name> <index>");
      }
      print_value(session.inspect_array_element(name, parse_frame_index(index_text)));
      continue;
    }
    if (command == "x") {
        std::string address_text;
        std::string length_text;
        std::string extra;
        if (!(input >> address_text >> length_text) || (input >> extra)) {
          throw std::invalid_argument("usage: x <address> <length>");
        }
        const auto address = parse_memory_address(address_text);
        const auto length = parse_memory_length(length_text);
        print_memory(address, session.read_memory(address, length));
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
  std::cerr
      << "usage: mdbg-core [--substitute-module-path <recorded-prefix> <local-prefix>]... "
         "[--debug-file <recorded-module> <local-debug-file>]... "
         "[--substitute-source-path <recorded-prefix> <local-prefix>]... <core-file>\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    mdbg::SnapshotModulePathResolver module_paths;
    mdbg::SourcePathResolver source_paths;
    int argument = 1;
    while (argument < argc - 1) {
      const std::string option(argv[argument]);
      if (option == "--substitute-module-path") {
        if (argument + 2 >= argc) {
          print_usage();
          return 2;
        }
        module_paths.add_substitution(argv[argument + 1], argv[argument + 2]);
        argument += 3;
        continue;
      }
      if (option == "--debug-file") {
        if (argument + 2 >= argc) {
          print_usage();
          return 2;
        }
        module_paths.add_debug_file(argv[argument + 1], argv[argument + 2]);
        argument += 3;
        continue;
      }
      if (option == "--substitute-source-path") {
        if (argument + 2 >= argc) {
          print_usage();
          return 2;
        }
        source_paths.add_substitution(argv[argument + 1], argv[argument + 2]);
        argument += 3;
        continue;
      }
      break;
    }
    if (argument + 1 != argc) {
      print_usage();
      return 2;
    }
    return run_session(argv[argument], std::move(module_paths), std::move(source_paths));
  } catch (const std::exception& error) {
    std::cerr << "mdbg-core: " << error.what() << '\n';
    return 1;
  }
}

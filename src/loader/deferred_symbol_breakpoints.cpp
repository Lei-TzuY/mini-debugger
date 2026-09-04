#include "loader/deferred_symbol_breakpoints.hpp"

#include "unwind/cfi.hpp"

#include <elf.h>
#include <link.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace mdbg {
namespace {

template <typename T>
T read_file_struct(std::ifstream& input, std::uint64_t offset, const char* what) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
    throw std::runtime_error(std::string(what) + " offset is out of range");
  }
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  T value{};
  input.read(reinterpret_cast<char*>(&value), sizeof(value));
  if (!input) throw std::runtime_error(std::string("failed to read ") + what);
  return value;
}

}  // namespace

DeferredSymbolBreakpoints::DeferredSymbolBreakpoints(Debugger& debugger,
                                                     const ElfFile& executable)
    : debugger_(debugger),
      executable_(executable),
      metadata_(read_loader_metadata(executable.path())) {}

DeferredSymbolBreakpoints::~DeferredSymbolBreakpoints() {
  if (debugger_.state() != ProcessState::Stopped) return;
  remove_internal_breakpoint(loader_breakpoint_id_, loader_breakpoint_address_);
  remove_internal_breakpoint(bootstrap_breakpoint_id_, bootstrap_breakpoint_address_);
}

DeferredSymbolBreakpoints::LoaderMetadata DeferredSymbolBreakpoints::read_loader_metadata(
    const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open executable for loader metadata: " + path);

  const auto header = read_file_struct<Elf64_Ehdr>(input, 0, "ELF header");
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
      header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
      header.e_machine != EM_X86_64) {
    throw std::runtime_error("loader rendezvous requires little-endian x86-64 ELF64");
  }
  if (header.e_phentsize != sizeof(Elf64_Phdr)) {
    throw std::runtime_error("unsupported program-header entry size for loader rendezvous");
  }

  std::optional<std::uint64_t> dt_debug_value_virtual_address;
  for (std::size_t index = 0; index < header.e_phnum; ++index) {
    const auto phdr_offset = static_cast<std::uint64_t>(header.e_phoff) +
                             index * static_cast<std::uint64_t>(sizeof(Elf64_Phdr));
    const auto phdr = read_file_struct<Elf64_Phdr>(input, phdr_offset, "program header");
    if (phdr.p_type != PT_DYNAMIC) continue;
    if (phdr.p_filesz % sizeof(Elf64_Dyn) != 0) {
      throw std::runtime_error("dynamic segment has a partial ELF64 dynamic entry");
    }

    const auto count = phdr.p_filesz / sizeof(Elf64_Dyn);
    for (std::uint64_t dyn_index = 0; dyn_index < count; ++dyn_index) {
      const auto file_offset = phdr.p_offset + dyn_index * sizeof(Elf64_Dyn);
      const auto dynamic = read_file_struct<Elf64_Dyn>(input, file_offset, "dynamic entry");
      if (dynamic.d_tag == DT_NULL) break;
      if (dynamic.d_tag != DT_DEBUG) continue;
      dt_debug_value_virtual_address =
          phdr.p_vaddr + dyn_index * sizeof(Elf64_Dyn) + offsetof(Elf64_Dyn, d_un);
      break;
    }
    if (dt_debug_value_virtual_address) break;
  }

  if (!dt_debug_value_virtual_address) {
    throw std::runtime_error("executable has no DT_DEBUG loader rendezvous slot");
  }
  return {header.e_entry, *dt_debug_value_virtual_address};
}

std::uint64_t DeferredSymbolBreakpoints::load_bias() const {
  return executable_.load_bias(debugger_.pid());
}

std::uintptr_t DeferredSymbolBreakpoints::runtime_address(std::uint64_t virtual_address) const {
  const auto bias = load_bias();
  if (virtual_address > std::numeric_limits<std::uint64_t>::max() - bias) {
    throw std::runtime_error("loader runtime address overflows address space");
  }
  const auto address = bias + virtual_address;
  if (address > std::numeric_limits<std::uintptr_t>::max()) {
    throw std::runtime_error("loader runtime address does not fit uintptr_t");
  }
  return static_cast<std::uintptr_t>(address);
}

std::uint64_t DeferredSymbolBreakpoints::read_u64(std::uintptr_t address) const {
  const auto bytes = debugger_.read_memory(address, sizeof(std::uint64_t));
  if (bytes.size() != sizeof(std::uint64_t)) {
    throw std::runtime_error("failed to read loader pointer from tracee");
  }
  std::uint64_t value = 0;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

std::optional<std::uintptr_t> DeferredSymbolBreakpoints::current_r_debug_address() const {
  const auto slot = runtime_address(metadata_.dt_debug_value_virtual_address);
  const auto value = read_u64(slot);
  if (value == 0) return std::nullopt;
  if (value > std::numeric_limits<std::uintptr_t>::max()) {
    throw std::runtime_error("DT_DEBUG pointer does not fit uintptr_t");
  }
  return static_cast<std::uintptr_t>(value);
}

std::uintptr_t DeferredSymbolBreakpoints::current_loader_break_address() const {
  if (!r_debug_address_) throw std::logic_error("r_debug address is not initialized");
  const auto bytes = debugger_.read_memory(*r_debug_address_, sizeof(::r_debug));
  if (bytes.size() != sizeof(::r_debug)) {
    throw std::runtime_error("failed to read r_debug rendezvous");
  }
  ::r_debug rendezvous{};
  std::memcpy(&rendezvous, bytes.data(), sizeof(rendezvous));
  if (rendezvous.r_version <= 0 || rendezvous.r_brk == 0) {
    throw std::runtime_error("dynamic loader rendezvous is not initialized");
  }
  return static_cast<std::uintptr_t>(rendezvous.r_brk);
}

bool DeferredSymbolBreakpoints::loader_state_is_consistent() const {
  if (!r_debug_address_) return false;
  const auto bytes = debugger_.read_memory(*r_debug_address_, sizeof(::r_debug));
  if (bytes.size() != sizeof(::r_debug)) {
    throw std::runtime_error("failed to read r_debug state");
  }
  ::r_debug rendezvous{};
  std::memcpy(&rendezvous, bytes.data(), sizeof(rendezvous));
  return rendezvous.r_state == ::r_debug::RT_CONSISTENT;
}

std::size_t DeferredSymbolBreakpoints::add(std::string symbol) {
  if (debugger_.state() != ProcessState::Stopped) {
    throw std::logic_error("deferred breakpoints can only be modified while stopped");
  }
  if (symbol.empty()) throw std::invalid_argument("deferred symbol must not be empty");
  for (const auto& [id, request] : requests_) {
    static_cast<void>(id);
    if (request.symbol == symbol) {
      throw std::invalid_argument("a deferred breakpoint request already exists for that symbol");
    }
  }

  DeferredSymbolBreakpoint request{next_request_id_++, std::move(symbol), std::nullopt,
                                   std::nullopt};
  if (const auto resolved =
          find_module_symbol_by_name(debugger_.pid(), request.symbol, executable_)) {
    request.address = resolved->address;
    request.breakpoint_id = debugger_.add_breakpoint(resolved->address);
  }

  const auto request_id = request.request_id;
  requests_.emplace(request_id, std::move(request));
  try {
    ensure_monitoring();
  } catch (...) {
    auto it = requests_.find(request_id);
    if (it != requests_.end() && it->second.breakpoint_id) {
      debugger_.remove_breakpoint(*it->second.breakpoint_id);
    }
    requests_.erase(request_id);
    throw;
  }
  return request_id;
}

bool DeferredSymbolBreakpoints::remove(std::size_t request_id) {
  const auto it = requests_.find(request_id);
  if (it == requests_.end()) return false;
  if (it->second.breakpoint_id && !debugger_.remove_breakpoint(*it->second.breakpoint_id)) {
    throw std::logic_error("resolved deferred breakpoint disappeared from debugger state");
  }
  requests_.erase(it);
  stop_monitoring_if_idle();
  return true;
}

std::vector<DeferredSymbolBreakpoint> DeferredSymbolBreakpoints::breakpoints() const {
  std::vector<DeferredSymbolBreakpoint> result;
  result.reserve(requests_.size());
  for (const auto& [id, request] : requests_) {
    static_cast<void>(id);
    result.push_back(request);
  }
  return result;
}

void DeferredSymbolBreakpoints::ensure_monitoring() {
  const bool pending = std::any_of(requests_.begin(), requests_.end(), [](const auto& entry) {
    return !entry.second.breakpoint_id.has_value();
  });
  if (!pending || loader_breakpoint_id_ || bootstrap_breakpoint_id_) return;
  if (!try_install_loader_breakpoint()) install_bootstrap_breakpoint();
}

bool DeferredSymbolBreakpoints::try_install_loader_breakpoint() {
  const auto address = current_r_debug_address();
  if (!address) return false;
  r_debug_address_ = *address;
  const auto loader_break = current_loader_break_address();
  loader_breakpoint_address_ = loader_break;
  loader_breakpoint_id_ = debugger_.add_breakpoint(loader_break);
  return true;
}

void DeferredSymbolBreakpoints::install_bootstrap_breakpoint() {
  const auto entry = runtime_address(metadata_.entry_virtual_address);
  bootstrap_breakpoint_address_ = entry;
  bootstrap_breakpoint_id_ = debugger_.add_breakpoint(entry);
}

void DeferredSymbolBreakpoints::resolve_pending() {
  for (auto& [id, request] : requests_) {
    static_cast<void>(id);
    if (request.breakpoint_id) continue;
    const auto resolved =
        find_module_symbol_by_name(debugger_.pid(), request.symbol, executable_);
    if (!resolved) continue;
    request.address = resolved->address;
    request.breakpoint_id = debugger_.add_breakpoint(resolved->address);
  }
}

void DeferredSymbolBreakpoints::stop_monitoring_if_idle() {
  const bool pending = std::any_of(requests_.begin(), requests_.end(), [](const auto& entry) {
    return !entry.second.breakpoint_id.has_value();
  });
  if (pending) return;
  remove_internal_breakpoint(loader_breakpoint_id_, loader_breakpoint_address_);
  remove_internal_breakpoint(bootstrap_breakpoint_id_, bootstrap_breakpoint_address_);
  r_debug_address_.reset();
}

bool DeferredSymbolBreakpoints::is_internal_stop(const StopInfo& stop) const {
  if (stop.reason != StopReason::Breakpoint || !stop.breakpoint_address) return false;
  return (bootstrap_breakpoint_address_ &&
          stop.breakpoint_address == bootstrap_breakpoint_address_) ||
         (loader_breakpoint_address_ && stop.breakpoint_address == loader_breakpoint_address_);
}

void DeferredSymbolBreakpoints::handle_internal_stop(const StopInfo& stop) {
  if (bootstrap_breakpoint_address_ && stop.breakpoint_address == bootstrap_breakpoint_address_) {
    remove_internal_breakpoint(bootstrap_breakpoint_id_, bootstrap_breakpoint_address_);
    if (!try_install_loader_breakpoint()) {
      throw std::runtime_error("DT_DEBUG was not initialized before executable entry");
    }
    resolve_pending();
    stop_monitoring_if_idle();
    return;
  }

  if (loader_breakpoint_address_ && stop.breakpoint_address == loader_breakpoint_address_) {
    if (loader_state_is_consistent()) {
      resolve_pending();
      stop_monitoring_if_idle();
    }
    return;
  }

  throw std::logic_error("attempted to handle a non-internal breakpoint stop");
}

StopInfo DeferredSymbolBreakpoints::continue_execution(SignalPolicy policy) {
  ensure_monitoring();
  for (;;) {
    const auto stop = debugger_.continue_execution(policy);
    policy = SignalPolicy::Suppress;
    if (!is_internal_stop(stop)) return stop;
    handle_internal_stop(stop);
  }
}

void DeferredSymbolBreakpoints::remove_internal_breakpoint(
    std::optional<std::size_t>& id, std::optional<std::uintptr_t>& address) noexcept {
  if (!id) {
    address.reset();
    return;
  }
  try {
    if (debugger_.state() == ProcessState::Stopped) debugger_.remove_breakpoint(*id);
  } catch (...) {
  }
  id.reset();
  address.reset();
}

}  // namespace mdbg

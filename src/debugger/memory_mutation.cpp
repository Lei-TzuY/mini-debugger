#include "debugger/debugger.hpp"
#include "ptrace/ptrace.hpp"

#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mdbg {
namespace {

constexpr std::size_t kMaxMemoryWriteBytes = 4096;

bool range_contains(std::uintptr_t address, std::size_t length,
                    std::uintptr_t target) {
  return length != 0 && target >= address &&
         target - address < static_cast<std::uintptr_t>(length);
}

}  // namespace

void Debugger::write_memory(std::uintptr_t address,
                            const std::vector<std::byte>& bytes) {
  const auto tid = stopped_tid();
  if (bytes.empty()) {
    throw std::invalid_argument("memory write must contain at least one byte");
  }
  if (bytes.size() > kMaxMemoryWriteBytes) {
    throw std::invalid_argument("memory write exceeds the 4096-byte bound");
  }
  if (address > std::numeric_limits<std::uintptr_t>::max() -
                    static_cast<std::uintptr_t>(bytes.size() - 1)) {
    throw std::invalid_argument("memory write range overflows the address space");
  }

  if (pending_breakpoint_step_ &&
      range_contains(address, bytes.size(), pending_breakpoint_step_->address)) {
    throw std::logic_error(
        "cannot write memory overlapping a pending breakpoint displaced step");
  }

  std::vector<std::pair<Breakpoint*, std::byte>> saved_byte_updates;
  saved_byte_updates.reserve(breakpoints_by_address_.size());

  for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
    const auto current = address + static_cast<std::uintptr_t>(offset);
    const auto breakpoint = breakpoints_by_address_.find(current);
    if (breakpoint == breakpoints_by_address_.end()) continue;
    if (!breakpoint->second.installed) {
      throw std::logic_error(
          "cannot write memory overlapping a temporarily restored breakpoint byte");
    }
    saved_byte_updates.emplace_back(&breakpoint->second, bytes[offset]);
  }

  for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
    const auto current = address + static_cast<std::uintptr_t>(offset);
    const auto breakpoint = breakpoints_by_address_.find(current);
    if (breakpoint != breakpoints_by_address_.end() && breakpoint->second.installed) {
      continue;
    }
    lowlevel::write_byte(tid, current, bytes[offset]);
  }

  for (const auto& [breakpoint, value] : saved_byte_updates) {
    breakpoint->original_byte = value;
  }
}

}  // namespace mdbg

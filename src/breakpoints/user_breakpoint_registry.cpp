#include "breakpoints/user_breakpoint_registry.hpp"

#include "unwind/cfi.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mdbg {

UserBreakpointRegistry::UserBreakpointRegistry(Debugger& debugger, const ElfFile& executable)
    : debugger_(debugger), executable_(executable), deferred_(debugger, executable) {}

std::size_t UserBreakpointRegistry::add_address(std::uintptr_t address,
                                                std::string expression) {
  const auto backend_id = debugger_.add_breakpoint(address);
  const auto id = next_id_++;
  entries_.emplace(id, Entry{std::move(expression), BackendKind::Managed, backend_id});
  return id;
}

std::size_t UserBreakpointRegistry::add_symbol(std::string symbol) {
  if (symbol.empty()) throw std::invalid_argument("breakpoint symbol must not be empty");

  if (const auto resolved =
          find_module_symbol_by_name(debugger_.pid(), symbol, executable_)) {
    return add_address(resolved->address, std::move(symbol));
  }

  const auto backend_id = deferred_.add(symbol);
  const auto id = next_id_++;
  entries_.emplace(id, Entry{std::move(symbol), BackendKind::Deferred, backend_id});
  return id;
}

bool UserBreakpointRegistry::remove(std::size_t id) {
  const auto it = entries_.find(id);
  if (it == entries_.end()) return false;

  const bool removed = it->second.kind == BackendKind::Managed
                           ? debugger_.remove_breakpoint(it->second.backend_id)
                           : deferred_.remove(it->second.backend_id);
  if (!removed) {
    throw std::logic_error("user breakpoint backend disappeared from registry ownership");
  }
  entries_.erase(it);
  return true;
}

UserBreakpoint UserBreakpointRegistry::snapshot(std::size_t id, const Entry& entry) const {
  if (entry.kind == BackendKind::Managed) {
    const auto managed = debugger_.breakpoints();
    const auto it = std::find_if(managed.begin(), managed.end(), [&](const Breakpoint& breakpoint) {
      return breakpoint.id == entry.backend_id;
    });
    if (it == managed.end()) {
      throw std::logic_error("managed breakpoint disappeared from user registry ownership");
    }
    return {id, entry.expression, it->address,
            it->installed ? UserBreakpointState::Enabled
                          : UserBreakpointState::TemporarilyRestored};
  }

  const auto deferred = deferred_.breakpoints();
  const auto request = std::find_if(
      deferred.begin(), deferred.end(), [&](const DeferredSymbolBreakpoint& breakpoint) {
        return breakpoint.request_id == entry.backend_id;
      });
  if (request == deferred.end()) {
    throw std::logic_error("deferred breakpoint disappeared from user registry ownership");
  }
  if (!request->breakpoint_id || !request->address) {
    return {id, entry.expression, std::nullopt, UserBreakpointState::Pending};
  }

  const auto managed = debugger_.breakpoints();
  const auto breakpoint =
      std::find_if(managed.begin(), managed.end(), [&](const Breakpoint& candidate) {
        return candidate.id == *request->breakpoint_id;
      });
  if (breakpoint == managed.end()) {
    throw std::logic_error("resolved deferred breakpoint disappeared from debugger ownership");
  }
  return {id, entry.expression, request->address,
          breakpoint->installed ? UserBreakpointState::Enabled
                                : UserBreakpointState::TemporarilyRestored};
}

std::optional<UserBreakpoint> UserBreakpointRegistry::breakpoint(std::size_t id) const {
  const auto it = entries_.find(id);
  if (it == entries_.end()) return std::nullopt;
  return snapshot(id, it->second);
}

std::vector<UserBreakpoint> UserBreakpointRegistry::breakpoints() const {
  std::vector<UserBreakpoint> result;
  result.reserve(entries_.size());
  for (const auto& [id, entry] : entries_) result.push_back(snapshot(id, entry));
  return result;
}

bool UserBreakpointRegistry::has_pending() const {
  for (const auto& [id, entry] : entries_) {
    if (snapshot(id, entry).state == UserBreakpointState::Pending) return true;
  }
  return false;
}

StopInfo UserBreakpointRegistry::continue_execution(SignalPolicy policy) {
  return deferred_.continue_execution(policy);
}

const char* user_breakpoint_state_name(UserBreakpointState state) noexcept {
  switch (state) {
    case UserBreakpointState::Pending:
      return "pending";
    case UserBreakpointState::Enabled:
      return "enabled";
    case UserBreakpointState::TemporarilyRestored:
      return "temporarily-restored";
  }
  return "unknown";
}

}  // namespace mdbg

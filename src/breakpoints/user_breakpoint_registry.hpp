#pragma once

#include "debugger/debugger.hpp"
#include "elf/elf.hpp"
#include "loader/deferred_symbol_breakpoints.hpp"
#include "unwind/cfi.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mdbg {

enum class UserBreakpointState { Pending, Enabled, TemporarilyRestored };

struct UserBreakpoint {
  std::size_t id;
  std::string expression;
  std::optional<std::uintptr_t> address;
  UserBreakpointState state;
};

class UserBreakpointRegistry {
 public:
  UserBreakpointRegistry(Debugger& debugger, const ElfFile& executable)
      : debugger_(debugger), executable_(executable) {}

  std::size_t add_address(std::uintptr_t address, std::string expression) {
    const auto backend_id = debugger_.add_breakpoint(address);
    const auto id = next_id_++;
    entries_.emplace(id, Entry{std::move(expression), BackendKind::Managed, backend_id});
    return id;
  }

  std::size_t add_symbol(std::string symbol) {
    if (symbol.empty()) throw std::invalid_argument("breakpoint symbol must not be empty");

    if (const auto resolved = find_module_symbol_by_name(debugger_.pid(), symbol, executable_)) {
      return add_address(resolved->address, std::move(symbol));
    }

    ensure_deferred();
    const auto backend_id = deferred_->add_symbol(symbol);
    const auto id = next_id_++;
    entries_.emplace(id, Entry{std::move(symbol), BackendKind::Deferred, backend_id});
    return id;
  }

  std::size_t add_source(std::string file, std::uint64_t line, std::string expression) {
    if (file.empty()) throw std::invalid_argument("breakpoint source file must not be empty");
    if (line == 0) throw std::invalid_argument("breakpoint source line must be non-zero");

    if (const auto resolved =
            find_module_source_by_file_line(debugger_.pid(), file, line, executable_)) {
      return add_address(resolved->address, std::move(expression));
    }

    ensure_deferred();
    const auto backend_id = deferred_->add_source(std::move(file), line);
    const auto id = next_id_++;
    entries_.emplace(id, Entry{std::move(expression), BackendKind::Deferred, backend_id});
    return id;
  }

  bool remove(std::size_t id) {
    const auto it = entries_.find(id);
    if (it == entries_.end()) return false;

    bool removed = false;
    if (it->second.kind == BackendKind::Managed) {
      removed = debugger_.remove_breakpoint(it->second.backend_id);
    } else {
      if (!deferred_) {
        throw std::logic_error("deferred user breakpoint has no loader controller");
      }
      removed = deferred_->remove(it->second.backend_id);
    }
    if (!removed) {
      throw std::logic_error("user breakpoint backend disappeared from registry ownership");
    }
    entries_.erase(it);
    return true;
  }

  [[nodiscard]] std::optional<UserBreakpoint> breakpoint(std::size_t id) const {
    const auto it = entries_.find(id);
    if (it == entries_.end()) return std::nullopt;
    return snapshot(id, it->second);
  }

  [[nodiscard]] std::vector<UserBreakpoint> breakpoints() const {
    std::vector<UserBreakpoint> result;
    result.reserve(entries_.size());
    for (const auto& [id, entry] : entries_) result.push_back(snapshot(id, entry));
    return result;
  }

  [[nodiscard]] bool has_pending() const {
    for (const auto& [id, entry] : entries_) {
      if (snapshot(id, entry).state == UserBreakpointState::Pending) return true;
    }
    return false;
  }

  StopInfo continue_execution(SignalPolicy policy = SignalPolicy::Suppress) {
    if (deferred_) return deferred_->continue_execution(policy);
    return debugger_.continue_execution(policy);
  }

 private:
  enum class BackendKind { Managed, Deferred };

  struct Entry {
    std::string expression;
    BackendKind kind;
    std::size_t backend_id;
  };

  void ensure_deferred() {
    if (!deferred_) deferred_.emplace(debugger_, executable_);
  }

  [[nodiscard]] UserBreakpoint snapshot(std::size_t id, const Entry& entry) const {
    if (entry.kind == BackendKind::Managed) {
      const auto managed = debugger_.breakpoints();
      const auto it =
          std::find_if(managed.begin(), managed.end(), [&](const Breakpoint& breakpoint) {
            return breakpoint.id == entry.backend_id;
          });
      if (it == managed.end()) {
        throw std::logic_error("managed breakpoint disappeared from user registry ownership");
      }
      return {id, entry.expression, it->address,
              it->installed ? UserBreakpointState::Enabled
                            : UserBreakpointState::TemporarilyRestored};
    }

    if (!deferred_) throw std::logic_error("deferred user breakpoint has no loader controller");
    const auto deferred = deferred_->breakpoints();
    const auto request = std::find_if(
        deferred.begin(), deferred.end(), [&](const DeferredBreakpoint& breakpoint) {
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

  Debugger& debugger_;
  const ElfFile& executable_;
  std::optional<DeferredBreakpoints> deferred_;
  std::map<std::size_t, Entry> entries_;
  std::size_t next_id_{1};
};

inline const char* user_breakpoint_state_name(UserBreakpointState state) noexcept {
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

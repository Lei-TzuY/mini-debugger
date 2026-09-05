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
 private:
  enum class BackendKind { Managed, Deferred };

  struct Entry {
    std::string expression;
    BackendKind kind;
    std::size_t backend_id;
  };

  struct DomainState {
    std::optional<DeferredBreakpoints> deferred;
    std::map<std::size_t, Entry> entries;
  };

 public:
  UserBreakpointRegistry(Debugger& debugger, const ElfFile& executable)
      : debugger_(debugger), executable_(executable) {}

  std::size_t add_address(std::uintptr_t address, std::string expression) {
    const auto backend_id = debugger_.add_breakpoint(address);
    const auto id = next_id_++;
    active_domain().entries.emplace(
        id, Entry{std::move(expression), BackendKind::Managed, backend_id});
    return id;
  }

  std::size_t add_symbol(std::string symbol) {
    if (symbol.empty()) throw std::invalid_argument("breakpoint symbol must not be empty");

    if (const auto resolved = find_module_symbol_by_name(debugger_.pid(), symbol, executable_)) {
      return add_address(resolved->address, std::move(symbol));
    }

    auto& domain = active_domain();
    ensure_deferred(domain);
    const auto backend_id = domain.deferred->add_symbol(symbol);
    const auto id = next_id_++;
    domain.entries.emplace(id, Entry{std::move(symbol), BackendKind::Deferred, backend_id});
    return id;
  }

  std::size_t add_source(std::string file, std::uint64_t line, std::string expression) {
    if (file.empty()) throw std::invalid_argument("breakpoint source file must not be empty");
    if (line == 0) throw std::invalid_argument("breakpoint source line must be non-zero");

    if (const auto resolved =
            find_module_source_by_file_line(debugger_.pid(), file, line, executable_)) {
      return add_address(resolved->address, std::move(expression));
    }

    auto& domain = active_domain();
    ensure_deferred(domain);
    const auto backend_id = domain.deferred->add_source(std::move(file), line);
    const auto id = next_id_++;
    domain.entries.emplace(id, Entry{std::move(expression), BackendKind::Deferred, backend_id});
    return id;
  }

  bool remove(std::size_t id) {
    auto* domain = active_domain_if_present();
    if (domain == nullptr) return false;
    const auto it = domain->entries.find(id);
    if (it == domain->entries.end()) return false;

    bool removed = false;
    if (it->second.kind == BackendKind::Managed) {
      removed = debugger_.remove_breakpoint(it->second.backend_id);
    } else {
      if (!domain->deferred) {
        throw std::logic_error("deferred user breakpoint has no loader controller");
      }
      removed = domain->deferred->remove(it->second.backend_id);
    }
    if (!removed) {
      throw std::logic_error("user breakpoint backend disappeared from registry ownership");
    }
    domain->entries.erase(it);
    return true;
  }

  void on_image_replaced() noexcept {
    auto* domain = active_domain_if_present();
    if (domain == nullptr) return;
    domain->deferred.reset();
    domain->entries.clear();
  }

  [[nodiscard]] std::optional<UserBreakpoint> breakpoint(std::size_t id) const {
    const auto* domain = active_domain_if_present();
    if (domain == nullptr) return std::nullopt;
    const auto it = domain->entries.find(id);
    if (it == domain->entries.end()) return std::nullopt;
    return snapshot(id, it->second, *domain);
  }

  [[nodiscard]] std::vector<UserBreakpoint> breakpoints() const {
    const auto* domain = active_domain_if_present();
    if (domain == nullptr) return {};

    std::vector<UserBreakpoint> result;
    result.reserve(domain->entries.size());
    for (const auto& [id, entry] : domain->entries) {
      result.push_back(snapshot(id, entry, *domain));
    }
    return result;
  }

  [[nodiscard]] bool has_pending() const {
    const auto* domain = active_domain_if_present();
    if (domain == nullptr) return false;
    for (const auto& [id, entry] : domain->entries) {
      if (snapshot(id, entry, *domain).state == UserBreakpointState::Pending) return true;
    }
    return false;
  }

  StopInfo continue_execution(SignalPolicy policy = SignalPolicy::Suppress) {
    auto* domain = active_domain_if_present();
    if (domain != nullptr && domain->deferred) {
      return domain->deferred->continue_execution(policy);
    }
    return debugger_.continue_execution(policy);
  }

 private:
  DomainState& active_domain() { return domains_[debugger_.pid()]; }

  DomainState* active_domain_if_present() {
    const auto it = domains_.find(debugger_.pid());
    return it == domains_.end() ? nullptr : &it->second;
  }

  const DomainState* active_domain_if_present() const {
    const auto it = domains_.find(debugger_.pid());
    return it == domains_.end() ? nullptr : &it->second;
  }

  void ensure_deferred(DomainState& domain) {
    if (!domain.deferred) domain.deferred.emplace(debugger_, executable_);
  }

  [[nodiscard]] UserBreakpoint snapshot(std::size_t id, const Entry& entry,
                                        const DomainState& domain) const {
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

    if (!domain.deferred) {
      throw std::logic_error("deferred user breakpoint has no loader controller");
    }
    const auto deferred = domain.deferred->breakpoints();
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
  std::map<pid_t, DomainState> domains_;
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

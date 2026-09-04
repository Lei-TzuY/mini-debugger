#pragma once

#include "debugger/debugger.hpp"
#include "elf/elf.hpp"
#include "loader/deferred_symbol_breakpoints.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
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
  UserBreakpointRegistry(Debugger& debugger, const ElfFile& executable);

  std::size_t add_address(std::uintptr_t address, std::string expression);
  std::size_t add_symbol(std::string symbol);
  bool remove(std::size_t id);

  [[nodiscard]] std::optional<UserBreakpoint> breakpoint(std::size_t id) const;
  [[nodiscard]] std::vector<UserBreakpoint> breakpoints() const;
  [[nodiscard]] bool has_pending() const;

  StopInfo continue_execution(SignalPolicy policy = SignalPolicy::Suppress);

 private:
  enum class BackendKind { Managed, Deferred };

  struct Entry {
    std::string expression;
    BackendKind kind;
    std::size_t backend_id;
  };

  [[nodiscard]] UserBreakpoint snapshot(std::size_t id, const Entry& entry) const;

  Debugger& debugger_;
  const ElfFile& executable_;
  DeferredSymbolBreakpoints deferred_;
  std::map<std::size_t, Entry> entries_;
  std::size_t next_id_{1};
};

const char* user_breakpoint_state_name(UserBreakpointState state) noexcept;

}  // namespace mdbg

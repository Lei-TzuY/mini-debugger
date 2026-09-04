#pragma once

#include "debugger/debugger.hpp"
#include "elf/elf.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mdbg {

enum class DeferredBreakpointTargetKind { Symbol, Source };

struct DeferredBreakpointTarget {
  DeferredBreakpointTargetKind kind;
  std::string name;
  std::uint64_t line{0};
};

struct DeferredBreakpoint {
  std::size_t request_id;
  DeferredBreakpointTarget target;
  std::optional<std::size_t> breakpoint_id;
  std::optional<std::uintptr_t> address;
};

class DeferredBreakpoints {
 public:
  DeferredBreakpoints(Debugger& debugger, const ElfFile& executable);
  ~DeferredBreakpoints();

  DeferredBreakpoints(const DeferredBreakpoints&) = delete;
  DeferredBreakpoints& operator=(const DeferredBreakpoints&) = delete;

  std::size_t add_symbol(std::string symbol);
  std::size_t add_source(std::string file, std::uint64_t line);
  bool remove(std::size_t request_id);
  [[nodiscard]] std::vector<DeferredBreakpoint> breakpoints() const;

  StopInfo continue_execution(SignalPolicy policy = SignalPolicy::Suppress);

 private:
  struct LoaderMetadata {
    std::uint64_t entry_virtual_address;
    std::uint64_t dt_debug_value_virtual_address;
  };

  static LoaderMetadata read_loader_metadata(const std::string& path);

  [[nodiscard]] std::uint64_t load_bias() const;
  [[nodiscard]] std::uintptr_t runtime_address(std::uint64_t virtual_address) const;
  [[nodiscard]] std::uint64_t read_u64(std::uintptr_t address) const;
  [[nodiscard]] std::optional<std::uintptr_t> current_r_debug_address() const;
  [[nodiscard]] std::uintptr_t current_loader_break_address() const;
  [[nodiscard]] bool loader_state_is_consistent() const;
  [[nodiscard]] std::optional<std::uintptr_t> resolve_target(
      const DeferredBreakpointTarget& target) const;

  std::size_t add_target(DeferredBreakpointTarget target);
  void ensure_monitoring();
  bool try_install_loader_breakpoint();
  void install_bootstrap_breakpoint();
  void reconcile_requests();
  void suspend_monitoring() noexcept;
  void stop_monitoring_if_idle();
  bool is_internal_stop(const StopInfo& stop) const;
  void handle_internal_stop(const StopInfo& stop);
  void remove_internal_breakpoint(std::optional<std::size_t>& id,
                                  std::optional<std::uintptr_t>& address) noexcept;

  Debugger& debugger_;
  const ElfFile& executable_;
  LoaderMetadata metadata_;
  std::map<std::size_t, DeferredBreakpoint> requests_;
  std::size_t next_request_id_{1};
  std::optional<std::uintptr_t> r_debug_address_;
  std::optional<std::size_t> bootstrap_breakpoint_id_;
  std::optional<std::uintptr_t> bootstrap_breakpoint_address_;
  std::optional<std::size_t> loader_breakpoint_id_;
  std::optional<std::uintptr_t> loader_breakpoint_address_;
};

using DeferredSymbolBreakpoints = DeferredBreakpoints;
using DeferredSymbolBreakpoint = DeferredBreakpoint;

}  // namespace mdbg

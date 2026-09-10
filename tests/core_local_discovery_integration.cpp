#include "dwarf/local_value.hpp"
#include "snapshot/session.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

bool has_name(const std::vector<mdbg::LocalDiscoveryEntry>& entries,
              const std::string& name) {
  return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
    return entry.name == name;
  });
}

void require_bounded_catalogue(const std::vector<mdbg::LocalDiscoveryEntry>& entries,
                               const std::string& context) {
  require(!entries.empty(), context + " unexpectedly discovered no active locals");
  require(entries.size() <= 64, context + " exceeded the bounded local catalogue");
  for (std::size_t index = 1; index < entries.size(); ++index) {
    require(entries[index - 1].name < entries[index].name,
            context + " local catalogue is not uniquely name-sorted");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_local_discovery_integration <core> <sibling-tid>\n";
    return 2;
  }

  try {
    const auto sibling_tid = static_cast<pid_t>(std::stol(argv[2]));
    mdbg::CoreInspectionSession session(argv[1]);

    const auto crash_locals = session.locals();
    require_bounded_catalogue(crash_locals, "crash frame");
    require(has_name(crash_locals, "xmm_value"),
            "crash frame did not discover compiler-owned xmm_value");
    require(has_name(crash_locals, "stack_local"),
            "crash frame did not discover compiler-owned stack_local");
    require(!has_name(crash_locals, "caller_stack_local"),
            "crash frame leaked a caller-only local");

    require(session.trace().frames.size() > 1,
            "genuine core did not recover the caller frame required for discovery");
    session.select_frame(1);
    const auto caller_locals = session.locals();
    require_bounded_catalogue(caller_locals, "caller frame");
    require(has_name(caller_locals, "caller_stack_local"),
            "caller frame did not discover compiler-owned caller_stack_local");
    require(!has_name(caller_locals, "xmm_value"),
            "caller frame leaked a callee-only local");

    session.select_thread(sibling_tid);
    require(session.selected_frame_index() == 0,
            "thread selection did not reset scoped discovery to frame zero");
    const auto sibling_locals = session.locals();
    require_bounded_catalogue(sibling_locals, "sibling frame");
    require(has_name(sibling_locals, "xmm_value"),
            "sibling frame did not discover its compiler-owned xmm_value");
    require(!has_name(sibling_locals, "stack_local") &&
                !has_name(sibling_locals, "caller_stack_local"),
            "sibling frame leaked locals from the crash-thread selection");

    std::cout << "bounded core local discovery integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "core local discovery integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

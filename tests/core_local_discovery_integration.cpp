#include "dwarf/local_value.hpp"
#include "snapshot/session.hpp"

#include <sys/wait.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
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

std::string shell_quote(const std::string& value) {
  std::string result("'");
  for (const char ch : value) {
    if (ch == '\'') {
      result += "'\\''";
    } else {
      result += ch;
    }
  }
  result += '\'';
  return result;
}

std::string run_command(const std::string& command) {
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) throw std::runtime_error("failed to start mdbg-core subprocess");
  std::string output;
  char buffer[512];
  while (::fgets(buffer, sizeof(buffer), pipe) != nullptr) output += buffer;
  const int status = ::pclose(pipe);
  if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    throw std::runtime_error("mdbg-core subprocess failed:\n" + output);
  }
  return output;
}

void require_cli_catalogue(const std::string& segment, const std::string& present,
                           const std::string& absent, const std::string& context) {
  require(segment.find("variable " + present) != std::string::npos,
          context + " CLI did not discover " + present);
  require(segment.find("variable " + absent) == std::string::npos,
          context + " CLI leaked " + absent);
}

void test_core_cli(const std::string& integration_path, const std::string& core_path,
                   pid_t sibling_tid) {
  const auto mdbg_core =
      std::filesystem::absolute(integration_path).parent_path() / "mdbg-core";
  require(std::filesystem::exists(mdbg_core), "mdbg-core executable is unavailable");

  const std::string script = "locals\\nframe 1\\nlocals\\nthread " +
                             std::to_string(sibling_tid) + "\\nlocals\\nquit\\n";
  const std::string command = "printf '" + script + "' | " +
                              shell_quote(mdbg_core.string()) + " " +
                              shell_quote(core_path) + " 2>&1";
  const auto output = run_command(command);

  const auto frame_marker = output.find("selected frame 1");
  const auto thread_marker = output.find("selected thread " + std::to_string(sibling_tid));
  require(frame_marker != std::string::npos && thread_marker != std::string::npos &&
              frame_marker < thread_marker,
          "mdbg-core did not preserve frame/thread selection markers");

  const auto crash_segment = output.substr(0, frame_marker);
  const auto caller_segment = output.substr(frame_marker, thread_marker - frame_marker);
  const auto sibling_segment = output.substr(thread_marker);
  require_cli_catalogue(crash_segment, "xmm_value", "caller_stack_local", "crash frame");
  require_cli_catalogue(caller_segment, "caller_stack_local", "xmm_value", "caller frame");
  require_cli_catalogue(sibling_segment, "xmm_value", "stack_local", "sibling frame");
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

    test_core_cli(argv[0], argv[1], sibling_tid);

    std::cout << "bounded core local discovery integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "core local discovery integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

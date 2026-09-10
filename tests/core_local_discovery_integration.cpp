#include "dwarf/local_value.hpp"
#include "snapshot/session.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kInnerShadowValue = 0xaaaabbbbccccddddULL;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

bool has_name(const std::vector<mdbg::LocalDiscoveryEntry>& entries,
              const std::string& name) {
  return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
    return entry.name == name;
  });
}

std::size_t count_name(const std::vector<mdbg::LocalDiscoveryEntry>& entries,
                       const std::string& name) {
  return static_cast<std::size_t>(
      std::count_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return entry.name == name;
      }));
}

bool has_entry(const std::vector<mdbg::LocalDiscoveryEntry>& entries,
               const std::string& name, mdbg::LocalDiscoveryKind kind) {
  return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
    return entry.name == name && entry.kind == kind;
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
  if (pipe == nullptr) throw std::runtime_error("failed to start subprocess");
  std::string output;
  char buffer[512];
  while (::fgets(buffer, sizeof(buffer), pipe) != nullptr) output += buffer;
  const int status = ::pclose(pipe);
  if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    throw std::runtime_error("subprocess failed:\n" + output);
  }
  return output;
}

std::size_t count_substring(const std::string& text, const std::string& needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = text.find(needle, offset)) != std::string::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
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
  require(count_substring(crash_segment, "variable shadow_value") == 1,
          "crash frame CLI did not collapse shadowed locals to one active binding");
  require_cli_catalogue(caller_segment, "caller_stack_local", "xmm_value", "caller frame");
  require(caller_segment.find("shadow_value") == std::string::npos,
          "caller frame CLI leaked a callee shadowed local");
  require_cli_catalogue(sibling_segment, "xmm_value", "stack_local", "sibling frame");
  require(sibling_segment.find("parameter seed") != std::string::npos,
          "sibling frame CLI did not classify seed as a formal parameter");
  require(sibling_segment.find("shadow_value") == std::string::npos,
          "sibling frame CLI leaked a crash-frame shadowed local");
}

void require_inline_cli_scope(const std::string& segment, const std::string& present,
                              const std::string& absent, const std::string& context) {
  require(segment.find("variable " + present) != std::string::npos,
          context + " did not expose its owned local");
  require(segment.find("variable shadow_value") != std::string::npos,
          context + " did not expose its shadowed binding");
  require(segment.find("variable " + absent) == std::string::npos,
          context + " leaked a nested/sibling inline local");
}

void test_inline_artifact_gate(const std::string& integration_path,
                               const mdbg::CoreInspectionSession& baseline_session) {
  const char* compiler_env = std::getenv("CC");
  require(compiler_env != nullptr && *compiler_env != '\0',
          "CC is unavailable for optimized inline compiler evidence");
  const std::string compiler(compiler_env);
  const bool non_pie = baseline_session.selected_frame().module_path.find("_nopie") !=
                       std::string::npos;
  const auto tag = std::string(non_pie ? "nopie-" : "pie-") + std::to_string(::getpid());
  const auto fixture = std::filesystem::path("/tmp") / ("mdbg-inline-" + tag);
  const std::string mode = non_pie ? " -fno-pie -no-pie " : " -fPIE -pie ";
  const std::string compile = shell_quote(compiler) +
                              " -O2 -g -gdwarf-4 " + mode +
                              " tests/fixtures/inline_core_fixture.c -o " +
                              shell_quote(fixture.string()) + " 2>&1";
  (void)run_command(compile);
  (void)run_command("python3 tests/core_inline_dwarf_oracle.py " +
                    shell_quote(fixture.string()) + " 2>&1");

  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("failed to fork optimized inline fixture");
  if (child == 0) {
    ::execl(fixture.c_str(), fixture.c_str(), nullptr);
    _exit(127);
  }
  int status = 0;
  while (::waitpid(child, &status, 0) == -1) {
  }
  require(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
          "optimized inline fixture did not terminate with SIGSEGV");
  const auto core_path = std::filesystem::path("/tmp") /
                         ("mdbg-core-" + std::to_string(child));
  require(std::filesystem::exists(core_path),
          "optimized inline fixture did not produce the expected genuine core");

  mdbg::CoreInspectionSession inline_session(core_path.string());
  const auto contexts = inline_session.inline_contexts();
  require(contexts.size() == 2, "optimized core did not expose exactly two inline contexts");
  require(contexts[0].name == "inline_outer" && contexts[0].depth == 0 &&
              contexts[1].name == "inline_inner" && contexts[1].depth == 1,
          "inline contexts were not ordered outer-to-inner with bounded depth");
  require(contexts[0].module_path == inline_session.selected_frame().module_path &&
              contexts[1].module_path == inline_session.selected_frame().module_path,
          "inline contexts lost physical-frame module ownership");
  require(contexts[0].call_site.line != 0 && contexts[1].call_site.line != 0,
          "inline contexts lost compiler call-site line metadata");

  const auto physical = inline_session.locals();
  require(has_name(physical, "physical_only"),
          "physical frame did not retain its compiler-owned local");
  require(!has_name(physical, "outer_only") && !has_name(physical, "inner_only") &&
              !has_name(physical, "shadow_value"),
          "physical local discovery flattened inline descendants");

  inline_session.select_inline_context(0);
  require(inline_session.selected_inline_context_index() == 0,
          "outer inline selection was not recorded");
  const auto outer = inline_session.locals();
  require(has_name(outer, "outer_only") && has_name(outer, "shadow_value") &&
              !has_name(outer, "inner_only") && count_name(outer, "shadow_value") == 1,
          "outer inline local ownership/shadowing is incorrect");

  inline_session.select_inline_context(1);
  require(inline_session.selected_inline_context_index() == 1,
          "inner inline selection was not recorded");
  const auto inner = inline_session.locals();
  require(has_name(inner, "inner_only") && has_name(inner, "shadow_value") &&
              !has_name(inner, "outer_only") && count_name(inner, "shadow_value") == 1,
          "inner inline local ownership/shadowing is incorrect");
  bool materialization_rejected = false;
  try {
    (void)inline_session.inspect_value("shadow_value");
  } catch (const std::logic_error&) {
    materialization_rejected = true;
  }
  require(materialization_rejected,
          "inline selection silently reused physical value materialization semantics");

  inline_session.select_frame(0);
  require(!inline_session.selected_inline_context_index(),
          "physical frame selection did not invalidate inline ownership");
  inline_session.select_inline_context(1);
  const auto selected_tid = inline_session.selected_thread_tid();
  inline_session.select_thread(selected_tid);
  require(!inline_session.selected_inline_context_index() &&
              inline_session.selected_frame_index() == 0,
          "thread selection did not invalidate inline ownership and reset frame zero");

  const auto mdbg_core =
      std::filesystem::absolute(integration_path).parent_path() / "mdbg-core";
  const std::string script =
      "inline\\ninline 0\\nlocals\\ninline 1\\nlocals\\ninline physical\\nlocals\\n"
      "inline 1\\nframe 0\\ninline\\nquit\\n";
  const std::string command = "printf '" + script + "' | " +
                              shell_quote(mdbg_core.string()) + " " +
                              shell_quote(core_path.string()) + " 2>&1";
  const auto output = run_command(command);
  const auto outer_marker = output.find("selected inline 0");
  const auto inner_marker = output.find("selected inline 1", outer_marker);
  const auto physical_marker = output.find("selected physical frame 0", inner_marker);
  const auto frame_marker = output.find("selected frame 0", physical_marker);
  require(outer_marker != std::string::npos && inner_marker != std::string::npos &&
              physical_marker != std::string::npos && frame_marker != std::string::npos &&
              outer_marker < inner_marker && inner_marker < physical_marker &&
              physical_marker < frame_marker,
          "mdbg-core inline/physical/frame selection markers are incomplete");
  require(output.substr(0, outer_marker).find("inline_outer") != std::string::npos &&
              output.substr(0, outer_marker).find("inline_inner") != std::string::npos,
          "mdbg-core did not list the compiler-proven inline call chain");
  require_inline_cli_scope(output.substr(outer_marker, inner_marker - outer_marker),
                           "outer_only", "inner_only", "outer inline CLI scope");
  require_inline_cli_scope(output.substr(inner_marker, physical_marker - inner_marker),
                           "inner_only", "outer_only", "inner inline CLI scope");
  const auto physical_segment = output.substr(physical_marker, frame_marker - physical_marker);
  require(physical_segment.find("variable physical_only") != std::string::npos &&
              physical_segment.find("variable outer_only") == std::string::npos &&
              physical_segment.find("variable inner_only") == std::string::npos &&
              physical_segment.find("variable shadow_value") == std::string::npos,
          "mdbg-core physical scope did not exclude inline descendants");
  require(output.substr(frame_marker).find("* inline") == std::string::npos,
          "frame selection retained stale inline selection");

  std::error_code error;
  std::filesystem::remove(core_path, error);
  std::filesystem::remove(fixture, error);
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
    require(count_name(crash_locals, "shadow_value") == 1,
            "crash frame did not collapse shadowed locals to one active binding");
    const auto shadow_value = session.inspect_value("shadow_value");
    require(shadow_value.raw_value == kInnerShadowValue,
            "shadowed-name materialization did not resolve the deepest active binding");
    require(!has_name(crash_locals, "caller_stack_local"),
            "crash frame leaked a caller-only local");

    require(session.trace().frames.size() > 1,
            "genuine core did not recover the caller frame required for discovery");
    session.select_frame(1);
    const auto caller_locals = session.locals();
    require_bounded_catalogue(caller_locals, "caller frame");
    require(has_name(caller_locals, "caller_stack_local"),
            "caller frame did not discover compiler-owned caller_stack_local");
    require(!has_name(caller_locals, "xmm_value") && !has_name(caller_locals, "shadow_value"),
            "caller frame leaked callee-only locals");

    session.select_thread(sibling_tid);
    require(session.selected_frame_index() == 0,
            "thread selection did not reset scoped discovery to frame zero");
    const auto sibling_locals = session.locals();
    require_bounded_catalogue(sibling_locals, "sibling frame");
    require(has_name(sibling_locals, "xmm_value"),
            "sibling frame did not discover its compiler-owned xmm_value");
    require(has_entry(sibling_locals, "seed", mdbg::LocalDiscoveryKind::FormalParameter),
            "sibling frame did not classify compiler-owned seed as a formal parameter");
    require(!has_name(sibling_locals, "stack_local") &&
                !has_name(sibling_locals, "caller_stack_local") &&
                !has_name(sibling_locals, "shadow_value"),
            "sibling frame leaked locals from the crash-thread selection");

    test_core_cli(argv[0], argv[1], sibling_tid);
    test_inline_artifact_gate(argv[0], session);

    std::cout << "bounded core local discovery integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "core local discovery integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

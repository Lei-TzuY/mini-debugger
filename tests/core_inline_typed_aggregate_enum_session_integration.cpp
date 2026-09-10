#include "snapshot/session.hpp"

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string generate_core(const std::string& fixture) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for typed-aggregate fixture");
  if (child == 0) {
    rlimit core_limit{};
    if (::getrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(120);
    core_limit.rlim_cur = core_limit.rlim_max;
    if (::setrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(121);
    ::execl(fixture.c_str(), fixture.c_str(), nullptr);
    _exit(127);
  }

  const auto core_path = "/tmp/mdbg-core-" + std::to_string(child);
  std::remove(core_path.c_str());
  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  require(waited == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
          "typed-aggregate fixture did not terminate from deterministic SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce the typed-aggregate core");
  return core_path;
}

std::size_t selected_inner_index(const mdbg::CoreInspectionSession& session) {
  const auto contexts = session.inline_contexts();
  for (std::size_t index = 0; index < contexts.size(); ++index) {
    if (contexts[index].name == "caller_typed_inline_inner") return index;
  }
  throw std::runtime_error("caller_typed_inline_inner inline context is unavailable");
}

bool supported_snapshot_storage(mdbg::LocalValueStorage storage) {
  return storage == mdbg::LocalValueStorage::SnapshotCoreMemory ||
         storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact;
}

const mdbg::LocalStructMember& member_named(const mdbg::LocalScalarValue& value,
                                            std::string_view name) {
  const auto it = std::find_if(value.members.begin(), value.members.end(),
                               [name](const mdbg::LocalStructMember& member) {
                                 return member.name == name;
                               });
  if (it == value.members.end()) {
    throw std::runtime_error("typed aggregate member is unavailable: " +
                             std::string(name));
  }
  return *it;
}

void require_enum_type(const mdbg::LocalScalarValue& value) {
  require(value.kind == mdbg::LocalValueKind::Enumeration,
          "typed aggregate mode leaf lost enum kind");
  require(value.byte_size == 4 && !value.is_signed && value.raw_value == 42,
          "typed aggregate mode leaf lost uint32 raw value 42");
  require(value.enum_type.has_value(),
          "typed aggregate mode leaf lost enum metadata");
  require(value.enum_type->name == "CallerInlineTypedMode" &&
              value.enum_type->byte_size == 4 && !value.enum_type->is_signed,
          "typed aggregate mode leaf lost CallerInlineTypedMode representation");
  require(value.enum_type->enumerators.size() == 3,
          "typed aggregate mode leaf lost bounded enumerator table");
  const auto symbol = mdbg::local_enum_symbol(value);
  require(symbol && *symbol == "CallerInlineTypedBusy",
          "typed aggregate mode leaf did not retain exact symbolic identity");
}

std::string run_core_cli(const std::string& cli, const std::string& core,
                         std::size_t inline_index) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create typed-aggregate CLI pipes");
  }
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for typed-aggregate CLI");
  if (child == 0) {
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    ::execl(cli.c_str(), cli.c_str(), core.c_str(), nullptr);
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  const std::string script =
      "frame 1\ninline " + std::to_string(inline_index) +
      "\nprint caller_typed_aggregate\n"
      "aggregate-member caller_typed_aggregate mode\nquit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset,
                               script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write typed-aggregate CLI script");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read typed-aggregate CLI output");
    if (count == 0) break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(output_pipe[0]);

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "typed-aggregate mdbg-core workflow did not exit cleanly");
  return output;
}

void exercise(const std::string& fixture, const std::string& cli) {
  const auto core = generate_core(fixture);
  try {
    mdbg::CoreInspectionSession session(core);
    require(session.trace().frames.size() > 1,
            "typed-aggregate core did not retain historical physical frame 1");
    session.select_frame(1);
    const auto inner = selected_inner_index(session);
    session.select_inline_context(inner);

    const auto aggregate = session.inspect_value("caller_typed_aggregate");
    require(aggregate.kind == mdbg::LocalValueKind::Structure &&
                aggregate.byte_size == 8 && aggregate.members.size() == 2,
            "typed selected-inline aggregate was not materialized as an eight-byte structure");
    require(supported_snapshot_storage(aggregate.storage),
            "typed selected-inline aggregate lost immutable snapshot provenance");
    const auto& direct = member_named(aggregate, "direct");
    require(direct.kind == mdbg::LocalValueKind::Integer && direct.byte_size == 4 &&
                direct.is_signed && direct.raw_value == UINT64_C(0x31415926),
            "typed aggregate integer member changed while composing enum identity");
    const auto& mode = member_named(aggregate, "mode");
    require(mode.kind == mdbg::LocalValueKind::Enumeration && mode.byte_size == 4 &&
                !mode.is_signed && mode.raw_value == 42,
            "typed aggregate materialization collapsed enum member identity");

    const auto selected =
        session.inspect_aggregate_member("caller_typed_aggregate", "mode");
    require_enum_type(selected);
    require(supported_snapshot_storage(selected.storage),
            "selected typed aggregate enum member lost immutable provenance");

    auto unknown = selected;
    unknown.raw_value = 11;
    require(!mdbg::local_enum_symbol(unknown),
            "unknown composed enum raw value must remain numeric");
    auto ambiguous = selected;
    ambiguous.enum_type->enumerators.push_back({"CallerInlineTypedBusyAlias", 42});
    require(!mdbg::local_enum_symbol(ambiguous),
            "duplicate composed enum raw values must remain symbolically ambiguous");

    const auto output = run_core_cli(cli, core, inner);
    require(output.find(
                "caller_typed_aggregate.mode = CallerInlineTypedMode::CallerInlineTypedBusy (0x2a)") !=
                std::string::npos,
            "mdbg-core did not render composed enum member symbol and numeric value");
    require(output.find("[4-byte enum unsigned]") != std::string::npos,
            "mdbg-core did not render composed enum member representation metadata");
  } catch (...) {
    std::remove(core.c_str());
    throw;
  }
  std::remove(core.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_inline_typed_aggregate_enum_session_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    exercise(argv[1], argv[2]);
    std::cout << "selected-inline typed aggregate enum integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "selected-inline typed aggregate enum integration failure: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}

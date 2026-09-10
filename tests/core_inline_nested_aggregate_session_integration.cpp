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
  if (child == -1) throw std::runtime_error("fork failed for nested-aggregate fixture");
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
          "nested-aggregate fixture did not terminate from deterministic SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce the nested-aggregate core");
  return core_path;
}

std::size_t selected_inner_index(const mdbg::CoreInspectionSession& session) {
  const auto contexts = session.inline_contexts();
  for (std::size_t index = 0; index < contexts.size(); ++index) {
    if (contexts[index].name == "caller_nested_inline_inner") return index;
  }
  throw std::runtime_error("caller_nested_inline_inner inline context is unavailable");
}

bool supported_snapshot_storage(mdbg::LocalValueStorage storage) {
  return storage == mdbg::LocalValueStorage::SnapshotCoreMemory ||
         storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact;
}

const mdbg::LocalStructMember& member_named(const std::vector<mdbg::LocalStructMember>& members,
                                            std::string_view name) {
  const auto it = std::find_if(members.begin(), members.end(),
                               [name](const mdbg::LocalStructMember& member) {
                                 return member.name == name;
                               });
  if (it == members.end()) {
    throw std::runtime_error("nested aggregate member is unavailable: " +
                             std::string(name));
  }
  return *it;
}

std::string run_core_cli(const std::string& cli, const std::string& core,
                         std::size_t inline_index) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create nested-aggregate CLI pipes");
  }
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for nested-aggregate CLI");
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
      "\nprint caller_nested_aggregate\n"
      "nested-aggregate-member caller_nested_aggregate inner terminal\nquit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset,
                               script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write nested-aggregate CLI script");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read nested-aggregate CLI output");
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
          "nested-aggregate mdbg-core workflow did not exit cleanly");
  return output;
}

void exercise(const std::string& fixture, const std::string& cli) {
  const auto core = generate_core(fixture);
  try {
    mdbg::CoreInspectionSession session(core);
    require(session.trace().frames.size() > 1,
            "nested-aggregate core did not retain historical physical frame 1");
    session.select_frame(1);
    const auto inline_index = selected_inner_index(session);
    session.select_inline_context(inline_index);

    const auto aggregate = session.inspect_value("caller_nested_aggregate");
    require(aggregate.kind == mdbg::LocalValueKind::Structure &&
                aggregate.byte_size == 8 && aggregate.members.size() == 2,
            "nested selected-inline outer value was not materialized as an eight-byte structure");
    require(supported_snapshot_storage(aggregate.storage),
            "nested outer aggregate lost immutable snapshot provenance");

    const auto& prefix = member_named(aggregate.members, "prefix");
    require(prefix.kind == mdbg::LocalValueKind::Integer && prefix.byte_size == 4 &&
                prefix.is_signed && prefix.raw_value == UINT64_C(0x10203040),
            "nested outer prefix member changed during composition");

    const auto& inner = member_named(aggregate.members, "inner");
    require(inner.kind == mdbg::LocalValueKind::Structure && inner.byte_size == 4 &&
                !inner.is_signed && inner.members.size() == 1,
            "inner aggregate identity was flattened or lost");
    const auto& terminal = member_named(inner.members, "terminal");
    require(terminal.kind == mdbg::LocalValueKind::Integer && terminal.byte_size == 4 &&
                terminal.is_signed && terminal.raw_value == UINT64_C(0x55667788),
            "nested terminal member was not materialized from outer-owned bytes");

    const auto selected = session.inspect_nested_aggregate_member(
        "caller_nested_aggregate", "inner", "terminal");
    require(selected.name == "caller_nested_aggregate.inner.terminal" &&
                selected.kind == mdbg::LocalValueKind::Integer &&
                selected.byte_size == 4 && selected.is_signed &&
                selected.raw_value == UINT64_C(0x55667788),
            "explicit nested aggregate selection did not yield the terminal scalar");
    require(selected.storage == aggregate.storage,
            "nested aggregate selection changed immutable snapshot provenance kind");

    const auto output = run_core_cli(cli, core, inline_index);
    require(output.find("caller_nested_aggregate.inner.terminal") != std::string::npos,
            "mdbg-core did not render the explicit nested aggregate member path");
    require(output.find("0x55667788") != std::string::npos,
            "mdbg-core did not render the nested terminal value");
  } catch (...) {
    std::remove(core.c_str());
    throw;
  }
  std::remove(core.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_inline_nested_aggregate_session_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    exercise(argv[1], argv[2]);
    std::cout << "selected-inline nested aggregate integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "selected-inline nested aggregate integration failure: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}

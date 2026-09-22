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
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string generate_core(const std::string& fixture) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("failed to fork frame-zero nested fixture");
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
          "frame-zero nested fixture did not terminate with SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce frame-zero inline nested core");
  return core_path;
}

std::size_t inner_index(const mdbg::CoreInspectionSession& session) {
  const auto contexts = session.inline_contexts();
  for (std::size_t index = 0; index < contexts.size(); ++index) {
    if (contexts[index].name == "frame_zero_inline_nested_inner") return index;
  }
  throw std::runtime_error("frame-zero inline nested context is unavailable");
}

const mdbg::LocalStructMember& member_named(
    const std::vector<mdbg::LocalStructMember>& members,
    std::string_view name) {
  const auto it = std::find_if(
      members.begin(), members.end(),
      [name](const mdbg::LocalStructMember& member) {
        return member.name == name;
      });
  if (it == members.end()) {
    throw std::runtime_error(
        "frame-zero nested member is unavailable: " + std::string(name));
  }
  return *it;
}

std::string run_cli(const std::string& cli, const std::string& core,
                    std::size_t inline_index) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create frame-zero nested CLI pipes");
  }
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("failed to fork mdbg-core");
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
      "inline " + std::to_string(inline_index) +
      "\nprint inline_nested\n"
      "nested-aggregate-member inline_nested inner terminal\n"
      "inline physical\nquit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset,
                               script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write frame-zero nested CLI");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read frame-zero nested CLI");
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
          "mdbg-core frame-zero nested workflow did not exit cleanly");
  return output;
}

void exercise(const std::string& fixture, const std::string& cli) {
  const auto core = generate_core(fixture);
  try {
    mdbg::CoreInspectionSession session(core);
    require(session.selected_frame_index() == 0,
            "frame-zero nested core did not start on physical frame zero");
    const auto index = inner_index(session);
    session.select_inline_context(index);

    const auto value = session.inspect_value("inline_nested");
    require(value.kind == mdbg::LocalValueKind::Structure &&
                value.byte_size == 8 && value.members.size() == 2,
            "frame-zero nested value lost bounded outer structure identity");
    require(value.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
            "frame-zero nested value lost stack/core provenance");

    const auto& prefix = member_named(value.members, "prefix");
    require(prefix.kind == mdbg::LocalValueKind::Integer &&
                prefix.byte_size == 4 && prefix.is_signed &&
                prefix.raw_value == UINT64_C(0x10203040),
            "frame-zero nested prefix is incorrect");

    const auto& inner = member_named(value.members, "inner");
    require(inner.kind == mdbg::LocalValueKind::Structure &&
                inner.byte_size == 4 && inner.offset == 4 &&
                inner.members.size() == 1,
            "frame-zero nested inner structure identity is incorrect");
    const auto& terminal = member_named(inner.members, "terminal");
    require(terminal.kind == mdbg::LocalValueKind::Integer &&
                terminal.byte_size == 4 && terminal.is_signed &&
                terminal.offset == 0 &&
                terminal.raw_value == UINT64_C(0x55667788),
            "frame-zero nested terminal is incorrect");

    const auto selected = session.inspect_nested_aggregate_member(
        "inline_nested", "inner", "terminal");
    require(selected.name == "inline_nested.inner.terminal" &&
                selected.raw_value == UINT64_C(0x55667788) &&
                selected.byte_size == 4 && selected.is_signed &&
                selected.storage == value.storage,
            "frame-zero nested explicit traversal lost value/provenance");

    const auto output = run_cli(cli, core, index);
    require(output.find("inline_nested = {") != std::string::npos &&
                output.find("prefix=0x10203040") != std::string::npos,
            "mdbg-core did not render frame-zero nested outer value");
    require(output.find("inline_nested.inner.terminal = 0x55667788") !=
                std::string::npos,
            "mdbg-core did not render frame-zero nested terminal");
    require(output.find("selected physical frame 0") != std::string::npos,
            "mdbg-core did not clear frame-zero nested ownership");

    session.select_frame(0);
    require(!session.selected_inline_context_index(),
            "physical frame selection did not invalidate frame-zero nested ownership");
  } catch (...) {
    std::remove(core.c_str());
    throw;
  }
  std::remove(core.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr
        << "usage: core_frame_zero_inline_nested_aggregate_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    exercise(argv[1], argv[2]);
    std::cout << "frame-zero selected-inline nested aggregate integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "frame-zero selected-inline nested aggregate integration failure: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}

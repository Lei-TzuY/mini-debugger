#include "snapshot/session.hpp"

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string generate_core(const std::string& fixture) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for selected-inline union fixture");
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
          "selected-inline union fixture did not terminate from deterministic SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce the selected-inline union core");
  return core_path;
}

std::size_t selected_inner_index(const mdbg::CoreInspectionSession& session) {
  const auto contexts = session.inline_contexts();
  for (std::size_t index = 0; index < contexts.size(); ++index) {
    if (contexts[index].name == "caller_inline_inner") return index;
  }
  throw std::runtime_error("caller_inline_inner inline context is unavailable");
}

bool supported_snapshot_storage(mdbg::LocalValueStorage storage) {
  return storage == mdbg::LocalValueStorage::SnapshotCoreMemory ||
         storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact;
}

std::string run_core_cli(const std::string& cli, const std::string& core,
                         std::size_t inline_index) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create mdbg-core union pipes");
  }
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for mdbg-core union workflow");
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
      "\nprint caller_union\n"
      "union-member caller_union signed_value\n"
      "union-member caller_union unsigned_value\n"
      "union-member caller_union missing\n"
      "quit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset, script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write mdbg-core union commands");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read mdbg-core union output");
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
          "mdbg-core union workflow did not exit cleanly");
  return output;
}

void exercise(const std::string& fixture, const std::string& cli) {
  const auto core = generate_core(fixture);
  try {
    mdbg::CoreInspectionSession session(core);
    require(session.trace().frames.size() > 1,
            "selected-inline union core did not retain historical physical frame 1");
    session.select_frame(1);
    const auto inner = selected_inner_index(session);
    session.select_inline_context(inner);

    const auto value = session.inspect_value("caller_union");
    require(value.kind == mdbg::LocalValueKind::Union,
            "caller_union was not materialized as an explicit union container");
    require(value.byte_size == sizeof(int) && value.members.size() == 2,
            "caller_union lost bounded overlapping-member metadata");
    require(value.members[0].name == "signed_value" &&
                value.members[0].byte_size == sizeof(int) && value.members[0].is_signed &&
                value.members[1].name == "unsigned_value" &&
                value.members[1].byte_size == sizeof(unsigned int) &&
                !value.members[1].is_signed,
            "caller_union member metadata does not match compiler evidence");
    require(supported_snapshot_storage(value.storage),
            "caller_union lost immutable snapshot provenance");

    const auto signed_view = session.inspect_union_member("caller_union", "signed_value");
    require(signed_view.kind == mdbg::LocalValueKind::Integer &&
                signed_view.raw_value == UINT64_C(0x44556677) && signed_view.is_signed,
            "explicit signed union member selection returned the wrong view");
    require(supported_snapshot_storage(signed_view.storage),
            "signed union member selection lost snapshot provenance");

    const auto unsigned_view = session.inspect_union_member("caller_union", "unsigned_value");
    require(unsigned_view.kind == mdbg::LocalValueKind::Integer &&
                unsigned_view.raw_value == UINT64_C(0x44556677) && !unsigned_view.is_signed,
            "explicit unsigned union member selection returned the wrong view");

    bool unknown_rejected = false;
    try {
      (void)session.inspect_union_member("caller_union", "missing");
    } catch (const std::runtime_error&) {
      unknown_rejected = true;
    }
    require(unknown_rejected, "unknown union member selection was accepted");

    bool aggregate_alias_rejected = false;
    try {
      (void)session.inspect_aggregate_member("caller_union", "signed_value");
    } catch (const std::runtime_error&) {
      aggregate_alias_rejected = true;
    }
    require(aggregate_alias_rejected,
            "union member selection leaked through the structure-only aggregate command");

    const auto output = run_core_cli(cli, core, inner);
    require(output.find("caller_union = union{signed_value, unsigned_value}") != std::string::npos,
            "mdbg-core inferred or failed to render explicit union member choices");
    require(output.find("caller_union.signed_value = 0x44556677") != std::string::npos,
            "mdbg-core did not render explicit signed union member selection");
    require(output.find("caller_union.unsigned_value = 0x44556677") != std::string::npos,
            "mdbg-core did not render explicit unsigned union member selection");
    require(output.find("bounded selected-inline union has no member named: missing") !=
                std::string::npos,
            "mdbg-core did not deterministically reject an unknown union member");
  } catch (...) {
    std::remove(core.c_str());
    throw;
  }
  std::remove(core.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_inline_union_session_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    exercise(argv[1], argv[2]);
    std::cout << "selected-inline union integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "selected-inline union integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

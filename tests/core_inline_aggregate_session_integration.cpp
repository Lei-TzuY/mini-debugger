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
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct GeneratedCore {
  std::string path;
  pid_t crash_tid;
};

GeneratedCore generate_core(const std::string& fixture) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for caller-inline core fixture");
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
          "caller-inline aggregate fixture did not terminate from deterministic SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce the caller-inline aggregate core");
  return GeneratedCore{core_path, child};
}

std::size_t selected_inner_index(const mdbg::CoreInspectionSession& session) {
  const auto contexts = session.inline_contexts();
  for (std::size_t index = 0; index < contexts.size(); ++index) {
    if (contexts[index].name == "caller_inline_inner") return index;
  }
  throw std::runtime_error("caller_inline_inner inline context is unavailable");
}

pid_t sibling_tid(const mdbg::CoreInspectionSession& session, pid_t crash_tid) {
  for (const auto& thread : session.threads()) {
    if (thread.tid != crash_tid) return thread.tid;
  }
  throw std::runtime_error("caller-inline core did not retain the sibling thread");
}

std::string run_core_cli(const std::string& cli, const std::string& core,
                         std::size_t inline_index, pid_t sibling) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create mdbg-core pipes");
  }
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for mdbg-core");
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
      "\nprint caller_direct_aggregate\n"
      "aggregate-member caller_direct_aggregate direct\n"
      "deref-aggregate-member caller_direct_aggregate linked\n"
      "member caller_aggregate_pointer direct\n"
      "deref-member caller_aggregate_pointer linked\n"
      "inline physical\n"
      "frame 1\ninline " + std::to_string(inline_index) +
      "\nthread " + std::to_string(sibling) + "\nquit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset, script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write mdbg-core commands");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read mdbg-core output");
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
          "mdbg-core aggregate workflow did not exit cleanly");
  return output;
}

bool supported_snapshot_storage(mdbg::LocalValueStorage storage) {
  return storage == mdbg::LocalValueStorage::SnapshotCoreMemory ||
         storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact;
}

mdbg::LocalStructMember member_named(const mdbg::LocalScalarValue& value,
                                     const std::string& name) {
  const auto it = std::find_if(value.members.begin(), value.members.end(),
                               [&name](const mdbg::LocalStructMember& member) {
                                 return member.name == name;
                               });
  if (it == value.members.end()) {
    throw std::runtime_error("selected-inline direct aggregate member is unavailable: " + name);
  }
  return *it;
}

void exercise(const std::string& fixture, const std::string& cli) {
  const auto generated = generate_core(fixture);
  try {
    mdbg::CoreInspectionSession session(generated.path);
    require(session.trace().frames.size() > 1,
            "caller-inline core did not retain historical physical frame 1");
    const auto sibling = sibling_tid(session, generated.crash_tid);
    session.select_frame(1);
    const auto inner = selected_inner_index(session);
    session.select_inline_context(inner);

    const auto pointer = session.inspect_value("caller_aggregate_pointer");
    require(pointer.kind == mdbg::LocalValueKind::Pointer && pointer.pointee_type &&
                pointer.pointee_type->kind == mdbg::LocalValueKind::Structure,
            "selected-inline aggregate pointer lost bounded structure metadata");
    require(pointer.pointee_type->members.size() == 2,
            "selected-inline aggregate pointer lost direct member metadata");

    std::optional<std::string> session_failure;
    try {
      const auto direct_aggregate = session.inspect_value("caller_direct_aggregate");
      require(direct_aggregate.kind == mdbg::LocalValueKind::Structure &&
                  direct_aggregate.byte_size == 16 && direct_aggregate.members.size() == 2,
              "selected-inline by-value aggregate was not materialized as the bounded structure");
      require(supported_snapshot_storage(direct_aggregate.storage),
              "selected-inline by-value aggregate lost immutable snapshot provenance");
      const auto direct_member = member_named(direct_aggregate, "direct");
      require(direct_member.kind == mdbg::LocalValueKind::Integer &&
                  direct_member.raw_value == UINT64_C(0x55667788) &&
                  direct_member.byte_size == sizeof(int) && direct_member.is_signed,
              "selected-inline by-value integer member was not materialized exactly");
      const auto linked_member = member_named(direct_aggregate, "linked");
      require(linked_member.kind == mdbg::LocalValueKind::Pointer &&
                  linked_member.raw_value != 0 && linked_member.byte_size == sizeof(void*) &&
                  linked_member.pointee_type &&
                  linked_member.pointee_type->byte_size == sizeof(int) &&
                  linked_member.pointee_type->is_signed,
              "selected-inline by-value pointer member lost bounded pointer metadata");

      const auto aggregate_direct =
          session.inspect_aggregate_member("caller_direct_aggregate", "direct");
      require(aggregate_direct.kind == mdbg::LocalValueKind::Integer &&
                  aggregate_direct.raw_value == UINT64_C(0x55667788) &&
                  aggregate_direct.byte_size == sizeof(int) && aggregate_direct.is_signed,
              "selected-inline direct aggregate integer-member selection was not exact");
      require(supported_snapshot_storage(aggregate_direct.storage),
              "selected-inline direct aggregate member lost immutable snapshot provenance");

      const auto aggregate_linked =
          session.inspect_aggregate_member("caller_direct_aggregate", "linked");
      require(aggregate_linked.kind == mdbg::LocalValueKind::Pointer &&
                  aggregate_linked.raw_value == linked_member.raw_value &&
                  aggregate_linked.byte_size == sizeof(void*) &&
                  aggregate_linked.pointee_type &&
                  aggregate_linked.pointee_type->kind == mdbg::LocalValueKind::Integer &&
                  aggregate_linked.pointee_type->byte_size == sizeof(int) &&
                  aggregate_linked.pointee_type->is_signed,
              "selected-inline direct aggregate pointer-member selection lost metadata");
      const auto aggregate_linked_value =
          session.dereference_aggregate_member("caller_direct_aggregate", "linked");
      require(aggregate_linked_value.kind == mdbg::LocalValueKind::Integer &&
                  aggregate_linked_value.raw_value == UINT64_C(0x02468ace) &&
                  aggregate_linked_value.byte_size == sizeof(int) &&
                  aggregate_linked_value.is_signed,
              "selected-inline direct aggregate pointer member did not dereference exactly once");
      require(supported_snapshot_storage(aggregate_linked_value.storage),
              "selected-inline direct aggregate pointee lost immutable snapshot provenance");

      const auto direct =
          session.inspect_pointer_member("caller_aggregate_pointer", "direct");
      require(direct.kind == mdbg::LocalValueKind::Integer &&
                  direct.raw_value == UINT64_C(0x11223344) &&
                  direct.byte_size == sizeof(int) && direct.is_signed,
              "selected-inline direct aggregate member was not materialized exactly");
      require(supported_snapshot_storage(direct.storage),
              "selected-inline direct member lost immutable snapshot provenance");

      const auto linked =
          session.inspect_pointer_member("caller_aggregate_pointer", "linked");
      require(linked.kind == mdbg::LocalValueKind::Pointer && linked.pointee_type &&
                  linked.pointee_type->kind == mdbg::LocalValueKind::Integer,
              "selected-inline pointer-valued member lost pointee metadata");
      const auto linked_value =
          session.dereference_pointer_member("caller_aggregate_pointer", "linked");
      require(linked_value.kind == mdbg::LocalValueKind::Integer &&
                  linked_value.raw_value == UINT64_C(0x13579bdf) &&
                  linked_value.byte_size == sizeof(int) && linked_value.is_signed,
              "selected-inline pointer-valued member did not dereference exactly once");
      require(supported_snapshot_storage(linked_value.storage),
              "selected-inline member pointee lost immutable snapshot provenance");

      session.select_frame(0);
      require(!session.selected_inline_context_index(),
              "frame selection did not invalidate selected-inline ownership");
      session.select_frame(1);
      session.select_inline_context(inner);
      session.clear_inline_context();
      require(!session.selected_inline_context_index(),
              "inline physical did not clear selected-inline ownership");
      session.select_inline_context(inner);
      session.select_thread(sibling);
      require(!session.selected_inline_context_index(),
              "thread selection did not invalidate selected-inline ownership");
    } catch (const std::exception& error) {
      session_failure = error.what();
    }

    const auto output = run_core_cli(cli, generated.path, inner, sibling);
    const bool cli_by_value =
        output.find("caller_direct_aggregate = { direct=0x55667788, linked=0x") !=
        std::string::npos;
    const bool cli_aggregate_direct =
        output.find("caller_direct_aggregate.direct = 0x55667788") != std::string::npos;
    const bool cli_aggregate_linked =
        output.find("*(caller_direct_aggregate.linked) = 0x2468ace") != std::string::npos;
    const bool cli_direct =
        output.find("caller_aggregate_pointer->direct = 0x11223344") != std::string::npos;
    const bool cli_linked =
        output.find("*(caller_aggregate_pointer->linked) = 0x13579bdf") != std::string::npos;
    const bool cli_thread =
        output.find("selected thread " + std::to_string(sibling)) != std::string::npos;

    if (session_failure) {
      throw std::runtime_error("CoreInspectionSession aggregate traversal failed: " +
                               *session_failure + "\nmdbg-core output:\n" + output);
    }
    require(cli_by_value,
            "mdbg-core did not render the selected-inline by-value aggregate");
    require(cli_aggregate_direct,
            "mdbg-core did not render selected-inline direct-aggregate integer member");
    require(cli_aggregate_linked,
            "mdbg-core did not dereference selected-inline direct-aggregate pointer member");
    require(cli_direct, "mdbg-core did not render selected-inline direct member");
    require(cli_linked, "mdbg-core did not dereference selected-inline pointer member");
    require(cli_thread, "mdbg-core did not complete thread-selection invalidation workflow");
  } catch (...) {
    std::remove(generated.path.c_str());
    throw;
  }
  std::remove(generated.path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_inline_aggregate_session_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    exercise(argv[1], argv[2]);
    std::cout << "selected-inline aggregate/member integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "selected-inline aggregate/member integration failure: " << error.what()
              << '\n';
    return 1;
  }
  return 0;
}

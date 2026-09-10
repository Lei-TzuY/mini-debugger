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
#include <thread>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct GeneratedCore {
  std::string path;
};

GeneratedCore generate_core(const std::string& fixture) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for caller-inline bit-field fixture");
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
          "caller-inline bit-field fixture did not terminate from deterministic SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce the caller-inline bit-field core");
  return GeneratedCore{core_path};
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

mdbg::LocalStructMember member_named(const mdbg::LocalScalarValue& value,
                                     const std::string& name) {
  const auto it = std::find_if(value.members.begin(), value.members.end(),
                               [&name](const mdbg::LocalStructMember& member) {
                                 return member.name == name;
                               });
  if (it == value.members.end()) {
    throw std::runtime_error("selected-inline bit-field member is unavailable: " + name);
  }
  return *it;
}

std::string run_core_cli(const std::string& cli, const std::string& core,
                         std::size_t inline_index) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create mdbg-core bit-field pipes");
  }
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for mdbg-core bit-field workflow");
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
      "\nprint caller_bit_fields\n"
      "aggregate-member caller_bit_fields signed_bits\n"
      "aggregate-member caller_bit_fields unsigned_bits\n"
      "quit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset, script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write mdbg-core bit-field commands");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read mdbg-core bit-field output");
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
          "mdbg-core bit-field workflow did not exit cleanly");
  return output;
}

void exercise(const std::string& fixture, const std::string& cli) {
  const auto generated = generate_core(fixture);
  try {
    mdbg::CoreInspectionSession session(generated.path);
    require(session.trace().frames.size() > 1,
            "caller-inline bit-field core did not retain historical physical frame 1");
    session.select_frame(1);
    const auto inner = selected_inner_index(session);
    session.select_inline_context(inner);

    const auto aggregate = session.inspect_value("caller_bit_fields");
    require(aggregate.kind == mdbg::LocalValueKind::Structure && aggregate.byte_size == 4 &&
                aggregate.members.size() == 2,
            "selected-inline bit-field aggregate was not materialized as four-byte structure");
    require(supported_snapshot_storage(aggregate.storage),
            "selected-inline bit-field aggregate lost immutable snapshot provenance");

    const auto signed_member = member_named(aggregate, "signed_bits");
    require(signed_member.kind == mdbg::LocalValueKind::Integer &&
                signed_member.byte_size == sizeof(int) && signed_member.is_signed &&
                signed_member.raw_value == UINT64_C(0xfffffff9),
            "five-bit signed member was not normalized to signed int32 -7");
    const auto unsigned_member = member_named(aggregate, "unsigned_bits");
    require(unsigned_member.kind == mdbg::LocalValueKind::Integer &&
                unsigned_member.byte_size == sizeof(unsigned int) && !unsigned_member.is_signed &&
                unsigned_member.raw_value == UINT64_C(0x29),
            "six-bit unsigned member was not normalized to uint32 41");

    const auto selected_signed =
        session.inspect_aggregate_member("caller_bit_fields", "signed_bits");
    require(selected_signed.raw_value == UINT64_C(0xfffffff9) && selected_signed.is_signed &&
                selected_signed.byte_size == sizeof(int) &&
                supported_snapshot_storage(selected_signed.storage),
            "aggregate-member did not preserve signed bit-field value/provenance");
    const auto selected_unsigned =
        session.inspect_aggregate_member("caller_bit_fields", "unsigned_bits");
    require(selected_unsigned.raw_value == UINT64_C(0x29) && !selected_unsigned.is_signed &&
                selected_unsigned.byte_size == sizeof(unsigned int) &&
                supported_snapshot_storage(selected_unsigned.storage),
            "aggregate-member did not preserve unsigned bit-field value/provenance");

    const auto output = run_core_cli(cli, generated.path, inner);
    require(output.find("caller_bit_fields = { signed_bits=0xfffffff9, unsigned_bits=0x29 }") !=
                std::string::npos,
            "mdbg-core did not render compiler-described bit-field members");
    require(output.find("caller_bit_fields.signed_bits = 0xfffffff9") != std::string::npos,
            "mdbg-core did not select signed bit-field member");
    require(output.find("caller_bit_fields.unsigned_bits = 0x29") != std::string::npos,
            "mdbg-core did not select unsigned bit-field member");
  } catch (...) {
    std::remove(generated.path.c_str());
    throw;
  }
  std::remove(generated.path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_inline_bitfield_session_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    exercise(argv[1], argv[2]);
    std::cout << "selected-inline bit-field integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "selected-inline bit-field integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

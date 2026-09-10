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

struct GeneratedCore {
  std::string path;
};

GeneratedCore generate_core(const std::string& fixture) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for caller-inline enum fixture");
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
          "caller-inline enum fixture did not terminate from deterministic SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce the caller-inline enum core");
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

void require_enumerator(const mdbg::LocalEnumType& type, const std::string& name,
                        std::uint64_t raw) {
  for (const auto& enumerator : type.enumerators) {
    if (enumerator.name == name && enumerator.raw_value == raw) return;
  }
  throw std::runtime_error("missing compiler enum entry: " + name);
}

std::string run_core_cli(const std::string& cli, const std::string& core,
                         std::size_t inline_index) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create mdbg-core enum pipes");
  }
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for mdbg-core enum workflow");
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
  const std::string script = "frame 1\ninline " + std::to_string(inline_index) +
                             "\nprint caller_mode\nquit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset,
                               script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write mdbg-core enum commands");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read mdbg-core enum output");
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
          "mdbg-core enum workflow did not exit cleanly");
  return output;
}

void exercise(const std::string& fixture, const std::string& cli) {
  const auto generated = generate_core(fixture);
  try {
    mdbg::CoreInspectionSession session(generated.path);
    require(session.trace().frames.size() > 1,
            "caller-inline enum core did not retain historical physical frame 1");
    session.select_frame(1);
    const auto inner = selected_inner_index(session);
    session.select_inline_context(inner);

    const auto value = session.inspect_value("caller_mode");
    require(value.kind == mdbg::LocalValueKind::Enumeration,
            "caller_mode did not preserve enum type identity");
    require(value.byte_size == 4 && !value.is_signed && value.raw_value == 42,
            "caller_mode did not preserve compiler-described uint32 value 42");
    require(supported_snapshot_storage(value.storage),
            "caller_mode lost immutable snapshot provenance");
    require(value.enum_type.has_value(), "caller_mode lost bounded enum metadata");
    require(value.enum_type->name == "CallerInlineMode" &&
                value.enum_type->byte_size == 4 && !value.enum_type->is_signed,
            "caller_mode enum identity/representation metadata is incorrect");
    require(value.enum_type->enumerators.size() == 3,
            "CallerInlineMode enumerator table is not bounded to compiler evidence");
    require_enumerator(*value.enum_type, "CallerInlineIdle", 3);
    require_enumerator(*value.enum_type, "CallerInlineReady", 7);
    require_enumerator(*value.enum_type, "CallerInlineBusy", 42);
    const auto symbol = mdbg::local_enum_symbol(value);
    require(symbol && *symbol == "CallerInlineBusy",
            "caller_mode raw value did not map to its exact symbolic enumerator");

    auto unknown = value;
    unknown.raw_value = 11;
    require(!mdbg::local_enum_symbol(unknown),
            "unknown enum raw value must remain numeric without an invented symbol");
    auto ambiguous = value;
    ambiguous.enum_type->enumerators.push_back({"CallerInlineBusyAlias", 42});
    require(!mdbg::local_enum_symbol(ambiguous),
            "duplicate enum raw values must not select an arbitrary symbolic name");

    const auto output = run_core_cli(cli, generated.path, inner);
    require(output.find("caller_mode = CallerInlineMode::CallerInlineBusy (0x2a)") !=
                std::string::npos,
            "mdbg-core did not render enum type, symbol, and numeric value together");
    require(output.find("[4-byte enum unsigned]") != std::string::npos,
            "mdbg-core did not render bounded enum representation metadata");
  } catch (...) {
    std::remove(generated.path.c_str());
    throw;
  }
  std::remove(generated.path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_inline_enum_session_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    exercise(argv[1], argv[2]);
    std::cout << "selected-inline enum integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "selected-inline enum integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

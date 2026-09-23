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

bool immutable_storage(mdbg::LocalValueStorage storage) {
  return storage == mdbg::LocalValueStorage::SnapshotCoreMemory ||
         storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact ||
         storage == mdbg::LocalValueStorage::SnapshotCoreRegister;
}

std::string generate_core(const std::string& fixture) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("failed to fork frame-zero enum aggregate fixture");
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
          "frame-zero enum aggregate fixture did not terminate with SIGSEGV");

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce frame-zero inline enum aggregate core");
  return core_path;
}

std::size_t inner_index(const mdbg::CoreInspectionSession& session) {
  const auto contexts = session.inline_contexts();
  for (std::size_t index = 0; index < contexts.size(); ++index) {
    if (contexts[index].name == "frame_zero_inline_enum_aggregate_inner")
      return index;
  }
  throw std::runtime_error("frame-zero inline enum aggregate context is unavailable");
}

std::string run_cli(const std::string& cli, const std::string& core,
                    std::size_t inline_index) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0)
    throw std::runtime_error("failed to create frame-zero enum aggregate CLI pipes");
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("failed to fork mdbg-core");
  if (child == 0) {
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]); ::close(input_pipe[1]);
    ::close(output_pipe[0]); ::close(output_pipe[1]);
    ::execl(cli.c_str(), cli.c_str(), core.c_str(), nullptr);
    _exit(127);
  }
  ::close(input_pipe[0]);
  ::close(output_pipe[1]);

  const std::string script =
      "inline " + std::to_string(inline_index) +
      "\nprint inline_enum_aggregate\n"
      "aggregate-member inline_enum_aggregate mode\n"
      "inline physical\nquit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset,
                               script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write frame-zero enum aggregate CLI");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read frame-zero enum aggregate CLI");
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
          "mdbg-core frame-zero enum aggregate workflow did not exit cleanly");
  return output;
}

void exercise(const std::string& fixture, const std::string& cli) {
  const auto core = generate_core(fixture);
  try {
    mdbg::CoreInspectionSession session(core);
    require(session.selected_frame_index() == 0,
            "frame-zero enum aggregate core did not start on physical frame zero");
    const auto index = inner_index(session);
    session.select_inline_context(index);

    const auto value = session.inspect_value("inline_enum_aggregate");
    require(value.kind == mdbg::LocalValueKind::Structure &&
                value.byte_size == 8 && value.members.size() == 2,
            "frame-zero enum aggregate lost bounded structure identity");
    require(immutable_storage(value.storage),
            "frame-zero enum aggregate lost physical snapshot provenance");

    require(value.members[0].name == "direct" &&
                value.members[0].kind == mdbg::LocalValueKind::Integer &&
                value.members[0].offset == 0 &&
                value.members[0].raw_value == UINT64_C(0x31415926),
            "frame-zero enum aggregate direct member changed");

    const auto& mode = value.members[1];
    require(mode.name == "mode" &&
                mode.kind == mdbg::LocalValueKind::Enumeration &&
                mode.offset == 4 && mode.raw_value == UINT64_C(42) &&
                mode.enum_type.has_value(),
            "frame-zero enum aggregate lost enum-valued member identity");
    require(mode.enum_type->name == "FrameZeroInlineMode" &&
                mode.enum_type->byte_size == 4 &&
                !mode.enum_type->is_signed &&
                mode.enum_type->enumerators.size() == 3,
            "frame-zero enum aggregate lost compiler enum metadata");

    const auto selected =
        session.inspect_aggregate_member("inline_enum_aggregate", "mode");
    require(selected.kind == mdbg::LocalValueKind::Enumeration &&
                selected.raw_value == UINT64_C(42) &&
                selected.enum_type.has_value() &&
                selected.storage == value.storage,
            "frame-zero enum member selection lost identity/provenance");
    const auto symbol = mdbg::local_enum_symbol(selected);
    require(symbol && *symbol == "FrameZeroInlineBusy",
            "frame-zero enum member did not recover its exact symbol");

    auto unknown = selected;
    unknown.raw_value = 11;
    require(!mdbg::local_enum_symbol(unknown),
            "unknown frame-zero enum raw value must remain numeric");
    auto ambiguous = selected;
    ambiguous.enum_type->enumerators.push_back({"FrameZeroInlineBusyAlias", 42});
    require(!mdbg::local_enum_symbol(ambiguous),
            "duplicate frame-zero enum aliases must remain ambiguous");

    const auto output = run_cli(cli, core, index);
    require(output.find("inline_enum_aggregate = { direct=0x31415926, mode=FrameZeroInlineMode::FrameZeroInlineBusy (0x2a) }") !=
                std::string::npos,
            "mdbg-core did not render the frame-zero enum aggregate");
    require(output.find("inline_enum_aggregate.mode = FrameZeroInlineMode::FrameZeroInlineBusy (0x2a)") !=
                std::string::npos,
            "mdbg-core did not render the selected frame-zero enum member");
    require(output.find("selected physical frame 0") != std::string::npos,
            "mdbg-core did not clear frame-zero enum aggregate ownership");

    session.select_frame(0);
    require(!session.selected_inline_context_index(),
            "physical frame selection did not invalidate frame-zero enum aggregate ownership");
  } catch (...) {
    std::remove(core.c_str());
    throw;
  }
  std::remove(core.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_frame_zero_inline_enum_aggregate_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    exercise(argv[1], argv[2]);
    std::cout << "frame-zero selected-inline enum aggregate integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "frame-zero selected-inline enum aggregate integration failure: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}

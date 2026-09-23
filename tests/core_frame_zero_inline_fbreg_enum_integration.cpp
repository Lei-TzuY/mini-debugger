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
  if (child == -1) {
    throw std::runtime_error("failed to fork frame-zero fbreg enum fixture");
  }
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
  require(waited == child && WIFSIGNALED(status) &&
              WTERMSIG(status) == SIGSEGV,
          "frame-zero fbreg enum fixture did not terminate with SIGSEGV");

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce frame-zero inline fbreg enum core");
  return core_path;
}

std::size_t inner_index(const mdbg::CoreInspectionSession& session) {
  const auto contexts = session.inline_contexts();
  for (std::size_t index = 0; index < contexts.size(); ++index) {
    if (contexts[index].name == "frame_zero_inline_fbreg_enum_inner") {
      return index;
    }
  }
  throw std::runtime_error(
      "frame-zero inline fbreg enum context is unavailable");
}

void require_enumerator(const mdbg::LocalEnumType& type,
                        const std::string& name, std::uint64_t raw) {
  for (const auto& enumerator : type.enumerators) {
    if (enumerator.name == name && enumerator.raw_value == raw) return;
  }
  throw std::runtime_error("missing frame-zero fbreg enum entry: " + name);
}

std::string run_cli(const std::string& cli, const std::string& core,
                    std::size_t inline_index) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create frame-zero fbreg enum CLI pipes");
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
      "\nprint inline_fbreg_mode\ninline physical\nquit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset,
                               script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) {
      throw std::runtime_error("failed to write frame-zero fbreg enum CLI");
    }
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) {
      throw std::runtime_error("failed to read frame-zero fbreg enum CLI");
    }
    if (count == 0) break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(output_pipe[0]);

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  require(waited == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "mdbg-core frame-zero fbreg enum workflow did not exit cleanly");
  return output;
}

void exercise(const std::string& fixture, const std::string& cli) {
  const auto core = generate_core(fixture);
  try {
    mdbg::CoreInspectionSession session(core);
    require(session.selected_frame_index() == 0,
            "frame-zero fbreg enum core did not start on physical frame zero");
    const auto index = inner_index(session);
    session.select_inline_context(index);

    const auto value = session.inspect_value("inline_fbreg_mode");
    require(value.kind == mdbg::LocalValueKind::Enumeration &&
                value.byte_size == sizeof(std::uint32_t) &&
                !value.is_signed && value.raw_value == UINT64_C(42),
            "frame-zero selected-inline fbreg enum lost value identity");
    require(value.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
            "frame-zero selected-inline fbreg enum lost stack-memory provenance");
    require(value.enum_type.has_value(),
            "frame-zero selected-inline fbreg enum lost type metadata");
    require(value.enum_type->name == "FrameZeroInlineFbregMode" &&
                value.enum_type->byte_size == sizeof(std::uint32_t) &&
                !value.enum_type->is_signed &&
                value.enum_type->enumerators.size() == 3,
            "frame-zero selected-inline fbreg enum metadata changed");
    require_enumerator(*value.enum_type, "FrameZeroInlineFbregIdle", 3);
    require_enumerator(*value.enum_type, "FrameZeroInlineFbregReady", 7);
    require_enumerator(*value.enum_type, "FrameZeroInlineFbregBusy", 42);
    const auto symbol = mdbg::local_enum_symbol(value);
    require(symbol && *symbol == "FrameZeroInlineFbregBusy",
            "frame-zero fbreg enum raw value lost exact symbolic identity");

    auto unknown = value;
    unknown.raw_value = 11;
    require(!mdbg::local_enum_symbol(unknown),
            "unknown frame-zero fbreg enum value invented a symbol");
    auto ambiguous = value;
    ambiguous.enum_type->enumerators.push_back(
        {"FrameZeroInlineFbregBusyAlias", 42});
    require(!mdbg::local_enum_symbol(ambiguous),
            "duplicate frame-zero fbreg enum alias selected an arbitrary symbol");

    const auto output = run_cli(cli, core, index);
    require(output.find(
                "inline_fbreg_mode = FrameZeroInlineFbregMode::"
                "FrameZeroInlineFbregBusy (0x2a)") != std::string::npos,
            "mdbg-core did not render frame-zero fbreg enum symbolically");
    require(output.find("[4-byte enum unsigned]") != std::string::npos,
            "mdbg-core did not render frame-zero fbreg enum representation");
    require(output.find("selected physical frame 0") != std::string::npos,
            "mdbg-core did not clear frame-zero fbreg enum ownership");

    session.select_frame(0);
    require(!session.selected_inline_context_index(),
            "physical frame selection did not invalidate frame-zero fbreg enum");
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
        << "usage: core_frame_zero_inline_fbreg_enum_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    exercise(argv[1], argv[2]);
    std::cout << "frame-zero selected-inline fbreg enum integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "frame-zero selected-inline fbreg enum integration failure: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}

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

constexpr std::uint64_t kFirst = UINT64_C(0x11223314);
constexpr std::uint64_t kSecond = UINT64_C(0x55667714);

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string generate_core(const std::string& fixture) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("failed to fork frame-zero structure fixture");
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
          "frame-zero structure fixture did not terminate with SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce frame-zero inline structure core");
  return core_path;
}

std::size_t inner_index(const mdbg::CoreInspectionSession& session) {
  const auto contexts = session.inline_contexts();
  for (std::size_t index = 0; index < contexts.size(); ++index) {
    if (contexts[index].name == "frame_zero_inline_structure_inner") return index;
  }
  throw std::runtime_error("frame-zero inline structure context is unavailable");
}

std::string run_cli(const std::string& cli, const std::string& core,
                    std::size_t inline_index) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create frame-zero structure CLI pipes");
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
      "\nprint inline_pair\naggregate-member inline_pair second\n"
      "inline physical\nquit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset,
                               script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write frame-zero structure CLI");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read frame-zero structure CLI");
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
          "mdbg-core frame-zero structure workflow did not exit cleanly");
  return output;
}

void exercise(const std::string& fixture, const std::string& cli) {
  const auto core = generate_core(fixture);
  try {
    mdbg::CoreInspectionSession session(core);
    require(session.selected_frame_index() == 0,
            "frame-zero structure core did not start on physical frame zero");
    const auto index = inner_index(session);
    session.select_inline_context(index);

    const auto value = session.inspect_value("inline_pair");
    require(value.kind == mdbg::LocalValueKind::Structure &&
                value.byte_size == 8 && value.members.size() == 2,
            "frame-zero selected-inline structure lost bounded type/materialization");
    require(value.storage == mdbg::LocalValueStorage::SnapshotCoreRegister,
            "frame-zero selected-inline structure lost register provenance");
    require(value.members[0].name == "first" &&
                value.members[0].raw_value == kFirst &&
                value.members[0].byte_size == sizeof(std::int32_t) &&
                value.members[0].is_signed,
            "frame-zero selected-inline first member is incorrect");
    require(value.members[1].name == "second" &&
                value.members[1].raw_value == kSecond &&
                value.members[1].byte_size == sizeof(std::int32_t) &&
                value.members[1].is_signed,
            "frame-zero selected-inline second member is incorrect");

    const auto second = session.inspect_aggregate_member("inline_pair", "second");
    require(second.kind == mdbg::LocalValueKind::Integer &&
                second.raw_value == kSecond &&
                second.byte_size == sizeof(std::int32_t) && second.is_signed &&
                second.storage == mdbg::LocalValueStorage::SnapshotCoreRegister,
            "frame-zero selected-inline member selection lost value/provenance");

    const auto output = run_cli(cli, core, index);
    require(output.find("inline_pair = { first=0x11223314, second=0x55667714 }") !=
                std::string::npos,
            "mdbg-core did not render the frame-zero selected-inline structure");
    require(output.find("inline_pair.second = 0x55667714") != std::string::npos,
            "mdbg-core did not render frame-zero selected-inline member selection");
    require(output.find("selected physical frame 0") != std::string::npos,
            "mdbg-core did not clear frame-zero inline ownership");

    session.select_frame(0);
    require(!session.selected_inline_context_index(),
            "physical frame selection did not invalidate frame-zero inline structure");
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
        << "usage: core_frame_zero_inline_structure_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    exercise(argv[1], argv[2]);
    std::cout << "frame-zero selected-inline structure integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "frame-zero selected-inline structure integration failure: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}

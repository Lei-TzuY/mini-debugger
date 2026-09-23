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

constexpr std::uint64_t kFirst = UINT64_C(0x10203014);
constexpr std::uint64_t kSecond = UINT64_C(0x40506014);

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string generate_core(const std::string& fixture) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("failed to fork frame-zero fbreg array fixture");
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
          "frame-zero fbreg array fixture did not terminate with SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce frame-zero inline fbreg array core");
  return core_path;
}

std::size_t inner_index(const mdbg::CoreInspectionSession& session) {
  const auto contexts = session.inline_contexts();
  for (std::size_t index = 0; index < contexts.size(); ++index) {
    if (contexts[index].name == "frame_zero_inline_fbreg_array_inner") return index;
  }
  throw std::runtime_error("frame-zero inline fbreg array context is unavailable");
}

std::string run_cli(const std::string& cli, const std::string& core,
                    std::size_t inline_index) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create frame-zero fbreg array CLI pipes");
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
      "\nprint inline_fbreg_array\narray-element inline_fbreg_array 1\n"
      "array-element inline_fbreg_array 2\ninline physical\nquit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset,
                               script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write frame-zero fbreg array CLI");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read frame-zero fbreg array CLI");
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
          "mdbg-core frame-zero fbreg array workflow did not exit cleanly");
  return output;
}

void exercise(const std::string& fixture, const std::string& cli) {
  const auto core = generate_core(fixture);
  try {
    mdbg::CoreInspectionSession session(core);
    require(session.selected_frame_index() == 0,
            "frame-zero fbreg array core did not start on physical frame zero");
    const auto index = inner_index(session);
    session.select_inline_context(index);

    const auto value = session.inspect_value("inline_fbreg_array");
    require(value.kind == mdbg::LocalValueKind::Array &&
                value.byte_size == 8 && value.elements.size() == 2 &&
                value.array_type.has_value(),
            "frame-zero selected-inline fbreg array lost bounded type/materialization");
    require(value.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
            "frame-zero selected-inline fbreg array lost stack-memory provenance");
    require(value.array_type->element_count == 2 &&
                value.array_type->element_byte_size == sizeof(std::int32_t) &&
                value.array_type->element_is_signed &&
                value.array_type->element_kind == mdbg::LocalValueKind::Integer,
            "frame-zero selected-inline fbreg array lost canonical element metadata");
    require(value.elements[0].raw_value == kFirst &&
                value.elements[1].raw_value == kSecond,
            "frame-zero selected-inline fbreg array element values are incorrect");

    const auto second = session.inspect_array_element("inline_fbreg_array", 1);
    require(second.name == "inline_fbreg_array[1]" &&
                second.kind == mdbg::LocalValueKind::Integer &&
                second.raw_value == kSecond &&
                second.byte_size == sizeof(std::int32_t) &&
                second.is_signed &&
                second.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
            "frame-zero selected-inline fbreg array indexing lost value/provenance");
    bool out_of_range = false;
    try {
      (void)session.inspect_array_element("inline_fbreg_array", 2);
    } catch (const std::out_of_range&) {
      out_of_range = true;
    }
    require(out_of_range, "frame-zero selected-inline fbreg array accepted index 2");

    const auto output = run_cli(cli, core, index);
    require(output.find(
                "inline_fbreg_array = [0x10203014, 0x40506014]") != std::string::npos,
            "mdbg-core did not render the frame-zero selected-inline fbreg array");
    require(output.find("inline_fbreg_array[1] = 0x40506014") != std::string::npos,
            "mdbg-core did not render frame-zero selected-inline fbreg array indexing");
    require(output.find("array index is out of range") != std::string::npos,
            "mdbg-core did not reject the frame-zero selected-inline fbreg array index 2");
    require(output.find("selected physical frame 0") != std::string::npos,
            "mdbg-core did not clear frame-zero inline fbreg array ownership");

    session.select_frame(0);
    require(!session.selected_inline_context_index(),
            "physical frame selection did not invalidate frame-zero inline fbreg array");
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
        << "usage: core_frame_zero_inline_fbreg_array_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    exercise(argv[1], argv[2]);
    std::cout << "frame-zero selected-inline fbreg array integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "frame-zero selected-inline fbreg array integration failure: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}

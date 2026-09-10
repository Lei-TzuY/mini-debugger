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
  if (child == -1) throw std::runtime_error("fork failed for caller-inline array fixture");
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
          "caller-inline array fixture did not terminate from deterministic SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce the caller-inline array core");
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

std::string run_core_cli(const std::string& cli, const std::string& core,
                         std::size_t inline_index) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create mdbg-core array pipes");
  }

  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for mdbg-core array workflow");
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
      "\nprint caller_fixed_array\n"
      "array-element caller_fixed_array 1\n"
      "array-element caller_fixed_array 3\n"
      "quit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset, script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write mdbg-core array commands");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read mdbg-core array output");
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
          "mdbg-core array workflow did not exit cleanly");
  return output;
}

void exercise(const std::string& fixture, const std::string& cli) {
  const auto generated = generate_core(fixture);
  try {
    mdbg::CoreInspectionSession session(generated.path);
    require(session.trace().frames.size() > 1,
            "caller-inline array core did not retain historical physical frame 1");
    session.select_frame(1);
    const auto inner = selected_inner_index(session);
    session.select_inline_context(inner);

    const auto array = session.inspect_value("caller_fixed_array");
    require(array.kind == mdbg::LocalValueKind::Array,
            "selected-inline fixed array was not materialized as an array");
    require(array.byte_size == 3 * sizeof(int) && array.array_type.has_value(),
            "selected-inline fixed array lost bounded type metadata");
    require(array.array_type->element_count == 3 &&
                array.array_type->element_byte_size == sizeof(int) &&
                array.array_type->element_is_signed &&
                array.array_type->element_kind == mdbg::LocalValueKind::Integer,
            "selected-inline fixed array metadata does not match compiler evidence");
    require(array.elements.size() == 3 &&
                array.elements[0].raw_value == UINT64_C(0x10203040) &&
                array.elements[1].raw_value == UINT64_C(0x22334455) &&
                array.elements[2].raw_value == UINT64_C(0x33445566),
            "selected-inline fixed array elements were not materialized exactly");
    require(supported_snapshot_storage(array.storage),
            "selected-inline fixed array lost immutable snapshot provenance");

    const auto middle = session.inspect_array_element("caller_fixed_array", 1);
    require(middle.kind == mdbg::LocalValueKind::Integer &&
                middle.raw_value == UINT64_C(0x22334455) &&
                middle.byte_size == sizeof(int) && middle.is_signed,
            "selected-inline fixed-array index 1 was not materialized exactly");
    require(supported_snapshot_storage(middle.storage),
            "selected-inline fixed-array element lost immutable snapshot provenance");

    bool rejected = false;
    try {
      (void)session.inspect_array_element("caller_fixed_array", 3);
    } catch (const std::out_of_range&) {
      rejected = true;
    }
    require(rejected, "selected-inline fixed-array out-of-range index was accepted");

    const auto output = run_core_cli(cli, generated.path, inner);
    require(output.find("caller_fixed_array = [0x10203040, 0x22334455, 0x33445566]") !=
                std::string::npos,
            "mdbg-core did not render the selected-inline fixed array");
    require(output.find("caller_fixed_array[1] = 0x22334455") != std::string::npos,
            "mdbg-core did not render selected-inline fixed-array index 1");
    require(output.find("array index is out of range") != std::string::npos,
            "mdbg-core did not reject the selected-inline fixed-array out-of-range index");
  } catch (...) {
    std::remove(generated.path.c_str());
    throw;
  }
  std::remove(generated.path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_inline_array_session_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    exercise(argv[1], argv[2]);
    std::cout << "selected-inline fixed-array integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "selected-inline fixed-array integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

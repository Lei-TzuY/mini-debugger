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

constexpr std::uint64_t kSigned = UINT64_C(0xfffffff9);
constexpr std::uint64_t kUnsigned = UINT64_C(0x29);

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string generate_core(const std::string& fixture) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("failed to fork frame-zero bit-field fixture");
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
          "frame-zero bit-field fixture did not terminate with SIGSEGV");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce frame-zero inline bit-field core");
  return core_path;
}

std::size_t inner_index(const mdbg::CoreInspectionSession& session) {
  const auto contexts = session.inline_contexts();
  for (std::size_t index = 0; index < contexts.size(); ++index) {
    if (contexts[index].name == "frame_zero_inline_bitfield_inner") return index;
  }
  throw std::runtime_error("frame-zero inline bit-field context is unavailable");
}

std::string run_cli(const std::string& cli, const std::string& core,
                    std::size_t inline_index) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create frame-zero bit-field CLI pipes");
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
      "\nprint inline_bit_fields\n"
      "aggregate-member inline_bit_fields signed_bits\n"
      "aggregate-member inline_bit_fields unsigned_bits\n"
      "inline physical\nquit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset,
                               script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write frame-zero bit-field CLI");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);
  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read frame-zero bit-field CLI");
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
          "mdbg-core frame-zero bit-field workflow did not exit cleanly");
  return output;
}

void exercise(const std::string& fixture, const std::string& cli) {
  const auto core = generate_core(fixture);
  try {
    mdbg::CoreInspectionSession session(core);
    require(session.selected_frame_index() == 0,
            "frame-zero bit-field core did not start on physical frame zero");
    const auto index = inner_index(session);
    session.select_inline_context(index);

    const auto value = session.inspect_value("inline_bit_fields");
    require(value.kind == mdbg::LocalValueKind::Structure &&
                value.byte_size == 4 && value.members.size() == 2,
            "frame-zero selected-inline bit fields lost bounded structure identity");
    require(value.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
            "frame-zero selected-inline bit fields lost stack/core provenance");
    require(value.members[0].name == "signed_bits" &&
                value.members[0].raw_value == kSigned &&
                value.members[0].is_signed &&
                value.members[0].bit_slice.has_value() &&
                value.members[0].bit_slice->bit_size == 5,
            "frame-zero selected-inline signed bit field is incorrect");
    require(value.members[1].name == "unsigned_bits" &&
                value.members[1].raw_value == kUnsigned &&
                !value.members[1].is_signed &&
                value.members[1].bit_slice.has_value() &&
                value.members[1].bit_slice->bit_size == 6,
            "frame-zero selected-inline unsigned bit field is incorrect");

    const auto signed_view =
        session.inspect_aggregate_member("inline_bit_fields", "signed_bits");
    const auto unsigned_view =
        session.inspect_aggregate_member("inline_bit_fields", "unsigned_bits");
    require(signed_view.raw_value == kSigned && signed_view.is_signed &&
                signed_view.storage == value.storage,
            "frame-zero signed bit-field selection lost value/provenance");
    require(unsigned_view.raw_value == kUnsigned && !unsigned_view.is_signed &&
                unsigned_view.storage == value.storage,
            "frame-zero unsigned bit-field selection lost value/provenance");

    const auto output = run_cli(cli, core, index);
    require(output.find(
                "inline_bit_fields = { signed_bits=0xfffffff9, unsigned_bits=0x29 }") !=
                std::string::npos,
            "mdbg-core did not render frame-zero inline bit fields");
    require(output.find("inline_bit_fields.signed_bits = 0xfffffff9") !=
                std::string::npos,
            "mdbg-core did not render frame-zero signed bit field");
    require(output.find("inline_bit_fields.unsigned_bits = 0x29") !=
                std::string::npos,
            "mdbg-core did not render frame-zero unsigned bit field");
    require(output.find("selected physical frame 0") != std::string::npos,
            "mdbg-core did not clear frame-zero bit-field ownership");

    session.select_frame(0);
    require(!session.selected_inline_context_index(),
            "physical frame selection did not invalidate frame-zero bit fields");
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
        << "usage: core_frame_zero_inline_bitfield_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    exercise(argv[1], argv[2]);
    std::cout << "frame-zero selected-inline bit-field integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "frame-zero selected-inline bit-field integration failure: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}

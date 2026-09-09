#include "snapshot/session.hpp"

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr std::uint64_t kExpectedValue = 0x6a09e667f3bcc909ULL;
constexpr std::uint64_t kExpectedFirst = 0xbb67ae8584caa73bULL;
constexpr std::uint64_t kExpectedSecond = 0x3c6ef372fe94f82bULL;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string generate_core(const std::string& fixture, const char* coredump_filter) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for snapshot local fixture");
  if (child == 0) {
    rlimit core_limit{};
    if (::getrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(120);
    core_limit.rlim_cur = core_limit.rlim_max;
    if (::setrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(121);

    std::ofstream filter("/proc/self/coredump_filter", std::ios::trunc);
    if (!filter) _exit(122);
    filter << coredump_filter << '\n';
    filter.close();
    if (!filter) _exit(123);

    ::execl(fixture.c_str(), fixture.c_str(), "--snapshot-crash", nullptr);
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
          "snapshot local fixture did not terminate from SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(core_path) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path), "kernel did not produce the snapshot core");
  return core_path;
}

void require_aggregate_value(const mdbg::LocalScalarValue& aggregate,
                             const std::string& context) {
  require(aggregate.kind == mdbg::LocalValueKind::Structure && aggregate.byte_size == 16,
          context + " aggregate type/size was not reconstructed");
  require(aggregate.members.size() == 2,
          context + " aggregate did not expose both members");
  require(aggregate.members[0].name == "first" &&
              aggregate.members[0].raw_value == kExpectedFirst &&
              aggregate.members[0].byte_size == 8 &&
              aggregate.members[1].name == "second" &&
              aggregate.members[1].raw_value == kExpectedSecond &&
              aggregate.members[1].byte_size == 8,
          context + " aggregate member values were not reconstructed");
}

void test_artifact_backed_local(const std::string& fixture) {
  const auto core_path = generate_core(fixture, "0x1");
  try {
    mdbg::CoreInspectionSession session(core_path);
    require(session.trace().frames.size() > 1,
            "restricted snapshot did not recover the caller frame");
    session.select_frame(1);

    const auto value = session.inspect_value("snapshot_file_scalar");
    require(value.kind == mdbg::LocalValueKind::Integer && value.byte_size == 8 &&
                value.raw_value == kExpectedValue,
            "artifact-backed snapshot local value was not reconstructed");
    require(value.storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact,
            "restricted snapshot local did not report runtime-artifact provenance");
    require(value.storage_module_path == fixture && !value.storage_file_path.empty(),
            "snapshot local artifact provenance lost module/file ownership");

    const auto aggregate = session.inspect_value("snapshot_file_aggregate");
    require_aggregate_value(aggregate, "artifact-backed snapshot");
    require(aggregate.storage == mdbg::LocalValueStorage::SnapshotRuntimeArtifact,
            "restricted snapshot aggregate did not report runtime-artifact provenance");
    require(aggregate.storage_module_path == fixture && !aggregate.storage_file_path.empty(),
            "snapshot aggregate artifact provenance lost module/file ownership");
  } catch (...) {
    std::remove(core_path.c_str());
    throw;
  }
  std::remove(core_path.c_str());
}

void test_captured_core_precedence(const std::string& fixture) {
  const auto core_path = generate_core(fixture, "0x5");
  try {
    mdbg::CoreInspectionSession session(core_path);
    require(session.trace().frames.size() > 1,
            "captured snapshot did not recover the caller frame");
    session.select_frame(1);
    const auto aggregate = session.inspect_value("snapshot_file_aggregate");
    require_aggregate_value(aggregate, "captured snapshot");
    require(aggregate.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
            "captured file-backed aggregate did not prefer immutable core evidence");
    require(aggregate.storage_module_path.empty() && aggregate.storage_file_path.empty() &&
                aggregate.storage_file_offset == 0,
            "core-backed aggregate incorrectly retained runtime-artifact provenance");
  } catch (...) {
    std::remove(core_path.c_str());
    throw;
  }
  std::remove(core_path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: snapshot_file_backed_local_value_integration <fixture>\n";
    return 2;
  }
  try {
    test_artifact_backed_local(argv[1]);
    test_captured_core_precedence(argv[1]);
    std::cout << "snapshot provenance local value integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "snapshot provenance local value failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

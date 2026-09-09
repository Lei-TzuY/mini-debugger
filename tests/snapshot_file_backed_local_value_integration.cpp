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

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string generate_restricted_core(const std::string& fixture) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for snapshot local fixture");
  if (child == 0) {
    rlimit core_limit{};
    if (::getrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(120);
    core_limit.rlim_cur = core_limit.rlim_max;
    if (::setrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(121);

    std::ofstream filter("/proc/self/coredump_filter", std::ios::trunc);
    if (!filter) _exit(122);
    filter << "0x1\n";
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
  require(std::filesystem::exists(core_path),
          "kernel did not produce the restricted snapshot core");
  return core_path;
}

void test_file_backed_local(const std::string& fixture) {
  const auto core_path = generate_restricted_core(fixture);
  try {
    mdbg::CoreInspectionSession session(core_path);
    require(session.trace().frames.size() > 1,
            "restricted snapshot did not recover the caller frame");
    session.select_frame(1);
    const auto value = session.inspect_value("snapshot_file_scalar");
    require(value.kind == mdbg::LocalValueKind::Integer && value.byte_size == 8 &&
                value.raw_value == kExpectedValue,
            "artifact-backed snapshot local value was not reconstructed");
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
    test_file_backed_local(argv[1]);
    std::cout << "snapshot file-backed local value integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "snapshot file-backed local value failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

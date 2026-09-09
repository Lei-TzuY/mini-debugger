#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kExpectedCaller = "inspect_entry_parameter";
constexpr const char* kExpectedValue = "0x458a30bf63ac1619";
constexpr const char* kExpectedSourceContext =
    "const uint64_t side_effect = clobber_argument_registers(1, 2, 3, 4, 5, 6);";

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string temp_path() {
  char pattern[] = "/tmp/mdbg-core-thread-ready-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd == -1) throw std::runtime_error("mkstemp failed for core thread fixture");
  ::close(fd);
  ::unlink(pattern);
  return pattern;
}

std::string temp_directory() {
  char pattern[] = "/tmp/mdbg-core-debuglink-XXXXXX";
  char* path = ::mkdtemp(pattern);
  if (path == nullptr) throw std::runtime_error("mkdtemp failed for debuglink fixture");
  return path;
}

std::string temp_source_directory() {
  char pattern[] = "/tmp/mdbg-core-source-XXXXXX";
  char* path = ::mkdtemp(pattern);
  if (path == nullptr) throw std::runtime_error("mkdtemp failed for relocated source fixture");
  return path;
}

std::filesystem::path formal_parameter_source_path() {
  auto integration_path = std::filesystem::path(__FILE__);
  if (!integration_path.is_absolute()) {
    const auto from_source_parent =
        (std::filesystem::current_path().parent_path() / integration_path).lexically_normal();
    if (std::filesystem::is_regular_file(from_source_parent)) {
      integration_path = from_source_parent;
    } else {
      integration_path = std::filesystem::absolute(integration_path).lexically_normal();
    }
  }
  const auto source =
      (integration_path.parent_path() / "fixtures" / "formal_parameter_fixture.c")
          .lexically_normal();
  require(std::filesystem::is_regular_file(source),
          "could not locate formal-parameter fixture source for relocation");
  return source;
}

class RelocatedSourceTree {
 public:
  explicit RelocatedSourceTree(std::filesystem::path source)
      : source_(std::move(source)), relocation_root_(temp_source_directory()),
        relocated_(std::filesystem::path(relocation_root_) / source_.filename()),
        hidden_(source_.parent_path() /
                (source_.filename().string() + ".mdbg-hidden-" + std::to_string(::getpid()))) {
    std::filesystem::copy_file(source_, relocated_,
                               std::filesystem::copy_options::overwrite_existing);
    try {
      std::filesystem::rename(source_, hidden_);
      hidden_active_ = true;
    } catch (...) {
      std::error_code error;
      std::filesystem::remove_all(relocation_root_, error);
      throw;
    }
  }

  RelocatedSourceTree(const RelocatedSourceTree&) = delete;
  RelocatedSourceTree& operator=(const RelocatedSourceTree&) = delete;

  ~RelocatedSourceTree() {
    std::error_code error;
    if (hidden_active_ && std::filesystem::exists(hidden_)) {
      std::filesystem::rename(hidden_, source_, error);
    }
    error.clear();
    std::filesystem::remove_all(relocation_root_, error);
  }

  [[nodiscard]] std::string recorded_prefix() const {
    return source_.parent_path().string();
  }

  [[nodiscard]] const std::string& local_prefix() const { return relocation_root_; }

 private:
  std::filesystem::path source_;
  std::string relocation_root_;
  std::filesystem::path relocated_;
  std::filesystem::path hidden_;
  bool hidden_active_{false};
};

void run_command(const std::vector<std::string>& arguments) {
  require(!arguments.empty(), "tool command must not be empty");
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for tool command");
  if (child == 0) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(argv.front(), argv.data());
    _exit(127);
  }

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "tool command failed: " + arguments.front());
}

std::vector<std::byte> read_file_bytes(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open core file: " + path);
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0) throw std::runtime_error("failed to determine core file size");
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(length));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input) throw std::runtime_error("failed to read core file");
  }
  return bytes;
}

std::string write_variant(const std::vector<std::byte>& bytes) {
  char pattern[] = "/tmp/mdbg-core-session-variant-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd == -1) throw std::runtime_error("mkstemp failed for core-session variant");
  ::close(fd);
  std::ofstream output(pattern, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("failed to write core-session variant");
  return pattern;
}

std::vector<std::byte> replace_all_ascii(std::vector<std::byte> bytes,
                                         const std::string& from,
                                         const std::string& to) {
  require(!from.empty() && from.size() == to.size(),
          "core module replacement must preserve non-empty width");
  std::size_t replacements = 0;
  for (std::size_t offset = 0; offset + from.size() <= bytes.size(); ++offset) {
    bool match = true;
    for (std::size_t index = 0; index < from.size(); ++index) {
      if (std::to_integer<unsigned char>(bytes[offset + index]) !=
          static_cast<unsigned char>(from[index])) {
        match = false;
        break;
      }
    }
    if (!match) continue;
    for (std::size_t index = 0; index < to.size(); ++index) {
      bytes[offset + index] = static_cast<std::byte>(static_cast<unsigned char>(to[index]));
    }
    ++replacements;
    offset += from.size() - 1;
  }
  require(replacements != 0, "real core did not contain the fixture module path");
  return bytes;
}

std::string unavailable_peer_path(const std::string& path) {
  auto candidate = path;
  for (std::size_t offset = candidate.size(); offset > 0; --offset) {
    const auto index = offset - 1;
    if (candidate[index] == '/') continue;
    const char original = candidate[index];
    candidate[index] = original == 'x' ? 'y' : 'x';
    if (!std::filesystem::exists(candidate)) return candidate;
    candidate[index] = original;
  }
  throw std::runtime_error("could not derive unavailable same-width module path");
}

struct GeneratedCore {
  std::string path;
  pid_t crash_tid;
  pid_t sibling_tid;
};

GeneratedCore generate_core(const std::string& fixture, bool omit_file_backed = false) {
  const auto ready_path = temp_path();
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for core-session fixture");
  if (child == 0) {
    rlimit core_limit{};
    if (::getrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(120);
    core_limit.rlim_cur = core_limit.rlim_max;
    if (::setrlimit(RLIMIT_CORE, &core_limit) != 0) _exit(121);
    if (omit_file_backed) {
      std::ofstream filter("/proc/self/coredump_filter", std::ios::trunc);
      if (!filter) _exit(122);
      filter << "0x1\n";
      filter.close();
      if (!filter) _exit(123);
    }
    ::execl(fixture.c_str(), fixture.c_str(), "--snapshot-crash-threaded",
            ready_path.c_str(), nullptr);
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
          "core-session fixture did not terminate from deterministic SIGSEGV");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((!std::filesystem::exists(core_path) || !std::filesystem::exists(ready_path)) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(std::filesystem::exists(core_path),
          "kernel did not produce the core-session snapshot");
  require(std::filesystem::exists(ready_path),
          "threaded core fixture did not publish its sibling TID");

  std::ifstream ready(ready_path);
  long sibling = -1;
  ready >> sibling;
  std::remove(ready_path.c_str());
  require(ready && sibling > 0 && sibling != child,
          "threaded core fixture published an invalid sibling TID");
  return GeneratedCore{core_path, child, static_cast<pid_t>(sibling)};
}

struct DebugArtifacts {
  std::string directory;
  std::string runtime_module;
  std::string debug_file;
  std::string bad_debug_file;
};

DebugArtifacts make_debuglink_artifacts(const std::string& fixture) {
  DebugArtifacts result;
  result.directory = temp_directory();
  result.runtime_module = result.directory + "/snapshot-runtime";
  result.debug_file = result.directory + "/snapshot-runtime.debug";
  const auto bad_directory = result.directory + "/bad";
  result.bad_debug_file = bad_directory + "/snapshot-runtime.debug";

  std::filesystem::copy_file(fixture, result.runtime_module,
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::permissions(result.runtime_module,
                               std::filesystem::status(fixture).permissions());
  run_command({"objcopy", "--only-keep-debug", fixture, result.debug_file});
  run_command({"objcopy", "--strip-debug", result.runtime_module});
  run_command({"objcopy", "--add-gnu-debuglink=" + result.debug_file,
               result.runtime_module});

  std::filesystem::create_directories(bad_directory);
  std::filesystem::copy_file(result.debug_file, result.bad_debug_file,
                             std::filesystem::copy_options::overwrite_existing);
  std::ofstream corrupt(result.bad_debug_file, std::ios::binary | std::ios::app);
  const char marker = '\x7f';
  corrupt.write(&marker, 1);
  if (!corrupt) throw std::runtime_error("failed to corrupt debug companion");
  return result;
}

std::string run_core_cli(const std::string& cli, const GeneratedCore& core,
                         const std::string& recorded_module = {},
                         const std::string& local_module = {},
                         const std::string& debug_module = {},
                         const std::string& debug_file = {},
                         const std::string& recorded_source = {},
                         const std::string& local_source = {}) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create core-session CLI pipes");
  }

  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for core-session CLI");
  if (child == 0) {
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);

    std::vector<std::string> arguments{cli};
    if (!recorded_module.empty()) {
      arguments.push_back("--substitute-module-path");
      arguments.push_back(recorded_module);
      arguments.push_back(local_module);
    }
    if (!debug_module.empty()) {
      arguments.push_back("--debug-file");
      arguments.push_back(debug_module);
      arguments.push_back(debug_file);
    }
    if (!recorded_source.empty()) {
      arguments.push_back("--substitute-source-path");
      arguments.push_back(recorded_source);
      arguments.push_back(local_source);
    }
    arguments.push_back(core.path);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(cli.c_str(), argv.data());
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  const std::string script =
      "threads\n"
      "thread " + std::to_string(core.sibling_tid) + "\n"
      "bt\n"
      "thread " + std::to_string(core.crash_tid) + "\n"
      "bt\n"
      "frame 1\n"
      "list\n"
      "print transformed\n"
      "continue\n"
      "thread 999999999\n"
      "frame 999\n"
      "quit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset, script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write core-session commands");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read core-session output");
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
          "mdbg-core did not exit cleanly; output: " + output);
  return output;
}

std::uintptr_t first_frame_address(const std::string& output) {
  const auto marker = output.find("#0 0x");
  if (marker == std::string::npos) {
    throw std::runtime_error("core CLI did not print the initial frame address");
  }
  const auto begin = marker + 5;
  std::size_t consumed = 0;
  const auto value = std::stoull(output.substr(begin), &consumed, 16);
  if (consumed == 0) throw std::runtime_error("core CLI printed an invalid frame address");
  return static_cast<std::uintptr_t>(value);
}

std::string run_omitted_memory_cli(const std::string& cli, const GeneratedCore& core) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create omitted-memory CLI pipes");
  }
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("fork failed for omitted-memory CLI");
  if (child == 0) {
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    ::execl(cli.c_str(), cli.c_str(), core.path.c_str(), nullptr);
    _exit(127);
  }
  ::close(input_pipe[0]);
  ::close(output_pipe[1]);

  std::string output;
  while (output.find("core> ") == std::string::npos) {
    char buffer[1024];
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("mdbg-core exited before initial prompt");
    output.append(buffer, static_cast<std::size_t>(count));
  }
  const auto address = first_frame_address(output);
  const std::string script = "x " + std::to_string(address) + " 1\nquit\n";
  std::size_t offset = 0;
  while (offset < script.size()) {
    const auto count = ::write(input_pipe[1], script.data() + offset, script.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write omitted-memory command");
    offset += static_cast<std::size_t>(count);
  }
  ::close(input_pipe[1]);

  for (;;) {
    char buffer[1024];
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("failed to read omitted-memory CLI output");
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
          "mdbg-core omitted-memory session did not exit cleanly");
  return output;
}

void require_core_session_output(const std::string& output,
                                 const GeneratedCore& core,
                                 const std::string& recorded_module) {
  require(output.find("core signal 11 tid " + std::to_string(core.crash_tid)) !=
              std::string::npos,
          "core session did not report immutable crash identity");
  require(output.find("* tid " + std::to_string(core.crash_tid) + " crash") !=
              std::string::npos,
          "core thread catalogue did not mark the selected crash thread");
  require(output.find("tid " + std::to_string(core.sibling_tid)) != std::string::npos,
          "core thread catalogue lost the real sibling thread");
  require(output.find("selected thread " + std::to_string(core.sibling_tid)) !=
              std::string::npos,
          "core session did not select the immutable sibling thread");
  require(output.find("selected thread " + std::to_string(core.crash_tid)) !=
              std::string::npos,
          "core session did not restore the immutable crash thread");
  require(output.find("#0 0x") != std::string::npos &&
              output.find("#1 0x") != std::string::npos,
          "core session backtrace did not expose crash and caller frames");
  require(output.find(recorded_module + "!" + kExpectedCaller) != std::string::npos,
          "core session caller frame lost recorded module-qualified ownership");
  require(output.find("selected frame 1") != std::string::npos,
          "core session did not select the recovered caller frame");
  require(output.find("unsupported in core session: list") == std::string::npos,
          "core session still lacks immutable source-context listing");
  require(output.find(kExpectedSourceContext) != std::string::npos,
          "core session did not render real caller source context");
  require(output.find(recorded_module + "!transformed = " + std::string(kExpectedValue)) !=
              std::string::npos,
          "core session did not evaluate the historical caller source value");
  require(output.find("unsupported in core session: continue") != std::string::npos,
          "core session exposed a live execution command");
  require(output.find("error: core thread TID is unavailable") != std::string::npos,
          "core session did not reject an unavailable immutable thread");
  require(output.find("error: core frame index is out of range") != std::string::npos,
          "core session did not reject invalid immutable frame selection");
}

void test_core_session(const std::string& fixture, const std::string& cli) {
  const auto core = generate_core(fixture);
  std::string relocated_path;
  try {
    const auto output = run_core_cli(cli, core);
    require_core_session_output(output, core, fixture);

    {
      RelocatedSourceTree relocated_source(formal_parameter_source_path());
      const auto unavailable_source_output = run_core_cli(cli, core);
      require(unavailable_source_output.find("source unavailable:") != std::string::npos,
              "relocated source tree unexpectedly remained readable without mapping");
      require(unavailable_source_output.find(kExpectedSourceContext) == std::string::npos,
              "relocated source text leaked through the recorded source path");
      require(unavailable_source_output.find(fixture + "!" + kExpectedCaller) !=
                  std::string::npos,
              "source relocation changed immutable module/frame ownership");

      const auto mapped_source_output =
          run_core_cli(cli, core, {}, {}, {}, {}, relocated_source.recorded_prefix(),
                       relocated_source.local_prefix());
      require(mapped_source_output.find(kExpectedSourceContext) != std::string::npos,
              "explicit core source-path substitution did not recover relocated source text");
      require(mapped_source_output.find(fixture + "!" + kExpectedCaller) !=
                  std::string::npos,
              "source-path substitution changed immutable module/frame ownership");
    }

    const auto unavailable = unavailable_peer_path(fixture);
    relocated_path = write_variant(
        replace_all_ascii(read_file_bytes(core.path), fixture, unavailable));
    const GeneratedCore relocated{relocated_path, core.crash_tid, core.sibling_tid};

    const auto unavailable_output = run_core_cli(cli, relocated);
    require(unavailable_output.find("#1 0x") == std::string::npos,
            "relocated core unexpectedly recovered a caller frame without a module mapping");
    require(unavailable_output.find(kExpectedValue) == std::string::npos,
            "relocated core unexpectedly evaluated caller value without a module mapping");

    const auto mapped_output = run_core_cli(cli, relocated, unavailable, fixture);
    require_core_session_output(mapped_output, relocated, unavailable);
  } catch (...) {
    if (!relocated_path.empty()) std::remove(relocated_path.c_str());
    std::remove(core.path.c_str());
    throw;
  }
  if (!relocated_path.empty()) std::remove(relocated_path.c_str());
  std::remove(core.path.c_str());
}

void test_separate_debug_file(const std::string& fixture, const std::string& cli) {
  const auto artifacts = make_debuglink_artifacts(fixture);
  GeneratedCore core{};
  try {
    core = generate_core(artifacts.runtime_module);
    const auto without_debug = run_core_cli(cli, core);
    require(without_debug.find("#1 0x") != std::string::npos,
            "stripped runtime ELF lost .eh_frame caller recovery");
    require(without_debug.find(kExpectedValue) == std::string::npos,
            "stripped runtime ELF unexpectedly retained caller DWARF value evidence");

    const auto with_debug = run_core_cli(cli, core, {}, {}, artifacts.runtime_module,
                                         artifacts.debug_file);
    require_core_session_output(with_debug, core, artifacts.runtime_module);

    const auto bad_debug = run_core_cli(cli, core, {}, {}, artifacts.runtime_module,
                                        artifacts.bad_debug_file);
    require(bad_debug.find("debug companion CRC mismatch") != std::string::npos,
            "corrupted debug companion was not rejected by GNU debuglink identity");
  } catch (...) {
    if (!core.path.empty()) std::remove(core.path.c_str());
    std::filesystem::remove_all(artifacts.directory);
    throw;
  }
  if (!core.path.empty()) std::remove(core.path.c_str());
  std::filesystem::remove_all(artifacts.directory);
}

void test_omitted_file_backed_memory(const std::string& fixture,
                                     const std::string& cli) {
  const auto core = generate_core(fixture, true);
  try {
    const auto output = run_omitted_memory_cli(cli, core);
    require(output.find("artifact:") != std::string::npos,
            "omitted file-backed core memory was not reconstructed from owned artifact; output: " +
                output);
    require(output.find(fixture) != std::string::npos,
            "artifact-backed memory output lost recorded module ownership");
  } catch (...) {
    std::remove(core.path.c_str());
    throw;
  }
  std::remove(core.path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_inspection_session_integration <fixture> <mdbg-core>\n";
    return 2;
  }
  try {
    test_core_session(argv[1], argv[2]);
    test_separate_debug_file(argv[1], argv[2]);
    test_omitted_file_backed_memory(argv[1], argv[2]);
    std::cout << "core inspection session integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "core inspection session failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

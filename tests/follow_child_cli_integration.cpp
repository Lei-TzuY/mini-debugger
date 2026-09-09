#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct CliProcess {
  pid_t pid{-1};
  int input{-1};
  int output{-1};
  std::string pending_output;
};

CliProcess spawn_cli(const std::string& mdbg, const std::string& driver) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create CLI pipes");
  }

  const pid_t pid = ::fork();
  if (pid == -1) throw std::runtime_error("failed to fork CLI");
  if (pid == 0) {
    (void)::setpgid(0, 0);
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    ::execl(mdbg.c_str(), mdbg.c_str(), driver.c_str(), "--fork-topology", nullptr);
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  return CliProcess{pid, input_pipe[1], output_pipe[0], {}};
}

std::string target_for_driver(const std::string& driver) {
  const auto path = std::filesystem::path(driver);
  auto name = path.filename().string();
  constexpr const char* prefix = "exec_driver_";
  if (name.rfind(prefix, 0) != 0) {
    throw std::runtime_error("cannot derive exec target from driver path");
  }
  name.replace(0, std::string(prefix).size(), "exec_target_");
  return (path.parent_path() / name).string();
}

CliProcess spawn_divergence_cli(const std::string& mdbg, const std::string& driver,
                                const std::string& target) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create divergence CLI pipes");
  }

  const pid_t pid = ::fork();
  if (pid == -1) throw std::runtime_error("failed to fork divergence CLI");
  if (pid == 0) {
    (void)::setpgid(0, 0);
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    ::execl(mdbg.c_str(), mdbg.c_str(), driver.c_str(), "--fork-exec-divergence",
            target.c_str(), nullptr);
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  return CliProcess{pid, input_pipe[1], output_pipe[0], {}};
}

void terminate_cli(CliProcess& cli) noexcept {
  if (cli.input != -1) {
    ::close(cli.input);
    cli.input = -1;
  }
  if (cli.output != -1) {
    ::close(cli.output);
    cli.output = -1;
  }
  if (cli.pid > 0) {
    (void)::kill(-cli.pid, SIGKILL);
    (void)::kill(cli.pid, SIGKILL);
    int status = 0;
    while (::waitpid(cli.pid, &status, 0) == -1 && errno == EINTR) {
    }
    cli.pid = -1;
  }
}

void write_cli(CliProcess& cli, const std::string& command) {
  std::size_t offset = 0;
  while (offset < command.size()) {
    const auto count = ::write(cli.input, command.data() + offset, command.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed to write CLI command");
    offset += static_cast<std::size_t>(count);
  }
}

void require_cli_alive(const CliProcess& cli, const std::string& needle) {
  if (cli.pid <= 0) return;
  int status = 0;
  const auto result = ::waitpid(cli.pid, &status, WNOHANG);
  if (result == 0 || (result == -1 && errno == EINTR)) return;
  if (result == cli.pid) {
    throw std::runtime_error("CLI exited before output: " + needle);
  }
  if (result == -1 && errno != ECHILD) {
    throw std::runtime_error("failed checking CLI liveness");
  }
}

std::string read_until(CliProcess& cli, const std::string& needle) {
  for (;;) {
    const auto position = cli.pending_output.find(needle);
    if (position != std::string::npos) {
      const auto end = position + needle.size();
      auto output = cli.pending_output.substr(0, end);
      cli.pending_output.erase(0, end);
      return output;
    }

    pollfd descriptor{cli.output, POLLIN | POLLHUP, 0};
    const int result = ::poll(&descriptor, 1, 250);
    if (result == -1 && errno == EINTR) continue;
    if (result == -1) throw std::runtime_error("failed polling CLI output");
    if (result == 0) {
      require_cli_alive(cli, needle);
      continue;
    }

    char buffer[512];
    const auto count = ::read(cli.output, buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("CLI exited before output: " + needle);
    cli.pending_output.append(buffer, static_cast<std::size_t>(count));
  }
}

std::string read_to_eof(CliProcess& cli) {
  std::string output;
  output.swap(cli.pending_output);
  for (;;) {
    pollfd descriptor{cli.output, POLLIN | POLLHUP, 0};
    const int result = ::poll(&descriptor, 1, 250);
    if (result == -1 && errno == EINTR) continue;
    if (result == -1) throw std::runtime_error("failed polling CLI output to EOF");
    if (result == 0) continue;
    char buffer[512];
    const auto count = ::read(cli.output, buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count == 0) return output;
    if (count < 0) throw std::runtime_error("failed reading CLI output");
    output.append(buffer, static_cast<std::size_t>(count));
  }
}

void test_cli_stream_framing() {
  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) throw std::runtime_error("failed to create framing pipe");
  CliProcess cli{-1, pipe_fds[1], pipe_fds[0], {}};
  try {
    write_cli(cli, "first(mdbg) second(mdbg) ");
    ::close(cli.input);
    cli.input = -1;
    require(read_until(cli, "(mdbg) ") == "first(mdbg) ",
            "CLI framing consumed bytes past the first prompt");
    require(read_until(cli, "(mdbg) ") == "second(mdbg) ",
            "CLI framing did not preserve buffered prompt tail");
    ::close(cli.output);
    cli.output = -1;
  } catch (...) {
    if (cli.input != -1) ::close(cli.input);
    if (cli.output != -1) ::close(cli.output);
    throw;
  }
}

pid_t parse_pid_after(const std::string& output, const std::string& marker) {
  const auto position = output.find(marker);
  if (position == std::string::npos) {
    throw std::runtime_error("CLI output missing marker: " + marker);
  }
  const auto begin = position + marker.size();
  std::size_t end = begin;
  while (end < output.size() && output[end] >= '0' && output[end] <= '9') ++end;
  if (end == begin) throw std::runtime_error("CLI topology PID is missing");
  return static_cast<pid_t>(std::stol(output.substr(begin, end - begin)));
}

pid_t tracer_pid(pid_t pid) {
  std::ifstream input(std::filesystem::path("/proc") / std::to_string(pid) / "status");
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("TracerPid:", 0) == 0) {
      return static_cast<pid_t>(std::stol(line.substr(std::string("TracerPid:").size())));
    }
  }
  throw std::runtime_error("process status did not expose TracerPid");
}

void require_untraced(pid_t pid) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (tracer_pid(pid) == 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  throw std::runtime_error("follow-child parent remained ptrace-owned");
}

void require_traced_by(pid_t tracee, pid_t tracer, const std::string& message) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (tracer_pid(tracee) == tracer) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  throw std::runtime_error(message);
}

void require_process_gone(pid_t pid) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (!std::filesystem::exists(std::filesystem::path("/proc") / std::to_string(pid))) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  throw std::runtime_error("process remained after debugger session completion");
}

void require_clean_cli_exit(CliProcess& cli) {
  if (cli.input != -1) {
    ::close(cli.input);
    cli.input = -1;
  }
  if (cli.output != -1) {
    ::close(cli.output);
    cli.output = -1;
  }
  int status = 0;
  pid_t result;
  do {
    result = ::waitpid(cli.pid, &status, 0);
  } while (result == -1 && errno == EINTR);
  require(result == cli.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "mdbg fork CLI did not exit cleanly");
  cli.pid = -1;
}

void run_follow_child_cli(const std::string& driver, const std::string& mdbg) {
  auto cli = spawn_cli(mdbg, driver);
  pid_t parent = -1;
  try {
    auto output = read_until(cli, "(mdbg) ");
    require(output.find("stopped after exec") != std::string::npos,
            "follow-child CLI did not expose initial exec stop");

    write_cli(cli, "set follow-fork-mode child\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("follow-fork-mode = child") != std::string::npos,
            "CLI did not accept follow-child fork policy");

    write_cli(cli, "break fork_shared_probe\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("Breakpoint 1") != std::string::npos,
            "CLI did not install fork probe breakpoint");

    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    parent = parse_pid_after(output, "process fork: parent ");
    const auto child = parse_pid_after(output, "unfollowed, followed child ");
    require(parent > 0 && child > 0 && parent != child,
            "follow-child CLI did not expose distinct parent/child identities");
    require_untraced(parent);

    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("fork_shared_probe") != std::string::npos,
            "followed child did not hit retained CLI breakpoint");
    require(output.find("[tid " + std::to_string(child) + "]") != std::string::npos,
            "CLI breakpoint stop did not identify the followed child");

    write_cli(cli, "delete 1\n");
    (void)read_until(cli, "(mdbg) ");
    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("stopped by signal " + std::to_string(SIGSTOP)) != std::string::npos,
            "followed child did not surface its SIGSTOP in CLI");

    write_cli(cli, "continue\n");
    output = read_to_eof(cli);
    require(output.find("process exited with code 0") != std::string::npos,
            "followed child did not exit cleanly in CLI");
    require_clean_cli_exit(cli);
    require_process_gone(parent);
  } catch (...) {
    if (parent > 0) (void)::kill(parent, SIGKILL);
    terminate_cli(cli);
    throw;
  }
}

void run_two_process_cli(const std::string& driver, const std::string& mdbg) {
  auto cli = spawn_cli(mdbg, driver);
  pid_t parent = -1;
  pid_t child = -1;
  try {
    auto output = read_until(cli, "(mdbg) ");
    require(output.find("stopped after exec") != std::string::npos,
            "two-process CLI did not expose initial exec stop");

    write_cli(cli, "set follow-fork-mode both\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("follow-fork-mode = both") != std::string::npos,
            "CLI did not accept simultaneous fork ownership");

    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    parent = parse_pid_after(output, "retained parent ");
    child = parse_pid_after(output, "retained child ");
    require(parent > 0 && child > 0 && parent != child,
            "simultaneous fork did not expose distinct retained processes");
    require_traced_by(parent, cli.pid, "retained parent is not traced by mdbg");
    require_traced_by(child, cli.pid, "retained child is not traced by mdbg");

    write_cli(cli, "info processes\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("* " + std::to_string(parent) + " stopped") != std::string::npos,
            "retained parent is not the initial active process");
    require(output.find("  " + std::to_string(child) + " stopped") != std::string::npos,
            "retained child is missing from process registry");

    write_cli(cli, "x fork_shared_value 8\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("45 90 45 28 18 28 18 27") != std::string::npos,
            "parent COW value changed before parent execution");

    write_cli(cli, "process " + std::to_string(child) + "\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("selected process " + std::to_string(child)) != std::string::npos,
            "CLI did not select retained child process");

    write_cli(cli, "stepi\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("single-step trap on thread " + std::to_string(child)) !=
                std::string::npos,
            "single-step did not route through selected child process");

    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("stopped by signal " + std::to_string(SIGSTOP)) != std::string::npos &&
                output.find("on thread " + std::to_string(child)) != std::string::npos,
            "child did not surface its independent SIGSTOP");

    write_cli(cli, "x fork_shared_value 8\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("46 90 45 28 18 28 18 27") != std::string::npos,
            "child COW write was not visible in selected process");

    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("process " + std::to_string(child) + " exited with code 0") !=
                std::string::npos,
            "child terminal event was not surfaced independently");
    require(output.find("active process " + std::to_string(parent)) != std::string::npos,
            "remaining parent was not promoted after child exit");

    write_cli(cli, "info processes\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("* " + std::to_string(parent) + " stopped") != std::string::npos,
            "parent is not the active remaining process");

    write_cli(cli, "x fork_shared_value 8\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("45 90 45 28 18 28 18 27") != std::string::npos,
            "parent memory was corrupted by child COW execution");

    write_cli(cli, "continue\n");
    output = read_to_eof(cli);
    require(output.find("process exited with code 0") != std::string::npos,
            "remaining parent did not exit cleanly");
    require_clean_cli_exit(cli);
    require_process_gone(parent);
    require_process_gone(child);
  } catch (...) {
    if (parent > 0) (void)::kill(parent, SIGKILL);
    if (child > 0) (void)::kill(child, SIGKILL);
    terminate_cli(cli);
    throw;
  }
}

void run_process_exec_divergence_cli(const std::string& driver, const std::string& mdbg) {
  const auto target = target_for_driver(driver);
  auto cli = spawn_divergence_cli(mdbg, driver, target);
  pid_t parent = -1;
  pid_t child = -1;
  try {
    auto output = read_until(cli, "(mdbg) ");
    require(output.find("stopped after exec") != std::string::npos,
            "divergence CLI did not expose initial exec stop");

    write_cli(cli, "set follow-fork-mode both\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("follow-fork-mode = both") != std::string::npos,
            "divergence CLI did not accept simultaneous fork ownership");

    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    parent = parse_pid_after(output, "retained parent ");
    child = parse_pid_after(output, "retained child ");
    require(parent > 0 && child > 0 && parent != child,
            "divergence fork did not expose distinct retained processes");

    write_cli(cli, "break fork_shared_probe\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("Breakpoint 1") != std::string::npos,
            "parent old-image user breakpoint was not installed");

    write_cli(cli, "process " + std::to_string(child) + "\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("selected process " + std::to_string(child)) != std::string::npos,
            "divergence CLI did not select child");

    write_cli(cli, "break fork_shared_probe\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("Breakpoint 2") != std::string::npos,
            "child old-image user breakpoint was not installed independently");

    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("image replaced by") != std::string::npos &&
                output.find(target) != std::string::npos,
            "child exec did not refresh only the selected process image");

    write_cli(cli, "process " + std::to_string(parent) + "\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("selected process " + std::to_string(parent)) != std::string::npos,
            "divergence CLI could not return to old-image parent");

    write_cli(cli, "info breakpoints\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("1 ") != std::string::npos &&
                output.find("fork_shared_probe") != std::string::npos,
            "child exec invalidated the sibling parent user breakpoint registry");

    write_cli(cli, "process " + std::to_string(child) + "\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("selected process " + std::to_string(child)) != std::string::npos,
            "divergence CLI could not return to replacement-image child");

    write_cli(cli, "break exec_target_probe\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("Breakpoint 3") != std::string::npos,
            "replacement-image child could not create a monotonic user breakpoint");

    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("exec_target_probe") != std::string::npos &&
                output.find("[tid " + std::to_string(child) + "]") != std::string::npos,
            "replacement-image child did not hit its new symbol breakpoint");

    write_cli(cli, "delete 3\n");
    (void)read_until(cli, "(mdbg) ");
    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("process " + std::to_string(child) + " exited with code 0") !=
                std::string::npos,
            "replacement-image child exit was not surfaced independently");
    require(output.find("active process " + std::to_string(parent)) != std::string::npos,
            "old-image parent was not promoted after child exit");

    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("fork_shared_probe") != std::string::npos &&
                output.find("[tid " + std::to_string(parent) + "]") != std::string::npos,
            "old-image parent breakpoint did not survive sibling exec");

    write_cli(cli, "delete 1\n");
    (void)read_until(cli, "(mdbg) ");
    write_cli(cli, "continue\n");
    output = read_to_eof(cli);
    require(output.find("process exited with code 0") != std::string::npos,
            "old-image parent did not exit cleanly after sibling exec");
    require_clean_cli_exit(cli);
    require_process_gone(parent);
    require_process_gone(child);
  } catch (...) {
    if (parent > 0) (void)::kill(parent, SIGKILL);
    if (child > 0) (void)::kill(child, SIGKILL);
    terminate_cli(cli);
    throw;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: follow_child_cli_integration <driver> <mdbg>\n";
    return 2;
  }
  try {
    test_cli_stream_framing();
    run_follow_child_cli(argv[1], argv[2]);
    run_two_process_cli(argv[1], argv[2]);
    run_process_exec_divergence_cli(argv[1], argv[2]);
    std::cout << "fork CLI integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "fork CLI integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

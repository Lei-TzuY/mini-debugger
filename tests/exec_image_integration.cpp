#include "breakpoints/user_breakpoint_registry.hpp"
#include "debugger/debugger.hpp"
#include "elf/elf.hpp"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
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

bool same_file(const std::string& left, const std::string& right) {
  std::error_code error;
  const bool equivalent = std::filesystem::equivalent(left, right, error);
  return !error && equivalent;
}

void run_exec_replacement(const std::string& driver, const std::string& target) {
  auto debugger = mdbg::Debugger::launch(driver, {target});
  require(debugger.stop_info().reason == mdbg::StopReason::InitialExec,
          "exec driver did not expose the initial exec stop");
  const auto leader = debugger.pid();

  mdbg::ElfFile image(driver);
  mdbg::UserBreakpointRegistry breakpoints(debugger, image);
  const auto old_id = breakpoints.add_symbol("exec_syscall_probe");

  auto info = breakpoints.continue_execution();
  require(info.reason == mdbg::StopReason::ThreadCreated && info.tid != leader,
          "exec driver worker was not surfaced as a traced thread");
  const auto worker = info.tid;

  info = breakpoints.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint && info.tid == worker,
          "worker did not hit the managed exec syscall breakpoint");

  info = breakpoints.continue_execution();
  require(info.reason == mdbg::StopReason::Exec && info.tid == leader,
          "successful execve was not surfaced as an explicit image-replacement stop");
  require(info.former_tid.has_value() && *info.former_tid == worker,
          "non-leader exec did not report the former worker TID");
  require(debugger.active_tid() == leader,
          "exec replacement did not collapse active ownership to the leader TID");
  const auto threads = debugger.threads();
  require(threads.size() == 1 && threads.front().tid == leader && threads.front().active,
          "obsolete sibling TIDs remained selectable after exec replacement");
  require(debugger.breakpoints().empty(),
          "old-image managed breakpoint ownership survived exec replacement");
  require(same_file(debugger.executable_path(), target),
          "debugger executable identity was not refreshed after exec replacement");

  breakpoints.on_image_replaced();
  image = mdbg::ElfFile(debugger.executable_path());
  const auto new_id = breakpoints.add_symbol("exec_target_probe");
  require(new_id > old_id, "user breakpoint IDs were reused across image replacement");

  info = breakpoints.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint && info.tid == leader,
          "new-image symbol breakpoint did not execute after exec replacement");
  require(breakpoints.remove(new_id), "new-image breakpoint could not be removed");

  info = breakpoints.continue_execution();
  require(info.reason == mdbg::StopReason::Exited && info.value == 0,
          "replacement executable did not exit cleanly");
}

struct DetachedChildGuard {
  pid_t pid{-1};
  ~DetachedChildGuard() {
    if (pid > 0) (void)::kill(pid, SIGKILL);
  }
};

void require_child_stopped_untraced(pid_t child) {
  const auto status_path = std::filesystem::path("/proc") / std::to_string(child) / "status";
  for (int attempt = 0; attempt < 200; ++attempt) {
    std::ifstream input(status_path);
    std::string line;
    char state = '\0';
    pid_t tracer = -1;
    while (std::getline(input, line)) {
      if (line.rfind("State:", 0) == 0) {
        const auto position = line.find_first_not_of(" \t", 6);
        if (position != std::string::npos) state = line[position];
      } else if (line.rfind("TracerPid:", 0) == 0) {
        tracer = static_cast<pid_t>(std::stol(line.substr(std::string("TracerPid:").size())));
      }
    }
    if ((state == 'T' || state == 't') && tracer == 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  throw std::runtime_error("fork child did not become independently stopped and untraced");
}

void run_follow_parent_fork(const std::string& driver) {
  auto debugger = mdbg::Debugger::launch(driver, {"--fork-topology"});
  require(debugger.stop_info().reason == mdbg::StopReason::InitialExec,
          "fork driver did not expose the initial exec stop");
  const auto leader = debugger.pid();

  const mdbg::ElfFile image(driver);
  const auto probe = image.find_symbol("fork_shared_probe");
  require(probe.has_value(), "fork shared probe symbol is unavailable");
  const auto probe_address = static_cast<std::uintptr_t>(image.runtime_address(leader, *probe));
  const auto breakpoint_id = debugger.add_breakpoint(probe_address);

  auto info = debugger.continue_execution();
  require(info.process_event == mdbg::ProcessEventKind::Fork,
          "real fork was not classified as a process-topology event");
  require(info.tid == leader, "fork event did not remain owned by the followed parent");
  require(info.child_pid.has_value() && *info.child_pid > 0 && *info.child_pid != leader,
          "fork event did not expose a distinct child process identity");
  DetachedChildGuard child{*info.child_pid};

  const auto threads = debugger.threads();
  require(threads.size() == 1 && threads.front().tid == leader && threads.front().active,
          "unfollowed fork child leaked into the parent's TID registry");
  require(debugger.breakpoints().size() == 1 &&
              debugger.breakpoints().front().id == breakpoint_id,
          "parent breakpoint ownership changed at fork");

  require_child_stopped_untraced(child.pid);
  require(::kill(child.pid, SIGCONT) == 0, "failed to release detached fork child");

  info = debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint && info.tid == leader &&
              info.breakpoint_address == probe_address,
          "parent managed breakpoint did not survive child detachment");
  child.pid = -1;

  require(debugger.remove_breakpoint(breakpoint_id),
          "parent managed breakpoint could not be removed after fork");
  info = debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Exited && info.value == 0,
          "followed parent did not exit cleanly after detached child completion");
}

void run_follow_parent_vfork(const std::string& driver, const std::string& target) {
  auto debugger = mdbg::Debugger::launch(driver, {"--vfork-topology", target});
  require(debugger.stop_info().reason == mdbg::StopReason::InitialExec,
          "vfork driver did not expose the initial exec stop");
  const auto leader = debugger.pid();

  const mdbg::ElfFile image(driver);
  const auto probe = image.find_symbol("vfork_parent_probe");
  const auto value = image.find_symbol("vfork_parent_value");
  require(probe.has_value() && value.has_value(),
          "vfork parent probe/value symbols are unavailable");
  const auto probe_address =
      static_cast<std::uintptr_t>(image.runtime_address(leader, *probe));
  const auto value_address =
      static_cast<std::uintptr_t>(image.runtime_address(leader, *value));

  const auto breakpoint_id = debugger.add_breakpoint(probe_address);
  const auto watchpoint_id =
      debugger.add_write_watchpoint(value_address, sizeof(std::uint64_t));

  auto require_vfork_event = [&](const char* release_path) {
    const auto info = debugger.continue_execution();
    require(info.process_event == mdbg::ProcessEventKind::Vfork,
            std::string("real vfork ") + release_path +
                " was not classified as a vfork process-topology event");
    require(info.tid == leader,
            std::string("vfork ") + release_path +
                " event did not remain owned by the followed parent");
    require(info.child_pid.has_value() && *info.child_pid > 0 && *info.child_pid != leader,
            std::string("vfork ") + release_path +
                " event did not expose a distinct child process identity");

    const auto threads = debugger.threads();
    require(threads.size() == 1 && threads.front().tid == leader && threads.front().active,
            std::string("transient vfork ") + release_path +
                " child leaked into the parent's TID registry");
    require(debugger.breakpoints().size() == 1 &&
                debugger.breakpoints().front().id == breakpoint_id,
            std::string("parent breakpoint ownership changed across vfork ") + release_path);
    require(debugger.watchpoints().size() == 1 &&
                debugger.watchpoints().front().id == watchpoint_id,
            std::string("parent watchpoint ownership changed across vfork ") + release_path);
  };

  require_vfork_event("child-exit release");
  require_vfork_event("child-exec release");

  auto info = debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint && info.tid == leader &&
              info.breakpoint_address == probe_address,
          "parent managed breakpoint did not survive vfork shared-VM handling");

  info = debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Watchpoint && info.tid == leader &&
              info.watchpoint_id == watchpoint_id &&
              info.watchpoint_address == value_address,
          "parent hardware watchpoint did not survive vfork shared-VM handling");

  require(debugger.remove_breakpoint(breakpoint_id),
          "parent managed breakpoint could not be removed after vfork");
  require(debugger.remove_watchpoint(watchpoint_id),
          "parent hardware watchpoint could not be removed after vfork");
  info = debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Exited && info.value == 0,
          "followed parent did not exit cleanly after vfork completion");
}

struct CliProcess {
  pid_t pid{-1};
  int input{-1};
  int output{-1};
};

CliProcess spawn_cli(const std::string& mdbg, const std::string& driver,
                     const std::vector<std::string>& arguments) {
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

    std::vector<std::string> command{mdbg, driver};
    command.insert(command.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (auto& value : command) argv.push_back(value.data());
    argv.push_back(nullptr);
    ::execv(mdbg.c_str(), argv.data());
    _exit(127);
  }

  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  return CliProcess{pid, input_pipe[1], output_pipe[0]};
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

std::string read_until(CliProcess& cli, const std::string& needle) {
  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (output.find(needle) == std::string::npos) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) throw std::runtime_error("timed out waiting for CLI output: " + needle);
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd descriptor{cli.output, POLLIN | POLLHUP, 0};
    const int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
    if (result == -1 && errno == EINTR) continue;
    if (result <= 0) throw std::runtime_error("timed out reading CLI output: " + needle);

    char buffer[512];
    const auto count = ::read(cli.output, buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("CLI exited before output: " + needle);
    output.append(buffer, static_cast<std::size_t>(count));
  }
  return output;
}

std::string read_to_eof(CliProcess& cli) {
  std::string output;
  for (;;) {
    pollfd descriptor{cli.output, POLLIN | POLLHUP, 0};
    const int result = ::poll(&descriptor, 1, 5000);
    if (result == -1 && errno == EINTR) continue;
    if (result <= 0) throw std::runtime_error("timed out waiting for CLI exit");
    char buffer[512];
    const auto count = ::read(cli.output, buffer, sizeof(buffer));
    if (count == -1 && errno == EINTR) continue;
    if (count == 0) return output;
    if (count < 0) throw std::runtime_error("failed reading CLI output");
    output.append(buffer, static_cast<std::size_t>(count));
  }
}

void require_clean_cli_exit(CliProcess& cli) {
  ::close(cli.input);
  cli.input = -1;
  ::close(cli.output);
  cli.output = -1;
  int status = 0;
  pid_t result;
  do {
    result = ::waitpid(cli.pid, &status, 0);
  } while (result == -1 && errno == EINTR);
  require(result == cli.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "mdbg CLI did not exit cleanly");
  cli.pid = -1;
}

pid_t parse_pid_after(const std::string& output, const std::string& marker) {
  const auto position = output.find(marker);
  if (position == std::string::npos) {
    throw std::runtime_error("CLI output missing process-topology marker: " + marker);
  }
  const auto begin = position + marker.size();
  std::size_t end = begin;
  while (end < output.size() && output[end] >= '0' && output[end] <= '9') ++end;
  if (end == begin) throw std::runtime_error("CLI process-topology PID is missing");
  return static_cast<pid_t>(std::stol(output.substr(begin, end - begin)));
}

void require_topology_text(const std::string& output, const std::string& kind,
                           pid_t& parent, pid_t& child) {
  const auto parent_marker = "process " + kind + ": followed parent ";
  parent = parse_pid_after(output, parent_marker);
  const auto child_marker = kind == "vfork" ? ", transient child " : ", child ";
  child = parse_pid_after(output, child_marker);
  require(parent > 0 && child > 0 && parent != child,
          "CLI process-topology identities are not distinct positive PIDs");
  require(output.find("unfollowed") != std::string::npos,
          "CLI process-topology stop did not say the child is unfollowed");
}

void run_cli_exec_replacement(const std::string& driver, const std::string& target) {
  const auto mdbg = (std::filesystem::path(driver).parent_path() / "mdbg").string();
  auto cli = spawn_cli(mdbg, driver, {target});

  auto output = read_until(cli, "(mdbg) ");
  require(output.find("stopped after exec") != std::string::npos,
          "CLI did not expose initial exec stop");

  write_cli(cli, "break exec_syscall_probe\n");
  output = read_until(cli, "(mdbg) ");
  require(output.find("Breakpoint 1") != std::string::npos,
          "CLI did not install old-image exec breakpoint");

  write_cli(cli, "continue\n");
  output = read_until(cli, "(mdbg) ");
  require(output.find("created and stopped") != std::string::npos,
          "CLI did not surface exec worker creation");

  write_cli(cli, "continue\n");
  output = read_until(cli, "(mdbg) ");
  require(output.find("exec_syscall_probe") != std::string::npos,
          "CLI did not stop at old-image exec syscall breakpoint");

  write_cli(cli, "continue\n");
  output = read_until(cli, "(mdbg) ");
  require(output.find("image replaced by") != std::string::npos,
          "CLI did not surface image replacement");
  require(output.find(target) != std::string::npos,
          "CLI did not refresh replacement executable identity");

  write_cli(cli, "break exec_target_probe\n");
  output = read_until(cli, "(mdbg) ");
  require(output.find("Breakpoint 2") != std::string::npos,
          "CLI reused old user breakpoint ID or failed new-image symbol resolution");

  write_cli(cli, "continue\n");
  output = read_until(cli, "(mdbg) ");
  require(output.find("exec_target_probe") != std::string::npos,
          "CLI did not hit new-image managed breakpoint");

  write_cli(cli, "delete 2\n");
  (void)read_until(cli, "(mdbg) ");
  write_cli(cli, "continue\n");
  output = read_to_eof(cli);
  require(output.find("process exited with code 0") != std::string::npos,
          "CLI replacement image did not exit cleanly");
  require_clean_cli_exit(cli);
}

void run_cli_follow_parent_fork(const std::string& driver) {
  const auto mdbg = (std::filesystem::path(driver).parent_path() / "mdbg").string();
  auto cli = spawn_cli(mdbg, driver, {"--fork-topology"});
  try {
    auto output = read_until(cli, "(mdbg) ");
    require(output.find("stopped after exec") != std::string::npos,
            "CLI fork workflow did not expose initial exec stop");

    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    pid_t parent = -1;
    pid_t child = -1;
    require_topology_text(output, "fork", parent, child);
    DetachedChildGuard guard{child};
    require_child_stopped_untraced(child);
    require(::kill(child, SIGCONT) == 0, "failed to release CLI-unfollowed fork child");

    write_cli(cli, "continue\n");
    output = read_to_eof(cli);
    require(output.find("process exited with code 0") != std::string::npos,
            "CLI followed parent did not exit cleanly after fork");
    guard.pid = -1;
    require_clean_cli_exit(cli);
  } catch (...) {
    terminate_cli(cli);
    throw;
  }
}

void run_cli_follow_parent_vfork(const std::string& driver, const std::string& target) {
  const auto mdbg = (std::filesystem::path(driver).parent_path() / "mdbg").string();
  auto cli = spawn_cli(mdbg, driver, {"--vfork-topology", target});
  try {
    auto output = read_until(cli, "(mdbg) ");
    require(output.find("stopped after exec") != std::string::npos,
            "CLI vfork workflow did not expose initial exec stop");

    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    pid_t parent = -1;
    pid_t first_child = -1;
    require_topology_text(output, "vfork", parent, first_child);

    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    pid_t second_parent = -1;
    pid_t second_child = -1;
    require_topology_text(output, "vfork", second_parent, second_child);
    require(second_parent == parent,
            "CLI vfork transitions changed the followed parent identity");

    write_cli(cli, "continue\n");
    output = read_to_eof(cli);
    require(output.find("process exited with code 0") != std::string::npos,
            "CLI followed parent did not exit cleanly after vfork");
    require_clean_cli_exit(cli);
  } catch (...) {
    terminate_cli(cli);
    throw;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: exec_image_integration <driver> <target>\n";
    return 2;
  }
  try {
    run_exec_replacement(argv[1], argv[2]);
    run_follow_parent_fork(argv[1]);
    run_follow_parent_vfork(argv[1], argv[2]);
    run_cli_exec_replacement(argv[1], argv[2]);
    run_cli_follow_parent_fork(argv[1]);
    run_cli_follow_parent_vfork(argv[1], argv[2]);
    std::cout << "exec/process topology integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "exec/process topology integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

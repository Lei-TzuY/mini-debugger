#include "debugger/debugger.hpp"
#include "elf/elf.hpp"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

unsigned byte_at(mdbg::Debugger& debugger, std::uintptr_t address) {
  const auto bytes = debugger.read_memory(address, 1);
  require(bytes.size() == 1, "single-byte memory read returned the wrong size");
  return std::to_integer<unsigned>(bytes.front());
}

void require_three_domains(const mdbg::Debugger& debugger, pid_t parent, pid_t child,
                           pid_t grandchild, pid_t active) {
  const auto processes = debugger.processes();
  require(processes.size() == 3, "nested fork did not retain exactly three process domains");
  std::set<pid_t> seen;
  for (const auto& process : processes) {
    seen.insert(process.pid);
    require(process.state == mdbg::ProcessState::Stopped,
            "retained nested-fork process domain is not stopped");
    require(process.active == (process.pid == active),
            "process list active marker does not match selected domain");
  }
  require(seen == std::set<pid_t>{parent, child, grandchild},
          "nested process registry did not expose parent/child/grandchild identities");
}

void run_nested_registry_api(const std::string& driver) {
  auto debugger = mdbg::Debugger::launch(driver, {"--nested-fork-topology"});
  require(debugger.stop_info().reason == mdbg::StopReason::InitialExec,
          "nested-fork driver did not expose initial exec stop");
  const auto parent = debugger.pid();

  const mdbg::ElfFile image(driver);
  const auto probe = image.find_symbol("fork_shared_probe");
  require(probe.has_value(), "nested-fork shared probe symbol is unavailable");
  const auto probe_address =
      static_cast<std::uintptr_t>(image.runtime_address(parent, *probe));

  debugger.set_fork_follow_policy(mdbg::ForkFollowPolicy::Both);
  auto info = debugger.continue_execution();
  require(info.process_event == mdbg::ProcessEventKind::Fork && info.retains_child &&
              info.parent_pid == parent && info.child_pid.has_value(),
          "first nested-fork transition did not retain parent and child");
  const auto child = *info.child_pid;
  require(child != parent, "first nested fork did not create a distinct child");

  debugger.select_process(child);
  info = debugger.continue_execution();
  require(info.process_event == mdbg::ProcessEventKind::Fork && info.retains_child &&
              info.parent_pid == child && info.child_pid.has_value(),
          "second nested-fork transition did not retain child and grandchild");
  const auto grandchild = *info.child_pid;
  require(grandchild != parent && grandchild != child,
          "second nested fork did not create a distinct grandchild");
  require_three_domains(debugger, parent, child, grandchild, child);

  bool unknown_rejected = false;
  try {
    debugger.select_process(static_cast<pid_t>(2147483000));
  } catch (const std::invalid_argument&) {
    unknown_rejected = true;
  }
  require(unknown_rejected, "unknown process selection was not rejected deterministically");

  debugger.select_process(grandchild);
  const auto original_probe_byte = byte_at(debugger, probe_address);
  require(original_probe_byte != 0xccU,
          "fixture probe unexpectedly begins with an INT3 before debugger ownership");
  const auto grandchild_breakpoint = debugger.add_breakpoint(probe_address);
  require(byte_at(debugger, probe_address) == 0xccU,
          "grandchild managed breakpoint was not physically installed");

  debugger.select_process(child);
  require(byte_at(debugger, probe_address) == original_probe_byte,
          "grandchild breakpoint rewrote the child copy-on-write domain");
  debugger.select_process(parent);
  require(byte_at(debugger, probe_address) == original_probe_byte,
          "grandchild breakpoint rewrote the parent copy-on-write domain");
  debugger.select_process(grandchild);

  info = debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint && info.tid == grandchild &&
              info.breakpoint_address == probe_address,
          "grandchild did not hit its process-scoped breakpoint");
  info = debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Signal && info.value == SIGSTOP &&
              info.tid == grandchild,
          "grandchild did not surface its independent SIGSTOP");
  require(debugger.remove_breakpoint(grandchild_breakpoint),
          "grandchild breakpoint could not be removed");
  info = debugger.continue_execution(mdbg::SignalPolicy::Suppress);
  require(info.reason == mdbg::StopReason::ProcessExited && info.tid == grandchild &&
              info.value == 0,
          "grandchild exit was not surfaced independently");
  require(debugger.pid() == parent,
          "terminal promotion did not choose the deterministic lowest-PID stopped survivor");
  bool terminal_rejected = false;
  try {
    debugger.select_process(grandchild);
  } catch (const std::invalid_argument&) {
    terminal_rejected = true;
  }
  require(terminal_rejected, "exited grandchild remained selectable");

  debugger.select_process(child);
  const auto child_breakpoint = debugger.add_breakpoint(probe_address);
  debugger.select_process(parent);
  require(byte_at(debugger, probe_address) == original_probe_byte,
          "child breakpoint rewrote the retained parent domain");
  debugger.select_process(child);
  info = debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint && info.tid == child,
          "child did not hit its process-scoped breakpoint after grandchild exit");
  info = debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Signal && info.value == SIGSTOP && info.tid == child,
          "child did not surface its independent SIGSTOP");
  require(debugger.remove_breakpoint(child_breakpoint),
          "child breakpoint could not be removed independently");
  info = debugger.continue_execution(mdbg::SignalPolicy::Suppress);
  require(info.reason == mdbg::StopReason::ProcessExited && info.tid == child && info.value == 0,
          "child exit was not surfaced independently");
  require(debugger.pid() == parent, "parent was not retained after child exit");

  const auto parent_breakpoint = debugger.add_breakpoint(probe_address);
  info = debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Breakpoint && info.tid == parent,
          "parent did not hit its independent managed breakpoint");
  require(debugger.remove_breakpoint(parent_breakpoint),
          "parent breakpoint could not be removed");
  info = debugger.continue_execution();
  require(info.reason == mdbg::StopReason::Exited && info.tid == parent && info.value == 0,
          "final retained parent did not exit cleanly");
}

struct CliProcess {
  pid_t pid{-1};
  int input{-1};
  int output{-1};
};

CliProcess spawn_cli(const std::string& mdbg, const std::string& driver) {
  int input_pipe[2];
  int output_pipe[2];
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
    throw std::runtime_error("failed to create CLI pipes");
  }
  const auto pid = ::fork();
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
    ::execl(mdbg.c_str(), mdbg.c_str(), driver.c_str(), "--nested-fork-topology", nullptr);
    _exit(127);
  }
  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  return CliProcess{pid, input_pipe[1], output_pipe[0]};
}

void terminate_cli(CliProcess& cli) noexcept {
  if (cli.input != -1) ::close(cli.input);
  if (cli.output != -1) ::close(cli.output);
  if (cli.pid > 0) {
    (void)::kill(-cli.pid, SIGKILL);
    (void)::kill(cli.pid, SIGKILL);
    int status = 0;
    while (::waitpid(cli.pid, &status, 0) == -1 && errno == EINTR) {
    }
  }
  cli = {};
}

void write_cli(CliProcess& cli, const std::string& command) {
  std::size_t offset = 0;
  while (offset < command.size()) {
    const auto count = ::write(cli.input, command.data() + offset, command.size() - offset);
    if (count == -1 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("failed writing CLI command");
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

pid_t parse_pid_after(const std::string& output, const std::string& marker) {
  const auto position = output.find(marker);
  if (position == std::string::npos) throw std::runtime_error("missing CLI marker: " + marker);
  const auto begin = position + marker.size();
  std::size_t end = begin;
  while (end < output.size() && output[end] >= '0' && output[end] <= '9') ++end;
  if (end == begin) throw std::runtime_error("missing PID after CLI marker: " + marker);
  return static_cast<pid_t>(std::stol(output.substr(begin, end - begin)));
}

void run_nested_registry_cli(const std::string& driver) {
  const auto mdbg = (std::filesystem::path(driver).parent_path() / "mdbg").string();
  auto cli = spawn_cli(mdbg, driver);
  try {
    (void)read_until(cli, "(mdbg) ");
    write_cli(cli, "set follow-fork-mode both\n");
    (void)read_until(cli, "(mdbg) ");

    write_cli(cli, "continue\n");
    auto output = read_until(cli, "(mdbg) ");
    const auto parent = parse_pid_after(output, "retained parent ");
    const auto child = parse_pid_after(output, "retained child ");
    require(parent != child, "CLI first retained fork identities are not distinct");

    write_cli(cli, "process " + std::to_string(child) + "\n");
    (void)read_until(cli, "(mdbg) ");
    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    const auto second_parent = parse_pid_after(output, "retained parent ");
    const auto grandchild = parse_pid_after(output, "retained child ");
    require(second_parent == child && grandchild != parent && grandchild != child,
            "CLI second fork did not expose child/grandchild retention");

    write_cli(cli, "info processes\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find(std::to_string(parent) + " stopped") != std::string::npos &&
                output.find("* " + std::to_string(child) + " stopped") != std::string::npos &&
                output.find(std::to_string(grandchild) + " stopped") != std::string::npos,
            "CLI did not enumerate all three retained process domains");

    write_cli(cli, "process " + std::to_string(grandchild) + "\n");
    (void)read_until(cli, "(mdbg) ");
    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("stopped by signal") != std::string::npos,
            "CLI grandchild did not reach its independent SIGSTOP");
    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("process " + std::to_string(grandchild) + " exited with code 0") !=
                std::string::npos,
            "CLI grandchild exit was not surfaced independently");

    write_cli(cli, "process " + std::to_string(child) + "\n");
    (void)read_until(cli, "(mdbg) ");
    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("stopped by signal") != std::string::npos,
            "CLI child did not reach its independent SIGSTOP");
    write_cli(cli, "continue\n");
    output = read_until(cli, "(mdbg) ");
    require(output.find("process " + std::to_string(child) + " exited with code 0") !=
                std::string::npos,
            "CLI child exit was not surfaced independently");

    write_cli(cli, "process " + std::to_string(parent) + "\n");
    (void)read_until(cli, "(mdbg) ");
    write_cli(cli, "continue\n");
    output = read_to_eof(cli);
    require(output.find("process exited with code 0") != std::string::npos,
            "CLI final retained parent did not exit cleanly");

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
            "nested-process CLI did not exit cleanly");
    cli.pid = -1;
  } catch (...) {
    terminate_cli(cli);
    throw;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: nested_process_domain_integration <driver>\n";
    return 2;
  }
  try {
    run_nested_registry_api(argv[1]);
    run_nested_registry_cli(argv[1]);
    std::cout << "nested process-domain integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "nested process-domain integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

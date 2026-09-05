#include "breakpoints/user_breakpoint_registry.hpp"
#include "debugger/debugger.hpp"
#include "elf/elf.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: exec_image_integration <driver> <target>\n";
    return 2;
  }
  try {
    run_exec_replacement(argv[1], argv[2]);
    std::cout << "exec image integration passed\n";
  } catch (const std::exception& error) {
    std::cerr << "exec image integration failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

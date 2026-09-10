#include "snapshot/session.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string shell_quote(const std::string& value) {
  std::string result("'");
  for (const char ch : value) {
    if (ch == '\'') {
      result += "'\\''";
    } else {
      result += ch;
    }
  }
  result += '\'';
  return result;
}

std::string run_command(const std::string& command) {
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) throw std::runtime_error("failed to start subprocess");
  std::string output;
  char buffer[512];
  while (::fgets(buffer, sizeof(buffer), pipe) != nullptr) output += buffer;
  const int status = ::pclose(pipe);
  if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    throw std::runtime_error("subprocess failed:\n" + output);
  }
  return output;
}

std::filesystem::path produce_core(const std::string& fixture) {
  const pid_t child = ::fork();
  if (child == -1) throw std::runtime_error("failed to fork cross-file inline fixture");
  if (child == 0) {
    ::execl(fixture.c_str(), fixture.c_str(), nullptr);
    _exit(127);
  }

  int status = 0;
  while (::waitpid(child, &status, 0) == -1) {
  }
  require(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
          "cross-file inline fixture did not terminate with SIGSEGV");
  const auto core = std::filesystem::path("/tmp") /
                    ("mdbg-core-" + std::to_string(child));
  require(std::filesystem::exists(core),
          "cross-file inline fixture did not produce a genuine core");
  return core;
}

std::string basename(const std::string& path) {
  return std::filesystem::path(path).filename().string();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core_inline_callsite_integration <fixture> <mdbg-core>\n";
    return 2;
  }

  std::filesystem::path core;
  try {
    core = produce_core(argv[1]);
    mdbg::CoreInspectionSession session(core.string());
    const auto contexts = session.inline_contexts();
    require(contexts.size() == 2,
            "cross-file core did not expose exactly two inline contexts");
    require(contexts[0].name == "inline_outer" &&
                contexts[1].name == "inline_inner",
            "cross-file inline chain ordering changed unexpectedly");
    require(basename(contexts[0].call_site.file) == "inline_core_fixture.c",
            "outer inline call site did not resolve DW_AT_call_file to source file");
    require(basename(contexts[1].call_site.file) == "inline_core_fixture.h",
            "inner inline call site did not resolve DW_AT_call_file to header file");

    const std::string command = "printf 'inline\\nquit\\n' | " +
                                shell_quote(argv[2]) + " " +
                                shell_quote(core.string()) + " 2>&1";
    const auto output = run_command(command);
    require(output.find("inline_outer called at") != std::string::npos &&
                output.find("inline_core_fixture.c:") != std::string::npos,
            "mdbg-core did not render the outer compiler-owned call-site file");
    require(output.find("inline_inner called at") != std::string::npos &&
                output.find("inline_core_fixture.h:") != std::string::npos,
            "mdbg-core did not render the inner compiler-owned call-site file");

    std::error_code error;
    std::filesystem::remove(core, error);
    std::cout << "cross-file inline call-site integration passed\n";
  } catch (const std::exception& error) {
    if (!core.empty()) {
      std::error_code ignored;
      std::filesystem::remove(core, ignored);
    }
    std::cerr << "cross-file inline call-site integration failure: " << error.what()
              << '\n';
    return 1;
  }
  return 0;
}

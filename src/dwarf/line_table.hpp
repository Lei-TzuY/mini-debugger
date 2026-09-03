#pragma once

#include "elf/elf.hpp"

#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mdbg {

struct SourceLocation {
  std::string file;
  std::uint64_t line;
  std::uint64_t column;
};

class DwarfLineTable {
 public:
  explicit DwarfLineTable(std::string path);

  [[nodiscard]] bool available() const noexcept { return !ranges_.empty(); }
  [[nodiscard]] std::optional<SourceLocation> find_virtual_address(
      std::uint64_t address) const;
  [[nodiscard]] std::optional<SourceLocation> find_runtime_address(
      pid_t pid, std::uint64_t address, const ElfFile& elf) const;

 private:
  struct Range {
    std::uint64_t begin;
    std::uint64_t end;
    SourceLocation location;
  };

  void parse();

  std::string path_;
  std::vector<Range> ranges_;
};

}  // namespace mdbg

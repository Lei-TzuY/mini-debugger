#pragma once

#include "elf/elf.hpp"

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
  [[nodiscard]] std::optional<std::string> find_virtual_file(
      std::uint64_t address, std::uint64_t file_index) const;
  [[nodiscard]] std::optional<std::uint64_t> find_virtual_source(
      std::string_view file, std::uint64_t line) const;
  [[nodiscard]] std::optional<std::uint64_t> find_runtime_source(
      pid_t pid, std::string_view file, std::uint64_t line, const ElfFile& elf) const;

 private:
  struct FileTable {
    bool dwarf5;
    std::vector<std::string> files;
  };

  struct Range {
    std::uint64_t begin;
    std::uint64_t end;
    SourceLocation location;
    std::size_t file_table_index;
  };

  void parse();

  std::string path_;
  std::vector<FileTable> file_tables_;
  std::vector<Range> ranges_;
};

}  // namespace mdbg

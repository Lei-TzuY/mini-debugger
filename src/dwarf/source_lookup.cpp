#include "dwarf/line_table.hpp"

#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mdbg {
namespace {

std::string normalized_source_path(const std::filesystem::path& path) {
  return path.lexically_normal().generic_string();
}

}  // namespace

std::optional<std::uint64_t> DwarfLineTable::find_virtual_source(
    std::string_view file, std::uint64_t line) const {
  if (file.empty() || line == 0) return std::nullopt;

  const std::filesystem::path query_path{std::string(file)};
  const auto normalized_query = query_path.lexically_normal();
  const bool qualified_query = query_path.is_absolute() || query_path.has_parent_path();

  std::optional<std::uint64_t> best_address;
  std::optional<std::string> matched_file;

  for (const auto& range : ranges_) {
    if (range.location.line != line) continue;

    const std::filesystem::path candidate_path(range.location.file);
    const bool matches = qualified_query
                             ? candidate_path.lexically_normal() == normalized_query
                             : candidate_path.filename() == normalized_query.filename();
    if (!matches) continue;

    if (!qualified_query) {
      const auto candidate_identity = normalized_source_path(candidate_path);
      if (matched_file && *matched_file != candidate_identity) {
        throw std::runtime_error("ambiguous source file for line lookup: " +
                                 std::string(file));
      }
      matched_file = candidate_identity;
    }

    if (!best_address || range.begin < *best_address) best_address = range.begin;
  }

  return best_address;
}

std::optional<std::uint64_t> DwarfLineTable::find_runtime_source(
    pid_t pid, std::string_view file, std::uint64_t line, const ElfFile& elf) const {
  const auto virtual_address = find_virtual_source(file, line);
  if (!virtual_address) return std::nullopt;

  const auto bias = elf.load_bias(pid);
  if (*virtual_address > std::numeric_limits<std::uint64_t>::max() - bias) {
    throw std::runtime_error("source address overflows runtime address space");
  }
  return *virtual_address + bias;
}

}  // namespace mdbg

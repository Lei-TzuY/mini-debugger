#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mdbg {

class SourcePathResolver {
 public:
  void add_substitution(std::filesystem::path from, std::filesystem::path to) {
    from = normalized(std::move(from));
    to = normalized(std::move(to));
    if (from.empty() || from == ".") {
      throw std::invalid_argument("source substitution prefix must not be empty");
    }
    if (to.empty()) {
      throw std::invalid_argument("source substitution target must not be empty");
    }

    for (auto& substitution : substitutions_) {
      if (substitution.from == from) {
        substitution.to = std::move(to);
        return;
      }
    }
    substitutions_.push_back(Substitution{std::move(from), std::move(to)});
  }

  [[nodiscard]] std::optional<std::filesystem::path> resolve(
      const std::string& recorded_file, const std::string& module_path = {}) const {
    if (recorded_file.empty()) return std::nullopt;

    const std::filesystem::path recorded(recorded_file);
    if (const auto resolved = resolve_candidate(recorded)) return resolved;

    if (!recorded.is_absolute() && !module_path.empty()) {
      return resolve_candidate(std::filesystem::path(module_path).parent_path() / recorded);
    }
    return std::nullopt;
  }

 private:
  struct Substitution {
    std::filesystem::path from;
    std::filesystem::path to;
  };

  static std::filesystem::path normalized(std::filesystem::path path) {
    return path.lexically_normal();
  }

  static bool has_component_prefix(const std::filesystem::path& path,
                                   const std::filesystem::path& prefix) {
    auto path_it = path.begin();
    for (auto prefix_it = prefix.begin(); prefix_it != prefix.end(); ++prefix_it, ++path_it) {
      if (path_it == path.end() || *path_it != *prefix_it) return false;
    }
    return true;
  }

  static std::size_t component_count(const std::filesystem::path& path) {
    std::size_t count = 0;
    for (auto it = path.begin(); it != path.end(); ++it) ++count;
    return count;
  }

  static std::filesystem::path suffix_after(const std::filesystem::path& path,
                                            const std::filesystem::path& prefix) {
    auto path_it = path.begin();
    for (auto prefix_it = prefix.begin(); prefix_it != prefix.end(); ++prefix_it) ++path_it;

    std::filesystem::path suffix;
    for (; path_it != path.end(); ++path_it) suffix /= *path_it;
    return suffix;
  }

  [[nodiscard]] std::optional<std::filesystem::path> substituted_candidate(
      const std::filesystem::path& candidate) const {
    const auto normalized_candidate = normalized(candidate);
    const Substitution* best = nullptr;
    std::size_t best_components = 0;
    for (const auto& substitution : substitutions_) {
      if (!has_component_prefix(normalized_candidate, substitution.from)) continue;
      const auto components = component_count(substitution.from);
      if (!best || components > best_components) {
        best = &substitution;
        best_components = components;
      }
    }
    if (!best) return std::nullopt;
    return normalized(best->to / suffix_after(normalized_candidate, best->from));
  }

  [[nodiscard]] std::optional<std::filesystem::path> resolve_candidate(
      const std::filesystem::path& candidate) const {
    std::error_code error;
    if (const auto substituted = substituted_candidate(candidate)) {
      if (std::filesystem::is_regular_file(*substituted, error)) return substituted;
    }

    error.clear();
    const auto normalized_candidate = normalized(candidate);
    if (std::filesystem::is_regular_file(normalized_candidate, error)) {
      return normalized_candidate;
    }
    return std::nullopt;
  }

  std::vector<Substitution> substitutions_;
};

}  // namespace mdbg

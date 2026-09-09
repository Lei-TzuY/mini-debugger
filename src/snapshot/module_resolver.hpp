#pragma once

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mdbg {

class SnapshotModuleResolver {
 public:
  void add_exact_mapping(std::string recorded_path, std::string backing_path) {
    if (recorded_path.empty() || backing_path.empty()) {
      throw std::invalid_argument("core module mapping paths must not be empty");
    }
    const std::filesystem::path recorded{std::move(recorded_path)};
    const std::filesystem::path backing{std::move(backing_path)};
    if (!recorded.is_absolute() || !backing.is_absolute()) {
      throw std::invalid_argument("core module mapping paths must be absolute");
    }
    exact_[recorded.lexically_normal().string()] = backing.lexically_normal().string();
  }

  [[nodiscard]] std::string resolve(std::string_view recorded_path) const {
    if (recorded_path.empty()) {
      throw std::invalid_argument("recorded core module path must not be empty");
    }
    const std::filesystem::path recorded{std::string(recorded_path)};
    const auto normalized = recorded.lexically_normal().string();
    const auto mapped = exact_.find(normalized);
    return mapped == exact_.end() ? normalized : mapped->second;
  }

  [[nodiscard]] bool empty() const noexcept { return exact_.empty(); }

 private:
  std::map<std::string, std::string> exact_;
};

}  // namespace mdbg

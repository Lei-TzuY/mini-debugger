#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mdbg {

class Debugger;
class ElfFile;

struct EhFrameCursor {
  std::uintptr_t instruction_pointer;
  std::uintptr_t stack_pointer;
  std::optional<std::uintptr_t> frame_pointer;
};

class EhFrame {
 public:
  explicit EhFrame(std::string path);

  [[nodiscard]] bool available() const noexcept { return available_; }
  [[nodiscard]] std::optional<std::uintptr_t> caller_return_address(
      const Debugger& debugger, const ElfFile& elf) const;
  [[nodiscard]] std::optional<EhFrameCursor> caller_frame(
      const Debugger& debugger, const ElfFile& elf, const EhFrameCursor& current) const;

 private:
  std::string path_;
  std::vector<std::byte> section_;
  std::uint64_t section_virtual_address_{0};
  bool available_{false};
};

}  // namespace mdbg

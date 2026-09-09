#pragma once

#include <sys/types.h>
#include <sys/user.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mdbg {

struct ElfSymbol {
  std::string name;
  std::uint64_t value;
  std::uint64_t size;
  unsigned char type;
  unsigned char binding;
};

struct ResolvedSymbol {
  ElfSymbol symbol;
  std::uint64_t offset;
};

class ElfFile {
 public:
  explicit ElfFile(std::string path);

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] bool is_pie() const noexcept;
  [[nodiscard]] std::uint64_t load_virtual_base() const noexcept {
    return zero_offset_load_vaddr_;
  }
  [[nodiscard]] const std::vector<ElfSymbol>& symbols() const noexcept { return symbols_; }
  [[nodiscard]] std::optional<ElfSymbol> find_symbol(std::string_view name) const;
  [[nodiscard]] std::optional<ResolvedSymbol> find_symbol_by_virtual_address(
      std::uint64_t address) const;

  [[nodiscard]] std::uint64_t load_bias(pid_t pid) const;
  [[nodiscard]] std::uint64_t runtime_address(pid_t pid, const ElfSymbol& symbol) const;
  [[nodiscard]] std::optional<ResolvedSymbol> find_symbol_by_runtime_address(
      pid_t pid, std::uint64_t address) const;

 private:
  void parse();

  std::string path_;
  std::vector<std::byte> bytes_;
  std::vector<ElfSymbol> symbols_;
  std::uint16_t elf_type_{0};
  std::uint64_t zero_offset_load_vaddr_{0};
};

struct CoreFileMapping {
  std::uint64_t start;
  std::uint64_t end;
  std::uint64_t file_offset;
  std::string path;
};

struct CoreCrashInfo {
  int signal_number;
  int signal_code;
  std::optional<std::uintptr_t> fault_address;
};

struct CoreProcessInfo {
  pid_t pid;
  pid_t parent_pid;
  pid_t process_group_id;
  pid_t session_id;
  std::string file_name;
  std::string command;
};

struct CoreThreadSnapshot {
  pid_t tid;
  int signal_number;
  user_regs_struct registers;
  bool is_crashed;
};

class CoreSnapshot {
 public:
  explicit CoreSnapshot(std::string path);

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] pid_t crashed_tid() const noexcept { return crashed_tid_; }
  [[nodiscard]] int signal_number() const noexcept { return signal_number_; }
  [[nodiscard]] const std::optional<CoreCrashInfo>& crash_info() const noexcept {
    return crash_info_;
  }
  [[nodiscard]] const std::optional<CoreProcessInfo>& process_info() const noexcept {
    return process_info_;
  }
  [[nodiscard]] const user_regs_struct& registers() const noexcept { return registers_; }
  [[nodiscard]] const std::vector<CoreThreadSnapshot>& threads() const noexcept {
    return threads_;
  }
  [[nodiscard]] const CoreThreadSnapshot& crashed_thread() const noexcept {
    return threads_.front();
  }
  [[nodiscard]] const CoreThreadSnapshot& thread(pid_t tid) const;
  [[nodiscard]] const std::vector<CoreFileMapping>& file_mappings() const noexcept {
    return file_mappings_;
  }
  [[nodiscard]] std::vector<std::byte> read_memory(std::uintptr_t address,
                                                   std::size_t length) const;
  [[nodiscard]] std::optional<CoreFileMapping> mapping_for_address(
      std::uintptr_t address) const;

 private:
  struct LoadSegment {
    std::uint64_t virtual_address;
    std::uint64_t file_offset;
    std::uint64_t file_size;
    std::uint64_t memory_size;
  };

  void parse();
  void parse_notes(std::uint64_t offset, std::uint64_t size);

  std::string path_;
  std::vector<std::byte> bytes_;
  std::vector<LoadSegment> load_segments_;
  std::vector<CoreFileMapping> file_mappings_;
  std::vector<CoreThreadSnapshot> threads_;
  std::optional<CoreCrashInfo> crash_info_;
  std::optional<CoreProcessInfo> process_info_;
  user_regs_struct registers_{};
  pid_t crashed_tid_{-1};
  int signal_number_{0};
  bool has_registers_{false};
};

}  // namespace mdbg

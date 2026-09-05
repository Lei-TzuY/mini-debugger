#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mdbg {

class Debugger;
class ElfFile;

enum class LocalValueKind { Integer, Pointer };

struct LocalIntegerValue {
  std::string module_path;
  std::string name;
  std::uint64_t raw_value;
  std::size_t byte_size;
  bool is_signed;
  LocalValueKind kind{LocalValueKind::Integer};
};

using LocalScalarValue = LocalIntegerValue;

LocalScalarValue inspect_local_value(const Debugger& debugger,
                                     const ElfFile& preferred_elf,
                                     std::string_view name);

LocalIntegerValue inspect_local_integer(const Debugger& debugger,
                                        const ElfFile& preferred_elf,
                                        std::string_view name);

}  // namespace mdbg

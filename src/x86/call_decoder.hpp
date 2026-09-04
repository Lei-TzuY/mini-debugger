#pragma once

#include <cstddef>
#include <functional>
#include <optional>

namespace mdbg::x86 {

using ByteReader = std::function<std::optional<unsigned char>(std::size_t)>;

std::optional<std::size_t> decode_supported_near_call_length(
    const ByteReader& read_byte);

}  // namespace mdbg::x86

#include "x86/call_decoder.hpp"

namespace mdbg::x86 {

std::optional<std::size_t> decode_supported_near_call_length(
    const ByteReader& read_byte) {
  const auto first = read_byte(0);
  if (!first) return std::nullopt;

  if (*first == 0xe8U) return 5U;

  if (*first == 0x41U || *first == 0x48U || *first == 0x49U) {
    const auto opcode = read_byte(1);
    const auto modrm_byte = read_byte(2);
    if (!opcode || !modrm_byte || *opcode != 0xffU) return std::nullopt;

    const auto mod = (*modrm_byte >> 6U) & 0x3U;
    const auto reg = (*modrm_byte >> 3U) & 0x7U;
    if (reg == 2U && mod == 3U) return 3U;
    return std::nullopt;
  }

  if (*first != 0xffU) return std::nullopt;

  const auto modrm_byte = read_byte(1);
  if (!modrm_byte) return std::nullopt;

  const auto mod = (*modrm_byte >> 6U) & 0x3U;
  const auto reg = (*modrm_byte >> 3U) & 0x7U;
  const auto rm = *modrm_byte & 0x7U;
  if (reg != 2U) return std::nullopt;

  if (mod == 3U) return 2U;
  if (mod == 0U && rm == 5U) return 6U;
  if (mod == 0U && rm == 4U) {
    const auto sib = read_byte(2);
    if (!sib) return std::nullopt;
    const auto base = *sib & 0x7U;
    return base == 5U ? 7U : 3U;
  }
  if (mod == 0U) return 2U;
  if (mod == 1U) return rm == 4U ? 4U : 3U;
  if (mod == 2U) return rm == 4U ? 7U : 6U;

  return std::nullopt;
}

}  // namespace mdbg::x86

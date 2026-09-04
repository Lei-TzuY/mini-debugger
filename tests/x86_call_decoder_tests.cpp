#include "x86/call_decoder.hpp"

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::optional<std::size_t> decode(std::initializer_list<unsigned> bytes) {
  std::vector<unsigned char> input;
  input.reserve(bytes.size());
  for (const auto byte : bytes) {
    require(byte <= 0xffU, "test byte is out of range");
    input.push_back(static_cast<unsigned char>(byte));
  }

  return mdbg::x86::decode_supported_near_call_length(
      [&](std::size_t offset) -> std::optional<unsigned char> {
        if (offset >= input.size()) return std::nullopt;
        return input[offset];
      });
}

void require_length(std::initializer_list<unsigned> bytes, std::size_t expected,
                    const std::string& name) {
  const auto length = decode(bytes);
  require(length.has_value(), name + " was not recognized");
  require(*length == expected,
          name + " decoded length " + std::to_string(*length) +
              " instead of " + std::to_string(expected));
}

void require_unsupported(std::initializer_list<unsigned> bytes,
                         const std::string& name) {
  require(!decode(bytes).has_value(), name + " must remain unsupported");
}

void test_supported_classes() {
  require_length({0xe8}, 5, "direct rel32 call");
  require_length({0xff, 0xd0}, 2, "register-indirect call");
  require_length({0xff, 0x10}, 2, "base-memory call");
  require_length({0xff, 0x15}, 6, "RIP-relative call");
  require_length({0xff, 0x14, 0x24}, 3, "SIB base-memory call");
  require_length({0xff, 0x14, 0x25}, 7, "SIB no-base call");
  require_length({0xff, 0x50}, 3, "disp8 base-memory call");
  require_length({0xff, 0x54}, 4, "disp8 SIB call");
  require_length({0xff, 0x90}, 6, "disp32 base-memory call");
  require_length({0xff, 0x94}, 7, "disp32 SIB call");
  require_length({0x41, 0xff, 0xd0}, 3, "REX.B register call");
  require_length({0x48, 0xff, 0xd0}, 3, "REX.W register call");
  require_length({0x49, 0xff, 0xd0}, 3, "REX.W+B register call");
}

void test_explicit_fallbacks() {
  require_unsupported({0x90}, "non-call opcode");
  require_unsupported({0xff}, "truncated FF call");
  require_unsupported({0xff, 0xd8}, "FF /3 far call");
  require_unsupported({0xff, 0x14}, "truncated SIB call");
  require_unsupported({0x40, 0xff, 0xd0}, "unsupported REX prefix");
  require_unsupported({0x41, 0xff}, "truncated supported REX call");
  require_unsupported({0x41, 0xff, 0x10}, "REX memory call");
}

void test_lazy_reads() {
  std::size_t reads = 0;
  const auto direct = mdbg::x86::decode_supported_near_call_length(
      [&](std::size_t offset) -> std::optional<unsigned char> {
        ++reads;
        if (offset == 0) return 0xe8U;
        throw std::runtime_error("direct call decoder read past opcode");
      });
  require(direct == 5U, "direct call lazy decode failed");
  require(reads == 1U, "direct call decoder performed unnecessary reads");

  reads = 0;
  const auto non_call = mdbg::x86::decode_supported_near_call_length(
      [&](std::size_t offset) -> std::optional<unsigned char> {
        ++reads;
        if (offset == 0) return 0x90U;
        throw std::runtime_error("non-call decoder read past opcode");
      });
  require(!non_call.has_value(), "non-call lazy decode must fall back");
  require(reads == 1U, "non-call decoder performed unnecessary reads");
}

}  // namespace

int main() {
  try {
    test_supported_classes();
    test_explicit_fallbacks();
    test_lazy_reads();
    return 0;
  } catch (const std::exception& error) {
    return error.what() == nullptr ? 2 : 1;
  }
}

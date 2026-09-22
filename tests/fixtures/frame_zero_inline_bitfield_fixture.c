#include <stdint.h>

struct FrameZeroInlineBitFields {
  signed int signed_bits : 5;
  unsigned int unsigned_bits : 6;
};

static __attribute__((always_inline)) inline int
frame_zero_inline_bitfield_inner(int seed) {
  struct FrameZeroInlineBitFields inline_bit_fields = {
      .signed_bits = -7,
      .unsigned_bits = 41,
  };

  __asm__ volatile(
      ".globl snapshot_frame_zero_inline_bitfield_probe\n"
      "snapshot_frame_zero_inline_bitfield_probe:\n"
      "movl %k1, (%%rax)\n"
      :
      : "m"(inline_bit_fields), "r"(inline_bit_fields.signed_bits),
        "a"((uintptr_t)0)
      : "memory");
  __builtin_unreachable();
}

static __attribute__((always_inline)) inline int
frame_zero_inline_bitfield_outer(int seed) {
  return frame_zero_inline_bitfield_inner(seed + 3);
}

__attribute__((noinline)) static int
frame_zero_inline_bitfield_physical(void) {
  return frame_zero_inline_bitfield_outer(17);
}

int main(void) { return frame_zero_inline_bitfield_physical(); }

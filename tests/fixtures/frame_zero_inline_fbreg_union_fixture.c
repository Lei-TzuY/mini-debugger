#include <stdint.h>

union FrameZeroInlineFbregUnion {
  int32_t signed_value;
  uint32_t unsigned_value;
};

static __attribute__((always_inline)) inline int
frame_zero_inline_fbreg_union_inner(int seed) {
  union FrameZeroInlineFbregUnion inline_fbreg_union = {
      .signed_value = seed + INT32_C(0x55667700),
  };

  __asm__ volatile(
      ".globl snapshot_frame_zero_inline_fbreg_union_probe\n"
      "snapshot_frame_zero_inline_fbreg_union_probe:\n"
      "movl %1, (%%rax)\n"
      :
      : "m"(inline_fbreg_union), "r"(inline_fbreg_union.signed_value),
        "a"((uintptr_t)0)
      : "memory");
  __builtin_unreachable();
}

static __attribute__((always_inline)) inline int
frame_zero_inline_fbreg_union_outer(int seed) {
  return frame_zero_inline_fbreg_union_inner(seed + 3);
}

__attribute__((noinline)) static int
frame_zero_inline_fbreg_union_physical(void) {
  return frame_zero_inline_fbreg_union_outer(17);
}

int main(void) { return frame_zero_inline_fbreg_union_physical(); }

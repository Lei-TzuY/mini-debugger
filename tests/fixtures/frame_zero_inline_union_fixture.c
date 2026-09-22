#include <stdint.h>

union FrameZeroInlineUnion {
  int32_t signed_value;
  uint32_t unsigned_value;
};

static __attribute__((always_inline)) inline int
frame_zero_inline_union_inner(int seed) {
  union FrameZeroInlineUnion inline_union = {
      .signed_value = seed + INT32_C(0x44556600),
  };

  __asm__ volatile("" : "+r"(inline_union.signed_value) :: "memory");
  __asm__ volatile(
      ".globl snapshot_frame_zero_inline_union_probe\n"
      "snapshot_frame_zero_inline_union_probe:\n"
      "movl %0, (%%rax)\n"
      :
      : "r"(inline_union.signed_value), "a"((uintptr_t)0)
      : "memory");
  __builtin_unreachable();
}

static __attribute__((always_inline)) inline int
frame_zero_inline_union_outer(int seed) {
  return frame_zero_inline_union_inner(seed + 3);
}

__attribute__((noinline)) static int
frame_zero_inline_union_physical(void) {
  return frame_zero_inline_union_outer(17);
}

int main(void) { return frame_zero_inline_union_physical(); }

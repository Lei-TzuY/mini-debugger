#include <stdint.h>

struct FrameZeroInlineNestedInner {
  int32_t terminal;
};

struct FrameZeroInlineNestedOuter {
  int32_t prefix;
  struct FrameZeroInlineNestedInner inner;
};

static __attribute__((always_inline)) inline int
frame_zero_inline_nested_inner(int seed) {
  struct FrameZeroInlineNestedOuter inline_nested = {
      INT32_C(0x10203040), {INT32_C(0x55667788)}};

  __asm__ volatile(
      ".globl snapshot_frame_zero_inline_nested_probe\n"
      "snapshot_frame_zero_inline_nested_probe:\n"
      "movl %k1, (%%rax)\n"
      :
      : "m"(inline_nested), "r"(inline_nested.prefix), "a"((uintptr_t)0)
      : "memory");
  __builtin_unreachable();
}

static __attribute__((always_inline)) inline int
frame_zero_inline_nested_outer(int seed) {
  return frame_zero_inline_nested_inner(seed + 3);
}

__attribute__((noinline)) static int
frame_zero_inline_nested_physical(void) {
  return frame_zero_inline_nested_outer(17);
}

int main(void) { return frame_zero_inline_nested_physical(); }

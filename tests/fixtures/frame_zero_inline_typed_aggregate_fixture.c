#include <stdint.h>

struct FrameZeroInlineTyped {
  int32_t* linked;
};

static int32_t frame_zero_inline_typed_payload = INT32_C(0x02468ace);

static __attribute__((always_inline)) inline int
frame_zero_inline_typed_inner(int seed) {
  struct FrameZeroInlineTyped inline_typed = {
      &frame_zero_inline_typed_payload};

  __asm__ volatile(
      ".globl snapshot_frame_zero_inline_typed_probe\n"
      "snapshot_frame_zero_inline_typed_probe:\n"
      "testq %0, %0\n"
      "movl %k1, (%%rax)\n"
      :
      : "r"(inline_typed.linked), "r"(seed), "a"((uintptr_t)0)
      : "cc", "memory");
  __builtin_unreachable();
}

static __attribute__((always_inline)) inline int
frame_zero_inline_typed_outer(int seed) {
  return frame_zero_inline_typed_inner(seed + 5);
}

__attribute__((noinline)) static int frame_zero_inline_typed_physical(void) {
  return frame_zero_inline_typed_outer(29);
}

int main(void) { return frame_zero_inline_typed_physical(); }

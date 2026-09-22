#include <stdint.h>

struct FrameZeroInlinePair {
  int32_t first;
  int32_t second;
};

static __attribute__((always_inline)) inline int
frame_zero_inline_structure_inner(int seed) {
  struct FrameZeroInlinePair inline_pair = {
      seed + INT32_C(0x11223300),
      seed + INT32_C(0x55667700),
  };

  __asm__ volatile("" : "+r"(inline_pair.first), "+r"(inline_pair.second) :: "memory");
  __asm__ volatile(
      ".globl snapshot_frame_zero_inline_structure_probe\n"
      "snapshot_frame_zero_inline_structure_probe:\n"
      "movl %0, (%%rax)\n"
      :
      : "r"(inline_pair.first), "r"(inline_pair.second),
        "a"((uintptr_t)0)
      : "memory");
  __builtin_unreachable();
}

static __attribute__((always_inline)) inline int
frame_zero_inline_structure_outer(int seed) {
  return frame_zero_inline_structure_inner(seed + 3);
}

__attribute__((noinline)) static int
frame_zero_inline_structure_physical(void) {
  return frame_zero_inline_structure_outer(17);
}

int main(void) { return frame_zero_inline_structure_physical(); }

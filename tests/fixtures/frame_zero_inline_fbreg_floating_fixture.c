#include <stdint.h>

static __attribute__((always_inline)) inline int
frame_zero_inline_fbreg_floating_inner(int seed) {
  double inline_fbreg_floating = seed == 20 ? 1234.25 : -0.5;

  __asm__ volatile(
      ".globl snapshot_frame_zero_inline_fbreg_floating_probe\n"
      "snapshot_frame_zero_inline_fbreg_floating_probe:\n"
      "movsd %1, (%%rax)\n"
      :
      : "m"(inline_fbreg_floating), "x"(inline_fbreg_floating),
        "a"((uintptr_t)0)
      : "memory");
  __builtin_unreachable();
}

static __attribute__((always_inline)) inline int
frame_zero_inline_fbreg_floating_outer(int seed) {
  return frame_zero_inline_fbreg_floating_inner(seed + 3);
}

__attribute__((noinline)) static int
frame_zero_inline_fbreg_floating_physical(void) {
  return frame_zero_inline_fbreg_floating_outer(17);
}

int main(void) { return frame_zero_inline_fbreg_floating_physical(); }

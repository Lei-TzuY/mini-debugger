#include <stdint.h>

static __attribute__((always_inline)) inline int
frame_zero_inline_fbreg_array_inner(int seed) {
  int32_t inline_fbreg_array[2] = {
      seed + INT32_C(0x10203000),
      seed + INT32_C(0x40506000),
  };

  __asm__ volatile(
      ".globl snapshot_frame_zero_inline_fbreg_array_probe\n"
      "snapshot_frame_zero_inline_fbreg_array_probe:\n"
      "movl %1, (%%rax)\n"
      :
      : "m"(inline_fbreg_array), "r"(inline_fbreg_array[1]),
        "a"((uintptr_t)0)
      : "memory");
  __builtin_unreachable();
}

static __attribute__((always_inline)) inline int
frame_zero_inline_fbreg_array_outer(int seed) {
  return frame_zero_inline_fbreg_array_inner(seed + 3);
}

__attribute__((noinline)) static int
frame_zero_inline_fbreg_array_physical(void) {
  return frame_zero_inline_fbreg_array_outer(17);
}

int main(void) { return frame_zero_inline_fbreg_array_physical(); }

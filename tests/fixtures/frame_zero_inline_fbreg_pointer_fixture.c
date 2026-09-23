#include <stdint.h>

static int32_t frame_zero_inline_fbreg_pointer_pointee =
    INT32_C(0x13579bdf);

static __attribute__((always_inline)) inline int
frame_zero_inline_fbreg_pointer_inner(int seed) {
  int32_t* inline_fbreg_pointer =
      seed == 20 ? &frame_zero_inline_fbreg_pointer_pointee : (int32_t*)0;

  __asm__ volatile(
      ".globl snapshot_frame_zero_inline_fbreg_pointer_probe\n"
      "snapshot_frame_zero_inline_fbreg_pointer_probe:\n"
      "movq %1, (%%rax)\n"
      :
      : "m"(inline_fbreg_pointer), "r"(inline_fbreg_pointer),
        "a"((uintptr_t)0)
      : "memory");
  __builtin_unreachable();
}

static __attribute__((always_inline)) inline int
frame_zero_inline_fbreg_pointer_outer(int seed) {
  return frame_zero_inline_fbreg_pointer_inner(seed + 3);
}

__attribute__((noinline)) static int
frame_zero_inline_fbreg_pointer_physical(void) {
  return frame_zero_inline_fbreg_pointer_outer(17);
}

int main(void) { return frame_zero_inline_fbreg_pointer_physical(); }

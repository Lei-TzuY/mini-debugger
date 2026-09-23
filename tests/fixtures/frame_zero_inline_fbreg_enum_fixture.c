#include <stdint.h>

enum FrameZeroInlineFbregMode {
  FrameZeroInlineFbregIdle = 3,
  FrameZeroInlineFbregReady = 7,
  FrameZeroInlineFbregBusy = 42,
};

static __attribute__((always_inline)) inline int
frame_zero_inline_fbreg_enum_inner(int seed) {
  enum FrameZeroInlineFbregMode inline_fbreg_mode =
      seed == 20 ? FrameZeroInlineFbregBusy : FrameZeroInlineFbregReady;

  __asm__ volatile(
      ".globl snapshot_frame_zero_inline_fbreg_enum_probe\n"
      "snapshot_frame_zero_inline_fbreg_enum_probe:\n"
      "movl %1, (%%rax)\n"
      :
      : "m"(inline_fbreg_mode), "r"((uint32_t)inline_fbreg_mode),
        "a"((uintptr_t)0)
      : "memory");
  __builtin_unreachable();
}

static __attribute__((always_inline)) inline int
frame_zero_inline_fbreg_enum_outer(int seed) {
  return frame_zero_inline_fbreg_enum_inner(seed + 3);
}

__attribute__((noinline)) static int
frame_zero_inline_fbreg_enum_physical(void) {
  return frame_zero_inline_fbreg_enum_outer(17);
}

int main(void) { return frame_zero_inline_fbreg_enum_physical(); }

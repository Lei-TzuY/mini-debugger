#include <stdint.h>

enum FrameZeroInlineMode {
  FrameZeroInlineIdle = 3,
  FrameZeroInlineReady = 7,
  FrameZeroInlineBusy = 42,
};

struct FrameZeroInlineEnumAggregate {
  int32_t direct;
  enum FrameZeroInlineMode mode;
};

static __attribute__((always_inline)) inline int
frame_zero_inline_enum_aggregate_inner(int seed) {
  struct FrameZeroInlineEnumAggregate inline_enum_aggregate = {
      INT32_C(0x31415926), FrameZeroInlineBusy};
  __asm__ volatile("" : : "r"(&inline_enum_aggregate) : "memory");
  __asm__ volatile(
      ".globl snapshot_frame_zero_inline_enum_aggregate_probe\n"
      "snapshot_frame_zero_inline_enum_aggregate_probe:\n"
      "testl %0, %0\n"
      "movl %0, (%%rax)\n"
      :
      : "r"(seed), "a"((uintptr_t)0)
      : "cc", "memory");
  __builtin_unreachable();
}

static __attribute__((always_inline)) inline int
frame_zero_inline_enum_aggregate_outer(int seed) {
  return frame_zero_inline_enum_aggregate_inner(seed + 5);
}

__attribute__((noinline)) static int
frame_zero_inline_enum_aggregate_physical(void) {
  return frame_zero_inline_enum_aggregate_outer(29);
}

int main(void) { return frame_zero_inline_enum_aggregate_physical(); }

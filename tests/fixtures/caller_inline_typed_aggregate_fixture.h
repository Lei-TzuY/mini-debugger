#pragma once

enum CallerInlineTypedMode {
  CallerInlineTypedIdle = 3,
  CallerInlineTypedReady = 7,
  CallerInlineTypedBusy = 42,
};

struct CallerInlineTypedAggregate {
  int direct;
  enum CallerInlineTypedMode mode;
};

int caller_typed_crash_leaf(int token,
                            const struct CallerInlineTypedAggregate* selected);

static __attribute__((always_inline)) inline int caller_typed_inline_inner(int seed) {
  struct CallerInlineTypedAggregate caller_typed_aggregate = {
      0x31415926, CallerInlineTypedBusy};
  __asm__ volatile("" : : "m"(caller_typed_aggregate) : "memory");
  int crashed = caller_typed_crash_leaf(seed + 5, &caller_typed_aggregate);
  __asm__ volatile(
      ".globl snapshot_caller_typed_inline_resume_probe\n"
      "snapshot_caller_typed_inline_resume_probe:\n"
      :
      : "m"(caller_typed_aggregate)
      : "memory");
  return crashed + caller_typed_aggregate.direct + (int)caller_typed_aggregate.mode;
}

static __attribute__((always_inline)) inline int caller_typed_inline_outer(int seed) {
  int outer_only = seed * 3 + 1;
  return caller_typed_inline_inner(outer_only);
}

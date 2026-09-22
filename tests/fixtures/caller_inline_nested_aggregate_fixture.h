#pragma once

struct CallerInlineNestedInner {
  int terminal;
};

struct CallerInlineNestedOuter {
  int prefix;
  struct CallerInlineNestedInner inner;
};

int caller_nested_crash_leaf(int token,
                             const struct CallerInlineNestedOuter* selected);

static __attribute__((always_inline)) inline int caller_nested_inline_inner(int seed) {
  struct CallerInlineNestedOuter caller_nested_aggregate = {
      0x10203040, {0x55667788}};
  __asm__ volatile("" : : "m"(caller_nested_aggregate) : "memory");
  int crashed = caller_nested_crash_leaf(seed + 9, &caller_nested_aggregate);
  __asm__ volatile(
      ".globl snapshot_caller_nested_inline_resume_probe\n"
      "snapshot_caller_nested_inline_resume_probe:\n"
      :
      : "m"(caller_nested_aggregate)
      : "memory");
  return crashed + caller_nested_aggregate.prefix +
         caller_nested_aggregate.inner.terminal;
}

static __attribute__((always_inline)) inline int caller_nested_inline_outer(int seed) {
  int outer_only = seed * 5 + 3;
  return caller_nested_inline_inner(outer_only);
}

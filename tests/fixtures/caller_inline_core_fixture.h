#pragma once

struct CallerInlineAggregate {
  int direct;
  const int* linked;
};

extern int caller_inline_pointee;
extern int caller_inline_member_pointee;
extern struct CallerInlineAggregate caller_inline_aggregate;

int caller_inline_crash_leaf(
    int token, const int* observed, const int* const* selected,
    const struct CallerInlineAggregate* const* selected_aggregate,
    const struct CallerInlineAggregate* direct_aggregate);

static __attribute__((always_inline)) inline int caller_inline_inner(int seed) {
  int caller_shadow = seed + 43;
  const int* caller_pointer = &caller_inline_pointee;
  const struct CallerInlineAggregate* caller_aggregate_pointer =
      &caller_inline_aggregate;
  struct CallerInlineAggregate caller_direct_aggregate = {
      0x55667788, &caller_inline_pointee};
  int crashed = caller_inline_crash_leaf(
      seed + 5, &caller_shadow, &caller_pointer, &caller_aggregate_pointer,
      &caller_direct_aggregate);
  __asm__ volatile(
      ".globl snapshot_caller_inline_resume_probe\n"
      "snapshot_caller_inline_resume_probe:\n"
      ::: "memory");
  return crashed + caller_shadow + *caller_pointer +
         caller_aggregate_pointer->direct + *caller_aggregate_pointer->linked +
         caller_direct_aggregate.direct + *caller_direct_aggregate.linked;
}

static __attribute__((always_inline)) inline int caller_inline_outer(int seed) {
  int outer_only = seed * 3 + 1;
  return caller_inline_inner(outer_only);
}

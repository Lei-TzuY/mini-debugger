#pragma once

struct CallerInlineAggregate {
  int direct;
  const int* linked;
};

union CallerInlineUnion {
  int signed_value;
  unsigned int unsigned_value;
};

extern int caller_inline_pointee;
extern int caller_inline_member_pointee;
extern struct CallerInlineAggregate caller_inline_aggregate;

int caller_inline_crash_leaf(
    int token, const int* observed, const int* const* selected,
    const struct CallerInlineAggregate* const* selected_aggregate,
    const struct CallerInlineAggregate* direct_aggregate,
    const int* fixed_array, const union CallerInlineUnion* selected_union);

static __attribute__((always_inline)) inline int caller_inline_inner(int seed) {
  int caller_shadow = seed + 43;
  const int* caller_pointer = &caller_inline_pointee;
  const struct CallerInlineAggregate* caller_aggregate_pointer =
      &caller_inline_aggregate;
  struct CallerInlineAggregate caller_direct_aggregate = {
      0x55667788, &caller_inline_pointee};
  int caller_fixed_array[3] = {0x10203040, 0x22334455, 0x33445566};
  union CallerInlineUnion caller_union = {.signed_value = 0x44556677};
  int crashed = caller_inline_crash_leaf(
      seed + 5, &caller_shadow, &caller_pointer, &caller_aggregate_pointer,
      &caller_direct_aggregate, caller_fixed_array, &caller_union);
  __asm__ volatile(
      ".globl snapshot_caller_inline_resume_probe\n"
      "snapshot_caller_inline_resume_probe:\n"
      ::: "memory");
  return crashed + caller_shadow + *caller_pointer +
         caller_aggregate_pointer->direct + *caller_aggregate_pointer->linked +
         caller_direct_aggregate.direct + *caller_direct_aggregate.linked +
         caller_fixed_array[1] + caller_union.signed_value;
}

static __attribute__((always_inline)) inline int caller_inline_outer(int seed) {
  int outer_only = seed * 3 + 1;
  return caller_inline_inner(outer_only);
}

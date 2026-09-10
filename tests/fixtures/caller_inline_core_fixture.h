#pragma once

struct CallerInlineAggregate {
  int direct;
  const int* linked;
};

union CallerInlineUnion {
  int signed_value;
  unsigned int unsigned_value;
};

struct CallerInlineBitFields {
  signed int signed_bits : 5;
  unsigned int unsigned_bits : 6;
};

enum CallerInlineState {
  CALLER_INLINE_STARTING = 3,
  CALLER_INLINE_READY = 7,
  CALLER_INLINE_DONE = 19,
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
  struct CallerInlineBitFields caller_bit_fields = {
      .signed_bits = -7, .unsigned_bits = 41};
  enum CallerInlineState caller_state = CALLER_INLINE_READY;
  __asm__ volatile("" : : "m"(caller_bit_fields), "m"(caller_state) : "memory");
  int crashed = caller_inline_crash_leaf(
      seed + 5 + caller_bit_fields.signed_bits + caller_bit_fields.unsigned_bits +
          caller_state,
      &caller_shadow, &caller_pointer, &caller_aggregate_pointer,
      &caller_direct_aggregate, caller_fixed_array, &caller_union);
  __asm__ volatile(
      ".globl snapshot_caller_inline_resume_probe\n"
      "snapshot_caller_inline_resume_probe:\n"
      :
      : "m"(caller_bit_fields), "m"(caller_state)
      : "memory");
  return crashed + caller_shadow + *caller_pointer +
         caller_aggregate_pointer->direct + *caller_aggregate_pointer->linked +
         caller_direct_aggregate.direct + *caller_direct_aggregate.linked +
         caller_fixed_array[1] + caller_union.signed_value +
         caller_bit_fields.signed_bits + caller_bit_fields.unsigned_bits +
         caller_state;
}

static __attribute__((always_inline)) inline int caller_inline_outer(int seed) {
  int outer_only = seed * 3 + 1;
  return caller_inline_inner(outer_only);
}

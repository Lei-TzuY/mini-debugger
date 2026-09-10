#include "caller_inline_core_fixture.h"

#include <stdint.h>

__attribute__((noinline)) int caller_inline_crash_leaf(
    int token, const int* observed, const int* const* selected,
    const struct CallerInlineAggregate* const* selected_aggregate,
    const struct CallerInlineAggregate* direct_aggregate,
    const int* fixed_array, const union CallerInlineUnion* selected_union) {
  const int observed_value = *observed;
  const int selected_value = **selected;
  const int aggregate_direct = (*selected_aggregate)->direct;
  const int aggregate_linked = *(*selected_aggregate)->linked;
  const int direct_aggregate_direct = direct_aggregate->direct;
  const int direct_aggregate_linked = *direct_aggregate->linked;
  const int array_value = fixed_array[0] + fixed_array[1] + fixed_array[2];
  const int union_value = selected_union->signed_value;
  __asm__ volatile(
      ".globl snapshot_caller_inline_crash_probe\n"
      "snapshot_caller_inline_crash_probe:\n"
      "movl %0, (%%rax)\n"
      :
      : "r"(token + observed_value + selected_value + aggregate_direct +
            aggregate_linked + direct_aggregate_direct + direct_aggregate_linked +
            array_value + union_value),
        "a"((uintptr_t)0)
      : "memory");
  return token + observed_value + selected_value + aggregate_direct + aggregate_linked +
         direct_aggregate_direct + direct_aggregate_linked + array_value + union_value;
}

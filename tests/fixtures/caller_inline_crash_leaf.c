#include "caller_inline_core_fixture.h"

#include <stdint.h>

__attribute__((noinline)) int caller_inline_crash_leaf(
    int token, const int* observed, const int* const* selected,
    const struct CallerInlineAggregate* const* selected_aggregate) {
  const int observed_value = *observed;
  const int selected_value = **selected;
  const int aggregate_direct = (*selected_aggregate)->direct;
  const int aggregate_linked = *(*selected_aggregate)->linked;
  __asm__ volatile(
      ".globl snapshot_caller_inline_crash_probe\n"
      "snapshot_caller_inline_crash_probe:\n"
      "movl %0, (%%rax)\n"
      :
      : "r"(token + observed_value + selected_value + aggregate_direct +
            aggregate_linked),
        "a"((uintptr_t)0)
      : "memory");
  return token + observed_value + selected_value + aggregate_direct +
         aggregate_linked;
}

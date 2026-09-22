#include "caller_inline_nested_aggregate_fixture.h"

#include <stdint.h>

__attribute__((noinline)) int caller_nested_crash_leaf(
    int token, const struct CallerInlineNestedOuter* selected) {
  const int prefix = selected->prefix;
  const int terminal = selected->inner.terminal;
  __asm__ volatile(
      ".globl snapshot_caller_nested_inline_crash_probe\n"
      "snapshot_caller_nested_inline_crash_probe:\n"
      "movl %0, (%%rax)\n"
      :
      : "r"(token + prefix + terminal), "a"((uintptr_t)0)
      : "memory");
  return token + prefix + terminal;
}

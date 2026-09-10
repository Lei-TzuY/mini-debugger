#include "caller_inline_typed_aggregate_fixture.h"

#include <stdint.h>

__attribute__((noinline)) int caller_typed_crash_leaf(
    int token, const struct CallerInlineTypedAggregate* selected) {
  const int direct = selected->direct;
  const int mode = (int)selected->mode;
  __asm__ volatile(
      ".globl snapshot_caller_typed_inline_crash_probe\n"
      "snapshot_caller_typed_inline_crash_probe:\n"
      "movl %0, (%%rax)\n"
      :
      : "r"(token + direct + mode), "a"((uintptr_t)0)
      : "memory");
  return token + direct + mode;
}

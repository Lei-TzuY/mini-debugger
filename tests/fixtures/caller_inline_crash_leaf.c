#include <stdint.h>

__attribute__((noinline)) int caller_inline_crash_leaf(int token, const int* observed,
                                                       const int* const* selected) {
  const int observed_value = *observed;
  const int selected_value = **selected;
  __asm__ volatile(
      ".globl snapshot_caller_inline_crash_probe\n"
      "snapshot_caller_inline_crash_probe:\n"
      "movl %0, (%%rax)\n"
      :
      : "r"(token + observed_value + selected_value), "a"((uintptr_t)0)
      : "memory");
  return token + observed_value + selected_value;
}

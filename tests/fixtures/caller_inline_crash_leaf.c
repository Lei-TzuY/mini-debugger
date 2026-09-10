#include <stdint.h>

__attribute__((noinline)) int caller_inline_crash_leaf(int token) {
  __asm__ volatile(
      ".globl snapshot_caller_inline_crash_probe\n"
      "snapshot_caller_inline_crash_probe:\n"
      "movl %0, (%%rax)\n"
      :
      : "r"(token), "a"((uintptr_t)0)
      : "memory");
  return token;
}

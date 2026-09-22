#pragma once

#include <stdint.h>

static int inline_pointer_pointee = INT32_C(0x13579bdf);

static __attribute__((always_inline)) inline int inline_inner(int seed) {
  int inner_only = seed + 7;
  int shadow_value = inner_only ^ 0x55;
  int* inline_pointer = &inline_pointer_pointee;
  __asm__ volatile("" : "+r"(inner_only), "+r"(shadow_value),
                   "+r"(inline_pointer) :: "memory");
  __asm__ volatile(
      ".globl snapshot_inline_crash_probe\n"
      "snapshot_inline_crash_probe:\n"
      "movl %0, (%%rax)\n"
      :
      : "r"(shadow_value), "a"((uintptr_t)0)
      : "memory");
  __builtin_unreachable();
}

static __attribute__((always_inline)) inline int inline_outer(int seed) {
  int outer_only = seed * 3;
  int shadow_value = outer_only + 11;
  __asm__ volatile("" : "+r"(outer_only), "+r"(shadow_value) :: "memory");
  return inline_inner(outer_only + shadow_value);
}

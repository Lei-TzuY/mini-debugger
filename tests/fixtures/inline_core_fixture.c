#include <stdint.h>

static __attribute__((always_inline)) inline int inline_inner(int seed) {
  int inner_only = seed + 7;
  int shadow_value = inner_only ^ 0x55;
  __asm__ volatile("" : "+r"(inner_only), "+r"(shadow_value) :: "memory");
  __asm__ volatile(
      ".globl snapshot_inline_crash_probe\n"
      "snapshot_inline_crash_probe:\n"
      "movl %0, (%%rax)\n"
      :
      : "r"(inner_only + shadow_value), "a"((uintptr_t)0)
      : "memory");
  __builtin_unreachable();
}

static __attribute__((always_inline)) inline int inline_outer(int seed) {
  int outer_only = seed * 3;
  int shadow_value = outer_only + 11;
  __asm__ volatile("" : "+r"(outer_only), "+r"(shadow_value) :: "memory");
  return inline_inner(outer_only + shadow_value);
}

__attribute__((noinline)) static int physical_frame(int seed) {
  int physical_only = seed + 1;
  __asm__ volatile("" : "+r"(physical_only) :: "memory");
  return inline_outer(seed + physical_only);
}

__attribute__((noinline)) static int physical_caller(void) {
  int caller_only = 21;
  __asm__ volatile("" : "+r"(caller_only) :: "memory");
  return physical_frame(caller_only);
}

int main(void) { return physical_caller(); }

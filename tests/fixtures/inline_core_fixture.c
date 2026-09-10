#include "inline_core_fixture.h"

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

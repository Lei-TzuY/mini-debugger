#pragma once

int caller_inline_crash_leaf(int token, const int* observed);

static __attribute__((always_inline)) inline int caller_inline_inner(int seed) {
  int caller_shadow = seed + 43;
  const int* caller_pointer = &caller_shadow;
  int crashed = caller_inline_crash_leaf(seed + 5, caller_pointer);
  __asm__ volatile(
      ".globl snapshot_caller_inline_resume_probe\n"
      "snapshot_caller_inline_resume_probe:\n"
      ::: "memory");
  return crashed + *caller_pointer;
}

static __attribute__((always_inline)) inline int caller_inline_outer(int seed) {
  int outer_only = seed * 3 + 1;
  return caller_inline_inner(outer_only);
}

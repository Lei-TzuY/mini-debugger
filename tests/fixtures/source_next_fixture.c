volatile int next_marker = 0;

__attribute__((noinline)) void next_callee(void) {
#line 510 "next_source.c"
  next_marker += 1;
#line 511 "next_source.c"
  next_marker += 0;
}

__attribute__((noinline)) void next_caller(void) {
#line 500 "next_source.c"
  next_callee();
#line 501 "next_source.c"
  next_marker += 2;
}

__attribute__((noinline)) void next_indirect_callee(void) {
#line 530 "next_source.c"
  next_marker += 4;
#line 531 "next_source.c"
  next_marker += 0;
}

__attribute__((noinline)) void next_indirect_caller(void) {
#line 520 "next_source.c"
  __asm__ volatile(
      "leaq next_indirect_callee(%%rip), %%rax\n\t"
      "call *%%rax"
      :
      :
      : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "cc", "memory");
#line 521 "next_source.c"
  next_marker += 8;
}

#line 1 "next_driver.c"
int main(void) {
  next_caller();
  next_indirect_caller();
  return next_marker == 15 ? 0 : 1;
}

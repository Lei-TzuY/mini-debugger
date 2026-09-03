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

__attribute__((noinline)) void next_memory_callee(void) {
#line 550 "next_source.c"
  next_marker += 16;
#line 551 "next_source.c"
  next_marker += 0;
}

__attribute__((used)) static void (*next_memory_target)(void) = next_memory_callee;

__attribute__((noinline)) void next_memory_caller(void) {
#line 540 "next_source.c"
  __asm__ volatile(
      "call *next_memory_target(%%rip)"
      :
      :
      : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "cc", "memory");
#line 541 "next_source.c"
  next_marker += 32;
}

__attribute__((noinline)) void next_base_memory_callee(void) {
#line 570 "next_source.c"
  next_marker += 64;
#line 571 "next_source.c"
  next_marker += 0;
}

__attribute__((used)) static void (*next_base_memory_target)(void) = next_base_memory_callee;

__attribute__((noinline)) void next_base_memory_caller(void) {
  void (**slot)(void);
#line 559 "next_source.c"
  __asm__ volatile("leaq next_base_memory_target(%%rip), %%rax" : "=a"(slot));
#line 560 "next_source.c"
  __asm__ volatile(
      "call *(%%rax)"
      : "+a"(slot)
      :
      : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "cc", "memory");
#line 561 "next_source.c"
  next_marker += 128;
}

#line 1 "next_driver.c"
int main(void) {
  next_caller();
  next_indirect_caller();
  next_memory_caller();
  next_base_memory_caller();
  return next_marker == 255 ? 0 : 1;
}

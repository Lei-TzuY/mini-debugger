volatile int disp8_marker = 0;
volatile int disp32_marker = 0;

__attribute__((noinline)) void next_disp8_callee(void) {
#line 590 "next_source.c"
  disp8_marker += 1;
#line 591 "next_source.c"
  disp8_marker += 0;
}

__attribute__((used)) static void (*next_disp8_targets[2])(void) = {0,
                                                                   next_disp8_callee};

__attribute__((noinline)) void next_disp8_caller(void) {
  void (**slot)(void);
#line 579 "next_source.c"
  __asm__ volatile("leaq next_disp8_targets(%%rip), %%rax" : "=a"(slot));
#line 580 "next_source.c"
  __asm__ volatile(
      "call *8(%%rax)"
      : "+a"(slot)
      :
      : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "cc", "memory");
#line 581 "next_source.c"
  disp8_marker += 2;
}

__attribute__((noinline)) void next_disp32_callee(void) {
#line 610 "next_source.c"
  disp32_marker += 1;
#line 611 "next_source.c"
  disp32_marker += 0;
}

__attribute__((used)) static void (*next_disp32_targets[2])(void) = {0,
                                                                    next_disp32_callee};

__attribute__((noinline)) void next_disp32_caller(void) {
  void (**slot)(void);
#line 599 "next_source.c"
  __asm__ volatile("leaq next_disp32_targets(%%rip), %%rax" : "=a"(slot));
#line 600 "next_source.c"
  __asm__ volatile(
      ".byte 0xff, 0x90, 0x08, 0x00, 0x00, 0x00"
      : "+a"(slot)
      :
      : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "cc", "memory");
#line 601 "next_source.c"
  disp32_marker += 2;
}

#line 1 "next_disp_driver.c"
int main(void) {
  next_disp8_caller();
  next_disp32_caller();
  return disp8_marker == 3 && disp32_marker == 3 ? 0 : 1;
}

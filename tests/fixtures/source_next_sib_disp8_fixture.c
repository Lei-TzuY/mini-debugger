volatile int sib_disp8_marker = 0;

__attribute__((noinline)) void next_sib_disp8_callee(void) {
#line 650 "next_source.c"
  sib_disp8_marker += 1;
#line 651 "next_source.c"
  sib_disp8_marker += 0;
}

__attribute__((used)) static void (*next_sib_disp8_targets[2])(void) = {
    0, next_sib_disp8_callee};

__attribute__((noinline)) void next_sib_disp8_caller(void) {
  void (**slot)(void);
#line 639 "next_source.c"
  __asm__ volatile("leaq next_sib_disp8_targets(%%rip), %%rax" : "=a"(slot));
#line 640 "next_source.c"
  __asm__ volatile(
      ".byte 0xff, 0x54, 0x20, 0x08"
      : "+a"(slot)
      :
      : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "cc", "memory");
#line 641 "next_source.c"
  sib_disp8_marker += 2;
}

#line 1 "next_sib_disp8_driver.c"
int main(void) {
  next_sib_disp8_caller();
  return sib_disp8_marker == 3 ? 0 : 1;
}

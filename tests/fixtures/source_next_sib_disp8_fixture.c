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

__attribute__((noinline)) void next_sib_disp32_callee(void) {
#line 670 "next_source.c"
  sib_disp8_marker += 4;
#line 671 "next_source.c"
  sib_disp8_marker += 0;
}

__attribute__((used)) static void (*next_sib_disp32_targets[2])(void) = {
    0, next_sib_disp32_callee};

__attribute__((noinline)) void next_sib_disp32_caller(void) {
  void (**slot)(void);
#line 659 "next_source.c"
  __asm__ volatile("leaq next_sib_disp32_targets(%%rip), %%rax" : "=a"(slot));
#line 660 "next_source.c"
  __asm__ volatile(
      ".byte 0xff, 0x94, 0x20, 0x08, 0x00, 0x00, 0x00"
      : "+a"(slot)
      :
      : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "cc", "memory");
#line 661 "next_source.c"
  sib_disp8_marker += 8;
}

__attribute__((noinline)) void next_sib_nobase_callee(void) {
#line 690 "next_source.c"
  sib_disp8_marker += 16;
#line 691 "next_source.c"
  sib_disp8_marker += 0;
}

__attribute__((used)) static void (*next_sib_nobase_target)(void) =
    next_sib_nobase_callee;

__attribute__((noinline)) void next_sib_nobase_caller(void) {
  void (**slot)(void);
#line 679 "next_source.c"
  __asm__ volatile("leaq next_sib_nobase_target(%%rip), %%rax" : "=a"(slot));
#line 680 "next_source.c"
  __asm__ volatile(
      ".byte 0xff, 0x14, 0x05, 0x00, 0x00, 0x00, 0x00"
      : "+a"(slot)
      :
      : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "cc", "memory");
#line 681 "next_source.c"
  sib_disp8_marker += 32;
}

#line 1 "next_sib_disp8_driver.c"
int main(void) {
  next_sib_disp8_caller();
  next_sib_disp32_caller();
  next_sib_nobase_caller();
  return sib_disp8_marker == 63 ? 0 : 1;
}

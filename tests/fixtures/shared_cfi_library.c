volatile int shared_cfi_marker = 0;
volatile int shared_next_marker = 0;
volatile int shared_symbol_marker = 0;

__attribute__((noinline)) void shared_symbol_break_target(void) {
  shared_symbol_marker += 1;
}

__attribute__((noinline)) void module_symbol_ambiguous(void) {
  __asm__ volatile("nop" ::: "memory");
}

__attribute__((noinline)) void shared_break_target(void) {
#line 700 "shared_break_source.c"
  __asm__ volatile("nop" ::: "memory");
#line 17 "shared_cfi_library.c"
}

__attribute__((noinline)) void shared_step_target(void) {
#line 710 "shared_step_source.c"
  __asm__ volatile("nop\n\tnop" ::: "memory");
#line 711 "shared_step_source.c"
  __asm__ volatile("nop" ::: "memory");
#line 25 "shared_cfi_library.c"
}

__attribute__((noinline)) static void shared_next_callee(void) {
#line 730 "shared_next_callee.c"
  shared_next_marker += 1;
#line 31 "shared_cfi_library.c"
}

__attribute__((noinline)) void shared_next_target(void) {
#line 720 "shared_next_source.c"
  shared_next_callee();
#line 721 "shared_next_source.c"
  __asm__ volatile("nop" ::: "memory");
#line 39 "shared_cfi_library.c"
}

__attribute__((noinline)) void shared_ambiguous_target(void) {
#line 900 "ambiguous_break_source.c"
  __asm__ volatile("nop" ::: "memory");
#line 45 "shared_cfi_library.c"
}

__attribute__((noinline)) void shared_leaf(void) {
  volatile unsigned char scratch[256];
  __asm__ volatile(".globl shared_cfi_probe\nshared_cfi_probe:\n\tnop");
  scratch[0] = 1;
  shared_cfi_marker += scratch[0];
}

__attribute__((noinline)) void shared_inner(void) {
  shared_leaf();
  shared_cfi_marker += 0;
}

__attribute__((noinline)) void shared_outer(void) {
  shared_inner();
  shared_cfi_marker += 0;
}

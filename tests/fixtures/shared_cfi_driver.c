#include <signal.h>

extern volatile int shared_cfi_marker;
extern void shared_symbol_break_target(void);
extern void shared_break_target(void);
extern void shared_step_target(void);
extern void shared_next_target(void);
extern void shared_outer(void);

__attribute__((noinline)) void module_symbol_ambiguous(void) {
  __asm__ volatile("nop" ::: "memory");
}

__attribute__((noinline)) void driver_break_target(void) {
#line 800 "driver_break_source.c"
  __asm__ volatile("nop" ::: "memory");
#line 18 "shared_cfi_driver.c"
}

__attribute__((noinline)) void driver_ambiguous_target(void) {
#line 900 "ambiguous_break_source.c"
  __asm__ volatile("nop" ::: "memory");
#line 24 "shared_cfi_driver.c"
}

int main(void) {
  if (raise(SIGSTOP) != 0) return 2;
  shared_symbol_break_target();
  shared_break_target();
  shared_step_target();
  shared_next_target();
  driver_break_target();
  shared_outer();
  return shared_cfi_marker == 1 ? 0 : 1;
}

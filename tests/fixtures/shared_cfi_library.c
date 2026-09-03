volatile int shared_cfi_marker = 0;

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

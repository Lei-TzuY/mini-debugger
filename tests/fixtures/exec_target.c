#include <stdint.h>

volatile uint64_t exec_target_value = UINT64_C(0x1020304050607080);

__attribute__((noinline)) void exec_target_probe(void) {
  __asm__ volatile("nop" ::: "memory");
  exec_target_value += 1;
}

int main(void) {
  exec_target_probe();
  return exec_target_value == UINT64_C(0x1020304050607081) ? 0 : 1;
}

#include <stdint.h>

volatile uint64_t pointer_target = UINT64_C(0x8877665544332211);

__attribute__((noinline)) int inspect_local_pointer(void) {
  uint64_t* local_pointer = (uint64_t*)&pointer_target;
  __asm__ volatile(".globl local_pointer_probe\n"
                   "local_pointer_probe:\n"
                   "nop"
                   : "+m"(local_pointer)
                   :
                   : "memory");
  return local_pointer == (uint64_t*)&pointer_target &&
                 *local_pointer == UINT64_C(0x8877665544332211)
             ? 0
             : 1;
}

int main(void) { return inspect_local_pointer(); }

#include <stdint.h>

volatile uint64_t value_seed = UINT64_C(0x1122334455667788);
#define LOCAL_VALUE_EXPECTED UINT64_C(0x1020304050607080)

__attribute__((noinline)) uint64_t inspect_local_value(void) {
  uint64_t local_value = value_seed ^ UINT64_C(0x0102030405060708);
  __asm__ volatile(".globl local_value_probe\nlocal_value_probe:\nnop" ::: "memory");
  return local_value;
}

#line 400 "mapped_source.c"
__attribute__((noinline)) void line_probe(void) {
#line 401 "mapped_source.c"
  __asm__ volatile("nop\n\tnop\n\tnop" ::: "memory");
#line 402 "mapped_source.c"
  __asm__ volatile("nop" ::: "memory");
}

#line 1 "debug_line_driver.c"
int main(int argc, char** argv) {
  (void)argv;
  if (argc == 2) {
    return inspect_local_value() == LOCAL_VALUE_EXPECTED ? 0 : 1;
  }
  line_probe();
  return 0;
}

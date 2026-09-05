#include <stdint.h>

#define PARAMETER_EXPECTED UINT64_C(0x1020304050607080)

volatile uint64_t parameter_seed = UINT64_C(0x1122334455667788);

__attribute__((noinline)) uint64_t inspect_parameter_value(uint64_t parameter) {
  __asm__ volatile(".globl formal_parameter_probe\n"
                   "formal_parameter_probe:\n"
                   "nop\n"
                   : "+D"(parameter)
                   :
                   : "memory");
  return parameter;
}

int main(void) {
  const uint64_t parameter = parameter_seed ^ UINT64_C(0x0102030405060708);
  return inspect_parameter_value(parameter) == PARAMETER_EXPECTED ? 0 : 1;
}

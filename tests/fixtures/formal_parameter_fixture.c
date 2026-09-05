#include <stdint.h>

#define PARAMETER_EXPECTED UINT64_C(0x1020304050607080)
#define OPTIMIZED_LOCAL_EXPECTED UINT64_C(0x1e3c1e781e3c1ef0)

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

__attribute__((noinline)) uint64_t inspect_optimized_local(void) {
  uint64_t optimized_local = parameter_seed ^ UINT64_C(0x0f1e2d3c4b5a6978);
  __asm__ volatile("" : "+D"(optimized_local) : : "memory");
  __asm__ volatile(".globl optimized_local_probe\n"
                   "optimized_local_probe:\n"
                   "nop\n"
                   : "+a"(optimized_local)
                   :
                   : "memory");
  return optimized_local;
}

int main(void) {
  const uint64_t parameter = parameter_seed ^ UINT64_C(0x0102030405060708);
  if (inspect_parameter_value(parameter) != PARAMETER_EXPECTED) return 1;
  return inspect_optimized_local() == OPTIMIZED_LOCAL_EXPECTED ? 0 : 2;
}

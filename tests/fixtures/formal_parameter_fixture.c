#include <stdint.h>

#define PARAMETER_EXPECTED UINT64_C(0x1020304050607080)
#define ENTRY_PARAMETER_XOR UINT64_C(0x55aa00ff33cc6699)
#define ENTRY_RDI_SENTINEL UINT64_C(0x777788889999aaaa)
#define ENTRY_RESULT_EXPECTED UINT64_C(0x54a805fe32c96390)
#define OPTIMIZED_LOCAL_EXPECTED UINT64_C(0x1e3c1e781e3c1ef0)
#define ARITHMETIC_LOCAL_EXPECTED UINT64_C(0x10203040506070a5)
#define INDIRECT_LOCAL_EXPECTED UINT64_C(0x8877665544332211)
#define INLINE_LOCAL_XOR UINT64_C(0x123456789abcdef0)
#define INLINE_LOCAL_EXPECTED UINT64_C(0x02146638cadcae70)

volatile uint64_t parameter_seed = UINT64_C(0x1122334455667788);
uint64_t indirect_seed = INDIRECT_LOCAL_EXPECTED;

__attribute__((noinline)) uint64_t inspect_parameter_value(uint64_t parameter) {
  __asm__ volatile(".globl formal_parameter_probe\n"
                   "formal_parameter_probe:\n"
                   "nop\n"
                   : "+D"(parameter)
                   :
                   : "memory");
  return parameter;
}

__attribute__((noinline)) uint64_t clobber_argument_registers(
    uint64_t first, uint64_t second, uint64_t third,
    uint64_t fourth, uint64_t fifth, uint64_t sixth) {
  const uint64_t result = parameter_seed ^ first ^ (second << 8U) ^
                          (third << 16U) ^ (fourth << 24U) ^
                          (fifth << 32U) ^ (sixth << 40U);
  __asm__ volatile("movabsq $0x777788889999aaaa, %%rdi\n" ::: "rdi", "memory");
  return result;
}

__attribute__((noinline)) uint64_t inspect_entry_parameter(uint64_t entry_parameter) {
  uint64_t transformed = entry_parameter ^ ENTRY_PARAMETER_XOR;
  const uint64_t side_effect = clobber_argument_registers(1, 2, 3, 4, 5, 6);
  __asm__ volatile(".globl transformed_local_probe\n"
                   "transformed_local_probe:\n"
                   "nop\n"
                   ::: "memory");
  return transformed ^ side_effect;
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

__attribute__((noinline)) uint64_t inspect_arithmetic_local(
    uint64_t first, uint64_t second, uint64_t third,
    uint64_t fourth, uint64_t fifth, uint64_t sixth) {
  uint64_t arithmetic_local = first ^ (second << 8U) ^ (third << 16U) ^
                              (fourth << 24U) ^ (fifth << 32U) ^
                              (sixth << 40U);
  __asm__ volatile(".globl arithmetic_local_probe\n"
                   "arithmetic_local_probe:\n"
                   "nop\n"
                   ::: "memory");
  return arithmetic_local;
}

__attribute__((noinline)) uint64_t inspect_indirect_local(uint64_t* ptr) {
  uint64_t indirect_local = *ptr;
  __asm__ volatile(".globl indirect_local_probe\n"
                   "indirect_local_probe:\n"
                   "nop\n"
                   :
                   : "r"(ptr));
  return indirect_local;
}

static __attribute__((always_inline)) inline uint64_t inspect_inlined_local(
    uint64_t parameter) {
  volatile uint64_t inline_local = parameter ^ INLINE_LOCAL_XOR;
  __asm__ volatile(".globl inlined_local_probe\n"
                   "inlined_local_probe:\n"
                   "nop\n"
                   :
                   : "m"(inline_local)
                   : "memory");
  return inline_local;
}

int main(void) {
  const uint64_t parameter = parameter_seed ^ UINT64_C(0x0102030405060708);
  if (inspect_parameter_value(parameter) != PARAMETER_EXPECTED) return 1;
  if (inspect_entry_parameter(parameter) != ENTRY_RESULT_EXPECTED) return 2;
  if (inspect_optimized_local() != OPTIMIZED_LOCAL_EXPECTED) return 3;
  if (inspect_arithmetic_local(UINT64_C(0xa5), UINT64_C(0x70), UINT64_C(0x60),
                               UINT64_C(0x50), UINT64_C(0x40), UINT64_C(0x102030)) !=
      ARITHMETIC_LOCAL_EXPECTED) {
    return 4;
  }
  if (inspect_indirect_local(&indirect_seed) != INDIRECT_LOCAL_EXPECTED) return 5;
  return inspect_inlined_local(parameter) == INLINE_LOCAL_EXPECTED ? 0 : 6;
}

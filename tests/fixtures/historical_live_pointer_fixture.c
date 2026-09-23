#include <stdint.h>

int32_t historical_pointer_target = INT32_C(0x13579bdf);

__attribute__((noinline)) uint64_t historical_pointer_callee(uint64_t input) {
  __asm__ volatile(
      "movabs $0x1122334455667788, %%rbx\n"
      "movabs $0x2233445566778899, %%r12\n"
      "movabs $0x33445566778899aa, %%r13\n"
      "movabs $0x445566778899aabb, %%r14\n"
      "movabs $0x5566778899aabbcc, %%r15\n"
      ".globl historical_pointer_callee_probe\n"
      "historical_pointer_callee_probe:\n"
      "nop\n"
      :
      : "r"(input)
      : "rbx", "r12", "r13", "r14", "r15", "memory");
  return input + UINT64_C(1);
}

__attribute__((noinline)) int historical_pointer_caller(
    int32_t* historical_pointer) {
  const uint64_t side_effect = historical_pointer_callee(UINT64_C(7));
  __asm__ volatile(
      ".globl historical_pointer_after_probe\n"
      "historical_pointer_after_probe:\n"
      "nop\n"
      :
      : "r"(side_effect), "r"(historical_pointer)
      : "memory");
  return side_effect == UINT64_C(8) &&
                 *historical_pointer == INT32_C(0x13579bdf)
             ? 0
             : 1;
}

int main(void) {
  return historical_pointer_caller(&historical_pointer_target);
}

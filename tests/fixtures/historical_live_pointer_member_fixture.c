#include <stdint.h>

int32_t historical_linked_target = INT32_C(0x02468ace);

struct HistoricalLiveLinked {
  uint32_t direct;
  int32_t* linked;
};

struct HistoricalLiveLinked historical_linked_object = {
    UINT32_C(0x55667788), &historical_linked_target};

__attribute__((noinline)) uint64_t historical_linked_callee(uint64_t input) {
  __asm__ volatile(
      "movabs $0x1122334455667788, %%rbx\n"
      "movabs $0x2233445566778899, %%r12\n"
      "movabs $0x33445566778899aa, %%r13\n"
      "movabs $0x445566778899aabb, %%r14\n"
      "movabs $0x5566778899aabbcc, %%r15\n"
      ".globl historical_linked_callee_probe\n"
      "historical_linked_callee_probe:\n"
      "nop\n"
      :
      : "r"(input)
      : "rbx", "r12", "r13", "r14", "r15", "memory");
  return input + UINT64_C(1);
}

__attribute__((noinline)) int historical_linked_caller(
    struct HistoricalLiveLinked* historical_member_pointer) {
  const uint64_t side_effect = historical_linked_callee(UINT64_C(7));
  __asm__ volatile(
      ".globl historical_linked_after_probe\n"
      "historical_linked_after_probe:\n"
      "nop\n"
      :
      : "r"(side_effect), "r"(historical_member_pointer)
      : "memory");
  return side_effect == UINT64_C(8) &&
                 historical_member_pointer->direct == UINT32_C(0x55667788) &&
                 *historical_member_pointer->linked == INT32_C(0x02468ace)
             ? 0
             : 1;
}

int main(void) {
  return historical_linked_caller(&historical_linked_object);
}

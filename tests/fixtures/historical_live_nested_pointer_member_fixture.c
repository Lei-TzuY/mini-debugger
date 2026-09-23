#include <stdint.h>

struct HistoricalLiveLeaf {
  int32_t terminal;
  uint32_t marker;
};

struct HistoricalLiveNested {
  uint32_t direct;
  struct HistoricalLiveLeaf* linked;
};

struct HistoricalLiveLeaf historical_nested_leaf = {
    INT32_C(0x02468ace), UINT32_C(0x89abcdef)};

struct HistoricalLiveNested historical_nested_object = {
    UINT32_C(0x55667788), &historical_nested_leaf};

__attribute__((noinline)) uint64_t historical_nested_callee(uint64_t input) {
  __asm__ volatile(
      "movabs $0x1122334455667788, %%rbx\n"
      "movabs $0x2233445566778899, %%r12\n"
      "movabs $0x33445566778899aa, %%r13\n"
      "movabs $0x445566778899aabb, %%r14\n"
      "movabs $0x5566778899aabbcc, %%r15\n"
      ".globl historical_nested_callee_probe\n"
      "historical_nested_callee_probe:\n"
      "nop\n"
      :
      : "r"(input)
      : "rbx", "r12", "r13", "r14", "r15", "memory");
  return input + UINT64_C(1);
}

__attribute__((noinline)) int historical_nested_caller(
    struct HistoricalLiveNested* historical_nested_pointer) {
  const uint64_t side_effect = historical_nested_callee(UINT64_C(7));
  __asm__ volatile(
      ".globl historical_nested_after_probe\n"
      "historical_nested_after_probe:\n"
      "nop\n"
      :
      : "r"(side_effect), "r"(historical_nested_pointer)
      : "memory");
  return side_effect == UINT64_C(8) &&
                 historical_nested_pointer->direct == UINT32_C(0x55667788) &&
                 historical_nested_pointer->linked->terminal ==
                     INT32_C(0x02468ace) &&
                 historical_nested_pointer->linked->marker ==
                     UINT32_C(0x89abcdef)
             ? 0
             : 1;
}

int main(void) {
  return historical_nested_caller(&historical_nested_object);
}

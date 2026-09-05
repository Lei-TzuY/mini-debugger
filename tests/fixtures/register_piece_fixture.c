#include <stdint.h>

struct RegisterPair {
  uint64_t first;
  uint64_t second;
};

#define REGISTER_PAIR_FIRST UINT64_C(0x1122334455667788)
#define REGISTER_PAIR_SECOND UINT64_C(0x99aabbccddeeff00)

volatile uint64_t register_pair_sink;

__attribute__((noinline)) uint64_t inspect_register_pair(struct RegisterPair pair) {
  __asm__ volatile(".globl register_pair_probe\n"
                   "register_pair_probe:\n"
                   "nop\n"
                   :
                   : "r"(pair.first), "r"(pair.second)
                   : "memory");
  register_pair_sink = pair.first;
  return pair.first ^ pair.second;
}

int main(void) {
  const struct RegisterPair pair = {REGISTER_PAIR_FIRST, REGISTER_PAIR_SECOND};
  return inspect_register_pair(pair) == (REGISTER_PAIR_FIRST ^ REGISTER_PAIR_SECOND) ? 0 : 1;
}

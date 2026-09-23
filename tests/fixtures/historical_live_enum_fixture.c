#include <stdint.h>

enum HistoricalLiveMode {
  HistoricalIdle = 3,
  HistoricalReady = 7,
  HistoricalBusy = 42,
};

volatile uint32_t historical_mode_seed = UINT32_C(42);

__attribute__((noinline)) uint64_t historical_enum_callee(uint64_t input) {
  __asm__ volatile(".globl historical_enum_callee_probe\n"
                   "historical_enum_callee_probe:\n"
                   "nop\n"
                   :
                   : "r"(input)
                   : "memory");
  return input + UINT64_C(1);
}

__attribute__((noinline)) int historical_enum_caller(void) {
  enum HistoricalLiveMode historical_mode =
      (enum HistoricalLiveMode)historical_mode_seed;
  const uint64_t side_effect = historical_enum_callee(UINT64_C(7));
  __asm__ volatile(".globl historical_enum_after_probe\n"
                   "historical_enum_after_probe:\n"
                   "nop\n"
                   :
                   : "r"(side_effect)
                   : "memory");
  return side_effect == UINT64_C(8) &&
                 historical_mode == HistoricalBusy
             ? 0
             : 1;
}

int main(void) { return historical_enum_caller(); }

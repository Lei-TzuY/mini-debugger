#define _GNU_SOURCE

#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>

struct SignalRegisterPair {
  uint64_t first;
  uint64_t second;
};

#define SIGNAL_PAIR_FIRST UINT64_C(0x1122334455667788)
#define SIGNAL_PAIR_SECOND UINT64_C(0x99aabbccddeeff00)
#define SIGNAL_CRASH_RDI UINT64_C(0x0badf00d11223344)
#define SIGNAL_CRASH_RSI UINT64_C(0x55667788cafef00d)

volatile uint64_t signal_piece_seed_first = SIGNAL_PAIR_FIRST;
volatile uint64_t signal_piece_seed_second = SIGNAL_PAIR_SECOND;
volatile uint64_t signal_piece_sink = 0;
volatile uintptr_t signal_piece_ucontext_address = 0;

__attribute__((noinline, noreturn)) void signal_piece_crash_from_handler(void) {
  __asm__ volatile(
      ".globl signal_piece_crash_probe\n"
      "signal_piece_crash_probe:\n"
      "movl $0, (%%rax)\n"
      :
      : "a"(0), "D"(SIGNAL_CRASH_RDI), "S"(SIGNAL_CRASH_RSI)
      : "memory");
  __builtin_unreachable();
}

static void signal_piece_handler(int signal_number, siginfo_t* info,
                                 void* raw_context) {
  (void)signal_number;
  (void)info;
  signal_piece_ucontext_address = (uintptr_t)raw_context;
  signal_piece_crash_from_handler();
}

__attribute__((noinline)) void signal_piece_interrupted_application(
    struct SignalRegisterPair interrupted_pair) {
  __asm__ volatile(
      ".globl signal_piece_interrupted_probe\n"
      "signal_piece_interrupted_probe:\n"
      "ud2\n"
      :
      : "D"(interrupted_pair.first), "S"(interrupted_pair.second)
      : "memory");
  signal_piece_sink = interrupted_pair.first ^ interrupted_pair.second;
}

int main(void) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = signal_piece_handler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGILL, &action, NULL) != 0) return 2;

  const struct SignalRegisterPair interrupted_pair = {
      signal_piece_seed_first, signal_piece_seed_second};
  signal_piece_interrupted_application(interrupted_pair);
  return signal_piece_sink == (SIGNAL_PAIR_FIRST ^ SIGNAL_PAIR_SECOND) ? 0 : 3;
}

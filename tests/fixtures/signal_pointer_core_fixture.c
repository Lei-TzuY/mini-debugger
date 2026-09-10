#define _GNU_SOURCE

#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>

#define SIGNAL_POINTER_TARGET_VALUE UINT64_C(0x7a6b5c4d3e2f1908)
#define SIGNAL_POINTER_CRASH_RDI UINT64_C(0x0ddc0ffeebadf00d)

uint64_t signal_pointer_target = 0;
volatile uint64_t signal_pointer_sink = 0;
volatile uintptr_t signal_pointer_ucontext_address = 0;

__attribute__((noinline)) void signal_pointer_crash_from_handler(void) {
  __asm__ volatile(
      ".globl signal_pointer_crash_probe\n"
      "signal_pointer_crash_probe:\n"
      "movl $0, (%%rax)\n"
      :
      : "a"(0), "D"(SIGNAL_POINTER_CRASH_RDI)
      : "memory");
}

__attribute__((noinline)) static void signal_pointer_handler(int signal_number,
                                                             siginfo_t* info,
                                                             void* raw_context) {
  (void)signal_number;
  (void)info;
  signal_pointer_ucontext_address = (uintptr_t)raw_context;
  signal_pointer_crash_from_handler();
  __asm__ volatile("" ::: "memory");
}

__attribute__((noinline)) void signal_pointer_interrupted_application(
    uint64_t* interrupted_pointer) {
  __asm__ volatile(
      ".globl signal_pointer_interrupted_probe\n"
      "signal_pointer_interrupted_probe:\n"
      "ud2\n"
      : "+D"(interrupted_pointer)
      :
      : "memory");
  signal_pointer_sink = *interrupted_pointer;
}

int main(void) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = signal_pointer_handler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGILL, &action, NULL) != 0) return 2;

  signal_pointer_target = SIGNAL_POINTER_TARGET_VALUE;
  signal_pointer_interrupted_application(&signal_pointer_target);
  return signal_pointer_sink == SIGNAL_POINTER_TARGET_VALUE ? 0 : 3;
}

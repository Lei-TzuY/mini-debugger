#define _GNU_SOURCE

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

struct SignalRegisterPair {
  uint64_t first;
  uint64_t second;
};

#define SIGNAL_PAIR_FIRST UINT64_C(0x1122334455667788)
#define SIGNAL_PAIR_SECOND UINT64_C(0x99aabbccddeeff00)

volatile uintptr_t signal_core_ucontext_address = 0;
volatile uintptr_t signal_core_saved_rip = 0;
volatile uintptr_t signal_core_saved_rsp = 0;
volatile uintptr_t signal_core_saved_rbp = 0;
volatile uintptr_t signal_core_saved_rbx = 0;
volatile uintptr_t signal_core_saved_r12 = 0;
volatile uintptr_t signal_core_saved_fpstate = 0;
volatile uint64_t signal_core_saved_xmm0_low = 0;
volatile uint64_t signal_core_value_seed = UINT64_C(0x1020304050607080);
volatile double signal_core_fp_seed = 1234.5;
static const double signal_core_handler_fp_marker = -4321.25;
volatile sig_atomic_t signal_core_interrupted_ready = 0;
volatile sig_atomic_t signal_core_main_tid = 0;

__attribute__((noinline)) void signal_core_crash_from_handler(void) {
  __asm__ volatile(
      ".globl signal_core_crash_probe\n"
      "signal_core_crash_probe:\n"
      "movl $0, (%%rax)\n"
      :
      : "a"(0)
      : "memory");
}

__attribute__((noinline)) void signal_core_handler(int signal_number, siginfo_t* info,
                                                   void* raw_context) {
  (void)signal_number;
  (void)info;
  ucontext_t* context = (ucontext_t*)raw_context;
  signal_core_ucontext_address = (uintptr_t)raw_context;
  signal_core_saved_rip = (uintptr_t)context->uc_mcontext.gregs[REG_RIP];
  signal_core_saved_rsp = (uintptr_t)context->uc_mcontext.gregs[REG_RSP];
  signal_core_saved_rbp = (uintptr_t)context->uc_mcontext.gregs[REG_RBP];
  signal_core_saved_rbx = (uintptr_t)context->uc_mcontext.gregs[REG_RBX];
  signal_core_saved_r12 = (uintptr_t)context->uc_mcontext.gregs[REG_R12];
  signal_core_saved_fpstate = (uintptr_t)context->uc_mcontext.fpregs;
  if (context->uc_mcontext.fpregs == NULL) _Exit(91);
  uint64_t saved_xmm0 = 0;
  memcpy(&saved_xmm0, &context->uc_mcontext.fpregs->_xmm[0], sizeof(saved_xmm0));
  signal_core_saved_xmm0_low = saved_xmm0;
  __asm__ volatile("movsd %0, %%xmm0"
                   :
                   : "m"(signal_core_handler_fp_marker)
                   : "xmm0");
  __asm__ volatile(".globl signal_core_handler_probe\n"
                   "signal_core_handler_probe:\n"
                   ::: "memory");
  signal_core_crash_from_handler();
  __asm__ volatile("" ::: "memory");
}

static void* signal_sender(void* argument) {
  (void)argument;
  while (!signal_core_interrupted_ready) {
    __asm__ volatile("pause" ::: "memory");
  }
  if (syscall(SYS_tgkill, getpid(), (pid_t)signal_core_main_tid, SIGUSR1) != 0) {
    _Exit(90);
  }
  return NULL;
}

__attribute__((noinline, noreturn)) void signal_core_interrupted_application(
    struct SignalRegisterPair interrupted_pair) {
  register uint64_t interrupted_register_local __asm__("r12") =
      signal_core_value_seed ^ UINT64_C(0xa5a55a5ac3c33c3c);
  double interrupted_fp_local = signal_core_fp_seed;
  signal_core_interrupted_ready = 1;
  for (;;) {
    __asm__ volatile(
        ".globl signal_core_interrupted_probe\n"
        "signal_core_interrupted_probe:\n"
        "pause\n"
        : "+D"(interrupted_pair.first), "+S"(interrupted_pair.second),
          "+r"(interrupted_register_local), "+x"(interrupted_fp_local)
        :
        : "memory");
  }
}

int main(void) {
  signal_core_main_tid = (sig_atomic_t)syscall(SYS_gettid);

  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = signal_core_handler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGUSR1, &action, NULL) != 0) return 2;

  pthread_t sender;
  if (pthread_create(&sender, NULL, signal_sender, NULL) != 0) return 3;
  const struct SignalRegisterPair interrupted_pair = {SIGNAL_PAIR_FIRST,
                                                       SIGNAL_PAIR_SECOND};
  signal_core_interrupted_application(interrupted_pair);
}

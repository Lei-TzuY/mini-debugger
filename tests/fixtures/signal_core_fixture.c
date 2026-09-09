#define _GNU_SOURCE

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

volatile uintptr_t signal_core_ucontext_address = 0;
volatile uintptr_t signal_core_saved_rip = 0;
volatile uintptr_t signal_core_saved_rsp = 0;
volatile sig_atomic_t signal_core_interrupted_ready = 0;
volatile sig_atomic_t signal_core_main_tid = 0;

__attribute__((noinline, noreturn)) void signal_core_crash_from_handler(void) {
  __asm__ volatile(
      ".globl signal_core_crash_probe\n"
      "signal_core_crash_probe:\n"
      "movl $0, (%%rax)\n"
      :
      : "a"(0)
      : "memory");
  __builtin_unreachable();
}

__attribute__((noinline)) void signal_core_handler(int signal_number, siginfo_t* info,
                                                   void* raw_context) {
  (void)signal_number;
  (void)info;
  ucontext_t* context = (ucontext_t*)raw_context;
  signal_core_ucontext_address = (uintptr_t)raw_context;
  signal_core_saved_rip = (uintptr_t)context->uc_mcontext.gregs[REG_RIP];
  signal_core_saved_rsp = (uintptr_t)context->uc_mcontext.gregs[REG_RSP];
  __asm__ volatile(".globl signal_core_handler_probe\n"
                   "signal_core_handler_probe:\n"
                   ::: "memory");
  signal_core_crash_from_handler();
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

__attribute__((noinline, noreturn)) void signal_core_interrupted_application(void) {
  signal_core_interrupted_ready = 1;
  __asm__ volatile(
      ".globl signal_core_interrupted_probe\n"
      "signal_core_interrupted_probe:\n"
      "pause\n"
      "jmp signal_core_interrupted_probe\n"
      ".globl signal_core_interrupted_probe_end\n"
      "signal_core_interrupted_probe_end:\n"
      :
      :
      : "memory");
  __builtin_unreachable();
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
  signal_core_interrupted_application();
}

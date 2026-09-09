#define _GNU_SOURCE

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static volatile sig_atomic_t sibling_ready = 0;
static volatile sig_atomic_t sibling_tid = 0;

__attribute__((noinline, noreturn)) static void sibling_hold_xmm(double seed) {
  double xmm_value = seed;
  __asm__ volatile("" : "+x"(xmm_value));
  sibling_ready = 1;
  __asm__ volatile(
      ".globl snapshot_xmm_sibling_probe\n"
      "snapshot_xmm_sibling_probe:\n"
      "pause\n"
      "jmp snapshot_xmm_sibling_probe\n"
      : "+x"(xmm_value)
      :
      : "memory");
  __builtin_unreachable();
}

static void* sibling_main(void* argument) {
  (void)argument;
  sibling_tid = (sig_atomic_t)syscall(SYS_gettid);
  sibling_hold_xmm(9876.5);
}

__attribute__((noinline, noreturn)) static void crash_with_xmm(void) {
  struct AggregatePointee {
    uint64_t first;
    uint64_t second;
  };
  static uint64_t pointee_value = UINT64_C(0x8877665544332211);
  static uint64_t* scalar_pointer = &pointee_value;
  static struct AggregatePointee aggregate_value = {
      UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)};
  static struct AggregatePointee* aggregate_pointer = &aggregate_value;
  uint64_t stack_local = UINT64_C(0x4f3e2d1c0b9a8877);
  double xmm_value = 1234.25;
  __asm__ volatile("" : "+m"(stack_local), "+x"(xmm_value)
                   : "m"(scalar_pointer), "m"(pointee_value),
                     "m"(aggregate_pointer), "m"(aggregate_value)
                   : "memory");
  __asm__ volatile(
      ".globl snapshot_xmm_crash_probe\n"
      "snapshot_xmm_crash_probe:\n"
      "movl $0, (%%rax)\n"
      : "+x"(xmm_value)
      : "a"(0), "m"(stack_local), "m"(scalar_pointer), "m"(pointee_value),
        "m"(aggregate_pointer), "m"(aggregate_value)
      : "memory");
  __builtin_unreachable();
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;

  pthread_t thread;
  if (pthread_create(&thread, NULL, sibling_main, NULL) != 0) return 3;
  while (!sibling_ready) {
    __asm__ volatile("pause" ::: "memory");
  }

  FILE* ready = fopen(argv[1], "w");
  if (ready == NULL) return 4;
  fprintf(ready, "%d\n", (int)sibling_tid);
  if (fclose(ready) != 0) return 5;

  crash_with_xmm();
}

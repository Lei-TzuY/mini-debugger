#include "caller_inline_core_fixture.h"

#include <pthread.h>

int caller_inline_pointee = 0x02468ace;
int caller_inline_member_pointee = 0x13579bdf;
struct CallerInlineAggregate caller_inline_aggregate = {
    0x11223344, &caller_inline_member_pointee};
static volatile int caller_inline_sibling_hold = 1;

static void* caller_inline_sibling(void* argument) {
  (void)argument;
  while (caller_inline_sibling_hold) {
    __asm__ volatile("" ::: "memory");
  }
  return 0;
}

__attribute__((noinline)) static int caller_physical_frame(int seed) {
  int physical_guard = seed;
  return caller_inline_outer(physical_guard);
}

int main(int argc, char** argv) {
  (void)argv;
  pthread_t sibling;
  if (pthread_create(&sibling, 0, caller_inline_sibling, 0) != 0) return 120;
  return caller_physical_frame(argc + 20);
}

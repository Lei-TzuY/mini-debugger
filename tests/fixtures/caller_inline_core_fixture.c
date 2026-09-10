#include "caller_inline_core_fixture.h"

__attribute__((noinline)) static int caller_physical_frame(int seed) {
  int physical_guard = seed;
  return caller_inline_outer(physical_guard);
}

int main(int argc, char** argv) {
  (void)argv;
  return caller_physical_frame(argc + 20);
}
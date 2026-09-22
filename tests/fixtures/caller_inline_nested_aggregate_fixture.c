#include "caller_inline_nested_aggregate_fixture.h"

__attribute__((noinline)) static int caller_nested_physical_frame(int seed) {
  int physical_guard = seed;
  return caller_nested_inline_outer(physical_guard);
}

int main(int argc, char** argv) {
  (void)argv;
  return caller_nested_physical_frame(argc + 30);
}

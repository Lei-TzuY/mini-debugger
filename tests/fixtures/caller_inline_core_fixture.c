#include "caller_inline_core_fixture.h"

__attribute__((noinline)) static int caller_physical_frame(void) {
  int physical_guard = 21;
  return caller_inline_outer(physical_guard);
}

int main(void) { return caller_physical_frame(); }

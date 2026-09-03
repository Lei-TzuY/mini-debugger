#include <signal.h>

extern volatile int shared_cfi_marker;
extern void shared_outer(void);

int main(void) {
  if (raise(SIGSTOP) != 0) return 2;
  shared_outer();
  return shared_cfi_marker == 1 ? 0 : 1;
}

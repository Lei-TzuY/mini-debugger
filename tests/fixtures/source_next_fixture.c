volatile int next_marker = 0;

__attribute__((noinline)) void next_callee(void) {
#line 510 "next_source.c"
  next_marker += 1;
#line 511 "next_source.c"
  next_marker += 0;
}

__attribute__((noinline)) void next_caller(void) {
#line 500 "next_source.c"
  next_callee();
#line 501 "next_source.c"
  next_marker += 2;
}

#line 1 "next_driver.c"
int main(void) {
  next_caller();
  return next_marker == 3 ? 0 : 1;
}

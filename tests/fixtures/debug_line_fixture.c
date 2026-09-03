#line 400 "mapped_source.c"
__attribute__((noinline)) void line_probe(void) {
  __asm__ volatile("nop" ::: "memory");
}

#line 1 "debug_line_driver.c"
int main(void) {
  line_probe();
  return 0;
}

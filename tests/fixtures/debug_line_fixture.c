#line 400 "mapped_source.c"
__attribute__((noinline)) void line_probe(void) {
#line 401 "mapped_source.c"
  __asm__ volatile("nop\n\tnop\n\tnop" ::: "memory");
#line 402 "mapped_source.c"
  __asm__ volatile("nop" ::: "memory");
}

#line 1 "debug_line_driver.c"
int main(void) {
  line_probe();
  return 0;
}

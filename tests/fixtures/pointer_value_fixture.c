#include <stdint.h>

volatile uint64_t pointer_target = UINT64_C(0x8877665544332211);

struct LocalPair {
  uint32_t count;
  int64_t delta;
};

__attribute__((noinline)) int inspect_local_pointer(void) {
  uint64_t* local_pointer = (uint64_t*)&pointer_target;
  __asm__ volatile(".globl local_pointer_probe\n"
                   "local_pointer_probe:\n"
                   "nop"
                   : "+m"(local_pointer)
                   :
                   : "memory");
  return local_pointer == (uint64_t*)&pointer_target &&
                 *local_pointer == UINT64_C(0x8877665544332211)
             ? 0
             : 1;
}

__attribute__((noinline)) int inspect_local_struct(void) {
  struct LocalPair local_struct = {UINT32_C(0x11223344), INT64_C(-123456789)};
  __asm__ volatile(".globl local_struct_probe\n"
                   "local_struct_probe:\n"
                   "nop"
                   : "+m"(local_struct)
                   :
                   : "memory");
  return local_struct.count == UINT32_C(0x11223344) &&
                 local_struct.delta == INT64_C(-123456789)
             ? 0
             : 2;
}

int main(void) {
  const int pointer_result = inspect_local_pointer();
  if (pointer_result != 0) return pointer_result;
  return inspect_local_struct();
}

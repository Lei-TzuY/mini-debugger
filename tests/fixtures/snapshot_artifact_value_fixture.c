#include <stdint.h>
#include <stdio.h>

#define ARTIFACT_EXPECTED UINT64_C(0x6a5b4c3d2e1f9081)
#define ARTIFACT_XOR UINT64_C(0x13579bdf2468ace0)

static const uint64_t artifact_seed = ARTIFACT_EXPECTED ^ ARTIFACT_XOR;

__attribute__((noinline)) static uint64_t inspect_artifact_local(const uint64_t* ptr) {
  const uint64_t artifact_local = *ptr ^ ARTIFACT_XOR;
  __asm__ volatile("" : : "D"(ptr));
  __asm__ volatile("xorq %%rax, %%rax\n"
                   "movq %%rax, (%%rax)\n"
                   ::: "rax");
  return artifact_local;
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  FILE* ready = fopen(argv[1], "w");
  if (ready == NULL) return 3;
  fprintf(ready, "%p\n", (const void*)&artifact_seed);
  if (fclose(ready) != 0) return 4;
  (void)inspect_artifact_local(&artifact_seed);
  return 5;
}

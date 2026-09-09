#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PARAMETER_EXPECTED UINT64_C(0x1020304050607080)
#define ENTRY_PARAMETER_XOR UINT64_C(0x55aa00ff33cc6699)
#define ENTRY_RDI_SENTINEL UINT64_C(0x777788889999aaaa)
#define ENTRY_RBX_SENTINEL UINT64_C(0x0badf00dfeedface)
#define ENTRY_RESULT_EXPECTED UINT64_C(0x54a805fe32c96390)
#define OPTIMIZED_LOCAL_EXPECTED UINT64_C(0x1e3c1e781e3c1ef0)
#define ARITHMETIC_LOCAL_EXPECTED UINT64_C(0x10203040506070a5)
#define INDIRECT_LOCAL_EXPECTED UINT64_C(0x8877665544332211)
#define INDIRECT_LOCAL_XOR UINT64_C(0x55aa00ff33cc6699)
#define INLINE_LOCAL_EXPECTED UINT64_C(0x02146638cadcae70)
#define SNAPSHOT_FILE_SCALAR_EXPECTED UINT64_C(0x6a09e667f3bcc909)
#define SNAPSHOT_AGGREGATE_FIRST UINT64_C(0xbb67ae8584caa73b)
#define SNAPSHOT_AGGREGATE_SECOND UINT64_C(0x3c6ef372fe94f82b)

struct SnapshotFileAggregate {
  uint64_t first;
  uint64_t second;
};

static const uint64_t snapshot_crash_xmm15[2] = {
    UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)};
static const uint64_t snapshot_sibling_xmm15[2] = {
    UINT64_C(0x0f1e2d3c4b5a6978), UINT64_C(0x8877665544332211)};

volatile uint64_t parameter_seed = UINT64_C(0x1122334455667788);
volatile uint64_t inline_seed = INLINE_LOCAL_EXPECTED;
uint64_t indirect_seed = INDIRECT_LOCAL_EXPECTED ^ INDIRECT_LOCAL_XOR;
uint64_t* indirect_ptr = &indirect_seed;
static volatile int snapshot_crash_enabled = 0;
static volatile int snapshot_sibling_ready = 0;

static void* snapshot_sibling_worker(void* argument) {
  const char* ready_path = (const char*)argument;
  FILE* ready = fopen(ready_path, "w");
  if (ready == NULL) return (void*)(uintptr_t)1;
  fprintf(ready, "%ld\n", (long)syscall(SYS_gettid));
  if (fclose(ready) != 0) return (void*)(uintptr_t)1;
  snapshot_sibling_ready = 1;
  __asm__ volatile("movdqu %0, %%xmm15\n"
                   "movq $34, %%rax\n"
                   "1:\n"
                   "syscall\n"
                   "jmp 1b\n"
                   :
                   : "m"(snapshot_sibling_xmm15)
                   : "rax", "rcx", "r11", "xmm15", "memory");
  __builtin_unreachable();
}

static int start_snapshot_sibling(const char* ready_path) {
  pthread_t thread;
  snapshot_sibling_ready = 0;
  if (pthread_create(&thread, NULL, snapshot_sibling_worker, (void*)ready_path) != 0) {
    return 0;
  }
  while (!snapshot_sibling_ready) usleep(1000);
  return 1;
}

__attribute__((noinline)) uint64_t inspect_parameter_value(uint64_t parameter) {
  __asm__ volatile(".globl formal_parameter_probe\n"
                   "formal_parameter_probe:\n"
                   "nop\n"
                   : "+D"(parameter)
                   :
                   : "memory");
  return parameter;
}

__attribute__((noinline)) uint64_t clobber_argument_registers(
    uint64_t first, uint64_t second, uint64_t third,
    uint64_t fourth, uint64_t fifth, uint64_t sixth) {
  const uint64_t result = parameter_seed ^ first ^ (second << 8U) ^
                          (third << 16U) ^ (fourth << 24U) ^
                          (fifth << 32U) ^ (sixth << 40U);
  __asm__ volatile("movabsq $0x777788889999aaaa, %%rdi\n"
                   "movabsq $0x0badf00dfeedface, %%rbx\n"
                   ".globl caller_register_probe\n"
                   "caller_register_probe:\n"
                   "nop\n"
                   ::: "rdi", "rbx", "memory");
  if (snapshot_crash_enabled) {
    __asm__ volatile("movdqu %0, %%xmm15\n"
                     "xorq %%rax, %%rax\n"
                     "movq %%rax, (%%rax)\n"
                     :
                     : "m"(snapshot_crash_xmm15)
                     : "rax", "xmm15", "memory");
  }
  return result;
}

__attribute__((noinline)) uint64_t inspect_entry_parameter(uint64_t entry_parameter) {
  static const uint64_t snapshot_file_scalar = SNAPSHOT_FILE_SCALAR_EXPECTED;
  static const struct SnapshotFileAggregate snapshot_file_aggregate = {
      SNAPSHOT_AGGREGATE_FIRST, SNAPSHOT_AGGREGATE_SECOND};
  uint64_t transformed = entry_parameter ^ ENTRY_PARAMETER_XOR;
  __asm__ volatile("" : : "m"(snapshot_file_scalar), "m"(snapshot_file_aggregate) : "memory");
  const uint64_t side_effect = clobber_argument_registers(1, 2, 3, 4, 5, 6);
  __asm__ volatile("nop" ::: "memory");
  __asm__ volatile(".globl transformed_local_probe\n"
                   "transformed_local_probe:\n"
                   "nop\n"
                   ::: "memory");
  return transformed ^ side_effect;
}

__attribute__((noinline)) uint64_t inspect_optimized_local(void) {
  uint64_t optimized_local = parameter_seed ^ UINT64_C(0x0f1e2d3c4b5a6978);
  __asm__ volatile("" : "+D"(optimized_local) : : "memory");
  __asm__ volatile(".globl optimized_local_probe\n"
                   "optimized_local_probe:\n"
                   "nop\n"
                   : "+a"(optimized_local)
                   :
                   : "memory");
  return optimized_local;
}

__attribute__((noinline)) uint64_t inspect_arithmetic_local(
    uint64_t first, uint64_t second, uint64_t third,
    uint64_t fourth, uint64_t fifth, uint64_t sixth) {
  uint64_t arithmetic_local = first ^ (second << 8U) ^ (third << 16U) ^
                              (fourth << 24U) ^ (fifth << 32U) ^
                              (sixth << 40U);
  __asm__ volatile(".globl arithmetic_local_probe\n"
                   "arithmetic_local_probe:\n"
                   "nop\n"
                   ::: "memory");
  return arithmetic_local;
}

__attribute__((noinline)) uint64_t inspect_indirect_local(uint64_t** ptr) {
  const uint64_t indirect_local = **ptr ^ INDIRECT_LOCAL_XOR;
  __asm__ volatile(".globl indirect_local_probe\n"
                   "indirect_local_probe:\n"
                   "nop\n"
                   :
                   : "r"(ptr));
  return indirect_local;
}

static __attribute__((always_inline)) inline uint64_t inspect_inlined_local(
    uint64_t parameter) {
  uint64_t inline_local = parameter;
  __asm__ volatile(".globl inlined_local_probe\n"
                   "inlined_local_probe:\n"
                   "nop\n"
                   : "+D"(inline_local)
                   :
                   : "memory");
  return inline_local;
}

int main(int argc, char** argv) {
  const int threaded_snapshot =
      argc == 3 && strcmp(argv[1], "--snapshot-crash-threaded") == 0;
  snapshot_crash_enabled =
      (argc == 2 && strcmp(argv[1], "--snapshot-crash") == 0) || threaded_snapshot;
  if (threaded_snapshot && !start_snapshot_sibling(argv[2])) return 7;

  const uint64_t parameter = parameter_seed ^ UINT64_C(0x0102030405060708);
  if (inspect_parameter_value(parameter) != PARAMETER_EXPECTED) return 1;
  if (inspect_entry_parameter(parameter) != ENTRY_RESULT_EXPECTED) return 2;
  if (inspect_optimized_local() != OPTIMIZED_LOCAL_EXPECTED) return 3;
  if (inspect_arithmetic_local(UINT64_C(0xa5), UINT64_C(0x70), UINT64_C(0x60),
                               UINT64_C(0x50), UINT64_C(0x40), UINT64_C(0x102030)) !=
      ARITHMETIC_LOCAL_EXPECTED) {
    return 4;
  }
  if (inspect_indirect_local(&indirect_ptr) != INDIRECT_LOCAL_EXPECTED) return 5;
  return inspect_inlined_local(inline_seed) == INLINE_LOCAL_EXPECTED ? 0 : 6;
}

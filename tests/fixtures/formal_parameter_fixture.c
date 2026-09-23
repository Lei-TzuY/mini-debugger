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
#define LIVE_ARRAY_FIRST INT32_C(0x10203040)
#define LIVE_ARRAY_SECOND INT32_C(0x22334455)
#define LIVE_ARRAY_THIRD INT32_C(0x33445566)
#define LIVE_UNION_VALUE INT32_C(0x44556677)
#define LIVE_BIT_SIGNED_VALUE (-7)
#define LIVE_BIT_UNSIGNED_VALUE UINT32_C(41)
#define LIVE_ENUM_AGGREGATE_DIRECT INT32_C(0x31415926)
#define LIVE_POINTER_DIRECT INT32_C(0x11223344)
#define LIVE_POINTER_TARGET INT32_C(0x02468ace)

enum LiveMode {
  LiveIdle = 3,
  LiveReady = 7,
  LiveBusy = 42,
};

union LiveUnion {
  int32_t signed_value;
  uint32_t unsigned_value;
};

struct LiveBitFields {
  signed int signed_bits : 5;
  unsigned int unsigned_bits : 6;
};

struct LiveEnumAggregate {
  int32_t direct;
  enum LiveMode mode;
};

struct LivePointerAggregate {
  int32_t direct;
  int32_t* linked;
};

struct SnapshotFileAggregate {
  uint64_t first;
  uint64_t second;
};

static const uint64_t snapshot_crash_xmm15[2] = {
    UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)};
static const uint64_t snapshot_sibling_xmm15[2] = {
    UINT64_C(0x0f1e2d3c4b5a6978), UINT64_C(0x8877665544332211)};

volatile uint64_t parameter_seed = UINT64_C(0x1122334455667788);
volatile uint32_t live_enum_seed = UINT32_C(42);
volatile uint64_t inline_seed = INLINE_LOCAL_EXPECTED;
uint64_t indirect_seed = INDIRECT_LOCAL_EXPECTED ^ INDIRECT_LOCAL_XOR;
uint64_t* indirect_ptr = &indirect_seed;
int32_t live_pointer_target = LIVE_POINTER_TARGET;
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

__attribute__((noinline)) enum LiveMode inspect_live_enum(void) {
  enum LiveMode live_mode = (enum LiveMode)live_enum_seed;
  __asm__ volatile("" : "+D"(live_mode) : : "memory");
  __asm__ volatile(".globl live_enum_probe\n"
                   "live_enum_probe:\n"
                   "nop\n"
                   : "+a"(live_mode)
                   :
                   : "memory");
  return live_mode;
}

__attribute__((noinline)) int32_t inspect_live_array(void) {
  int32_t live_array[3] = {
      LIVE_ARRAY_FIRST, LIVE_ARRAY_SECOND, LIVE_ARRAY_THIRD};
  __asm__ volatile("" : "+m"(live_array) : : "memory");
  __asm__ volatile(".globl live_array_probe\n"
                   "live_array_probe:\n"
                   "nop\n"
                   : "+m"(live_array)
                   :
                   : "memory");
  return live_array[0] ^ live_array[1] ^ live_array[2];
}

__attribute__((noinline)) int32_t inspect_live_union(void) {
  union LiveUnion live_union = {.signed_value = LIVE_UNION_VALUE};
  __asm__ volatile("" : "+m"(live_union) : : "memory");
  __asm__ volatile(".globl live_union_probe\n"
                   "live_union_probe:\n"
                   "nop\n"
                   : "+m"(live_union)
                   :
                   : "memory");
  return live_union.signed_value;
}

__attribute__((noinline)) int32_t inspect_live_bit_fields(void) {
  struct LiveBitFields live_bit_fields = {
      .signed_bits = LIVE_BIT_SIGNED_VALUE,
      .unsigned_bits = LIVE_BIT_UNSIGNED_VALUE};
  __asm__ volatile("" : "+m"(live_bit_fields) : : "memory");
  __asm__ volatile(".globl live_bit_field_probe\n"
                   "live_bit_field_probe:\n"
                   "nop\n"
                   : "+m"(live_bit_fields)
                   :
                   : "memory");
  return live_bit_fields.signed_bits + (int32_t)live_bit_fields.unsigned_bits;
}

__attribute__((noinline)) int32_t inspect_live_enum_aggregate(void) {
  struct LiveEnumAggregate live_enum_aggregate = {
      .direct = LIVE_ENUM_AGGREGATE_DIRECT, .mode = LiveBusy};
  __asm__ volatile("" : "+m"(live_enum_aggregate) : : "memory");
  __asm__ volatile(".globl live_enum_aggregate_probe\n"
                   "live_enum_aggregate_probe:\n"
                   "nop\n"
                   : "+m"(live_enum_aggregate)
                   :
                   : "memory");
  return live_enum_aggregate.direct ^ (int32_t)live_enum_aggregate.mode;
}

__attribute__((noinline)) int32_t inspect_live_pointer_aggregate(void) {
  struct LivePointerAggregate live_pointer_aggregate = {
      .direct = LIVE_POINTER_DIRECT, .linked = &live_pointer_target};
  __asm__ volatile("" : "+m"(live_pointer_aggregate) : : "memory");
  __asm__ volatile(".globl live_pointer_aggregate_probe\n"
                   "live_pointer_aggregate_probe:\n"
                   "nop\n"
                   : "+m"(live_pointer_aggregate)
                   :
                   : "memory");
  return live_pointer_aggregate.direct ^ *live_pointer_aggregate.linked;
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
  if (inspect_live_enum() != LiveBusy) return 8;
  if (inspect_live_array() !=
      (LIVE_ARRAY_FIRST ^ LIVE_ARRAY_SECOND ^ LIVE_ARRAY_THIRD)) {
    return 9;
  }
  if (inspect_live_union() != LIVE_UNION_VALUE) return 10;
  if (inspect_live_bit_fields() !=
      LIVE_BIT_SIGNED_VALUE + (int32_t)LIVE_BIT_UNSIGNED_VALUE) {
    return 11;
  }
  if (inspect_live_enum_aggregate() !=
      (LIVE_ENUM_AGGREGATE_DIRECT ^ (int32_t)LiveBusy)) {
    return 12;
  }
  if (inspect_live_pointer_aggregate() !=
      (LIVE_POINTER_DIRECT ^ LIVE_POINTER_TARGET)) {
    return 13;
  }
  if (inspect_arithmetic_local(UINT64_C(0xa5), UINT64_C(0x70), UINT64_C(0x60),
                               UINT64_C(0x50), UINT64_C(0x40), UINT64_C(0x102030)) !=
      ARITHMETIC_LOCAL_EXPECTED) {
    return 4;
  }
  if (inspect_indirect_local(&indirect_ptr) != INDIRECT_LOCAL_EXPECTED) return 5;
  return inspect_inlined_local(inline_seed) == INLINE_LOCAL_EXPECTED ? 0 : 6;
}

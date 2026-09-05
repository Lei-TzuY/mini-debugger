#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

volatile uint64_t fixture_value = 0x1122334455667788ULL;
static volatile sig_atomic_t keep_running = 1;
static volatile sig_atomic_t worker_marker = 0;
static volatile sig_atomic_t signal_handler_count = 0;
static volatile sig_atomic_t signal_handler_tid = 0;
static volatile sig_atomic_t signal_worker_tid = 0;
static volatile sig_atomic_t selection_release = 0;
static volatile sig_atomic_t register_release = 0;
static volatile sig_atomic_t register_worker_ready = 0;

#define REGISTER_MUTATION_SEED UINT64_C(0x13579bdf2468ace0)
#define REGISTER_MUTATION_VALUE UINT64_C(0xa5a55a5ac3c33c3c)
#define CALLER_FRAME_LOCAL_EXPECTED UINT64_C(0x6a5b4c3d2e1f9081)

__attribute__((noinline)) void breakpoint_one(void) {
  __asm__ volatile("nop" ::: "memory");
  fixture_value += 1;
}

__attribute__((noinline)) void breakpoint_two(void) {
  __asm__ volatile("nop" ::: "memory");
  fixture_value += 1;
}

__attribute__((noinline)) void watched_write(void) {
  __asm__ volatile(".globl watchpoint_write_probe\n"
                   "watchpoint_write_probe:\n"
                   "addq $1, fixture_value(%%rip)\n"
                   ::: "memory");
}

__attribute__((noinline)) void backtrace_leaf(void) {
  __asm__ volatile(".globl backtrace_probe\n"
                   "backtrace_probe:\n"
                   "nop\n"
                   ::: "memory");
  fixture_value += 1;
}

__attribute__((noinline)) void backtrace_inner(void) {
  uint64_t caller_frame_local = CALLER_FRAME_LOCAL_EXPECTED;
  __asm__ volatile("" : "+m"(caller_frame_local) : : "memory");
  backtrace_leaf();
  __asm__ volatile("" : "+m"(caller_frame_local) : : "memory");
  if (caller_frame_local == CALLER_FRAME_LOCAL_EXPECTED) fixture_value += 1;
}

__attribute__((noinline)) void backtrace_outer(void) {
  backtrace_inner();
  fixture_value += 1;
}

static void publish_addresses(const char* path) {
  FILE* file = fopen(path, "w");
  if (file == NULL) _Exit(80);
  fprintf(file, "%p %p %p\n", (void*)&breakpoint_one, (void*)&breakpoint_two,
          (void*)&fixture_value);
  if (fclose(file) != 0) _Exit(81);
}

static void stop_attach_loop(int signal_number) {
  (void)signal_number;
  keep_running = 0;
}

static void worker_signal_handler(int signal_number) {
  (void)signal_number;
  signal_handler_count += 1;
  signal_handler_tid = (sig_atomic_t)syscall(SYS_gettid);
}

static void release_selection_worker(int signal_number) {
  (void)signal_number;
  selection_release = 1;
}

static void release_register_worker(int signal_number) {
  (void)signal_number;
  register_release = 1;
}

static void* lifecycle_worker(void* argument) {
  (void)argument;
  worker_marker = 1;
  return NULL;
}

static int run_thread_lifecycle(void) {
  pthread_t thread;
  if (pthread_create(&thread, NULL, lifecycle_worker, NULL) != 0) return 86;
  if (pthread_join(thread, NULL) != 0) return 87;
  return worker_marker == 1 ? 0 : 88;
}

static void* breakpoint_worker(void* argument) {
  (void)argument;
  breakpoint_one();
  breakpoint_one();
  worker_marker = 2;
  return NULL;
}

static int run_thread_breakpoint(void) {
  pthread_t thread;
  if (pthread_create(&thread, NULL, breakpoint_worker, NULL) != 0) return 89;
  if (pthread_join(thread, NULL) != 0) return 90;
  return worker_marker == 2 && fixture_value == 0x112233445566778aULL ? 0 : 91;
}

static void* signal_worker(void* argument) {
  (void)argument;
  signal_worker_tid = (sig_atomic_t)syscall(SYS_gettid);
  raise(SIGUSR1);
  raise(SIGUSR1);
  worker_marker = 3;
  return NULL;
}

static int run_thread_signal(void) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = worker_signal_handler;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGUSR1, &action, NULL) != 0) return 92;

  pthread_t thread;
  if (pthread_create(&thread, NULL, signal_worker, NULL) != 0) return 93;
  if (pthread_join(thread, NULL) != 0) return 94;
  return worker_marker == 3 && signal_handler_count == 1 &&
                 signal_handler_tid == signal_worker_tid
             ? 0
             : 95;
}

static void* blocking_worker(void* argument) {
  (void)argument;
  worker_marker = 4;
  while (keep_running) usleep(1000);
  return NULL;
}

static int run_thread_cleanup(void) {
  pthread_t thread;
  if (pthread_create(&thread, NULL, blocking_worker, NULL) != 0) return 96;
  if (pthread_join(thread, NULL) != 0) return 97;
  return 98;
}

static int run_threaded_attach(const char* path) {
  signal(SIGUSR1, stop_attach_loop);
  pthread_t thread;
  if (pthread_create(&thread, NULL, blocking_worker, NULL) != 0) return 99;
  while (worker_marker != 4) usleep(1000);
  publish_addresses(path);
  while (keep_running) usleep(1000);
  if (pthread_join(thread, NULL) != 0) return 100;
  breakpoint_two();
  return 0;
}

static void* selectable_worker(void* argument) {
  (void)argument;
  worker_marker = 5;
  while (!selection_release) usleep(1000);
  breakpoint_one();
  worker_marker = 6;
  return NULL;
}

static int run_threaded_select_attach(const char* path) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = release_selection_worker;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGUSR2, &action, NULL) != 0) return 101;

  pthread_t thread;
  if (pthread_create(&thread, NULL, selectable_worker, NULL) != 0) return 102;
  while (worker_marker != 5) usleep(1000);
  publish_addresses(path);
  if (pthread_join(thread, NULL) != 0) return 103;
  return worker_marker == 6 && fixture_value == 0x1122334455667789ULL ? 0 : 104;
}

static void* watchpoint_worker(void* argument) {
  (void)argument;
  worker_marker = 7;
  while (!selection_release) usleep(1000);
  watched_write();
  worker_marker = 8;
  return NULL;
}

static int run_threaded_watchpoint_attach(const char* path) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = release_selection_worker;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGUSR2, &action, NULL) != 0) return 105;

  pthread_t thread;
  if (pthread_create(&thread, NULL, watchpoint_worker, NULL) != 0) return 106;
  while (worker_marker != 7) usleep(1000);
  publish_addresses(path);
  if (pthread_join(thread, NULL) != 0) return 107;
  watched_write();
  return worker_marker == 8 && fixture_value == 0x112233445566778aULL ? 0 : 108;
}

static void* register_mutation_worker(void* argument) {
  (void)argument;
  unsigned char matched = 0;
  __asm__ volatile(
      "movabsq $0x13579bdf2468ace0, %%r12\n"
      "movl $1, register_worker_ready(%%rip)\n"
      "1:\n"
      "cmpl $0, register_release(%%rip)\n"
      "je 1b\n"
      "movabsq $0xa5a55a5ac3c33c3c, %%rax\n"
      "cmpq %%rax, %%r12\n"
      "sete %0\n"
      : "=q"(matched)
      :
      : "rax", "r12", "cc", "memory");
  return matched != 0 ? NULL : (void*)(uintptr_t)1;
}

static int run_threaded_register_mutation_attach(const char* path) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = release_register_worker;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGUSR2, &action, NULL) != 0) return 109;
  if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) != 0) return 110;

  register_release = 0;
  register_worker_ready = 0;
  pthread_t thread;
  if (pthread_create(&thread, NULL, register_mutation_worker, NULL) != 0) return 111;
  while (!register_worker_ready) usleep(1000);
  publish_addresses(path);
  void* result = NULL;
  if (pthread_join(thread, &result) != 0) return 112;
  return result == NULL ? 0 : 113;
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "thread-lifecycle") == 0) {
    return run_thread_lifecycle();
  }

  if (argc < 3) return 82;
  if (strcmp(argv[2], "attach-threaded") == 0) {
    return run_threaded_attach(argv[1]);
  }
  if (strcmp(argv[2], "attach-thread-select") == 0) {
    return run_threaded_select_attach(argv[1]);
  }
  if (strcmp(argv[2], "attach-thread-watchpoint") == 0) {
    return run_threaded_watchpoint_attach(argv[1]);
  }
  if (strcmp(argv[2], "attach-register-mutation") == 0) {
    return run_threaded_register_mutation_attach(argv[1]);
  }
  publish_addresses(argv[1]);

  if (strcmp(argv[2], "attach") == 0) {
    signal(SIGUSR1, stop_attach_loop);
    while (keep_running) {
      breakpoint_one();
      usleep(1000);
    }
    breakpoint_two();
    return 0;
  }

  raise(SIGSTOP);

  if (strcmp(argv[2], "exit") == 0) return 0;
  if (strcmp(argv[2], "trap") == 0) {
    raise(SIGTRAP);
    return 0;
  }
  if (strcmp(argv[2], "signal") == 0) {
    raise(SIGUSR1);
    return 0;
  }
  if (strcmp(argv[2], "terminate") == 0) {
    raise(SIGTERM);
    return 83;
  }
  if (strcmp(argv[2], "backtrace") == 0) {
    backtrace_outer();
    return 0;
  }
  if (strcmp(argv[2], "backtrace-repeat") == 0) {
    backtrace_outer();
    backtrace_outer();
    return fixture_value == 0x112233445566778eULL ? 0 : 114;
  }
  if (strcmp(argv[2], "watchpoint") == 0) {
    watched_write();
    watched_write();
    return fixture_value == 0x112233445566778aULL ? 0 : 85;
  }
  if (strcmp(argv[2], "thread-breakpoint") == 0) {
    return run_thread_breakpoint();
  }
  if (strcmp(argv[2], "thread-signal") == 0) {
    return run_thread_signal();
  }
  if (strcmp(argv[2], "thread-cleanup") == 0) {
    return run_thread_cleanup();
  }

  breakpoint_one();
  breakpoint_two();
  breakpoint_one();
  return fixture_value == 0x112233445566778bULL ? 0 : 84;
}

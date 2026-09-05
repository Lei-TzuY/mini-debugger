#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

volatile uint64_t fixture_value = 0x1122334455667788ULL;
static volatile sig_atomic_t keep_running = 1;
static volatile sig_atomic_t worker_marker = 0;

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
  backtrace_leaf();
  fixture_value += 1;
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

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "thread-lifecycle") == 0) {
    return run_thread_lifecycle();
  }

  if (argc < 3) return 82;
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
  if (strcmp(argv[2], "watchpoint") == 0) {
    watched_write();
    watched_write();
    return fixture_value == 0x112233445566778aULL ? 0 : 85;
  }
  if (strcmp(argv[2], "thread-breakpoint") == 0) {
    return run_thread_breakpoint();
  }

  breakpoint_one();
  breakpoint_two();
  breakpoint_one();
  return fixture_value == 0x112233445566778bULL ? 0 : 84;
}

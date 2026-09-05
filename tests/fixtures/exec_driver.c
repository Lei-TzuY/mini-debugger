#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern char** environ;

volatile uint64_t fork_shared_value = UINT64_C(0x2718281828459045);
volatile uint64_t vfork_parent_value = UINT64_C(0x3141592653589793);

__attribute__((noinline)) void fork_shared_probe(void) {
  __asm__ volatile("nop" ::: "memory");
  fork_shared_value += 1;
}

__attribute__((noinline)) void vfork_parent_probe(void) {
  __asm__ volatile("nop" ::: "memory");
}

static int block_sigchld(void) {
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGCHLD);
  return sigprocmask(SIG_BLOCK, &blocked, NULL);
}

static int wait_clean_child(pid_t child) {
  int status = 0;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result == -1 && errno == EINTR);
  return result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int run_fork_topology(void) {
  if (block_sigchld() != 0) return 19;

  const pid_t child = fork();
  if (child == -1) return 20;
  if (child == 0) {
    fork_shared_probe();
    raise(SIGSTOP);
    _exit(0);
  }

  if (wait_clean_child(child) != 0) return 21;

  fork_shared_probe();
  return 0;
}

static int run_nested_fork_topology(void) {
  if (block_sigchld() != 0) return 33;

  const pid_t child = fork();
  if (child == -1) return 34;
  if (child == 0) {
    const pid_t grandchild = fork();
    if (grandchild == -1) _exit(35);
    if (grandchild == 0) {
      fork_shared_probe();
      raise(SIGSTOP);
      _exit(0);
    }

    if (wait_clean_child(grandchild) != 0) _exit(36);
    fork_shared_probe();
    raise(SIGSTOP);
    _exit(0);
  }

  if (wait_clean_child(child) != 0) return 37;
  fork_shared_probe();
  return 0;
}

static int run_fork_exec_divergence(const char* target) {
  if (block_sigchld() != 0) return 29;

  const pid_t child = fork();
  if (child == -1) return 30;
  if (child == 0) {
    execl(target, target, NULL);
    _exit(31);
  }

  if (wait_clean_child(child) != 0) return 32;
  fork_shared_probe();
  return 0;
}

static int run_vfork_topology(const char* target) {
  if (block_sigchld() != 0) return 22;

  const pid_t exit_child = vfork();
  if (exit_child == -1) return 23;
  if (exit_child == 0) _exit(0);
  if (wait_clean_child(exit_child) != 0) return 24;

  const pid_t exec_child = vfork();
  if (exec_child == -1) return 25;
  if (exec_child == 0) {
    execl(target, target, NULL);
    _exit(26);
  }
  if (wait_clean_child(exec_child) != 0) return 27;

  vfork_parent_probe();
  vfork_parent_value += 1;
  return vfork_parent_value == UINT64_C(0x3141592653589794) ? 0 : 28;
}

__attribute__((noreturn, noinline)) static void exec_target(const char* target) {
  char* const argv[] = {(char*)target, NULL};
  long result = 0;
  __asm__ volatile(".globl exec_syscall_probe\n"
                   "exec_syscall_probe:\n"
                   "syscall\n"
                   : "=a"(result)
                   : "a"((long)SYS_execve), "D"(target), "S"(argv), "d"(environ)
                   : "rcx", "r11", "memory");
  (void)result;
  _exit(120);
}

static void* exec_worker(void* argument) {
  exec_target((const char*)argument);
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--fork-topology") == 0) {
    return run_fork_topology();
  }
  if (argc == 2 && strcmp(argv[1], "--nested-fork-topology") == 0) {
    return run_nested_fork_topology();
  }
  if (argc == 3 && strcmp(argv[1], "--fork-exec-divergence") == 0) {
    return run_fork_exec_divergence(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "--vfork-topology") == 0) {
    return run_vfork_topology(argv[2]);
  }
  if (argc != 2) return 2;

  pthread_t thread;
  if (pthread_create(&thread, NULL, exec_worker, argv[1]) != 0) return 3;
  if (pthread_join(thread, NULL) != 0) return 4;
  return 5;
}

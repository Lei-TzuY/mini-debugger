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

volatile uint64_t vfork_parent_value = UINT64_C(0x3141592653589793);

__attribute__((noinline)) void fork_shared_probe(void) {
  __asm__ volatile("nop" ::: "memory");
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

static int run_fork_topology(void) {
  if (block_sigchld() != 0) return 19;

  const pid_t child = fork();
  if (child == -1) return 20;
  if (child == 0) {
    fork_shared_probe();
    raise(SIGSTOP);
    _exit(0);
  }

  int status = 0;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result == -1 && errno == EINTR);
  if (result != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 21;

  fork_shared_probe();
  return 0;
}

static int run_vfork_topology(const char* target) {
  if (block_sigchld() != 0) return 22;

  const pid_t child = vfork();
  if (child == -1) return 23;
  if (child == 0) {
    execl(target, target, NULL);
    _exit(24);
  }

  int status = 0;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result == -1 && errno == EINTR);
  if (result != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 25;

  vfork_parent_probe();
  vfork_parent_value += 1;
  return vfork_parent_value == UINT64_C(0x3141592653589794) ? 0 : 26;
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
  if (argc == 3 && strcmp(argv[1], "--vfork-topology") == 0) {
    return run_vfork_topology(argv[2]);
  }
  if (argc != 2) return 2;

  pthread_t thread;
  if (pthread_create(&thread, NULL, exec_worker, argv[1]) != 0) return 3;
  if (pthread_join(thread, NULL) != 0) return 4;
  return 5;
}

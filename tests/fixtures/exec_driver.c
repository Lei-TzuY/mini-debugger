#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <stdint.h>
#include <stdlib.h>

extern char** environ;

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
  if (argc != 2) return 2;

  pthread_t thread;
  if (pthread_create(&thread, NULL, exec_worker, argv[1]) != 0) return 3;
  if (pthread_join(thread, NULL) != 0) return 4;
  return 5;
}

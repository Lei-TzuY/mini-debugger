#include <dlfcn.h>
#include <string.h>

__attribute__((noinline)) void reload_barrier(void) {
  __asm__ volatile("" ::: "memory");
}

static int call_plugin(const char* path) {
  void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (handle == 0) return -1;

  void* symbol = dlsym(handle, "deferred_target");
  if (symbol == 0) {
    dlclose(handle);
    return -2;
  }

  int (*target)(void) = 0;
  memcpy(&target, &symbol, sizeof(target));
  const int value = target();
  if (dlclose(handle) != 0) return -3;
  return value;
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;

  if (call_plugin(argv[1]) != 73) return 3;
  reload_barrier();
  if (call_plugin(argv[1]) != 73) return 4;
  return 0;
}

#include <dlfcn.h>
#include <string.h>

int main(int argc, char** argv) {
  if (argc != 2) return 2;

  void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (handle == 0) return 3;

  void* symbol = dlsym(handle, "deferred_target");
  if (symbol == 0) {
    dlclose(handle);
    return 4;
  }

  int (*target)(void) = 0;
  memcpy(&target, &symbol, sizeof(target));
  const int value = target();
  const int close_result = dlclose(handle);
  if (close_result != 0) return 5;
  return value == 73 ? 0 : 6;
}

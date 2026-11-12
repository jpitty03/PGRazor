// Exercise real Android libdl dependency lookup, forcing the RTLD_DEFAULT failure from the report.
// This fixture never opens a game library or attaches to the game process.
#include <dlfcn.h>
#include <cassert>
#include <cstdio>
#include <cstring>
static void *without_default(void *handle, const char *symbol) {
  if (handle == RTLD_DEFAULT && !std::strcmp(symbol, "__loader_dlopen")) return nullptr;
  return ::dlsym(handle, symbol);
}
#define dlsym without_default
#include "runtime_bootstrap.cpp"
#undef dlsym
const void *pgrazor_runtime_metadata_api(uint32_t) { return nullptr; }
int main() {
  auto loader = resolve_loader_dlopen();
  assert(loader && "explicit libdl/ld-android lookup must resolve the real Android loader");
  void *handle = loader("libdl.so", RTLD_NOLOAD | RTLD_NOW, reinterpret_cast<const void *>(&main));
  assert(handle && ::dlsym(handle, "dlopen"));
  ::dlclose(handle);
  std::puts("PASS real Android loader: RTLD_DEFAULT deliberately hidden; explicit loaded-host lookup and NOLOAD caller-based open succeeded");
}

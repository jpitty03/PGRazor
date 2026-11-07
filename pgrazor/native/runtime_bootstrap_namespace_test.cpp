// Android fixture for the rooted-only namespace fallback. Build with the NDK
// and run on a test device; the separate runtime_bootstrap_test exercises the
// public probe lifecycle against the actual production shared library.
#include <link.h>
#include <dlfcn.h>
#include <cassert>
#include <cstring>
#include <cstdio>

static int fixture_iterate(int (*callback)(dl_phdr_info *, size_t, void *), void *context);
static void *fixture_dlsym(void *, const char *);
#define dl_iterate_phdr fixture_iterate
#define dlsym fixture_dlsym
#include "runtime_bootstrap.cpp"
#undef dl_iterate_phdr
#undef dlsym

const void *pgrazor_runtime_metadata_api(uint32_t) { return nullptr; }

static unsigned count, opens;
static bool loader_available = true;
static ElfW(Phdr) segment;
static dl_phdr_info entry;
static void *open_fixture(const char *path, int flags, const void *caller) {
  ++opens;
  assert(!std::strcmp(path, "/game/libil2cpp.so"));
  assert(flags == (RTLD_NOLOAD | RTLD_NOW));
  assert(caller == reinterpret_cast<void *>(0x1200));
  return reinterpret_cast<void *>(0x55);
}
static int fixture_iterate(int (*callback)(dl_phdr_info *, size_t, void *), void *context) {
  for (unsigned i=0; i<count; ++i) callback(&entry, sizeof(entry), context);
  return 0;
}
static void *fixture_dlsym(void *, const char *symbol) {
  assert(!std::strcmp(symbol, "__loader_dlopen"));
  return loader_available ? reinterpret_cast<void *>(open_fixture) : nullptr;
}
int main() {
  entry.dlpi_name="/game/libil2cpp.so"; entry.dlpi_addr=0x1000;
  entry.dlpi_phdr=&segment; entry.dlpi_phnum=1;
  segment.p_type=PT_LOAD; segment.p_flags=PF_R|PF_X;
  segment.p_vaddr=0x200; segment.p_memsz=0x100;
  count=0; assert(!loaded_namespace_handle("libil2cpp.so") && opens==0);
  count=1; assert(!loaded_namespace_handle("il2cpp.so") && opens==0);
  count=2; assert(!loaded_namespace_handle("libil2cpp.so") && opens==0);
  count=1; segment.p_flags=PF_R;
  assert(!loaded_namespace_handle("libil2cpp.so") && opens==0);
  segment.p_flags=PF_X; segment.p_vaddr=UINTPTR_MAX;
  assert(!loaded_namespace_handle("libil2cpp.so") && opens==0);
  segment.p_vaddr=0x200; loader_available=false;
  assert(!loaded_namespace_handle("libil2cpp.so") && opens==0);
  loader_available=true;
  assert(loaded_namespace_handle("libil2cpp.so")==reinterpret_cast<void *>(0x55));
  assert(opens==1);
  assert(loaded_namespace_handle("/game/libil2cpp.so")==reinterpret_cast<void *>(0x55));
  assert(opens==2);
  std::puts("PASS namespace fallback: exact name/path, absent/duplicate images, executable segment, overflow, missing loader, NOLOAD and caller identity");
}

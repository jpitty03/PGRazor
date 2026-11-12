// Android fixture for the rooted-only namespace fallback. Build with the NDK
// and run on a test device; the separate runtime_bootstrap_test exercises the
// public probe lifecycle against the actual production shared library.
//
// It drives loaded_namespace_handle() directly and models bionic's dl* surface so the
// loader-resolution routes are deterministic on-host: __loader_dlopen is reached first via
// dlsym(RTLD_DEFAULT) and, when that fails (the real rooted-namespace case), via an explicit
// handle on libdl.so / ld-android.so. It asserts the precise PgrBootLoaderDetail reported for
// each failure, that every RTLD_NOLOAD host open is balanced by a dlclose, that the game image
// is opened RTLD_NOLOAD with its own executable-segment address as the caller, and that no route
// ever fabricates a handle.
#include <link.h>
#include <dlfcn.h>
#include <cassert>
#include <cstring>
#include <cstdio>

static int fixture_iterate(int (*callback)(dl_phdr_info *, size_t, void *), void *context);
static void *fixture_dlopen(const char *name, int flags);
static int fixture_dlclose(void *handle);
static void *fixture_dlsym(void *handle, const char *symbol);
#define dl_iterate_phdr fixture_iterate
#define dlopen fixture_dlopen
#define dlclose fixture_dlclose
#define dlsym fixture_dlsym
#include "runtime_bootstrap.cpp"
#undef dl_iterate_phdr
#undef dlopen
#undef dlclose
#undef dlsym

const void *pgrazor_runtime_metadata_api(uint32_t) { return nullptr; }

// Sentinel handles. RTLD_DEFAULT is (void*)0 on LP64 NDK; none of these collide with it.
static void *const LIBDL_HANDLE = reinterpret_cast<void *>(0x4001);
static void *const LDANDROID_HANDLE = reinterpret_cast<void *>(0x4002);
static void *const GAME_HANDLE = reinterpret_cast<void *>(0x55);

static unsigned count;         // images reported by dl_iterate_phdr
static unsigned game_opens;    // __loader_dlopen(game, ...) invocations
static int host_balance;       // dlopen(host) minus dlclose(host); must settle at 0
// Which route yields __loader_dlopen, and whether each host is even mapped.
static bool default_has_loader, libdl_present, libdl_has_loader, ldandroid_present, ldandroid_has_loader;
static bool game_open_succeeds;
static ElfW(Phdr) segment;
static dl_phdr_info entry;

// The real __loader_dlopen stand-in: opens the game image RTLD_NOLOAD with the exact caller.
static void *open_fixture(const char *path, int flags, const void *caller) {
  ++game_opens;
  assert(!std::strcmp(path, "/game/libil2cpp.so"));
  assert(flags == (RTLD_NOLOAD | RTLD_NOW));
  assert(caller == reinterpret_cast<void *>(0x1200)); // dlpi_addr 0x1000 + p_vaddr 0x200
  return game_open_succeeds ? GAME_HANDLE : nullptr;
}
static int fixture_iterate(int (*callback)(dl_phdr_info *, size_t, void *), void *context) {
  for (unsigned i = 0; i < count; ++i) callback(&entry, sizeof(entry), context);
  return 0;
}
static void *fixture_dlopen(const char *name, int flags) {
  assert(flags == (RTLD_NOLOAD | RTLD_NOW)); // hosts are only ever opened without force-loading
  if (!std::strcmp(name, "libdl.so")) { if (!libdl_present) return nullptr; ++host_balance; return LIBDL_HANDLE; }
  if (!std::strcmp(name, "ld-android.so")) { if (!ldandroid_present) return nullptr; ++host_balance; return LDANDROID_HANDLE; }
  assert(false && "unexpected dlopen host"); // the game image is opened via __loader_dlopen, not dlopen
  return nullptr;
}
static int fixture_dlclose(void *handle) {
  assert(handle == LIBDL_HANDLE || handle == LDANDROID_HANDLE); // only host handles are ever closed
  assert(host_balance > 0);
  --host_balance;
  return 0;
}
static void *fixture_dlsym(void *handle, const char *symbol) {
  assert(!std::strcmp(symbol, "__loader_dlopen"));
  void *loader = reinterpret_cast<void *>(open_fixture);
  if (handle == RTLD_DEFAULT) return default_has_loader ? loader : nullptr;
  if (handle == LIBDL_HANDLE) return libdl_has_loader ? loader : nullptr;
  if (handle == LDANDROID_HANDLE) return ldandroid_has_loader ? loader : nullptr;
  assert(false && "unexpected dlsym handle");
  return nullptr;
}

// Default to a healthy environment; individual scenarios narrow a single knob.
static void reset() {
  count = 1;
  game_opens = 0;
  host_balance = 0;
  default_has_loader = false;
  libdl_present = libdl_has_loader = true;
  ldandroid_present = ldandroid_has_loader = true;
  game_open_succeeds = true;
  entry.dlpi_name = "/game/libil2cpp.so";
  entry.dlpi_addr = 0x1000;
  entry.dlpi_phdr = &segment;
  entry.dlpi_phnum = 1;
  segment.p_type = PT_LOAD;
  segment.p_flags = PF_R | PF_X;
  segment.p_vaddr = 0x200;
  segment.p_memsz = 0x100;
}

// Call the unit under test; capture the diagnostic and assert the host open/close balance settled.
static int diag;
static void *call(const char *name) {
  diag = -1;
  void *handle = loaded_namespace_handle(name, &diag);
  assert(host_balance == 0); // every RTLD_NOLOAD host open was released
  return handle;
}

int main() {
  // ---- image discovery failures (loader never consulted) -------------------------------------
  reset(); count = 0;
  assert(!call("libil2cpp.so") && diag == PGR_BOOT_LD_IMAGE_PENDING && game_opens == 0); // not mapped yet
  reset();
  assert(!call("il2cpp.so") && diag == PGR_BOOT_LD_IMAGE_PENDING && game_opens == 0); // name mismatch
  reset(); count = 2;
  assert(!call("libil2cpp.so") && diag == PGR_BOOT_LD_IMAGE_AMBIGUOUS && game_opens == 0); // duplicate image
  reset(); segment.p_flags = PF_R;
  assert(!call("libil2cpp.so") && diag == PGR_BOOT_LD_NO_EXEC_SEGMENT && game_opens == 0); // no executable seg
  reset(); segment.p_vaddr = UINTPTR_MAX;
  assert(!call("libil2cpp.so") && diag == PGR_BOOT_LD_NO_EXEC_SEGMENT && game_opens == 0); // caller overflow

  // ---- __loader_dlopen resolution routes (image is unique + executable) ----------------------
  reset(); default_has_loader = true; // device where RTLD_DEFAULT still exposes the loader
  assert(call("libil2cpp.so") == GAME_HANDLE && diag == PGR_BOOT_LD_IMAGE_PENDING && game_opens == 1);

  reset(); // RTLD_DEFAULT missing -> resolved through the already-loaded libdl handle (the real fix)
  assert(call("libil2cpp.so") == GAME_HANDLE && game_opens == 1);

  reset(); libdl_has_loader = false; // libdl mapped but symbol only via ld-android -> fall through
  assert(call("libil2cpp.so") == GAME_HANDLE && game_opens == 1);

  reset(); libdl_present = false; // libdl handle unavailable -> ld-android route
  assert(call("libil2cpp.so") == GAME_HANDLE && game_opens == 1);

  // ---- unavailable fallback: no route yields the symbol --------------------------------------
  reset(); libdl_has_loader = ldandroid_has_loader = false; // both hosts mapped, neither exports it
  assert(!call("libil2cpp.so") && diag == PGR_BOOT_LD_SYMBOL_ABSENT && game_opens == 0);
  reset(); libdl_present = ldandroid_present = false; // neither host even mapped
  assert(!call("libil2cpp.so") && diag == PGR_BOOT_LD_SYMBOL_ABSENT && game_opens == 0);

  // ---- loader resolved but the namespace NOLOAD open returns null ----------------------------
  reset(); default_has_loader = true; game_open_succeeds = false;
  assert(!call("libil2cpp.so") && diag == PGR_BOOT_LD_NOLOAD_NULL && game_opens == 1);

  // ---- match by full path, resolved via the libdl fallback -----------------------------------
  reset();
  assert(call("/game/libil2cpp.so") == GAME_HANDLE && game_opens == 1);

  std::puts("PASS namespace fallback: image-pending/mismatch/ambiguous/no-exec-segment/overflow, "
            "RTLD_DEFAULT and libdl and ld-android loader routes, symbol-absent and noload-null "
            "diagnostics, balanced host open/close, RTLD_NOLOAD + exact caller identity, full-path match");
}

// Actual production dlopen/query54 fixture for the feature 54 IL2CPP bootstrap binding.
// The il2cpp exports are FAKE; the feature 47 metadata validators reached through the metadata
// seam are the REAL compiled ones, driven over a fake metadata universe and a real page-aligned
// fake-module buffer. No game process, no hooks, no Frida: this proves NOTHING about live
// translated-ARM64 resolution under Houdini. Feature 54 is a process-lifetime LATCHED singleton
// (READY or POISONED), so every terminal outcome runs in its own fork of a fresh singleton.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 // RTLD_NOLOAD on glibc hosts; Android NDK has it unconditionally
#endif
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// ---- fake IL2CPP metadata universe -----------------------------------------------------------
struct Class;
struct Type { std::string name; Class *klass = nullptr; };
struct Class { Type *type = nullptr; };
struct Method {
  void *code = nullptr; // MethodInfo's first pointer is read by debug_get_method_info
  Class *owner = nullptr;
  std::string name;
  Type *result = nullptr;
  uint32_t count = 0, flags = 0;
  bool generic = false, inflated = false;
};

std::vector<Type *> types;
Class exec_class, duplicate_class;
Type exec_type, void_type;
Method execute_tasks;
int domain_token = 0, image_a = 0, image_b = 0, thread_token = 0, fake_handle = 0;
void *assemblies[] = {&image_a, &image_b};

unsigned char *module_base = nullptr;
uint64_t module_size = 0;
constexpr uint64_t EXEC_RVA = 0x4000;
const unsigned char PROLOGUE[] = {0xfd, 0x7b, 0xbf, 0xa9, 0xfd, 0x03, 0x00, 0x91, 0xf3, 0x03, 0x00, 0xaa};

// ---- fixture control knobs (reset per scenario) ---------------------------------------------
bool loaded = true;         // dlopen seam: library present
bool domain_up = true;      // domain_get seam: managed domain initialized
bool thread_attached = false; // thread_current seam: calling thread already attached
void *attach_result = &thread_token; // thread_attach seam return (null -> attach failure)
bool duplicate = false;     // class_from_name returns a different handle in image_b
const char *missing_name = nullptr; // dlsym seam returns null for this symbol
unsigned attach_calls = 0, detach_calls = 0, reentries = 0;
void *last_detached = nullptr;
bool reenter = false;       // domain_get seam reenters probe to prove BUSY / no deadlock

const pgrazor_native_api_v2 *core = nullptr;
const PgrBootApi *api = nullptr;
PgrBootConfig base_config{};

unsigned names_allocated = 0, names_freed = 0;

// ---- fake export implementations -------------------------------------------------------------
void unused_stub() { std::abort(); } // any export this path does not exercise must never be called

void *f_domain_get() {
  if (!domain_up) return nullptr;
  if (reenter) {
    reenter = false;
    // Same-thread reentry through a managed callback: must return BUSY, never deadlock.
    PgrBootBinding b;
    std::memset(&b, 0xa5, sizeof(b));
    assert(api->probe(&base_config, &b) == PGR_BOOT_ERR_BUSY);
    assert(b.status == PGR_BOOT_ERR_BUSY && b.handle == nullptr && b.execute_tasks == nullptr);
    // Concurrent thread during the same managed callback: also BUSY (serialized, non-blocking).
    std::thread other([] {
      PgrBootBinding c;
      std::memset(&c, 0xa5, sizeof(c));
      assert(api->probe(&base_config, &c) == PGR_BOOT_ERR_BUSY && c.status == PGR_BOOT_ERR_BUSY);
      // snapshot is also guarded and must not deadlock while the outer probe holds entry.
      assert(api->snapshot(&c) == PGR_BOOT_ERR_BUSY);
    });
    other.join();
    ++reentries;
  }
  return &domain_token;
}
void *f_domain_get_assemblies(void *, size_t *count) { *count = 2; return assemblies; }
void *f_assembly_get_image(void *assembly) { return assembly; }
void *f_class_from_name(void *image, const char *ns, const char *name) {
  if (std::strcmp(ns, "UnityEngine") == 0 && std::strcmp(name, "UnitySynchronizationContext") == 0) {
    if (image == &image_a) return &exec_class;
    if (image == &image_b && duplicate) return &duplicate_class;
  }
  return nullptr;
}
void *f_class_get_type(void *klass) { return static_cast<Class *>(klass)->type; }
char *f_type_get_name(void *raw) {
  const std::string &name = static_cast<Type *>(raw)->name;
  char *copy = static_cast<char *>(std::malloc(name.size() + 1));
  assert(copy);
  std::memcpy(copy, name.c_str(), name.size() + 1);
  ++names_allocated;
  return copy;
}
void f_mem_free(void *p) { assert(p); ++names_freed; std::free(p); }
void *f_class_get_method_from_name(void *owner, const char *name, int argc) {
  if (owner == &exec_class && execute_tasks.name == name && int(execute_tasks.count) == argc)
    return &execute_tasks;
  return nullptr;
}
void *f_method_get_class(void *raw) { return static_cast<Method *>(raw)->owner; }
char *f_method_get_name(void *raw) { return const_cast<char *>(static_cast<Method *>(raw)->name.c_str()); }
uint32_t f_method_get_flags(void *raw, uint32_t *impl) { *impl = 0; return static_cast<Method *>(raw)->flags; }
uint32_t f_method_get_param_count(void *raw) { return static_cast<Method *>(raw)->count; }
void *f_method_get_param(void *, uint32_t) { std::abort(); }
void *f_method_get_return_type(void *raw) { return static_cast<Method *>(raw)->result; }
bool f_method_is_generic(void *raw) { return static_cast<Method *>(raw)->generic; }
bool f_method_is_inflated(void *raw) { return static_cast<Method *>(raw)->inflated; }
bool f_debug_get_method_info(void *raw, void *out) {
  static_cast<void **>(out)[0] = static_cast<Method *>(raw)->code;
  return true;
}
void *f_thread_current() { return thread_attached ? &thread_token : nullptr; }
void *f_thread_attach(void *domain) { assert(domain == &domain_token); ++attach_calls; return attach_result; }
void f_thread_detach(void *thread) { ++detach_calls; last_detached = thread; }

struct Entry { const char *name; void *fn; };
std::vector<Entry> sym_table;

void map(const char *name, void *fn) { sym_table.push_back({name, fn}); }

// ---- seams ------------------------------------------------------------------------------------
void *seam_dlopen(void *, const char *soname, int flags) {
  assert(std::strcmp(soname, "libil2cpp.so") == 0);
  assert((flags & RTLD_NOLOAD) != 0);
  return loaded ? &fake_handle : nullptr;
}
void *seam_dlsym(void *, void *handle, const char *name) {
  assert(handle == &fake_handle);
  if (missing_name && std::strcmp(name, missing_name) == 0) return nullptr;
  for (const auto &e : sym_table)
    if (std::strcmp(e.name, name) == 0) return e.fn;
  return reinterpret_cast<void *>(&unused_stub); // non-null; never called on exercised paths
}
int seam_module(void *, const char *soname, const void **base, uint64_t *size) {
  assert(std::strcmp(soname, "libil2cpp.so") == 0);
  *base = module_base;
  *size = module_size;
  return 0;
}
const void *seam_metadata(void *, uint32_t version) { return core->query_feature(47, version); }
int seam_segment(void *, const void *address, uint64_t length, PgrMetaSegment *out) {
  *out = {};
  uintptr_t a = reinterpret_cast<uintptr_t>(address), base = reinterpret_cast<uintptr_t>(module_base);
  if (a < base || a - base > module_size || !length || length > module_size - (a - base)) return 0;
  out->found = out->readable = out->host_exec = 1;
  out->seg_base = module_base;
  out->seg_size = module_size;
  return 0;
}

void reset_knobs() {
  loaded = true;
  domain_up = true;
  thread_attached = false;
  attach_result = &thread_token;
  duplicate = false;
  missing_name = nullptr;
  attach_calls = detach_calls = reentries = 0;
  last_detached = nullptr;
  reenter = false;
}

void build_fixture() {
  long page = sysconf(_SC_PAGESIZE);
  assert(page > 0);
  module_size = ((EXEC_RVA + sizeof(PROLOGUE) + page - 1) / page) * page;
  void *mapped = mmap(nullptr, module_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(mapped != MAP_FAILED);
  module_base = static_cast<unsigned char *>(mapped);
  std::memcpy(module_base + EXEC_RVA, PROLOGUE, sizeof(PROLOGUE));

  void_type.name = "System.Void";
  exec_type.name = "UnityEngine.UnitySynchronizationContext";
  exec_type.klass = &exec_class;
  exec_class.type = &exec_type;
  duplicate_class.type = &exec_type; // same canonical, different handle -> duplicate

  execute_tasks.owner = &exec_class;
  execute_tasks.name = "ExecuteTasks";
  execute_tasks.result = &void_type;
  execute_tasks.count = 0;
  execute_tasks.flags = 0x10; // METHOD_ATTRIBUTE_STATIC
  execute_tasks.code = module_base + EXEC_RVA;

  map("il2cpp_domain_get", reinterpret_cast<void *>(f_domain_get));
  map("il2cpp_domain_get_assemblies", reinterpret_cast<void *>(f_domain_get_assemblies));
  map("il2cpp_assembly_get_image", reinterpret_cast<void *>(f_assembly_get_image));
  map("il2cpp_class_from_name", reinterpret_cast<void *>(f_class_from_name));
  map("il2cpp_class_get_type", reinterpret_cast<void *>(f_class_get_type));
  map("il2cpp_type_get_name", reinterpret_cast<void *>(f_type_get_name));
  map("il2cpp_free", reinterpret_cast<void *>(f_mem_free));
  map("il2cpp_class_get_method_from_name", reinterpret_cast<void *>(f_class_get_method_from_name));
  map("il2cpp_method_get_class", reinterpret_cast<void *>(f_method_get_class));
  map("il2cpp_method_get_name", reinterpret_cast<void *>(f_method_get_name));
  map("il2cpp_method_get_flags", reinterpret_cast<void *>(f_method_get_flags));
  map("il2cpp_method_get_param_count", reinterpret_cast<void *>(f_method_get_param_count));
  map("il2cpp_method_get_param", reinterpret_cast<void *>(f_method_get_param));
  map("il2cpp_method_get_return_type", reinterpret_cast<void *>(f_method_get_return_type));
  map("il2cpp_method_is_generic", reinterpret_cast<void *>(f_method_is_generic));
  map("il2cpp_method_is_inflated", reinterpret_cast<void *>(f_method_is_inflated));
  map("il2cpp_debug_get_method_info", reinterpret_cast<void *>(f_debug_get_method_info));
  map("il2cpp_thread_current", reinterpret_cast<void *>(f_thread_current));
  map("il2cpp_thread_attach", reinterpret_cast<void *>(f_thread_attach));
  map("il2cpp_thread_detach", reinterpret_cast<void *>(f_thread_detach));

  base_config.version = PGR_BOOT_CONFIG_VERSION;
  base_config.size = sizeof(PgrBootConfig);
  base_config.soname = "libil2cpp.so";
  base_config.dlopen_fn = seam_dlopen;
  base_config.dlsym_fn = seam_dlsym;
  base_config.module_fn = seam_module;
  base_config.seam_ctx = nullptr;
  base_config.metadata_fn = seam_metadata;
  base_config.seg_query = seam_segment;
  base_config.seg_ctx = nullptr;
  // exec_* left null -> compiled defaults (UnityEngine.UnitySynchronizationContext.ExecuteTasks)
}

bool cleared(const PgrBootBinding &b, int status, int detail) {
  return b.version == PGR_BOOT_BINDING_VERSION && b.size == sizeof(PgrBootBinding) &&
         b.status == status && b.detail == detail && b.flags == 0 && b.handle == nullptr &&
         b.module_base == nullptr && b.execute_tasks == nullptr && b.domain == nullptr;
}

PgrBootBinding probe() {
  PgrBootBinding b;
  std::memset(&b, 0xa5, sizeof(b));
  int rc = api->probe(&base_config, &b);
  assert(rc == b.status);
  return b;
}

void expect_fail(int status, int detail) {
  PgrBootBinding b = probe();
  assert(b.status == status && b.detail == detail && cleared(b, status, detail));
}

void assert_success(const PgrBootBinding &b, bool attached_expected) {
  assert(b.status == PGR_BOOT_OK && b.detail == 0 && b.version == PGR_BOOT_BINDING_VERSION &&
         b.size == sizeof(PgrBootBinding));
  assert(b.handle == &fake_handle && b.module_base == module_base && b.module_size == module_size);
  assert(b.domain == &domain_token && b.execute_tasks == &execute_tasks);
  assert(b.meta.domain_get == reinterpret_cast<void *>(f_domain_get));
  assert(b.meta.class_from_name == reinterpret_cast<void *>(f_class_from_name));
  assert(b.runtime.thread_current == reinterpret_cast<void *>(f_thread_current));
  assert(b.runtime.runtime_invoke == reinterpret_cast<void *>(&unused_stub));
  assert((b.flags & 1u) == (attached_expected ? 1u : 0u));
  assert(names_allocated == names_freed);
}

// ---- fork harness: each terminal outcome runs in a fresh feature-54/47 singleton --------------
void run(const char *name, const std::function<void()> &body) {
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    reset_knobs();
    body();
    std::_Exit(0);
  }
  int status = 0;
  assert(waitpid(pid, &status, 0) == pid);
  if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
    std::fprintf(stderr, "FAIL scenario: %s\n", name);
    std::abort();
  }
}

} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  assert(library);
  auto get = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(library, "pgrazor_native_api"));
  assert(get);
  core = static_cast<const pgrazor_native_api_v2 *>(get(2));
  assert(core && core->query_feature);
  assert(!core->query_feature(54, 2)); // exact-version only
  api = static_cast<const PgrBootApi *>(core->query_feature(54, 1));
  assert(api && api->version == 1 && api->size == 24 && api->probe && api->snapshot);
  assert(core->query_feature(47, 1)); // feature 47 must be registered for the metadata seam
  build_fixture();

  // 1. Full success (no compiled-address identity), snapshot, idempotent replay.
  run("success + snapshot + replay", [] {
    PgrBootBinding b = probe();
    assert_success(b, /*attached*/ true); // thread_current null at start -> attach+detach once
    assert(attach_calls == 1 && detach_calls == 1 && last_detached == &thread_token);
    PgrBootBinding s;
    std::memset(&s, 0xa5, sizeof(s));
    assert(api->snapshot(&s) == PGR_BOOT_OK);
    assert(std::memcmp(&s, &b, sizeof(s)) == 0);
    unsigned a0 = attach_calls;
    PgrBootBinding again = probe(); // replay does not re-dlopen / re-attach
    assert(std::memcmp(&again, &b, sizeof(b)) == 0 && attach_calls == a0);
  });

  // 2. Success WITH compiled-address identity: exercises the segment resolver + prologue bytes.
  run("success with code identity", [] {
    base_config.exec_code_rva = EXEC_RVA;
    base_config.exec_prologue = PROLOGUE;
    base_config.exec_prologue_len = sizeof(PROLOGUE);
    PgrBootBinding b = probe();
    assert_success(b, true);
    assert(b.execute_tasks_code == module_base + EXEC_RVA);
  });

  // 3. NOT_READY is retriable and latches nothing: library absent, then domain down, then success.
  run("NOT_READY retries then success", [] {
    loaded = false;
    PgrBootBinding b = probe();
    assert(b.status == PGR_BOOT_NOT_READY && cleared(b, PGR_BOOT_NOT_READY, 0));
    loaded = true;
    domain_up = false;
    b = probe();
    assert(b.status == PGR_BOOT_NOT_READY && cleared(b, PGR_BOOT_NOT_READY, 0));
    assert(attach_calls == 0); // never attached while domain down
    domain_up = true;
    b = probe();
    assert_success(b, true);
  });

  // 4. Reentry + concurrency during a managed callback: BUSY, no deadlock, outer still succeeds.
  run("reentry/concurrency BUSY", [] {
    reenter = true;
    PgrBootBinding b = probe();
    assert(reentries == 1);
    assert_success(b, true);
  });

  // 5. Missing export fails closed and the precise status replays.
  run("missing export poison + replay", [] {
    missing_name = "il2cpp_runtime_invoke";
    expect_fail(PGR_BOOT_ERR_MISSING_EXPORT, 0);
    missing_name = nullptr; // even with the export now present, the owner stays poisoned
    expect_fail(PGR_BOOT_ERR_MISSING_EXPORT, 0);
    PgrBootBinding s;
    std::memset(&s, 0xa5, sizeof(s));
    assert(api->snapshot(&s) == PGR_BOOT_ERR_MISSING_EXPORT && cleared(s, PGR_BOOT_ERR_MISSING_EXPORT, 0));
  });

  // 6. Invalid module range (overflow) fails closed.
  run("module range poison", [] {
    static const void *bad_base = reinterpret_cast<void *>(UINTPTR_MAX - 3);
    base_config.module_fn = [](void *, const char *, const void **base, uint64_t *size) {
      *base = bad_base;
      *size = 8;
      return 0;
    };
    expect_fail(PGR_BOOT_ERR_MODULE_RANGE, 0);
  });

  // 7. Absent/incompatible feature 47 fails closed.
  run("no metadata poison", [] {
    base_config.metadata_fn = [](void *, uint32_t) -> const void * { return nullptr; };
    expect_fail(PGR_BOOT_ERR_NO_METADATA, 0);
  });

  // 8. Thread attach failure (domain up but attach returns null) fails closed.
  run("thread attach poison", [] {
    attach_result = nullptr;
    expect_fail(PGR_BOOT_ERR_THREAD, 0);
    assert(attach_calls == 1 && detach_calls == 0);
  });

  // 9. Already-attached thread: probe must not attach or detach.
  run("already attached thread", [] {
    thread_attached = true;
    PgrBootBinding b = probe();
    assert_success(b, /*attached*/ false);
    assert(attach_calls == 0 && detach_calls == 0);
  });

  // 10. feature 47 bind mismatch surfaces as ERR_BIND with the precise detail. Pre-bind 47 with a
  //     DIFFERENT module range so feature 54's compatible-config bind is rejected.
  run("bind mismatch poison", [] {
    const PgrMetaApi *md = static_cast<const PgrMetaApi *>(core->query_feature(47, 1));
    PgrMetaExports e{};
    void *stub = reinterpret_cast<void *>(&unused_stub);
    for (size_t i = 0; i < sizeof(PgrMetaExports) / sizeof(void *); ++i)
      reinterpret_cast<void **>(&e)[i] = stub;
    PgrMetaBindConfig bc{};
    bc.version = PGR_MD_CONFIG_VERSION;
    bc.size = sizeof(bc);
    bc.exports = &e;
    bc.module_base = module_base + module_size; // different base than feature 54 will present
    bc.module_size = module_size;
    bc.seg_query = seam_segment;
    assert(md->bind(&bc) == PGR_MD_OK);
    expect_fail(PGR_BOOT_ERR_BIND, PGR_MD_ERR_BOUND_MISMATCH);
  });

  // 11. Duplicate class across images -> ERR_RESOLVE(DUPLICATE).
  run("duplicate class poison", [] {
    duplicate = true;
    expect_fail(PGR_BOOT_ERR_RESOLVE, PGR_MD_ERR_DUPLICATE);
  });

  // 12. Non-static ExecuteTasks -> ERR_RESOLVE(STATIC).
  run("non-static method poison", [] {
    execute_tasks.flags = 0; // clear METHOD_ATTRIBUTE_STATIC
    expect_fail(PGR_BOOT_ERR_RESOLVE, PGR_MD_ERR_STATIC);
  });

  // 13. Prologue mismatch at the pinned code address -> ERR_RESOLVE(PROLOGUE).
  run("prologue mismatch poison", [] {
    base_config.exec_code_rva = EXEC_RVA;
    base_config.exec_prologue = PROLOGUE;
    base_config.exec_prologue_len = sizeof(PROLOGUE);
    module_base[EXEC_RVA] ^= 1;
    expect_fail(PGR_BOOT_ERR_RESOLVE, PGR_MD_ERR_PROLOGUE);
  });

  // 14. Code address outside the module range -> ERR_RESOLVE(CODE_RANGE) via the pinned-address check.
  run("code range poison", [] {
    base_config.exec_code_rva = module_size + 0x1000; // valid against base, outside the module span
    base_config.exec_prologue = PROLOGUE;
    base_config.exec_prologue_len = sizeof(PROLOGUE);
    expect_fail(PGR_BOOT_ERR_RESOLVE, PGR_MD_ERR_CODE_RANGE);
  });

  // 15. Config faults do NOT poison (caller programming errors) and give precise status.
  run("config faults, no poison", [] {
    PgrBootBinding b;
    std::memset(&b, 0xa5, sizeof(b));
    assert(api->probe(nullptr, &b) == PGR_BOOT_ERR_NULL_ARG && cleared(b, PGR_BOOT_ERR_NULL_ARG, 0));
    auto bad = [&](std::function<void(PgrBootConfig &)> mut, int status) {
      PgrBootConfig c = base_config;
      mut(c);
      PgrBootBinding o;
      std::memset(&o, 0xa5, sizeof(o));
      assert(api->probe(&c, &o) == status && cleared(o, status, 0));
    };
    bad([](PgrBootConfig &c) { c.version = 2; }, PGR_BOOT_ERR_BAD_VERSION);
    bad([](PgrBootConfig &c) { c.size -= 1; }, PGR_BOOT_ERR_BAD_SIZE);
    bad([](PgrBootConfig &c) { c.reserved = 2; }, PGR_BOOT_ERR_BAD_CONFIG);
    bad([](PgrBootConfig &c) { c.exec_code_rva = EXEC_RVA; }, PGR_BOOT_ERR_BAD_CONFIG); // rva, no prologue
    bad([](PgrBootConfig &c) { c.exec_prologue = PROLOGUE; c.exec_prologue_len = sizeof(PROLOGUE); },
        PGR_BOOT_ERR_BAD_CONFIG); // prologue, no rva
    // None of the above latched: a clean config now succeeds.
    assert_success(probe(), true);
    // Null out argument.
    assert(api->probe(&base_config, nullptr) == PGR_BOOT_ERR_NULL_ARG);
    assert(api->snapshot(nullptr) == PGR_BOOT_ERR_NULL_ARG);
  });

  // 16. snapshot before any probe is a clean NOT_READY with no dl*/il2cpp calls.
  run("snapshot before probe", [] {
    PgrBootBinding s;
    std::memset(&s, 0xa5, sizeof(s));
    assert(api->snapshot(&s) == PGR_BOOT_NOT_READY && cleared(s, PGR_BOOT_NOT_READY, 0));
    assert(attach_calls == 0 && names_allocated == 0);
  });

  run("render caller must already be attached", [] {
    base_config.reserved=1;
    PgrBootBinding b=probe();
    assert(b.status==PGR_BOOT_NOT_READY && attach_calls==0 && detach_calls==0);
    PgrBootConfig changed=base_config; changed.reserved=0;
    assert(api->probe(&changed,&b)==PGR_BOOT_ERR_BAD_CONFIG);
    thread_attached=true;
    assert_success(probe(),false);
    assert(attach_calls==0 && detach_calls==0);
  });
  run("ready immutable config and output copy", [] {
    assert_success(probe(), true);
    PgrBootBinding b{};
    PgrBootConfig c=base_config;
    c.exec_method="Other";
    assert(api->probe(&c,&b)==PGR_BOOT_ERR_BAD_CONFIG && cleared(b,PGR_BOOT_ERR_BAD_CONFIG,0));
    c=base_config; c.version=2;
    assert(api->probe(&c,&b)==PGR_BOOT_ERR_BAD_VERSION);
    assert(api->probe(nullptr,&b)==PGR_BOOT_ERR_NULL_ARG);
    b=probe(); b.execute_tasks=nullptr; b.meta.domain_get=nullptr;
    assert(api->snapshot(&b)==PGR_BOOT_OK); assert_success(b,true);
    assert(attach_calls==1 && detach_calls==1);
  });
  run("not ready immutable config", [] {
    domain_up=false; PgrBootBinding b=probe();
    assert(b.status==PGR_BOOT_NOT_READY);
    PgrBootConfig c=base_config; c.soname="other.so";
    assert(api->probe(&c,&b)==PGR_BOOT_ERR_BAD_CONFIG && cleared(b,PGR_BOOT_ERR_BAD_CONFIG,0));
    domain_up=true; assert_success(probe(),true);
  });
  run("ready prologue contents immutable", [] {
    unsigned char bytes[sizeof(PROLOGUE)]; std::memcpy(bytes,PROLOGUE,sizeof(bytes));
    base_config.exec_code_rva=EXEC_RVA; base_config.exec_prologue=bytes;
    base_config.exec_prologue_len=sizeof(bytes);
    assert_success(probe(),true);
    bytes[0]^=1; PgrBootBinding b{};
    assert(api->probe(&base_config,&b)==PGR_BOOT_ERR_BAD_CONFIG);
    bytes[0]^=1; assert_success(probe(),true);
  });

  assert(munmap(module_base, module_size) == 0);
  std::puts("PASS runtime bootstrap actual-library: success(+code identity), NOT_READY retry, reentry/"
            "concurrency BUSY, poison+replay (missing export/module range/no metadata/thread attach/"
            "bind mismatch/duplicate/non-static/prologue/code range), attach lifecycle, config faults, "
            "pull-only snapshot");
  // No hooks installed; the production library stays loaded until process exit. No production claim.
  return 0;
}

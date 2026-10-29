// Feature 54 (API 1): compiled native IL2CPP bootstrap binding. See runtime_bootstrap.h and
// .artifacts/native-migration/runtime-bootstrap-contract.txt for scope. No hooks, no Gum/Dobby,
// no JS callbacks, no Frida. Loads/discovers/validates only and returns a copied, bounded, opaque
// binding table. Never dlcloses the live game library.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 // RTLD_NOLOAD on glibc hosts (harness); Android NDK has it unconditionally
#endif

#include "runtime_bootstrap.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>

#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0x00004 // only a loaded library yields a handle; never force-load the game lib
#endif

namespace {

// Compiled default ExecuteTasks descriptor. DATA, not a guessed pointer. The exact namespace/class/
// method are the best-known Unity names and are OVERRIDABLE via PgrBootConfig; the coordinator must
// confirm them (and the strict export-name table below) against the shipped libil2cpp before relying
// on the production default path.
const char kExecNamespace[] = "UnityEngine";
const char kExecClass[]     = "UnitySynchronizationContext";
const char kExecMethod[]    = "ExecuteTasks";

// Strict supported il2cpp export names, in the FIXED order of PgrMetaExports / PgrBootRuntimeExports.
// This is the single reviewable list of symbols dlsym'd from the real libil2cpp. The fixture harness
// maps these names to fake functions through the dlsym test seam; it does not need a real library.
struct SymSlot { size_t off; const char *name; };

const SymSlot kMetaSyms[] = {
  {offsetof(PgrMetaExports, domain_get),                 "il2cpp_domain_get"},
  {offsetof(PgrMetaExports, domain_get_assemblies),      "il2cpp_domain_get_assemblies"},
  {offsetof(PgrMetaExports, assembly_get_image),         "il2cpp_assembly_get_image"},
  {offsetof(PgrMetaExports, class_from_name),            "il2cpp_class_from_name"},
  {offsetof(PgrMetaExports, class_get_type),             "il2cpp_class_get_type"},
  {offsetof(PgrMetaExports, type_get_name),              "il2cpp_type_get_name"},
  {offsetof(PgrMetaExports, mem_free),                   "il2cpp_free"},
  {offsetof(PgrMetaExports, class_get_field_from_name),  "il2cpp_class_get_field_from_name"},
  {offsetof(PgrMetaExports, field_get_parent),           "il2cpp_field_get_parent"},
  {offsetof(PgrMetaExports, field_get_flags),            "il2cpp_field_get_flags"},
  {offsetof(PgrMetaExports, field_get_offset),           "il2cpp_field_get_offset"},
  {offsetof(PgrMetaExports, field_get_type),             "il2cpp_field_get_type"},
  {offsetof(PgrMetaExports, class_from_type),            "il2cpp_class_from_type"},
  {offsetof(PgrMetaExports, class_is_valuetype),         "il2cpp_class_is_valuetype"},
  {offsetof(PgrMetaExports, class_is_enum),              "il2cpp_class_is_enum"},
  {offsetof(PgrMetaExports, class_enum_basetype),        "il2cpp_class_enum_basetype"},
  {offsetof(PgrMetaExports, class_value_size),           "il2cpp_class_value_size"},
  {offsetof(PgrMetaExports, class_get_method_from_name), "il2cpp_class_get_method_from_name"},
  {offsetof(PgrMetaExports, method_get_class),           "il2cpp_method_get_class"},
  {offsetof(PgrMetaExports, method_get_name),            "il2cpp_method_get_name"},
  {offsetof(PgrMetaExports, method_get_flags),           "il2cpp_method_get_flags"},
  {offsetof(PgrMetaExports, method_get_param_count),     "il2cpp_method_get_param_count"},
  {offsetof(PgrMetaExports, method_get_param),           "il2cpp_method_get_param"},
  {offsetof(PgrMetaExports, method_get_return_type),     "il2cpp_method_get_return_type"},
  {offsetof(PgrMetaExports, method_is_generic),          "il2cpp_method_is_generic"},
  {offsetof(PgrMetaExports, method_is_inflated),         "il2cpp_method_is_inflated"},
  {offsetof(PgrMetaExports, debug_get_method_info),      "il2cpp_debug_get_method_info"},
  {offsetof(PgrMetaExports, class_is_assignable_from),   "il2cpp_class_is_assignable_from"},
  {offsetof(PgrMetaExports, class_get_methods),          "il2cpp_class_get_methods"},
  {offsetof(PgrMetaExports, class_get_nested_types),     "il2cpp_class_get_nested_types"},
  {offsetof(PgrMetaExports, class_get_declaring_type),   "il2cpp_class_get_declaring_type"},
};
static_assert(sizeof(kMetaSyms) / sizeof(kMetaSyms[0]) == sizeof(PgrMetaExports) / sizeof(void *),
              "metadata symbol table must cover every PgrMetaExports slot");

const SymSlot kRuntimeSyms[] = {
  {offsetof(PgrBootRuntimeExports, runtime_invoke),              "il2cpp_runtime_invoke"},
  {offsetof(PgrBootRuntimeExports, runtime_invoke_convert_args), "il2cpp_runtime_invoke_convert_args"},
  {offsetof(PgrBootRuntimeExports, thread_attach),              "il2cpp_thread_attach"},
  {offsetof(PgrBootRuntimeExports, thread_detach),              "il2cpp_thread_detach"},
  {offsetof(PgrBootRuntimeExports, thread_current),             "il2cpp_thread_current"},
  {offsetof(PgrBootRuntimeExports, gchandle_new),               "il2cpp_gchandle_new"},
  {offsetof(PgrBootRuntimeExports, gchandle_new_weakref),       "il2cpp_gchandle_new_weakref"},
  {offsetof(PgrBootRuntimeExports, gchandle_get_target),        "il2cpp_gchandle_get_target"},
  {offsetof(PgrBootRuntimeExports, gchandle_free),              "il2cpp_gchandle_free"},
  {offsetof(PgrBootRuntimeExports, field_get_value),            "il2cpp_field_get_value"},
  {offsetof(PgrBootRuntimeExports, field_set_value),            "il2cpp_field_set_value"},
  {offsetof(PgrBootRuntimeExports, field_static_get_value),     "il2cpp_field_static_get_value"},
  {offsetof(PgrBootRuntimeExports, field_static_set_value),     "il2cpp_field_static_set_value"},
  {offsetof(PgrBootRuntimeExports, object_new),                 "il2cpp_object_new"},
  {offsetof(PgrBootRuntimeExports, object_unbox),               "il2cpp_object_unbox"},
  {offsetof(PgrBootRuntimeExports, object_get_class),           "il2cpp_object_get_class"},
  {offsetof(PgrBootRuntimeExports, value_box),                  "il2cpp_value_box"},
  {offsetof(PgrBootRuntimeExports, string_new),                 "il2cpp_string_new"},
  {offsetof(PgrBootRuntimeExports, string_chars),               "il2cpp_string_chars"},
  {offsetof(PgrBootRuntimeExports, array_new),                  "il2cpp_array_new"},
};
static_assert(sizeof(kRuntimeSyms) / sizeof(kRuntimeSyms[0]) ==
                  sizeof(PgrBootRuntimeExports) / sizeof(void *),
              "runtime symbol table must cover every PgrBootRuntimeExports slot");

// Process-lifetime singleton. phase: 0 unbound, 1 ready (cached valid), 2 poisoned. Guarded by the
// NON-BLOCKING atomic entry flag below: only one thread is ever inside a mutating API, and a reentrant
// managed callback gets BUSY instead of a deadlock (no mutex is held across any il2cpp/dl* call).
struct Identity {
  PgrBootConfig cfg{};
  char names[5][256]{};
  unsigned char prologue[PGR_MD_PROLOGUE_MAX]{};
};
struct State {
  bool identified = false;
  Identity identity{};
  int phase = 0;
  int poison_status = 0;
  int poison_detail = 0;
  void *handle = nullptr;   // kept for the process lifetime; NEVER dlclosed
  PgrBootBinding cached{};
};
State g;
std::atomic<uint32_t> g_entry{0};

struct Guard {
  bool ok;
  Guard() {
    uint32_t expected = 0;
    ok = g_entry.compare_exchange_strong(expected, 1u, std::memory_order_acquire,
                                          std::memory_order_relaxed);
  }
  ~Guard() {
    if (ok) g_entry.store(0u, std::memory_order_release);
  }
  Guard(const Guard &) = delete;
  Guard &operator=(const Guard &) = delete;
};

void fill_header(PgrBootBinding *out, int status, int detail) {
  std::memset(out, 0, sizeof(*out));
  out->version = PGR_BOOT_BINDING_VERSION;
  out->size = sizeof(PgrBootBinding);
  out->status = status;
  out->detail = detail;
}

int poison(PgrBootBinding *out, int status, int detail) {
  g.phase = 2;
  g.poison_status = status;
  g.poison_detail = detail;
  fill_header(out, status, detail);
  return status;
}

void *resolve_sym(const PgrBootConfig *cfg, void *handle, const char *name) {
  return cfg->dlsym_fn ? cfg->dlsym_fn(cfg->seam_ctx, handle, name) : ::dlsym(handle, name);
}

bool resolve_symbols(const PgrBootConfig *cfg, void *handle, PgrMetaExports *meta,
                     PgrBootRuntimeExports *rt) {
  for (const SymSlot &slot : kMetaSyms) {
    void *s = resolve_sym(cfg, handle, slot.name);
    if (!s) return false;
    *reinterpret_cast<void **>(reinterpret_cast<char *>(meta) + slot.off) = s;
  }
  for (const SymSlot &slot : kRuntimeSyms) {
    void *s = resolve_sym(cfg, handle, slot.name);
    if (!s) return false;
    *reinterpret_cast<void **>(reinterpret_cast<char *>(rt) + slot.off) = s;
  }
  return true;
}

// Resolve+validate ExecuteTasks THROUGH feature 47. Binds 47 with the DEFAULT segment resolver and NO
// debug prologue in production (cfg->seg_query/seg_ctx null) so the config is identical to Routes 53's;
// 47's bind is idempotent for an identical config, so both owners coexist. Returns 0 on success, or a
// PgrBootStatus (ERR_BIND / ERR_RESOLVE) with *detail set to the underlying PgrMetaStatus.
int resolve_execute_tasks(const PgrBootConfig *cfg, const PgrMetaApi *md, const PgrMetaExports *meta,
                          const void *base, uint64_t size, const void **out_method,
                          const void **out_code, int *detail) {
  *out_method = nullptr;
  *out_code = nullptr;
  *detail = 0;

  PgrMetaBindConfig bc{};
  bc.version = PGR_MD_CONFIG_VERSION;
  bc.size = sizeof(bc);
  bc.exports = meta;
  bc.module_base = base;
  bc.module_size = size;
  bc.seg_query = cfg->seg_query; // null in production -> 47 default resolver (Routes 53 compatible)
  bc.seg_ctx = cfg->seg_ctx;
  bc.debug_prologue = nullptr;
  bc.debug_prologue_len = 0;
  int rc = md->bind(&bc);
  if (rc != 0) {
    *detail = rc;
    return PGR_BOOT_ERR_BIND;
  }

  PgrMetaClassQuery cq{};
  cq.version = PGR_MD_QUERY_VERSION;
  cq.size = sizeof(cq);
  cq.name_space = cfg->exec_namespace ? cfg->exec_namespace : kExecNamespace;
  cq.name = cfg->exec_class ? cfg->exec_class : kExecClass;
  cq.canonical = cfg->exec_canonical; // null -> ns.name EXACT (feature 47 default)
  cq.canonical_match = cfg->exec_canonical ? PGR_MD_MATCH_EXACT : PGR_MD_MATCH_ANY;
  PgrMetaClassResult cr{};
  rc = md->resolve_class(&cq, &cr);
  if (rc != 0) {
    *detail = rc;
    return PGR_BOOT_ERR_RESOLVE;
  }

  PgrMetaMethodQuery mq{};
  mq.version = PGR_MD_QUERY_VERSION;
  mq.size = sizeof(mq);
  mq.owner = cr.klass;
  mq.name = cfg->exec_method ? cfg->exec_method : kExecMethod;
  mq.return_type = "System.Void"; // static System.Void ExecuteTasks()
  mq.return_match = PGR_MD_MATCH_EXACT;
  mq.param_count = 0;
  mq.is_static = 1;
  mq.allow_inflated = 0; // generic is always rejected by feature 47
  if (cfg->exec_code_rva != 0) {
    const uintptr_t b = reinterpret_cast<uintptr_t>(base);
    if (cfg->exec_code_rva > UINTPTR_MAX - b) { // base+rva overflow -> treat as a resolution fault
      *detail = PGR_MD_ERR_CODE_RANGE;
      return PGR_BOOT_ERR_RESOLVE;
    }
    mq.expected_code = reinterpret_cast<const void *>(b + cfg->exec_code_rva);
    mq.prologue = cfg->exec_prologue;
    mq.prologue_len = cfg->exec_prologue_len;
  }
  PgrMetaMethodResult mr{};
  rc = md->validate_method(&mq, &mr);
  if (rc != 0) {
    *detail = rc;
    return PGR_BOOT_ERR_RESOLVE;
  }
  *out_method = mr.method;
  *out_code = mr.code;
  return 0;
}

// Copy bounded descriptor DATA, never retain caller-owned strings/prologue.
bool identity(const PgrBootConfig *c, Identity *out) {
  out->cfg = *c;
  const char *names[] = {c->soname ? c->soname : "libil2cpp.so",
    c->exec_namespace ? c->exec_namespace : kExecNamespace,
    c->exec_class ? c->exec_class : kExecClass,
    c->exec_canonical ? c->exec_canonical : "",
    c->exec_method ? c->exec_method : kExecMethod};
  for (unsigned i=0;i<5;++i) {
    size_t n=0; while(n<sizeof(out->names[i]) && names[i][n]) ++n;
    if(n==sizeof(out->names[i]) || (i!=1 && i!=3 && n==0)) return false;
    std::memcpy(out->names[i],names[i],n+1);
  }
  if(c->exec_prologue_len) std::memcpy(out->prologue,c->exec_prologue,c->exec_prologue_len);
  return true;
}
bool same_identity(const Identity &a,const Identity &b) {
  const auto &x=a.cfg; const auto &y=b.cfg;
  return std::memcmp(a.names,b.names,sizeof(a.names))==0 &&
    x.dlopen_fn==y.dlopen_fn && x.dlsym_fn==y.dlsym_fn && x.module_fn==y.module_fn &&
    x.seam_ctx==y.seam_ctx && x.metadata_fn==y.metadata_fn && x.seg_query==y.seg_query &&
    x.seg_ctx==y.seg_ctx && x.exec_code_rva==y.exec_code_rva &&
    x.exec_prologue_len==y.exec_prologue_len && x.reserved==y.reserved &&
    std::memcmp(a.prologue,b.prologue,x.exec_prologue_len)==0;
}

int api_probe(const PgrBootConfig *cfg, PgrBootBinding *out) {
  if (out == nullptr) return PGR_BOOT_ERR_NULL_ARG;
  Guard guard;
  if (!guard.ok) { // concurrent/reentrant probe; shared state untouched
    fill_header(out, PGR_BOOT_ERR_BUSY, 0);
    return PGR_BOOT_ERR_BUSY;
  }

  // Config validation. These are caller programming errors, not the environment; they do NOT poison.
  if (cfg == nullptr) { fill_header(out, PGR_BOOT_ERR_NULL_ARG, 0); return PGR_BOOT_ERR_NULL_ARG; }
  if (cfg->version != PGR_BOOT_CONFIG_VERSION) {
    fill_header(out, PGR_BOOT_ERR_BAD_VERSION, 0);
    return PGR_BOOT_ERR_BAD_VERSION;
  }
  if (cfg->size != sizeof(PgrBootConfig)) {
    fill_header(out, PGR_BOOT_ERR_BAD_SIZE, 0);
    return PGR_BOOT_ERR_BAD_SIZE;
  }
  if (cfg->reserved & ~1u) { fill_header(out, PGR_BOOT_ERR_BAD_CONFIG, 0); return PGR_BOOT_ERR_BAD_CONFIG; }
  if (cfg->exec_code_rva != 0) {
    if (cfg->exec_prologue == nullptr || cfg->exec_prologue_len == 0 ||
        cfg->exec_prologue_len > PGR_MD_PROLOGUE_MAX) {
      fill_header(out, PGR_BOOT_ERR_BAD_CONFIG, 0);
      return PGR_BOOT_ERR_BAD_CONFIG;
    }
  } else if (cfg->exec_prologue != nullptr || cfg->exec_prologue_len != 0) {
    fill_header(out, PGR_BOOT_ERR_BAD_CONFIG, 0);
    return PGR_BOOT_ERR_BAD_CONFIG;
  }

  Identity candidate{};
  if (!identity(cfg, &candidate) || (g.identified && !same_identity(candidate,g.identity))) {
    fill_header(out,PGR_BOOT_ERR_BAD_CONFIG,0); return PGR_BOOT_ERR_BAD_CONFIG;
  }
  if (g.phase == 1) { *out = g.cached; return PGR_BOOT_OK; }
  if (g.phase == 2) { fill_header(out,g.poison_status,g.poison_detail); return g.poison_status; }
  if (!g.identified) { g.identity=candidate; g.identified=true; }

  const char *soname = cfg->soname ? cfg->soname : "libil2cpp.so";

  // 1. Module handle via dlopen RTLD_NOLOAD. Obtained at most once and kept for the process lifetime;
  //    reused across NOT_READY retries so polling never grows the refcount. Null -> not loaded yet.
  void *handle = g.handle;
  if (handle == nullptr) {
    handle = cfg->dlopen_fn ? cfg->dlopen_fn(cfg->seam_ctx, soname, RTLD_NOLOAD | RTLD_NOW)
                            : ::dlopen(soname, RTLD_NOLOAD | RTLD_NOW);
    if (handle == nullptr) { // library not yet loaded; RETRIABLE, nothing latched
      fill_header(out, PGR_BOOT_NOT_READY, 0);
      return PGR_BOOT_NOT_READY;
    }
    g.handle = handle;
  }

  // 2. dlsym the strict supported export set. A missing export is an unsupported build -> fail closed.
  PgrMetaExports meta{};
  PgrBootRuntimeExports rt{};
  if (!resolve_symbols(cfg, handle, &meta, &rt)) return poison(out, PGR_BOOT_ERR_MISSING_EXPORT, 0);

  // 3. feature 47 metadata API.
  const void *raw = cfg->metadata_fn ? cfg->metadata_fn(cfg->seam_ctx, PGR_MD_API_VERSION)
                                     : pgrazor_runtime_metadata_api(PGR_MD_API_VERSION);
  const PgrMetaApi *md = static_cast<const PgrMetaApi *>(raw);
  if (md == nullptr || md->version != PGR_MD_API_VERSION || md->size != sizeof(PgrMetaApi) ||
      !md->bind || !md->resolve_module || !md->resolve_class || !md->validate_method || !md->validate_code)
    return poison(out, PGR_BOOT_ERR_NO_METADATA, 0);

  // 4. Real loaded-module range. Null module_fn delegates to 47 (same range Routes 53 binds against).
  const void *base = nullptr;
  uint64_t size = 0;
  int mrc = cfg->module_fn ? cfg->module_fn(cfg->seam_ctx, soname, &base, &size)
                           : md->resolve_module(soname, &base, &size);
  if(mrc==PGR_MD_ERR_BUSY){fill_header(out,PGR_BOOT_ERR_BUSY,mrc);return PGR_BOOT_ERR_BUSY;}
  if (mrc != 0 || base == nullptr || size == 0 ||
      size > UINTPTR_MAX - reinterpret_cast<uintptr_t>(base))
    return poison(out, PGR_BOOT_ERR_MODULE_RANGE, mrc);

  // Validate real dlsym addresses BEFORE calling domain/thread functions. Native
  // fixture callbacks intentionally live outside their synthetic module image.
  if(!cfg->dlopen_fn&&!cfg->dlsym_fn&&!cfg->module_fn&&!cfg->metadata_fn&&!cfg->seg_query){
    PgrMetaBindConfig binding{};binding.version=1;binding.size=sizeof(binding);
    binding.exports=&meta;binding.module_base=base;binding.module_size=size;
    const int bound=md->bind(&binding);
    if(bound==PGR_MD_ERR_BUSY){fill_header(out,PGR_BOOT_ERR_BUSY,bound);return PGR_BOOT_ERR_BUSY;}
    if(bound)return poison(out,PGR_BOOT_ERR_BIND,bound);
    for(const auto &entry:kMetaSyms){
      const void *address=nullptr;std::memcpy(&address,reinterpret_cast<const char *>(&meta)+entry.off,sizeof(address));
      PgrMetaCodeQuery query{};query.version=1;query.size=sizeof(query);query.address=address;query.length=4;
      const int checked=md->validate_code(&query);
      if(checked==PGR_MD_ERR_BUSY){fill_header(out,PGR_BOOT_ERR_BUSY,checked);return PGR_BOOT_ERR_BUSY;}
      if(checked)return poison(out,PGR_BOOT_ERR_EXPORT_CODE,checked);
    }
    for(const auto &entry:kRuntimeSyms){
      const void *address=nullptr;std::memcpy(&address,reinterpret_cast<const char *>(&rt)+entry.off,sizeof(address));
      PgrMetaCodeQuery query{};query.version=1;query.size=sizeof(query);query.address=address;query.length=4;
      const int checked=md->validate_code(&query);
      if(checked==PGR_MD_ERR_BUSY){fill_header(out,PGR_BOOT_ERR_BUSY,checked);return PGR_BOOT_ERR_BUSY;}
      if(checked)return poison(out,PGR_BOOT_ERR_EXPORT_CODE,checked);
    }
  }

  // 5. Managed domain must be up. Null -> RETRIABLE (runtime not initialized yet), nothing latched.
  void *self = reinterpret_cast<void *(*)()>(rt.thread_current)();
  if((cfg->reserved&1u)&&!self){fill_header(out,PGR_BOOT_NOT_READY,0);return PGR_BOOT_NOT_READY;}
  void *domain = reinterpret_cast<void *(*)()>(meta.domain_get)();
  if (domain == nullptr) {
    fill_header(out, PGR_BOOT_NOT_READY, 0);
    return PGR_BOOT_NOT_READY;
  }

  // 6. Attach the calling (native worker) thread if it is not already attached, resolve ExecuteTasks
  //    through 47, then detach iff we attached. No spin thread; no mutex held across these calls.
  bool attached = false;
  if (self == nullptr) {
    self = reinterpret_cast<void *(*)(void *)>(rt.thread_attach)(domain);
    if (self == nullptr) return poison(out, PGR_BOOT_ERR_THREAD, 0);
    attached = true;
  }

  const void *method = nullptr;
  const void *code = nullptr;
  int detail = 0;
  int rc = resolve_execute_tasks(cfg, md, &meta, base, size, &method, &code, &detail);

  if (attached) reinterpret_cast<void (*)(void *)>(rt.thread_detach)(self);

  if(detail==PGR_MD_ERR_BUSY){fill_header(out,PGR_BOOT_ERR_BUSY,detail);return PGR_BOOT_ERR_BUSY;}
  if (rc != 0) return poison(out, rc, detail);

  // 7. Publish the copied, bounded binding table and latch READY.
  PgrBootBinding &b = g.cached;
  std::memset(&b, 0, sizeof(b));
  b.version = PGR_BOOT_BINDING_VERSION;
  b.size = sizeof(PgrBootBinding);
  b.status = PGR_BOOT_OK;
  b.detail = 0;
  b.flags = attached ? 1u : 0u;
  b.handle = handle;
  b.module_base = base;
  b.module_size = size;
  b.domain = domain;
  b.execute_tasks = method;
  b.execute_tasks_code = code;
  b.meta = meta;
  b.runtime = rt;
  g.phase = 1;

  *out = b;
  return PGR_BOOT_OK;
}

int api_snapshot(PgrBootBinding *out) {
  if (out == nullptr) return PGR_BOOT_ERR_NULL_ARG;
  Guard guard;
  if (!guard.ok) {
    fill_header(out, PGR_BOOT_ERR_BUSY, 0);
    return PGR_BOOT_ERR_BUSY;
  }
  if (g.phase == 1) { *out = g.cached; return PGR_BOOT_OK; }
  if (g.phase == 2) {
    fill_header(out, g.poison_status, g.poison_detail);
    return g.poison_status;
  }
  fill_header(out, PGR_BOOT_NOT_READY, 0);
  return PGR_BOOT_NOT_READY;
}

const PgrBootApi g_api = {PGR_BOOT_API_VERSION, sizeof(PgrBootApi), api_probe, api_snapshot};

} // namespace

const void *pgrazor_runtime_bootstrap_api(uint32_t version) {
  return version == PGR_BOOT_API_VERSION ? &g_api : nullptr;
}

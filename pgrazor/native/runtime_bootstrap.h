#pragma once
#include <stddef.h>
#include <stdint.h>
#include "runtime_metadata.h" // PgrMetaExports / pgr_meta_seg_fn (feature47) reused directly

// Feature 54 (API 1). COMPILED native IL2CPP bootstrap binding. This is the compiled C++ replacement
// for the Frida Module/export DISCOVERY embedded in runtime/native-core.js (Module.getGlobalExportByName
// + dlopen/dlsym, which can stall under MuMu after a new ARM library loads). It resolves the real
// libil2cpp mapping with the GUEST dl* functions, dlsym's a FIXED, strictly-typed table of supported
// il2cpp exports, validates the module base/size, and resolves the single static System.Void
// UnitySynchronizationContext.ExecuteTasks() MethodInfo THROUGH feature 47 so later native orchestration
// (the feature 48 UnitySynchronizationContext scheduler) can be handed a ready binding table without
// touching Frida at all.
//
// SCOPE / NON-GOALS (see .artifacts/native-migration/runtime-bootstrap-contract.txt for full rationale):
//  * NO hooks, NO Gum/Dobby, NO trampolines, NO GC roots, NO JS callbacks, NO Frida requirement. This
//    module only LOADS/DISCOVERS/VALIDATES and returns a copied, bounded, opaque binding table. It does
//    not install or retire anything. The active Gum scheduler owners (quick-close.js / quick-ball.js) are
//    NOT removed by this; nothing here claims the JS scheduler or full bootstrap is retired.
//  * ONE polling probe. It never spawns a background/spin thread. While the library is not yet loaded
//    (dlopen RTLD_NOLOAD returns null) or the managed domain is not up (il2cpp_domain_get() == null) it
//    returns PGR_BOOT_NOT_READY (a positive, RETRIABLE status) without a ready binding; config identity is retained and the caller may retry.
//    A malformed / unsupported environment (missing export, bad module range, absent/incompatible
//    feature 47, bind mismatch, failed ExecuteTasks validation, thread-attach failure) fails CLOSED with a
//    precise negative status, clears the binding output, and permanently poisons this owner. Caller config errors and BUSY do not poison.
//  * It binds the feature 47 metadata singleton with the DEFAULT segment resolver and NO debug prologue
//    in production, i.e. the SAME config Routes feature 53 (routes_metadata) uses. feature 47 bind is
//    idempotent for an identical config, so feature 54 and feature 53 coexist; feature 54 must never bind
//    47 with an incompatible config that would lock feature 53 out. The seg_query/seg_ctx test seam below
//    is for the standalone fixture only and MUST be null in production.
//  * Keeps the dlopen handle for the process lifetime to protect the library mapping; it NEVER dlcloses
//    the live game library.
//  * NO production-readiness claim. A successful compile or fixture-harness run is NOT evidence that real
//    translated-ARM64 resolution under Houdini on the game works. The default module/segment resolver is
//    feature 47's (UNVERIFIED on real Houdini per the feature 47 contract).
//  * Internal C++ interface: the single hidden getter is NOT added to pgrazor.map; the coordinator
//    registers it under query_feature(54) (same pattern as features 24/43/47/53).

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "RuntimeBootstrap native ABI requires LP64");

enum PgrBootInfo {
  PGR_BOOT_FEATURE_ID      = 54,
  PGR_BOOT_API_VERSION     = 1,
  PGR_BOOT_CONFIG_VERSION  = 1,
  PGR_BOOT_BINDING_VERSION = 1
};

// probe()/snapshot() status. 0 == OK. PGR_BOOT_NOT_READY is a POSITIVE, RETRIABLE status (library or
// managed domain not yet available). Environment failures permanently poison this owner. Caller argument/config faults and BUSY
// leave the existing state untouched. On any non-OK status the binding output is cleared except its
// {version,size,status,detail} header, where detail carries the underlying feature 47 PgrMetaStatus for
// bind/resolve failures (else 0).
enum PgrBootStatus {
  PGR_BOOT_OK                 = 0,
  PGR_BOOT_NOT_READY          = 1,   // dlopen(NOLOAD)==null or domain==null; retry later, nothing latched
  PGR_BOOT_ERR_NULL_ARG       = -1,  // required pointer argument was null
  PGR_BOOT_ERR_BAD_VERSION    = -2,  // config version != PGR_BOOT_CONFIG_VERSION
  PGR_BOOT_ERR_BAD_SIZE       = -3,  // config/binding size mismatch
  PGR_BOOT_ERR_BAD_CONFIG     = -4,  // reserved!=0, prologue without rva, rva without prologue, etc.
  PGR_BOOT_ERR_BUSY           = -5,  // concurrent/reentrant probe; shared state untouched
  PGR_BOOT_ERR_MISSING_EXPORT = -6,  // a required il2cpp export dlsym'd to null (unsupported build)
  PGR_BOOT_ERR_MODULE_RANGE   = -7,  // module base/size null, zero, or overflowing
  PGR_BOOT_ERR_NO_METADATA    = -8,  // feature 47 getter absent or wrong ABI version
  PGR_BOOT_ERR_BIND           = -9,  // feature 47 bind failed/mismatch (detail = PgrMetaStatus)
  PGR_BOOT_ERR_RESOLVE        = -10, // ExecuteTasks class/method validation failed (detail = PgrMetaStatus)
  PGR_BOOT_ERR_THREAD         = -11, // il2cpp_thread_attach returned null after domain was up
  PGR_BOOT_ERR_EXPORT_CODE    = -13, // production export outside validated executable module
  PGR_BOOT_ERR_POISONED       = -12  // reserved. Once poisoned, probe()/snapshot() REPLAY the original
                                     // precise fail-closed status+detail (deterministic); this code is
                                     // reserved for a future caller that prefers a single latched code.
};

// GUEST dl* test seam. In production every callback is null and the module uses the real guest ::dlopen
// / ::dlsym and feature 47's default dl_iterate_phdr/maps module resolver. The compiled fixture harness
// injects these as native C++ function pointers ONLY; they never cross JSON.
typedef void *(*pgr_boot_dlopen_fn)(void *ctx, const char *soname, int flags);
typedef void *(*pgr_boot_dlsym_fn)(void *ctx, void *handle, const char *name);
// module resolver: return 0 and fill base/size when the module range was found, negative on fault.
// null -> delegate to feature 47 resolve_module(soname,...) (same range Routes 53 binds against).
typedef int (*pgr_boot_module_fn)(void *ctx, const char *soname, const void **base, uint64_t *size);
// feature 47 getter seam: null -> pgrazor_runtime_metadata_api. The fixture passes the REAL getter so the
// real feature 47 validators run over a fake il2cpp universe.
typedef const void *(*pgr_boot_metadata_fn)(void *ctx, uint32_t version);

// probe() configuration. First valid probe fixes process identity, including NOT_READY.
// Strings are bounded to255 bytes and compared by copied contents; prologue compared by bytes.
// Changed identity is BAD_CONFIG without invalidating the existing binding. Every call validates
// version/size/null config, including READY. snapshot has no configuration argument.
struct PgrBootConfig {
  uint32_t version;              // == PGR_BOOT_CONFIG_VERSION
  uint32_t size;                 // == sizeof(PgrBootConfig)
  const char *soname;            // null -> "libil2cpp.so"
  // Test seams (all null in production):
  pgr_boot_dlopen_fn dlopen_fn;  // null -> ::dlopen(soname, RTLD_NOLOAD|RTLD_NOW)
  pgr_boot_dlsym_fn dlsym_fn;    // null -> ::dlsym
  pgr_boot_module_fn module_fn;  // null -> feature 47 resolve_module
  void *seam_ctx;                // ctx passed to dlopen_fn/dlsym_fn/module_fn
  pgr_boot_metadata_fn metadata_fn; // null -> pgrazor_runtime_metadata_api
  pgr_meta_seg_fn seg_query;     // feature 47 bind segment resolver; MUST be null in production
  void *seg_ctx;                 // ctx for seg_query; MUST be null in production
  // ExecuteTasks descriptor (DATA, never a guessed pointer). Null string -> compiled default.
  const char *exec_namespace;    // null -> default ("UnityEngine")
  const char *exec_class;        // null -> default ("UnitySynchronizationContext")
  const char *exec_canonical;    // null -> ns.name EXACT
  const char *exec_method;       // null -> default ("ExecuteTasks")
  const void *exec_prologue;     // required iff exec_code_rva != 0; else null
  uint64_t exec_code_rva;        // 0 -> skip compiled-address identity (fixture default); else base+rva
  uint32_t exec_prologue_len;    // 0 iff exec_code_rva==0; else 1..PGR_MD_PROLOGUE_MAX
  uint32_t reserved;             // bit0: require an already-attached caller; no thread_attach
};
static_assert(sizeof(PgrBootConfig) == 128, "PgrBootConfig must remain 128 bytes on LP64");

// The extra typed il2cpp runtime exports beyond feature 47's metadata set, needed by native gameplay
// orchestration and the feature 48 scheduler. Fixed order; stored as void* and reinterpret_cast by the
// consumer. Discovered by dlsym alongside the metadata exports; every slot must be non-null.
struct PgrBootRuntimeExports {
  void *runtime_invoke;              //  0  void* (MethodInfo*, void* obj, void** params, void** exc)
  void *runtime_invoke_convert_args; //  1  void* (MethodInfo*, void* obj, void** params, int, void** exc)
  void *thread_attach;               //  2  void* (void* domain)
  void *thread_detach;               //  3  void  (void* thread)
  void *thread_current;              //  4  void* (void)
  void *gchandle_new;                //  5  uintptr_t(void* obj, int pinned), fingerprint before use
  void *gchandle_new_weakref;        //  6  uintptr_t(void* obj, int track_resurrection)
  void *gchandle_get_target;         //  7  void* (uintptr_t handle)
  void *gchandle_free;               //  8  void  (uintptr_t handle)
  void *field_get_value;             //  9  void  (void* obj, FieldInfo*, void* out)
  void *field_set_value;             // 10  void  (void* obj, FieldInfo*, void* val)
  void *field_static_get_value;      // 11  void  (FieldInfo*, void* out)
  void *field_static_set_value;      // 12  void  (FieldInfo*, void* val)
  void *object_new;                  // 13  void* (void* klass)
  void *object_unbox;                // 14  void* (void* obj)
  void *object_get_class;            // 15  void* (void* obj)
  void *value_box;                   // 16  void* (void* klass, void* val)
  void *string_new;                  // 17  void* (const char*)
  void *string_chars;                // 18  void* (void* str)
  void *array_new;                   // 19  void* (void* element_class, uintptr_t length)
};
static_assert(sizeof(PgrBootRuntimeExports) == 160, "PgrBootRuntimeExports must remain 20 LP64 pointers");

// The copied, bounded, opaque binding table returned by probe()/snapshot(). All pointers are stable
// il2cpp handles/exports (this module roots nothing). The caller copies it out; no internal pointer
// escapes. {version,size,status,detail,flags} are ALWAYS written; everything else is zero unless status
// == PGR_BOOT_OK.
struct PgrBootBinding {
  uint32_t version;                // == PGR_BOOT_BINDING_VERSION
  uint32_t size;                   // == sizeof(PgrBootBinding)
  int32_t status;                  // mirrors the probe()/snapshot() return
  int32_t detail;                  // underlying PgrMetaStatus for BIND/RESOLVE failures, else 0
  uint32_t flags;                  // bit0: probe attached+detached a native worker thread this call
  uint32_t reserved;               // 0
  void *handle;                    // opaque dlopen handle (kept for process lifetime; never dlclosed)
  const void *module_base;         // validated libil2cpp mapping base
  uint64_t module_size;            // module mapping size (> 0)
  const void *domain;              // il2cpp_domain_get() at bind time
  const void *execute_tasks;       // validated UnitySynchronizationContext.ExecuteTasks MethodInfo*
  const void *execute_tasks_code;  // compiled address when exec_code_rva was supplied, else null
  PgrMetaExports meta;             // the 31 discovered metadata exports (feed feature 47 bind directly)
  PgrBootRuntimeExports runtime;   // the extra runtime/gameplay/scheduler exports
};
static_assert(sizeof(PgrBootBinding) == 480, "PgrBootBinding must remain 480 bytes on LP64");

// Fixed LP64 table: 8-byte header + 2 function pointers = 24 bytes.
//  probe:    perform (or re-attempt) discovery/validation; returns PgrBootStatus. Serialized by a
//            NON-BLOCKING atomic entry flag (never a mutex held across a managed/il2cpp call), so a
//            reentrant managed callback or a concurrent thread gets PGR_BOOT_ERR_BUSY, not a deadlock.
//  snapshot: pull-only. Returns the cached binding when READY, else NOT_READY / the poisoned status.
//            Makes NO dl*/il2cpp calls and never changes state.
struct PgrBootApi {
  uint32_t version;  // == PGR_BOOT_API_VERSION
  uint32_t size;     // == sizeof(PgrBootApi)
  int (*probe)(const PgrBootConfig *config, PgrBootBinding *out);
  int (*snapshot)(PgrBootBinding *out);
};
static_assert(sizeof(PgrBootApi) == 24, "PgrBootApi must remain a 24-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 54 and answers only on an exact version match.
const void *pgrazor_runtime_bootstrap_api(uint32_t version);

#ifdef __cplusplus
}
#endif

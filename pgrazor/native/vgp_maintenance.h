#pragma once
#include <stdint.h>

// Feature 32 (API 1). Native VGP MAINTENANCE: a focused, read-mostly C++ port of the two residual
// post-decision side effects left in runtime/features/vgp.js vgpTick -- the final red-ball inventory
// recheck issued immediately before a straight ball throw (~line 211) and the successful-spin local
// cooldown setter applied after a spin result comes back (~line 226). Nothing else moves: the host
// (feature-25 VGP) still owns every hook, RPC, request issue, promise/result handoff, the seen policy,
// the service-acquisition lease and the pending record. This module installs NO Dobby/Gum hook,
// registers NO native callback, retains NO managed root across calls and NEVER calls back into JS.
//
// SCOPE: two synchronous owner-thread entry points, each a direct transliteration of the JS it replaces:
//   balls(svc)            -> read the item-bag reference field off the already-acquired service object
//                            (exact service + item-bag class), root it, invoke GetItemCount(item id 1)
//                            NON-virtually, read the boxed Int32 and return the (>=0) count. No
//                            MapSceneContext / DiContainer / Resolve walk: the host already resolved the
//                            service this tick; this is only the last-moment balls recheck.
//   cooldown(stop,resp)   -> for a spin whose result is success (resp.result == spin_success) and whose
//                            stop is still alive (static UnityEngine.Object.op_Implicit(stop)), read the
//                            full 64-bit resp.until field and invoke the stop's virtual SetCooldownTime
//                            with a pointer to it. The local setter mirrors the server cooldown so the
//                            host stops re-queuing the same stop; it issues NO network call.
//
// Native read-only intent: every managed value gets a temporary GC root for the duration of the call and
// is freed before return; NO raw managed pointer is reused across a managed call (relocating collector) --
// receivers are re-resolved from their roots. Both entry points run managed work only on the proven Unity
// owner thread (owner must equal the current OS tid) and only inside a short residual lease (clamped to
// PGR_VM_MAX_LEASE_MS). balls re-samples a fresh monotonic clock AFTER its reads and only commits a
// positive count if the lease is still open. cooldown re-checks owner/lease once more after its managed
// resolves and BEFORE the setter invocation; once the setter has been invoked without a managed throw it
// reports success even if the clock expired during the invocation -- the local setter already ran.
//
// Outcomes (shared sign convention):
//   PGR_VM_OK (1)   -> balls: *out_count set to the current count (0 is valid). cooldown: the setter was
//                      invoked without a managed throw.
//   PGR_VM_SKIP (0) -> benign host skip: closed/expired lease, owner mismatch (incl. owner 0), a not-ready
//                      null item-bag field; cooldown: the spin result was not success, or the stop was no
//                      longer alive. No managed side effect was applied.
//   negative        -> conservative failure the host surfaces as a VGP pause (no legacy fallback): bad
//                      shape (version/size), not installed, a reentrant call, a wrong class, a managed
//                      throw, a GC-root allocation failure, or (balls) a negative count.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "VgpMaintenance native ABI requires LP64");

enum PgrVgpMaintenanceInfo {
  PGR_VM_FEATURE_ID     = 32,
  PGR_VM_API_VERSION    = 1,
  PGR_VM_CONFIG_VERSION = 1,
  PGR_VM_MAX_LEASE_MS   = 2500,  // residual foreground lease ceiling (JS vgpLease gate)
  PGR_VM_BALL_ITEM_ID   = 1      // GetItemCount argument: red Poke Ball
};

// Entry-point return codes (positive success, 0 benign skip, negative conservative failure).
enum PgrVgpMaintenanceStatus {
  PGR_VM_OK                = 1,
  PGR_VM_SKIP              = 0,
  PGR_VM_ERR_NULL_ARG      = -1,
  PGR_VM_ERR_BAD_VERSION   = -2,
  PGR_VM_ERR_BAD_SIZE      = -3,
  PGR_VM_ERR_MISSING_PTR   = -4,   // install(): a required config pointer was null
  PGR_VM_ERR_NOT_INSTALLED = -6,
  PGR_VM_ERR_RECONFIG      = -9,   // install(): a different config at an already-installed table
  PGR_VM_ERR_CLOCK_FAILURE = -10,
  PGR_VM_ERR_BUSY          = -11,  // a reentrant call while another was in progress (defensive)
  PGR_VM_ERR_GCHANDLE      = -12,  // a temporary root could not be allocated / a receiver was collected
  PGR_VM_ERR_MANAGED       = -13,  // a managed call threw
  PGR_VM_ERR_METADATA      = -14,  // a resolved object had the wrong class, or a box was not Int32/Boolean
  PGR_VM_ERR_NEG_COUNT     = -16   // balls(): the Int32 count came back negative (fail closed)
};

// Fixed LP64 config (176 bytes): 8-byte header + 20 validated il2cpp pointers in the EXACT order below,
// then the Int32 spin-success sentinel and a reserved word. Every pointer is an already-validated export
// / MethodInfo* / FieldInfo* / Il2CppClass* packed by the host off p.vgp and p.items; the native layer
// re-checks the header and rejects any null. No hook address. m_item_count and m_alive are invoked
// NON-virtually (m_alive is the static UnityEngine.Object.op_Implicit(Object) overload, taking the stop as
// its argument); m_cooldown is the BASE SetCooldownTime MethodInfo* resolved per stop with
// object_get_virtual_method. The field offsets of this struct are the published contract: never reorder.
struct PgrVgpMaintenanceConfig {
  uint32_t version;  // == PGR_VM_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrVgpMaintenanceConfig)

  // [0..7] il2cpp runtime exports.
  void *gchandle_new;              // 0 uintptr_t (void* obj, int pinned)
  void *gchandle_get_target;       // 1 void*     (uintptr_t handle)
  void *gchandle_free;             // 2 void      (uintptr_t handle)
  void *object_get_class;          // 3 void*     (void* obj)
  void *runtime_invoke;            // 4 void*     (MethodInfo*, void* obj, void** params, void** exc)
  void *object_unbox;              // 5 void*     (void* boxed) -> value storage
  void *object_get_virtual_method; // 6 void*     (void* obj, MethodInfo* base) -> resolved slot
  void *field_get_value;           // 7 void      (void* obj, FieldInfo*, void* out)

  // [8..10] MethodInfo*.
  void *m_item_count;  // 8  p.vgp.m.itemCount  (IBAG GetItemCount(Item) -> Int32, non-virtual)
  void *m_alive;       // 9  p.methods.alive      (static Object.op_Implicit(Object) -> Boolean)
  void *m_cooldown;    // 10 p.vgp.m.cooldown   (base virtual SetCooldownTime(Int64) on the stop)

  // [11..13] FieldInfo*.
  void *f_item_bag;    // 11 p.items.fields.bag (item bag reference field on the service)
  void *f_until;       // 12 p.vgp.f.until      (Int64 cooldown target on the spin result)
  void *f_result;      // 13 p.vgp.f.result     (Int32 outcome code on the spin result)

  // [14..19] Il2CppClass* identities.
  void *class_service;      // 14 field_get_parent(p.items.fields.bag); the svc argument's exact class
  void *class_item_bag;     // 15 IBAG; the item-bag field's exact class
  void *class_int32;        // 16 System.Int32 (count box class)
  void *class_bool;         // 17 System.Boolean (alive box class, value size 1)
  void *class_stop;         // 18 VGP.stop; the cooldown stop argument's exact class
  void *class_spin_result;  // 19 SPINRESULT; the cooldown response argument's exact class

  int32_t  spin_success;  // off 168 p.vgp.spinSuccess; resp.result must equal this for a success
  uint32_t reserved;      // off 172 0
};

static_assert(sizeof(PgrVgpMaintenanceConfig) == 176,
              "PgrVgpMaintenanceConfig must remain 176 bytes on LP64");

// Fixed LP64 table: 8-byte header + 3 function pointers = 32 bytes.
struct PgrVgpMaintenanceApi {
  uint32_t version;  // == PGR_VM_API_VERSION
  uint32_t size;     // == sizeof(PgrVgpMaintenanceApi)
  // install(config): bind the validated runtime once for the process lifetime. Idempotent for an
  // identical config; a different config returns PGR_VM_ERR_RECONFIG. Any null required pointer or a
  // bad header is rejected (fail closed). No hook is installed.
  int (*install)(const PgrVgpMaintenanceConfig *config);
  // balls(svc, owner, lease_ms, out_count): read the current red Poke Ball count off the service's item
  // bag. *out_count is always initialised to 0 before any early return; set to the (>=0) count only on
  // PGR_VM_OK. Owner thread only.
  int (*balls)(void *svc, uint64_t owner, uint64_t lease_ms, int32_t *out_count);
  // cooldown(stop, response, owner, lease_ms): apply the local post-spin cooldown setter for a
  // successful, still-alive stop. No output beyond the return code. Owner thread only.
  int (*cooldown)(void *stop, void *response, uint64_t owner, uint64_t lease_ms);
};

static_assert(sizeof(PgrVgpMaintenanceApi) == 32,
              "PgrVgpMaintenanceApi must remain a 32-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 32 and answers only on an exact version match.
const void *pgrazor_vgp_maintenance_api(uint32_t version);

#ifdef __cplusplus
}
#endif

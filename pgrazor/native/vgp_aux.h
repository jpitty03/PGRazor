#pragma once
#include <stdint.h>

// Feature 34 (API 1). VGP CONSOLIDATION AUXILIARY: two STATELESS managed-call helpers that native-ise
// the only two reflection/inventory lookups the VGP host still performs inline in JS --
//   * runtime/features/vgp.js vgpSpinMethod (~37-44): resolve the closed (inflated) SendRpc MethodInfo*
//     for a FortSearch spin. It reads the rpc service off the item bag's `cynr` field, resolves the open
//     generic SendRpc on the rpc class, reflects it, builds a 2-element System.Type[] of {request, spin-
//     result}, invokes MakeGenericMethod, and unreflects the result into an inflated MethodInfo*.
//   * runtime/features/vgp.js vgpTransferTick (~119-121): given the catch RESPONSE and the pokemon bag,
//     read the full 64-bit captured id, look the pokemon up via bag.GetPokemon(id), prove it is the same
//     id + exact Pokemon class, and read the bag's GameMaster handle.
//
// This module owns NOTHING persistent: it installs NO hook, holds NO retained GC root between calls,
// keeps NO cache, runs NO automation, and NEVER calls back into JS. Each op roots its temporaries, does
// its managed work, re-resolves every rooted object from its handle after every managed call (relocating
// collector), hands the caller OWN GC handles (and, for spin, the raw inflated MethodInfo*), and frees
// every temporary before returning. install() publishes the immutable config once. A busy guard refuses a
// reentrant op (e.g. one fired from inside a managed callback). The VGP controller (JS, feature 25) owns
// the gate, the pending record, the promise polling and every policy decision; it feeds each op a proven
// owner tid and a residual foreground lease and drives the actual SendRpc / transfer itself.
//
// Division of labour with runtime/native-vgp-aux.js: the adapter validates + packs the 28 il2cpp pointers
// (16 runtime exports, 7 exact class identities, 4 FieldInfo* and the GetPokemon MethodInfo*) into the
// 232-byte config and calls install() ONLY -- it does NOT wrap spin()/transfer(); the controller invokes
// those on the raw table. The adapter resolves the `cynr` rpc FieldInfo on the item-bag class itself
// (parent walk <=16, offset 16..512, field class == rpc) and reports an interface blocker if it does not
// match the legacy shape. The adapter supplies NO bag class for transfer: the controller hands transfer()
// an already-validated pokemon bag. No transfer policy and no ReleaseRPC live here -- this is lookup only.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "VgpAux native ABI requires LP64");

enum PgrVgpAuxInfo {
  PGR_VX_FEATURE_ID     = 34,
  PGR_VX_API_VERSION    = 1,
  PGR_VX_CONFIG_VERSION = 1,
  PGR_VX_OUT_VERSION    = 1,
  PGR_VX_MAX_LEASE_MS   = 2500,  // residual foreground lease ceiling (JS vgpLease gate)
  PGR_VX_SENDRPC_ARITY  = 5,     // SendRpc<Request,Result>(rpcId, request, 0, 10000, 0) param count
  PGR_VX_TYPE_ARGS      = 2,     // SendRpc generic arity (request + spin-result)
  PGR_VX_ARRAY_DATA_OFF = 32     // Il2CppArray<T> first element byte offset on LP64
};

// spin()/transfer() return 1 SUCCESS (*out fully written, OWN handles transferred), 0 NOT-READY/REFUSED
// (gate/busy/thread/expired or GetPokemon null, *out zeroed, nothing invoked that must be retried), 2 SKIP
// (transfer only: captured id 0 or a pokemon identity mismatch -- drop this transfer, not an error), or a
// negative error. install() returns PGR_VX_OK (0) or a negative error.
enum PgrVgpAuxStatus {
  PGR_VX_SUCCESS           = 1,   // op succeeded; *out written; OWN handles belong to the caller
  PGR_VX_OK                = 0,   // install OK / op refused (gate/busy/thread/expired/not-ready)
  PGR_VX_SKIP              = 2,   // transfer: captured id 0 or pokemon identity mismatch (drop, no error)
  PGR_VX_ERR_NULL_ARG      = -1,
  PGR_VX_ERR_BAD_VERSION   = -2,
  PGR_VX_ERR_BAD_SIZE      = -3,
  PGR_VX_ERR_MISSING_PTR   = -4,
  PGR_VX_ERR_NOT_INSTALLED = -6,
  PGR_VX_ERR_RECONFIG      = -9,
  PGR_VX_ERR_CLOCK_FAILURE = -10,
  PGR_VX_ERR_IDENTITY      = -11,  // item-bag/rpc/response class mismatch, or a required object was null
  PGR_VX_ERR_GCHANDLE      = -12,  // a GC root / Type[] allocation failed (fail closed)
  PGR_VX_ERR_MANAGED       = -13,  // a managed invoke (MakeGenericMethod / GetPokemon) threw
  PGR_VX_ERR_REFLECT       = -14   // SendRpc / MakeGenericMethod / closed-method resolution/validation failed
};

// Fixed LP64 config (232 bytes): 8-byte header + 28 validated il2cpp pointers. Every pointer is already
// validated by the JS adapter straight off the host metadata; the native layer re-checks the header and
// rejects any null. No hook address. The indices below match runtime/native-vgp-aux.js exactly.
struct PgrVgpAuxConfig {
  uint32_t version;  // == PGR_VX_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrVgpAuxConfig)

  // --- il2cpp runtime exports (indices 0..15) ---------------------------------------
  void *gc_new;                     // 0  uintptr_t (void* obj, int pinned)
  void *gc_get;                     // 1  void*     (uintptr_t handle)
  void *gc_free;                    // 2  void      (uintptr_t handle)
  void *object_get_class;           // 3  void*     (void* obj)
  void *runtime_invoke;             // 4  void*     (MethodInfo*, void* obj, void** params, void** exc)
  void *field_get_value;            // 5  void      (void* obj, FieldInfo*, void* out)
  void *class_get_method_from_name; // 6  MethodInfo* (Il2CppClass*, const char* name, int argc)
  void *method_is_generic;          // 7  bool      (const MethodInfo*)
  void *method_get_param_count;     // 8  uint32_t  (const MethodInfo*)
  void *method_get_object;          // 9  void*     (const MethodInfo*, Il2CppClass*) -> Reflection.MethodInfo
  void *array_new;                  // 10 void*     (Il2CppClass* elem, uintptr_t len) -> array object
  void *class_get_type;             // 11 void*     (Il2CppClass*) -> Il2CppType*
  void *type_get_object;            // 12 void*     (Il2CppType*) -> System.Type
  void *gc_wbarrier_set_field;      // 13 void      (void* obj, void** field, void* value)
  void *method_get_from_reflection; // 14 MethodInfo* (void* reflectionMethod) -> inflated MethodInfo*
  void *method_is_inflated;         // 15 bool      (const MethodInfo*)

  // --- exact Il2CppClass* identities (indices 16..22) -------------------------------
  void *class_rpc;        // 16 VGP rpc service (SendRpc owner; exact cynr-field class)
  void *class_request;    // 17 FortSearch request type (SendRpc first generic arg)
  void *class_spin;       // 18 FortSearch spin-result type (SPINRESULT; SendRpc second generic arg)
  void *class_type;       // 19 System.Type (Type[] element class for array_new)
  void *class_item_bag;   // 20 IItemBag (IBAG; exact item-bag class for spin())
  void *class_pokemon;    // 21 Pokemon (PK; exact GetPokemon result class for transfer())
  void *class_response;   // 22 catch RESPONSE (RESP; exact response class for transfer())

  // --- FieldInfo* + MethodInfo* (indices 23..27) ------------------------------------
  void *f_rpc;            // 23 IBAG.cynr (rpc service; adapter-validated parent walk + class == rpc)
  void *f_caught_id;      // 24 RESP captured id (System.UInt64; read full 64 bits)
  void *f_pokemon_id;     // 25 Pokemon id (p.details.fields.id; identity match vs captured id)
  void *f_bag_gm;         // 26 pokemon bag GameMaster handle (p.vgp.f.bagGm; IGameMasterData, nonnull)
  void *m_get_pokemon;    // 27 bag.GetPokemon(UInt64) -> Pokemon (p.transfer.methods.get.method)
};

static_assert(sizeof(PgrVgpAuxConfig) == 232, "PgrVgpAuxConfig must remain 232 bytes on LP64");

// Fixed LP64 spin result (32 bytes). OWN: the caller adopts rpc_handle + reflection_handle (both il2cpp GC
// handles) and frees them with il2cpp_gchandle_free. closed_method is the inflated SendRpc MethodInfo*; it
// is a raw pointer that stays valid ONLY while reflection_handle keeps the closed reflection object rooted,
// so the caller MUST hold reflection_handle until the spin's promise completes. Written only on SUCCESS.
struct PgrVgpAuxSpin {
  uint32_t version;         // == PGR_VX_OUT_VERSION
  uint32_t size;            // == sizeof(PgrVgpAuxSpin)
  uint64_t rpc_handle;      // OWN GC handle rooting the rpc service read off the item bag
  uint64_t reflection_handle; // OWN GC handle rooting the closed SendRpc Reflection.MethodInfo
  void    *closed_method;   // inflated SendRpc MethodInfo* (borrowed; lives with reflection_handle)
};

static_assert(sizeof(PgrVgpAuxSpin) == 32, "PgrVgpAuxSpin must remain 32 bytes on LP64");

// Fixed LP64 transfer result (32 bytes). OWN: the caller adopts pokemon_handle + gm_handle (il2cpp GC
// handles) and frees them with il2cpp_gchandle_free. caught_id is the full 64-bit captured id. Written
// only on SUCCESS (code 1); SKIP (code 2) and REFUSED (code 0) leave it zeroed.
struct PgrVgpAuxTransfer {
  uint32_t version;        // == PGR_VX_OUT_VERSION
  uint32_t size;           // == sizeof(PgrVgpAuxTransfer)
  uint64_t caught_id;      // full 64-bit RESP captured id (== pokemon id)
  uint64_t pokemon_handle; // OWN GC handle rooting the resolved Pokemon
  uint64_t gm_handle;      // OWN GC handle rooting the bag GameMaster handle
};

static_assert(sizeof(PgrVgpAuxTransfer) == 32, "PgrVgpAuxTransfer must remain 32 bytes on LP64");

// Fixed LP64 table: 8-byte header + 3 function pointers = 32 bytes.
struct PgrVgpAuxApi {
  uint32_t version;  // == PGR_VX_API_VERSION
  uint32_t size;     // == sizeof(PgrVgpAuxApi)
  // Validate + publish the immutable config once for the process lifetime. A repeat of the identical
  // config is PGR_VX_OK; a different one is PGR_VX_ERR_RECONFIG. No managed work.
  int (*install)(const PgrVgpAuxConfig *config);
  // Resolve the closed SendRpc for a spin off `item_bag`. Requires current tid == owner_tid and a residual
  // lease (clamped to 2500 ms, re-checked after the managed work). Returns PGR_VX_SUCCESS (*out written,
  // OWN handles transferred), PGR_VX_OK (refused: gate/busy/thread/expired), or a negative error. Owner
  // thread only; a reentrant call is refused.
  int (*spin)(void *item_bag, uint64_t owner_tid, uint64_t lease_ms, PgrVgpAuxSpin *out);
  // Look up the caught pokemon for a transfer from `response` + `pokemon_bag`. Returns PGR_VX_SUCCESS
  // (*out written), PGR_VX_OK (refused, or GetPokemon returned null -> inventory not ready, retry later),
  // PGR_VX_SKIP (captured id 0 or identity mismatch -> drop), or a negative error. Owner thread only.
  int (*transfer)(void *response, void *pokemon_bag, uint64_t owner_tid, uint64_t lease_ms,
                  PgrVgpAuxTransfer *out);
};

static_assert(sizeof(PgrVgpAuxApi) == 32, "PgrVgpAuxApi must remain a 32-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 34 and answers only on an exact version match.
const void *pgrazor_vgp_aux_api(uint32_t version);

#ifdef __cplusplus
}
#endif

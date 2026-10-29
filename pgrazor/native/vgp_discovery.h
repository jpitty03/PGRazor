#pragma once
#include <stdint.h>

// Feature 31 (API 1). Native VGP INVENTORY/ENUMERATION DISCOVERY: a focused, read-only C++ port of the
// per-tick inventory resolution + map enumeration inside runtime/features/vgp.js vgpTick (the service /
// container / contract Resolve + 5 Int32 counts at ~lines 230-240, and the FindObjectsOfType array walk
// at ~line 244) plus the lookup metadata assembled in diagnostics/quick-close.js (p.items.lookup.find /
// container / resolve, p.vgp.m.findAll, the count methods and the item-bag field).
//
// SCOPE: this module owns ONLY three synchronous owner-thread reads:
//   snapshot(svc)   -> resolves FindObjectOfType(map).get_Container().Resolve(playerContract/bagContract),
//                      reads the item bag off the service field, validates the expected concrete classes,
//                      and returns the five Int32 inventory counts (balls via GetItemCount(item id 1),
//                      stored, capacity, totalItems, maxItems) plus TWO owned GC handles -- the pokemon
//                      bag and the item bag -- adopted by the caller exactly once.
//   enumerate(kind) -> FindObjectsOfType(wild|stop), validates Object[] assignability, reads the
//                      element count (<= PGR_VD_MAX_ARRAY) and returns ONE owned GC handle for the array.
//   at(array,index) -> revalidates the array class/count/index, reads the raw LP64 element pointer at
//                      data offset 32, and returns ONE owned GC handle for a non-null element.
//
// NOT: this module issues NO RPC, NO encounter/capture/spin action, NO transfer, NO cooldown and changes
// NO inventory. It installs NO Dobby/Gum hook, registers NO native callback, retains NO managed root
// across calls (only the handles explicitly transferred to the caller survive) and NEVER calls back into
// JS. The host (feature-25 VGP) keeps the seen policy, the service acquisition lease, the pending record
// and every action issue. FindObjectsOfType enumeration is split snapshot/enumerate/at so the host can
// root the array once and walk it with per-element at() calls.
//
// Native read-only: every managed value gets a temporary GC root for the duration of the call and is
// freed before return; no raw managed pointer is reused across a managed call (relocating collector) --
// receivers are re-resolved from their roots. All three entry points run managed work only on the proven
// Unity owner thread (owner_tid must equal the current OS tid) and only inside a short residual lease
// (clamped to PGR_VD_MAX_LEASE_MS); a fresh monotonic clock is re-sampled AFTER the lookups and a positive
// result is returned only if the lease is still open at that point. The output struct is fully
// initialised before any early return, and no partially-returned handle is ever left in it.
//
// Outcomes (return value, identical sign convention for all three entry points):
//   PGR_VD_OK (1)   -> success; *out populated and any owned handle(s) set exactly once.
//   PGR_VD_SKIP (0) -> benign host skip: closed/expired lease, owner mismatch (incl. owner 0),
//                      map/container/contract/service not ready (a null managed result), FindObjectsOfType
//                      null, or a null element at the requested index. No handle retained.
//   negative        -> conservative failure the host surfaces as a VGP pause (no legacy fallback): bad
//                      shape (version/size/kind/index), oversize array, negative count, a wrong class
//                      ("invalid class throws"), a managed exception, or a GC-root allocation failure.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "VgpDiscovery native ABI requires LP64");

enum PgrVgpDiscoveryInfo {
  PGR_VD_FEATURE_ID      = 31,
  PGR_VD_API_VERSION     = 1,
  PGR_VD_CONFIG_VERSION  = 2,
  PGR_VD_SNAPSHOT_VERSION = 1,
  PGR_VD_ENUM_VERSION    = 1,
  PGR_VD_MAX_LEASE_MS    = 2500,  // residual foreground lease ceiling (JS vgpLease gate)
  PGR_VD_KIND_CATCH      = 1,     // enumerate wild map pokemon (VGP.wild)
  PGR_VD_KIND_SPIN       = 2,     // enumerate stop map objects (VGP.stop)
  PGR_VD_MAX_ARRAY       = 2048,  // strict element-count ceiling (JS 'vgp-map-count' guard)
  PGR_VD_ARRAY_DATA_OFF  = 32     // raw LP64 managed array data start (existing ABI: element[i] @ 32+i*8)
};

// Entry-point return codes (positive success, 0 benign skip, negative conservative failure).
enum PgrVgpDiscoveryStatus {
  PGR_VD_OK                = 1,
  PGR_VD_SKIP              = 0,
  PGR_VD_ERR_NULL_ARG      = -1,
  PGR_VD_ERR_BAD_VERSION   = -2,
  PGR_VD_ERR_BAD_SIZE      = -3,
  PGR_VD_ERR_MISSING_PTR   = -4,   // install(): a required config pointer was null
  PGR_VD_ERR_NOT_INSTALLED = -6,
  PGR_VD_ERR_RECONFIG      = -9,   // install(): a different config at an already-installed table
  PGR_VD_ERR_CLOCK_FAILURE = -10,
  PGR_VD_ERR_BUSY          = -11,  // a reentrant call while another was in progress (defensive)
  PGR_VD_ERR_GCHANDLE      = -12,  // a temporary root or a returned handle could not be allocated
  PGR_VD_ERR_MANAGED       = -13,  // a managed call threw
  PGR_VD_ERR_METADATA      = -14,  // a resolved object had the wrong class, or a count box was not Int32
  PGR_VD_ERR_BAD_KIND      = -15,  // enumerate(): kind not catch/spin
  PGR_VD_ERR_BAD_COUNT     = -16,  // array length exceeded PGR_VD_MAX_ARRAY
  PGR_VD_ERR_BAD_INDEX     = -17,  // at(): index >= element count
  PGR_VD_ERR_NEG_COUNT     = -18   // snapshot(): an Int32 count came back negative (fail closed)
};

// Fixed LP64 config (272 bytes): 8-byte header + 32 validated il2cpp pointers in the EXACT order below.
// Every pointer is an already-validated export / MethodInfo* / FieldInfo* / Il2CppClass* packed by the
// host off p.items.lookup and p.vgp.m; the native layer re-checks the header and rejects any null. No
// hook address. Each count method and the two Resolve-returned objects are invoked NON-virtually, exactly
// as the legacy JS path. The field offsets of this struct are the published contract: never reorder.
struct PgrVgpDiscoveryConfig {
  uint32_t version;  // == PGR_VD_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrVgpDiscoveryConfig)

  // [0..9] il2cpp runtime exports.
  void *gchandle_new;          // 0  uintptr_t (void* obj, int pinned)
  void *gchandle_get_target;   // 1  void*     (uintptr_t handle)
  void *gchandle_free;         // 2  void      (uintptr_t handle)
  void *object_get_class;      // 3  void*     (void* obj)
  void *runtime_invoke;        // 4  void*     (MethodInfo*, void* obj, void** params, void** exc)
  void *object_unbox;          // 5  void*     (void* boxed) -> value storage
  void *field_get_value;       // 6  void      (void* obj, FieldInfo*, void* out)
  void *class_get_type;        // 7  void*     (void* klass) -> Il2CppType*
  void *type_get_object;       // 8  void*     (void* type) -> managed System.Type
  void *array_length;          // 9  size_t    (void* array)

  // [10..18] MethodInfo* (invoked non-virtually via runtime_invoke).
  void *m_find;          // 10  p.items.lookup.find    (static Object FindObjectOfType(Type))
  void *m_container;     // 11  p.items.lookup.container(instance DiContainer get_Container())
  void *m_resolve;       // 12  p.items.lookup.resolve (instance Object Resolve(Type))
  void *m_item_count;    // 13  p.vgp.m.itemCount      (IBAG GetItemCount(Item) -> Int32)
  void *m_pokemon_count; // 14  p.vgp.m.pokemonCount   (BAG  get_Count() -> Int32)
  void *m_max_pokemon;   // 15  p.vgp.m.maxPokemon     (player get_MaxPokemon() -> Int32)
  void *m_total_items;   // 16  p.vgp.m.totalItems     (IBAG GetTotalItems() -> Int32)
  void *m_max_items;     // 17  p.vgp.m.maxItems       (player get_MaxItems() -> Int32)
  void *m_find_all;      // 18  p.vgp.m.findAll        (static Object[] FindObjectsOfType(Type))

  // [19..30] Il2CppClass* identities; scene/container/array results allow compatible subclasses.
  void *class_map;             // 19  lookup.map  (MapSceneContext; FindObjectOfType argument type)
  void *class_player_contract; // 20  VGP.playerContract (Resolve argument type)
  void *class_bag_contract;    // 21  VGP.bagContract    (Resolve argument type)
  void *class_player;          // 22  VGP.player  (resolved player object's exact class)
  void *class_pokemon_bag;     // 23  BAG         (resolved pokemon bag object's exact class)
  void *class_item_bag;        // 24  IBAG        (item bag field's exact class)
  void *class_item_service;    // 25  field_get_parent(p.items.fields.bag); the svc argument's exact class
  void *class_int32;           // 26  System.Int32 (count box class)
  void *class_wild;            // 27  VGP.wild (catch enumeration element / array type class)
  void *class_stop;            // 28  VGP.stop (spin enumeration element class)
  void *class_object_array;    // 29  return class of m_find_all (UnityEngine.Object[]; expected array base class)
  void *class_container;       // 30  return class of m_container (Zenject.DiContainer; container class)

  // [31] FieldInfo*.
  void *f_item_bag;            // 31  p.items.fields.bag (item bag reference field on the service)
  void *class_is_assignable_from; // 32 expected class, actual class; IL2CPP bool return
};

static_assert(sizeof(PgrVgpDiscoveryConfig) == 272,
              "PgrVgpDiscoveryConfig must remain 272 bytes on LP64");

// Fixed LP64 snapshot output (48 bytes). Always fully initialised before any early return. The two
// handles are OWNED (adopted by the caller exactly once) and set ONLY on PGR_VD_OK; they are 0 otherwise.
struct PgrVgpDiscoverySnapshot {
  uint32_t version;          // off 0  == PGR_VD_SNAPSHOT_VERSION
  uint32_t size;             // off 4  == sizeof(PgrVgpDiscoverySnapshot)
  int32_t  balls;            // off 8  GetItemCount(item id 1)
  int32_t  stored;           // off 12 pokemon bag get_Count()
  int32_t  capacity;         // off 16 player get_MaxPokemon()
  int32_t  total_items;      // off 20 item bag GetTotalItems()
  int32_t  max_items;        // off 24 player get_MaxItems()
  uint32_t reserved;         // off 28 0
  uint64_t pokemon_bag_handle; // off 32 OWNED GC handle for the pokemon bag (BAG)
  uint64_t item_bag_handle;    // off 40 OWNED GC handle for the item bag (IBAG)
};

static_assert(sizeof(PgrVgpDiscoverySnapshot) == 48,
              "PgrVgpDiscoverySnapshot must remain 48 bytes on LP64");

// Fixed LP64 enumeration output (24 bytes). Always fully initialised. arrayHandle is an OWNED GC handle
// set ONLY on PGR_VD_OK (0 otherwise). count == 0 is a valid success.
struct PgrVgpDiscoveryEnumeration {
  uint32_t version;      // off 0  == PGR_VD_ENUM_VERSION
  uint32_t size;         // off 4  == sizeof(PgrVgpDiscoveryEnumeration)
  uint32_t count;        // off 8  element count (<= PGR_VD_MAX_ARRAY)
  uint32_t reserved;     // off 12 0
  uint64_t array_handle; // off 16 OWNED GC handle for the Object[] (adopt exactly once)
};

static_assert(sizeof(PgrVgpDiscoveryEnumeration) == 24,
              "PgrVgpDiscoveryEnumeration must remain 24 bytes on LP64");

// Fixed LP64 table: 8-byte header + 4 function pointers = 40 bytes.
struct PgrVgpDiscoveryApi {
  uint32_t version;  // == PGR_VD_API_VERSION
  uint32_t size;     // == sizeof(PgrVgpDiscoveryApi)
  // install(config): bind the validated runtime once for the process lifetime. Idempotent for an
  // identical config; a different config returns PGR_VD_ERR_RECONFIG. Any null required pointer or a bad
  // header is rejected (fail closed). No hook is installed.
  int (*install)(const PgrVgpDiscoveryConfig *config);
  // snapshot(svc, owner, lease_ms, out): resolve the inventory and counts off the service object `svc`
  // (a raw managed pointer the caller keeps rooted). Owner thread only.
  int (*snapshot)(void *svc, uint64_t owner, uint64_t lease_ms, PgrVgpDiscoverySnapshot *out);
  // enumerate(kind, owner, lease_ms, out): FindObjectsOfType(wild|stop) and return the owned array handle
  // plus its element count. Owner thread only.
  int (*enumerate)(uint32_t kind, uint64_t owner, uint64_t lease_ms, PgrVgpDiscoveryEnumeration *out);
  // at(array, index, owner, lease_ms, out_handle): return an owned GC handle for element `index` of the
  // already-rooted Object[] `array`. *out_handle is always initialised (0 on any non-OK path). Owner
  // thread only.
  int (*at)(void *array, uint32_t index, uint64_t owner, uint64_t lease_ms, uintptr_t *out_handle);
};

static_assert(sizeof(PgrVgpDiscoveryApi) == 40,
              "PgrVgpDiscoveryApi must remain a 40-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 31 and answers only on an exact version match.
const void *pgrazor_vgp_discovery_api(uint32_t version);

#ifdef __cplusplus
}
#endif

#pragma once
#include <stdint.h>

// Feature 26 (API 1). Native VGP REQUEST ACTIONS: a focused C++ port of the three synchronous managed
// request issues inside runtime/features/vgp.js vgpTick -- the SendEncounterRequest call (~162), the
// TryCapture call with the fixed 12-byte PokeballThrow (red ball id 1, grade 1.0, remaining 0, null AR
// metadata) (~133-134), and the FortSearch construction + caller-resolved generic SendRpc (~163-166).
// It owns NOTHING else: the pending record, the promise polling (vgpPromise), the manual-exclusion /
// inventory / service gating, the result handling + auto-transfer and the marker all stay in JS this
// increment. This module installs NO hook, adds NO automation behaviour, and NEVER calls back into JS.
//
// SCOPE / NOT: three synchronous actions only. The caller (the feature-25 VGP host, in JS) chooses the
// target AFTER its own manual-exclusion / inventory / service gates and only while VGP is enabled; it
// then calls one action, receives an OWN GC-rooted promise handle, and drives the EXISTING pending
// record + promise polling. No new hook and no new automation is introduced. When the native core is
// absent the host keeps the inline JS invokes untouched (create() returns null).
//
// Ownership of the returned handle: each action, on success, roots the managed promise it received from
// the game into a fresh il2cpp GC handle and writes it to *out_handle. Ownership transfers to the caller
// EXACTLY ONCE: the caller adopts it into its own owned-handle set and frees it later. Native retains NO
// managed pointer past the synchronous call except that one transferred handle. Every temporary root
// (target / fort / rpc / request) is held across the allocations / ctor / field writes / invoke and
// freed before return. No raw managed pointer is reused across a managed call (relocating collector).
//
// Identity / arguments: the caller passes RAW managed pointers it keeps rooted for the synchronous call;
// native roots them internally too and re-resolves the exact classes (target == VGP wild for catch,
// VGP stop for spin; spin's fort ==
// FortData, rpc == the rpc service, and the request it builds == the FortSearch request type) and checks
// the player lat/lon are finite and in range. The caller-resolved closed (inflated) SendRpc MethodInfo*
// is validated non-null; native does NOT reflect/inflate it.
//
// Gate / reentrancy / once-only: update() arms a mask (bit 1 = catch, bit 2 = spin), a residual lease
// clamped to 2500 ms and the caller's epoch; a foreign thread can only revoke. Each action re-checks the
// gate against a fresh clock, claims a strictly monotonic token, re-checks the revocation generation
// IMMEDIATELY before the one game-action invoke, and performs that invoke at most once. A busy flag
// refuses a reentrant action (e.g. one fired from inside a managed callback). stop() revokes immediately
// from any thread (bumps the generation); a disable or re-arm bumps it too, invalidating any action that
// has not yet reached its pre-invoke re-check. No mutex is ever held across a managed invoke or a GC
// handle call. A managed throw is reported as an error; a successful invoke whose promise cannot be
// rooted is reported as UNCERTAIN (the action issued -- the host must NOT retry).

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "VgpActions native ABI requires LP64");

enum PgrVgpActionsInfo {
  PGR_VA_FEATURE_ID      = 26,
  PGR_VA_API_VERSION     = 1,
  PGR_VA_CONFIG_VERSION  = 1,
  PGR_VA_TARGET_VERSION  = 1,
  PGR_VA_SPIN_VERSION    = 1,
  PGR_VA_SAMPLE_VERSION  = 1,
  PGR_VA_MAX_LEASE_MS    = 2500,  // residual foreground lease ceiling (JS vgpLease gate)
  PGR_VA_MASK_CATCH      = 1,     // update() mask bit: encounter + capture enabled (JS vgpMask & 1)
  PGR_VA_MASK_SPIN       = 2,     // update() mask bit: spin enabled               (JS vgpMask & 2)
  PGR_VA_BALL_RED        = 1      // PokeballThrow ball id (red Poke Ball)
};

// encounter()/capture()/spin() return 1 ISSUED (*out_handle set, ownership transferred), 0 REFUSED
// (gate/busy/thread, *out_handle 0, NOT invoked), or a negative error. install()/update()/stop()/
// sample() return PGR_VA_OK (0) or a negative error.
enum PgrVgpActionsStatus {
  PGR_VA_ISSUED            = 1,   // action invoked, promise rooted, *out_handle set (owned by caller)
  PGR_VA_OK                = 0,   // generic success / action: refused (gate/busy/thread), NOT invoked
  PGR_VA_ERR_NULL_ARG      = -1,
  PGR_VA_ERR_BAD_VERSION   = -2,
  PGR_VA_ERR_BAD_SIZE      = -3,
  PGR_VA_ERR_MISSING_PTR   = -4,
  PGR_VA_ERR_NOT_INSTALLED = -6,
  PGR_VA_ERR_RECONFIG      = -9,
  PGR_VA_ERR_CLOCK_FAILURE = -10,
  PGR_VA_ERR_IDENTITY      = -11,  // target/fort/rpc/request class mismatch, or coords non-finite/range
  PGR_VA_ERR_GCHANDLE      = -12,  // a root / request allocation failed BEFORE the action (fail closed)
  PGR_VA_ERR_MANAGED       = -13,  // a managed invoke (ctor / action) threw
  PGR_VA_ERR_UNCERTAIN     = -14   // the action was invoked but its promise could not be rooted: NO retry
};

// Fixed LP64 config (208 bytes): 8-byte header + 25 validated il2cpp pointers. Every pointer is an
// already-validated export / MethodInfo* / Il2CppClass* / FieldInfo* packed by the host straight off
// p.vgp; the native layer re-checks the header and rejects any null. No hook address; the inflated
// SendRpc MethodInfo* is NOT here (it is caller-resolved and passed per spin()).
struct PgrVgpActionsConfig {
  uint32_t version;  // == PGR_VA_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrVgpActionsConfig)

  // il2cpp runtime exports.
  void *gchandle_new;         // uintptr_t (void* obj, int pinned)
  void *gchandle_get_target;  // void*     (uintptr_t handle)
  void *gchandle_free;        // void      (uintptr_t handle)
  void *object_get_class;     // void*     (void* obj)
  void *object_new;           // void*     (Il2CppClass*) -> new managed object
  void *runtime_invoke;       // void*     (MethodInfo*, void* obj, void** params, void** exc)
  void *field_get_value;      // void      (void* obj, FieldInfo*, void* out)
  void *field_set_value;      // void      (void* obj, FieldInfo*, void* value)      [value-type fields]
  void *field_set_value_obj;  // void      (void* obj, FieldInfo*, void* valueObj)   [reference fields]

  // MethodInfo* (invoked via runtime_invoke).
  void *m_encounter;  // wild.SendEncounterRequest() -> IPromise (no params)
  void *m_capture;    // wild.TryCapture(PokeballThrow, ARPlusEncounterValuesProto) -> IPromise
  void *m_ctor;       // request.ctor() -> void

  // Exact Il2CppClass* identities.
  void *class_wild;     // VGP.wild (encounter/capture target)
  void *class_fort;     // VGP.fort (FortData; spin)
  void *class_rpc;      // VGP.rpc  (the rpc service; spin receiver)
  void *class_request;  // VGP.request (the FortSearch request type native constructs)

  // FieldInfo* (read/written via field_get_value / field_set_value[_obj]).
  void *f_fort_id;         // fort.FortId (String)      -> request.Id
  void *f_fort_lat;        // fort.Latitude (Double)    -> request.FortLatDegrees
  void *f_fort_lon;        // fort.Longitude (Double)   -> request.FortLngDegrees
  void *f_req_id;          // request.Id (String, reference)
  void *f_req_lat;         // request.PlayerLatDegrees (Double)
  void *f_req_lon;         // request.PlayerLngDegrees (Double)
  void *f_req_target_lat;  // request.FortLatDegrees (Double)
  void *f_req_target_lon;  // request.FortLngDegrees (Double)
  void *class_stop;       // PokestopMapObject, distinct from a wild Pokemon
};

static_assert(sizeof(PgrVgpActionsConfig) == 208,
              "PgrVgpActionsConfig must remain 208 bytes on LP64");

// Fixed LP64 target request (32 bytes) for encounter()/capture(). target is a raw managed pointer the
// caller keeps rooted for the synchronous call; native also roots it on entry.
struct PgrVgpActionsTarget {
  uint32_t version;  // == PGR_VA_TARGET_VERSION
  uint32_t size;     // == sizeof(PgrVgpActionsTarget)
  uint64_t epoch;    // caller's current epoch; must equal the armed epoch at the gate
  uint64_t tid;      // proven owner tid (must equal the current OS tid)
  void *target;      // VGP wild map pokemon
};

static_assert(sizeof(PgrVgpActionsTarget) == 32,
              "PgrVgpActionsTarget must remain 32 bytes on LP64");

// Fixed LP64 spin request (80 bytes). target/fort/rpc are raw managed pointers rooted by native on
// entry; closed_method is the caller-resolved inflated SendRpc MethodInfo* (validated non-null, never
// reflected by native). lat/lon are the player coordinates (finite, |lat|<=90, |lon|<=180).
struct PgrVgpActionsSpin {
  uint32_t version;      // == PGR_VA_SPIN_VERSION
  uint32_t size;         // == sizeof(PgrVgpActionsSpin)
  uint64_t epoch;        // caller's current epoch; must equal the armed epoch at the gate
  uint64_t tid;          // proven owner tid (must equal the current OS tid)
  int32_t rpc_id;        // FortSearch rpc id (SendRpc param 0)
  int32_t reserved;      // 0
  double lat;            // player latitude  -> request.PlayerLatDegrees
  double lon;            // player longitude -> request.PlayerLngDegrees
  void *target;          // VGP.stop PokestopMapObject (rooted, exact stop class)
  void *fort;            // VGP fort (FortData), source of Id + fort lat/lon
  void *rpc;             // VGP rpc service (SendRpc receiver)
  void *closed_method;   // inflated SendRpc MethodInfo* (caller-resolved, non-null)
};

static_assert(sizeof(PgrVgpActionsSpin) == 80,
              "PgrVgpActionsSpin must remain 80 bytes on LP64");

// Fixed LP64 telemetry gauge (72 bytes). Pull-only; the native layer never calls JS.
struct PgrVgpActionsSample {
  uint32_t version;          // == PGR_VA_SAMPLE_VERSION
  uint32_t size;             // == sizeof(PgrVgpActionsSample)
  uint64_t encounter_serial; // bumps on each ISSUED encounter()
  uint64_t capture_serial;   // bumps on each ISSUED capture()
  uint64_t spin_serial;      // bumps on each ISSUED spin()
  uint64_t refused_serial;   // bumps on each refused (gate/busy/thread) action
  uint64_t fault_serial;     // bumps on each managed/uncertain action error
  uint64_t last_token;       // last token claimed by an action
  uint64_t last_tid;         // owner tid of the last action
  uint32_t roots;            // live GC roots (0 between calls; nonzero only mid-action)
  uint32_t busy;             // 1 while an action is in flight
};

static_assert(sizeof(PgrVgpActionsSample) == 72,
              "PgrVgpActionsSample must remain 72 bytes on LP64");

// Fixed LP64 table: 8-byte header + 7 function pointers = 64 bytes.
struct PgrVgpActionsApi {
  uint32_t version;  // == PGR_VA_API_VERSION
  uint32_t size;     // == sizeof(PgrVgpActionsApi)
  int (*install)(const PgrVgpActionsConfig *config);
  // Refresh the gate: mask (bit1 catch / bit2 spin), a residual lease clamped to 2500 ms, and the
  // caller's epoch. Carries proven_tid; once the owner is latched a foreign thread can only revoke
  // (mask forced to 0). A disable / re-arm bumps the revocation generation. Any thread.
  int (*update)(uint32_t mask, uint64_t remaining_lease_ms, uint64_t epoch, uint64_t proven_tid);
  // SendEncounterRequest on the target. Requires mask bit1. Returns PGR_VA_ISSUED (*out_handle set), 0
  // (refused), or a negative error. Owner thread only.
  int (*encounter)(const PgrVgpActionsTarget *request, uintptr_t *out_handle);
  // TryCapture with the fixed red-ball PokeballThrow and null AR metadata. Requires mask bit1. Returns
  // PGR_VA_ISSUED / 0 / negative. Owner thread only.
  int (*capture)(const PgrVgpActionsTarget *request, uintptr_t *out_handle);
  // Construct the FortSearch request and invoke the caller-resolved SendRpc. Requires mask bit2.
  // Returns PGR_VA_ISSUED / 0 / negative. Owner thread only.
  int (*spin)(const PgrVgpActionsSpin *request, uintptr_t *out_handle);
  // Immediate revoke from any thread (bumps the generation, clears the mask). A later update() re-arms;
  // stop() never re-arms. No managed work; no deadlock with an in-flight action.
  int (*stop)(uint64_t proven_tid);
  int (*sample)(PgrVgpActionsSample *out_sample);
};

static_assert(sizeof(PgrVgpActionsApi) == 64,
              "PgrVgpActionsApi must remain a 64-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature
// under feature id 26 and answers only on an exact version match.
const void *pgrazor_vgp_actions_api(uint32_t version);

#ifdef __cplusplus
}
#endif

#pragma once
#include <stdint.h>

// Feature 33 (API 1). Native CONSOLIDATED VGP CONTROLLER: the full C++ port of the orchestration that
// runtime/features/vgp.js vgpTick / vgpTransferTick / vgpMark keep inline. It owns ALL VGP orchestration
// -- the scan cadence / alternation / quarantine (via feature 25), the three request actions (feature 26),
// the on-screen marker projection (28), per-candidate eligibility (29), promise/result classification
// (30), inventory + map enumeration (31), the ball recheck + spin cooldown (32), the scoped item service
// (14) and the stateless reflection/getPokemon aux calls (34) -- plus the pieces that never had a native
// sibling: the manual-encounter exclusion root, the pending record and its OWN roots, the 10 s inventory
// wait, the shared-auto-transfer HANDOFF and the on-screen marker edges. It REUSES the sibling tables that
// are already installed by the coordinator (it installs none of them) and it installs NO hook of its own.
//
// NO NEW HOOK: the manual-encounter nameplate site is still instrumented by the shared JS/Gum observer;
// that observer merely forwards (gui, target, owner) into observe(). The controller NEVER calls back into
// JS and NEVER installs Dobby/Gum instrumentation.
//
// SHARED AUTO-TRANSFER stays OUTSIDE: the controller decides a caught Pokemon is a transfer candidate,
// waits out the inventory update, resolves the PK + GM via the aux helper, and HANDS OFF four OWNED GC
// handles (response, pokemon bag, PK, GM) + the full caught id + a fresh token through Out120 with the
// TRANSFER_READY flag. The shared JS transfer classifier/executor (feature 23 + its record) adopts those
// handles, drives the release, and reports progress back through transfer_feedback(). The controller never
// classifies or executes the transfer and never hands the same catch off twice.
//
// PULL-ONLY: every entry point runs managed work only on the single proven Unity owner thread (latched
// from a genuine on-thread call). A foreign thread can only revoke()/stop(); a stale generation blocks a
// NEW action but never a pending final completion. The host polls tick() on the Unity heartbeat and reads
// pull-only telemetry; the native layer never invents a tick and never calls JS.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "VgpController native ABI requires LP64");

enum PgrVgpControllerInfo {
  PGR_VGC_FEATURE_ID     = 33,
  PGR_VGC_API_VERSION    = 1,
  PGR_VGC_CONFIG_VERSION = 1,
  PGR_VGC_TICK_VERSION   = 1,
  PGR_VGC_OUT_VERSION    = 1,
  PGR_VGC_SAMPLE_VERSION = 1,
  PGR_VGC_MAX_LEASE_MS   = 2500  // residual foreground lease ceiling (JS vgpLease gate)
};

// Table-function return codes. 0 == OK (tick), everything negative is a conservative error. tick()/observe
// /transfer_feedback/revoke/stop return PGR_VGC_OK or a negative error; install() the same; generation()
// returns the current revocation generation.
enum PgrVgpControllerStatus {
  PGR_VGC_OK               = 0,
  PGR_VGC_ERR_NULL_ARG     = -1,
  PGR_VGC_ERR_BAD_VERSION  = -2,
  PGR_VGC_ERR_BAD_SIZE     = -3,
  PGR_VGC_ERR_MISSING_PTR  = -4,  // install(): a required config pointer was null
  PGR_VGC_ERR_BAD_TABLE    = -5,  // install(): a sibling table failed version/size/function validation
  PGR_VGC_ERR_NOT_INSTALLED= -6,
  PGR_VGC_ERR_RECONFIG     = -9,  // install(): a different config at an already-installed table
  PGR_VGC_ERR_CLOCK_FAILURE= -10,
  PGR_VGC_ERR_OFF_OWNER    = -12  // an owner-gated call arrived on a foreign thread
};

// out.status: the single user-visible controller state this tick (mirrors vgpStatus in vgp.js).
enum PgrVgpControllerStatusCode {
  PGR_VGC_ST_OFF                 = 0,
  PGR_VGC_ST_WAIT_READY          = 1,   // "Waiting for game readiness"
  PGR_VGC_ST_WAIT_INV            = 2,   // "Waiting for inventory"
  PGR_VGC_ST_OUT_BALLS           = 3,   // "Out of red Poke Balls"
  PGR_VGC_ST_STORAGE_FULL        = 4,   // "Pokemon storage full"
  PGR_VGC_ST_ITEM_FULL           = 5,   // "Item bag full"
  PGR_VGC_ST_RECENT              = 6,   // "Waiting for Pokemon retry / new targets"
  PGR_VGC_ST_OUT_RANGE           = 7,   // "Nearby Pokemon outside 40m catch range"
  PGR_VGC_ST_WATCHING            = 8,   // "Watching nearby Pokemon and PokeStops"
  PGR_VGC_ST_CATCHING            = 9,   // "Catching nearby Pokemon"
  PGR_VGC_ST_SPINNING            = 10,  // "Spinning nearby PokeStop"
  PGR_VGC_ST_CAUGHT              = 11,  // "Pokemon caught"
  PGR_VGC_ST_NOT_CAUGHT          = 12,  // "Pokemon not caught"
  PGR_VGC_ST_SPUN               = 13,   // "PokeStop spun"
  PGR_VGC_ST_SPIN_FAIL          = 14,   // "Spin unsuccessful"
  PGR_VGC_ST_REJECTED           = 15,   // "Request rejected"
  PGR_VGC_ST_TIMEOUT            = 16,   // "Response timed out; toggle VGP to resume"
  PGR_VGC_ST_ERROR              = 17,   // "Paused: ..." (out.error carries the sibling/native code)
  PGR_VGC_ST_CHECK_TRANSFER     = 18,   // "Checking auto-transfer"
  PGR_VGC_ST_TRANSFERRING       = 19,   // "Transferring caught Pokemon"
  PGR_VGC_ST_TRANSFERRED        = 20,   // "Transferred"
  PGR_VGC_ST_TRANSFER_ERROR     = 21,   // "Transfer error" / "Caught; transfer check failed"
  PGR_VGC_ST_KEPT               = 22,   // "Kept: ..."
  PGR_VGC_ST_TRANSFER_INV_TIMEOUT=23,   // "Caught; transfer inventory update timed out"
  PGR_VGC_ST_CLEAN_INV          = 24,   // "Cleaning inventory"
  PGR_VGC_ST_FULL_RECENT        = 25,   // "Waiting for recent targets to expire"
  PGR_VGC_ST_INIT_SUPPORT       = 26,   // "Initializing encounter support"
  PGR_VGC_ST_CATCH_DISABLED     = 27,   // "Pokemon catching disabled"
  PGR_VGC_ST_NATIVE_UNAVAILABLE = 28    // "VGP native actions unavailable"
};

// out.flags: edge-triggered events for THIS tick (OR-combined). event_serial bumps on any non-zero flags.
enum PgrVgpControllerEvent {
  PGR_VGC_EV_CATCH_ISSUED   = 1,    // a capture request was issued this tick (not encounter)
  PGR_VGC_EV_SPIN_ISSUED    = 2,    // a spin request was issued this tick
  PGR_VGC_EV_CATCH_RESULT   = 4,    // a catch result resolved (success/catch_status/retry_ms set)
  PGR_VGC_EV_SPIN_RESULT    = 8,    // a spin result resolved (success set)
  PGR_VGC_EV_ITEM_CLEANUP   = 16,   // a final catch/spin outcome: reset item poll + arm cleanup window
  PGR_VGC_EV_TRANSFER_READY = 32,   // 4 OWNED handles + caught_id + token handed off (adopt or free ALL)
  PGR_VGC_EV_MARKER_UPDATE  = 64,   // marker_x/marker_y/marker_visible are valid this tick
  PGR_VGC_EV_MARKER_HIDE    = 128,  // hide the marker this tick
  PGR_VGC_EV_TRANSFER_CANCEL= 256   // release the shared JS transfer record for transfer_token
};

// tick.flags: boolean host inputs for this tick.
enum PgrVgpControllerTickFlag {
  PGR_VGC_TF_MAP_VISIBLE     = 1,  // s.vgpMapVisible (manual-exclusion aging)
  PGR_VGC_TF_TRANSFER_ENABLED= 2,  // s.transferEnabled
  PGR_VGC_TF_ITEM_ENABLED    = 4,  // s.itemEnabled (cleanup window + vgpCleanupUntil writeback)
  PGR_VGC_TF_MANUAL_READY    = 8   // s.vgpManualReady (encounter support initialised)
};

// out.phase / sample.phase: the controller's current lifecycle phase.
enum PgrVgpControllerPhase {
  PGR_VGC_PH_NONE          = 0,
  PGR_VGC_PH_ENCOUNTER     = 1,
  PGR_VGC_PH_CATCH         = 2,
  PGR_VGC_PH_SPIN          = 3,
  PGR_VGC_PH_INVENTORY_WAIT= 4,  // caught + transfer candidate, waiting for the inventory update
  PGR_VGC_PH_HANDED_OFF    = 5   // the record is with the shared JS transfer executor
};

// transfer_feedback() state: the shared JS executor's progress for a handed-off record.
enum PgrVgpControllerFeedback {
  PGR_VGC_FB_PENDING   = 0,
  PGR_VGC_FB_REQUESTED = 1,  // release RPC issued -> status TRANSFERRING
  PGR_VGC_FB_SUCCESS   = 2,  // -> status TRANSFERRED  (+ CANCEL, record released)
  PGR_VGC_FB_FAILED    = 3,  // -> status TRANSFER_ERROR (+ CANCEL)
  PGR_VGC_FB_KEPT      = 4   // -> status KEPT          (+ CANCEL)
};

// Fixed LP64 config (168 bytes): 8-byte header + 19 pointers + rpc_id + reserved. Pointers 0..8 are the
// already-installed sibling feature tables (the controller re-validates each table's version/size and that
// every function pointer is non-null); 9..18 are the shared il2cpp primitives the controller uses directly
// for the manual-exclusion exclusion identity / alive check. The config is immutable after install().
struct PgrVgpControllerConfig {
  uint32_t version;  // == PGR_VGC_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrVgpControllerConfig)

  void *policy_table;       // 0  PgrVgpPolicyApi*        (feature 25)
  void *actions_table;      // 1  PgrVgpActionsApi*       (feature 26)
  void *candidates_table;   // 2  PgrVgpCandidateApi*     (feature 29)
  void *results_table;      // 3  PgrVgpResultsApi*       (feature 30)
  void *discovery_table;    // 4  PgrVgpDiscoveryApi*     (feature 31)
  void *maintenance_table;  // 5  PgrVgpMaintenanceApi*   (feature 32)
  void *marker_table;       // 6  PgrVgpMarkerApi*        (feature 28)
  void *items_table;        // 7  pgrazor_items_api_v1*   (feature 14)
  void *aux_table;          // 8  pgrazor_vgp_aux_api*    (feature 34)

  void *gc_new;             // 9  uintptr_t (void* obj, int pinned)
  void *gc_get;             // 10 void*     (uintptr_t handle)
  void *gc_free;            // 11 void      (uintptr_t handle)
  void *object_class;       // 12 void*     (void* obj)
  void *runtime_invoke;     // 13 void*     (MethodInfo*, void* obj, void** params, void** exc)
  void *object_unbox;       // 14 void*     (void* boxed) -> value storage
  void *m_alive;            // 15 p.methods.alive.method (static alive(obj) -> Boolean)
  void *class_wild;         // 16 VGP.wild (manual exclusion identity class)
  void *class_nameplate;    // 17 VGP nameplate GUI class (observe() gui identity)
  void *class_bool;         // 18 System.Boolean (alive box class, value size 1)

  int32_t  rpc_id;          // @160 FortSearch rpc id forwarded to the actions spin()
  uint32_t reserved;        // @164 0
};

static_assert(sizeof(PgrVgpControllerConfig) == 168,
              "PgrVgpControllerConfig must remain 168 bytes on LP64");

// Fixed LP64 tick input (80 bytes). owner is the proven Unity owner tid (must equal the current OS tid).
// epoch is the host transfer epoch. lease_ms / allowed_ms / cleanup_ms are RESIDUAL durations (ms) the
// adapter computed from its absolute deadlines; the controller re-samples CLOCK_BOOTTIME and treats each as
// a relative window. generation is the controller generation the adapter captured BEFORE it packed this
// tick's game reads; a mismatch with the live generation blocks a NEW action (never a pending completion).
struct PgrVgpControllerTick {
  uint32_t version;    // == PGR_VGC_TICK_VERSION
  uint32_t size;       // == sizeof(PgrVgpControllerTick)
  uint64_t owner;      // @8  proven Unity owner tid
  uint64_t epoch;      // @16 host transfer epoch
  uint64_t lease_ms;   // @24 residual vgp lease (mask gate)
  uint64_t allowed_ms; // @32 residual foreground/readiness lease (transfer gate)
  uint64_t cleanup_ms; // @40 residual item-cleanup window (vgpCleanupUntil)
  uint64_t generation; // @48 generation captured before the adapter packed this tick
  double   lat;        // @56 player latitude
  double   lon;        // @64 player longitude
  uint32_t mask;       // @72 s.vgpMask (bit1 catch, bit2 spin)
  uint32_t flags;      // @76 PgrVgpControllerTickFlag
};

static_assert(sizeof(PgrVgpControllerTick) == 80,
              "PgrVgpControllerTick must remain 80 bytes on LP64");

// Fixed LP64 tick output (120 bytes). Always fully initialised before any early return. The four transfer
// handles are OWNED and set ONLY alongside PGR_VGC_EV_TRANSFER_READY; the caller adopts ALL FOUR or frees
// ALL FOUR (no partial ownership). All other handle fields are 0.
struct PgrVgpControllerOut {
  uint32_t version;         // @0   == PGR_VGC_OUT_VERSION
  uint32_t size;            // @4   == sizeof(PgrVgpControllerOut)
  uint32_t status;          // @8   PgrVgpControllerStatusCode
  uint32_t phase;           // @12  PgrVgpControllerPhase
  uint32_t flags;           // @16  PgrVgpControllerEvent (OR of this tick's edges)
  uint32_t status_known;    // @20  1 iff catch_status was read from an exact RESP
  int32_t  catch_status;    // @24  catch Status enum when status_known; else 0
  uint32_t success;         // @28  1 iff the resolved catch/spin succeeded
  uint64_t event_serial;    // @32  monotonic; bumps whenever flags != 0
  uint64_t retry_ms;        // @40  catch-result retry suppression (CATCH_RESULT only; else 0)
  uint64_t transfer_token;  // @48  token for a TRANSFER_READY / CANCEL record
  uint64_t caught_id;       // @56  full 64-bit caught id (TRANSFER_READY only)
  uint64_t response_handle; // @64  OWNED catch response (TRANSFER_READY only)
  uint64_t bag_handle;      // @72  OWNED pokemon bag  (TRANSFER_READY only)
  uint64_t pokemon_handle;  // @80  OWNED resolved PK  (TRANSFER_READY only)
  uint64_t gm_handle;       // @88  OWNED GameMaster/full-id helper (TRANSFER_READY only)
  double   marker_x;        // @96  normalized marker x (MARKER_UPDATE only)
  double   marker_y;        // @104 normalized marker y (MARKER_UPDATE only)
  uint32_t marker_visible;  // @112 1 iff on-screen (MARKER_UPDATE only)
  int32_t  error;           // @116 sibling/native error code accompanying PGR_VGC_ST_ERROR
};

static_assert(sizeof(PgrVgpControllerOut) == 120,
              "PgrVgpControllerOut must remain 120 bytes on LP64");

// Fixed LP64 telemetry gauge (32 bytes). Thread-safe pure atomic read. roots is the controller-owned live
// root count (manual + pending target/bag/promise/reflection + the internal transfer response/bag); the
// handed-off exports are NOT counted. generation mirrors generation().
struct PgrVgpControllerSample {
  uint32_t version;     // == PGR_VGC_SAMPLE_VERSION
  uint32_t size;        // == sizeof(PgrVgpControllerSample)
  uint32_t roots;       // @8  live controller-owned GC roots
  uint32_t phase;       // @12 PgrVgpControllerPhase
  uint32_t status;      // @16 last out.status
  int32_t  error;       // @20 last out.error
  uint64_t generation;  // @24 current revocation generation
};

static_assert(sizeof(PgrVgpControllerSample) == 32,
              "PgrVgpControllerSample must remain 32 bytes on LP64");

// Fixed LP64 table: 8-byte header + 8 function pointers = 72 bytes.
struct PgrVgpControllerApi {
  uint32_t version;  // == PGR_VGC_API_VERSION
  uint32_t size;     // == sizeof(PgrVgpControllerApi)
  // install(config): validate the header, the 10 shared primitives and each of the 9 already-installed
  // sibling tables (version/size/every function pointer), then bind the immutable config once for the
  // process lifetime. Idempotent for an identical config; a different config returns PGR_VGC_ERR_RECONFIG.
  // Installs NO hook.
  int (*install)(const PgrVgpControllerConfig *config);
  // tick(in, out): one orchestration pass on the proven owner thread. out is always fully initialised.
  // A reentrant tick is a no-op (out reflects the idle gauge). Returns PGR_VGC_OK or a negative error.
  int (*tick)(const PgrVgpControllerTick *in, PgrVgpControllerOut *out);
  // observe(gui, target, owner): the shared nameplate observer forwards the manual encounter here. On the
  // owner thread only; may be called nested from inside a managed callback during tick(). An exact
  // nameplate gui with an exact wild target replaces the manual-exclusion root (root-before-free); any
  // other target clears it. Returns PGR_VGC_OK or a negative error.
  int (*observe)(void *gui, void *target, uint64_t owner);
  // transfer_feedback(token, state, owner): the shared JS transfer executor reports progress for a
  // handed-off record. Matched by token + owner; a stale token is ignored. Owner thread only.
  int (*transfer_feedback)(uint64_t token, uint32_t state, uint64_t owner);
  // revoke(): restrictive revoke from any thread (mask/epoch/lease became restrictive). Bumps the
  // generation and stops the actions sibling. Root cleanup is deferred to the owner's next tick()/stop().
  int (*revoke)(void);
  // stop(owner): permanent teardown. Bumps the generation, stops actions, resets the policy. Frees every
  // owned root when called on the latched owner thread; otherwise defers the free to the owner's tick().
  int (*stop)(uint64_t owner);
  int (*sample)(PgrVgpControllerSample *out);
  // generation(): the current revocation generation (the adapter captures this before packing a tick).
  uint64_t (*generation)(void);
};

static_assert(sizeof(PgrVgpControllerApi) == 72,
              "PgrVgpControllerApi must remain a 72-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 33 and answers only on an exact version match.
const void *pgrazor_vgp_controller_api(uint32_t version);

#ifdef __cplusplus
}
#endif

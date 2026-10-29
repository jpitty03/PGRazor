#pragma once
#include <cstdint>

#include "catch_animation.h"  // PgrSpeedApi v1: feature49 DELEGATES the speed attempt to feature8.

// PGRazor native migration: catch OBSERVERS ownership (feature id 49, API 1).
//
// Ports the remaining production Gum observers of diagnostics/quick-ball.js that this
// stage still left in JS: the btxu result observer (ordinary-ball response caller path,
// incl. caller/thread/rate/ball-class validation and the confirmed/escape -> speed
// delegation), the outcome dedup+correlation state (the JS outcome()/seen ring, rate
// limit and confirmed/suppressed classification), and the caught(btdd)/escaped(btdf)
// completion observers. See .artifacts/native-migration/catch-observers-contract.txt.
//
// Division of labour:
//   * This module installs the three Dobby hooks (btxu/btdd/btdf) and OWNS the outcome
//     state. It NEVER calls back into JS; telemetry is pull-only through sample().
//   * The per-instance Animator speed attempt is NOT re-implemented here. It is DELEGATED
//     to the existing feature8 speed engine (catch_animation.*, PgrSpeedApi v1) through
//     config.speed_api: feature49 calls speed_api->update() (micro-refresh of feature8's
//     lease, == the JS s.nativeSpeed.update() before apply), ->apply(animator,response)
//     on a confirmed success, and ->complete(response,reason) on btdd/btdf. feature8 keeps
//     owning the GC roots/snapshot/restore and its own lease/tick (JS-driven).
//   * No Gum hook may share any of these three targets while native observers are selected;
//     the read-only Animator trace that piggybacked them stays JS-diagnostic and OFF here.
//
// Rooting / ABI invariants (mirrored from quick-ball.js and the sibling native modules):
//   * btxu: root `self` BEFORE calling the original, root the returned response AFTER, and
//     re-resolve both through gc_get_target for every field read (GC may move them). Return
//     the RE-RESOLVED response pointer, never a pre-operation raw pointer (no stale return).
//     Free both roots on the proven thread AFTER capturing the return address.
//   * Single confirmed success -> one speed delegation; a concurrent owned attempt is
//     skipped (no re-apply). bread>1 latches a fault; bread==1 skips a special ball.
//   * Outcome seen handles are strong, bounded (<=4), expire after 8 s and are freed ONLY
//     on the proven Unity thread (tick prune / stop drain on the next proven tick).
//   * A reentry flag (not a mutex) spans the btxu-leave managed work and completion; no lock
//     is held across any managed field read or the delegated feature8 call.
//   * il2cpp attachment is proven independently of the lease: every hook body and the cleanup
//     tick fail closed (passthrough / skip) unless config.thread_current() returns non-null, so
//     the native module never performs a managed call on an unattached thread. gchandle_free is
//     treated as NON-relocating (freeing a handle removes a table entry; it is not a GC
//     safepoint that moves the just-freed object); every OTHER managed free that can safepoint
//     is ordered BEFORE the final re-resolve+free of the pointer handed back to the game.
//   * Completion (btdd/btdf) is a CLEANUP signal: it is gated on attachment + the cleanup-owner
//     thread (g_cleanup_tid), NOT on a live lease, so a restore delegated to feature8 still runs
//     after the observer lease has expired (the animation routinely completes past the 2.5 s
//     observer lease). A revocation generation is captured when a btxu arms and re-checked
//     immediately before the delegated speed mutation so a concurrent stop() aborts the apply.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "CatchObservers native ABI requires LP64");

enum PgrObsInfo {
    PGR_OBS_FEATURE_ID  = 49,
    PGR_OBS_API_VERSION = 1,
    PGR_OBS_CONFIG_VERSION = 1,
    PGR_OBS_SAMPLE_VERSION = 1,
    PGR_OBS_MAX_LEASE_MS = 2500,  // LIMITS.leaseMs
    PGR_OBS_EVENT_RING   = 16     // bounded native event ring (append table drained by cursor)
};

// Status codes (0 == success); aligned with the sibling native modules.
enum PgrObsStatus {
    PGR_OBS_OK = 0,
    PGR_OBS_ERR_NULL = 1,
    PGR_OBS_ERR_BAD_CONFIG = 2,
    PGR_OBS_ERR_HOOK = 3,
    PGR_OBS_ERR_LEASE = 4,
    PGR_OBS_ERR_NOT_INSTALLED = 6,
    PGR_OBS_ERR_BAD_ARG = 9
};

// Event (action) codes surfaced through sample().action.
enum PgrObsAction {
    PGR_OBS_A_NONE = 0,
    PGR_OBS_A_OUTCOME_CONFIRMED = 1,
    PGR_OBS_A_OUTCOME_SUPPRESSED = 2,
    PGR_OBS_A_OUTCOME_UNAVAILABLE = 3,
    PGR_OBS_A_SKIP = 4,
    PGR_OBS_A_APPLIED = 5,
    PGR_OBS_A_COMPLETED = 6
};

// Reason codes surfaced through sample().reason.
enum PgrObsReason {
    PGR_OBS_R_NONE = 0,
    // suppressed reasons
    PGR_OBS_R_AMBIGUOUS = 1,      // CatchMissed
    PGR_OBS_R_NOT_CONFIRMED = 2,  // known literal, not confirmed (error / item-replacement)
    PGR_OBS_R_UNKNOWN = 3,        // status value matched no known literal
    // confirmed outcomes
    PGR_OBS_R_CAUGHT = 10,        // CatchSuccess
    PGR_OBS_R_BROKE_FREE = 11,    // CatchEscape
    PGR_OBS_R_FLED = 12,          // CatchFlee
    // skip reasons
    PGR_OBS_R_CONCURRENT = 20,
    PGR_OBS_R_SPECIAL = 21,
    PGR_OBS_R_REVOKED = 22        // stop() bumped the revocation generation mid-flight
};

// Fixed LP64 config (168 bytes): 8-byte header + 15 validated pointers + 6 int32 literals
// + 2 reserved int32 + 1 trailing validated pointer (thread_current). thread_current was
// appended at the END of the still-unreleased config ABI so the observer can prove il2cpp
// attachment itself (defence in depth, mirroring the sibling native modules) rather than
// trusting gettid() alone. See the contract for the exact pointer order.
struct PgrObsConfig {
    uint32_t version;              // 0   == PGR_OBS_CONFIG_VERSION
    uint32_t size;                 // 4   == sizeof(PgrObsConfig)

    void *object_get_class;        // 8   void* (void* obj)
    void *field_get_value;         // 16  void  (void* obj, FieldInfo*, void* out)
    void *gchandle_new;            // 24  void* (void* obj, int pinned)
    void *gchandle_get_target;     // 32  void* (void* handle)
    void *gchandle_free;           // 40  void  (void* handle)

    void *code_btxu;               // 48  Pokeball.btxu patch site
    void *code_caught;             // 56  EncounterCaptureState.btdd
    void *code_escaped;            // 64  EncounterCaptureState.btdf

    void *class_ball;              // 72  exact Pokeball class
    void *class_response;          // 80  exact CatchPokemonOutProto class
    void *class_animator;          // 88  exact UnityEngine.Animator class

    void *field_bread;             // 96  Pokeball <HoldingBreadBall> bool
    void *field_animator;          // 104 Pokeball animator ptr
    void *field_status;            // 112 CatchPokemonOutProto Status (Int32 enum)

    void *speed_api;               // 120 feature8 PgrSpeedApi* (v1, 56-byte table)

    int32_t status_success;        // 128
    int32_t status_escape;         // 132
    int32_t status_flee;           // 136
    int32_t status_missed;         // 140
    int32_t status_error;          // 144
    int32_t status_item_replacement; // 148
    uint32_t reserved;             // 152
    uint32_t reserved2;            // 156

    void *thread_current;          // 160 void* (void) -> attached Unity thread or null (appended)
};

static_assert(sizeof(PgrObsConfig) == 168, "PgrObsConfig must remain 168 bytes on LP64");

// One entry of the bounded native event ring. Each slot carries its own global `seq` so a
// reader (the JS adapter) can detect tearing (slot seq != expected) and gaps (seq skipped),
// instead of silently dropping every outcome but the last between two polls.
struct PgrObsEvent {
    uint64_t seq;        // 0   1-based global append sequence (0 == never written)
    uint64_t tid;        // 8   owner tid when the event was emitted
    uint32_t action;     // 16  event code (PgrObsAction)
    uint32_t reason;     // 20  reason code (PgrObsReason)
    int32_t  status;     // 24  raw status value for outcome events
    uint32_t pad;        // 28  reserved / alignment
};

static_assert(sizeof(PgrObsEvent) == 32, "PgrObsEvent must remain 32 bytes on LP64");

// Pull-only telemetry (616 bytes). The monotonic counters + last_* slot are retained for
// debugging/compat; the primary event stream is the bounded ring drained by cursor through
// `event_head`. The adapter turns ring entries into the existing outcome-* / skip event names
// and surfaces an 'outcome-dropped' note on a detected gap; no native->JS callback is made.
struct PgrObsSample {
    uint32_t version;    // 0   == PGR_OBS_SAMPLE_VERSION
    uint32_t size;       // 4   == sizeof(PgrObsSample)
    uint64_t confirmed;  // 8   confirmed outcomes reported
    uint64_t suppressed; // 16  suppressed outcomes reported
    uint64_t dropped;    // 24  outcomes dropped (rate / queue pressure)
    uint64_t applied;    // 32  speed apply() delegations issued
    uint64_t completed;  // 40  speed complete() delegations issued
    uint64_t faults;     // 48  latched faults observed
    uint64_t last_tid;   // 56  owner tid of the last event
    uint32_t serial;     // 64  low 32 bits of event_head (one bump per emitted event)
    uint32_t action;     // 68  last event code (PgrObsAction)
    uint32_t reason;     // 72  last reason code (PgrObsReason)
    int32_t  status;     // 76  raw status value for the last outcome event
    uint32_t seen_count; // 80  retained roots: <=4 outcome handles + <=1 deferred receiver
    uint32_t fault;      // 84  latched fault flag
    uint64_t last_caller;// 88  last observed btxu return address (debug; validates the caller gate)
    uint64_t event_head; // 96  total events ever appended to the ring (cursor horizon)
    PgrObsEvent events[PGR_OBS_EVENT_RING]; // 104  the ring (indexed by seq % PGR_OBS_EVENT_RING)
};

static_assert(sizeof(PgrObsSample) == 616, "PgrObsSample must remain 616 bytes on LP64");

// Fixed LP64 table: 8-byte header + 5 function pointers = 48 bytes.
struct PgrObsApi {
    uint32_t version;    // == PGR_OBS_API_VERSION
    uint32_t size;       // == sizeof(PgrObsApi)
    int (*install)(const PgrObsConfig *config);
    // Arm feature49's own observer/outcome gate. lease clamped/refused above 2500 ms. enabled==0
    // closes the window (soft stop). expected_caller gates the btxu return address (nullptr ->
    // accept any). Carries the proven Unity tid. Any thread may close; arming needs the owner tid.
    int (*update)(int enabled, uint64_t remaining_lease_ms, uint64_t unity_tid, void *expected_caller);
    // Proven-thread cleanup pass: prune expired outcome handles (drain all on stop/fault/off).
    int (*tick)(uint64_t proven_tid);
    // Immediate revoke from any thread: set stopping + bump generation; frees nothing (deferred to
    // the next proven tick). A later update() re-arms.
    int (*stop)(uint64_t proven_tid);
    int (*sample)(PgrObsSample *out);
};

static_assert(sizeof(PgrObsApi) == 48, "PgrObsApi must remain a 48-byte version-1 table");

// API2: one immutable compiled berry predicate at the existing btxu owner.
// Called only on the registered attached owner with a live rooted receiver.
// 1 holds the pending response (return null without original); 0 passes through.
// Callback must not throw, unload, install hooks or call the original itself.
typedef int (*PgrObsBerryFilter)(void *rooted_ball,uint64_t owner_tid);
struct PgrObsApi2 {
    uint32_t version,size;
    // Exact callback+owner repeats are idempotent; rebinding is BAD_CONFIG.
    // Registration requires API1 installation and actual attached owner thread.
    int (*register_filter)(PgrObsBerryFilter,uint64_t owner_tid);
    int (*filter_ready)();
};
static_assert(sizeof(PgrObsApi2)==24,"Catch observer berry bridge API2");

// Hidden getter. NOT exported by the version script; the coordinator registers it in
// query_feature under feature id 49 and answers only on an exact version match.
const void *pgrazor_catch_observers_api(uint32_t version);

#ifdef __cplusplus
}
#endif

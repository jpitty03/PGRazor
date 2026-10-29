#pragma once
#include <stdint.h>

// Feature 51 (API 1). Native SHARED CATCH ORCHESTRATION: the compiled owner of the shared manual
// catch/summary/details RECORD table, CANDIDATE QUEUE state, outcome correlation, the strictly-later
// Unity-tick ARMING rule, PRUNE/lifetime decisions, and the transfer-callback token<->slot correlation
// that diagnostics/quick-close.js used to own in readable JS (the cleanupTick record loop, recordForGui,
// cancel, prune, the candidate.find/readyMatch arming, and the transferCatch/transferTick/finishTransfer
// correlation). See .artifacts/native-migration/shared-catch-contract.txt for the full division of
// labour.
//
// SCOPE / NOT: this module installs NO hook and makes NO managed/il2cpp call. It STORES NO MANAGED
// POINTER -- a record is keyed by an OPAQUE host-chosen gui key (uint64) and a monotonic slot id; the
// GC roots stay owned and freed by the host JS on the owner thread. The managed identity re-validation
// and the actual dismissal invoke stay in the already-native leaf features (24 catch_completion, 36-40
// summary/details completion+recovery+canvas+discovery). Transfer CLASSIFICATION and the ReleasePokemon
// LIFECYCLE stay in features 4/35/23; feature 51 holds ONLY the token<->slot attribution (one-shot).
// The single shared ExecuteTasks Interceptor remains the one existing Gum owner of that address; feature
// 51 is a tick-driven, pull-only consumer and adds no second owner on any target and drops no observer.
//
// Lifetime / concurrency: the owner tid is latched from a genuine on-thread call; a foreign thread can
// only revoke (update enabled->0 / stop). Every mutating op requires current tid == proven tid ==
// latched owner, else it is a fail-closed no-op. The gate (enabled + finite lease clamped to 2500 ms +
// epoch, re-read against CLOCK_BOOTTIME) controls select(); a dead gate drains the queue without a
// decision. A single state mutex guards the table; no mutex is held across a host callback (there is no
// managed callback). When native prunes or reaps a slot it emits a pull-only drain event so the host
// frees the matching GC root; the host never frees a root native still references (native references
// none). Decisions are proposals: the host and leaf completion feature revalidate current
// policy before managed mutation. Revocation cannot retract a proposal already returned.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "SharedCatch native ABI requires LP64");

enum PgrSharedCatchInfo {
  PGR_SH_FEATURE_ID      = 51,
  PGR_SH_API_VERSION     = 1,
  PGR_SH_CONFIG_VERSION  = 1,
  PGR_SH_SAMPLE_VERSION  = 1,
  PGR_SH_DECISION_VERSION = 1,
  PGR_SH_EVENT_VERSION   = 1,
  PGR_SH_MAX_RECORDS     = 4,     // bounded candidate table (JS LIMITS.maxRecords)
  PGR_SH_TIMEOUT_MS      = 8000,  // per-candidate uncertain deadline (JS LIMITS.timeoutMs)
  PGR_SH_MAX_LEASE_MS    = 2500,  // residual foreground lease ceiling (JS allowedUntil gate)
  PGR_SH_XFER_SLOTS      = 64     // bounded token<->slot correlation table
};

enum PgrSharedCatchStatus {
  PGR_SH_OK              = 0,
  PGR_SH_ERR_NULL_ARG    = -1,
  PGR_SH_ERR_BAD_VERSION = -2,
  PGR_SH_ERR_BAD_SIZE    = -3,
  PGR_SH_ERR_MISSING_PTR = -4,   // out-of-range config bound
  PGR_SH_ERR_NOT_INSTALLED = -6,
  PGR_SH_ERR_RECONFIG    = -9,
  PGR_SH_ERR_CLOCK_FAILURE = -10,
  PGR_SH_ERR_OVERFLOW    = -13   // FIFO full (fail closed)
};

enum PgrSharedCatchKind {
  PGR_SH_KIND_SUMMARY = 0,
  PGR_SH_KIND_DETAILS = 1
};

// Candidate progress stages fed from the host's opener/init observers (owner thread).
enum PgrSharedCatchStage {
  PGR_SH_STAGE_INIT          = 1,  // aux bit0 = confirmed success, bit1 = captured id present
  PGR_SH_STAGE_SET_SCORES    = 2,
  PGR_SH_STAGE_OPEN_COMPLETE = 3,
  PGR_SH_STAGE_PRESENT       = 4   // details present (kind == DETAILS)
};

// select() action.
enum PgrSharedCatchAction {
  PGR_SH_ACT_NONE    = 0,
  PGR_SH_ACT_ARM     = 1,  // first ready tick: latch readyMatch, wait for a strictly later tick
  PGR_SH_ACT_DISMISS = 2   // ready on a strictly later tick: the host runs the leaf dismissal
};

// Cancel / prune reason codes. 1..7 are native-internal; a host cancel() passes its own reason through
// verbatim and should use >= 8 (PGR_SH_REASON_HOST) to avoid colliding with the internal set.
enum PgrSharedCatchReason {
  PGR_SH_REASON_PERMISSION_REVOKED = 1,  // record.epoch != armed epoch
  PGR_SH_REASON_STOP               = 2,
  PGR_SH_REASON_OFF                = 3,  // gate disabled (allowedUntil == 0)
  PGR_SH_REASON_LEASE_EXPIRED      = 4,
  PGR_SH_REASON_TIMEOUT            = 5,  // now >= record deadline
  PGR_SH_REASON_RECORD_OVERFLOW    = 6,  // evicted to make room (JS 'record-overflow')
  PGR_SH_REASON_NEW_INITIALIZATION = 7,  // cancel_all (JS 'new-initialization')
  PGR_SH_REASON_HOST               = 8   // and above: host-supplied explicit cancel reason
};

enum PgrSharedCatchEventKind {
  PGR_SH_EVENT_CANCELLED = 1,  // slot dropped (cancel/prune/overflow); reason in .reason; host frees root
  PGR_SH_EVENT_REAPED    = 2   // slot finished (invoked) and removed; host frees root
};

// Fixed LP64 config (32 bytes): 8-byte header + 6 uint32 bounds. No pointers -- this module calls nothing.
struct PgrSharedCatchConfig {
  uint32_t version;          // == PGR_SH_CONFIG_VERSION
  uint32_t size;             // == sizeof(PgrSharedCatchConfig)
  uint32_t max_records;      // 1..PGR_SH_MAX_RECORDS
  uint32_t timeout_ms;       // 1..PGR_SH_TIMEOUT_MS
  uint32_t lease_ceiling_ms; // 1..PGR_SH_MAX_LEASE_MS
  uint32_t xfer_slots;       // 1..PGR_SH_XFER_SLOTS
  uint32_t reserved0;        // 0
  uint32_t reserved1;        // 0
};

static_assert(sizeof(PgrSharedCatchConfig) == 32, "PgrSharedCatchConfig must remain 32 bytes on LP64");

// Fixed LP64 select() output (40 bytes).
struct PgrSharedCatchDecision {
  uint32_t version;  // == PGR_SH_DECISION_VERSION
  uint32_t size;     // == sizeof(PgrSharedCatchDecision)
  uint32_t action;   // PgrSharedCatchAction
  uint32_t kind;     // PgrSharedCatchKind of the target slot (valid when action != NONE)
  uint64_t slot;     // target slot id (0 when action == NONE)
  uint64_t gui_key;  // the opaque gui key of the target slot (host maps back to its GC root)
  uint64_t tick;     // the current native-observed unity tick (== the unity_ticks the host passed)
};

static_assert(sizeof(PgrSharedCatchDecision) == 40, "PgrSharedCatchDecision must remain 40 bytes on LP64");

// Fixed LP64 drain record (24 bytes). slot/gui_key are opaque host ids -- never a managed pointer.
struct PgrSharedCatchEvent {
  uint32_t kind;    // PgrSharedCatchEventKind
  int32_t  reason;  // CANCELLED: PgrSharedCatchReason; REAPED: 0
  uint64_t slot;
  uint64_t gui_key;
};

static_assert(sizeof(PgrSharedCatchEvent) == 24, "PgrSharedCatchEvent must remain 24 bytes on LP64");

// Fixed LP64 telemetry gauge (88 bytes). Pull-only; the native layer never calls the host.
struct PgrSharedCatchSample {
  uint32_t version;         // == PGR_SH_SAMPLE_VERSION
  uint32_t size;            // == sizeof(PgrSharedCatchSample)
  uint64_t created_serial;  // bumps on each upsert that creates a new record
  uint64_t armed_serial;    // bumps on each ARM decision
  uint64_t dismiss_serial;  // bumps on each DISMISS decision
  uint64_t cancel_serial;   // bumps on each CANCELLED event
  uint64_t reap_serial;     // bumps on each REAPED event
  uint64_t xfer_bind_serial;    // bumps on each transfer_bind
  uint64_t xfer_resolve_serial; // bumps on each successful one-shot transfer_resolve
  uint64_t last_tid;        // owner tid of the last mutating event
  uint32_t records;         // live records currently tracked
  uint32_t pending_events;  // records currently queued in the drain FIFO
  uint32_t open_xfer;       // token<->slot bindings currently held
  uint32_t fault;           // 1 if a bounded table ever overflowed (latched)
};

static_assert(sizeof(PgrSharedCatchSample) == 88, "PgrSharedCatchSample must remain 88 bytes on LP64");

// Fixed LP64 table: 8-byte header + 13 function pointers = 112 bytes.
struct PgrSharedCatchApi {
  uint32_t version;  // == PGR_SH_API_VERSION
  uint32_t size;     // == sizeof(PgrSharedCatchApi)
  int (*install)(const PgrSharedCatchConfig *config);
  // Refresh the gate: enabled, a residual lease clamped to the configured ceiling, and the caller's
  // current epoch. Carries proven_tid; once the owner is latched a foreign thread can only revoke. Any
  // thread. enabled==0 marks OFF and clears the lease; stop() (below) marks STOPPING in addition.
  int (*update)(int enabled, uint64_t remaining_lease_ms, uint64_t epoch, uint64_t proven_tid);
  // Cancel every non-invoked record (JS "new-initialization" on a fresh SummaryState.Initialize onEnter).
  // Each dropped slot emits a CANCELLED event. Owner thread only.
  int (*cancel_all)(int32_t reason, uint64_t proven_tid);
  // Find the record for gui_key or create one (kind set on create). On a full table the first non-invoked
  // record is evicted (CANCELLED, reason record-overflow); if none is evictable *out_slot = 0 (refused).
  // Owner thread only. *out_slot is the slot id (nonzero) or 0.
  int (*upsert)(uint64_t gui_key, uint32_t kind, uint64_t proven_tid, uint64_t *out_slot);
  // Set a candidate progress stage (INIT/SET_SCORES/OPEN_COMPLETE/PRESENT). Resets the slot's readyMatch
  // so arming re-waits for a strictly later tick, mirroring the JS r.readyMatch=null on each progress.
  // Owner thread only. A no-op on an unknown/cancelled/invoked slot.
  int (*mark)(uint64_t slot, uint32_t stage, uint32_t aux, uint64_t proven_tid);
  // Explicit host cancel (user-dismiss, response-mismatch, state-exit, gui-class-mismatch, ...). reason
  // is passed through to the CANCELLED event. Owner thread only. A no-op on an unknown/invoked slot.
  int (*cancel)(uint64_t slot, int32_t reason, uint64_t proven_tid);
  // One owner-thread tick. Prunes stale/off/expired/timed-out records (CANCELLED events, JS prune order),
  // then returns at most one decision in *out: NONE, ARM (first ready tick, latch readyMatch), or DISMISS
  // (ready on a strictly later unity tick than readyMatch). unity_ticks is the host's proven Unity-thread
  // tick count (from the retained single ExecuteTasks observer). Owner thread only; off-owner yields NONE.
  int (*select)(uint64_t unity_ticks, uint64_t proven_tid, PgrSharedCatchDecision *out);
  // Latch a candidate as invoked after the host ran the leaf dismissal, then reap it (REAPED event). A
  // suppressed/failed dismiss still latches so the decision is never issued twice. Owner thread only.
  int (*finish)(uint64_t slot, int advanced, uint64_t proven_tid);
  // Bind a feature-23 release request token to the originating candidate slot (transfer correlation).
  // Owner thread only. Overflow fails closed (ERR_OVERFLOW, fault latched).
  int (*transfer_bind)(uint64_t slot, uint64_t token, uint64_t proven_tid);
  // One-shot: resolve + CONSUME the slot a feature-23 token belongs to when draining a release event.
  // A duplicate/late/unknown token yields *out_slot = 0 (no re-attribution). Owner thread only.
  int (*transfer_resolve)(uint64_t token, uint64_t proven_tid, uint64_t *out_slot);
  // Immediate revoke from any thread: disable the gate, mark stopping, bump the revocation generation.
  // A later update(enabled) re-arms and clears stopping. No host callback; no deadlock.
  int (*stop)(uint64_t proven_tid);
  int (*sample)(PgrSharedCatchSample *out_sample);
  // Pop one drain record (returns 1), report empty (0), or a negative status. Any thread.
  int (*next_event)(PgrSharedCatchEvent *out);
};

static_assert(sizeof(PgrSharedCatchApi) == 112, "PgrSharedCatchApi must remain a 112-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 51 and answers only on an exact version match.
const void *pgrazor_shared_catch_api(uint32_t version);

#ifdef __cplusplus
}
#endif

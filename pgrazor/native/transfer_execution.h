#pragma once
#include <stdint.h>

#include "transfer_policy.h"  // PgrTransferInput + pgrazor_transfer_policy_api(1)->decide (feature 4)

// Feature 23 (API 1). Native request LIFECYCLE for Pokemon auto-transfer: a focused C++ port of the
// issue/callback-bookkeeping machinery in diagnostics/quick-close.js transferCatch (~1676-1752) and
// finishTransfer/transferTick/the result+error observers (~1504-1544). It owns ONE thing the JS no
// longer does: the ReleasePokemon invocation and everything around it -- the current-bag identity
// re-lookup, the durable bag/pokemon/promise GC roots, the process-wide "seen captured id" quarantine
// (max 4096), the bounded pending-transaction table (max 64), callback matching, the monotonic request
// token, and the 10 s timeout bookkeeping.
//
// SCOPE / NOT: the classification (keep-species / protect flags / IV cutoff) and the encounter/source
// validation (direct, details-GUI, VGP response) stay in JS this increment. The policy decision is the
// EXISTING native transfer_policy (feature 4) -- submit() RE-RUNS decide() and never trusts any caller
// "authorized" bit. This module installs NO Dobby/Gum hook and NEVER calls back into JS: the host keeps
// its existing release-result and release-error Gum observers and forwards each (onEnter -> onLeave)
// into observe_begin()/observe_end(); the host maps the returned token to its record AFTER submit()
// returns, then drains sample()/next_event(). The observer halves MUST NOT drain, so a synchronous
// callback firing inside ReleasePokemon can never beat the host's token->record registration.
//
// The caller supplies an already-classified request: the proven caught id, the current bag and pokemon
// (both re-validated in JS immediately before the call), the caller's current epoch, the classified
// PgrTransferInput by value, and the proven owner tid. submit() re-roots, re-resolves across every
// managed call, re-looks-up bag.GetPokemon(id), proves it is the SAME rooted pokemon with the SAME id,
// re-runs the feature-4 policy, applies a fresh epoch+lease+owner gate IMMEDIATELY before the release,
// latches seen+issued BEFORE the invoke (so an uncertain release is never retried), and always writes
// *out_token once issued (even on a throw or a null promise). See transfer_execution.cpp for the
// rooting / reentrancy / deferred-teardown discipline (modelled on outgoing_gifts.cpp).

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "TransferExecution native ABI requires LP64");

enum PgrTransferExecInfo {
  PGR_TX_FEATURE_ID     = 23,
  PGR_TX_API_VERSION    = 1,
  PGR_TX_CONFIG_VERSION = 1,
  PGR_TX_REQUEST_VERSION = 1,
  PGR_TX_SAMPLE_VERSION = 1,
  PGR_TX_EVENT_VERSION  = 1,
  PGR_TX_MAX_LEASE_MS   = 2500,   // residual foreground lease ceiling (JS allowedUntil gate)
  PGR_TX_TIMEOUT_MS     = 10000,  // issued-request uncertain deadline (JS r.transferDeadline = now+10000)
  PGR_TX_SEEN_MAX       = 4096,   // process-wide captured-id quarantine (JS transferSeen.size >= 4096)
  PGR_TX_PENDING_MAX    = 64,     // bounded concurrent issued transactions; overflow fails closed
  PGR_TX_EVENT_CAP      = 64,     // bounded FIFO depth; overflow fails closed
  PGR_TX_TICKET_SLOTS   = 64      // bounded observe_begin/observe_end tickets (nested callbacks)
};

// submit() returns 1 ISSUED / 0 REFUSED / negative error. Every other table function returns
// PGR_TX_OK (0) on success or a negative error. observe_begin() returns a nonzero ticket or 0.
enum PgrTransferExecStatus {
  PGR_TX_ISSUED            = 1,   // submit(): release was issued, *out_token set
  PGR_TX_OK                = 0,   // generic success / submit(): REFUSED (policy/gate/duplicate/thread)
  PGR_TX_ERR_NULL_ARG      = -1,
  PGR_TX_ERR_BAD_VERSION   = -2,
  PGR_TX_ERR_BAD_SIZE      = -3,
  PGR_TX_ERR_MISSING_PTR   = -4,
  PGR_TX_ERR_NOT_INSTALLED = -6,
  PGR_TX_ERR_RECONFIG      = -9,
  PGR_TX_ERR_CLOCK_FAILURE = -10,
  PGR_TX_ERR_IDENTITY      = -11,  // captured id 0 / class mismatch / id mismatch / GetPokemon mismatch
  PGR_TX_ERR_GCHANDLE      = -12,  // a GC root could not be allocated (fail closed: NO release)
  PGR_TX_ERR_OVERFLOW      = -13,  // pending/FIFO table full (fail closed)
  PGR_TX_ERR_NO_POLICY     = -14   // feature-4 transfer_policy api(1) was not resolvable at install
};

// next_event() record kinds. id/token/aux are codes/ints only -- never a pokemon/bag id string.
enum PgrTransferExecEventKind {
  PGR_TX_EVENT_REQUEST  = 1,  // optional: a release was issued                     -> transfer-requested
  PGR_TX_EVENT_COMPLETE = 2,  // callback completed; aux bit0 = success(1)/error(0) -> transfer-confirmed/-error
  PGR_TX_EVENT_TIMEOUT  = 3   // 10 s elapsed with no callback (uncertain, once)    -> transfer-unconfirmed
};

// Fixed LP64 config (144 bytes): 8-byte header + 16 validated il2cpp pointers + 2 int32. Every pointer
// is an already-validated export / MethodInfo* / Il2CppClass* / FieldInfo* packed by the host straight
// off p.transfer; the native layer re-checks the header and rejects any null. No hook address.
struct PgrTransferExecConfig {
  uint32_t version;  // == PGR_TX_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrTransferExecConfig)

  // il2cpp runtime exports.
  void *gchandle_new;         // uintptr_t (void* obj, int pinned)
  void *gchandle_get_target;  // void*     (uintptr_t handle)
  void *gchandle_free;        // void      (uintptr_t handle)
  void *object_get_class;     // void*     (void* obj)
  void *runtime_invoke;       // void*     (MethodInfo*, void* obj, void** params, void** exc)
  void *field_get_value;      // void      (void* obj, FieldInfo*, void* out)

  // MethodInfo* (invoked via runtime_invoke).
  void *m_get;      // bag.GetPokemon(UInt64) -> Pokemon (params[0] = &captured_id)
  void *m_release;  // bag.ReleasePokemon(Pokemon) -> IPromise (params[0] = pokemon)

  // Exact Il2CppClass* identities.
  void *class_bag;          // the bag/inventory holder (t.bag host object class)
  void *class_pokemon;      // Pokemon (PK)
  void *class_callback;     // the release callback nested type (t.callbackClass)
  void *class_release_out;  // Holoholo.Rpc ReleasePokemon response (RELEASE)

  // FieldInfo* (read via field_get_value).
  void *f_id;               // Pokemon.id (UInt64) -- on pokemon, current and callbackPokemon
  void *f_callback_pokemon; // callback.<pokemon> -> Pokemon
  void *f_callback_bag;     // callback.<bag> -> bag
  void *f_status;           // ReleaseOut status (Int32 enum)

  // Validated enum literal.
  int32_t success_literal;  // t.resultSuccess (ReleaseOut status Success)
  int32_t reserved;         // 0
};

static_assert(sizeof(PgrTransferExecConfig) == 144,
              "PgrTransferExecConfig must remain 144 bytes on LP64");

// Fixed LP64 submit request (104 bytes). The caller has ALREADY classified the pokemon and validated
// its encounter/source in JS immediately before this call; native then protects the window with the
// epoch/lease generation. bag/pokemon are raw managed pointers rooted by native on entry.
struct PgrTransferExecRequest {
  uint32_t version;         // == PGR_TX_REQUEST_VERSION
  uint32_t size;            // == sizeof(PgrTransferExecRequest)
  uint64_t captured_id;     // proven caught id (must be nonzero and match the pokemon/current id)
  uint64_t epoch;           // caller's current epoch; must equal the armed epoch at the final gate
  uint64_t tid;             // proven owner tid (must equal the current OS tid)
  void *bag;                // current bag (GetPokemon target + ReleasePokemon receiver)
  void *pokemon;            // current pokemon (release target; must equal bag.GetPokemon(id))
  PgrTransferInput input;   // the feature-4 classification, re-decided by submit()
};

static_assert(sizeof(PgrTransferExecRequest) == 104,
              "PgrTransferExecRequest must remain 104 bytes on LP64");

// Fixed LP64 telemetry gauge (88 bytes). Pull-only; the native layer never calls JS.
struct PgrTransferExecSample {
  uint32_t version;         // == PGR_TX_SAMPLE_VERSION
  uint32_t size;            // == sizeof(PgrTransferExecSample)
  uint64_t request_serial;  // bumps on each issued release
  uint64_t complete_serial; // bumps on each completed callback (success or error)
  uint64_t timeout_serial;  // bumps on each one-shot 10 s timeout
  uint64_t error_serial;    // bumps on each completed callback error
  uint64_t last_tid;        // owner tid of the last event
  uint64_t last_token;      // last issued request token
  uint32_t roots;           // durable GC roots + active frames (coordinator finish guard)
  uint32_t pending;         // issued transactions currently tracked
  uint32_t seen;            // captured ids in the process-wide quarantine
  uint32_t fault;           // 1 once latched (FIFO/pending overflow)
  uint32_t overflow;        // 1 if a bounded table ever overflowed
  uint32_t pending_events;  // records currently queued in the FIFO
  uint32_t open_tickets;    // observe_begin tickets awaiting observe_end
  uint32_t reserved;        // 0
};

static_assert(sizeof(PgrTransferExecSample) == 88,
              "PgrTransferExecSample must remain 88 bytes on LP64");

// Fixed LP64 drain record (24 bytes). token is the monotonic safe-JS request token; id/aux are
// codes/ints only, never an id string.
struct PgrTransferExecEvent {
  uint32_t kind;      // PgrTransferExecEventKind
  int32_t  id;        // reserved reason code (0 in v1)
  uint64_t token;     // the request token this record belongs to
  int32_t  aux;       // COMPLETE: bit0 = success; else 0
  uint32_t reserved;  // 0
};

static_assert(sizeof(PgrTransferExecEvent) == 24,
              "PgrTransferExecEvent must remain 24 bytes on LP64");

// Fixed LP64 table: 8-byte header + 8 function pointers = 72 bytes.
struct PgrTransferExecApi {
  uint32_t version;  // == PGR_TX_API_VERSION
  uint32_t size;     // == sizeof(PgrTransferExecApi)
  int (*install)(const PgrTransferExecConfig *config);
  // Refresh the policy gate: enabled, a residual lease clamped to 2500 ms, and the caller's current
  // epoch (bumped by the caller whenever keep settings change). Carries proven_tid; once the owner is
  // latched a foreign thread can only revoke (enabled is forced to 0). Any thread.
  int (*update)(int enabled, uint64_t remaining_lease_ms, uint64_t epoch, uint64_t proven_tid);
  // One owner-thread release lifecycle. Returns PGR_TX_ISSUED (1, *out_token set), PGR_TX_OK (0,
  // refused: policy/duplicate/gate/thread), or a negative error. *out_token is written once issued,
  // including when ReleasePokemon throws or returns a null promise (uncertain, retained, no retry).
  int (*submit)(const PgrTransferExecRequest *request, uint64_t *out_token);
  // Latch a release result/error callback match (onEnter). Returns a nonzero one-shot ticket when the
  // callback's class, rooted bag identity and rooted pokemon identity+id match an issued transaction,
  // else 0. is_error != 0 forces failure (the error observer); otherwise success requires an exact
  // ReleaseOut response whose status equals the configured literal. Owner thread only. Does NOT free.
  uint64_t (*observe_begin)(void *callback, void *response, int is_error, uint64_t proven_tid);
  // Complete the latched match (onLeave). One-shot; a stale/duplicate/unknown ticket is a no-op. Emits
  // one COMPLETE record; the transaction's roots are freed at the outermost owner frame. Owner thread.
  int (*observe_end)(uint64_t ticket, uint64_t proven_tid);
  // One owner-thread bookkeeping pass: emit a one-shot TIMEOUT for any issued request past 10 s and
  // reap completed transactions. stopping != 0 revokes the gate immediately (any thread) and tears down
  // every tracked transaction at the outermost owner frame; the seen quarantine persists. No-op off
  // the owner thread (never frees).
  int (*tick)(uint64_t proven_tid, int stopping);
  int (*sample)(PgrTransferExecSample *out_sample);
  // Pop one FIFO record (returns 1), report empty (0), or a negative status. Any thread.
  int (*next_event)(PgrTransferExecEvent *out);
};

static_assert(sizeof(PgrTransferExecApi) == 72,
              "PgrTransferExecApi must remain a 72-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature
// under feature id 23 and answers only on an exact version match.
const void *pgrazor_transfer_execution_api(uint32_t version);

#ifdef __cplusplus
}
#endif

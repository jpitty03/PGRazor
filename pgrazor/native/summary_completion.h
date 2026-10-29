#pragma once
#include <stdint.h>

// Feature 36 (API 1). Native ORDINARY summary dismissal: a focused C++ port of the confirmed
// summary-return branch of diagnostics/quick-close.js cleanupTick (~1256-1328) -- the summary
// identity validation, the single inherited GuiController.Dismiss() invoke, and the postcondition
// read (currentState advanced off STATE_OPEN, or the GUI is gone). It installs NO hook and NEVER
// calls back into JS. This is the plain summary path, distinct from feature 24 (direct Quick Catch
// completion) and feature 3 (the presentation bridge / Dismiss intercept); feature 36 does not add
// or alter any hook and does NOT change feature 24 or feature 3.
//
// SCOPE / NOT: the host keeps everything around this one managed step -- candidate/root capture, the
// pendingDetails bookkeeping, wrapping the call in presentationBridge.begin/end, and the follow-on
// completeDirectPresentation + canvas/discovery -- all stay in JS this increment. No details-click
// migration this batch (feature's details branch stays inline). When the native core is not selected
// (create() returns null) the host keeps the inline JS summary branch untouched. A runtime disable /
// missing feature / refusal NEVER invokes Dismiss; a native invocation error NEVER retries and NEVER
// falls back to the legacy JS invoke -- the host runs only ONE invocation path.
//
// Unlike feature 24 there is NO dismissEnabled gate on the native side: an auto-transfer-only session
// still needs the ordinary summary dismiss. The host's own presentation-bridge wrapping remains
// gated in JS; this module only owns the identity + invoke + postcondition.
//
// Identity: the caller lends GC handles for the synchronous call. Native duplicates each freshly
// resolved target into an OWN temporary root and refreshes it after managed calls/allocations.
// No managed reference survives the call. Native proves:
//   * gui != null and object_get_class(gui) == EncounterSummaryGuiController (SGUI);
//   * gui is alive: UnityEngine.Object.op_Implicit(gui) returns a boxed System.Boolean (exact BOOL class),
//     unboxed to exactly 1 (0 -> not alive -> invalid; any other byte -> fail closed);
//   * proto != null and class == CatchPokemonOutProto (RESP), response == proto (same pointer),
//     proto status == CatchSuccess literal, and the 8-byte CapturedPokemonId equals the expected 8
//     bytes (which must be nonzero). The id is compared as 8 raw bytes, NEVER numerically converted.
//     This proto/response/id check holds even when state is absent;
//   * state is OPTIONAL: when non-null it must be class EncounterSummaryState (SSTATE) with dscx == gui
//     and dscy == proto;
//   * gui currentState == STATE_OPEN;
//   * onDismissed (read on gui) is non-null and equals the rooted captured dismissal callback, which
//     must itself be non-null.
// The metadata (classes, FieldInfo*, MethodInfo*, exports) is exactly what the host read from the
// live p/classes/fields/methods; native never invents a layout.
//
// Double-invoke / reentrancy: a monotonic request token is issued by acquire() and CONSUMED before
// Dismiss inside complete(); complete() rejects a reused or older token and refuses while busy, so a
// re-entrant Dismiss (or a retry) can never invoke twice. The owner thread is latched; update() arms
// an enabled+lease+epoch gate (lease clamped to 2500 ms) and a foreign thread can only revoke. stop()
// revokes immediately from any thread (bumps the generation); complete() re-checks the generation
// right before the invoke. No mutex is held across runtime_invoke or a field read (no deadlock during
// the managed Dismiss). A thrown managed exception from Dismiss is reported as a failure (invoked is
// still reported true); the host never retries Dismiss. The postcondition (after / gone) is read
// after the invoke regardless of whether Dismiss threw, mirroring the JS try/catch.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "SummaryCompletion native ABI requires LP64");

enum PgrSummaryCompletionInfo {
  PGR_SC_FEATURE_ID      = 36,
  PGR_SC_API_VERSION     = 1,
  PGR_SC_CONFIG_VERSION  = 1,
  PGR_SC_REQUEST_VERSION = 1,
  PGR_SC_OUTPUT_VERSION  = 1,
  PGR_SC_SAMPLE_VERSION  = 1,
  PGR_SC_MAX_LEASE_MS    = 2500   // residual foreground lease ceiling (JS allowedUntil gate)
};

// validate() returns 1 VALID / 0 INVALID / negative error. complete() returns 1 DONE (invoked, no
// throw) / 0 REFUSED (gate/token/validation, NOT invoked) / negative error. acquire() returns
// PGR_SC_OK with *out_token set to a nonzero token, or 0 when refused. Every other function returns
// PGR_SC_OK or a negative error.
enum PgrSummaryCompletionStatus {
  PGR_SC_DONE              = 1,   // complete(): Dismiss invoked, no throw / validate(): valid
  PGR_SC_OK                = 0,   // generic success / complete(): refused / validate(): invalid
  PGR_SC_ERR_NULL_ARG      = -1,
  PGR_SC_ERR_BAD_VERSION   = -2,
  PGR_SC_ERR_BAD_SIZE      = -3,
  PGR_SC_ERR_MISSING_PTR   = -4,
  PGR_SC_ERR_NOT_INSTALLED = -6,
  PGR_SC_ERR_RECONFIG      = -9,
  PGR_SC_ERR_CLOCK_FAILURE = -10,
  PGR_SC_ERR_MANAGED       = -11  // Dismiss threw; invoked stays true and host never retries it
};

// Fixed LP64 config (168 bytes): 8-byte header + 19 validated il2cpp pointers + 2 int32. Every pointer
// is an already-validated export / Il2CppClass* / FieldInfo* / MethodInfo* packed by the host off
// p.module / classes / fields / methods; native re-checks the header and rejects any null. No hook
// address -- feature 36 installs no hook.
struct PgrSummaryCompletionConfig {
  uint32_t version;  // == PGR_SC_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrSummaryCompletionConfig)

  // il2cpp runtime exports.
  void *object_get_class;  // void* (void* obj)
  void *field_get_value;   // void  (void* obj, FieldInfo*, void* out)
  void *runtime_invoke;    // void* (MethodInfo*, void* obj, void** params, void** exc)
  void *object_unbox;      // void* (void* boxed) -- unbox the boxed System.Boolean from op_Implicit

  // Exact Il2CppClass* identities.
  void *class_gui;       // EncounterSummaryGuiController (SGUI)
  void *class_state;     // EncounterSummaryState (SSTATE) -- optional request field
  void *class_response;  // Holoholo.Rpc CatchPokemonOutProto (RESP; also the proto class)
  void *class_bool;      // System.Boolean (BOOL) -- exact class of the boxed op_Implicit result

  // FieldInfo* (read through native-owned temporary roots).
  void *f_dscx;          // SSTATE dscx -> gui
  void *f_dscy;          // SSTATE dscy -> proto
  void *f_status;        // RESP Status (Int32 enum)
  void *f_captured_id;   // RESP CapturedPokemonId (UInt64, read as 8 raw bytes)
  void *f_current_state; // GuiController currentState (Int32)
  void *f_on_dismissed;  // SGUI OnDismissed (System.Action)

  // MethodInfo* (invoked via runtime_invoke).
  void *m_alive;         // static UnityEngine.Object.op_Implicit(Object) -> boxed System.Boolean
  void *m_dismiss;       // GuiController.Dismiss() -- the ordinary inherited dismissal

  void *gc_new;          // uintptr_t (void*, int pinned)
  void *gc_get;          // void* (uintptr_t)
  void *gc_free;         // void (uintptr_t)

  // Validated enum literal + state constant.
  int32_t success_literal; // p.success (CatchPokemonOutProto status CatchSuccess)
  int32_t state_open;      // STATE_OPEN (GuiController currentState == 2)
};

static_assert(sizeof(PgrSummaryCompletionConfig) == 168,
              "PgrSummaryCompletionConfig must remain 168 bytes on LP64");

// Fixed LP64 request (72 bytes). The last five void* values are opaque borrowed GC HANDLE IDs, not
// managed object addresses. Caller retains them for the call; state is OPTIONAL (null when not
// captured). expected_key is the 8-byte CapturedPokemonId written verbatim (little-endian decimal
// bytes), compared byte-for-byte and never numerically converted.
struct PgrSummaryCompletionRequest {
  uint32_t version;         // == PGR_SC_REQUEST_VERSION
  uint32_t size;            // == sizeof(PgrSummaryCompletionRequest)
  uint8_t  expected_key[8]; // proven caught id (nonzero); 8 raw bytes, never numerically converted
  uint64_t epoch;           // caller's current epoch; must equal the armed epoch at the gate
  uint64_t tid;             // proven owner tid (must equal the current OS tid)
  void *gui;                // borrowed handle -> EncounterSummaryGuiController
  void *state;              // borrowed handle -> EncounterSummaryState (OPTIONAL null)
  void *proto;              // borrowed handle -> authoritative CatchPokemonOutProto
  void *response;           // borrowed handle -> same object as proto (handles may differ)
  void *dismissed;          // borrowed handle -> captured System.Action
};

static_assert(sizeof(PgrSummaryCompletionRequest) == 72,
              "PgrSummaryCompletionRequest must remain 72 bytes on LP64");

// complete() output flags (in PgrSummaryCompletionOutput.flags).
enum PgrSummaryCompletionFlags {
  PGR_SC_FLAG_INVOKED      = 1u << 0, // Dismiss was invoked (committed past the final gate)
  PGR_SC_FLAG_GONE         = 1u << 1, // the GUI is gone after the invoke (class mismatch or not alive)
  PGR_SC_FLAG_AFTER_KNOWN  = 1u << 2, // 'after' holds a read currentState
  PGR_SC_FLAG_BEFORE_KNOWN = 1u << 3  // 'before' holds the validated pre-invoke currentState
};

// Fixed LP64 output (24 bytes). complete() always fills this (even on refusal / error) so the host
// learns invoked/before/after/gone and can never double-invoke. 'status' mirrors the return value.
struct PgrSummaryCompletionOutput {
  uint32_t version; // == PGR_SC_OUTPUT_VERSION
  uint32_t size;    // == sizeof(PgrSummaryCompletionOutput)
  uint32_t flags;   // PgrSummaryCompletionFlags bits
  int32_t  status;  // mirrors the complete() return code
  int32_t  before;  // pre-invoke currentState (valid iff PGR_SC_FLAG_BEFORE_KNOWN)
  int32_t  after;   // post-invoke currentState (valid iff PGR_SC_FLAG_AFTER_KNOWN)
};

static_assert(sizeof(PgrSummaryCompletionOutput) == 24,
              "PgrSummaryCompletionOutput must remain 24 bytes on LP64");

// Fixed LP64 telemetry gauge (72 bytes). Pull-only; the native layer never calls JS.
struct PgrSummaryCompletionSample {
  uint32_t version;          // == PGR_SC_SAMPLE_VERSION
  uint32_t size;             // == sizeof(PgrSummaryCompletionSample)
  uint64_t validate_ok;      // bumps on each VALID validate()
  uint64_t validate_fail;    // bumps on each INVALID validate()
  uint64_t complete_ok;      // bumps on each DONE complete() (Dismiss invoked, no throw)
  uint64_t complete_refused; // bumps on each refused complete() (gate/token/validation)
  uint64_t complete_fault;   // bumps on each complete() whose Dismiss threw
  uint64_t last_token;       // last token consumed by a complete()
  uint64_t last_tid;         // owner tid of the last event
  uint32_t busy;             // 1 during validation/completion and temporary-root cleanup
  uint32_t fault;            // reserved; 0 in v1
};

static_assert(sizeof(PgrSummaryCompletionSample) == 72,
              "PgrSummaryCompletionSample must remain 72 bytes on LP64");

// Fixed LP64 table: 8-byte header + 7 function pointers = 64 bytes.
struct PgrSummaryCompletionApi {
  uint32_t version;  // == PGR_SC_API_VERSION
  uint32_t size;     // == sizeof(PgrSummaryCompletionApi)
  int (*install)(const PgrSummaryCompletionConfig *config);
  // Refresh the gate: enabled, a residual lease clamped to 2500 ms, and the caller's current epoch.
  // Carries proven_tid; once the owner is latched a foreign thread can only revoke (enabled -> 0).
  // Any thread.
  int (*update)(int enabled, uint64_t remaining_lease_ms, uint64_t epoch, uint64_t proven_tid);
  // Summary identity validation. Returns 1 valid / 0 invalid / negative error. Reads managed fields
  // and invokes op_Implicit (alive); owner thread only (0 off-owner). Does NOT gate on the lease,
  // consume a token, or invoke Dismiss.
  int (*validate)(const PgrSummaryCompletionRequest *request);
  // Issue one monotonic request token for a single completion attempt. *out_token is a nonzero token
  // when armed (gate live), on the owner thread, and not busy; otherwise 0. Owner thread only.
  int (*acquire)(uint64_t epoch, uint64_t proven_tid, uint64_t *out_token);
  // One owner-thread completion. Re-checks the gate + that token is the latest issued and unconsumed,
  // CONSUMES it and marks busy BEFORE anything, re-validates the identity, re-checks the generation,
  // then runtime_invokes Dismiss exactly once and reads the postcondition. Always fills *out. Returns
  // PGR_SC_DONE (invoked, no throw), PGR_SC_OK (refused: gate/token/busy/validation, NOT invoked), or
  // PGR_SC_ERR_MANAGED (Dismiss threw; invoked still reported). Never invokes
  // twice on retry or reentrancy. Owner thread only.
  int (*complete)(const PgrSummaryCompletionRequest *request, uint64_t token,
                  PgrSummaryCompletionOutput *out);
  // Immediate revoke from any thread (bumps the generation, disables the gate). A later update()
  // re-arms. No managed work; no deadlock with an in-flight Dismiss.
  int (*stop)(uint64_t proven_tid);
  int (*sample)(PgrSummaryCompletionSample *out_sample);
};

static_assert(sizeof(PgrSummaryCompletionApi) == 64,
              "PgrSummaryCompletionApi must remain a 64-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature
// under feature id 36 and answers only on an exact version match.
const void *pgrazor_summary_completion_api(uint32_t version);

#ifdef __cplusplus
}
#endif

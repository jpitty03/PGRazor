#pragma once
#include <stdint.h>

// Feature 24 (API 1). Native DIRECT Quick Catch completion: a focused C++ port of the two managed
// steps at the tail of diagnostics/quick-close.js completeDirectPresentation (~1120-1142) -- the
// direct-catch identity validation (directCatchValid, ~1106-1112) and the single
// Action.Invoke() that returns the game's supplied post-catch dismissal delegate (~1134). It owns
// NOTHING else: the ShowPokemonInfo source capture (catch-presentation-bridge.js, feature 3), the
// orchestration/flag plumbing, the ordinary-details fallback replay (replayDirectPresentation) and
// transferCatch all stay in JS this increment. This module installs NO hook and NEVER calls back
// into JS. Feature 3 remains the sole owner of the Dismiss intercept / suppression / details
// capture; feature 24 does not add or alter any hook.
//
// SCOPE / NOT: no full post-catch dismissal migration -- only (a) validate() a confirmed direct
// summary-return state, and (b) complete() it by invoking the captured dismissal Action exactly
// once. The host keeps its existing transfer-before-completion ordering: it runs transferCatch in
// JS and only THEN calls complete(); complete() re-validates the same state immediately before the
// invoke. When the native core is not selected (create() returns null), the host keeps the inline
// JS directCatchValid + Action.Invoke path untouched. A runtime disable / missing feature / refusal
// never invokes the callback; the host then replays the ordinary details screen (its safe fallback)
// only while the confirmed view is still active, and NEVER invokes both the native and the legacy
// callback after a failure.
//
// Identity (validate() and the re-check inside complete()): the caller passes RAW managed pointers
// that it keeps GC-rooted for the duration of the synchronous call (native roots nothing and stores
// no managed pointer across calls). Native resolves object_get_class on each and proves the EXACT
// classes (view=EncounterViewPokemonState, response=CatchPokemonOutProto, pokemon=PokemonProto,
// callback=System.Action), the active-view flag (== 1), the callback.m_target == view, the
// view.dsvw == response, the response status == the configured CatchSuccess literal, and 64-bit ID
// equality across the expected id, the response CapturedPokemonId and the Pokemon id_. The id is
// compared as a raw 64-bit value -- it is NEVER numerically converted.
//
// Double-invoke / reentrancy: a monotonic request token is issued by acquire() and CONSUMED before
// runtime_invoke inside complete(); complete() rejects a reused or older token and refuses while
// busy, so a callback that re-enters (or a retry) can never invoke twice. The owner thread is
// latched; update() arms an enabled+lease+epoch gate (lease clamped to 2500 ms) and a foreign thread
// can only revoke. stop() revokes immediately from any thread (bumps the generation); complete()
// re-checks the generation right before the invoke. No mutex is ever held across runtime_invoke or a
// field read (no deadlock during the managed callback). A thrown managed exception is reported back
// as a failure so the host runs its safe ordinary-details fallback.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "CatchCompletion native ABI requires LP64");

enum PgrCatchCompletionInfo {
  PGR_CC_FEATURE_ID      = 24,
  PGR_CC_API_VERSION     = 1,
  PGR_CC_CONFIG_VERSION  = 1,
  PGR_CC_REQUEST_VERSION = 1,
  PGR_CC_SAMPLE_VERSION  = 1,
  PGR_CC_MAX_LEASE_MS    = 2500   // residual foreground lease ceiling (JS allowedUntil gate)
};

// validate() returns 1 VALID / 0 INVALID / negative error. complete() returns 1 DONE (invoked) /
// 0 REFUSED (gate/token/validation, NOT invoked) / negative error. acquire() returns PGR_CC_OK with
// *out_token set to a nonzero token, or 0 when refused. Every other function returns PGR_CC_OK or a
// negative error.
enum PgrCatchCompletionStatus {
  PGR_CC_DONE              = 1,   // complete(): the callback was invoked / validate(): valid
  PGR_CC_OK                = 0,   // generic success / complete(): refused / validate(): invalid
  PGR_CC_ERR_NULL_ARG      = -1,
  PGR_CC_ERR_BAD_VERSION   = -2,
  PGR_CC_ERR_BAD_SIZE      = -3,
  PGR_CC_ERR_MISSING_PTR   = -4,
  PGR_CC_ERR_NOT_INSTALLED = -6,
  PGR_CC_ERR_RECONFIG      = -9,
  PGR_CC_ERR_CLOCK_FAILURE = -10,
  PGR_CC_ERR_MANAGED       = -11  // the dismissal Action threw -> host runs its safe fallback
};

// Fixed LP64 config (128 bytes): 8-byte header + 14 validated il2cpp pointers + 2 int32. Every
// pointer is an already-validated export / Il2CppClass* / FieldInfo* / MethodInfo* packed by the host
// straight off p.directPresentation / p.details / p.fields; the native layer re-checks the header and
// rejects any null. No hook address -- feature 24 installs no hook.
struct PgrCatchCompletionConfig {
  uint32_t version;  // == PGR_CC_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrCatchCompletionConfig)

  // il2cpp runtime exports.
  void *object_get_class;  // void* (void* obj)
  void *field_get_value;   // void  (void* obj, FieldInfo*, void* out)
  void *runtime_invoke;    // void* (MethodInfo*, void* obj, void** params, void** exc)

  // Exact Il2CppClass* identities.
  void *class_view;      // EncounterViewPokemonState (VIEW) -- also the delegate target
  void *class_response;  // Holoholo.Rpc CatchPokemonOutProto (RESP)
  void *class_pokemon;   // Holoholo.Rpc PokemonProto (PK)
  void *class_action;    // System.Action (ACTION)

  // FieldInfo* (read via field_get_value; no object is rooted by native).
  void *f_active;          // GameState.<IsActiveState> backing bool, read on the view
  void *f_delegate_target; // System.Delegate.m_target (on the callback) -> view
  void *f_view_response;   // view.dsvw -> response
  void *f_status;          // response status (Int32 enum)
  void *f_captured_id;     // response CapturedPokemonId (UInt64)
  void *f_pokemon_id;      // pokemon id_ (UInt64)

  // MethodInfo* (invoked via runtime_invoke).
  void *m_callback_invoke; // System.Action.Invoke() -- the captured dismissal delegate

  // Validated enum literal.
  int32_t success_literal; // p.success (CatchPokemonOutProto status CatchSuccess)
  int32_t reserved;        // 0
};

static_assert(sizeof(PgrCatchCompletionConfig) == 128,
              "PgrCatchCompletionConfig must remain 128 bytes on LP64");

// Fixed LP64 request (64 bytes). view/response/pokemon/callback are raw managed pointers the caller
// keeps GC-rooted for the synchronous call; native stores none of them. expected_id is the proven
// caught id (nonzero), compared as a raw 64-bit value against the response/pokemon ids.
struct PgrCatchCompletionRequest {
  uint32_t version;     // == PGR_CC_REQUEST_VERSION
  uint32_t size;        // == sizeof(PgrCatchCompletionRequest)
  uint64_t expected_id; // proven caught id (nonzero); raw 64-bit, never numerically converted
  uint64_t epoch;       // caller's current epoch; must equal the armed epoch at the gate
  uint64_t tid;         // proven owner tid (must equal the current OS tid)
  void *view;           // EncounterViewPokemonState
  void *response;       // CatchPokemonOutProto
  void *pokemon;        // PokemonProto
  void *callback;       // System.Action (dismissal delegate)
};

static_assert(sizeof(PgrCatchCompletionRequest) == 64,
              "PgrCatchCompletionRequest must remain 64 bytes on LP64");

// Fixed LP64 telemetry gauge (72 bytes). Pull-only; the native layer never calls JS.
struct PgrCatchCompletionSample {
  uint32_t version;          // == PGR_CC_SAMPLE_VERSION
  uint32_t size;             // == sizeof(PgrCatchCompletionSample)
  uint64_t validate_ok;      // bumps on each VALID validate()
  uint64_t validate_fail;    // bumps on each INVALID validate()
  uint64_t complete_ok;      // bumps on each DONE complete() (callback invoked, no throw)
  uint64_t complete_refused; // bumps on each refused complete() (gate/token/validation)
  uint64_t complete_fault;   // bumps on each complete() whose callback threw
  uint64_t last_token;       // last token consumed by a complete()
  uint64_t last_tid;         // owner tid of the last event
  uint32_t busy;             // 1 while a complete() invoke is in flight
  uint32_t fault;            // reserved; 0 in v1
};

static_assert(sizeof(PgrCatchCompletionSample) == 72,
              "PgrCatchCompletionSample must remain 72 bytes on LP64");

// Fixed LP64 table: 8-byte header + 7 function pointers = 64 bytes.
struct PgrCatchCompletionApi {
  uint32_t version;  // == PGR_CC_API_VERSION
  uint32_t size;     // == sizeof(PgrCatchCompletionApi)
  int (*install)(const PgrCatchCompletionConfig *config);
  // Refresh the gate: enabled, a residual lease clamped to 2500 ms, and the caller's current epoch.
  // Carries proven_tid; once the owner is latched a foreign thread can only revoke (enabled -> 0).
  // Any thread.
  int (*update)(int enabled, uint64_t remaining_lease_ms, uint64_t epoch, uint64_t proven_tid);
  // Validation-only (directCatchValid). Returns 1 valid / 0 invalid / negative error. Reads managed
  // fields; owner thread only (0 off-owner). Does NOT gate on the lease, consume a token, or invoke.
  int (*validate)(const PgrCatchCompletionRequest *request);
  // Issue one monotonic request token for a single completion attempt. *out_token is a nonzero token
  // when armed (gate live), on the owner thread, and not busy; otherwise 0. Any older/reused token is
  // then rejected by complete(). Owner thread only.
  int (*acquire)(uint64_t epoch, uint64_t proven_tid, uint64_t *out_token);
  // One owner-thread completion. Re-checks the gate + that token is the latest issued and unconsumed,
  // CONSUMES it and marks busy BEFORE anything, re-validates the identity, re-checks the generation,
  // then runtime_invokes the dismissal Action exactly once. Returns PGR_CC_DONE (invoked),
  // PGR_CC_OK (refused: gate/token/busy/validation, NOT invoked), or PGR_CC_ERR_MANAGED (the callback
  // threw -> host safe fallback). Never invokes twice on retry or reentrancy. Owner thread only.
  int (*complete)(const PgrCatchCompletionRequest *request, uint64_t token);
  // Immediate revoke from any thread (bumps the generation, disables the gate). A later update()
  // re-arms. No managed work; no deadlock with an in-flight callback.
  int (*stop)(uint64_t proven_tid);
  int (*sample)(PgrCatchCompletionSample *out_sample);
};

static_assert(sizeof(PgrCatchCompletionApi) == 64,
              "PgrCatchCompletionApi must remain a 64-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature
// under feature id 24 and answers only on an exact version match.
const void *pgrazor_catch_completion_api(uint32_t version);

#ifdef __cplusplus
}
#endif

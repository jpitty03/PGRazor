#pragma once
#include <stdint.h>

// Feature 37 (API 1). Native POST-CATCH DETAILS completion: the C++ port of the confirmed
// details-return branch of diagnostics/quick-close.js cleanupTick -- the post-catch details identity
// validation, the single inherited details "click" invoke, and the postcondition read (currentState
// advanced off STATE_OPEN, or the GUI is gone). It installs NO hook and NEVER calls back into JS.
//
// Relationship to feature 36 (summary_completion): the lifecycle, gate/token/lease model, owner
// latching, and native-owned GC rooting are identical to the CURRENT feature 36 -- this module reuses
// that machinery verbatim and does NOT change feature 36. The differences are entirely in identity:
// details validation proves the post-catch details scene (not a summary, and crucially NOT an ordinary
// inventory details view), including a one-element backing list whose single item is the caught
// Pokemon. Like feature 36 there is NO dismissEnabled gate: an auto-transfer-only session still needs
// the details completion, so this path works independently of dismissEnabled.
//
// SCOPE / NOT: the host keeps candidate/root capture, pendingDetails bookkeeping, the presentation-
// bridge wrapping, and the follow-on canvas/discovery in JS. No new hook, no canvas/discovery
// migration this batch. When the native core is not selected the host keeps its inline JS details
// branch. A disable / missing feature / refusal NEVER invokes click; a managed error NEVER retries and
// NEVER falls back to the legacy JS invoke -- one invocation path only.
//
// Identity (READONLY; validate() never gates on STATE_OPEN or opener depth, so it may run while the
// details canvas is still OPENING, for suppression). The caller lends GC handles for the synchronous
// call; native duplicates each freshly resolved target into an OWN temporary root and re-resolves it
// after every managed call / allocation. No managed reference survives the call. Native proves, with
// all six objects at their EXACT classes (gui/view/response/data/pokemon/dismissed):
//   * gui alive via static UnityEngine.Object.op_Implicit -> boxed System.Boolean (exact BOOL, byte 1);
//   * view.response == response; response.status == CatchSuccess; response.caughtId AND pokemon.id both
//     equal the expected nonzero 8-byte caught id (compared as raw bytes, never numerically);
//   * dismissed.delegateTarget == view; gui.data == data; gui.callback == dismissed; data.onDismissed ==
//     dismissed; data.post byte == 1 and data.index == 0 (rejects ordinary inventory details);
//   * data.list is the exact list class; list.count() is a boxed System.Int32 (exact INT) unboxing to
//     exactly 1; list.item(0) == the rooted pokemon.
// The count()/item() invokes may relocate the heap, so the list is rooted, the boxed count and the
// item result are rooted, and the scalar identities are RE-CHECKED after the managed calls with the
// list still backing data. complete() additionally requires currentState == STATE_OPEN.
//
// Double-invoke / reentrancy: identical to feature 36. acquire() issues a monotonic token CONSUMED
// before the click inside complete(); a reused/older token or a busy window is refused. The owner
// thread is latched; update() arms enabled+lease(<=2500 ms)+epoch, a foreign thread can only revoke;
// stop() revokes from any thread (bumps the generation) and complete() re-checks it right before the
// invoke. No mutex is held across a managed call. A thrown managed exception is reported as a failure
// (invoked stays true); the postcondition is read after the invoke regardless of a throw.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "DetailsCompletion native ABI requires LP64");

enum PgrDetailsCompletionInfo {
  PGR_DC_FEATURE_ID      = 37,
  PGR_DC_API_VERSION     = 1,
  PGR_DC_CONFIG_VERSION  = 1,
  PGR_DC_REQUEST_VERSION = 1,
  PGR_DC_OUTPUT_VERSION  = 1,
  PGR_DC_SAMPLE_VERSION  = 1,
  PGR_DC_MAX_LEASE_MS    = 2500   // residual foreground lease ceiling (JS allowedUntil gate)
};

// validate() returns 1 VALID / 0 INVALID / negative error. complete() returns 1 DONE (invoked, no
// throw) / 0 REFUSED (gate/token/validation, NOT invoked) / negative error. acquire() returns PGR_DC_OK
// with *out_token nonzero when armed, else 0. Every other function returns PGR_DC_OK or negative.
enum PgrDetailsCompletionStatus {
  PGR_DC_DONE              = 1,   // complete(): click invoked, no throw / validate(): valid
  PGR_DC_OK                = 0,   // generic success / complete(): refused / validate(): invalid
  PGR_DC_ERR_NULL_ARG      = -1,
  PGR_DC_ERR_BAD_VERSION   = -2,
  PGR_DC_ERR_BAD_SIZE      = -3,
  PGR_DC_ERR_MISSING_PTR   = -4,
  PGR_DC_ERR_NOT_INSTALLED = -6,
  PGR_DC_ERR_RECONFIG      = -9,
  PGR_DC_ERR_CLOCK_FAILURE = -10,
  PGR_DC_ERR_MANAGED       = -11  // click threw; invoked stays true and the host never retries it
};

// Fixed LP64 config (272 bytes): 8-byte header + 32 validated il2cpp pointers + 2 int32. Every pointer
// is an already-validated export / Il2CppClass* / FieldInfo* / MethodInfo* packed by the host off
// p.module / classes / fields / methods; native re-checks the header and rejects any null. No hook
// address -- feature 37 installs no hook. Pointer order is the shared contract's canonical layout.
struct PgrDetailsCompletionConfig {
  uint32_t version;  // == PGR_DC_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrDetailsCompletionConfig)

  // il2cpp runtime exports (slots 0-6).
  void *object_get_class;  // void* (void* obj)
  void *field_get_value;   // void  (void* obj, FieldInfo*, void* out)
  void *runtime_invoke;    // void* (MethodInfo*, void* obj, void** params, void** exc)
  void *object_unbox;      // void* (void* boxed) -- unbox the boxed Boolean / Int32
  void *gc_new;            // uintptr_t (void*, int pinned)
  void *gc_get;            // void* (uintptr_t)
  void *gc_free;           // void (uintptr_t)

  // Exact Il2CppClass* identities (slots 7-15).
  void *class_gui;       // DetailsGuiController (DGUI)
  void *class_view;      // details VIEW
  void *class_response;  // CatchPokemonOutProto (RESP)
  void *class_data;      // details DATA
  void *class_pokemon;   // Pokemon (PK)
  void *class_action;    // System.Action (ACTION) -- the captured dismissal callback
  void *class_bool;      // System.Boolean (BOOL) -- exact class of op_Implicit's boxed result
  void *class_int;       // System.Int32 (INT) -- exact class of list.count()'s boxed result
  void *class_list;      // p.details.listClass -- exact class of data.list

  // MethodInfo* invoked via runtime_invoke (slots 16-19).
  void *m_alive;         // static UnityEngine.Object.op_Implicit(Object) -> boxed System.Boolean
  void *m_click;         // details click (the single inherited completion invoke on gui)
  void *m_count;         // list.count() -> boxed System.Int32
  void *m_item;          // list.item(int index) -> Pokemon

  // FieldInfo* read through native-owned temporary roots (slots 20-31).
  void *f_current_state; // DGUI currentState (Int32)
  void *f_status;        // RESP Status (Int32 enum)
  void *f_caught_id;     // RESP CapturedPokemonId (UInt64, read as 8 raw bytes)
  void *f_view_response; // VIEW response -> response
  void *f_pokemon_id;    // PK id (UInt64, read as 8 raw bytes)
  void *f_delegate_target; // ACTION delegateTarget -> view
  void *f_gui_data;      // DGUI data -> data
  void *f_gui_callback;  // DGUI callback -> dismissed
  void *f_data_callback; // DATA onDismissed -> dismissed
  void *f_post;          // DATA post (Boolean byte, must be exactly 1)
  void *f_index;         // DATA index (Int32, must be 0)
  void *f_list;          // DATA list -> the backing list object

  // Validated enum literal + state constant.
  int32_t success_literal; // p.success (CatchPokemonOutProto status CatchSuccess)
  int32_t state_open;      // STATE_OPEN (DGUI currentState open value)
};

static_assert(sizeof(PgrDetailsCompletionConfig) == 272,
              "PgrDetailsCompletionConfig must remain 272 bytes on LP64");

// Fixed LP64 request (80 bytes). The last six void* values are opaque borrowed GC HANDLE IDs, not
// managed object addresses; the caller retains them for the call. ALL six are required (unlike
// feature 36's optional state). expected_key is the 8-byte caught id written verbatim, compared
// byte-for-byte and never numerically converted.
struct PgrDetailsCompletionRequest {
  uint32_t version;         // == PGR_DC_REQUEST_VERSION
  uint32_t size;            // == sizeof(PgrDetailsCompletionRequest)
  uint8_t  expected_key[8]; // proven caught id (nonzero); 8 raw bytes, never numerically converted
  uint64_t epoch;           // caller's current epoch; must equal the armed epoch at the gate
  uint64_t tid;             // proven owner tid (must equal the current OS tid)
  void *gui;                // borrowed handle -> DetailsGuiController
  void *view;               // borrowed handle -> details VIEW
  void *response;           // borrowed handle -> authoritative CatchPokemonOutProto
  void *data;               // borrowed handle -> details DATA
  void *pokemon;            // borrowed handle -> the caught Pokemon (list.item(0))
  void *dismissed;          // borrowed handle -> captured System.Action
};

static_assert(sizeof(PgrDetailsCompletionRequest) == 80,
              "PgrDetailsCompletionRequest must remain 80 bytes on LP64");

// complete() output flags (in PgrDetailsCompletionOutput.flags).
enum PgrDetailsCompletionFlags {
  PGR_DC_FLAG_INVOKED      = 1u << 0, // click was invoked (committed past the final gate)
  PGR_DC_FLAG_GONE         = 1u << 1, // the GUI is definitively gone (post-invoke alive probe == false)
  PGR_DC_FLAG_AFTER_KNOWN  = 1u << 2, // 'after' holds a read currentState
  PGR_DC_FLAG_BEFORE_KNOWN = 1u << 3  // 'before' holds the validated pre-invoke currentState
};

// Fixed LP64 output (24 bytes). complete() always fills this (even on refusal / error) so the host
// learns invoked/before/after/gone and can never double-invoke. 'status' mirrors the return value.
struct PgrDetailsCompletionOutput {
  uint32_t version; // == PGR_DC_OUTPUT_VERSION
  uint32_t size;    // == sizeof(PgrDetailsCompletionOutput)
  uint32_t flags;   // PgrDetailsCompletionFlags bits
  int32_t  status;  // mirrors the complete() return code
  int32_t  before;  // pre-invoke currentState (valid iff PGR_DC_FLAG_BEFORE_KNOWN)
  int32_t  after;   // post-invoke currentState (valid iff PGR_DC_FLAG_AFTER_KNOWN)
};

static_assert(sizeof(PgrDetailsCompletionOutput) == 24,
              "PgrDetailsCompletionOutput must remain 24 bytes on LP64");

// Fixed LP64 telemetry gauge (72 bytes). Pull-only; the native layer never calls JS.
struct PgrDetailsCompletionSample {
  uint32_t version;          // == PGR_DC_SAMPLE_VERSION
  uint32_t size;             // == sizeof(PgrDetailsCompletionSample)
  uint64_t validate_ok;      // bumps on each VALID validate()
  uint64_t validate_fail;    // bumps on each INVALID validate()
  uint64_t complete_ok;      // bumps on each DONE complete() (click invoked, no throw)
  uint64_t complete_refused; // bumps on each refused complete() (gate/token/validation)
  uint64_t complete_fault;   // bumps on each complete() whose click threw
  uint64_t last_token;       // last token consumed by a complete()
  uint64_t last_tid;         // owner tid of the last event
  uint32_t busy;             // 1 during validation/completion and temporary-root cleanup
  uint32_t fault;            // reserved; 0 in v1
};

static_assert(sizeof(PgrDetailsCompletionSample) == 72,
              "PgrDetailsCompletionSample must remain 72 bytes on LP64");

// Fixed LP64 table: 8-byte header + 7 function pointers = 64 bytes. Same shape as feature 36.
struct PgrDetailsCompletionApi {
  uint32_t version;  // == PGR_DC_API_VERSION
  uint32_t size;     // == sizeof(PgrDetailsCompletionApi)
  int (*install)(const PgrDetailsCompletionConfig *config);
  // Refresh the gate: enabled, a residual lease clamped to 2500 ms, the caller's current epoch and
  // proven_tid. Once the owner is latched a foreign thread can only revoke (enabled -> 0). Any thread.
  int (*update)(int enabled, uint64_t remaining_lease_ms, uint64_t epoch, uint64_t proven_tid);
  // Details identity validation. 1 valid / 0 invalid / negative error. Reads managed fields and invokes
  // op_Implicit/count/item; owner thread only (0 off-owner). Does NOT gate on the lease, consume a
  // token, require STATE_OPEN, or invoke click -- so it is usable during a still-opening canvas.
  int (*validate)(const PgrDetailsCompletionRequest *request);
  // Issue one monotonic request token for a single completion attempt. *out_token is nonzero when armed
  // (gate live), on the owner thread, and not busy; otherwise 0. Owner thread only.
  int (*acquire)(uint64_t epoch, uint64_t proven_tid, uint64_t *out_token);
  // One owner-thread completion. Re-checks the gate + that token is the latest issued and unconsumed,
  // CONSUMES it and marks busy BEFORE anything, re-validates the identity (plus STATE_OPEN), re-checks
  // the generation, then runtime_invokes the details click exactly once and reads the postcondition.
  // Always fills *out. Returns PGR_DC_DONE / PGR_DC_OK (refused) / PGR_DC_ERR_MANAGED (click threw).
  // Never invokes twice on retry or reentrancy. Owner thread only.
  int (*complete)(const PgrDetailsCompletionRequest *request, uint64_t token,
                  PgrDetailsCompletionOutput *out);
  // Immediate revoke from any thread (bumps the generation, disables the gate). No managed work.
  int (*stop)(uint64_t proven_tid);
  int (*sample)(PgrDetailsCompletionSample *out_sample);
};

static_assert(sizeof(PgrDetailsCompletionApi) == 64,
              "PgrDetailsCompletionApi must remain a 64-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature
// under feature id 37 and answers only on an exact version match.
const void *pgrazor_details_completion_api(uint32_t version);

#ifdef __cplusplus
}
#endif

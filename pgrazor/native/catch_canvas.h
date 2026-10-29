#pragma once
#include <stdint.h>

// Feature 39 (API 1). Native CATCH CANVAS visibility control: the C++ port of the post-catch
// "no-flash" canvas machinery of diagnostics/quick-close.js (catchCanvasAllowed, setCatchCanvas,
// restoreCatchCanvas, hideOpeningDetails, hideDismissedCatch, the cleanupTick restore sweep and the
// set_CanvasGroupVisible interceptor). This module owns BOTH the canvas hide/restore operations AND
// the setter visibility hook; the host keeps all catch identity, candidate/record orchestration and
// transfer/dismiss ACTIONS -- this module performs NO catch/transfer/dismiss and never calls close().
//
// Hook: the ONE validated p.catchCanvas.address (GuiController.set_CanvasGroupVisible), original
// signature void(void *self, bool visible, void *MethodInfo). Installed through hook_engine.h / Dobby
// (no Gum on the selected native path). The replacement masks a true->false ONLY for a GUI that is a
// currently OWNED opening-details root, on the current actual owner thread, under a live policy with a
// matching epoch; every unrelated / foreign-thread / disabled / expired / off call passes the original
// bool through unchanged and always chains the original exactly once with the same MethodInfo. The hook
// NEVER invokes a managed method or JS and NEVER arms/latches the owner. The coordinator disables the
// legacy JS canvasInterceptor whenever this pilot is selected, even if install() fails.
//
// Owner / policy model (identical shape to feature 36/37): update() arms enabled + a residual lease
// (<=2500 ms, CLOCK_BOOTTIME) + epoch on the verified owner thread; a foreign thread or disable can
// only REVOKE; stop() revokes from any thread. The owner tid is latched once from a genuine on-thread
// call. Native-owned GC roots (one per hidden opening-details GUI, bounded to 8 slots, each with its
// epoch and a monotonic nonzero token <= 2^53-1) are only ever freed or restored on the owner thread
// via release()/tick(); a foreign thread never frees a root. No managed call or GC op runs under a lock.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "CatchCanvas native ABI requires LP64");

enum PgrCatchCanvasInfo {
  PGR_CANVAS_FEATURE_ID     = 39,
  PGR_CANVAS_API_VERSION    = 1,
  PGR_CANVAS_CONFIG_VERSION = 1,
  PGR_CANVAS_SAMPLE_VERSION = 1,
  PGR_CANVAS_MAX_LEASE_MS   = 2500, // residual foreground lease ceiling (JS allowedUntil gate)
  PGR_CANVAS_MAX_SLOTS      = 8     // bounded owned opening-details roots
};

// hide_opening()/hide_dismissed() return 1 HIDDEN (managed set-false invoked, no throw) / 0 OK (refused
// by gate/identity/overflow, nothing invoked) / negative error. Every other function returns
// PGR_CANVAS_OK or a negative error. (Prefix PGR_CANVAS_; PGR_CC_ already belongs to feature 24.)
enum PgrCatchCanvasStatus {
  PGR_CANVAS_HIDDEN            = 1,  // managed set-false invoked without a throw
  PGR_CANVAS_OK               = 0,  // generic success / refused
  PGR_CANVAS_ERR_NULL_ARG     = -1,
  PGR_CANVAS_ERR_BAD_VERSION  = -2,
  PGR_CANVAS_ERR_BAD_SIZE     = -3,
  PGR_CANVAS_ERR_MISSING_PTR  = -4,
  PGR_CANVAS_ERR_NOT_INSTALLED = -6,
  PGR_CANVAS_ERR_RECONFIG     = -9,
  PGR_CANVAS_ERR_CLOCK_FAILURE = -10,
  PGR_CANVAS_ERR_HOOK_FAILED  = -11,
  PGR_CANVAS_ERR_OWNER = -12, // release refused; caller retains token
  PGR_CANVAS_ERR_BUSY = -13 // release deferred; caller retains token
};

// Fixed LP64 config (128 bytes): 8-byte header + 14 validated il2cpp pointers + int32 state_open +
// reserved. Every pointer is an already-validated export / Il2CppClass* / FieldInfo* / MethodInfo* /
// hook address packed by the host off p.module / classes / fields / methods / p.catchCanvas; native
// re-checks the header and rejects any null. Pointer order is the shared contract's canonical layout.
struct PgrCatchCanvasConfig {
  uint32_t version;  // == PGR_CANVAS_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrCatchCanvasConfig)

  // il2cpp runtime exports (slots 0-6).
  void *object_get_class;  // 0: void* (void* obj)
  void *field_get_value;   // 1: void  (void* obj, FieldInfo*, void* out)
  void *runtime_invoke;    // 2: void* (MethodInfo*, void* obj, void** params, void** exc)
  void *object_unbox;      // 3: void* (void* boxed) -- unbox the boxed System.Boolean
  void *gc_new;            // 4: uintptr_t (void*, int pinned)
  void *gc_get;            // 5: void* (uintptr_t)
  void *gc_free;           // 6: void (uintptr_t)

  // Exact Il2CppClass* identities (slots 7-9).
  void *class_summary;     // 7: SummaryGuiController (SGUI)
  void *class_details;     // 8: DetailsGuiController (DGUI) -- the only class a hide_opening slot holds
  void *class_bool;        // 9: System.Boolean (BOOL) -- exact class of op_Implicit's boxed result

  // FieldInfo* / MethodInfo* / hook address (slots 10-13).
  void *f_current_state;   // 10: GuiController currentState (Int32)
  void *m_alive;           // 11: static UnityEngine.Object.op_Implicit(Object) -> boxed System.Boolean
  void *m_canvas;          // 12: p.catchCanvas.method -- set_CanvasGroupVisible(bool) via runtime_invoke
  void *canvas_address;    // 13: p.catchCanvas.address -- the compiled setter patched by the hook

  int32_t state_open;      // STATE_OPEN (GuiController currentState open value)
  uint32_t reserved0;      // reserved; 0 in v1
};

static_assert(sizeof(PgrCatchCanvasConfig) == 128,
              "PgrCatchCanvasConfig must remain 128 bytes on LP64");

// Fixed LP64 telemetry gauge (40 bytes). Pull-only; the native layer never calls JS. 'owned' is the
// current number of live hidden opening-details roots; 'fault' latches only on a real install/runtime
// clock failure (never on an identity refusal); the counters are monotonic.
struct PgrCatchCanvasSample {
  uint32_t version;       // == PGR_CANVAS_SAMPLE_VERSION  (@0)
  uint32_t size;          // == sizeof(PgrCatchCanvasSample) (@4)
  uint32_t owned;         // live owned opening-details roots (@8)
  uint32_t fault;         // install/clock fault latch (@12)
  uint64_t masked_count;  // hook true->false masks (@16)
  uint64_t hide_count;    // managed set-false invocations (@24)
  uint64_t restore_count; // managed set-true restorations (@32)
};

static_assert(sizeof(PgrCatchCanvasSample) == 40,
              "PgrCatchCanvasSample must remain 40 bytes on LP64");

// Fixed LP64 table: 8-byte header + 8 function pointers = 72 bytes. Functions in the canonical order.
struct PgrCatchCanvasApi {
  uint32_t version;  // == PGR_CANVAS_API_VERSION
  uint32_t size;     // == sizeof(PgrCatchCanvasApi)
  // Install the config and the single set_CanvasGroupVisible hook. Immutable + idempotent; a partial
  // hook failure faults closed and is never retried; the original is published atomically.
  int (*install)(const PgrCatchCanvasConfig *config);
  // Refresh the gate: enabled, a residual lease clamped to 2500 ms, the caller's current epoch and
  // proven_tid. Arming requires the verified owner thread; a foreign thread/disable only revokes. Any
  // thread. Never frees or restores a root.
  int (*update)(int enabled, uint64_t remaining_lease_ms, uint64_t epoch, uint64_t proven_tid);
  // Hide a confirmed, still-OPENING post-catch details canvas. Owner thread only. Duplicates the
  // borrowed GUI into an OWN root, proves exact DGUI + Unity alive + a live gate (NO STATE_OPEN gate),
  // inserts the owned slot BEFORE the managed set-false (so the hook can keep it hidden), and returns a
  // nonzero token on success. A same-GUI/current-epoch request reuses its token without a redundant set.
  // Returns 1 HIDDEN / 0 refused / negative error; *out_token is nonzero only on 1.
  int (*hide_opening)(void *gui, uint64_t epoch, uint64_t tid, uint64_t *out_token);
  // Hide an already-dismissed catch canvas (summary OR details) once -- no persistent slot, no close().
  // Owner thread only; proves SGUI/DGUI + Unity alive + currentState == expected_after (!= STATE_OPEN,
  // re-read after the alive probe) + a live gate, then a single managed set-false. 1 / 0 / negative.
  int (*hide_dismissed)(void *gui, uint64_t epoch, uint64_t tid, int32_t expected_after);
  // Release one owned slot by token (owner thread only; ignores the enable/lease state). Removes the
  // slot BEFORE restoring so the hook cannot re-hide, restores true ONLY when the GUI is still exact
  // DGUI + alive + currentState == STATE_OPEN, and ALWAYS frees the own root. Idempotent for 0/unknown.
  int (*release)(uint64_t token, uint64_t tid);
  // Owner-thread maintenance sweep: prune/restore every slot whose epoch is stale or whose gate is
  // off/expired/stopped, using the same restore-then-free cleanup as release. Always runs (even after
  // stop()); a foreign thread is a no-op. Bounded, non-reentrant.
  int (*tick)(uint64_t tid);
  // Immediate revoke from any thread (disables the gate, bumps the generation). No managed work and the
  // original is NOT detached: the hook stays installed and passes through safely.
  int (*stop)();
  int (*sample)(PgrCatchCanvasSample *out);
};

static_assert(sizeof(PgrCatchCanvasApi) == 72,
              "PgrCatchCanvasApi must remain a 72-byte version-1 table");

// API2 is a separate immutable native observer registration table; API1 is unchanged.
// filter MUST be a pure, nonallocating predicate (no managed invocation/GC). on_enter
// runs BEFORE the original setter, with the receiver rooted by39; it may allocate.
// The original receives the refreshed receiver and preserves39 masking. Both callbacks
// must remain valid for process lifetime. No locks are held across either callback.
struct PgrCatchCanvasObserver {
 uint32_t version,size;
 int (*filter)(void *context,void *receiver,uint64_t tid);
 void (*on_enter)(void *context,void *receiver,uint64_t tid);
 void *context;
};
struct PgrCatchCanvasApi2 {
 uint32_t version,size;
 int (*subscribe)(const PgrCatchCanvasObserver *);
};
static_assert(sizeof(PgrCatchCanvasObserver)==32,"observer ABI");
static_assert(sizeof(PgrCatchCanvasApi2)==16,"observer table ABI");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature
// under feature id 39 and answers only on an exact version match.
const void *pgrazor_catch_canvas_api(uint32_t version);

#ifdef __cplusplus
}
#endif

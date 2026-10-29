#pragma once
#include <stddef.h>
#include <stdint.h>

// Feature 46 (API 1). Native ROUTES RESULT-PRESENTATION OBSERVERS: the C++ port of the parts of
// runtime/features/routes.js that features 43/44/45 deliberately left in JS -- the reward/rating-GUI
// Initialize observers and the bjan completion-response observer, the pendingUi queue and its
// GuiController.Dismiss result dismissal, the completing/completion/rewardAt result state, the
// route-tracking call-depth table, the nested location-enabled override decision and the waypoint
// throttle + diagnostic events. See .artifacts/native-migration/routes48-contract.txt for the full
// division of labour. This module installs THREE il2cpp hooks through the shared hook engine and OWNS
// all of the above state; it NEVER calls back into JS. feature 43 (actions), 44 (discovery) and 45
// (service capture + dica) are untouched and NONE of their hooked addresses is re-hooked here.
//
// Hooked methods (exact il2cpp signatures; chained once, return preserved):
//   * RouteCompletionGuiController.Initialize(arg) -> void   (addr_reward_init)
//   * RouteRatingGuiController.Initialize(arg)     -> void   (addr_rating_init)
//   * RouteConsumptionService.bjan(progress)       -> void   (addr_completion_resp)
// The reward/rating hooks root the GUI receiver into the bounded pendingUi queue (<=4) when a reward is
// already confirmed and the capture window is live. The bjan hook correlates the ProgressRouteOutProto
// status + the current route id (read through this module's OWN config: get_CurrentRoute on the hook
// receiver, then Route/Id) against the latched `completing` and publishes the reward confirmation.
//
// SCOPED LIMITATION: the route-tracking error method, the waypoint method and libunity's
// LocationService::IsServiceEnabledByUser() icall KEEP a thin Gum attach in routes.js this batch (their
// native signatures are not established and cannot be device-verified now). Those attach points hold NO
// state: they forward to tracking_enter/tracking_leave/location_gate/waypoint_observed so ALL observer
// state lives native. Converting them to Dobby full-replacement is remaining work (routes48-contract.txt).

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "RoutesObservers native ABI requires LP64");

enum PgrRoutesObserversInfo {
  PGR_RO_FEATURE_ID        = 46,
  PGR_RO_API_VERSION       = 1,
  PGR_RO_CONFIG_VERSION    = 1,
  PGR_RO_REQUEST_VERSION   = 1,
  PGR_RO_COMPLETING_VERSION= 1,
  PGR_RO_OUTPUT_VERSION    = 1,
  PGR_RO_MAX_LEASE_MS      = 2500,  // lease ceiling (CLOCK_BOOTTIME); mirrors quick-close foreground lease
  PGR_RO_REWARD_CAPTURE_MS = 15000, // reward/rating Initialize capture window from rewardAt
  PGR_RO_COMPLETING_MS     = 30000, // bjan correlation window from completing.at
  PGR_RO_WAYPOINT_MS       = 10000, // waypoint event throttle
  PGR_RO_UI_SETTLE_MS      = 100,   // pendingUi minimum age before a dismissal decision
  PGR_RO_UI_MAX_MS         = 10000, // pendingUi maximum age before it is dropped
  PGR_RO_COMPLETION_MS     = 3000,  // completion reported to the snapshot after rewardAt + this
  PGR_RO_ID_MAX            = 128,   // UTF16 route-id length ceiling (routes.js id length 1..128)
  PGR_RO_PENDING_UI_MAX    = 4,     // pendingUi queue ceiling (routes.js pendingUi.length>=4)
  PGR_RO_MAX_EVENTS        = 8,     // events drained per poll()
  PGR_RO_PTR_COUNT         = 22,    // config pointer slots 0..21, all mandatory (non-null)
  PGR_RO_PROGRESS_STATUS_OFFSET = 0x14, // ProgressRouteOutProto.Status (int32)
  PGR_RO_GUI_STATE_OFFSET       = 0x70, // GuiController.currentState (int32)
  PGR_RO_GUI_STATE_DISMISSED    = 2,    // currentState value that permits Dismiss()
  PGR_RO_COMPLETION_SUCCESS     = 1     // bjan progress Status success branch
};

// Entry-point return == out.status (poll): 1 OK / 0 SKIP (benign gate) / negative fault. install()
// returns 0 on success; negatives are faults. The lite entries return 1 (acted) / 0 (did not).
enum PgrRoutesObserversStatus {
  PGR_RO_OK                = 1,
  PGR_RO_SKIP              = 0,
  PGR_RO_OK_INSTALL        = 0,   // install() success (alias of 0)
  PGR_RO_FAULT             = -1,
  PGR_RO_ERR_NULL_ARG      = -2,
  PGR_RO_ERR_BAD_VERSION   = -3,
  PGR_RO_ERR_BAD_SIZE      = -4,
  PGR_RO_ERR_MISSING_PTR   = -5,  // install(): a mandatory config pointer was null
  PGR_RO_ERR_NOT_INSTALLED = -6,
  PGR_RO_ERR_RECONFIG      = -9,  // install(): a different config at an already-installed table
  PGR_RO_ERR_CLOCK         = -10,
  PGR_RO_ERR_BUSY          = -11, // reentrant poll/drain refused (defensive)
  PGR_RO_ERR_HOOK          = -12  // a Dobby hook failed to install (rolled back, fail-closed)
};

// poll() out.flags.
enum PgrRoutesObserversFlags {
  PGR_RO_FLAG_COMPLETION_READY = 1  // completion set && boottime-rewardAt>=COMPLETION_MS && pendingUi empty
};

// Event type (PgrRoutesObserversEvent.type).
enum PgrRoutesObserversEventType {
  PGR_RO_EV_TRACKING_STATUS    = 1, // i0 = tracking ret code
  PGR_RO_EV_WAYPOINT           = 2, // f0 = lat, f1 = lon
  PGR_RO_EV_REWARD_CONFIRMED   = 3, // i0 = seq, id = route id
  PGR_RO_EV_REWARD_REJECTED    = 4, // i0 = progress status, id = route id
  PGR_RO_EV_RESULT_DISMISSED   = 5, // aux = screen (0 reward, 1 rating)
  PGR_RO_EV_RESULT_DISMISS_ERR = 6  // aux = screen
};

enum PgrRoutesObserversScreen { PGR_RO_SCREEN_REWARD = 0, PGR_RO_SCREEN_RATING = 1 };

// Fixed LP64 config (184 bytes): 8-byte header + 22 pointers in the EXACT slot order below. Every
// pointer is an already-validated il2cpp export / Il2CppClass* / MethodInfo* / FieldInfo* / live code
// address packed by the host from routes.js metadata. All are MANDATORY (non-null). Reordering is
// forbidden. install() publishes the config immutably: identical config idempotent (0); different rejected.
struct PgrRoutesObserversConfig {
  uint32_t version;  // == PGR_RO_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrRoutesObserversConfig)

  // il2cpp runtime exports (slots 0-8).
  void *runtime_invoke;        // 0  void*     (MethodInfo*, obj, void** params, void** exc)
  void *object_get_class;      // 1  void*     (obj)
  void *field_get_value;       // 2  void      (obj, FieldInfo*, out)     [reference read]
  void *gchandle_new;          // 3  uintptr_t (obj, int pinned)
  void *gchandle_get_target;   // 4  void*     (uintptr_t)
  void *gchandle_free;         // 5  void      (uintptr_t)
  void *thread_current;        // 6  void*     (void) -> attached Unity thread or null
  void *string_length;         // 7  int32_t   (string)
  void *string_chars;          // 8  uint16_t* (string) -> interior chars (not NUL terminated)

  // Exact Il2CppClass* identities (slots 9-14).
  void *class_consumption;     // 9  RouteConsumptionService (bjan receiver)
  void *class_progress;        // 10 ProgressRouteOutProto (bjan arg0)
  void *class_play;            // 11 RoutePlayProto (get_CurrentRoute result)
  void *class_shared;          // 12 SharedRouteProto (route definition)
  void *class_reward_gui;      // 13 RouteCompletionGuiController (reward Initialize receiver)
  void *class_rating_gui;      // 14 RouteRatingGuiController (rating Initialize receiver)

  // MethodInfo* (slots 15-16), invoked via runtime_invoke.
  void *m_current_route;       // 15 get_CurrentRoute() -> RoutePlayProto (nullable)
  void *m_dismiss_gui;         // 16 GuiController.Dismiss() -> void (invoked on the GUI instance)

  // Validated Il2CppFieldInfo* (slots 17-18).
  void *f_play_route;          // 17 RoutePlayProto.Route -> SharedRouteProto (reference)
  void *f_shared_id;           // 18 SharedRouteProto.Id -> System.String (reference)

  // Live code addresses of the three hooked methods (slots 19-21).
  void *addr_reward_init;      // 19 RouteCompletionGuiController.Initialize(1)
  void *addr_rating_init;      // 20 RouteRatingGuiController.Initialize(1)
  void *addr_completion_resp;  // 21 RouteConsumptionService.bjan(ProgressRouteOutProto)
};

static_assert(sizeof(PgrRoutesObserversConfig) == 184, "PgrRoutesObserversConfig must remain 184 bytes on LP64");
static_assert(offsetof(PgrRoutesObserversConfig, runtime_invoke) == 8, "config body start (slot 0)");
static_assert(offsetof(PgrRoutesObserversConfig, class_consumption) == 80, "slot 9");       // 8 + 9*8
static_assert(offsetof(PgrRoutesObserversConfig, m_current_route) == 128, "slot 15");       // 8 + 15*8
static_assert(offsetof(PgrRoutesObserversConfig, f_play_route) == 144, "slot 17");          // 8 + 17*8
static_assert(offsetof(PgrRoutesObserversConfig, addr_reward_init) == 160, "slot 19");      // 8 + 19*8
static_assert(offsetof(PgrRoutesObserversConfig, addr_completion_resp) == 176, "slot 21");  // 8 + 21*8

// Fixed LP64 request (32 bytes) for poll / clear_completing / drain.
struct PgrRoutesObserversRequest {
  uint32_t version;     // off 0  == PGR_RO_REQUEST_VERSION
  uint32_t size;        // off 4  == sizeof(PgrRoutesObserversRequest)
  uint64_t tid;         // off 8  proven owner tid
  uint64_t residual_ms; // off 16 lease window; clamped, 0 refuses (drain ignores the lease)
  uint64_t generation;  // off 24 revocation token (== generation() at stamp)
};

static_assert(sizeof(PgrRoutesObserversRequest) == 32, "PgrRoutesObserversRequest must remain 32 bytes on LP64");
static_assert(offsetof(PgrRoutesObserversRequest, tid) == 8, "request tid");
static_assert(offsetof(PgrRoutesObserversRequest, residual_ms) == 16, "request residual");
static_assert(offsetof(PgrRoutesObserversRequest, generation) == 24, "request generation");

// Fixed LP64 set_completing input (312 bytes). consumption_handle is a BORROWED feature-45 GC handle;
// this module stores its id to re-resolve the bjan receiver later and NEVER frees it.
struct PgrRoutesObserversCompleting {
  uint32_t version;           // off 0  == PGR_RO_COMPLETING_VERSION
  uint32_t size;              // off 4  == sizeof(PgrRoutesObserversCompleting)
  uint64_t tid;               // off 8  proven owner tid
  uint64_t residual_ms;       // off 16 lease window; clamped, 0 refuses
  uint64_t generation;        // off 24 revocation token
  uint64_t consumption_handle;// off 32 borrowed consumption-service GC handle (never freed here)
  uint64_t seq;               // off 40 the complete command sequence
  uint32_t id_length;         // off 48 UTF16 units in id (1..128)
  uint32_t reserved;          // off 52 0 in v1
  uint16_t id[PGR_RO_ID_MAX]; // off 56 requested route id (UTF16, not NUL terminated)
};

static_assert(sizeof(PgrRoutesObserversCompleting) == 312, "PgrRoutesObserversCompleting must remain 312 bytes on LP64");
static_assert(offsetof(PgrRoutesObserversCompleting, consumption_handle) == 32, "completing handle");
static_assert(offsetof(PgrRoutesObserversCompleting, seq) == 40, "completing seq");
static_assert(offsetof(PgrRoutesObserversCompleting, id_length) == 48, "completing id length");
static_assert(offsetof(PgrRoutesObserversCompleting, id) == 56, "completing id");

// One drained observer event (296 bytes).
struct PgrRoutesObserversEvent {
  uint32_t type;              // off 0  PgrRoutesObserversEventType
  uint32_t aux;               // off 4  screen for dismiss events
  int64_t  i0;                // off 8  tracking code / reward seq / reject status
  double   f0;                // off 16 waypoint lat
  double   f1;                // off 24 waypoint lon
  uint32_t id_length;         // off 32 UTF16 units in id (reward events; else 0)
  uint32_t reserved;          // off 36 0 in v1
  uint16_t id[PGR_RO_ID_MAX]; // off 40 route id (reward events)
};

static_assert(sizeof(PgrRoutesObserversEvent) == 296, "PgrRoutesObserversEvent must remain 296 bytes on LP64");
static_assert(offsetof(PgrRoutesObserversEvent, i0) == 8, "event i0");
static_assert(offsetof(PgrRoutesObserversEvent, f0) == 16, "event f0");
static_assert(offsetof(PgrRoutesObserversEvent, id) == 40, "event id");

// Fixed LP64 poll output (2672 bytes). Fully zeroed on entry; header re-stamped.
struct PgrRoutesObserversOutput {
  uint32_t version;              // off 0  == PGR_RO_OUTPUT_VERSION
  uint32_t size;                 // off 4  == sizeof(PgrRoutesObserversOutput)
  int32_t  status;               // off 8  == the poll() return
  uint32_t flags;                // off 12 PgrRoutesObserversFlags
  uint64_t generation;           // off 16 g_gen at publish
  uint64_t completion_seq;       // off 24 confirmed completion seq (valid with COMPLETION_READY)
  uint32_t completion_id_length; // off 32 UTF16 units in completion_id (1..128 with COMPLETION_READY)
  uint32_t event_count;          // off 36 valid events in events[] (<= PGR_RO_MAX_EVENTS)
  uint32_t dropped_events;       // off 40 events dropped since the previous poll (ring overflow)
  uint32_t reserved;             // off 44 0 in v1
  uint16_t completion_id[PGR_RO_ID_MAX];       // off 48  confirmed completion route id
  PgrRoutesObserversEvent events[PGR_RO_MAX_EVENTS]; // off 304
};

static_assert(sizeof(PgrRoutesObserversOutput) == 2672, "PgrRoutesObserversOutput must remain 2672 bytes on LP64");
static_assert(offsetof(PgrRoutesObserversOutput, status) == 8, "output status");
static_assert(offsetof(PgrRoutesObserversOutput, completion_seq) == 24, "output completion seq");
static_assert(offsetof(PgrRoutesObserversOutput, completion_id) == 48, "output completion id");
static_assert(offsetof(PgrRoutesObserversOutput, events) == 304, "output events");

// Fixed LP64 table: 8-byte header + 11 function pointers = 96 bytes.
struct PgrRoutesObserversApi {
  uint32_t version;  // == PGR_RO_API_VERSION
  uint32_t size;     // == sizeof(PgrRoutesObserversApi)
  // Verify + publish the immutable config and install the three hooks ONCE for the process lifetime.
  // Idempotent for an identical config; a different config returns PGR_RO_ERR_RECONFIG. A partial hook
  // install is rolled back and latched (PGR_RO_ERR_HOOK).
  int (*install)(const PgrRoutesObserversConfig *config);
  // Current revocation generation (initially 1). The host stamps it onto each owner request.
  uint64_t (*generation)(void);
  // Atomically bump the generation and clear the lease deadline. Any thread; no managed work; no frees.
  uint64_t (*revoke)(void);
  // Owner-thread tick: refresh the lease, dismiss settled pendingUi, drain events, report completion.
  int (*poll)(const PgrRoutesObserversRequest *request, PgrRoutesObserversOutput *out);
  // Owner-thread: latch `completing` just before the complete invoke so the bjan hook can correlate it.
  int (*set_completing)(const PgrRoutesObserversCompleting *completing);
  // Owner-thread: clear `completing` when the complete action was refused/faulted (no bjan will arrive).
  int (*clear_completing)(const PgrRoutesObserversRequest *request);
  // Owner-thread: free every pendingUi root and clear all result/tracking state (release / stopping).
  int (*drain)(const PgrRoutesObserversRequest *request);
  // Route-tracking enter: depth[tid]++ (tid must equal gettid()); returns 1 when counted.
  int (*tracking_enter)(uint64_t tid);
  // Route-tracking leave: depth[tid]--; queue TRACKING_STATUS when the code changed. Returns 1 when counted.
  int (*tracking_leave)(uint64_t tid, int32_t code);
  // Nested location-enabled override decision: 1 iff depth[tid]>0 and the lease is live.
  int (*location_gate)(uint64_t tid);
  // Waypoint observation: lease + 10s throttle gate; queue WAYPOINT. Returns 1 when queued.
  int (*waypoint_observed)(uint64_t tid, double lat, double lon);
};

static_assert(sizeof(PgrRoutesObserversApi) == 96, "PgrRoutesObserversApi must remain a 96-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 46 and answers only on an exact version match.
const void *pgrazor_routes_observers_api(uint32_t version);

#ifdef __cplusplus
}
#endif

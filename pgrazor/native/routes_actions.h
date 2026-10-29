#pragma once
#include <stdint.h>

// Feature 43 (API 1). Native ROUTES MANAGED STATE/ACTIONS: the C++ port of the authoritative
// current-route read and the discover/start/complete/pause managed submissions in
// runtime/features/routes.js (readCurrent / doDiscover / doStart / doComplete / doPause). It owns
// ONLY those five synchronous operations. Java keeps the walking orchestration; JS keeps discovery
// snapshotting, service capture, the reward/rating UI observers and all event emission. This module
// installs NO hook, builds NO RPC, adds NO retry or auto-walk, and NEVER calls back into JS.
//
// One entry point drives everything: execute(request,out). The action is selected by request.action
// (0 CURRENT read, 1 DISCOVER, 2 START, 3 COMPLETE, 4 PAUSE). The service and route objects arrive as
// BORROWED host GC handles: the module resolves each fresh, takes its OWN native root before any other
// managed work, and NEVER frees a host handle. Every temporary root is re-resolved after each
// runtime_invoke / object_new / gchandle_* (relocating collector) and freed before the output is
// published; no raw managed pointer is reused across an allocation and no mutex is ever held across a
// managed call. A busy flag spans ALL temporary-root cleanup; a reentrant call is refused.
//
// Authorization is carried per request: a proven owner tid, a residual foreground lease (clamped to
// PGR_RA_MAX_LEASE_MS, CLOCK_BOOTTIME) and a revocation generation. generation() hands the host the
// current token to stamp onto requests; revoke() atomically bumps it from any thread (no managed work,
// no handle frees on a foreign thread), so a stamped-but-stale request fails the gate. A fresh
// deadline + generation check precedes every managed call and is re-checked immediately before the one
// mutating invoke (pre-invoke must be eligible-fresh). A mutation that has issued its invoke reports
// SUBMITTED even if a revoke raced the call -- it is NEVER retried. A managed exception becomes a
// negative fault. For mutating actions a process-wide monotonic sequence high-water is consumed once
// before any managed work (surviving later failures); a repeated or backward seq is refused stale-seq.
// CURRENT uses seq 0 and is exempt.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "RoutesActions native ABI requires LP64");

enum PgrRoutesActionsInfo {
  PGR_RA_FEATURE_ID      = 43,
  PGR_RA_API_VERSION     = 1,
  PGR_RA_CONFIG_VERSION  = 1,
  PGR_RA_REQUEST_VERSION = 1,
  PGR_RA_OUTPUT_VERSION  = 1,
  PGR_RA_MAX_LEASE_MS    = 2500,  // residual foreground lease ceiling (JS quick-close lease window)
  PGR_RA_ID_MAX          = 128,   // UTF16 route-id length ceiling (JS id length 1..128)
  PGR_RA_PTR_COUNT       = 43     // config pointer slots 0..42, all mandatory (non-null)
};

// execute() action selector (request.action).
enum PgrRoutesActionsAction {
  PGR_RA_ACT_CURRENT  = 0,  // read-only authoritative current-route snapshot (seq 0, no mutation)
  PGR_RA_ACT_DISCOVER = 1,  // RequestNearbyRoutes() on the routes service
  PGR_RA_ACT_START    = 2,  // build RoutesConsumptionStartData + StartRoute()
  PGR_RA_ACT_COMPLETE = 3,  // confirm completion (bizv) on the authoritative current route
  PGR_RA_ACT_PAUSE    = 4   // PauseRoute(true)
};

// execute() return == out.status: 1 SUCCESS (CURRENT published / mutation submitted), 0 REFUSE (a
// benign gate / game-state refusal; out.reason carries which), or a negative FAULT. install() returns
// 0 on success; negatives are faults. out.reason is always one of PgrRoutesActionsReason.
enum PgrRoutesActionsStatus {
  PGR_RA_SUCCESS           = 1,
  PGR_RA_REFUSE            = 0,
  PGR_RA_OK                = 0,   // install() success (alias of 0)
  PGR_RA_FAULT             = -1,  // native/managed fault; out.reason = alloc-failed or native-error
  PGR_RA_ERR_NULL_ARG      = -2,
  PGR_RA_ERR_BAD_VERSION   = -3,
  PGR_RA_ERR_BAD_SIZE      = -4,
  PGR_RA_ERR_MISSING_PTR   = -5,  // install(): a mandatory config pointer was null
  PGR_RA_ERR_NOT_INSTALLED = -6,
  PGR_RA_ERR_RECONFIG      = -9,  // install(): a different config at an already-installed table
  PGR_RA_ERR_CLOCK         = -10
};

// out.reason: the enumerated cause. SUCCESS carries NONE. REFUSE carries one of the game-state /
// gate reasons. A negative FAULT carries ALLOC_FAILED (object_new) or NATIVE_ERROR (managed throw /
// root failure / reentry / wrong class on a fail-closed path). Values match the host adapter.
enum PgrRoutesActionsReason {
  PGR_RA_REASON_NONE          = 0,
  PGR_RA_REASON_LEASE_EXPIRED = 1,   // gate: thread/attached/residual/generation not fresh
  PGR_RA_REASON_AWAITING_SVC  = 2,   // service handle did not resolve to the expected class
  PGR_RA_REASON_NOT_INIT      = 3,   // START: FeatureFlagInitialized false
  PGR_RA_REASON_NOT_PERMITTED = 4,   // START: RouteConsumptionEnabledForPlayer false
  PGR_RA_REASON_ALREADY_ACTIVE= 5,   // START: already consuming a route
  PGR_RA_REASON_NOT_DISCOVERED= 6,   // START: no route handle supplied
  PGR_RA_REASON_ROUTE_STALE   = 7,   // START: route handle null / wrong class
  PGR_RA_REASON_ID_MISMATCH   = 8,   // START/COMPLETE: route id != requested id
  PGR_RA_REASON_NOT_REVERSIBLE= 9,   // START: reverse requested but route not reversible
  PGR_RA_REASON_ALLOC_FAILED  = 10,  // START: RoutesConsumptionStartData allocation failed
  PGR_RA_REASON_NO_CURRENT    = 11,  // COMPLETE/PAUSE: no current route
  PGR_RA_REASON_COMPLETING    = 12,  // COMPLETE: already completing
  PGR_RA_REASON_REWARDED      = 13,  // COMPLETE: reward already acquired
  PGR_RA_REASON_NOT_ELIGIBLE  = 14,  // COMPLETE: paused / not started / not walked / out of range
  PGR_RA_REASON_ALREADY_PAUSED= 15,  // PAUSE: already paused
  PGR_RA_REASON_STALE_SEQ     = 16,  // mutating: seq not above the process-wide high-water
  PGR_RA_REASON_NATIVE_ERROR  = 17   // managed exception / root failure / reentry / fail-closed class
};

// out.flags (CURRENT): the authoritative current-route snapshot. An empty current (no play / null /
// wrong class) publishes flags 0 and id_length 0. A present route id decodes to id_length 1..128.
enum PgrRoutesActionsFlags {
  PGR_RA_FLAG_PRESENT         = 1,
  PGR_RA_FLAG_STARTED         = 2,
  PGR_RA_FLAG_PAUSED          = 4,
  PGR_RA_FLAG_COMPLETED_WALK  = 8,
  PGR_RA_FLAG_ACQUIRED_REWARD = 16,
  PGR_RA_FLAG_IN_RANGE_END    = 32,
  PGR_RA_FLAG_IN_RANGE_RESUME = 64,
  PGR_RA_FLAG_IS_COMPLETING   = 128,
  PGR_RA_FLAG_REVERSED        = 256
};

// Fixed LP64 config (352 bytes): 8-byte header + 43 pointers in the EXACT slot order below. Every
// pointer is an already-validated il2cpp export / Il2CppClass* / MethodInfo* / FieldInfo* packed by
// the host from routes.js metadata. All are MANDATORY (non-null). Reordering is forbidden. install()
// publishes the config immutably: an identical config is idempotent (0); a different one is rejected.
struct PgrRoutesActionsConfig {
  uint32_t version;  // == PGR_RA_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrRoutesActionsConfig)

  // il2cpp runtime exports (slots 0-10).
  void *runtime_invoke;        // 0  void*     (MethodInfo*, void* obj, void** params, void** exc)
  void *object_get_class;      // 1  void*     (void* obj)
  void *field_get_value;       // 2  void      (void* obj, FieldInfo*, void* out)  [no error return]
  void *object_unbox;          // 3  void*     (void* boxed) -> value pointer
  void *gchandle_new;          // 4  uintptr_t (void* obj, int pinned)
  void *gchandle_get_target;   // 5  void*     (uintptr_t handle)
  void *gchandle_free;         // 6  void      (uintptr_t handle)
  void *thread_current;        // 7  void*     (void) -> attached Unity thread or null
  void *object_new;            // 8  void*     (Il2CppClass*) -> new managed object
  void *field_set_value_object;// 9  void      (void* obj, FieldInfo*, void* valueObj) [reference]
  void *field_set_value;       // 10 void      (void* obj, FieldInfo*, void* value)    [value-type]

  // Exact Il2CppClass* identities (slots 11-17).
  void *class_consumption;     // 11 RouteConsumptionService
  void *class_routes;          // 12 RoutesService
  void *class_play;            // 13 RoutePlayProto (CurrentRoute)
  void *class_shared;          // 14 SharedRouteProto (the route definition)
  void *class_startData;       // 15 RoutesConsumptionStartData (START constructs this)
  void *class_bool;            // 16 System.Boolean (boxed getter result)
  void *class_string;          // 17 System.String (route id)

  // MethodInfo* (slots 18-31), invoked via runtime_invoke.
  void *m_currentRoute;        // 18 get_CurrentRoute() -> RoutePlayProto (nullable)
  void *m_hasStarted;          // 19 get_RouteHasStarted() -> Boolean
  void *m_inRangeOfEnd;        // 20 get_InRangeOfEnd() -> Boolean
  void *m_inRangeToResume;     // 21 get_InRangeToResume() -> Boolean
  void *m_isCompleting;        // 22 get_IsCompletingRoute() -> Boolean
  void *m_flagInitialized;     // 23 get_FeatureFlagInitialized() -> Boolean
  void *m_enabledForPlayer;    // 24 get_RouteConsumptionEnabledForPlayer() -> Boolean
  void *m_isConsuming;         // 25 get_IsConsumingRoute() -> Boolean
  void *m_isPaused;            // 26 get_IsRoutePaused() -> Boolean
  void *m_start;               // 27 StartRoute(RoutesConsumptionStartData) -> void
  void *m_confirmComplete;     // 28 bizv() -> void  (confirmed completion)
  void *m_pause;               // 29 PauseRoute(Boolean) -> IPromise
  void *m_requestNearby;       // 30 RequestNearbyRoutes()
  void *m_ctor;                // 31 RoutesConsumptionStartData..ctor() -> void

  // Validated Il2CppFieldInfo* (slots 32-40).
  void *f_playRoute;           // 32 RoutePlayProto.Route -> SharedRouteProto (reference)
  void *f_playPaused;          // 33 RoutePlayProto.Paused (bool)
  void *f_playCompletedWalk;   // 34 RoutePlayProto.CompletedWalk (bool)
  void *f_playAcquiredReward;  // 35 RoutePlayProto.AcquiredReward (bool)
  void *f_playTravelInReverse; // 36 RoutePlayProto.TravelInReverse (bool)
  void *f_sharedId;            // 37 SharedRouteProto.Id -> System.String (reference)
  void *f_sharedReversible;    // 38 SharedRouteProto.Reversible (bool)
  void *f_dataPublishedRoute;  // 39 RoutesConsumptionStartData.PublishedRouteProto (reference)
  void *f_dataReversed;        // 40 RoutesConsumptionStartData.Reversed (bool)

  // il2cpp string exports (slots 41-42).
  void *string_length;         // 41 int32_t   (void* string)
  void *string_chars;          // 42 uint16_t* (void* string) -> interior chars (not NUL terminated)
};

static_assert(sizeof(PgrRoutesActionsConfig) == 352, "PgrRoutesActionsConfig must remain 352 bytes on LP64");
static_assert(offsetof(PgrRoutesActionsConfig, runtime_invoke) == 8, "config body start (slot 0)");
static_assert(offsetof(PgrRoutesActionsConfig, class_consumption) == 96, "slot 11");   // 8 + 11*8
static_assert(offsetof(PgrRoutesActionsConfig, class_play) == 112, "slot 13");         // 8 + 13*8
static_assert(offsetof(PgrRoutesActionsConfig, m_currentRoute) == 152, "slot 18");     // 8 + 18*8
static_assert(offsetof(PgrRoutesActionsConfig, m_ctor) == 256, "slot 31");             // 8 + 31*8
static_assert(offsetof(PgrRoutesActionsConfig, f_playRoute) == 264, "slot 32");        // 8 + 32*8
static_assert(offsetof(PgrRoutesActionsConfig, f_sharedId) == 304, "slot 37");         // 8 + 37*8
static_assert(offsetof(PgrRoutesActionsConfig, f_dataPublishedRoute) == 320, "slot 39");// 8 + 39*8
static_assert(offsetof(PgrRoutesActionsConfig, string_length) == 336, "slot 41");      // 8 + 41*8
static_assert(offsetof(PgrRoutesActionsConfig, string_chars) == 344, "slot 42");       // 8 + 42*8

// Fixed LP64 request (328 bytes). tid is the proven owner tid. residual_ms is the remaining lease
// (clamped to PGR_RA_MAX_LEASE_MS; 0 refuses). generation is the revocation token stamped from
// generation(). service_handle / route_handle are BORROWED host GC handles (never freed here). seq is
// the process-wide monotonic mutating sequence (ignored for CURRENT, which uses 0). id is the
// requested route id (START/COMPLETE), id_length 1..128 units.
struct PgrRoutesActionsRequest {
  uint32_t version;        // off 0  == PGR_RA_REQUEST_VERSION
  uint32_t size;           // off 4  == sizeof(PgrRoutesActionsRequest)
  uint64_t tid;            // off 8  proven owner tid
  uint64_t residual_ms;    // off 16 residual lease; clamped, 0 refuses
  uint64_t generation;     // off 24 revocation token (== generation() at stamp time)
  uint64_t service_handle; // off 32 borrowed consumption/routes service GC handle
  uint64_t route_handle;   // off 40 borrowed SharedRouteProto GC handle (START)
  uint64_t seq;            // off 48 mutating sequence (CURRENT: 0, exempt)
  uint32_t action;         // off 56 PgrRoutesActionsAction
  uint32_t reversed;       // off 60 START: request reverse walk (0/1)
  uint32_t id_length;      // off 64 UTF16 units in id (START/COMPLETE: 1..128)
  uint32_t reserved;       // off 68 0 in v1
  uint16_t id[PGR_RA_ID_MAX]; // off 72 requested route id (UTF16, not NUL terminated)
};

static_assert(sizeof(PgrRoutesActionsRequest) == 328, "PgrRoutesActionsRequest must remain 328 bytes on LP64");
static_assert(offsetof(PgrRoutesActionsRequest, tid) == 8, "request tid");
static_assert(offsetof(PgrRoutesActionsRequest, residual_ms) == 16, "request residual");
static_assert(offsetof(PgrRoutesActionsRequest, generation) == 24, "request generation");
static_assert(offsetof(PgrRoutesActionsRequest, service_handle) == 32, "request service handle");
static_assert(offsetof(PgrRoutesActionsRequest, route_handle) == 40, "request route handle");
static_assert(offsetof(PgrRoutesActionsRequest, seq) == 48, "request seq");
static_assert(offsetof(PgrRoutesActionsRequest, action) == 56, "request action");
static_assert(offsetof(PgrRoutesActionsRequest, reversed) == 60, "request reversed");
static_assert(offsetof(PgrRoutesActionsRequest, id_length) == 64, "request id length");
static_assert(offsetof(PgrRoutesActionsRequest, reserved) == 68, "request reserved");
static_assert(offsetof(PgrRoutesActionsRequest, id) == 72, "request id");

// Fixed LP64 output (280 bytes). status mirrors the return; reason the enumerated cause. For CURRENT,
// flags/id carry the snapshot (0/0 when empty). For a mutation, flags 0 and id_length 0.
struct PgrRoutesActionsOutput {
  uint32_t version;    // off 0  == PGR_RA_OUTPUT_VERSION
  uint32_t size;       // off 4  == sizeof(PgrRoutesActionsOutput)
  int32_t status;      // off 8  == the execute() return
  int32_t reason;      // off 12 PgrRoutesActionsReason
  uint32_t flags;      // off 16 PgrRoutesActionsFlags (CURRENT)
  uint32_t id_length;  // off 20 UTF16 units in id (CURRENT present route: 1..128)
  uint16_t id[PGR_RA_ID_MAX]; // off 24 current route id (UTF16, not NUL terminated)
};

static_assert(sizeof(PgrRoutesActionsOutput) == 280, "PgrRoutesActionsOutput must remain 280 bytes on LP64");
static_assert(offsetof(PgrRoutesActionsOutput, status) == 8, "output status");
static_assert(offsetof(PgrRoutesActionsOutput, reason) == 12, "output reason");
static_assert(offsetof(PgrRoutesActionsOutput, flags) == 16, "output flags");
static_assert(offsetof(PgrRoutesActionsOutput, id_length) == 20, "output id length");
static_assert(offsetof(PgrRoutesActionsOutput, id) == 24, "output id");

// Fixed LP64 table: 8-byte header + 4 function pointers = 40 bytes.
struct PgrRoutesActionsApi {
  uint32_t version;  // == PGR_RA_API_VERSION
  uint32_t size;     // == sizeof(PgrRoutesActionsApi)
  // Verify + publish the immutable config once for the process lifetime. Idempotent for an identical
  // config; a different config returns PGR_RA_ERR_RECONFIG. A null pointer or bad header is rejected.
  int (*install)(const PgrRoutesActionsConfig *config);
  // Current revocation generation (initially 1). The host stamps it onto each request.
  uint64_t (*generation)(void);
  // Atomically bump the generation and return the new value. Any thread; no managed work; frees no
  // handles. A request stamped with the prior generation then fails the gate (lease-expired).
  uint64_t (*revoke)(void);
  // Owner-thread synchronous read / managed submission. See the file header for the full contract.
  // 1 SUCCESS / 0 REFUSE / negative FAULT; out always carries status + reason.
  int (*execute)(const PgrRoutesActionsRequest *request, PgrRoutesActionsOutput *out);
};

static_assert(sizeof(PgrRoutesActionsApi) == 40, "PgrRoutesActionsApi must remain a 40-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature
// under feature id 43 and answers only on an exact version match.
const void *pgrazor_routes_actions_api(uint32_t version);

#ifdef __cplusplus
}
#endif

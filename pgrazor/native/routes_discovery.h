#pragma once
#include <stddef.h>
#include <stdint.h>

// Feature 44 (API 1). Native ROUTES DISCOVERY reader + rooted route cache: the C++ port of the read-only
// in-game Routes discovery snapshot and the retained route-definition cache in
// runtime/features/routes.js (readDiscovery / readShared / readAnchor and cacheRoute / pruneCache / the
// routeCache part of releaseAll). doStart's `routeCache.get(id)` route-handle acquisition becomes a
// lease-guarded native lookup(id). Java walking, service capture, the reward/rating UI observers, event
// emission and ALL of feature 43 (routes_actions: current + discover/start/complete/pause submissions)
// are unchanged. This module installs NO hook, builds NO RPC, adds NO retry/auto-walk, and NEVER calls
// back into JS.
//
// It OWNS the discovery read AND the retained GC roots to accepted SharedRouteProto route definitions
// (max PGR_RD_MAX_ROUTES). It has its OWN revocation generation, distinct from feature 43. doStart passes
// the BORROWED handle lookup() returns straight to the existing feature-43 action(START); feature 43
// resolves + roots it and never frees it. feature 43's ABI is NOT altered by this module.
//
// Entry points (all but install/generation/revoke are OWNER-THREAD ONLY):
//   discover(request,out)        -- walk didn->NearbyRoutesInfo->List<SharedRouteProto>, build a NEW cache
//                                   and TRANSACTIONALLY swap it in on full success (old roots freed on the
//                                   owner thread), publishing a bounded scalar route-header array. No
//                                   partial publish; a valid empty snapshot clears the cache; a fault /
//                                   expiry keeps the OLD cache unchanged.
//   points(request,index,out)    -- copy the already-decoded point array of admitted route `index`. NO
//                                   managed work; guarded by owner thread + generation-unchanged + range.
//   lookup(lookup,out_handle)    -- borrowed cache GC handle for the route whose id matches, bound to the
//                                   RoutesService identity and the published generation. Never freed here.
//   drain(request)               -- free ALL retained roots and clear the cache (release / stopping). Needs
//                                   an attached owner thread; NO lease; an off-owner call is a benign SKIP.
//   revoke()                     -- ANY thread: atomically bump the generation. NO managed work, frees NO
//                                   handle; a stamped-but-stale request then fails the gate and the
//                                   published cache can never revive. generation() hands out the token.
//
// Lifetime / rooting discipline is identical in intent to map_poi.cpp and routes_actions.cpp: every
// managed return is rooted immediately on the proven owner thread and re-resolved from its root after any
// runtime_invoke / gchandle_* (relocating collector); no raw managed pointer is reused across an
// allocation. Classes are validated exact; boxed Int32 counts against class_int32; bool bytes 0/1. The
// owner is latched once from a genuine on-thread call. The residual lease is clamped to
// PGR_RD_MAX_LEASE_MS (CLOCK_BOOTTIME), re-checked before every managed step and once more AFTER every
// temporary root is released and before the cache is swapped / the output published. An atomic busy guard
// spans ALL temporary-root cleanup; reentry refuses. Managed exceptions surface through runtime_invoke's
// exc out-param: a throw on the LIST level is a fault, a throw while reading one route's waypoints only
// skips that route (matching the JS try/catch). Cache point arrays are heap-staged per route with
// allocation-failure handling; the fixed ABI output buffers hold only copied scalars + copied UTF16.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "RoutesDiscovery native ABI requires LP64");

enum PgrRoutesDiscoveryInfo {
  PGR_RD_FEATURE_ID      = 44,
  PGR_RD_API_VERSION     = 1,
  PGR_RD_CONFIG_VERSION  = 1,
  PGR_RD_REQUEST_VERSION = 1,
  PGR_RD_LOOKUP_VERSION  = 1,
  PGR_RD_OUTPUT_VERSION  = 1,
  PGR_RD_POINTS_VERSION  = 1,
  PGR_RD_MAX_LEASE_MS    = 2500,  // residual foreground lease ceiling (JS quick-close lease window)
  PGR_RD_MAX_ROUTES      = 100,   // snapshot / cache route ceiling (JS LIMITS.routesMax)
  PGR_RD_MAX_POINTS      = 5000,  // waypoints per route (JS LIMITS.pointsMax)
  PGR_RD_ID_MAX          = 128,   // UTF16 route-id length ceiling (JS id length 1..128)
  PGR_RD_NAME_MAX        = 256,   // UTF16 route-name length ceiling (JS boundSnapshot name slice)
  PGR_RD_PTR_COUNT       = 38     // config pointer slots 0..35, all mandatory (non-null)
};

// Entry-point return == out.status: 1 OK / 0 SKIP (benign gate/game-state) / negative fault.
enum PgrRoutesDiscoveryStatus {
  PGR_RD_OK                = 1,
  PGR_RD_SKIP              = 0,
  PGR_RD_ERR_NULL_ARG      = -1,
  PGR_RD_ERR_BAD_VERSION   = -2,
  PGR_RD_ERR_BAD_SIZE      = -3,
  PGR_RD_ERR_MISSING_PTR   = -4,  // install(): a mandatory config pointer was null
  PGR_RD_ERR_NOT_INSTALLED = -6,
  PGR_RD_ERR_RECONFIG      = -9,  // install(): a different config at an already-installed table
  PGR_RD_ERR_CLOCK         = -10,
  PGR_RD_ERR_BUSY          = -11, // reentrant call refused (defensive)
  PGR_RD_ERR_GCHANDLE      = -12, // a temporary/cache root could not be allocated / expected value null
  PGR_RD_ERR_MANAGED       = -13, // a LIST-level managed call threw
  PGR_RD_ERR_METADATA      = -14, // a resolved object had the wrong class / a count box was not Int32
  PGR_RD_ERR_BAD_COUNT     = -16, // the nearby-routes List length exceeded PGR_RD_MAX_ROUTES
  PGR_RD_ERR_ALLOCATION    = -17, // heap staging / point-array allocation failed
  PGR_RD_ERR_BAD_INDEX     = -18  // points(): index >= the published route count
};

// Route header flags.
enum PgrRoutesDiscoveryFlags {
  PGR_RD_FLAG_REVERSIBLE = 1,
  PGR_RD_FLAG_HAS_START  = 2,
  PGR_RD_FLAG_HAS_END    = 4
};

// Fixed LP64 config (312 bytes): 8-byte header + 38 pointers in the EXACT slot order below. Every pointer
// is an already-validated il2cpp export / Il2CppClass* / MethodInfo* / FieldInfo* packed by the host from
// routes.js metadata. All are MANDATORY (non-null). Reordering is forbidden. install() publishes the
// config immutably: an identical config is idempotent (0); a different one is rejected (RECONFIG).
struct PgrRoutesDiscoveryConfig {
  uint32_t version;  // == PGR_RD_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrRoutesDiscoveryConfig)

  // il2cpp runtime exports (slots 0-9).
  void *runtime_invoke;        // 0  void*     (MethodInfo*, void* obj, void** params, void** exc)
  void *object_get_class;      // 1  void*     (void* obj)
  void *field_get_value;       // 2  void      (void* obj, FieldInfo*, void* out)  [no error return]
  void *object_unbox;          // 3  void*     (void* boxed) -> value pointer
  void *gchandle_new;          // 4  uintptr_t (void* obj, int pinned)
  void *gchandle_get_target;   // 5  void*     (uintptr_t handle)
  void *gchandle_free;         // 6  void      (uintptr_t handle)
  void *thread_current;        // 7  void*     (void) -> attached Unity thread or null
  void *string_length;         // 8  int32_t   (void* string)
  void *string_chars;          // 9  uint16_t* (void* string) -> interior chars (not NUL terminated)

  // Exact Il2CppClass* identities (slots 10-17).
  void *class_routes;          // 10 RoutesService
  void *class_nearby;          // 11 NearbyRoutesInfo
  void *class_shared;          // 12 SharedRouteProto (route definition; cached)
  void *class_anchor;          // 13 RoutePoiAnchor
  void *class_waypoint;        // 14 RouteWaypointProto
  void *class_playerStats;     // 15 PlayerRouteStats
  void *class_string;          // 16 System.String
  void *class_int32;           // 17 System.Int32 (boxed collection-count class)

  // MethodInfo* (slots 18-21), invoked via runtime_invoke (inflated generics).
  void *m_listCount;           // 18 List<SharedRouteProto>.get_Count() -> Int32 (boxed)
  void *m_listItem;            // 19 List<SharedRouteProto>.get_Item(Int32) -> SharedRouteProto
  void *m_wpCount;             // 20 RepeatedField<RouteWaypointProto>.get_Count() -> Int32 (boxed)
  void *m_wpItem;              // 21 RepeatedField<RouteWaypointProto>.get_Item(Int32) -> RouteWaypointProto

  // Validated Il2CppFieldInfo* (slots 22-35).
  void *f_routes_didn;         // 22 RoutesService.didn -> NearbyRoutesInfo (reference)
  void *f_nearby_routes;       // 23 NearbyRoutesInfo.<Routes>k__BackingField -> List (reference)
  void *f_shared_id;           // 24 SharedRouteProto.Id -> System.String (reference)
  void *f_shared_name;         // 25 SharedRouteProto.Name -> System.String (reference)
  void *f_shared_reversible;   // 26 SharedRouteProto.Reversible (bool)
  void *f_shared_distance;     // 27 SharedRouteProto.RouteDistanceMeters (int64)
  void *f_shared_stats;        // 28 SharedRouteProto.PlayerStats -> PlayerRouteStats (reference)
  void *f_shared_waypoints;    // 29 SharedRouteProto.Waypoints -> RepeatedField (reference)
  void *f_shared_startPoi;     // 30 SharedRouteProto.StartPoi -> RoutePoiAnchor (reference)
  void *f_shared_endPoi;       // 31 SharedRouteProto.EndPoi -> RoutePoiAnchor (reference)
  void *f_anchor_anchor;       // 32 RoutePoiAnchor.Anchor -> RouteWaypointProto (reference)
  void *f_wp_lat;              // 33 RouteWaypointProto.LatDegrees (double)
  void *f_wp_lon;              // 34 RouteWaypointProto.LngDegrees (double)
  void *f_stats_cooldown;      // 35 PlayerRouteStats.CooldownFinishMs (int64)
  void *class_list; // 36 validated Routes backing collection class
  void *class_waypoints; // 37 validated SharedRoute Waypoints collection class
};

static_assert(sizeof(PgrRoutesDiscoveryConfig) == 312, "PgrRoutesDiscoveryConfig must remain 312 bytes on LP64");
static_assert(offsetof(PgrRoutesDiscoveryConfig, runtime_invoke) == 8, "config body start (slot 0)");
static_assert(offsetof(PgrRoutesDiscoveryConfig, class_routes) == 88, "slot 10");        // 8 + 10*8
static_assert(offsetof(PgrRoutesDiscoveryConfig, class_int32) == 144, "slot 17");        // 8 + 17*8
static_assert(offsetof(PgrRoutesDiscoveryConfig, m_listCount) == 152, "slot 18");        // 8 + 18*8
static_assert(offsetof(PgrRoutesDiscoveryConfig, f_routes_didn) == 184, "slot 22");      // 8 + 22*8
static_assert(offsetof(PgrRoutesDiscoveryConfig, f_wp_lon) == 280, "slot 34");           // 8 + 34*8
static_assert(offsetof(PgrRoutesDiscoveryConfig, f_stats_cooldown) == 288, "slot 35");   // 8 + 35*8

// Fixed LP64 request (48 bytes) for discover / points / drain.
struct PgrRoutesDiscoveryRequest {
  uint32_t version;        // off 0  == PGR_RD_REQUEST_VERSION
  uint32_t size;           // off 4  == sizeof(PgrRoutesDiscoveryRequest)
  uint64_t tid;            // off 8  proven owner tid
  uint64_t residual_ms;    // off 16 residual lease; clamped, 0 refuses (drain ignores the lease)
  uint64_t generation;     // off 24 revocation token (== generation() at stamp)
  uint64_t service_handle; // off 32 borrowed RoutesService GC handle (discover binds cache to it)
  uint32_t index;          // off 40 points(): route index (0 for discover/drain)
  uint32_t reserved;       // off 44 0 in v1
};

static_assert(sizeof(PgrRoutesDiscoveryRequest) == 48, "PgrRoutesDiscoveryRequest must remain 48 bytes on LP64");
static_assert(offsetof(PgrRoutesDiscoveryRequest, tid) == 8, "request tid");
static_assert(offsetof(PgrRoutesDiscoveryRequest, residual_ms) == 16, "request residual");
static_assert(offsetof(PgrRoutesDiscoveryRequest, generation) == 24, "request generation");
static_assert(offsetof(PgrRoutesDiscoveryRequest, service_handle) == 32, "request service handle");
static_assert(offsetof(PgrRoutesDiscoveryRequest, index) == 40, "request index");

// Fixed LP64 lookup (304 bytes). service_handle MUST resolve to the cache-bound RoutesService object.
struct PgrRoutesDiscoveryLookup {
  uint32_t version;        // off 0  == PGR_RD_LOOKUP_VERSION
  uint32_t size;           // off 4  == sizeof(PgrRoutesDiscoveryLookup)
  uint64_t tid;            // off 8  proven owner tid
  uint64_t residual_ms;    // off 16 residual lease; clamped, 0 refuses
  uint64_t generation;     // off 24 revocation token (== generation() at stamp)
  uint64_t service_handle; // off 32 borrowed RoutesService GC handle (must match the cache binding)
  uint32_t id_length;      // off 40 UTF16 units in id (1..128)
  uint32_t reserved;       // off 44 0 in v1
  uint16_t id[PGR_RD_ID_MAX]; // off 48 requested route id (UTF16, not NUL terminated)
};

static_assert(sizeof(PgrRoutesDiscoveryLookup) == 304, "PgrRoutesDiscoveryLookup must remain 304 bytes on LP64");
static_assert(offsetof(PgrRoutesDiscoveryLookup, service_handle) == 32, "lookup service handle");
static_assert(offsetof(PgrRoutesDiscoveryLookup, id_length) == 40, "lookup id length");
static_assert(offsetof(PgrRoutesDiscoveryLookup, id) == 48, "lookup id");

// Fixed LP64 route header (832 bytes), discover() output element. Scalar only -- NO managed pointer and NO
// inline points (fetched separately via points()). start/end doubles are valid only when flagged.
struct PgrRoutesDiscoveryRoute {
  uint32_t flags;        // off 0  PgrRoutesDiscoveryFlags
  uint32_t id_length;    // off 4  UTF16 units in id (1..128)
  uint32_t name_length;  // off 8  UTF16 units in name (0..256)
  uint32_t point_count;  // off 12 decoded waypoint count (2..5000)
  double distance_meters;// off 16 RouteDistanceMeters
  double cooldown_finish_ms; // off 24 PlayerStats.CooldownFinishMs (0 when no stats)
  double start_lat;      // off 32 (HAS_START)
  double start_lon;      // off 40 (HAS_START)
  double end_lat;        // off 48 (HAS_END)
  double end_lon;        // off 56 (HAS_END)
  uint16_t id[PGR_RD_ID_MAX];     // off 64  copied UTF16 route id
  uint16_t name[PGR_RD_NAME_MAX]; // off 320 copied UTF16 route name
};

static_assert(sizeof(PgrRoutesDiscoveryRoute) == 832, "PgrRoutesDiscoveryRoute must remain 832 bytes on LP64");
static_assert(offsetof(PgrRoutesDiscoveryRoute, distance_meters) == 16, "route distance");
static_assert(offsetof(PgrRoutesDiscoveryRoute, start_lat) == 32, "route start");
static_assert(offsetof(PgrRoutesDiscoveryRoute, id) == 64, "route id");
static_assert(offsetof(PgrRoutesDiscoveryRoute, name) == 320, "route name");

// Fixed LP64 discover output (83216 bytes). Fully zeroed on entry; header re-stamped. On a positive return
// the first `count` route headers are valid; on 0/negative count stays 0.
struct PgrRoutesDiscoveryOutput {
  uint32_t version;  // off 0  == PGR_RD_OUTPUT_VERSION
  uint32_t size;     // off 4  == sizeof(PgrRoutesDiscoveryOutput)
  int32_t  status;   // off 8  == the discover() return
  uint32_t count;    // off 12 number of valid route headers (<= PGR_RD_MAX_ROUTES)
  PgrRoutesDiscoveryRoute routes[PGR_RD_MAX_ROUTES]; // off 16
};

static_assert(sizeof(PgrRoutesDiscoveryOutput) == 83216, "PgrRoutesDiscoveryOutput must remain 83216 bytes on LP64");
static_assert(offsetof(PgrRoutesDiscoveryOutput, status) == 8, "output status");
static_assert(offsetof(PgrRoutesDiscoveryOutput, count) == 12, "output count");
static_assert(offsetof(PgrRoutesDiscoveryOutput, routes) == 16, "output routes");

// One decoded waypoint.
struct PgrRoutesDiscoveryPoint {
  double lat;  // off 0
  double lon;  // off 8
};

static_assert(sizeof(PgrRoutesDiscoveryPoint) == 16, "PgrRoutesDiscoveryPoint must remain 16 bytes on LP64");

// Fixed LP64 points output (80016 bytes), reused per route. Fully zeroed on entry; header re-stamped.
struct PgrRoutesDiscoveryPoints {
  uint32_t version;  // off 0  == PGR_RD_POINTS_VERSION
  uint32_t size;     // off 4  == sizeof(PgrRoutesDiscoveryPoints)
  int32_t  status;   // off 8  == the points() return
  uint32_t count;    // off 12 number of valid points (<= PGR_RD_MAX_POINTS)
  PgrRoutesDiscoveryPoint points[PGR_RD_MAX_POINTS]; // off 16
};

static_assert(sizeof(PgrRoutesDiscoveryPoints) == 80016, "PgrRoutesDiscoveryPoints must remain 80016 bytes on LP64");
static_assert(offsetof(PgrRoutesDiscoveryPoints, points) == 16, "points points");

// Fixed LP64 table: 8-byte header + 7 function pointers = 64 bytes.
struct PgrRoutesDiscoveryApi {
  uint32_t version;  // == PGR_RD_API_VERSION
  uint32_t size;     // == sizeof(PgrRoutesDiscoveryApi)
  // Verify + publish the immutable config once for the process lifetime. Idempotent for an identical
  // config; a different config returns PGR_RD_ERR_RECONFIG. A null pointer or bad header is rejected.
  int (*install)(const PgrRoutesDiscoveryConfig *config);
  // Current revocation generation (initially 1). The host stamps it onto each request.
  uint64_t (*generation)(void);
  // Atomically bump the generation and return the new value. Any thread; no managed work; frees no handle.
  uint64_t (*revoke)(void);
  // Owner-thread discovery read + transactional cache swap + scalar header publish.
  int (*discover)(const PgrRoutesDiscoveryRequest *request, PgrRoutesDiscoveryOutput *out);
  // Owner-thread copy of a published route's decoded point array (no managed work). index from request.index.
  int (*points)(const PgrRoutesDiscoveryRequest *request, uint32_t index, PgrRoutesDiscoveryPoints *out);
  // Owner-thread borrowed-handle lookup by id. *out_handle always written (0 on any non-OK); on OK it is a
  // BORROWED cache GC handle the caller must NOT free.
  int (*lookup)(const PgrRoutesDiscoveryLookup *lookup, uint64_t *out_handle);
  // Owner-thread cache drain: free ALL retained roots and clear the cache. Needs an attached thread; no
  // lease. An off-owner call is a benign SKIP (no frees).
  int (*drain)(const PgrRoutesDiscoveryRequest *request);
};

static_assert(sizeof(PgrRoutesDiscoveryApi) == 64, "PgrRoutesDiscoveryApi must remain a 64-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 44 and answers only on an exact version match.
const void *pgrazor_routes_discovery_api(uint32_t version);

#ifdef __cplusplus
}
#endif

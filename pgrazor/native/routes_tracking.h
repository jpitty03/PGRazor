#pragma once
#include <stddef.h>
#include <stdint.h>

// Feature 50 (API 1). Native ROUTE-TRACKING / LOCATION-OVERRIDE / WAYPOINT hooks: the Dobby
// full-replacement port of the THREE observe-only attach points that batch 48 (feature 46,
// routes_observers) deliberately left as thin Gum Interceptor attaches in
// runtime/features/routes.js because their native signatures were not yet established:
//
//   * Niantic.Holoholo.Routes.RoutesLocationService::CheckTrackingErrorStatus  (RVA 174921352)
//       managed:  .ErrorType CheckTrackingErrorStatus()         [instance, 0 managed args]
//       native :  int32_t fn(void *self, const void *method)    [trailing MethodInfo* unused]
//       -> the ErrorType enum is returned int32 in w0; see .artifacts/route-research disasm.
//   * Niantic.Holoholo.Routes.RoutesLocationService::bjfz        (RVA 174925564, waypoint add)
//       managed:  void bjfz(Holoholo.Rpc.RouteWaypointProto wp)  [instance, 1 managed arg]
//       native :  void fn(void *self, void *waypoint, const void *method)  [MethodInfo* unused]
//   * UnityEngine.LocationService::IsServiceEnabledByUser()       (libunity.so icall)
//       managed:  bool IsServiceEnabledByUser()                  [static icall, no MethodInfo]
//       native :  uint8_t fn(void)                               [bool in w0]
//
// SIGNATURE EVIDENCE (not guessed; see .artifacts/native-migration/routes-tracking-contract.txt):
//   * .artifacts/account-management/all-methods.json -- RVA->name (174921352 CheckTrackingErrorStatus,
//     174925564 bjfz, owner 4970 RoutesLocationService).
//   * .artifacts/route-research/location-methods.txt L114/L119 -- CheckTrackingErrorStatus -> .ErrorType ()
//     and bjfz -> void (Holoholo.Rpc.RouteWaypointProto).
//   * .artifacts/route-research/location-disasm.txt L1356+ / L2609+ -- CheckTrackingErrorStatus reads only
//     x0 (this) and returns int32 in w0; bjfz reads x1 (the RouteWaypointProto) at [x1,#0x18]/[x1,#0x20]
//     (= LatDegrees/LngDegrees doubles) and [x1,#0x28], returns void. The trailing MethodInfo register is
//     untouched by either body (forwarded verbatim regardless).
//   * .artifacts/route-research/fields.txt L498-502 -- Holoholo.Rpc.RouteWaypointProto.LatDegrees @0x18
//     (double), LngDegrees @0x20 (double): direct, guaranteed-nonallocating reads.
//
// This module OWNS NO STATE. It forwards every observation to the feature-46 routes_observers API,
// obtained INTERNALLY through that module's hidden getter pgrazor_routes_observers_api(1):
//   * CheckTrackingErrorStatus -> tracking_enter(gettid()) BEFORE and tracking_leave(gettid(),ret) AFTER
//     the original (balanced across reentry and an expired lease: leave is called iff enter counted).
//   * IsServiceEnabledByUser -> chains the original FIRST, then returns 1 only when location_gate(gettid())
//     reports a nested valid gate (route tracking depth>0 && lease live); otherwise returns the original
//     result unchanged.
//   * bjfz -> exact-class-checks the raw receiver arg (object_get_class == class_waypoint), reads the two
//     double fields to locals WITHOUT allocating any GC root (the reads are plain loads), forwards them
//     through waypoint_observed(gettid(),lat,lon) -- which owns the 10s throttle + event -- then chains the
//     original with ALL arguments unchanged. No managed invoke is performed, so the incoming receiver is
//     never relocated and needs no rooting/deferred release.
//
// NO hook collision: feature 46 does NOT hook any of these three addresses (it hooks only the reward/rating
// Initialize + bjan addresses). The coordinator skips the legacy routes.js Gum attaches ONLY after this
// adapter reports installed(). Built -fno-exceptions/-fno-rtti like every sibling; no C++ throw escapes.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "RoutesTracking native ABI requires LP64");

enum PgrRoutesTrackingInfo {
  PGR_RT_FEATURE_ID          = 50,
  PGR_RT_API_VERSION         = 1,
  PGR_RT_CONFIG_VERSION      = 1,
  PGR_RT_PTR_COUNT           = 5,     // config pointer slots 0..4, all mandatory (non-null)
  PGR_RT_WAYPOINT_LAT_OFFSET = 0x18,  // expected RouteWaypointProto.LatDegrees byte offset (fields.txt L501)
  PGR_RT_WAYPOINT_LNG_OFFSET = 0x20,  // expected RouteWaypointProto.LngDegrees byte offset (fields.txt L502)
  PGR_RT_MAX_FIELD_OFFSET    = 0x1000 // sanity ceiling on the validated field offsets
};

// install() returns 0 on success; negatives are faults. installed() returns 1/0.
enum PgrRoutesTrackingStatus {
  PGR_RT_OK               = 0,
  PGR_RT_FAULT            = -1,
  PGR_RT_ERR_NULL_ARG     = -2,
  PGR_RT_ERR_BAD_VERSION  = -3,
  PGR_RT_ERR_BAD_SIZE     = -4,
  PGR_RT_ERR_MISSING_PTR  = -5,  // a mandatory config pointer was null
  PGR_RT_ERR_BAD_OFFSET   = -6,  // lat/lng offset not 8-aligned, out of range, or equal
  PGR_RT_ERR_DUP_ADDR     = -7,  // two hook addresses coincide
  PGR_RT_ERR_RECONFIG     = -9,  // a different config at an already-installed table
  PGR_RT_ERR_HOOK         = -12, // a Dobby hook failed to install (rolled back, fail-closed)
  PGR_RT_ERR_NO_OBSERVERS = -13  // feature 46 routes_observers API unavailable / version mismatch
};

// Fixed LP64 config (64 bytes): 8-byte header + 5 pointers + 2 validated offsets + 2 reserved u32.
// Every pointer is MANDATORY (non-null). The host validates the hook addresses (checkPrefix +
// MethodInfo/resolver owner+executable checks) BEFORE calling install(); the native side never
// re-checks a patched prologue. install() publishes the config immutably: an identical config is
// idempotent (0); a different one is rejected (RECONFIG).
struct PgrRoutesTrackingConfig {
  uint32_t version;           // off 0  == PGR_RT_CONFIG_VERSION
  uint32_t size;              // off 4  == sizeof(PgrRoutesTrackingConfig)
  void *object_get_class;     // off 8  il2cpp_object_get_class(obj) -> Il2CppClass* (nonallocating)
  void *class_waypoint;       // off 16 Il2CppClass* Holoholo.Rpc.RouteWaypointProto (exact arg check)
  void *addr_tracking;        // off 24 live RoutesLocationService.CheckTrackingErrorStatus
  void *addr_waypoint;        // off 32 live RoutesLocationService.bjfz (waypoint add)
  void *addr_location_icall;  // off 40 resolved UnityEngine.LocationService::IsServiceEnabledByUser icall
  uint32_t lat_offset;        // off 48 RouteWaypointProto.LatDegrees byte offset (expected 0x18)
  uint32_t lng_offset;        // off 52 RouteWaypointProto.LngDegrees byte offset (expected 0x20)
  uint32_t reserved0;         // off 56 0 in v1
  uint32_t reserved1;         // off 60 0 in v1
};

static_assert(sizeof(PgrRoutesTrackingConfig) == 64, "PgrRoutesTrackingConfig must remain 64 bytes on LP64");
static_assert(offsetof(PgrRoutesTrackingConfig, object_get_class) == 8, "slot 0");
static_assert(offsetof(PgrRoutesTrackingConfig, addr_tracking) == 24, "slot 2");
static_assert(offsetof(PgrRoutesTrackingConfig, addr_location_icall) == 40, "slot 4");
static_assert(offsetof(PgrRoutesTrackingConfig, lat_offset) == 48, "lat offset field");
static_assert(offsetof(PgrRoutesTrackingConfig, lng_offset) == 52, "lng offset field");

// Fixed LP64 table: 8-byte header + 2 function pointers = 24 bytes.
struct PgrRoutesTrackingApi {
  uint32_t version;  // == PGR_RT_API_VERSION
  uint32_t size;     // == sizeof(PgrRoutesTrackingApi)
  // Verify + publish the immutable config and install the THREE Dobby hooks ONCE for the process
  // lifetime. Idempotent for an identical config; a different config returns PGR_RT_ERR_RECONFIG. A
  // partial hook install is rolled back and latched (PGR_RT_ERR_HOOK). Requires the feature-46 API.
  int (*install)(const PgrRoutesTrackingConfig *config);
  // 1 once install() has fully succeeded (the coordinator may now skip the legacy Gum attaches); else 0.
  int (*installed)(void);
};

static_assert(sizeof(PgrRoutesTrackingApi) == 24, "PgrRoutesTrackingApi must remain a 24-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 50 and answers only on an exact version match.
const void *pgrazor_routes_tracking_api(uint32_t version);

#ifdef __cplusplus
}
#endif

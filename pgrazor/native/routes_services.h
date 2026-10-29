#pragma once
#include <stddef.h>
#include <stdint.h>

// Feature 45 (API 1). Native ROUTES SERVICE CAPTURE: the C++ port of the service-capture hooks and the
// consumption dependency derivation in runtime/features/routes.js (the installCapture() CAPTURE loop and
// the tick RoutesService.dica derivation block). It installs TWO il2cpp hooks through the shared hook
// engine and OWNS the GC roots to the captured RoutesService / RouteConsumptionService. It does NOT port
// the reward/rating UI or tracking observers, the discovery read (feature 44) or the managed actions
// (feature 43); Java walking is untouched, and this module NEVER calls back into JS.
//
// Hooked methods (both take no managed argument besides the receiver; the existing validated CAPTURE
// addresses/prefixes in routes.js):
//   * RoutesService.RequestNearbyRoutes       -> captures the RoutesService receiver; the managed return
//     is a generic managed reference (kind 21, void*) chained through exactly once, returned verbatim.
//   * RouteConsumptionService.get_InRangeOfEnd -> captures the ConsumptionService receiver; returns a
//     System.Boolean chained through exactly once.
// StartRoute / CompleteRouteButtonClick capture is DROPPED: RoutesService.dica yields the authoritative
// consumption service and poll() re-derives it on a service replacement.
//
// Capture discipline: the hook persists a native root for the receiver BEFORE the original runs (the
// RequestNearbyRoutes return may relocate the collector), re-resolves the receiver from that root and
// passes the re-resolved pointer to the original; roots are NEVER freed on the hook path after the
// original. One bounded PENDING root per kind; only poll() commits pending->active and frees the previous
// active root. A hook off the owner thread / with an expired heartbeat / reentrant / with the wrong
// receiver class captures nothing and chains the original unchanged (never a fabricated return).
//
// Owner tid is latched once from a genuine on-thread poll/drain. poll() refreshes the capture heartbeat
// (CLOCK_BOOTTIME, clamped) and is independent of the gameRoutesAllowed planning lease. revoke() bumps the
// generation and clears the heartbeat from ANY thread with no managed work and no handle frees; a pending
// root stamped under the prior generation is dropped by the next poll, so capture cannot revive without a
// fresh owner heartbeat. drain() frees all roots on the owner thread (benign SKIP off-owner). Output
// carries ONLY borrowed gchandle ids + scalar bits; the host borrows the active handles (feature 43/44
// service handles) and must never free them.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "RoutesServices native ABI requires LP64");

enum PgrRoutesServicesInfo {
  PGR_RS_FEATURE_ID      = 45,
  PGR_RS_API_VERSION     = 1,
  PGR_RS_CONFIG_VERSION  = 1,
  PGR_RS_REQUEST_VERSION = 1,
  PGR_RS_OUTPUT_VERSION  = 1,
  PGR_RS_MAX_LEASE_MS    = 2500, // capture heartbeat ceiling (CLOCK_BOOTTIME)
  PGR_RS_PTR_COUNT       = 11    // config pointer slots 0..10, all mandatory (non-null)
};

// Entry-point return == out.status (poll): 1 OK / 0 SKIP (benign gate / not captured) / negative fault.
// install() returns 0 on success; negatives are faults.
enum PgrRoutesServicesStatus {
  PGR_RS_OK                = 1,
  PGR_RS_SKIP              = 0,
  PGR_RS_OK_INSTALL        = 0,   // install() success (alias of 0)
  PGR_RS_FAULT             = -1,
  PGR_RS_ERR_NULL_ARG      = -2,
  PGR_RS_ERR_BAD_VERSION   = -3,
  PGR_RS_ERR_BAD_SIZE      = -4,
  PGR_RS_ERR_MISSING_PTR   = -5,  // install(): a mandatory config pointer was null
  PGR_RS_ERR_NOT_INSTALLED = -6,
  PGR_RS_ERR_RECONFIG      = -9,  // install(): a different config at an already-installed table
  PGR_RS_ERR_CLOCK         = -10,
  PGR_RS_ERR_BUSY          = -11, // reentrant poll/drain refused (defensive)
  PGR_RS_ERR_HOOK          = -12  // a Dobby hook failed to install (rolled back, fail-closed)
};

// poll() out.flags.
enum PgrRoutesServicesFlags {
  PGR_RS_FLAG_ROUTES_PRESENT        = 1,
  PGR_RS_FLAG_CONSUMPTION_PRESENT   = 2,
  PGR_RS_FLAG_ROUTES_REPLACED       = 4,  // active routes swapped this poll
  PGR_RS_FLAG_CONSUMPTION_REPLACED  = 8,  // active consumption swapped this poll
  PGR_RS_FLAG_CONSUMPTION_FROM_DICA = 16  // active consumption is dica-derived (authoritative)
};

// Fixed LP64 config (96 bytes): 8-byte header + 11 pointers in the EXACT slot order below. Every pointer
// is an already-validated il2cpp export / Il2CppClass* / FieldInfo* / live code address packed by the host
// from routes.js metadata. All are MANDATORY (non-null). Reordering is forbidden. install() publishes the
// config immutably: an identical config is idempotent (0); a different one is rejected (RECONFIG).
struct PgrRoutesServicesConfig {
  uint32_t version;  // == PGR_RS_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrRoutesServicesConfig)

  void *object_get_class;      // 0  void*     (void* obj)
  void *field_get_value;       // 1  void      (void* obj, FieldInfo*, void* out)
  void *gchandle_new;          // 2  uintptr_t (void* obj, int pinned)
  void *gchandle_get_target;   // 3  void*     (uintptr_t handle)
  void *gchandle_free;         // 4  void      (uintptr_t handle)
  void *thread_current;        // 5  void*     (void) -> attached Unity thread or null
  void *class_routes;          // 6  Il2CppClass* RoutesService (exact)
  void *class_consumption;     // 7  Il2CppClass* RouteConsumptionService (exact)
  void *f_routes_dica;         // 8  FieldInfo*  RoutesService.dica -> RouteConsumptionService (reference)
  void *addr_request_nearby;   // 9  live code address of RoutesService.RequestNearbyRoutes
  void *addr_in_range_of_end;  // 10 live code address of RouteConsumptionService.get_InRangeOfEnd
};

static_assert(sizeof(PgrRoutesServicesConfig) == 96, "PgrRoutesServicesConfig must remain 96 bytes on LP64");
static_assert(offsetof(PgrRoutesServicesConfig, object_get_class) == 8, "config body start (slot 0)");
static_assert(offsetof(PgrRoutesServicesConfig, class_routes) == 56, "slot 6");            // 8 + 6*8
static_assert(offsetof(PgrRoutesServicesConfig, f_routes_dica) == 72, "slot 8");           // 8 + 8*8
static_assert(offsetof(PgrRoutesServicesConfig, addr_in_range_of_end) == 88, "slot 10");   // 8 + 10*8

// Fixed LP64 request (32 bytes) for poll / drain.
struct PgrRoutesServicesRequest {
  uint32_t version;     // off 0  == PGR_RS_REQUEST_VERSION
  uint32_t size;        // off 4  == sizeof(PgrRoutesServicesRequest)
  uint64_t tid;         // off 8  proven owner tid
  uint64_t residual_ms; // off 16 capture heartbeat window; clamped, 0 refuses (drain ignores the lease)
  uint64_t generation;  // off 24 revocation token (== generation() at stamp)
};

static_assert(sizeof(PgrRoutesServicesRequest) == 32, "PgrRoutesServicesRequest must remain 32 bytes on LP64");
static_assert(offsetof(PgrRoutesServicesRequest, tid) == 8, "request tid");
static_assert(offsetof(PgrRoutesServicesRequest, residual_ms) == 16, "request residual");
static_assert(offsetof(PgrRoutesServicesRequest, generation) == 24, "request generation");

// Fixed LP64 output (40 bytes) for poll. Fully zeroed on entry; header re-stamped.
struct PgrRoutesServicesOutput {
  uint32_t version;            // off 0  == PGR_RS_OUTPUT_VERSION
  uint32_t size;               // off 4  == sizeof(PgrRoutesServicesOutput)
  int32_t  status;             // off 8  == the poll() return
  uint32_t flags;              // off 12 PgrRoutesServicesFlags
  uint64_t generation;         // off 16 g_gen at publish
  uint64_t routes_handle;      // off 24 borrowed ACTIVE RoutesService gchandle id (0 if none)
  uint64_t consumption_handle; // off 32 borrowed ACTIVE ConsumptionService gchandle id (0 if none)
};

static_assert(sizeof(PgrRoutesServicesOutput) == 40, "PgrRoutesServicesOutput must remain 40 bytes on LP64");
static_assert(offsetof(PgrRoutesServicesOutput, status) == 8, "output status");
static_assert(offsetof(PgrRoutesServicesOutput, flags) == 12, "output flags");
static_assert(offsetof(PgrRoutesServicesOutput, generation) == 16, "output generation");
static_assert(offsetof(PgrRoutesServicesOutput, routes_handle) == 24, "output routes handle");
static_assert(offsetof(PgrRoutesServicesOutput, consumption_handle) == 32, "output consumption handle");

// Fixed LP64 table: 8-byte header + 5 function pointers = 48 bytes.
struct PgrRoutesServicesApi {
  uint32_t version;  // == PGR_RS_API_VERSION
  uint32_t size;     // == sizeof(PgrRoutesServicesApi)
  // Verify + publish the immutable config and install the two hooks ONCE for the process lifetime.
  // Idempotent for an identical config; a different config returns PGR_RS_ERR_RECONFIG. A null pointer /
  // bad header is rejected; a partial hook install is rolled back and latched (PGR_RS_ERR_HOOK).
  int (*install)(const PgrRoutesServicesConfig *config);
  // Current revocation generation (initially 1). The host stamps it onto each request.
  uint64_t (*generation)(void);
  // Atomically bump the generation and clear the heartbeat. Any thread; no managed work; frees no handle.
  uint64_t (*revoke)(void);
  // Owner-thread capture heartbeat + pending->active commit + dica derivation + borrowed-handle publish.
  int (*poll)(const PgrRoutesServicesRequest *request, PgrRoutesServicesOutput *out);
  // Owner-thread root drain: free ALL pending + active roots and clear the slots. Needs an attached owner
  // thread; no lease. An off-owner call is a benign SKIP (no frees).
  int (*drain)(const PgrRoutesServicesRequest *request);
};

static_assert(sizeof(PgrRoutesServicesApi) == 48, "PgrRoutesServicesApi must remain a 48-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 45 and answers only on an exact version match.
const void *pgrazor_routes_services_api(uint32_t version);

#ifdef __cplusplus
}
#endif

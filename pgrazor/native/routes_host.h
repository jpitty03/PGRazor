#pragma once
#include <stddef.h>
#include <stdint.h>

#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "unity_scheduler.h"

// Feature 60 (API 1). Native ROUTES PRODUCTION ORCHESTRATION HOST: the full compiled C++ replacement
// for the native-selected Routes controller in runtime/features/routes.js.
//
// C++ owns:
//   * Feature 53 (routes_metadata) initialization, preferring Feature 54 (runtime_bootstrap) bindings
//     when ready or explicit PgrRoutesHostConfig bindings when driven by the temporary JS scheduler or
//     isolated native test fixtures.
//   * Validated leaf configuration + installation for Feature 43 (routes_actions), Feature 44
//     (routes_discovery), Feature 45 (routes_services), Feature 46 (routes_observers), and Feature 50
//     (routes_tracking), plus Feature 27 (routes_gate) lease/sequence lifecycle.
//   * Command JSON parsing (100% parity with routes.js parseCommand), bounded snapshot shaping
//     (boundSnapshot: <=100 routes, 2..5000 finite points, 256-char names, 500,000-byte JSON budget),
//     status/discovery cadence, service capture + dica derivation, reward/rating UI dismissal and
//     completion correlation, and one-shot action execution (discover/start/complete/pause).
//   * Event & snapshot serialization into both a bounded 64-slot typed pull queue (for the thin JS
//     marshalling adapter) and optional direct Feature 52 (runtime_transport) emission (opcodes 7, 8,
//     9, 19), plus a Feature 48 (unity_scheduler) PgrUnityWork subscriber entry point.
//
// True feature IDs in pgrazor_native.cpp (query_feature):
//   27 = routes_gate, 43 = routes_actions, 44 = routes_discovery, 45 = routes_services,
//   46 = routes_observers, 47 = runtime_metadata, 48 = unity_scheduler, 50 = routes_tracking,
//   52 = runtime_transport, 53 = routes_metadata, 54 = runtime_bootstrap, 60 = routes_host.
//   (26/28/29 in query_feature are VGP modules.)

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "RoutesHost native ABI requires LP64");

enum PgrRoutesHostInfo {
  PGR_RH_FEATURE_ID           = 60,
  PGR_RH_API_VERSION          = 1,
  PGR_RH_CONFIG_VERSION       = 1,
  PGR_RH_OPTIONS_OUT_VERSION  = 1,
  PGR_RH_EVENT_VERSION        = 1,
  PGR_RH_SAMPLE_VERSION       = 1,
  PGR_RH_MAX_LEASE_MS         = 2500,
  PGR_RH_STATUS_CADENCE_MS    = 1000,
  PGR_RH_DISCOVER_CADENCE_MS  = 5000,
  PGR_RH_META_RETRY_MS        = 2000,
  PGR_RH_COMMAND_MAX_BYTES    = 4096,
  PGR_RH_ID_MAX_CHARS         = 128,
  PGR_RH_NAME_MAX_CHARS       = 256,
  PGR_RH_ROUTES_MAX           = 100,
  PGR_RH_POINTS_MAX           = 5000,
  PGR_RH_SNAPSHOT_BUDGET      = 500000,
  PGR_RH_EVENT_QUEUE_MAX      = 64
};

enum PgrRoutesHostStatus {
  PGR_RH_OK                   = 0,
  PGR_RH_EVENT_POPPED         = 1,
  PGR_RH_NOT_READY            = 1,
  PGR_RH_ERR_NULL_ARG         = -1,
  PGR_RH_ERR_BAD_VERSION      = -2,
  PGR_RH_ERR_BAD_SIZE         = -3,
  PGR_RH_ERR_MISSING_PTR      = -4,
  PGR_RH_ERR_BAD_TABLE        = -5,
  PGR_RH_ERR_NOT_CONFIGURED   = -6,
  PGR_RH_ERR_BUFFER_TOO_SMALL = -8,
  PGR_RH_ERR_RECONFIG         = -9,
  PGR_RH_ERR_CLOCK            = -10,
  PGR_RH_ERR_BUSY             = -11,
  PGR_RH_ERR_OFF_OWNER        = -12
};

enum PgrRoutesHostConfigFlags {
  PGR_RH_CFG_PREFER_BOOTSTRAP = 1u << 0, // prefer Feature 54 snapshot when ready
  PGR_RH_CFG_EMIT_TRANSPORT52 = 1u << 1, // emit events/snapshots directly to Feature 52
  PGR_RH_CFG_POLL_TRANSPORT52 = 1u << 2, // poll gameRoutesAllowed + gameRoutesCommand from Feature 52
  PGR_RH_CFG_SKIP_QUEUE       = 1u << 3, // do not enqueue into adapter ring when EMIT_TRANSPORT52 is set
  PGR_RH_CFG_FIXTURE_HOOKS    = 1u << 4  // fixture-only (requires seg_query != nullptr): override hook targets
};

enum PgrRoutesHostOptStatus {
  PGR_RH_OPT_OFF              = 0,
  PGR_RH_OPT_REJECTED         = 1,
  PGR_RH_OPT_ACCEPTED         = 2
};

enum PgrRoutesHostOptReason {
  PGR_RH_REASON_NONE          = 0,
  PGR_RH_REASON_NOT_ALLOWED   = 1, // "not-allowed"
  PGR_RH_REASON_NO_COMMAND    = 2, // "no-command"
  PGR_RH_REASON_BAD_JSON      = 3, // "bad-json"
  PGR_RH_REASON_BAD_SHAPE     = 4, // "bad-shape"
  PGR_RH_REASON_BAD_SEQ       = 5, // "bad-seq"
  PGR_RH_REASON_BAD_ACTION    = 6, // "bad-action"
  PGR_RH_REASON_BAD_ID        = 7, // "bad-id"
  PGR_RH_REASON_BAD_REVERSED  = 8, // "bad-reversed"
  PGR_RH_REASON_ID_REQUIRED   = 9, // "id-required"
  PGR_RH_REASON_STALE_SEQ     = 10 // "stale-seq"
};

enum PgrRoutesHostEventKind {
  PGR_RH_EV_NONE                   = 0,
  PGR_RH_EV_READY                  = 1,  // "routes-ready"
  PGR_RH_EV_METADATA_READY         = 2,  // "routes-metadata-ready"
  PGR_RH_EV_LOCATION_GATE_READY    = 3,  // "routes-location-gate-ready"
  PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE = 4,  // "routes-diagnostic-unavailable"
  PGR_RH_EV_SERVICE_CAPTURED       = 5,  // "routes-service-captured"
  PGR_RH_EV_TRACKING_STATUS        = 6,  // "routes-tracking-status"
  PGR_RH_EV_WAYPOINT               = 7,  // "routes-waypoint"
  PGR_RH_EV_REWARD_CONFIRMED       = 8,  // "routes-reward-confirmed"
  PGR_RH_EV_REWARD_REJECTED        = 9,  // "routes-reward-rejected"
  PGR_RH_EV_RESULT_DISMISSED       = 10, // "routes-result-dismissed"
  PGR_RH_EV_RESULT_DISMISS_ERROR   = 11, // "routes-result-dismiss-error"
  PGR_RH_EV_CURRENT                = 12, // "routes-current"
  PGR_RH_EV_SNAPSHOT               = 13, // "routes-snapshot"
  PGR_RH_EV_READ_ERROR             = 14, // "routes-read-error"
  PGR_RH_EV_ACTION                 = 15  // "routes-action"
};

// Fixed LP64 config (264 bytes). Passing nullptr to configure() selects pure Feature 54 bootstrap mode.
struct PgrRoutesHostConfig {
  uint32_t version;                   // off 0   == PGR_RH_CONFIG_VERSION
  uint32_t size;                      // off 4   == sizeof(PgrRoutesHostConfig)
  uint32_t flags;                     // off 8   PgrRoutesHostConfigFlags
  uint32_t reserved0;                 // off 12  == 0
  const void *module_base;            // off 16  libil2cpp.so base (optional if bootstrap54 ready)
  uint64_t module_size;               // off 24  libil2cpp.so byte size
  const PgrMetaExports *meta_exports; // off 32  31 metadata exports for 47/53
  pgr_meta_seg_fn seg_query;          // off 40  optional fixture segment callback (nullptr in prod)
  void *seg_ctx;                      // off 48  optional fixture segment context

  // Runtime + extra exports used to populate leaves 43/44/45/46/50 when not resolved via bootstrap54:
  void *runtime_invoke;               // off 56
  void *object_get_class;             // off 64
  void *field_get_value;              // off 72
  void *field_set_value;              // off 80
  void *field_set_value_object;       // off 88
  void *object_unbox;                 // off 96
  void *object_new;                   // off 104
  void *gchandle_new;                 // off 112
  void *gchandle_get_target;          // off 120
  void *gchandle_free;                // off 128
  void *thread_current;               // off 136
  void *string_length;                // off 144
  void *string_chars;                 // off 152
  void *type_get_type;                // off 160
  void *type_is_byref;                // off 168
  void *resolve_icall;                // off 176

  // Optional fixture hook code overrides (used ONLY when seg_query != nullptr && PGR_RH_CFG_FIXTURE_HOOKS):
  void *fixture_request_nearby;       // off 184
  void *fixture_in_range_of_end;      // off 192
  void *fixture_reward_init;          // off 200
  void *fixture_rating_init;          // off 208
  void *fixture_completion_resp;      // off 216
  void *fixture_tracking_status;      // off 224
  void *fixture_tracking_waypoint;    // off 232
  void *fixture_location_icall;       // off 240
  const void *unity_module_base;      // off 248 optional fixture libunity.so base
  uint64_t unity_module_size;         // off 256 optional fixture libunity.so size
};

static_assert(sizeof(PgrRoutesHostConfig) == 264, "PgrRoutesHostConfig must remain 264 bytes on LP64");
static_assert(offsetof(PgrRoutesHostConfig, module_base) == 16, "cfg module_base");
static_assert(offsetof(PgrRoutesHostConfig, runtime_invoke) == 56, "cfg runtime_invoke");
static_assert(offsetof(PgrRoutesHostConfig, resolve_icall) == 176, "cfg resolve_icall");
static_assert(offsetof(PgrRoutesHostConfig, fixture_request_nearby) == 184, "cfg fixture_request_nearby");
static_assert(offsetof(PgrRoutesHostConfig, unity_module_size) == 256, "cfg unity_module_size");

// Fixed LP64 set_options output (96 bytes).
struct PgrRoutesHostOptionsOut {
  uint32_t version;     // off 0   == PGR_RH_OPTIONS_OUT_VERSION
  uint32_t size;        // off 4   == sizeof(PgrRoutesHostOptionsOut)
  int32_t  status_code; // off 8   PgrRoutesHostOptStatus (0 off, 1 rejected, 2 accepted)
  int32_t  reason_code; // off 12  PgrRoutesHostOptReason
  uint32_t action_code; // off 16  0 none, 1 discover, 2 start, 3 complete, 4 pause
  uint32_t reserved0;   // off 20  == 0
  uint64_t seq;         // off 24  accepted command seq, else 0
  char     reason[64];  // off 32  NUL-terminated rejection reason
};

static_assert(sizeof(PgrRoutesHostOptionsOut) == 96, "PgrRoutesHostOptionsOut must remain 96 bytes on LP64");
static_assert(offsetof(PgrRoutesHostOptionsOut, seq) == 24, "opt out seq");
static_assert(offsetof(PgrRoutesHostOptionsOut, reason) == 32, "opt out reason");

// Fixed LP64 drained event descriptor (64 bytes).
struct PgrRoutesHostEvent {
  uint32_t version;      // off 0   == PGR_RH_EVENT_VERSION
  uint32_t size;         // off 4   == sizeof(PgrRoutesHostEvent)
  uint32_t kind;         // off 8   PgrRoutesHostEventKind
  uint32_t json_len;     // off 12  UTF-8 JSON payload byte length (excluding NUL)
  uint64_t seq;          // off 16  command or reward seq when applicable
  int64_t  scalar0;      // off 24  available(0/1), tracking code, reject status, or screen index
  double   lat;          // off 32  waypoint lat
  double   lon;          // off 40  waypoint lon
  char     kind_tag[16]; // off 48  NUL-terminated suffix after "routes-" (e.g. "ready", "snapshot")
};

static_assert(sizeof(PgrRoutesHostEvent) == 64, "PgrRoutesHostEvent must remain 64 bytes on LP64");
static_assert(offsetof(PgrRoutesHostEvent, seq) == 16, "event seq");
static_assert(offsetof(PgrRoutesHostEvent, lat) == 32, "event lat");
static_assert(offsetof(PgrRoutesHostEvent, kind_tag) == 48, "event kind_tag");

// Fixed LP64 telemetry sample (64 bytes).
struct PgrRoutesHostSample {
  uint32_t version;            // off 0   == PGR_RH_SAMPLE_VERSION
  uint32_t size;               // off 4   == sizeof(PgrRoutesHostSample)
  uint32_t configured;         // off 8   0/1
  uint32_t meta_ready;         // off 12  0/1
  uint32_t services_ready;     // off 16  0/1 (both routes + consumption active)
  uint32_t tracking_ready;     // off 20  0/1 (feature 50 installed)
  uint32_t lease_active;       // off 24  0/1
  uint32_t queued_events;      // off 28  events currently in pull queue
  uint64_t owner_tid;          // off 32  latched Unity owner tid
  uint64_t last_seq;           // off 40  gate sequence high-water
  uint64_t ticks_run;          // off 48  total owner ticks executed
  int32_t  last_error;         // off 56  last negative status or metadata error
  uint32_t cached_route_count; // off 60  routes in last bounded snapshot
};

static_assert(sizeof(PgrRoutesHostSample) == 64, "PgrRoutesHostSample must remain 64 bytes on LP64");
static_assert(offsetof(PgrRoutesHostSample, owner_tid) == 32, "sample owner_tid");
static_assert(offsetof(PgrRoutesHostSample, last_error) == 56, "sample last_error");

// Fixed LP64 API table (64 bytes): 8-byte header + 7 function pointers.
struct PgrRoutesHostApi {
  uint32_t version; // == PGR_RH_API_VERSION
  uint32_t size;    // == sizeof(PgrRoutesHostApi)
  // Configure the host (nullptr selects pure Bootstrap 54 mode). Idempotent for identical config.
  int (*configure)(const PgrRoutesHostConfig *config);
  // Parse command_json + update foreground lease and gate (any thread; no managed calls).
  int (*set_options)(int allowed, const char *command_json, uint32_t command_len, PgrRoutesHostOptionsOut *out);
  // Run one orchestration tick on the proven Unity owner thread (tid == gettid()).
  int (*tick)(uint64_t tid);
  // Pop the oldest queued event. Returns 1 when popped, 0 when empty, PGR_RH_ERR_BUFFER_TOO_SMALL (-8)
  // if json_buf != nullptr and json_cap < json_len (leaving the event queued and setting out->json_len).
  int (*next_event)(PgrRoutesHostEvent *out, char *json_buf, uint32_t json_cap);
  // Revoke all leaf generations and (when on the owner thread) drain all leaf roots and clear state.
  int (*stop)(uint64_t tid);
  // Read pull-only telemetry snapshot (any thread).
  int (*sample)(PgrRoutesHostSample *out);
  // Feature 48 (unity_scheduler) PgrUnityWork subscriber callback entry point.
  void (*on_unity_pulse)(void *context, const PgrUnityPulse *pulse);
};

static_assert(sizeof(PgrRoutesHostApi) == 64, "PgrRoutesHostApi must remain a 64-byte version-1 table");

const void *pgrazor_routes_host_api(uint32_t version);

#ifdef __cplusplus
}
#endif

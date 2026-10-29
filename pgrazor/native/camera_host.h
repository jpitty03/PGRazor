#ifndef PGRAZOR_NATIVE_CAMERA_HOST_H_
#define PGRAZOR_NATIVE_CAMERA_HOST_H_

#include <stddef.h>
#include <stdint.h>

#include "camera_metadata.h"
#include "runtime_metadata.h"
#include "unity_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  PGR_CH_FEATURE_ID = 64u,
  PGR_CH_API_VERSION = 1u,

  PGR_CH_ATTEMPTS_MAX = 60u,
  PGR_CH_RETRY_MS = 1000u,
  PGR_CH_PREPARE_MS = 60000u,
  PGR_CH_ASM_LIMIT = 128u,
  PGR_CH_SCAN_MS = 3000u,
  PGR_CH_NAME_MAX = 256u,
  PGR_CH_OBSERVE_MS = 12000u,
  PGR_CH_POLL_MS = 250u,
  PGR_CH_THREADS_MAX = 8u,
  PGR_CH_CALLS_MAX = 200000u,
  PGR_CH_SAMPLE_MS = 10000u,
  PGR_CH_SAMPLE_INTERVAL_MS = 500u,
  PGR_CH_SAMPLES_MAX = 20u,

  PGR_CH_CONTINUOUS_INTERVAL_MS = 100u,
  PGR_CH_CONTINUOUS_LEASE_MS = 250u,
  PGR_CH_HEADING_LOG_INTERVAL_MS = 1000u,
  PGR_CH_MIN_MATCHES = 3u,
  PGR_CH_EVENT_QUEUE_MAX = 64u
};

enum PgrCameraHostResult {
  PGR_CH_OK = 0,
  PGR_CH_HAS_EVENT = 1,
  PGR_CH_RETRY = 2,
  PGR_CH_UNAVAILABLE = 3,
  PGR_CH_ERR_INVALID_ARG = -1,
  PGR_CH_ERR_VERSION = -2,
  PGR_CH_ERR_MISSING_EXPORT = -3,
  PGR_CH_ERR_NOT_CONFIGURED = -4,
  PGR_CH_ERR_HOOK_FAILED = -5,
  PGR_CH_ERR_INTERNAL = -6,
  PGR_CH_ERR_ALREADY_RUNNING = -7,
  PGR_CH_ERR_RESTART_REQUIRED = -8
};

enum PgrCameraHostConfigFlags {
  PGR_CH_FLAG_NONE = 0u,
  PGR_CH_FLAG_USE_TRANSPORT = 1u << 0,
  PGR_CH_FLAG_USE_BOOTSTRAP = 1u << 1,
  PGR_CH_FLAG_FIXTURE_HOOK_OVERRIDE = 1u << 2,
  PGR_CH_FLAG_ALLOW_IN_EXECUTE = 1u << 3,
  PGR_CH_FLAG_REQUIRE_IN_EXECUTE = 1u << 4,
  PGR_CH_FLAG_LOCAL_CONTINUOUS_SAMPLER = 1u << 5
};

enum PgrCameraHostOptionFlags {
  PGR_CH_OPT_NONE = 0u,
  PGR_CH_OPT_FORCE_POLL = 1u << 0,
  PGR_CH_OPT_CLEAR_LEASE = 1u << 1,
  PGR_CH_OPT_REQUEST_FAILED = 1u << 2
};

enum PgrCameraHostEventKind {
  PGR_CH_EVENT_NONE = 0u,
  PGR_CH_EVENT_SIGNATURE = 1u,
  PGR_CH_EVENT_READY = 2u,
  PGR_CH_EVENT_CAMERA_SAMPLE = 3u,
  PGR_CH_EVENT_CAMERA_UNAVAILABLE = 4u,
  PGR_CH_EVENT_SAMPLER_ERROR = 5u,
  PGR_CH_EVENT_THREAD_SUMMARY = 6u,
  PGR_CH_EVENT_FINISHED = 7u,
  PGR_CH_EVENT_SAMPLER_READY = 8u
};

enum PgrCameraHostEventFlags {
  PGR_CH_EVFLAG_NONE = 0u,
  PGR_CH_EVFLAG_MAP_VISIBLE = 1u << 0,
  PGR_CH_EVFLAG_STATUS_COMPLETE = 1u << 1
};

typedef double (*PgrCameraHostClockFn)(void *ctx);
typedef uint64_t (*PgrCameraHostTidFn)(void *ctx);

typedef struct PgrCameraHostConfig {
  uint32_t version;
  uint32_t size;
  uint32_t flags;
  int32_t expected_layer;
  const void *module_base;
  uint64_t module_size;
  const PgrMetaExports *meta_exports;
  const PgrCameraMetaExtra *meta_extra;
  pgr_meta_seg_fn seg_query;
  void *seg_ctx;
  const void *domain;
  void *thread_current;
  void *thread_attach;
  void *thread_detach;
  PgrCameraHostClockFn clock_fn;
  void *clock_ctx;
  PgrCameraHostTidFn tid_fn;
  void *tid_ctx;
  void *fixture_getter_code;
  uint64_t reserved0;
} PgrCameraHostConfig;

typedef struct PgrCameraHostOptions {
  uint32_t version;
  uint32_t size;
  uint8_t enabled;
  uint8_t sample_camera;
  uint8_t continuous;
  uint8_t sample_requested;
  uint32_t lease_ms;
  uint64_t now_ms;
  uint32_t flags;
  uint32_t reserved;
} PgrCameraHostOptions;

typedef struct PgrCameraHostEvent {
  uint32_t version;
  uint32_t size;
  uint32_t kind;
  uint32_t flags;
  uint32_t sample_index;
  int32_t camera_id;
  uint32_t culling_mask;
  int32_t world_map_layer;
  uint64_t tid;
  double time_ms;
  double age_ms;
  double x;
  double y;
  double z;
  double yaw_degrees;
  char kind_name[32];
  char reason[64];
  char status[32];
  char json[1576];
} PgrCameraHostEvent;

typedef struct PgrCameraHostSample {
  uint32_t version;
  uint32_t size;
  uint8_t active;
  uint8_t ready;
  uint8_t stopping;
  uint8_t sample_camera;
  uint8_t continuous;
  uint8_t overflow;
  uint8_t unavailable_reported;
  uint8_t hook_installed;
  uint32_t vector_fields;
  uint32_t attempts;
  uint32_t calls;
  uint32_t raw_matches;
  uint32_t matches;
  uint32_t threads_count;
  uint32_t active_matches;
  uint32_t sample_count;
  uint32_t sample_attempts;
  uint32_t queued_events;
  uint64_t owner_tid;
  uint64_t execute_tasks_method;
  uint64_t get_main_camera_address;
  uint64_t remaining_lease_ms;
  int32_t last_error;
  int32_t reserved;
  char reason[64];
  char failure_stage[64];
  char failure_name[64];
  char failure_message[264];
} PgrCameraHostSample;

typedef struct PgrCameraHostApi {
  uint32_t version;
  uint32_t size;
  int (*configure)(const PgrCameraHostConfig *config);
  int (*start)(const PgrCameraHostOptions *options);
  int (*set_options)(const PgrCameraHostOptions *options);
  int (*tick)(uint64_t tid);
  int (*on_execute_enter)(uint64_t tid, uint32_t is_execute_tasks);
  int (*on_execute_leave)(uint64_t tid, uint32_t is_execute_tasks);
  int (*on_main_camera_getter)(void *rig, void *returned_camera, uint64_t tid);
  int (*next_event)(PgrCameraHostEvent *out_event);
  int (*stop)(const char *reason);
  int (*sample)(PgrCameraHostSample *out_sample);
  void (*on_unity_begin)(void *ctx, const PgrUnityPulse *pulse);
  void (*on_unity_end)(void *ctx, const PgrUnityPulse *pulse);
  void (*bind_core_handle)(void *lib_handle);
} PgrCameraHostApi;

#if defined(__cplusplus) && (defined(__LP64__) || defined(_LP64) || defined(__aarch64__) || defined(__x86_64__))
static_assert(sizeof(PgrCameraHostConfig) == 144, "PgrCameraHostConfig size");
static_assert(offsetof(PgrCameraHostConfig, module_base) == 16, "module_base offset");
static_assert(offsetof(PgrCameraHostConfig, meta_exports) == 32, "meta_exports offset");
static_assert(offsetof(PgrCameraHostConfig, meta_extra) == 40, "meta_extra offset");
static_assert(offsetof(PgrCameraHostConfig, seg_query) == 48, "seg_query offset");
static_assert(offsetof(PgrCameraHostConfig, domain) == 64, "domain offset");
static_assert(offsetof(PgrCameraHostConfig, thread_current) == 72, "thread_current offset");
static_assert(offsetof(PgrCameraHostConfig, clock_fn) == 96, "clock_fn offset");
static_assert(offsetof(PgrCameraHostConfig, tid_fn) == 112, "tid_fn offset");
static_assert(offsetof(PgrCameraHostConfig, fixture_getter_code) == 128, "fixture_getter_code offset");

static_assert(sizeof(PgrCameraHostOptions) == 32, "PgrCameraHostOptions size");
static_assert(offsetof(PgrCameraHostOptions, enabled) == 8, "enabled offset");
static_assert(offsetof(PgrCameraHostOptions, lease_ms) == 12, "lease_ms offset");
static_assert(offsetof(PgrCameraHostOptions, now_ms) == 16, "now_ms offset");

static_assert(sizeof(PgrCameraHostEvent) == 1792, "PgrCameraHostEvent size");
static_assert(offsetof(PgrCameraHostEvent, tid) == 32, "tid offset");
static_assert(offsetof(PgrCameraHostEvent, x) == 56, "x offset");
static_assert(offsetof(PgrCameraHostEvent, yaw_degrees) == 80, "yaw_degrees offset");
static_assert(offsetof(PgrCameraHostEvent, kind_name) == 88, "kind_name offset");
static_assert(offsetof(PgrCameraHostEvent, reason) == 120, "reason offset");
static_assert(offsetof(PgrCameraHostEvent, status) == 184, "status offset");
static_assert(offsetof(PgrCameraHostEvent, json) == 216, "json offset");

static_assert(sizeof(PgrCameraHostSample) == 552, "PgrCameraHostSample size");
static_assert(offsetof(PgrCameraHostSample, vector_fields) == 16, "vector_fields offset");
static_assert(offsetof(PgrCameraHostSample, owner_tid) == 56, "owner_tid offset");
static_assert(offsetof(PgrCameraHostSample, last_error) == 88, "last_error offset");
static_assert(offsetof(PgrCameraHostSample, reason) == 96, "reason offset");
static_assert(offsetof(PgrCameraHostSample, failure_stage) == 160, "failure_stage offset");
static_assert(offsetof(PgrCameraHostSample, failure_name) == 224, "failure_name offset");
static_assert(offsetof(PgrCameraHostSample, failure_message) == 288, "failure_message offset");

static_assert(sizeof(PgrCameraHostApi) == 112, "PgrCameraHostApi size");
#endif

const void *pgrazor_camera_host_api(uint32_t version);

#ifdef __cplusplus
}
#endif

#endif  // PGRAZOR_NATIVE_CAMERA_HOST_H_

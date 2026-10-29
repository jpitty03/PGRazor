#include "camera_host.h"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <mutex>

#include "camera_metadata.h"
#include "camera_sampler.h"
#include "hook_engine.h"
#include "pgrazor_native_api.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"

namespace {

using namespace pgr_camera_schema;

typedef void *(*Il2CppDomainGetFn)(void);
typedef void *(*Il2CppThreadCurrentFn)(void);
typedef void *(*Il2CppThreadAttachFn)(const void *domain);
typedef void (*Il2CppThreadDetachFn)(void *thread);
typedef void *(*Il2CppRuntimeInvokeFn)(const void *method, void *obj,
                                       void **params, void **exc);
typedef void *(*Il2CppObjectGetClassFn)(void *obj);
typedef void *(*Il2CppObjectUnboxFn)(void *obj);
typedef uintptr_t (*Il2CppGcHandleNewFn)(void *obj, int32_t pinned);
typedef void *(*Il2CppGcHandleGetTargetFn)(uintptr_t gchandle);
typedef void (*Il2CppGcHandleFreeFn)(uintptr_t gchandle);
typedef void (*Il2CppFieldGetValueFn)(void *obj, const void *field,
                                      void *out_value);
typedef bool (*Il2CppClassIsAssignableFromFn)(const void *klass,
                                              const void *oklass);
typedef void *(*Il2CppStringNewFn)(const char *str);
typedef void *(*MainCameraGetterFn)(void *rig,const void *method_info);

enum {
  kTransportBoolBuddySampling = 9u,
  kTransportBoolCameraSteering = 10u,
  kTransportPulseCameraSample = 0u,

  kActionCameraDiagnostic = 11u,
  kActionEvent = 19u,
  kActionInvalidateCameraHeading = 21u,
  kActionSetCameraDiagnosticRunning = 29u,
  kActionUpdateCameraHeadingAt = 57u
};

constexpr double kMaxSafeInt = 9007199254740991.0;
constexpr double kPi = 3.14159265358979323846;

struct ThreadEntry {
  uint64_t tid;
  uint32_t count;
  uint32_t depth;
};

struct SampleRecord {
  bool valid;
  uint32_t sample_index;
  double time_ms;
  uint64_t tid;
  int32_t camera_id;
  uint32_t culling_mask;
  int32_t world_map_layer;
  bool map_visible;
  double x;
  double y;
  double z;
  double yaw_degrees;
};

struct UnavailableRecord {
  bool valid;
  double time_ms;
  uint64_t tid;
  uint32_t culling_mask;
  int32_t world_map_layer;
  bool has_mask;
  char reason[64];
};

struct FailureRecord {
  bool valid;
  uint64_t tid;
  uint32_t sample_attempt;
  char stage[64];
  char error_name[64];
  char message[264];
};

struct HostState {
  std::atomic_flag busy = ATOMIC_FLAG_INIT;
  std::atomic<bool> stop_requested{false};
  void *core_handle = nullptr;

  bool configured = false;
  PgrCameraHostConfig cfg = {};
  PgrMetaExports boot_meta_exports = {};
  PgrCameraMetaExtra boot_meta_extra = {};

  bool active = false;
  bool ready = false;
  bool stopping = false;
  bool pending_finish = false;
  char pending_finish_status[32] = {};
  char pending_finish_reason[64] = {};
  char pending_finish_message[264] = {};
  bool pending_finish_with_summary = false;
  bool pending_finish_with_attempts = false;

  bool sample_camera = false;
  bool continuous = false;
  bool continuous_ever_started = false;
  bool overflow = false;
  bool unavailable_reported = false;
  bool hook_installed = false;
  bool sampler_installed = false;
  bool use_local_continuous = false;
  bool sample_done = false;
  bool has_sampled = false;
  bool has_sample_started = false;
  double sample_started_at_ms = 0.0;
  bool has_heading_log = false;
  bool in_subscriber_pulse = false;
  bool explicit_execute_tracking = false;

  std::atomic<int32_t> sampling{0};
  uint64_t sampling_tid = 0;
  std::atomic<int32_t> getter_depth{0};

  uint32_t attempts = 0;
  uint32_t calls = 0;
  uint32_t raw_matches = 0;
  uint32_t matches = 0;
  uint32_t threads_count = 0;
  ThreadEntry threads[PGR_CH_THREADS_MAX] = {};
  uint64_t owner_tid = 0;

  uint32_t sample_count = 0;
  uint32_t sample_attempts = 0;
  int32_t last_error = PGR_CH_OK;
  char reason[64] = {};
  char sample_error[64] = {};
  FailureRecord sample_failure = {};

  double started_at_ms = 0.0;
  double observed_at_ms = 0.0;
  double last_prepare_at_ms = 0.0;
  bool has_prepared = false;
  double last_poll_at_ms = 0.0;
  double last_sample_at_ms = 0.0;
  double last_heading_log_ms = 0.0;
  double lease_until_ms = 0.0;
  bool has_last_clock = false;
  double last_clock_ms = 0.0;
  int32_t local_world_map_layer = -1;

  PgrCameraMetaOutput meta = {};
  void *hooked_getter_target = nullptr;
  std::atomic<MainCameraGetterFn> orig_getter{nullptr};
  bool hook_poisoned = false;
  uint64_t transport_sequence = 0, transport_generation = 0;
  bool transport_boolean_demand = false;

  SampleRecord latest_sample = {};
  UnavailableRecord pending_unavailable = {};
  SampleRecord bounded_queue[PGR_CH_SAMPLES_MAX] = {};
  uint32_t bounded_head = 0;
  uint32_t bounded_count = 0;
  char last_bounded_reading[160] = {};

  PgrCameraHostEvent events[PGR_CH_EVENT_QUEUE_MAX] = {};
  uint32_t event_head = 0;
  uint32_t event_count = 0;
};

HostState g_state;
thread_local int32_t tls_getter_depth = 0;
thread_local unsigned state_depth=0;
// No thread waits while managed code runs. Reentrant owner work can revoke;
// callbacks on other threads fail promptly, avoiding cross-thread deadlock.
struct StateGuard {
 bool entered=false;
 StateGuard(){if(state_depth){++state_depth;entered=true;}else if(!g_state.busy.test_and_set(std::memory_order_acquire)){state_depth=1;entered=true;}}
 ~StateGuard(){if(entered&&!--state_depth)g_state.busy.clear(std::memory_order_release);}
};
bool revoked(){return g_state.stopping||g_state.stop_requested.load(std::memory_order_acquire);}


#if defined(PGR_CAMERA_HOST_STANDALONE_TEST)
typedef int (*StandaloneHookFn)(void *target, void *replacement,
                                void **original);
typedef int (*StandaloneUnhookFn)(void *target);
StandaloneHookFn g_standalone_hook = nullptr;
StandaloneUnhookFn g_standalone_unhook = nullptr;
#endif

void copy_str(char *dst, size_t cap, const char *src) {
  if (!dst || cap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t len = strlen(src);
  if (len >= cap) len = cap - 1;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

const pgrazor_native_api_v2 *resolve_native_api() {
  if (g_state.core_handle != nullptr) {
    typedef const void *(*NativeApiFn)(uint32_t);
    auto fn = reinterpret_cast<NativeApiFn>(
        dlsym(g_state.core_handle, "pgrazor_native_api"));
    if (fn != nullptr) {
      return static_cast<const pgrazor_native_api_v2 *>(fn(2u));
    }
  }
#if !defined(PGR_CAMERA_HOST_STANDALONE_TEST)
  return static_cast<const pgrazor_native_api_v2 *>(pgrazor_native_api(2u));
#else
  return nullptr;
#endif
}

const PgrCameraMetaApi *resolve_metadata_api() {
#if !defined(PGR_CAMERA_HOST_STANDALONE_TEST)
  const void *direct = pgrazor_camera_metadata_api(PGR_CAM_API_VERSION);
  if (direct != nullptr) return static_cast<const PgrCameraMetaApi *>(direct);
#endif
  const pgrazor_native_api_v2 *native = resolve_native_api();
  if (native != nullptr && native->query_feature != nullptr) {
    return static_cast<const PgrCameraMetaApi *>(
        native->query_feature(PGR_CAM_FEATURE_ID, PGR_CAM_API_VERSION));
  }
  return nullptr;
}

const PgrCameraSamplerApi *resolve_sampler_api() {
#if !defined(PGR_CAMERA_HOST_STANDALONE_TEST)
  const void *direct = pgrazor_camera_sampler_api(PGR_CS_API_VERSION);
  if (direct != nullptr) {
    return static_cast<const PgrCameraSamplerApi *>(direct);
  }
#endif
  const pgrazor_native_api_v2 *native = resolve_native_api();
  if (native != nullptr && native->query_feature != nullptr) {
    return static_cast<const PgrCameraSamplerApi *>(
        native->query_feature(PGR_CS_FEATURE_ID, PGR_CS_API_VERSION));
  }
  return nullptr;
}

const PgrRuntimeTransportApi *resolve_transport_api() {
#if !defined(PGR_CAMERA_HOST_STANDALONE_TEST)
  const void *direct =
      pgrazor_runtime_transport_api(PGR_TRANSPORT_API_VERSION);
  if (direct != nullptr) {
    return static_cast<const PgrRuntimeTransportApi *>(direct);
  }
#endif
  const pgrazor_native_api_v2 *native = resolve_native_api();
  if (native != nullptr && native->query_feature != nullptr) {
    return static_cast<const PgrRuntimeTransportApi *>(native->query_feature(
        PGR_TRANSPORT_FEATURE_ID, PGR_TRANSPORT_API_VERSION));
  }
  return nullptr;
}

const PgrBootApi *resolve_bootstrap_api() {
#if !defined(PGR_CAMERA_HOST_STANDALONE_TEST)
  const void *direct = pgrazor_runtime_bootstrap_api(PGR_BOOT_API_VERSION);
  if (direct != nullptr) return static_cast<const PgrBootApi *>(direct);
#endif
  const pgrazor_native_api_v2 *native = resolve_native_api();
  if (native != nullptr && native->query_feature != nullptr) {
    return static_cast<const PgrBootApi *>(
        native->query_feature(PGR_BOOT_FEATURE_ID, PGR_BOOT_API_VERSION));
  }
  return nullptr;
}

int call_hook(void *target, void *replacement, void **original) {
#if !defined(PGR_CAMERA_HOST_STANDALONE_TEST)
  return pgrazor_hook(target, replacement, original);
#else
  if (g_standalone_hook != nullptr) {
    return g_standalone_hook(target, replacement, original);
  }
  if (g_state.core_handle != nullptr) {
    auto fn = reinterpret_cast<StandaloneHookFn>(
        dlsym(g_state.core_handle, "pgrazor_hook"));
    if (fn != nullptr) return fn(target, replacement, original);
  }
  return -1;
#endif
}


uint64_t current_tid() {
  if (g_state.cfg.tid_fn != nullptr) {
    return g_state.cfg.tid_fn(g_state.cfg.tid_ctx);
  }
#if defined(__linux__)
  return static_cast<uint64_t>(static_cast<pid_t>(syscall(SYS_gettid)));
#else
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pthread_self()));
#endif
}

bool read_raw_clock_ms(double *out_ms, bool *threw_out = nullptr) {
  if (threw_out) *threw_out = false;
  if (g_state.cfg.clock_fn != nullptr) {
#if defined(__EXCEPTIONS)
    try {
      *out_ms = g_state.cfg.clock_fn(g_state.cfg.clock_ctx);
      return true;
    } catch (...) {
      if (threw_out) *threw_out = true;
      return false;
    }
#else
    *out_ms = g_state.cfg.clock_fn(g_state.cfg.clock_ctx);
    return true;
#endif
  }
  struct timespec ts = {};
#ifdef CLOCK_BOOTTIME
  if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0 && ts.tv_sec >= 0 &&
      ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000L) {
    *out_ms = static_cast<double>(ts.tv_sec) * 1000.0 +
              static_cast<double>(ts.tv_nsec) / 1000000.0;
    return true;
  }
#endif
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0 && ts.tv_sec >= 0 &&
      ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000L) {
    *out_ms = static_cast<double>(ts.tv_sec) * 1000.0 +
              static_cast<double>(ts.tv_nsec) / 1000000.0;
    return true;
  }
  return false;
}

bool sample_validated_clock(double *out_ms) {
  bool threw = false;
  double v = 0.0;
  if (!read_raw_clock_ms(&v, &threw)) {
    if (g_state.sample_error[0] == '\0') {
      copy_str(g_state.sample_error, sizeof(g_state.sample_error),
               threw ? "boot-clock-read-failed" : "invalid-boot-clock");
    }
    return false;
  }
  if (g_state.continuous) {
    if (!std::isfinite(v) || v < 0.0 || v > kMaxSafeInt ||
        (g_state.has_last_clock && v < g_state.last_clock_ms)) {
      if (g_state.sample_error[0] == '\0') {
        copy_str(g_state.sample_error, sizeof(g_state.sample_error),
                 "invalid-boot-clock");
      }
      return false;
    }
  }
  g_state.has_last_clock = true;
  g_state.last_clock_ms = v;
  *out_ms = v;
  return true;
}

double fallback_now_ms() {
  double v = 0.0;
  if (read_raw_clock_ms(&v, nullptr) && std::isfinite(v) && v >= 0.0) {
    return v;
  }
  return g_state.last_clock_ms;
}

uint64_t remaining_lease_ms(double now) {
  if (!std::isfinite(now) || g_state.lease_until_ms <= now) return 0;
  double diff = g_state.lease_until_ms - now;
  if (diff > static_cast<double>(PGR_CH_CONTINUOUS_LEASE_MS)) {
    return PGR_CH_CONTINUOUS_LEASE_MS;
  }
  uint64_t ms = static_cast<uint64_t>(std::ceil(diff));
  return ms == 0 ? 1 : ms;
}

uint32_t total_active_depth() {
  uint32_t total = 0;
  for (uint32_t i = 0; i < g_state.threads_count; ++i) {
    total += g_state.threads[i].depth;
  }
  return total;
}

const char *vector_fields_name(uint32_t code) {
  switch (code) {
    case PGR_CAM_VF_EXPORTS_UNAVAILABLE:
      return "exports-unavailable";
    case PGR_CAM_VF_CLASS_UNAVAILABLE:
      return "class-unavailable";
    case PGR_CAM_VF_SIGNATURE_MISMATCH:
      return "signature-mismatch";
    case PGR_CAM_VF_VERIFIED:
      return "verified";
    default:
      return "exports-unavailable";
  }
}

void append_raw(char *dst, size_t cap, size_t *pos, const char *s) {
  if (!dst || !pos || !s || *pos >= cap - 1) return;
  size_t avail = cap - 1 - *pos;
  size_t len = strlen(s);
  if (len > avail) len = avail;
  memcpy(dst + *pos, s, len);
  *pos += len;
  dst[*pos] = '\0';
}

void append_fmt(char *dst, size_t cap, size_t *pos, const char *fmt, ...) {
  if (!dst || !pos || *pos >= cap - 1) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(dst + *pos, cap - *pos, fmt, ap);
  va_end(ap);
  if (n > 0) {
    size_t added = static_cast<size_t>(n);
    if (*pos + added >= cap) {
      *pos = cap - 1;
    } else {
      *pos += added;
    }
  }
}

void append_json_str(char *dst, size_t cap, size_t *pos, const char *s) {
  append_raw(dst, cap, pos, "\"");
  if (s) {
    for (const char *p = s; *p != '\0'; ++p) {
      unsigned char c = static_cast<unsigned char>(*p);
      if (c == '\"') {
        append_raw(dst, cap, pos, "\\\"");
      } else if (c == '\\') {
        append_raw(dst, cap, pos, "\\\\");
      } else if (c == '\n') {
        append_raw(dst, cap, pos, "\\n");
      } else if (c == '\r') {
        append_raw(dst, cap, pos, "\\r");
      } else if (c == '\t') {
        append_raw(dst, cap, pos, "\\t");
      } else if (c < 0x20) {
        append_fmt(dst, cap, pos, "\\u%04x", static_cast<unsigned int>(c));
      } else {
        char ch[2] = {static_cast<char>(c), '\0'};
        append_raw(dst, cap, pos, ch);
      }
    }
  }
  append_raw(dst, cap, pos, "\"");
}

uint64_t remaining_lease_ms(double now);
bool transport_current(){
 if(!(g_state.cfg.flags&PGR_CH_FLAG_USE_TRANSPORT))return true;
 const auto rt=resolve_transport_api();PgrRuntimeTransportSample sample{};
 return rt&&rt->sample(&sample)==0&&sample.snapshot_fresh&&!sample.fault&&sample.snapshot_seq==g_state.transport_sequence&&sample.generation==g_state.transport_generation;
}
void transport_result(int rc){if(rc!=0){g_state.last_error=PGR_CH_ERR_INTERNAL;g_state.stop_requested.store(true,std::memory_order_release);g_state.lease_until_ms=0;}}
void emit_transport_string(uint32_t opcode, const char *s0) {
  if ((g_state.cfg.flags & PGR_CH_FLAG_USE_TRANSPORT) == 0) return;
  const PgrRuntimeTransportApi *rt = resolve_transport_api();
  if (!rt || !rt->emit) return;
  const char *txt = s0 ? s0 : "";
  PgrRuntimeTransportString s = {txt, static_cast<uint32_t>(strlen(txt))};
  transport_result(rt->emit(opcode, nullptr, 0, &s, 1));
}

void emit_transport_bool(uint32_t opcode, bool val) {
  if ((g_state.cfg.flags & PGR_CH_FLAG_USE_TRANSPORT) == 0) return;
  const PgrRuntimeTransportApi *rt = resolve_transport_api();
  if (!rt || !rt->emit) return;
  int64_t scalars[1] = {val ? 1 : 0};
  transport_result(rt->emit(opcode, scalars, 1, nullptr, 0));
}

void emit_transport_event(const char *channel, const char *json) {
  if ((g_state.cfg.flags & PGR_CH_FLAG_USE_TRANSPORT) == 0) return;
  const PgrRuntimeTransportApi *rt = resolve_transport_api();
  if (!rt || !rt->emit) return;
  const char *ch = channel ? channel : "camera-probe";
  const char *js = json ? json : "{}";
  PgrRuntimeTransportString strings[2] = {
      {ch, static_cast<uint32_t>(strlen(ch))},
      {js, static_cast<uint32_t>(strlen(js))},
  };
  transport_result(rt->emit(kActionEvent, nullptr, 0, strings, 2));
}

void emit_transport_heading(double yaw_degrees, int32_t camera_id,
                            bool map_visible, double time_ms) {
  if ((g_state.cfg.flags & PGR_CH_FLAG_USE_TRANSPORT) == 0) return;
  const PgrRuntimeTransportApi *rt = resolve_transport_api();
  if (!rt || !rt->emit) return;
  if (!std::isfinite(yaw_degrees) || camera_id == 0 ||
      !std::isfinite(time_ms) || time_ms < 0.0) {
    emit_transport_string(kActionInvalidateCameraHeading,
                          "Invalid camera reading");
    return;
  }
  int64_t yaw_bits = 0;
  memcpy(&yaw_bits, &yaw_degrees, sizeof(yaw_bits));
  int64_t scalars[4] = {
      yaw_bits,
      static_cast<int64_t>(camera_id),
      map_visible ? 1 : 0,
      static_cast<int64_t>(std::floor(time_ms)),
  };
  if(!revoked()&&transport_current()&&remaining_lease_ms(fallback_now_ms()))transport_result(rt->emit(kActionUpdateCameraHeadingAt, scalars, 4, nullptr, 0));
}

void sync_transport_demand(double now) {
 if(!(g_state.cfg.flags&PGR_CH_FLAG_USE_TRANSPORT))return;
 const auto rt=resolve_transport_api();PgrRuntimeTransportSample before{},after{};int steering=0,buddy=0,pulse=0;
 bool fresh=rt&&rt->sample(&before)==0&&before.snapshot_fresh&&!before.fault;
 if(fresh)fresh=rt->get_bool(kTransportBoolCameraSteering,&steering)==0&&rt->get_bool(kTransportBoolBuddySampling,&buddy)==0&&rt->take_pulse(kTransportPulseCameraSample,&pulse)==0;
 if(fresh)fresh=rt->sample(&after)==0&&after.snapshot_fresh&&!after.fault&&before.snapshot_seq==after.snapshot_seq&&before.generation==after.generation;
 if(before.fault||after.fault){g_state.last_error=PGR_CH_ERR_INTERNAL;g_state.stop_requested.store(true,std::memory_order_release);}
 if(!fresh){g_state.lease_until_ms=0;g_state.transport_sequence=g_state.transport_generation=0;g_state.latest_sample.valid=false;return;}
 g_state.transport_sequence=after.snapshot_seq;g_state.transport_generation=after.generation;
 if(steering>0||buddy>0||pulse>0)g_state.lease_until_ms=now+PGR_CH_CONTINUOUS_LEASE_MS;
 else if(g_state.transport_boolean_demand||!g_state.continuous)g_state.lease_until_ms=0;
 g_state.transport_boolean_demand=steering>0||buddy>0;
}

void push_event(const PgrCameraHostEvent &ev) {
  if(g_state.cfg.flags&PGR_CH_FLAG_USE_TRANSPORT){
    // Same Control routing as embedded-agent: continuous samples publish typed
    // heading57 plus throttled camera-heading below. Every other kind, including
    // all bounded/passive events, publishes camera-probe19 exactly once.
    if(ev.kind!=PGR_CH_EVENT_CAMERA_SAMPLE||!g_state.continuous)
      emit_transport_event("camera-probe",ev.json);
    return;
  }
  if (g_state.event_count < PGR_CH_EVENT_QUEUE_MAX) {
    uint32_t idx =
        (g_state.event_head + g_state.event_count) % PGR_CH_EVENT_QUEUE_MAX;
    g_state.events[idx] = ev;
    g_state.event_count++;
  } else {
    g_state.events[g_state.event_head] = ev;
    g_state.event_head = (g_state.event_head + 1) % PGR_CH_EVENT_QUEUE_MAX;
  }
}

void enqueue_signature_events() {
  uint32_t count = g_state.sample_camera ? kTargetCount : (kTargetCount - 1u);
  for (uint32_t i = 0; i < count; ++i) {
    const TargetSpec &spec = kTargets[i];
    PgrCameraHostEvent ev = {};
    ev.version = PGR_CH_API_VERSION;
    ev.size = sizeof(ev);
    ev.kind = PGR_CH_EVENT_SIGNATURE;
    copy_str(ev.kind_name, sizeof(ev.kind_name), "signature");
    snprintf(ev.json, sizeof(ev.json),
             "{\"kind\":\"signature\",\"type\":\"signature\",\"className\":"
             "\"UnityEngine.%s\",\"name\":\"%s\",\"parameterCount\":0,"
             "\"returnType\":\"%s\",\"isStatic\":%s}",
             spec.class_name, spec.method_name, spec.return_type,
             spec.is_static ? "true" : "false");
    push_event(ev);
  }
}

void enqueue_sampler_ready_event() {
  PgrCameraHostEvent ev = {};
  ev.version = PGR_CH_API_VERSION;
  ev.size = sizeof(ev);
  ev.kind = PGR_CH_EVENT_SAMPLER_READY;
  copy_str(ev.kind_name, sizeof(ev.kind_name), "native-camera-sampler-ready");
  snprintf(ev.json, sizeof(ev.json),
           "{\"kind\":\"native-camera-sampler-ready\",\"type\":"
           "\"native-camera-sampler-ready\",\"backend\":\"native\"}");
  push_event(ev);
}

void enqueue_ready_event() {
  PgrCameraHostEvent ev = {};
  ev.version = PGR_CH_API_VERSION;
  ev.size = sizeof(ev);
  ev.kind = PGR_CH_EVENT_READY;
  ev.world_map_layer = g_state.cfg.expected_layer;
  copy_str(ev.kind_name, sizeof(ev.kind_name), "ready");

  const char *stage = g_state.continuous      ? "continuous-camera-sampling"
                      : g_state.sample_camera ? "bounded-camera-sampling"
                                              : "passive-thread-observation";
  const char *vf_str = vector_fields_name(g_state.meta.vector_fields);

  if (g_state.sample_camera && g_state.continuous) {
    snprintf(ev.json, sizeof(ev.json),
             "{\"kind\":\"ready\",\"type\":\"ready\",\"stage\":\"%s\","
             "\"observeMs\":null,\"vectorFields\":\"%s\",\"attempts\":%u,"
             "\"executeTasksMethod\":\"0x%llx\",\"sampleCamera\":true,"
             "\"continuous\":true,\"mainCameraRigGetter\":\"0x%llx\","
             "\"worldMapLayer\":%d,\"exports\":[\"il2cpp_runtime_invoke\"]}",
             stage, vf_str, g_state.attempts,
             static_cast<unsigned long long>(
                 reinterpret_cast<uintptr_t>(g_state.meta.execute)),
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(
                 g_state.meta.get_main_camera_address)),
             g_state.cfg.expected_layer);
  } else {
    snprintf(ev.json, sizeof(ev.json),
             "{\"kind\":\"ready\",\"type\":\"ready\",\"stage\":\"%s\","
             "\"observeMs\":%u,\"vectorFields\":\"%s\",\"attempts\":%u,"
             "\"executeTasksMethod\":\"0x%llx\",\"sampleCamera\":%s,"
             "\"continuous\":false,\"exports\":[\"il2cpp_runtime_invoke\"]}",
             stage, PGR_CH_OBSERVE_MS, vf_str, g_state.attempts,
             static_cast<unsigned long long>(
                 reinterpret_cast<uintptr_t>(g_state.meta.execute)),
             g_state.sample_camera ? "true" : "false");
  }
  push_event(ev);
}

void enqueue_sampler_error_event(const FailureRecord &f) {
  PgrCameraHostEvent ev = {};
  ev.version = PGR_CH_API_VERSION;
  ev.size = sizeof(ev);
  ev.kind = PGR_CH_EVENT_SAMPLER_ERROR;
  ev.tid = f.tid;
  ev.sample_index = f.sample_attempt;
  copy_str(ev.kind_name, sizeof(ev.kind_name), "sampler-error");
  copy_str(ev.reason, sizeof(ev.reason), f.message);

  size_t pos = 0;
  append_raw(ev.json, sizeof(ev.json), &pos,
             "{\"kind\":\"sampler-error\",\"type\":\"sampler-error\",\"stage\":");
  append_json_str(ev.json, sizeof(ev.json), &pos, f.stage);
  append_raw(ev.json, sizeof(ev.json), &pos, ",\"errorName\":");
  append_json_str(ev.json, sizeof(ev.json), &pos, f.error_name);
  append_raw(ev.json, sizeof(ev.json), &pos, ",\"message\":");
  append_json_str(ev.json, sizeof(ev.json), &pos, f.message);
  append_fmt(ev.json, sizeof(ev.json), &pos,
             ",\"tid\":%llu,\"sampleAttempt\":%u}",
             static_cast<unsigned long long>(f.tid), f.sample_attempt);
  push_event(ev);
}

void format_summary_fields(char *dst, size_t cap, size_t *pos,
                           uint64_t observed_ms) {
  append_fmt(dst, cap, pos,
             "\"observedMs\":%llu,\"calls\":%u,\"rawMatches\":%u,"
             "\"matches\":%u,\"threads\":[",
             static_cast<unsigned long long>(observed_ms), g_state.calls,
             g_state.raw_matches, g_state.matches);
  for (uint32_t i = 0; i < g_state.threads_count; ++i) {
    if (i > 0) append_raw(dst, cap, pos, ",");
    append_fmt(dst, cap, pos, "{\"tid\":%llu,\"count\":%u}",
               static_cast<unsigned long long>(g_state.threads[i].tid),
               g_state.threads[i].count);
  }
  append_fmt(dst, cap, pos,
             "],\"activeMatches\":%u,\"sampleCamera\":%s,\"samples\":%u,"
             "\"sampleFailure\":",
             total_active_depth(), g_state.sample_camera ? "true" : "false",
             g_state.sample_count);
  if (g_state.sample_failure.valid) {
    append_raw(dst, cap, pos, "{\"stage\":");
    append_json_str(dst, cap, pos, g_state.sample_failure.stage);
    append_raw(dst, cap, pos, ",\"errorName\":");
    append_json_str(dst, cap, pos, g_state.sample_failure.error_name);
    append_raw(dst, cap, pos, ",\"message\":");
    append_json_str(dst, cap, pos, g_state.sample_failure.message);
    append_fmt(dst, cap, pos, ",\"tid\":%llu,\"sampleAttempt\":%u}",
               static_cast<unsigned long long>(g_state.sample_failure.tid),
               g_state.sample_failure.sample_attempt);
  } else {
    append_raw(dst, cap, pos, "null");
  }
}

void enqueue_thread_summary_event(uint64_t observed_ms) {
  PgrCameraHostEvent ev = {};
  ev.version = PGR_CH_API_VERSION;
  ev.size = sizeof(ev);
  ev.kind = PGR_CH_EVENT_THREAD_SUMMARY;
  ev.time_ms = static_cast<double>(observed_ms);
  copy_str(ev.kind_name, sizeof(ev.kind_name), "thread-summary");

  size_t pos = 0;
  append_raw(ev.json, sizeof(ev.json), &pos,
             "{\"kind\":\"thread-summary\",\"type\":\"thread-summary\",");
  format_summary_fields(ev.json, sizeof(ev.json), &pos, observed_ms);
  append_raw(ev.json, sizeof(ev.json), &pos, "}");
  push_event(ev);
}

void drain_samples() {
  if(g_state.continuous&&(revoked()||!transport_current()||!remaining_lease_ms(fallback_now_ms()))){g_state.latest_sample.valid=false;g_state.pending_unavailable.valid=false;return;}
  if (g_state.sample_error[0] != '\0') {
    g_state.latest_sample.valid = false;
    g_state.pending_unavailable.valid = false;
    g_state.bounded_count = 0;
    g_state.bounded_head = 0;
    return;
  }

  if (g_state.continuous) {
    if (g_state.pending_unavailable.valid) {
      UnavailableRecord u = g_state.pending_unavailable;
      g_state.pending_unavailable.valid = false;
      if (!g_state.unavailable_reported) {
        double delivered_at = fallback_now_ms();
        g_state.unavailable_reported = true;
        PgrCameraHostEvent ev = {};
        ev.version = PGR_CH_API_VERSION;
        ev.size = sizeof(ev);
        ev.kind = PGR_CH_EVENT_CAMERA_UNAVAILABLE;
        ev.tid = u.tid;
        ev.time_ms = u.time_ms;
        ev.age_ms = delivered_at - u.time_ms;
        ev.culling_mask = u.culling_mask;
        ev.world_map_layer = u.world_map_layer;
        copy_str(ev.kind_name, sizeof(ev.kind_name), "camera-unavailable");
        copy_str(ev.reason, sizeof(ev.reason), u.reason);
        if (u.has_mask) {
          snprintf(ev.json, sizeof(ev.json),
                   "{\"kind\":\"camera-unavailable\",\"type\":"
                   "\"camera-unavailable\",\"unavailable\":true,\"timeMs\":"
                   "%.15g,\"ageMillis\":%.15g,\"tid\":%llu,\"reason\":\"%s\","
                   "\"mapVisible\":false,\"cameraCullingMask\":%u,"
                   "\"cullingMask\":%d,\"worldMapLayer\":%d,"
                   "\"mapGate\":\"world-map-layer-visible\"}",
                   u.time_ms, ev.age_ms, static_cast<unsigned long long>(u.tid),
                   u.reason, u.culling_mask,
                   static_cast<int32_t>(u.culling_mask), u.world_map_layer);
        } else {
          snprintf(ev.json, sizeof(ev.json),
                   "{\"kind\":\"camera-unavailable\",\"type\":"
                   "\"camera-unavailable\",\"unavailable\":true,\"timeMs\":"
                   "%.15g,\"ageMillis\":%.15g,\"tid\":%llu,\"reason\":\"%s\","
                   "\"mapVisible\":false}",
                   u.time_ms, ev.age_ms, static_cast<unsigned long long>(u.tid),
                   u.reason);
        }
        push_event(ev);
        emit_transport_string(kActionInvalidateCameraHeading,
                              "Camera unavailable");

      }
    }
    if (g_state.latest_sample.valid) {
      SampleRecord r = g_state.latest_sample;
      g_state.latest_sample.valid = false;
      double delivered_at = 0.0;
      if (!sample_validated_clock(&delivered_at)) {
        return;
      }
      g_state.unavailable_reported = false;

      PgrCameraHostEvent ev = {};
      ev.version = PGR_CH_API_VERSION;
      ev.size = sizeof(ev);
      ev.kind = PGR_CH_EVENT_CAMERA_SAMPLE;
      ev.flags = r.map_visible ? PGR_CH_EVFLAG_MAP_VISIBLE : 0u;
      ev.sample_index = r.sample_index;
      ev.camera_id = r.camera_id;
      ev.culling_mask = r.culling_mask;
      ev.world_map_layer = r.world_map_layer;
      ev.tid = r.tid;
      ev.time_ms = r.time_ms;
      ev.age_ms = delivered_at - r.time_ms;
      ev.x = r.x;
      ev.y = r.y;
      ev.z = r.z;
      ev.yaw_degrees = r.yaw_degrees;
      copy_str(ev.kind_name, sizeof(ev.kind_name), "camera-sample");
      snprintf(ev.json, sizeof(ev.json),
               "{\"kind\":\"camera-sample\",\"type\":\"camera-sample\","
               "\"sampleIndex\":%u,\"timeMs\":%.15g,\"tid\":%llu,"
               "\"cameraId\":%d,\"cameraCullingMask\":%u,\"cullingMask\":%d,"
               "\"worldMapLayer\":%d,\"mapVisible\":%s,"
               "\"mapGate\":\"world-map-layer-visible\",\"x\":%.15g,"
               "\"y\":%.15g,\"z\":%.15g,\"yawDegrees\":%.15g,"
               "\"ageMillis\":%.15g}",
               r.sample_index, r.time_ms,
               static_cast<unsigned long long>(r.tid), r.camera_id,
               r.culling_mask, static_cast<int32_t>(r.culling_mask),
               r.world_map_layer, r.map_visible ? "true" : "false", r.x, r.y,
               r.z, r.yaw_degrees, ev.age_ms);
      push_event(ev);
      emit_transport_heading(r.yaw_degrees, r.camera_id, r.map_visible,
                             r.time_ms);
      if (!g_state.has_heading_log ||
          r.time_ms - g_state.last_heading_log_ms >=
              static_cast<double>(PGR_CH_HEADING_LOG_INTERVAL_MS)) {
        g_state.has_heading_log = true;
        g_state.last_heading_log_ms = r.time_ms;
        emit_transport_event("camera-heading", ev.json);
      }
    }
    return;
  }

  while (g_state.bounded_count > 0) {
    SampleRecord r = g_state.bounded_queue[g_state.bounded_head];
    g_state.bounded_head = (g_state.bounded_head + 1) % PGR_CH_SAMPLES_MAX;
    g_state.bounded_count--;

    double delivered_at = 0.0;
    if (!sample_validated_clock(&delivered_at)) {
      break;
    }

    PgrCameraHostEvent ev = {};
    ev.version = PGR_CH_API_VERSION;
    ev.size = sizeof(ev);
    ev.kind = PGR_CH_EVENT_CAMERA_SAMPLE;
    ev.sample_index = r.sample_index;
    ev.camera_id = r.camera_id;
    ev.tid = r.tid;
    ev.time_ms = r.time_ms;
    ev.age_ms =
        delivered_at >= r.time_ms ? (delivered_at - r.time_ms) : 0.0;
    ev.x = r.x;
    ev.y = r.y;
    ev.z = r.z;
    ev.yaw_degrees = r.yaw_degrees;
    copy_str(ev.kind_name, sizeof(ev.kind_name), "camera-sample");
    snprintf(ev.json, sizeof(ev.json),
             "{\"kind\":\"camera-sample\",\"type\":\"camera-sample\","
             "\"sampleIndex\":%u,\"timeMs\":%.15g,\"tid\":%llu,\"cameraId\":%d,"
             "\"x\":%.15g,\"y\":%.15g,\"z\":%.15g,\"yawDegrees\":%.15g,"
             "\"ageMillis\":%.15g}",
             r.sample_index, r.time_ms,
             static_cast<unsigned long long>(r.tid), r.camera_id, r.x, r.y, r.z,
             r.yaw_degrees, ev.age_ms);
    push_event(ev);

    snprintf(g_state.last_bounded_reading, sizeof(g_state.last_bounded_reading),
             "Camera yaw (world): %.1f\xc2\xb0\nCamera %d \xc2\xb7 sample %u",
             r.yaw_degrees, r.camera_id, r.sample_index);
    emit_transport_string(kActionCameraDiagnostic,
                          g_state.last_bounded_reading);
  }
}

void teardown_hooks_and_sampler() {
  // Hooks/trampolines are process lifetime. OFF is passthrough, never unhook
  // code that another frame might already be executing.
  g_state.lease_until_ms=0;
}

void finish_observation(const char *override_status,
                        const char *override_reason,
                        const char *override_message, bool include_summary,
                        bool include_attempts) {
  if (!g_state.active) return;
  if (g_state.sampling.load(std::memory_order_acquire) != 0) {
    g_state.stopping = true;
    if (!g_state.pending_finish) {
      g_state.pending_finish = true;
      g_state.pending_finish_with_summary = include_summary;
      g_state.pending_finish_with_attempts = include_attempts;
      copy_str(g_state.pending_finish_status,
               sizeof(g_state.pending_finish_status),
               override_status ? override_status : "");
      copy_str(g_state.pending_finish_reason,
               sizeof(g_state.pending_finish_reason),
               override_reason ? override_reason : "");
      copy_str(g_state.pending_finish_message,
               sizeof(g_state.pending_finish_message),
               override_message ? override_message : "");
    }
    return;
  }

  g_state.stopping = true;
  g_state.pending_finish = false;
  g_state.active = false;
  teardown_hooks_and_sampler();

  if (include_summary) {
    drain_samples();
  }

  double now = fallback_now_ms();
  uint64_t observed_ms =
      (g_state.ready && now >= g_state.observed_at_ms)
          ? static_cast<uint64_t>(now - g_state.observed_at_ms)
          : 0;

  if (include_summary && g_state.ready) {
    if (g_state.sample_failure.valid) {
      enqueue_sampler_error_event(g_state.sample_failure);
    }
    enqueue_thread_summary_event(observed_ms);
  }

  const char *final_status = "unavailable";
  const char *final_reason = "";
  const char *final_message = override_message ? override_message : "";
  uint64_t candidate_tid = 0;

  if (override_status != nullptr && override_status[0] != '\0') {
    final_status = override_status;
    final_reason = override_reason ? override_reason : "";
  } else if (override_reason != nullptr && override_reason[0] != '\0') {
    if (strcmp(override_reason, "cancelled") == 0 ||
        strcmp(override_reason, "stopped") == 0) {
      final_status = "cancelled";
      final_reason = override_reason;
    } else {
      final_status = "unavailable";
      final_reason = override_reason;
    }
  } else {
    if (g_state.sample_error[0] != '\0') {
      final_reason = g_state.sample_error;
    } else if (g_state.overflow) {
      final_reason = "observation-budget";
    } else if (g_state.matches == 0) {
      final_reason = "execute-tasks-not-observed";
    } else if (g_state.threads_count != 1) {
      final_reason = "multiple-matching-threads";
    } else if (g_state.matches < PGR_CH_MIN_MATCHES) {
      final_reason = "insufficient-matching-calls";
    } else if (total_active_depth() > 0) {
      final_reason = "invocation-still-active";
    } else if (g_state.sample_camera && g_state.sample_count == 0) {
      final_reason = "no-camera-samples";
    }
    if (final_reason[0] == '\0') {
      final_status = "complete";
      candidate_tid = g_state.threads[0].tid;
    } else {
      final_status = "unavailable";
    }
  }

  if(!g_state.last_error){
    if(strcmp(final_status,"error")==0)g_state.last_error=PGR_CH_ERR_INTERNAL;
    else if(strcmp(final_status,"unavailable")==0)g_state.last_error=PGR_CH_UNAVAILABLE;
  }
  copy_str(g_state.reason, sizeof(g_state.reason), final_reason);

  PgrCameraHostEvent ev = {};
  ev.version = PGR_CH_API_VERSION;
  ev.size = sizeof(ev);
  ev.kind = PGR_CH_EVENT_FINISHED;
  ev.flags = (strcmp(final_status, "complete") == 0)
                 ? PGR_CH_EVFLAG_STATUS_COMPLETE
                 : 0u;
  ev.tid = candidate_tid;
  ev.time_ms = now;
  copy_str(ev.kind_name, sizeof(ev.kind_name), "finished");
  copy_str(ev.status, sizeof(ev.status), final_status);
  copy_str(ev.reason, sizeof(ev.reason), final_reason);

  size_t pos = 0;
  append_raw(ev.json, sizeof(ev.json), &pos,
             "{\"kind\":\"finished\",\"type\":\"finished\",\"status\":");
  append_json_str(ev.json, sizeof(ev.json), &pos, final_status);
  append_raw(ev.json, sizeof(ev.json), &pos, ",\"reason\":");
  if (final_reason[0] != '\0') {
    append_json_str(ev.json, sizeof(ev.json), &pos, final_reason);
  } else {
    append_raw(ev.json, sizeof(ev.json), &pos, "null");
  }
  if (final_message[0] != '\0') {
    append_raw(ev.json, sizeof(ev.json), &pos, ",\"message\":");
    append_json_str(ev.json, sizeof(ev.json), &pos, final_message);
  }
  if (include_attempts) {
    append_fmt(ev.json, sizeof(ev.json), &pos,
               ",\"attempts\":%u,\"sampleCamera\":%s,\"continuous\":%s}",
               g_state.attempts, g_state.sample_camera ? "true" : "false",
               g_state.continuous ? "true" : "false");
  } else if (include_summary && g_state.ready) {
    if (candidate_tid != 0) {
      append_fmt(ev.json, sizeof(ev.json), &pos,
                 ",\"mainThreadCandidateTid\":%llu,",
                 static_cast<unsigned long long>(candidate_tid));
    } else {
      append_raw(ev.json, sizeof(ev.json), &pos,
                 ",\"mainThreadCandidateTid\":null,");
    }
    format_summary_fields(ev.json, sizeof(ev.json), &pos, observed_ms);
    append_fmt(ev.json, sizeof(ev.json), &pos, ",\"continuous\":%s}",
               g_state.continuous ? "true" : "false");
  } else {
    append_fmt(ev.json, sizeof(ev.json), &pos,
               ",\"sampleCamera\":%s,\"continuous\":%s}",
               g_state.sample_camera ? "true" : "false",
               g_state.continuous ? "true" : "false");
  }

  push_event(ev);

  if (g_state.continuous && g_state.sample_camera) {

    emit_transport_string(kActionInvalidateCameraHeading,
                          "Camera reader stopped");
  } else if (g_state.sample_camera && !g_state.continuous) {
    char msg[512] = {};
    if (g_state.last_bounded_reading[0] != '\0') {
      snprintf(msg, sizeof(msg), "%s\nDiagnostic %s%s%s%s%s",
               g_state.last_bounded_reading, final_status,
               final_reason[0] ? ": " : "", final_reason,
               final_message[0] ? ": " : "", final_message);
    } else {
      snprintf(msg, sizeof(msg), "Diagnostic %s%s%s%s%s", final_status,
               final_reason[0] ? ": " : "", final_reason,
               final_message[0] ? ": " : "", final_message);
    }
    emit_transport_string(kActionCameraDiagnostic, msg);
    emit_transport_bool(kActionSetCameraDiagnosticRunning, false);
  }
}

void record_failure(const char *stage, const char *error_name,
                    const char *message, const char *latch_reason,
                    uint64_t tid) {
  if (!g_state.sample_failure.valid) {
    g_state.sample_failure.valid = true;
    g_state.sample_failure.tid = tid;
    g_state.sample_failure.sample_attempt = g_state.sample_attempts;
    copy_str(g_state.sample_failure.stage, sizeof(g_state.sample_failure.stage),
             stage ? stage : "sample-start");
    copy_str(g_state.sample_failure.error_name,
             sizeof(g_state.sample_failure.error_name),
             error_name ? error_name : "Error");
    copy_str(g_state.sample_failure.message,
             sizeof(g_state.sample_failure.message),
             message ? message : "No error message");
  }
  const char *reason_to_latch =
      (latch_reason && latch_reason[0]) ? latch_reason : message;
  if (g_state.sample_error[0] == '\0') {
    copy_str(g_state.sample_error, sizeof(g_state.sample_error),
             (reason_to_latch && reason_to_latch[0]) ? reason_to_latch
                                                     : "native-sampler-error");
  }
}

const char *map_sampler_unavailable_reason(uint32_t code) {
  switch (code) {
    case PGR_CS_REASON_OFF:
      return "off";
    case PGR_CS_REASON_LEASE:
      return "lease-expired";
    case PGR_CS_REASON_THREAD:
      return "owner-thread-unavailable";
    case PGR_CS_REASON_NON_MAP_RIG:
      return "non-map-rig";
    case PGR_CS_REASON_NULL_CAMERA:
      return "camera-getter-returned-null";
    case PGR_CS_REASON_CAMERA_MISMATCH:
      return "camera-mismatch";
    case PGR_CS_REASON_NULL_TRANSFORM:
      return "camera-transform-unavailable";
    case PGR_CS_REASON_MAP_HIDDEN:
      return "map-hidden";
    default:
      return "camera-unavailable";
  }
}

const char *map_sampler_fault_reason(uint32_t code) {
  switch (code) {
    case PGR_CS_REASON_CLOCK:
      return "invalid-boot-clock";
    case PGR_CS_REASON_GCHANDLE:
      return "gc-handle-allocation-failed";
    case PGR_CS_REASON_MANAGED:
      return "managed-getter-exception";
    case PGR_CS_REASON_METADATA:
      return "runtime-class-mismatch";
    case PGR_CS_REASON_LAYER:
      return "world-map-layer-mismatch";
    case PGR_CS_REASON_NONFINITE:
      return "nonfinite-forward-vector";
    case PGR_CS_REASON_VERTICAL:
      return "vertical-forward-vector";
    case PGR_CS_REASON_BUSY:
      return "reentrant";
    default:
      return "native-sampler-error";
  }
}

uint64_t executing_generation(uint64_t tid);
void execute_sample_locked(uint64_t tid, void *rig_receiver,
                           void *returned_camera, bool is_continuous_call) {
  if (!g_state.active || !g_state.ready || !g_state.sample_camera ||
      revoked() || g_state.sampling.load(std::memory_order_acquire) != 0 ||
      g_state.sample_error[0] != '\0' || g_state.sample_done ||
      g_state.overflow || g_state.threads_count != 1 ||
      g_state.matches < PGR_CH_MIN_MATCHES) {
    return;
  }
  if (g_state.continuous != is_continuous_call) return;

  uint32_t active_depth = total_active_depth();
  if (g_state.continuous) {
    // API48/3 proves this getter is inside the real original ExecuteTasks;
    // post-original subscriber phases never create that proof themselves.
    if(!executing_generation(tid))return;
    if (g_state.in_subscriber_pulse) return;
    if (g_state.owner_tid != tid || !rig_receiver) return;
  } else {
    if (active_depth != 0 || g_state.owner_tid != tid) return;
  }

  double now = 0.0;
  if (!sample_validated_clock(&now)) return;

  if (g_state.continuous) {
    sync_transport_demand(now);
    if (remaining_lease_ms(now) == 0) return;
  } else {
    if (g_state.has_sample_started &&
        (now - g_state.sample_started_at_ms >=
             static_cast<double>(PGR_CH_SAMPLE_MS) ||
         g_state.sample_count >= PGR_CH_SAMPLES_MAX)) {
      g_state.sample_done = true;
      return;
    }
  }

  double min_interval =
      g_state.continuous ? static_cast<double>(PGR_CH_CONTINUOUS_INTERVAL_MS)
                         : static_cast<double>(PGR_CH_SAMPLE_INTERVAL_MS);
  if (g_state.has_sampled && (now - g_state.last_sample_at_ms) < min_interval) {
    return;
  }

  int32_t expected = 0;
  if (!g_state.sampling.compare_exchange_strong(expected, 1,
                                                std::memory_order_acq_rel)) {
    return;
  }
  const uint64_t sample_generation=g_state.continuous?executing_generation(tid):0;
  g_state.sampling_tid = tid;
  g_state.sample_attempts++;
  if (!g_state.has_sample_started) {
    g_state.has_sample_started = true;
    g_state.sample_started_at_ms = now;
  }
  g_state.has_sampled = true;
  g_state.last_sample_at_ms = now;

  // Use Feature 41 (camera_sampler) in continuous mode when installed and no
  // local test seam override is active.
  if (g_state.continuous && g_state.sampler_installed &&
      !g_state.use_local_continuous) {
    const PgrCameraSamplerApi *sampler = resolve_sampler_api();
    if (sampler && sampler->sample) {
      uint64_t rem = remaining_lease_ms(now);
      PgrCameraSamplerRequest req = {};
      req.version = PGR_CS_REQUEST_VERSION;
      req.size = sizeof(req);
      req.tid = tid;
      req.remaining_ms = rem;
      req.rig = rig_receiver;
      req.camera = returned_camera;

      PgrCameraSamplerOutput out = {};
      out.version = PGR_CS_OUTPUT_VERSION;
      out.size = sizeof(out);

      int rc = sampler->sample(&req, &out);
      double sample_time =
          (g_state.cfg.clock_fn != nullptr) ? now : out.time_ms;

      if(revoked()||!transport_current()||!remaining_lease_ms(fallback_now_ms())||executing_generation(tid)!=sample_generation){g_state.sampling.store(0,std::memory_order_release);g_state.latest_sample.valid=false;return;}
      if (rc == PGR_CS_SAMPLE && out.status == PGR_CS_SAMPLE) {
        if (!revoked() && g_state.active &&
            g_state.sample_error[0] == '\0') {
          g_state.sample_count++;
          g_state.latest_sample.valid = true;
          g_state.latest_sample.sample_index = g_state.sample_count;
          g_state.latest_sample.time_ms = sample_time;
          g_state.latest_sample.tid = tid;
          g_state.latest_sample.camera_id = out.camera_id;
          g_state.latest_sample.culling_mask = out.mask;
          g_state.latest_sample.world_map_layer = out.layer;
          g_state.latest_sample.map_visible = true;
          g_state.latest_sample.x = out.x;
          g_state.latest_sample.y = out.y;
          g_state.latest_sample.z = out.z;
          g_state.latest_sample.yaw_degrees = out.yaw;
        }
      } else if (rc == PGR_CS_UNAVAILABLE && out.status == PGR_CS_UNAVAILABLE) {
        if (!revoked() && g_state.active &&
            g_state.sample_error[0] == '\0' &&
            out.reason != PGR_CS_REASON_OFF &&
            out.reason != PGR_CS_REASON_LEASE) {
          g_state.latest_sample.valid = false;
          g_state.pending_unavailable.valid = true;
          g_state.pending_unavailable.time_ms = sample_time;
          g_state.pending_unavailable.tid = tid;
          g_state.pending_unavailable.culling_mask = out.mask;
          g_state.pending_unavailable.world_map_layer = out.layer;
          g_state.pending_unavailable.has_mask =
              (out.reason == PGR_CS_REASON_MAP_HIDDEN);
          copy_str(g_state.pending_unavailable.reason,
                   sizeof(g_state.pending_unavailable.reason),
                   map_sampler_unavailable_reason(out.reason));
        }
      } else {
        const char *f_reason = map_sampler_fault_reason(out.reason);
        record_failure("native-camera-sample", "Error", f_reason, f_reason,
                       tid);
      }

      g_state.sampling.store(0, std::memory_order_release);
      if (g_state.pending_finish) {
        char st[32] = {};
        char rsn[64] = {};
        char msg[264] = {};
        copy_str(st, sizeof(st), g_state.pending_finish_status);
        copy_str(rsn, sizeof(rsn), g_state.pending_finish_reason);
        copy_str(msg, sizeof(msg), g_state.pending_finish_message);
        finish_observation(st, rsn, msg, g_state.pending_finish_with_summary,
                           g_state.pending_finish_with_attempts);
      }
      return;
    }
  }

  // Bounded diagnostic sampling (and local continuous fallback when test seams
  // are active).
  const PgrCameraMetaExtra *ex = g_state.cfg.meta_extra;
  const PgrMetaExports *me = g_state.cfg.meta_exports;
  auto thread_current =
      reinterpret_cast<Il2CppThreadCurrentFn>(g_state.cfg.thread_current);
  auto runtime_invoke =
      reinterpret_cast<Il2CppRuntimeInvokeFn>(ex ? ex->runtime_invoke : nullptr);
  auto object_get_class = reinterpret_cast<Il2CppObjectGetClassFn>(
      ex ? ex->object_get_class : nullptr);
  auto object_unbox =
      reinterpret_cast<Il2CppObjectUnboxFn>(ex ? ex->object_unbox : nullptr);
  auto gchandle_new = reinterpret_cast<Il2CppGcHandleNewFn>(
      ex ? const_cast<void *>(ex->gchandle_new) : nullptr);
  auto gchandle_get_target = reinterpret_cast<Il2CppGcHandleGetTargetFn>(
      ex ? const_cast<void *>(ex->gchandle_get_target) : nullptr);
  auto gchandle_free = reinterpret_cast<Il2CppGcHandleFreeFn>(
      ex ? const_cast<void *>(ex->gchandle_free) : nullptr);
  auto field_get_value = reinterpret_cast<Il2CppFieldGetValueFn>(
      ex ? ex->field_get_value : nullptr);
  auto string_new =
      reinterpret_cast<Il2CppStringNewFn>(ex ? ex->string_new : nullptr);
  auto class_is_assignable = reinterpret_cast<Il2CppClassIsAssignableFromFn>(
      me ? me->class_is_assignable_from : nullptr);

  uintptr_t handles[8] = {};
  int handle_count = 0;
  char stage[64] = "sample-start";
  const char *probe_reason = nullptr;
  const char *unavail_reason = nullptr;
  uint32_t unavail_mask = 0;
  int32_t unavail_layer = 0;
  bool unavail_has_mask = false;
  SampleRecord built_sample = {};

  auto check_current = [&]() -> bool {
    char prev[64];
    copy_str(prev, sizeof(prev), stage);
    snprintf(stage, sizeof(stage), "%s:thread-check", prev);
    if (!g_state.active || revoked() ||
        (g_state.continuous&&(!transport_current()||!remaining_lease_ms(fallback_now_ms())||executing_generation(tid)!=sample_generation)) ||
        g_state.sample_error[0] != '\0') {
      probe_reason = "sampling-cancelled";
      return false;
    }
    if (current_tid() != tid) {
      probe_reason = "sampling-thread-changed";
      return false;
    }
    snprintf(stage, sizeof(stage), "%s:thread-current", prev);
    if (!thread_current || thread_current() == nullptr) {
      probe_reason = "unity-thread-not-attached";
      return false;
    }
    copy_str(stage, sizeof(stage), prev);
    return true;
  };

  auto target_of = [&](uintptr_t h) -> void * {
    char prev[64];
    copy_str(prev, sizeof(prev), stage);
    snprintf(stage, sizeof(stage), "%s:gchandle-get-target", prev);
    void *obj = gchandle_get_target ? gchandle_get_target(h) : nullptr;
    if (!obj) {
      probe_reason = "gc-handle-target-missing";
      return nullptr;
    }
    copy_str(stage, sizeof(stage), prev);
    return obj;
  };

  auto retain_obj = [&](void *obj, const void *expected_cls, bool subclass,
                        const char *label, uintptr_t *out_h) -> bool {
    snprintf(stage, sizeof(stage), "%s:null-check", label);
    if (!obj) {
      probe_reason = "camera-getter-returned-null";
      return false;
    }
    snprintf(stage, sizeof(stage), "%s:gchandle-new", label);
    uintptr_t h = gchandle_new ? gchandle_new(obj, 0) : 0;
    if (h == 0) {
      probe_reason = "gc-handle-allocation-failed";
      return false;
    }
    handles[handle_count++] = h;
    snprintf(stage, sizeof(stage), "%s:object-get-class", label);
    void *t = target_of(h);
    if (!t) return false;
    void *actual = object_get_class ? object_get_class(t) : nullptr;
    snprintf(stage, sizeof(stage), "%s:class-check", label);
    if (!actual) {
      probe_reason = "runtime-class-mismatch";
      return false;
    }
    bool ok = subclass ? (class_is_assignable &&
                          class_is_assignable(expected_cls, actual))
                       : (actual == expected_cls);
    if (!ok) {
      if (g_state.continuous && strcmp(label, "MainCameraRig") == 0) {
        unavail_reason = "non-map-rig";
        return false;
      }
      probe_reason = "runtime-class-mismatch";
      return false;
    }
    *out_h = h;
    return true;
  };

  auto invoke_method = [&](const void *method, uintptr_t recv_h,
                           bool has_recv, const char *label,
                           void **params, void **out_val) -> bool {
    snprintf(stage, sizeof(stage), "%s:invoke-preparation", label);
    if (!check_current()) return false;
    snprintf(stage, sizeof(stage), "%s:exception-reset", label);
    void *exc = nullptr;
    snprintf(stage, sizeof(stage), "%s:receiver", label);
    void *recv_obj = nullptr;
    if (has_recv) {
      recv_obj = target_of(recv_h);
      if (!recv_obj) return false;
    }
    snprintf(stage, sizeof(stage), "%s:runtime-invoke", label);
    void *res = runtime_invoke ? runtime_invoke(method, recv_obj, params, &exc)
                               : nullptr;
    if (!check_current()) return false;
    snprintf(stage, sizeof(stage), "%s:exception-read", label);
    if (exc != nullptr) {
      probe_reason = "managed-getter-exception";
      return false;
    }
    *out_val = res;
    return true;
  };

  auto unbox_int32 = [&](uintptr_t h, const char *label,
                         const char *null_reason, int32_t *out_v) -> bool {
    void *t = target_of(h);
    if (!t) return false;
    snprintf(stage, sizeof(stage), "%s:object-unbox", label);
    void *u = object_unbox ? object_unbox(t) : nullptr;
    if (!u) {
      probe_reason = null_reason;
      return false;
    }
    snprintf(stage, sizeof(stage), "%s:value-read", label);
    memcpy(out_v, u, sizeof(int32_t));
    return true;
  };

#if defined(__EXCEPTIONS)
  try {
#endif
    do {
      if (!check_current()) break;
      uintptr_t natural_h = 0;
      if (g_state.continuous) {
        uintptr_t rig_h = 0;
        if (!retain_obj(rig_receiver, g_state.meta.classes[C_RIG], false,
                        "MainCameraRig", &rig_h)) {
          break;
        }
        if (!returned_camera) {
          unavail_reason = "camera-getter-returned-null";
          break;
        }
        if (!retain_obj(returned_camera, g_state.meta.classes[C_CAMERA], false,
                        "NaturalCamera", &natural_h)) {
          break;
        }
      }

      void *cam_obj = nullptr;
      if (!invoke_method(g_state.meta.methods[M_GET_MAIN], 0, false,
                         "Camera.get_main", nullptr, &cam_obj)) {
        break;
      }
      if (g_state.continuous && !cam_obj) {
        unavail_reason = "camera-getter-returned-null";
        break;
      }
      uintptr_t cam_h = 0;
      if (!retain_obj(cam_obj, g_state.meta.classes[C_CAMERA], false, "Camera",
                      &cam_h)) {
        break;
      }
      if (g_state.continuous) {
        void *nat_t = target_of(natural_h);
        if (!nat_t) break;
        void *cam_t = target_of(cam_h);
        if (!cam_t) break;
        if (nat_t != cam_t) {
          unavail_reason = "camera-mismatch";
          break;
        }
        if (g_state.local_world_map_layer < 0) {
          copy_str(stage, sizeof(stage), "WorldMap:string-new");
          if (!check_current()) break;
          void *str_obj = string_new ? string_new("WorldMap") : nullptr;
          uintptr_t str_h = 0;
          if (!retain_obj(str_obj, g_state.meta.classes[C_STRING], false,
                          "String", &str_h)) {
            break;
          }
          void *str_t = target_of(str_h);
          if (!str_t) break;
          void *args[1] = {str_t};
          void *layer_obj = nullptr;
          bool inv_ok = invoke_method(g_state.meta.methods[M_NAME_TO_LAYER], 0,
                                      false, "LayerMask.NameToLayer", args,
                                      &layer_obj);
          args[0] = nullptr;
          if (!inv_ok) break;
          uintptr_t layer_h = 0;
          if (!retain_obj(layer_obj, g_state.meta.classes[C_INT32], false,
                          "LayerIndex", &layer_h)) {
            break;
          }
          int32_t layer_idx = 0;
          if (!unbox_int32(layer_h, "LayerIndex", "boxed-integer-unavailable",
                           &layer_idx)) {
            break;
          }
          if (layer_idx != g_state.cfg.expected_layer) {
            probe_reason = "world-map-layer-mismatch";
            break;
          }
          g_state.local_world_map_layer = layer_idx;
        }

        void *mask_obj = nullptr;
        if (!invoke_method(g_state.meta.methods[M_GET_CULLING_MASK], cam_h,
                           true, "Camera.get_cullingMask", nullptr,
                           &mask_obj)) {
          break;
        }
        uintptr_t mask_h = 0;
        if (!retain_obj(mask_obj, g_state.meta.classes[C_INT32], false,
                        "CullingMask", &mask_h)) {
          break;
        }
        int32_t mask_i32 = 0;
        if (!unbox_int32(mask_h, "CullingMask", "boxed-integer-unavailable",
                         &mask_i32)) {
          break;
        }
        uint32_t bits = static_cast<uint32_t>(mask_i32);
        unavail_mask = bits;
        unavail_layer = g_state.local_world_map_layer;
        if ((bits & (1u << static_cast<uint32_t>(unavail_layer))) == 0) {
          unavail_reason = "map-hidden";
          unavail_has_mask = true;
          break;
        }
      }

      void *tr_obj = nullptr;
      if (!invoke_method(g_state.meta.methods[M_GET_TRANSFORM], cam_h, true,
                         "Component.get_transform", nullptr, &tr_obj)) {
        break;
      }
      if (g_state.continuous && !tr_obj) {
        unavail_reason = "camera-transform-unavailable";
        break;
      }
      uintptr_t tr_h = 0;
      if (!retain_obj(tr_obj, g_state.meta.classes[C_TRANSFORM], true,
                      "Transform", &tr_h)) {
        break;
      }

      void *vec_obj = nullptr;
      if (!invoke_method(g_state.meta.methods[M_GET_FORWARD], tr_h, true,
                         "Transform.get_forward", nullptr, &vec_obj)) {
        break;
      }
      uintptr_t vec_h = 0;
      if (!retain_obj(vec_obj, g_state.meta.classes[C_VECTOR3], false,
                      "Vector3", &vec_h)) {
        break;
      }

      const char *axis_names[3] = {"x", "y", "z"};
      double coords[3] = {};
      for (int axis = 0; axis < 3; ++axis) {
        snprintf(stage, sizeof(stage), "Vector3.%s:field-get-value",
                 axis_names[axis]);
        if (!check_current()) break;
        void *vt = target_of(vec_h);
        if (!vt) break;
        float fv = 0.0f;
        field_get_value(vt, g_state.meta.fields[axis], &fv);
        snprintf(stage, sizeof(stage), "Vector3.%s:float-read",
                 axis_names[axis]);
        if (!std::isfinite(fv)) {
          probe_reason = "nonfinite-forward-vector";
          break;
        }
        coords[axis] = static_cast<double>(fv);
      }
      if (probe_reason || unavail_reason) break;

      if (std::hypot(coords[0], coords[2]) < 1e-6) {
        probe_reason = "vertical-forward-vector";
        break;
      }
      double yaw_degrees =
          std::fmod(std::atan2(coords[0], coords[2]) * 180.0 / kPi + 360.0,
                    360.0);

      void *id_obj = nullptr;
      if (!invoke_method(g_state.meta.methods[M_GET_INSTANCE_ID], cam_h, true,
                         "Object.GetInstanceID", nullptr, &id_obj)) {
        break;
      }
      uintptr_t id_h = 0;
      if (!retain_obj(id_obj, g_state.meta.classes[C_INT32], false, "Int32",
                      &id_h)) {
        break;
      }
      int32_t camera_id = 0;
      if (!unbox_int32(id_h, "Int32", "boxed-identity-unavailable",
                       &camera_id)) {
        break;
      }
      if (!check_current()) break;

      g_state.sample_count++;
      built_sample.valid = true;
      built_sample.sample_index = g_state.sample_count;
      built_sample.time_ms = now;
      built_sample.tid = tid;
      built_sample.camera_id = camera_id;
      built_sample.culling_mask = unavail_mask;
      built_sample.world_map_layer = unavail_layer;
      built_sample.map_visible = g_state.continuous;
      built_sample.x = coords[0];
      built_sample.y = coords[1];
      built_sample.z = coords[2];
      built_sample.yaw_degrees = yaw_degrees;
      if (!g_state.continuous && g_state.sample_count >= PGR_CH_SAMPLES_MAX) {
        g_state.sample_done = true;
      }
    } while (false);
#if defined(__EXCEPTIONS)
  } catch (const std::exception &e) {
    record_failure(stage, "Error", e.what(), "native-sampler-error", tid);
  } catch (...) {
    record_failure(stage, "Error", "native-sampler-error",
                   "native-sampler-error", tid);
  }
#endif

  if (probe_reason != nullptr) {
    record_failure(stage, "Error", probe_reason, probe_reason, tid);
  }

  for (int i = handle_count - 1; i >= 0; --i) {
#if defined(__EXCEPTIONS)
    try {
#endif
      copy_str(stage, sizeof(stage), "cleanup:gchandle-free");
      if (gchandle_free && handles[i] != 0) {
        gchandle_free(handles[i]);
      }
#if defined(__EXCEPTIONS)
    } catch (const std::exception &e) {
      record_failure(stage, "Error", e.what(), "gc-handle-cleanup-failed", tid);
      copy_str(g_state.sample_error, sizeof(g_state.sample_error),
               "gc-handle-cleanup-failed");
    } catch (...) {
      record_failure(stage, "Error", "gc-handle-cleanup-failed",
                     "gc-handle-cleanup-failed", tid);
      copy_str(g_state.sample_error, sizeof(g_state.sample_error),
               "gc-handle-cleanup-failed");
    }
#endif
  }

  if (g_state.sample_error[0] == '\0' && !revoked() && g_state.active && (!g_state.continuous||(transport_current()&&remaining_lease_ms(fallback_now_ms())&&executing_generation(tid)==sample_generation))) {
    if (unavail_reason != nullptr) {
      if (g_state.continuous) {
        g_state.latest_sample.valid = false;
        g_state.pending_unavailable.valid = true;
        g_state.pending_unavailable.time_ms = now;
        g_state.pending_unavailable.tid = tid;
        g_state.pending_unavailable.culling_mask = unavail_mask;
        g_state.pending_unavailable.world_map_layer = unavail_layer;
        g_state.pending_unavailable.has_mask = unavail_has_mask;
        copy_str(g_state.pending_unavailable.reason,
                 sizeof(g_state.pending_unavailable.reason), unavail_reason);
      }
    } else if (built_sample.valid) {
      if (g_state.continuous) {
        g_state.latest_sample = built_sample;
      } else if (g_state.bounded_count < PGR_CH_SAMPLES_MAX) {
        uint32_t idx =
            (g_state.bounded_head + g_state.bounded_count) % PGR_CH_SAMPLES_MAX;
        g_state.bounded_queue[idx] = built_sample;
        g_state.bounded_count++;
      }
    }
  }

  g_state.sampling.store(0, std::memory_order_release);
  if (g_state.pending_finish) {
    char st[32] = {};
    char rsn[64] = {};
    char msg[264] = {};
    copy_str(st, sizeof(st), g_state.pending_finish_status);
    copy_str(rsn, sizeof(rsn), g_state.pending_finish_reason);
    copy_str(msg, sizeof(msg), g_state.pending_finish_message);
    finish_observation(st, rsn, msg, g_state.pending_finish_with_summary,
                       g_state.pending_finish_with_attempts);
  }
}

uint64_t executing_generation(uint64_t tid) {
 const PgrUnitySchedulerApi3 *scheduler=nullptr;
#if !defined(PGR_CAMERA_HOST_STANDALONE_TEST)
 scheduler=static_cast<const PgrUnitySchedulerApi3*>(pgrazor_unity_scheduler_api(3));
#else
 auto core=resolve_native_api();if(core&&core->query_feature)scheduler=static_cast<const PgrUnitySchedulerApi3*>(core->query_feature(48,3));
#endif
 return scheduler&&scheduler->version==3&&scheduler->size==sizeof(*scheduler)&&scheduler->executing_generation?scheduler->executing_generation(tid):0;
}
void *hook_get_main_camera(void *rig,const void *method_info) {
  MainCameraGetterFn original=g_state.orig_getter.load(std::memory_order_acquire);
  if(!original)__builtin_trap();
  StateGuard state_guard;
  if(!state_guard.entered)return original(rig,method_info);
  const uint64_t tid=current_tid(), generation=executing_generation(tid);
  const bool eligible=g_state.active&&g_state.ready&&g_state.continuous&&!revoked()&&
    !g_state.sampling.load()&&!g_state.sample_error[0]&&!g_state.overflow&&!tls_getter_depth&&
    !g_state.in_subscriber_pulse&&generation&&g_state.matches>=PGR_CH_MIN_MATCHES&&
    g_state.threads_count==1&&g_state.owner_tid==tid&&rig;
  if(!eligible)return original(rig,method_info);
  const auto ex=g_state.cfg.meta_extra;
  auto make=reinterpret_cast<Il2CppGcHandleNewFn>(const_cast<void*>(ex->gchandle_new));
  auto get=reinterpret_cast<Il2CppGcHandleGetTargetFn>(const_cast<void*>(ex->gchandle_get_target));
  auto free=reinterpret_cast<Il2CppGcHandleFreeFn>(const_cast<void*>(ex->gchandle_free));
  struct Roots {Il2CppGcHandleFreeFn release;uintptr_t self=0,camera=0;~Roots(){if(camera)release(camera);if(self)release(self);}} roots{free};
  struct Depth {Depth(){++tls_getter_depth;g_state.getter_depth.fetch_add(1);}~Depth(){--tls_getter_depth;g_state.getter_depth.fetch_sub(1);}} depth;
  roots.self=make(rig,1);if(!roots.self)return original(rig,method_info);
  void *returned=original(get(roots.self),method_info);
  if(returned){roots.camera=make(returned,1);if(!roots.camera)return returned;}
  if(!revoked()&&executing_generation(tid)==generation)execute_sample_locked(tid,get(roots.self),roots.camera?get(roots.camera):nullptr,true);
  // Both objects stay pinned across41 allocation and cleanup; the original
  // receiver and MethodInfo were forwarded exactly once, including exceptions.
  return roots.camera?get(roots.camera):returned;
}

void populate_config_from_bootstrap(PgrCameraHostConfig *cfg) {
  const PgrBootApi *rb = resolve_bootstrap_api();
  if (!rb || !rb->snapshot) return;
  PgrBootBinding b = {};
  b.version = PGR_BOOT_BINDING_VERSION;
  b.size = sizeof(b);
  if (rb->snapshot(&b) != PGR_BOOT_OK) return;

  if (!cfg->module_base) cfg->module_base = b.module_base;
  if (cfg->module_size == 0) cfg->module_size = b.module_size;
  if (!cfg->domain) cfg->domain = b.domain;
  if (!cfg->thread_current) cfg->thread_current = b.runtime.thread_current;
  if (!cfg->thread_attach) cfg->thread_attach = b.runtime.thread_attach;
  if (!cfg->thread_detach) cfg->thread_detach = b.runtime.thread_detach;

  if (!cfg->meta_exports) {
    g_state.boot_meta_exports = b.meta;
    cfg->meta_exports = &g_state.boot_meta_exports;
  }

  if (!cfg->meta_extra) {
    PgrCameraMetaExtra &ex = g_state.boot_meta_extra;
    memset(&ex, 0, sizeof(ex));
    ex.version = PGR_CAM_EXTRA_VERSION;
    ex.size = sizeof(ex);
    if (b.handle != nullptr) {
      ex.image_get_name = dlsym(b.handle, "il2cpp_image_get_name");
      ex.get_corlib = dlsym(b.handle, "il2cpp_get_corlib");
    }
    ex.runtime_invoke = b.runtime.runtime_invoke;
    ex.object_get_class = b.runtime.object_get_class;
    ex.object_unbox = b.runtime.object_unbox;
    ex.gchandle_new = b.runtime.gchandle_new;
    ex.gchandle_get_target = b.runtime.gchandle_get_target;
    ex.gchandle_free = b.runtime.gchandle_free;
    ex.field_get_value = b.runtime.field_get_value;
    ex.string_new = b.runtime.string_new;

    uint32_t present = 0;
    if (b.meta.class_get_field_from_name)
      present |= PGR_CAM_EXP_CLASS_GET_FIELD_FROM_NAME;
    if (b.meta.field_get_type) present |= PGR_CAM_EXP_FIELD_GET_TYPE;
    if (b.meta.field_get_parent) present |= PGR_CAM_EXP_FIELD_GET_PARENT;
    if (b.meta.field_get_flags) present |= PGR_CAM_EXP_FIELD_GET_FLAGS;
    if (ex.runtime_invoke) present |= PGR_CAM_EXP_RUNTIME_INVOKE;
    if (ex.object_get_class) present |= PGR_CAM_EXP_OBJECT_GET_CLASS;
    if (ex.object_unbox) present |= PGR_CAM_EXP_OBJECT_UNBOX;
    if (ex.gchandle_new) present |= PGR_CAM_EXP_GCHANDLE_NEW;
    if (ex.gchandle_get_target) present |= PGR_CAM_EXP_GCHANDLE_GET_TARGET;
    if (ex.gchandle_free) present |= PGR_CAM_EXP_GCHANDLE_FREE;
    if (ex.field_get_value) present |= PGR_CAM_EXP_FIELD_GET_VALUE;
    if (ex.get_corlib) present |= PGR_CAM_EXP_GET_CORLIB;
    if (b.meta.class_is_assignable_from)
      present |= PGR_CAM_EXP_CLASS_IS_ASSIGNABLE_FROM;
    if (b.meta.class_is_valuetype) present |= PGR_CAM_EXP_CLASS_IS_VALUETYPE;
    if (b.meta.class_value_size) present |= PGR_CAM_EXP_CLASS_VALUE_SIZE;
    if (b.meta.debug_get_method_info)
      present |= PGR_CAM_EXP_DEBUG_GET_METHOD_INFO;
    if (b.meta.method_is_generic) present |= PGR_CAM_EXP_METHOD_IS_GENERIC;
    if (b.meta.method_is_inflated) present |= PGR_CAM_EXP_METHOD_IS_INFLATED;
    if (b.meta.method_get_param) present |= PGR_CAM_EXP_METHOD_GET_PARAM;
    if (ex.string_new) present |= PGR_CAM_EXP_STRING_NEW;
    ex.present = present;

    cfg->meta_extra = &g_state.boot_meta_extra;
  }
}

int attempt_prepare(double now) {
  if (!g_state.active || g_state.ready || revoked()) return PGR_CH_OK;

  if ((g_state.cfg.flags & PGR_CH_FLAG_USE_BOOTSTRAP) != 0) {
    populate_config_from_bootstrap(&g_state.cfg);
  }

  g_state.attempts++;
  g_state.has_prepared = true;
  g_state.last_prepare_at_ms = now;

  if (!g_state.cfg.module_base || g_state.cfg.module_size == 0) {
    copy_str(g_state.reason, sizeof(g_state.reason), "library-not-loaded");
    if (!g_state.continuous) {
      double elapsed =
          now >= g_state.started_at_ms ? (now - g_state.started_at_ms) : 0.0;
      if (g_state.attempts >= PGR_CH_ATTEMPTS_MAX ||
          elapsed + static_cast<double>(PGR_CH_RETRY_MS) >
              static_cast<double>(PGR_CH_PREPARE_MS)) {
        finish_observation("unavailable", "library-not-loaded", nullptr, false,
                           true);
        return PGR_CH_UNAVAILABLE;
      }
    }
    return PGR_CH_RETRY;
  }

  if (!g_state.cfg.meta_exports || !g_state.cfg.meta_extra ||
      !g_state.cfg.thread_current || !g_state.cfg.meta_extra->image_get_name) {
    finish_observation("error", "missing-il2cpp-exports",
                       "Error: Missing export il2cpp", false, false);
    return PGR_CH_ERR_MISSING_EXPORT;
  }

  const void *domain = g_state.cfg.domain;
  if (!domain && g_state.cfg.meta_exports->domain_get) {
    auto dget = reinterpret_cast<Il2CppDomainGetFn>(
        g_state.cfg.meta_exports->domain_get);
    domain = dget();
  }
  if (!domain) {
    copy_str(g_state.reason, sizeof(g_state.reason), "domain-not-ready");
    if (!g_state.continuous) {
      double elapsed =
          now >= g_state.started_at_ms ? (now - g_state.started_at_ms) : 0.0;
      if (g_state.attempts >= PGR_CH_ATTEMPTS_MAX ||
          elapsed + static_cast<double>(PGR_CH_RETRY_MS) >
              static_cast<double>(PGR_CH_PREPARE_MS)) {
        finish_observation("unavailable", "domain-not-ready", nullptr, false,
                           true);
        return PGR_CH_UNAVAILABLE;
      }
    }
    return PGR_CH_RETRY;
  }

  auto thread_current =
      reinterpret_cast<Il2CppThreadCurrentFn>(g_state.cfg.thread_current);
  // Preparation belongs to an already-attached Unity owner. Retry without
  // attaching a background thread or changing managed thread identity.
  if(thread_current()==nullptr)return PGR_CH_RETRY;

  const PgrCameraMetaApi *meta_api = resolve_metadata_api();
  if (!meta_api || !meta_api->resolve) {
    finish_observation("error", "native-camera-metadata-missing",
                       "Error: native-camera-metadata-missing", false, false);
    return PGR_CH_ERR_INTERNAL;
  }

  // Feature 59 requires real OS gettid() for owner_tid during resolve().
  uint64_t real_os_tid =
      static_cast<uint64_t>(static_cast<pid_t>(syscall(SYS_gettid)));

  PgrCameraMetaConfig mcfg = {};
  mcfg.version = PGR_CAM_CFG_VERSION;
  mcfg.size = sizeof(mcfg);
  mcfg.binding.version = PGR_MD_CONFIG_VERSION;
  mcfg.binding.size = sizeof(PgrMetaBindConfig);
  mcfg.binding.exports = g_state.cfg.meta_exports;
  mcfg.binding.module_base = g_state.cfg.module_base;
  mcfg.binding.module_size = g_state.cfg.module_size;
  mcfg.binding.seg_query = g_state.cfg.seg_query;
  mcfg.binding.seg_ctx = g_state.cfg.seg_ctx;
  mcfg.thread_current = g_state.cfg.thread_current;
  mcfg.owner_tid = real_os_tid;
  mcfg.domain = domain;
  mcfg.extra = g_state.cfg.meta_extra;
  mcfg.sample_camera = g_state.sample_camera ? 1u : 0u;
  mcfg.continuous = g_state.continuous ? 1u : 0u;

  PgrCameraMetaOutput out = {};
  out.version = PGR_CAM_OUT_VERSION;
  out.size = sizeof(out);
  int rc = meta_api->resolve(&mcfg, &out);
  if(revoked())return PGR_CH_RETRY;

  if (rc < 0) {
    const char *r =
        out.reason[0] != '\0' ? out.reason : "native-camera-metadata-error";
    finish_observation("error", r, r, false, false);
    return PGR_CH_UNAVAILABLE;
  }
  if (rc == PGR_CAM_UNAVAILABLE) {
    const char *r = out.reason[0] != '\0' ? out.reason : "unavailable";
    finish_observation("unavailable", r, nullptr, false, false);
    return PGR_CH_UNAVAILABLE;
  }
  if (rc == PGR_CAM_RETRY) {
    const char *r = out.reason[0] != '\0' ? out.reason : "runtime-not-ready";
    copy_str(g_state.reason, sizeof(g_state.reason), r);
    if (!g_state.continuous) {
      double elapsed =
          now >= g_state.started_at_ms ? (now - g_state.started_at_ms) : 0.0;
      if (g_state.attempts >= PGR_CH_ATTEMPTS_MAX ||
          elapsed + static_cast<double>(PGR_CH_RETRY_MS) >
              static_cast<double>(PGR_CH_PREPARE_MS)) {
        finish_observation("unavailable", r, nullptr, false, true);
        return PGR_CH_UNAVAILABLE;
      }
    }
    return PGR_CH_RETRY;
  }

  g_state.meta = out;
  g_state.reason[0] = '\0';

  if (g_state.sample_camera && g_state.continuous) {
    const PgrCameraSamplerApi *sampler = resolve_sampler_api();
    const PgrCameraMetaExtra *ex = g_state.cfg.meta_extra;
    const PgrMetaExports *me = g_state.cfg.meta_exports;
    if (!sampler || !sampler->install || !ex || !me) {
      finish_observation("unavailable", "native-camera-sampler-unavailable",
                         nullptr, false, false);
      return PGR_CH_ERR_INTERNAL;
    }

    PgrCameraSamplerConfig scfg = {};
    scfg.version = PGR_CS_CONFIG_VERSION;
    scfg.size = sizeof(scfg);
    scfg.runtime_invoke = ex->runtime_invoke;
    scfg.object_get_class = ex->object_get_class;
    scfg.class_is_assignable_from = me->class_is_assignable_from;
    scfg.field_get_value = ex->field_get_value;
    scfg.object_unbox = ex->object_unbox;
    scfg.gchandle_new = const_cast<void *>(ex->gchandle_new);
    scfg.gchandle_get_target = const_cast<void *>(ex->gchandle_get_target);
    scfg.gchandle_free = const_cast<void *>(ex->gchandle_free);
    scfg.thread_current = g_state.cfg.thread_current;
    scfg.string_new = ex->string_new;

    scfg.class_camera = const_cast<void *>(out.classes[C_CAMERA]);
    scfg.class_transform = const_cast<void *>(out.classes[C_TRANSFORM]);
    scfg.class_vector = const_cast<void *>(out.classes[C_VECTOR3]);
    scfg.class_rig = const_cast<void *>(out.classes[C_RIG]);
    scfg.class_int = const_cast<void *>(out.classes[C_INT32]);
    scfg.class_string = const_cast<void *>(out.classes[C_STRING]);

    scfg.m_main = const_cast<void *>(out.methods[M_GET_MAIN]);
    scfg.m_transform = const_cast<void *>(out.methods[M_GET_TRANSFORM]);
    scfg.m_forward = const_cast<void *>(out.methods[M_GET_FORWARD]);
    scfg.m_nametolayer = const_cast<void *>(out.methods[M_NAME_TO_LAYER]);
    scfg.m_culling = const_cast<void *>(out.methods[M_GET_CULLING_MASK]);
    scfg.m_instanceid = const_cast<void *>(out.methods[M_GET_INSTANCE_ID]);

    scfg.f_x = const_cast<void *>(out.fields[F_X]);
    scfg.f_y = const_cast<void *>(out.fields[F_Y]);
    scfg.f_z = const_cast<void *>(out.fields[F_Z]);
    scfg.expected_layer = g_state.cfg.expected_layer;
    scfg.reserved0 = 0;

    int s_rc = sampler->install(&scfg);
    if (s_rc == 0) {
      g_state.sampler_installed = true;
      g_state.use_local_continuous =
          ((g_state.cfg.flags & PGR_CH_FLAG_LOCAL_CONTINUOUS_SAMPLER) != 0) ||
          (g_state.cfg.tid_fn != nullptr);
    } else if (s_rc == PGR_CS_ERR_RECONFIG &&
               g_state.cfg.seg_query != nullptr) {
      g_state.sampler_installed = true;
      g_state.use_local_continuous = true;
    } else {
      finish_observation("unavailable", "native-camera-sampler-install",
                         nullptr, false, false);
      return PGR_CH_ERR_INTERNAL;
    }
    if(revoked())return PGR_CH_RETRY;
    enqueue_sampler_ready_event();

    void *target_code = const_cast<void *>(out.get_main_camera_address);
    if ((g_state.cfg.flags & PGR_CH_FLAG_FIXTURE_HOOK_OVERRIDE) != 0 &&
        g_state.cfg.seg_query != nullptr &&
        g_state.cfg.fixture_getter_code != nullptr) {
      target_code = g_state.cfg.fixture_getter_code;
    }

    if(g_state.hook_poisoned){finish_observation("error","hook-install-failed",nullptr,false,false);return PGR_CH_ERR_HOOK_FAILED;}
    if(g_state.hook_installed){
      if(target_code!=g_state.hooked_getter_target){finish_observation("error","hook-target-changed",nullptr,false,false);return PGR_CH_ERR_RESTART_REQUIRED;}
    }else if(target_code){
      void *orig=nullptr;const int hook_rc=call_hook(target_code,reinterpret_cast<void*>(&hook_get_main_camera),&orig);
      if(orig){g_state.orig_getter.store(reinterpret_cast<MainCameraGetterFn>(orig),std::memory_order_release);g_state.hooked_getter_target=target_code;g_state.hook_installed=true;}
      if(hook_rc||!orig){g_state.hook_poisoned=true;finish_observation("error","hook-install-failed",nullptr,false,false);return PGR_CH_ERR_HOOK_FAILED;}
    }else {finish_observation("error","hook-target-missing",nullptr,false,false);return PGR_CH_ERR_HOOK_FAILED;}

  }

  if(revoked())return PGR_CH_RETRY;
  g_state.ready = true;
  g_state.observed_at_ms = now;
  g_state.last_poll_at_ms = now;
  enqueue_signature_events();
  enqueue_ready_event();
  return PGR_CH_OK;
}

void poll_and_maintain(double now, bool force_poll) {
  if(g_state.stop_requested.load()&&g_state.active)finish_observation("cancelled","stopped",nullptr,false,false);
  if (!g_state.active || g_state.stopping) return;

  if (!g_state.ready) {
    if (!g_state.has_prepared ||
        (now - g_state.last_prepare_at_ms) >=
            static_cast<double>(PGR_CH_RETRY_MS)) {
      attempt_prepare(now);
    }
    return;
  }

  double poll_interval =
      g_state.continuous ? static_cast<double>(PGR_CH_CONTINUOUS_INTERVAL_MS)
                         : static_cast<double>(PGR_CH_POLL_MS);
  if (force_poll || (now - g_state.last_poll_at_ms) >= poll_interval) {
    g_state.last_poll_at_ms = now;
    drain_samples();
  }

  if (g_state.continuous) {
    if (g_state.overflow || g_state.sample_error[0] != '\0') {
      finish_observation(nullptr, nullptr, nullptr, true, false);
    }
    return;
  }

  if (g_state.has_sample_started &&
      (now - g_state.sample_started_at_ms) >=
          static_cast<double>(PGR_CH_SAMPLE_MS)) {
    g_state.sample_done = true;
  }

  double elapsed =
      now >= g_state.observed_at_ms ? (now - g_state.observed_at_ms) : 0.0;
  if (elapsed >= static_cast<double>(PGR_CH_OBSERVE_MS) || g_state.overflow ||
      g_state.sample_error[0] != '\0' || g_state.sample_done) {
    finish_observation(nullptr, nullptr, nullptr, true, false);
  }
}

ThreadEntry *find_or_add_thread(uint64_t tid) {
  for (uint32_t i = 0; i < g_state.threads_count; ++i) {
    if (g_state.threads[i].tid == tid) return &g_state.threads[i];
  }
  if (g_state.threads_count >= PGR_CH_THREADS_MAX) {
    g_state.overflow = true;
    return nullptr;
  }
  ThreadEntry *e = &g_state.threads[g_state.threads_count++];
  e->tid = tid;
  e->count = 0;
  e->depth = 0;
  if (g_state.threads_count == 1) {
    g_state.owner_tid = tid;
  } else if (g_state.sample_camera && g_state.sample_error[0] == '\0') {
    copy_str(g_state.sample_error, sizeof(g_state.sample_error),
             "multiple-matching-threads");
  }
  return e;
}

int api_configure(const PgrCameraHostConfig *config) {
  StateGuard guard;if(!guard.entered)return PGR_CH_RETRY;
  if(g_state.hook_installed){if(config&&memcmp(config,&g_state.cfg,sizeof(*config))==0)return PGR_CH_OK;return PGR_CH_ERR_RESTART_REQUIRED;}
  if (config == nullptr) {
    g_state.cfg = {};
    g_state.cfg.version = PGR_CH_API_VERSION;
    g_state.cfg.size = sizeof(PgrCameraHostConfig);
    g_state.cfg.flags = PGR_CH_FLAG_USE_BOOTSTRAP | PGR_CH_FLAG_USE_TRANSPORT;
    g_state.cfg.expected_layer = PGR_CS_WORLD_MAP_LAYER;
    populate_config_from_bootstrap(&g_state.cfg);
    g_state.configured = true;
    return PGR_CH_OK;
  }
  if (config->version != PGR_CH_API_VERSION ||
      config->size != sizeof(PgrCameraHostConfig) || config->reserved0 != 0) {
    return PGR_CH_ERR_VERSION;
  }
  g_state.cfg = *config;
  if (g_state.cfg.expected_layer <= 0) {
    g_state.cfg.expected_layer = PGR_CS_WORLD_MAP_LAYER;
  }
  if ((g_state.cfg.flags & PGR_CH_FLAG_USE_BOOTSTRAP) != 0) {
    populate_config_from_bootstrap(&g_state.cfg);
  }
  g_state.configured = true;
  g_state.continuous_ever_started = false;
  g_state.local_world_map_layer = -1;
  return PGR_CH_OK;
}

void reset_session_state(double now, bool clear_events) {
  teardown_hooks_and_sampler();
  g_state.active = false;
  g_state.ready = false;
  g_state.stopping = false;
  g_state.stop_requested.store(false,std::memory_order_release);
  g_state.pending_finish = false;
  g_state.pending_finish_status[0] = '\0';
  g_state.pending_finish_reason[0] = '\0';
  g_state.pending_finish_message[0] = '\0';
  g_state.pending_finish_with_summary = false;
  g_state.pending_finish_with_attempts = false;
  g_state.overflow = false;
  g_state.unavailable_reported = false;
  g_state.sample_done = false;
  g_state.has_sampled = false;
  g_state.has_sample_started = false;
  g_state.sample_started_at_ms = 0.0;
  g_state.has_heading_log = false;
  g_state.in_subscriber_pulse = false;
  g_state.explicit_execute_tracking = false;
  g_state.sampling.store(0, std::memory_order_release);
  g_state.sampling_tid = 0;

  g_state.attempts = 0;
  g_state.calls = 0;
  g_state.raw_matches = 0;
  g_state.matches = 0;
  g_state.threads_count = 0;
  memset(g_state.threads, 0, sizeof(g_state.threads));
  g_state.owner_tid = 0;

  g_state.sample_count = 0;
  g_state.sample_attempts = 0;
  g_state.last_error = PGR_CH_OK;
  g_state.reason[0] = '\0';
  g_state.sample_error[0] = '\0';
  g_state.sample_failure = {};

  g_state.started_at_ms = now;
  g_state.observed_at_ms = 0.0;
  g_state.last_prepare_at_ms = 0.0;
  g_state.has_prepared = false;
  g_state.last_poll_at_ms = now;
  g_state.last_sample_at_ms = 0.0;
  g_state.last_heading_log_ms = 0.0;
  g_state.lease_until_ms = 0.0;
  g_state.has_last_clock = false;
  g_state.last_clock_ms = 0.0;

  g_state.meta = {};
  g_state.latest_sample = {};
  g_state.pending_unavailable = {};
  memset(g_state.bounded_queue, 0, sizeof(g_state.bounded_queue));
  g_state.bounded_head = 0;
  g_state.bounded_count = 0;
  g_state.last_bounded_reading[0] = '\0';

  if (clear_events) {
    g_state.event_head = 0;
    g_state.event_count = 0;
  }
}

int api_start(const PgrCameraHostOptions *options) {
  StateGuard guard;if(!guard.entered)return PGR_CH_RETRY;
  if(g_state.sampling.load()||g_state.getter_depth.load())return PGR_CH_RETRY;
  if (!options) return PGR_CH_ERR_INVALID_ARG;
  if (options->version != PGR_CH_API_VERSION ||
      options->size != sizeof(PgrCameraHostOptions)) {
    return PGR_CH_ERR_VERSION;
  }
  if (!g_state.configured) {
    api_configure(nullptr);
  }
  if (options->enabled == 0) {
    return PGR_CH_OK;
  }
  if (g_state.active) {
    return PGR_CH_ERR_ALREADY_RUNNING;
  }

  double now = options->now_ms != 0 ? static_cast<double>(options->now_ms)
                                    : fallback_now_ms();
  reset_session_state(now, true);

  g_state.active = true;
  g_state.sample_camera = options->sample_camera != 0;
  g_state.continuous = options->continuous != 0;

  if (g_state.continuous && !g_state.sample_camera) {
    finish_observation("unavailable", "continuous-requires-camera-sampling",
                       nullptr, false, false);
    return PGR_CH_UNAVAILABLE;
  }
  if (g_state.continuous && g_state.continuous_ever_started) {
    finish_observation("unavailable", "continuous-restart-required", nullptr,
                       false, false);
    return PGR_CH_ERR_RESTART_REQUIRED;
  }
  if (g_state.continuous) {
    g_state.continuous_ever_started = true;
  }

  if (options->sample_requested != 0) {
    uint32_t lease = options->lease_ms != 0 ? options->lease_ms
                                            : PGR_CH_CONTINUOUS_LEASE_MS;
    if (lease > PGR_CH_CONTINUOUS_LEASE_MS) lease = PGR_CH_CONTINUOUS_LEASE_MS;
    g_state.lease_until_ms = now + static_cast<double>(lease);
  }

  if (g_state.sample_camera && !g_state.continuous) {
    emit_transport_bool(kActionSetCameraDiagnosticRunning, true);
    emit_transport_string(kActionCameraDiagnostic,
                          "Checking camera thread...");
  }

  return attempt_prepare(now);
}

int api_set_options(const PgrCameraHostOptions *options) {
  StateGuard guard;if(!guard.entered)return PGR_CH_RETRY;
  if (!options) return PGR_CH_ERR_INVALID_ARG;
  if (options->version != PGR_CH_API_VERSION ||
      options->size != sizeof(PgrCameraHostOptions)) {
    return PGR_CH_ERR_VERSION;
  }

  double now = options->now_ms != 0 ? static_cast<double>(options->now_ms)
                                    : fallback_now_ms();
  if (options->enabled == 0) {
    if (g_state.active) {
      finish_observation("cancelled", "stopped", nullptr, false, false);
    }
    return PGR_CH_OK;
  }

  if (!g_state.active) {
    return api_start(options);
  }

  if ((options->flags & PGR_CH_OPT_REQUEST_FAILED) != 0) {
    if (g_state.sample_error[0] == '\0') {
      copy_str(g_state.sample_error, sizeof(g_state.sample_error),
               "sampling-request-failed");
    }
  }
  if ((options->flags & PGR_CH_OPT_CLEAR_LEASE) != 0 ||
      options->sample_requested == 0) {
    g_state.lease_until_ms = 0.0;
  }
  if (options->sample_requested != 0) {
    uint32_t lease = options->lease_ms != 0 ? options->lease_ms
                                            : PGR_CH_CONTINUOUS_LEASE_MS;
    if (lease > PGR_CH_CONTINUOUS_LEASE_MS) lease = PGR_CH_CONTINUOUS_LEASE_MS;
    g_state.lease_until_ms = now + static_cast<double>(lease);
  }

  if ((options->flags & PGR_CH_OPT_FORCE_POLL) != 0) {
    poll_and_maintain(now, true);
  }
  return PGR_CH_OK;
}

int api_on_execute_enter(uint64_t tid, uint32_t is_execute_tasks) {
  StateGuard guard;if(!guard.entered)return PGR_CH_RETRY;
  if (!g_state.active || !g_state.ready || revoked() ||
      g_state.overflow || g_state.sample_error[0] != '\0') {
    return PGR_CH_OK;
  }
  if (tid == 0) tid = current_tid();
  g_state.explicit_execute_tracking = true;

  if (g_state.sampling.load(std::memory_order_acquire) != 0) {
    if (is_execute_tasks != 0 && tid != g_state.sampling_tid &&
        g_state.sample_error[0] == '\0') {
      copy_str(g_state.sample_error, sizeof(g_state.sample_error),
               "multiple-matching-threads");
    }
    return PGR_CH_OK;
  }

  g_state.calls++;
  if (g_state.calls > PGR_CH_CALLS_MAX) {
    g_state.overflow = true;
    return PGR_CH_OK;
  }
  if (is_execute_tasks == 0) {
    return PGR_CH_OK;
  }

  g_state.raw_matches++;
  ThreadEntry *e = find_or_add_thread(tid);
  if (!e) return PGR_CH_OK;

  if (e->depth == 0) {
    g_state.matches++;
    e->count++;
  }
  e->depth++;
  return PGR_CH_OK;
}

int api_on_execute_leave(uint64_t tid, uint32_t is_execute_tasks) {
  StateGuard guard;if(!guard.entered)return PGR_CH_RETRY;
  if (!g_state.active || !g_state.ready) return PGR_CH_OK;
  if (tid == 0) tid = current_tid();
  if (is_execute_tasks == 0) return PGR_CH_OK;

  ThreadEntry *target = nullptr;
  for (uint32_t i = 0; i < g_state.threads_count; ++i) {
    if (g_state.threads[i].tid == tid) {
      target = &g_state.threads[i];
      break;
    }
  }
  if (!target || target->depth == 0) return PGR_CH_OK;

  target->depth--;
  if (target->depth == 0 && !g_state.continuous) {
    execute_sample_locked(tid, nullptr, nullptr, false);
  }
  return PGR_CH_OK;
}

int api_on_main_camera_getter(void *rig, void *returned_camera, uint64_t tid) {
  StateGuard guard;if(!guard.entered)return PGR_CH_RETRY;
  if (tid == 0) tid = current_tid();
  if (g_state.getter_depth.load(std::memory_order_acquire) != 0 ||
      tls_getter_depth != 0) {
    return PGR_CH_OK;
  }
  execute_sample_locked(tid, rig, returned_camera, true);
  return PGR_CH_OK;
}

int api_tick(uint64_t tid) {
  (void)tid;
  StateGuard guard;if(!guard.entered)return PGR_CH_RETRY;
  double now = fallback_now_ms();
  sync_transport_demand(now);
  poll_and_maintain(now, true);
  return g_state.event_count > 0 ? PGR_CH_HAS_EVENT : PGR_CH_OK;
}

int api_next_event(PgrCameraHostEvent *out_event) {
  if (!out_event) return PGR_CH_ERR_INVALID_ARG;
  if (out_event->version != PGR_CH_API_VERSION ||
      out_event->size != sizeof(PgrCameraHostEvent)) {
    return PGR_CH_ERR_VERSION;
  }
  StateGuard guard;if(!guard.entered)return PGR_CH_RETRY;
  if (g_state.event_count == 0) {
    out_event->kind = PGR_CH_EVENT_NONE;
    return PGR_CH_OK;
  }
  *out_event = g_state.events[g_state.event_head];
  g_state.event_head = (g_state.event_head + 1) % PGR_CH_EVENT_QUEUE_MAX;
  g_state.event_count--;
  return PGR_CH_HAS_EVENT;
}

int api_stop(const char *reason) {
  g_state.stop_requested.store(true,std::memory_order_release);
  StateGuard guard;if(!guard.entered)return PGR_CH_OK;
  if (!g_state.active) return PGR_CH_OK;
  const char *r =
      (reason != nullptr && reason[0] != '\0') ? reason : "cancelled";
  finish_observation("cancelled", r, nullptr, false, false);
  return PGR_CH_OK;
}

int api_sample(PgrCameraHostSample *out_sample) {
  if (!out_sample) return PGR_CH_ERR_INVALID_ARG;
  if (out_sample->version != PGR_CH_API_VERSION ||
      out_sample->size != sizeof(PgrCameraHostSample)) {
    return PGR_CH_ERR_VERSION;
  }
  StateGuard guard;if(!guard.entered)return PGR_CH_RETRY;
  double now = fallback_now_ms();
  memset(out_sample, 0, sizeof(*out_sample));
  out_sample->version = PGR_CH_API_VERSION;
  out_sample->size = sizeof(PgrCameraHostSample);
  out_sample->active = g_state.active ? 1 : 0;
  out_sample->ready = g_state.ready ? 1 : 0;
  out_sample->stopping = revoked() ? 1 : 0;
  out_sample->sample_camera = g_state.sample_camera ? 1 : 0;
  out_sample->continuous = g_state.continuous ? 1 : 0;
  out_sample->overflow = g_state.overflow ? 1 : 0;
  out_sample->unavailable_reported = g_state.unavailable_reported ? 1 : 0;
  out_sample->hook_installed = g_state.hook_installed ? 1 : 0;
  out_sample->vector_fields = g_state.meta.vector_fields;
  out_sample->attempts = g_state.attempts;
  out_sample->calls = g_state.calls;
  out_sample->raw_matches = g_state.raw_matches;
  out_sample->matches = g_state.matches;
  out_sample->threads_count = g_state.threads_count;
  out_sample->active_matches = total_active_depth();
  out_sample->sample_count = g_state.sample_count;
  out_sample->sample_attempts = g_state.sample_attempts;
  out_sample->queued_events = g_state.event_count;
  out_sample->owner_tid = g_state.owner_tid;
  out_sample->execute_tasks_method =
      reinterpret_cast<uintptr_t>(g_state.meta.execute);
  out_sample->get_main_camera_address =
      reinterpret_cast<uintptr_t>(g_state.meta.get_main_camera_address);
  out_sample->remaining_lease_ms = remaining_lease_ms(now);
  out_sample->last_error = g_state.last_error;
  copy_str(out_sample->reason, sizeof(out_sample->reason),
           g_state.sample_error[0] ? g_state.sample_error : g_state.reason);
  if (g_state.sample_failure.valid) {
    copy_str(out_sample->failure_stage, sizeof(out_sample->failure_stage),
             g_state.sample_failure.stage);
    copy_str(out_sample->failure_name, sizeof(out_sample->failure_name),
             g_state.sample_failure.error_name);
    copy_str(out_sample->failure_message, sizeof(out_sample->failure_message),
             g_state.sample_failure.message);
  }
  return PGR_CH_OK;
}

bool valid_pulse(const PgrUnityPulse *p){
 if(!p||p->reserved||p->tid<=0||uint64_t(p->tid)!=current_tid())return false;
 const auto core=resolve_native_api();const auto scheduler=core&&core->query_feature?static_cast<const PgrUnitySchedulerApi*>(core->query_feature(48,2)):nullptr;
 auto thread=reinterpret_cast<Il2CppThreadCurrentFn>(g_state.cfg.thread_current);
 return scheduler&&scheduler->valid(p->generation)==1&&thread&&thread();
}
void api_on_unity_begin(void *ctx, const PgrUnityPulse *pulse) {
  (void)ctx;
  StateGuard guard;if(!guard.entered)return;
  if(!valid_pulse(pulse))return;
  struct SubscriberGuard{bool previous;SubscriberGuard():previous(g_state.in_subscriber_pulse){g_state.in_subscriber_pulse=true;}~SubscriberGuard(){g_state.in_subscriber_pulse=previous;}} subscriber_guard;
  uint64_t tid =
      (pulse != nullptr && pulse->tid > 0)
          ? static_cast<uint64_t>(static_cast<uint32_t>(pulse->tid))
          : current_tid();
  double now = fallback_now_ms();

  if (g_state.active && !g_state.ready && !revoked()) {
    if (!g_state.has_prepared ||
        (now - g_state.last_prepare_at_ms) >=
            static_cast<double>(PGR_CH_RETRY_MS)) {
      attempt_prepare(now);
    }
  }

  if (g_state.active && g_state.ready && !revoked() &&
      !g_state.explicit_execute_tracking) {
    g_state.calls++;
    if (g_state.calls > PGR_CH_CALLS_MAX) {
      g_state.overflow = true;
    } else {
      g_state.raw_matches++;
      ThreadEntry *e = find_or_add_thread(tid);
      if (e) {
        g_state.matches++;
        e->count++;
      }
    }
  }
}

void api_on_unity_end(void *ctx, const PgrUnityPulse *pulse) {
  (void)ctx;
  StateGuard guard;if(!guard.entered)return;
  if(!valid_pulse(pulse))return;
  struct SubscriberGuard{bool previous;SubscriberGuard():previous(g_state.in_subscriber_pulse){g_state.in_subscriber_pulse=true;}~SubscriberGuard(){g_state.in_subscriber_pulse=previous;}} subscriber_guard;
  uint64_t tid =
      (pulse != nullptr && pulse->tid > 0)
          ? static_cast<uint64_t>(static_cast<uint32_t>(pulse->tid))
          : current_tid();
  double now = fallback_now_ms();

  if (g_state.active && g_state.ready && !revoked()) {
    if (g_state.sample_camera && !g_state.continuous &&
        !g_state.explicit_execute_tracking) {
      execute_sample_locked(tid, nullptr, nullptr, false);
    }
    sync_transport_demand(now);
    poll_and_maintain(now, false);
  }
}

void api_bind_core_handle(void *lib_handle) {
  StateGuard guard;if(!guard.entered)return;
  g_state.core_handle = lib_handle;
}

const PgrCameraHostApi kApi = {
    .version = PGR_CH_API_VERSION,
    .size = sizeof(PgrCameraHostApi),
    .configure = api_configure,
    .start = api_start,
    .set_options = api_set_options,
    .tick = api_tick,
    .on_execute_enter = api_on_execute_enter,
    .on_execute_leave = api_on_execute_leave,
    .on_main_camera_getter = api_on_main_camera_getter,
    .next_event = api_next_event,
    .stop = api_stop,
    .sample = api_sample,
    .on_unity_begin = api_on_unity_begin,
    .on_unity_end = api_on_unity_end,
    .bind_core_handle = api_bind_core_handle,
};

}  // namespace

const void *pgrazor_camera_host_api(uint32_t version) {
  if (version != PGR_CH_API_VERSION) return nullptr;
  return &kApi;
}

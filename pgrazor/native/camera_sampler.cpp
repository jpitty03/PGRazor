#include "camera_sampler.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 41: synchronous, read-only continuous camera sampling. No hooks, no callbacks, no retained managed
// state. Every temporary managed value is rooted on the owner thread and freed before the output is
// published; the output is stamped only AFTER all roots are released, with the busy guard still held. Built
// -fno-exceptions / -fno-rtti like every sibling: managed exceptions surface through runtime_invoke's exc
// out-param, never a C++ throw. No mutex is held across a managed call.

static_assert(sizeof(PgrCameraSamplerConfig) == 216, "config 216");
static_assert(sizeof(PgrCameraSamplerRequest) == 40, "request 40");
static_assert(sizeof(PgrCameraSamplerOutput) == 72, "output 72");
static_assert(sizeof(PgrCameraSamplerApi) == 24, "table 24");
static_assert(offsetof(PgrCameraSamplerConfig, f_z) == 200, "config fields");
static_assert(offsetof(PgrCameraSamplerConfig, expected_layer) == 208, "config expected_layer");
static_assert(offsetof(PgrCameraSamplerConfig, reserved0) == 212, "config reserved0");
static_assert(offsetof(PgrCameraSamplerRequest, rig) == 24, "request rig");
static_assert(offsetof(PgrCameraSamplerRequest, camera) == 32, "request camera");
static_assert(offsetof(PgrCameraSamplerOutput, mask) == 20, "output mask");
static_assert(offsetof(PgrCameraSamplerOutput, layer) == 24, "output layer");
static_assert(offsetof(PgrCameraSamplerOutput, x) == 32, "output x");
static_assert(offsetof(PgrCameraSamplerOutput, time_ms) == 64, "output time_ms");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_CS_MAX_LEASE_MS;
constexpr int kConfigPtrCount = 25;  // validated pointers after the 8-byte header
constexpr double kPi = 3.14159265358979323846;

using New        = uintptr_t (*)(void *, int);
using Get        = void *(*)(uintptr_t);
using FreeH      = void (*)(uintptr_t);
using ObjClass   = void *(*)(void *);
using Invoke     = void *(*)(const void *, void *, void **, void **);
using Unbox      = void *(*)(void *);
using FieldGet   = void (*)(void *, void *, void *);
using Assignable = bool (*)(void *, void *);
using ThreadCur  = void *(*)();
using StringNew  = void *(*)(const char *);

const char kWorldMap[] = "WorldMap";

std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrCameraSamplerConfig g_cfg{};
std::atomic<bool> g_installed{false};

// Permanent Unity owner, latched once from a genuine on-thread call.
std::atomic<uint64_t> g_owner_tid{0};
// Verified WorldMap layer index, cached after the first successful NameToLayer("WorldMap") == 13. -1 unknown.
std::atomic<int32_t> g_world_map_layer{-1};
bool g_busy = false;  // a sample, INCLUDING root cleanup, is in flight (guarded by g_state_mutex)

uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

ObjClass obj_class() { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }
bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }
bool is_assignable(void *expected, void *obj) {
  void *actual = obj ? obj_class()(obj) : nullptr;
  return actual != nullptr && reinterpret_cast<Assignable>(g_cfg.class_is_assignable_from)(expected, actual);
}

// Owning temporary root. set() roots a raw managed object; the destructor frees any live handle so an early
// return always balances every root allocated on that path. No handle ever leaves this module.
struct Root {
  uintptr_t handle = 0;
  Root() = default;
  Root(const Root &) = delete;
  Root &operator=(const Root &) = delete;
  ~Root() { if (handle) reinterpret_cast<FreeH>(g_cfg.gchandle_free)(handle); }
  void *get() const { return handle ? reinterpret_cast<Get>(g_cfg.gchandle_get_target)(handle) : nullptr; }
  bool set(void *object) {
    if (!object) return false;
    handle = reinterpret_cast<New>(g_cfg.gchandle_new)(object, 0);
    return handle && get();
  }
};

// Busy reset runs AFTER the Roots declared below it are destroyed, so reentrant root cleanup still observes
// busy == true (the guard spans destruction/root cleanup).
struct BusyExit { ~BusyExit() { std::lock_guard<std::mutex> lock(g_state_mutex); g_busy = false; } };

// Scalar sampling result, filled entirely from roots before any publication.
struct Result {
  int status = PGR_CS_UNAVAILABLE;
  uint32_t reason = PGR_CS_REASON_NONE;
  int32_t camera_id = 0;
  uint32_t mask = 0;
  int32_t layer = 0;
  double x = 0, y = 0, z = 0, yaw = 0, time_ms = 0;
};

// Check the window between managed operations and again after all root cleanup.
bool within_window(uint64_t deadline, Result &r) {
  uint64_t now=0;
  if (!read_boottime_ms(&now)) { r.status=PGR_CS_ERR_CLOCK; r.reason=PGR_CS_REASON_CLOCK; return false; }
  if (now>=deadline) { r.status=PGR_CS_UNAVAILABLE; r.reason=PGR_CS_REASON_LEASE; return false; }
  if (!reinterpret_cast<ThreadCur>(g_cfg.thread_current)()) { r.status=PGR_CS_UNAVAILABLE; r.reason=PGR_CS_REASON_THREAD; return false; }
  return true;
}

// Invoke a method and root its (possibly null) return value. receiver null => static call; otherwise the
// receiver is re-resolved from its root (relocating collector). Returns one of CALL_*.
enum CallResult { CALL_OK, CALL_THROW, CALL_GCHANDLE, CALL_DEAD };

CallResult call(void *method, const Root *receiver, void **args, Root &out, bool allowNull) {
  void *self = nullptr;
  if (receiver) {
    self = receiver->get();
    if (!self) return CALL_DEAD;  // receiver collected despite its root (never expected)
  }
  void *exc = nullptr;
  void *value = reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(method, self, args, &exc);
  if (exc) return CALL_THROW;
  if (!value) return allowNull ? CALL_OK : CALL_GCHANDLE;
  return out.set(value) ? CALL_OK : CALL_GCHANDLE;
}

// Map a non-OK CallResult to its fault status and set the reason. Returns 0 for CALL_OK.
int call_fault(CallResult cr, Result &r) {
  switch (cr) {
    case CALL_THROW:    r.reason = PGR_CS_REASON_MANAGED;  return PGR_CS_ERR_MANAGED;
    case CALL_GCHANDLE: r.reason = PGR_CS_REASON_GCHANDLE; return PGR_CS_ERR_GCHANDLE;
    case CALL_DEAD:     r.reason = PGR_CS_REASON_GCHANDLE; return PGR_CS_ERR_GCHANDLE;
    default:            return 0;
  }
}

// Unbox a boxed System.Int32 rooted in `boxed`, re-resolving its target. false on a null/unboxable value.
bool unbox_i32(const Root &boxed, int32_t *out) {
  void *obj = boxed.get();
  if (!obj) return false;
  void *value = reinterpret_cast<Unbox>(g_cfg.object_unbox)(obj);
  if (!value) return false;
  std::memcpy(out, value, sizeof(int32_t));
  return true;
}

// Read one Vector3 float field from the CURRENT vector root into a double. false if the root is dead.
bool read_axis(const Root &vec, void *field, double *out) {
  void *obj = vec.get();
  if (!obj) return false;
  float value = 0.0f;
  reinterpret_cast<FieldGet>(g_cfg.field_get_value)(obj, field, &value);
  *out = static_cast<double>(value);
  return true;
}

// Resolve the verified WorldMap layer index (cached after the first verified == 13). Returns 0 and sets
// *out_layer on success; otherwise returns a negative fault and sets r.reason.
int ensure_world_map_layer(int32_t *out_layer, uint64_t deadline, Result &r) {
  const int32_t cached = g_world_map_layer.load(std::memory_order_acquire);
  if (cached >= 0) { *out_layer = cached; return 0; }

  if (!within_window(deadline,r)) return r.status;
  void *str_raw = reinterpret_cast<StringNew>(g_cfg.string_new)(kWorldMap);
  Root str;
  if (!str.set(str_raw)) { r.reason = PGR_CS_REASON_GCHANDLE; return PGR_CS_ERR_GCHANDLE; }
  if (!is_exact(str.get(), g_cfg.class_string)) { r.reason = PGR_CS_REASON_METADATA; return PGR_CS_ERR_METADATA; }

  // NameToLayer(string) -> boxed System.Int32. The string object is the reference argument, passed directly.
  if (!within_window(deadline,r)) return r.status;
  Root box;
  void *args[] = {str.get()};
  if (int fault = call_fault(call(g_cfg.m_nametolayer, nullptr, args, box, /*allowNull=*/false), r)) return fault;
  if (!is_exact(box.get(), g_cfg.class_int)) { r.reason = PGR_CS_REASON_METADATA; return PGR_CS_ERR_METADATA; }
  int32_t layer = 0;
  if (!unbox_i32(box, &layer)) { r.reason = PGR_CS_REASON_GCHANDLE; return PGR_CS_ERR_GCHANDLE; }
  if (layer != g_cfg.expected_layer) { r.reason = PGR_CS_REASON_LAYER; return PGR_CS_ERR_LAYER; }

  g_world_map_layer.store(layer, std::memory_order_release);
  *out_layer = layer;
  return 0;
}

// All managed work for one sample. Every root is local; the scalar outcome is written into r. Returns the
// status (1 / 0 / negative). The deadline is re-sampled here (the last managed step is complete) and the
// roots are freed when this function returns, before the caller publishes.
int run_sample(const PgrCameraSamplerRequest *request, uint64_t deadline, Result &r) {
  // (1) RAW rig: class proof ONLY, taken before any gchandle_new/runtime_invoke. A non-MainCameraRig rig is
  // the off-map case -> benign 0. The rig pointer is never retained or re-read.
  if (!is_exact(request->rig, g_cfg.class_rig)) { r.reason = PGR_CS_REASON_NON_MAP_RIG; return PGR_CS_UNAVAILABLE; }

  // (2) RAW returned camera: null is benign; otherwise root it FIRST, before any other allocation.
  if (!request->camera) { r.reason = PGR_CS_REASON_NULL_CAMERA; return PGR_CS_UNAVAILABLE; }
  Root natural;
  if (!natural.set(request->camera)) { r.reason = PGR_CS_REASON_GCHANDLE; return PGR_CS_ERR_GCHANDLE; }
  if (!is_exact(natural.get(), g_cfg.class_camera)) { r.reason = PGR_CS_REASON_METADATA; return PGR_CS_ERR_METADATA; }

  // (3) Camera.get_main() must match the returned camera BEFORE any queries.
  if (!within_window(deadline,r)) return r.status;
  Root main;
  if (int fault = call_fault(call(g_cfg.m_main, nullptr, nullptr, main, /*allowNull=*/true), r)) return fault;
  if (!main.get()) { r.reason = PGR_CS_REASON_NULL_CAMERA; return PGR_CS_UNAVAILABLE; }
  if (!is_exact(main.get(), g_cfg.class_camera)) { r.reason = PGR_CS_REASON_METADATA; return PGR_CS_ERR_METADATA; }
  if (natural.get() != main.get()) { r.reason = PGR_CS_REASON_CAMERA_MISMATCH; return PGR_CS_UNAVAILABLE; }

  // (4) WorldMap layer (verified == 13 / cached) and the culling-mask gate.
  int32_t layer = 0;
  if (!within_window(deadline,r)) return r.status;
  if (int fault = ensure_world_map_layer(&layer, deadline, r)) return fault;
  if (!within_window(deadline,r)) return r.status;
  r.layer = layer;

  if (!within_window(deadline,r)) return r.status;
  Root mask_box;
  if (int fault = call_fault(call(g_cfg.m_culling, &main, nullptr, mask_box, /*allowNull=*/false), r)) return fault;
  if (!is_exact(mask_box.get(), g_cfg.class_int)) { r.reason = PGR_CS_REASON_METADATA; return PGR_CS_ERR_METADATA; }
  int32_t mask = 0;
  if (!unbox_i32(mask_box, &mask)) { r.reason = PGR_CS_REASON_GCHANDLE; return PGR_CS_ERR_GCHANDLE; }
  r.mask = static_cast<uint32_t>(mask);
  if ((r.mask & (1u << static_cast<uint32_t>(layer))) == 0) {
    r.reason = PGR_CS_REASON_MAP_HIDDEN;  // output carries mask + layer
    return PGR_CS_UNAVAILABLE;
  }

  // (5) Transform (assignable) and its forward vector (exact Vector3).
  if (!within_window(deadline,r)) return r.status;
  Root transform;
  if (int fault = call_fault(call(g_cfg.m_transform, &main, nullptr, transform, /*allowNull=*/true), r)) return fault;
  if (!transform.get()) { r.reason = PGR_CS_REASON_NULL_TRANSFORM; return PGR_CS_UNAVAILABLE; }
  if (!is_assignable(g_cfg.class_transform, transform.get())) {
    r.reason = PGR_CS_REASON_METADATA; return PGR_CS_ERR_METADATA;
  }
  if (!within_window(deadline,r)) return r.status;
  Root vector;
  if (int fault = call_fault(call(g_cfg.m_forward, &transform, nullptr, vector, /*allowNull=*/false), r)) return fault;
  if (!is_exact(vector.get(), g_cfg.class_vector)) { r.reason = PGR_CS_REASON_METADATA; return PGR_CS_ERR_METADATA; }

  double x = 0, y = 0, z = 0;
  if (!read_axis(vector, g_cfg.f_x, &x) || !read_axis(vector, g_cfg.f_y, &y) || !read_axis(vector, g_cfg.f_z, &z)) {
    r.reason = PGR_CS_REASON_GCHANDLE; return PGR_CS_ERR_GCHANDLE;
  }
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
    r.reason = PGR_CS_REASON_NONFINITE; return PGR_CS_ERR_VECTOR;
  }
  if (std::sqrt(x * x + z * z) < 1e-6) { r.reason = PGR_CS_REASON_VERTICAL; return PGR_CS_ERR_VECTOR; }
  r.x = x; r.y = y; r.z = z;
  r.yaw = std::fmod(std::atan2(x, z) * 180.0 / kPi + 360.0, 360.0);

  // (6) Object.GetInstanceID() -> boxed Int32.
  if (!within_window(deadline,r)) return r.status;
  Root id_box;
  if (int fault = call_fault(call(g_cfg.m_instanceid, &main, nullptr, id_box, /*allowNull=*/false), r)) return fault;
  if (!is_exact(id_box.get(), g_cfg.class_int)) { r.reason = PGR_CS_REASON_METADATA; return PGR_CS_ERR_METADATA; }
  int32_t id = 0;
  if (!unbox_i32(id_box, &id)) { r.reason = PGR_CS_REASON_GCHANDLE; return PGR_CS_ERR_GCHANDLE; }
  r.camera_id = id;

  // (7) Fresh pre-publication lease gate (CLOCK_BOOTTIME), after every managed read.
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) { r.reason = PGR_CS_REASON_CLOCK; return PGR_CS_ERR_CLOCK; }
  if (now >= deadline) { r.reason = PGR_CS_REASON_LEASE; return PGR_CS_UNAVAILABLE; }

  r.reason = PGR_CS_REASON_OK;
  return PGR_CS_SAMPLE;
}

void init_output(PgrCameraSamplerOutput *out) {
  std::memset(out, 0, sizeof(*out));
  out->version = PGR_CS_OUTPUT_VERSION;
  out->size = sizeof(PgrCameraSamplerOutput);
}

// Publish r into *out (which was zeroed by init_output). Emits no garbage: success fills everything, a 0
// refusal fills reason + time_ms (plus mask/layer only on map-hidden), a fault fills status + reason only.
void publish(PgrCameraSamplerOutput *out, const Result &r) {
  out->status = r.status;
  out->reason = r.reason;
  if (r.status >= 0) out->time_ms = r.time_ms;
  if (r.status == PGR_CS_SAMPLE) {
    out->camera_id = r.camera_id;
    out->mask = r.mask;
    out->layer = r.layer;
    out->x = r.x; out->y = r.y; out->z = r.z; out->yaw = r.yaw;
  } else if (r.status == PGR_CS_UNAVAILABLE && r.reason == PGR_CS_REASON_MAP_HIDDEN) {
    out->mask = r.mask;
    out->layer = r.layer;
  }
}

// Supplied tid must be nonzero AND equal the current OS tid BEFORE the owner is latched; a later foreign tid
// refuses. Mirrors the sibling owner latch.
bool owner_ready(uint64_t tid) {
  if (tid == 0 || current_tid() != tid) return false;
  uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (!(owner == 0 || owner == tid)) return false;
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
  return g_owner_tid.load(std::memory_order_acquire) == tid;
}

bool check_request(const PgrCameraSamplerRequest *request, int *rc) {
  if (!g_installed.load(std::memory_order_acquire)) { *rc = PGR_CS_ERR_NOT_INSTALLED; return false; }
  if (request == nullptr) { *rc = PGR_CS_ERR_NULL_ARG; return false; }
  if (request->version != PGR_CS_REQUEST_VERSION) { *rc = PGR_CS_ERR_BAD_VERSION; return false; }
  if (request->size != sizeof(PgrCameraSamplerRequest)) { *rc = PGR_CS_ERR_BAD_SIZE; return false; }
  *rc = 0;
  return true;
}

// --- config validation / install ---------------------------------------------------
bool validate_config(const PgrCameraSamplerConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_CS_ERR_NULL_ARG; return false; }
  if (c->version != PGR_CS_CONFIG_VERSION) { *rc = PGR_CS_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrCameraSamplerConfig)) { *rc = PGR_CS_ERR_BAD_SIZE; return false; }
  void *ptrs[kConfigPtrCount];
  std::memcpy(ptrs, reinterpret_cast<const unsigned char *>(c) + offsetof(PgrCameraSamplerConfig, runtime_invoke),
              sizeof(ptrs));
  for (int i = 0; i < kConfigPtrCount; ++i)
    if (ptrs[i] == nullptr) { *rc = PGR_CS_ERR_MISSING_PTR; return false; }
  if (c->expected_layer != PGR_CS_WORLD_MAP_LAYER || c->reserved0 != 0) { *rc = PGR_CS_ERR_BAD_CONFIG; return false; }
  *rc = 0;
  return true;
}

int api_install(const PgrCameraSamplerConfig *config) {
  int rc = 0;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_CS_ERR_CLOCK;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    return std::memcmp(&g_cfg, config, sizeof(*config)) == 0 ? 0 : PGR_CS_ERR_RECONFIG;
  }
  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;
    g_busy = false;
  }
  g_installed.store(true, std::memory_order_release);
  return 0;
}

int api_sample(const PgrCameraSamplerRequest *request, PgrCameraSamplerOutput *out) {
  if (out == nullptr) return PGR_CS_ERR_NULL_ARG;  // cannot report
  init_output(out);
  int rc = 0;
  if (!check_request(request, &rc)) { out->status=rc; return rc; }

  Result r;
  // Owner latch + attached-thread gate (benign 0 / reason thread on failure).
  if (request->tid==0 || current_tid()!=request->tid || !reinterpret_cast<ThreadCur>(g_cfg.thread_current)() || !owner_ready(request->tid)) { r.reason = PGR_CS_REASON_THREAD; publish(out, r); return PGR_CS_UNAVAILABLE; }
  if (reinterpret_cast<ThreadCur>(g_cfg.thread_current)() == nullptr) {
    r.reason = PGR_CS_REASON_THREAD; publish(out, r); return PGR_CS_UNAVAILABLE;
  }
  const uint64_t remaining = request->remaining_ms > kMaxLeaseMs ? kMaxLeaseMs : request->remaining_ms;
  if (remaining == 0) { r.reason = PGR_CS_REASON_OFF; publish(out, r); return PGR_CS_UNAVAILABLE; }
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) { r.status = PGR_CS_ERR_CLOCK; r.reason = PGR_CS_REASON_CLOCK; publish(out, r); return PGR_CS_ERR_CLOCK; }
  r.time_ms = static_cast<double>(now);
  const uint64_t deadline = now + remaining;

  { std::lock_guard<std::mutex> lock(g_state_mutex); if (g_busy) { r.status = PGR_CS_ERR_BUSY; r.reason = PGR_CS_REASON_BUSY; publish(out, r); return PGR_CS_ERR_BUSY; } g_busy = true; }
  BusyExit busy_exit;

  // All managed work (and its roots) lives in run_sample; roots are freed when it returns, before publish.
  r.status = run_sample(request, deadline, r);
  if (r.status>=0) within_window(deadline,r);
  publish(out, r);
  return r.status;
}

const PgrCameraSamplerApi kApiTable = {
    PGR_CS_API_VERSION, static_cast<uint32_t>(sizeof(PgrCameraSamplerApi)),
    &api_install, &api_sample,
};

}  // namespace

extern "C" const void *pgrazor_camera_sampler_api(uint32_t version) {
  if (version != PGR_CS_API_VERSION) return nullptr;
  return &kApiTable;
}

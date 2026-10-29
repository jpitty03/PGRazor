#include "map_tap.h"
#include "hook_engine.h"

#include <atomic>
#include <mutex>
#include <cstring>
#include <cmath>
#include <time.h>
#include <unistd.h>

// Feature 13 map-tap. See map_tap.h for the ownership contract.
//
// Threading model (the invariants this file enforces):
//   * Ground.OnTap / MapView.GetGroundPosition run on the game's Unity thread. The
//     first on-thread observation whose tid matches the policy's proven_tid latches
//     g_owner_tid once, permanently. A policy update from another thread can only
//     disable; it can never reassign the owner (callbacks else chain the original).
//   * A coordinate is captured only inside a live, verified Ground.OnTap scope
//     (g_scope_gen != 0) established by hook_tap on this same owner thread. The
//     generation re-validates the scope before and after each managed call.
//   * The native busy guard (g_call_depth) protects against map recursion -- a
//     GetGroundPosition reentered while we are already capturing simply chains its
//     original and captures nothing. This replaces the feature's own old JS `s.busy`
//     guard; it does NOT provide cross-feature exclusion. As with native quick-spin,
//     once this controller is selected the coordinator skips the legacy Gum map-tap
//     observers, so there is no shared hook site and no double emission. No JS
//     callback is ever invoked from C++: taps are materialised into a POD queue and
//     pulled by drain().
//   * The map GC root lives only for the single owner-thread hook frame that creates
//     it; it is created and freed on the owner thread, so a direct free is safe and no
//     retired-handle list is needed. No mutex is ever held across a managed call or an
//     original invocation.

namespace {

constexpr uint64_t kMaxLeaseMs = 2500ULL;

// Vector2 (float2) by value. Declaring it as a homogeneous float aggregate lets the
// C++ ABI place it in the FP registers exactly as the game does on ARM64 AAPCS and on
// x86_64 SysV; the integer registers still carry self / out / MethodInfo. Never split.
struct MapFloat2 {
  float x;
  float y;
};
static_assert(sizeof(MapFloat2) == 8, "Vector2 by-value must be 8 bytes");

using GcHandleNewFn = uintptr_t (*)(void *obj, int pinned);
using GcHandleGetTargetFn = void *(*)(uintptr_t handle);
using GcHandleFreeFn = void (*)(uintptr_t handle);
using ObjectGetClassFn = void *(*)(void *obj);
using RuntimeInvokeFn = void *(*)(const void *method, void *obj, void **params, void **exc);
using ObjectUnboxFn = void *(*)(void *obj);

// Ground.OnTap(self, object sender, EventArgs, MethodInfo*) -> void.
using TapHookFn = void (*)(void *self, void *sender, void *event_args, const void *method);
// MapView.GetGroundPosition(self, Vector2 byValue, Vector3* out, MethodInfo*) -> bool.
using PositionHookFn = uint8_t (*)(void *self, MapFloat2 pos, void *out_vec3, const void *method);

struct ResolvedConfig {
  GcHandleNewFn gchandle_new;
  GcHandleGetTargetFn gchandle_get_target;
  GcHandleFreeFn gchandle_free;
  ObjectGetClassFn object_get_class;
  RuntimeInvokeFn runtime_invoke;
  ObjectUnboxFn object_unbox;

  void *tap_address;
  void *position_address;

  const void *location_method;

  void *ground_class;
  void *map_class;
  void *latlng_class;
};

std::mutex g_install_mutex;
std::mutex g_state_mutex;

std::atomic<bool> g_installed{false};
bool g_install_attempted = false;  // install mutex; partially installed hooks stay alive
int g_install_error = PGR_MAP_TAP_ERR_NOT_INSTALLED;
PgrMapTapConfig g_bound_config{};
std::atomic<TapHookFn> g_orig_tap{nullptr};
std::atomic<PositionHookFn> g_orig_position{nullptr};

// The permanent cleanup/capture owner, latched once from a genuine on-thread
// observation. Separate from the policy hint (g_proven_tid) and the reentrancy busy
// depth (g_call_depth) so an off-thread update can never reassign it.
std::atomic<uint64_t> g_owner_tid{0};

// --- Everything below is guarded by g_state_mutex. ---
ResolvedConfig g_cfg{};
bool g_enabled = false;
uint64_t g_lease_until_ms = 0;
uint64_t g_proven_tid = 0;
uint64_t g_epoch = 0;
int32_t g_mode = 0;
uint32_t g_tap_scope_depth = 0;   // Ground.OnTap nesting on the owner thread
uint64_t g_scope_gen = 0;         // non-zero while an OnTap scope is live
uint64_t g_next_generation = 0;
uint32_t g_call_depth = 0;        // capture reentrancy busy guard
uint32_t g_inflight_roots = 0;    // GC roots held by an in-flight capture (finish guard)
int g_fault = 0;                  // latched after a fatal runtime fault

// Bounded POD ring. Two quick taps are both retained; drain() pops one at a time.
PgrMapTapEvent g_queue[PGR_MAP_TAP_QUEUE_CAPACITY]{};
uint32_t g_q_head = 0;
uint32_t g_q_tail = 0;
uint32_t g_q_count = 0;

uint32_t g_captured_serial = 0;
uint32_t g_dropped_serial = 0;
uint32_t g_error_serial = 0;
uint32_t g_overflow_serial = 0;
uint32_t g_last_error_code = PGR_MAP_TAP_RUNTIME_NONE;
uint64_t g_last_tid = 0;

uint64_t current_tid() {
  return static_cast<uint64_t>(static_cast<pid_t>(gettid()));
}

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) {
    return false;
  }
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
            static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

// Publish an original trampoline pointer, or wait for install to publish it if a call
// races in before the install mutex is released. Never skips the original.
template <typename Fn>
Fn load_orig(std::atomic<Fn> &slot) {
  Fn fn = slot.load(std::memory_order_acquire);
  if (fn != nullptr) {
    return fn;
  }
  std::lock_guard<std::mutex> lock(g_install_mutex);
  return slot.load(std::memory_order_acquire);
}

// --- helpers; all require g_state_mutex held by the caller ---

bool permitted_locked(uint64_t now_ms) {
  return g_fault == 0 && g_enabled && g_proven_tid != 0 && now_ms < g_lease_until_ms &&
         g_mode >= 1 && g_mode <= 3;
}

void latch_owner_locked(uint64_t tid) {
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
}

void publish_error_locked(uint32_t code, uint64_t tid) {
  g_last_error_code = code;
  g_last_tid = tid;
  ++g_error_serial;
}

void note_drop_locked(uint32_t code, uint64_t tid) {
  ++g_dropped_serial;
  if (code != PGR_MAP_TAP_RUNTIME_NONE) {
    publish_error_locked(code, tid);
  }
}

void flush_queue_locked() {
  g_q_head = 0;
  g_q_tail = 0;
  g_q_count = 0;
}

bool enqueue_locked(const PgrMapTapEvent &ev) {
  if (g_q_count >= PGR_MAP_TAP_QUEUE_CAPACITY) {
    return false;
  }
  g_queue[g_q_tail] = ev;
  g_q_tail = (g_q_tail + 1U) % PGR_MAP_TAP_QUEUE_CAPACITY;
  ++g_q_count;
  return true;
}

uint32_t owned_root_count_locked() {
  return g_inflight_roots + g_call_depth + g_tap_scope_depth;
}

bool finite_f(float v) { return std::isfinite(v); }
bool finite_d(double v) { return std::isfinite(v); }

// A temporary GC root held for the lifetime of a single owner-thread frame. Created
// and freed only on the owner thread, so a direct free is safe. Noncopyable.
class ScopedRoot {
 public:
  ScopedRoot(const ScopedRoot &) = delete;
  ScopedRoot &operator=(const ScopedRoot &) = delete;
  explicit ScopedRoot(const ResolvedConfig &cfg) : cfg_(cfg), handle_(0) {}
  ~ScopedRoot() { reset(); }

  void acquire(void *obj) {
    reset();
    if (obj == nullptr || cfg_.gchandle_new == nullptr) {
      return;
    }
    handle_ = cfg_.gchandle_new(obj, 0);
    if (handle_ != 0) {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      ++g_inflight_roots;
    }
  }

  bool ok() const { return handle_ != 0; }

  void *target() const {
    if (handle_ == 0 || cfg_.gchandle_get_target == nullptr) {
      return nullptr;
    }
    return cfg_.gchandle_get_target(handle_);
  }

  void reset() {
    if (handle_ == 0) {
      return;
    }
    uintptr_t h = handle_;
    handle_ = 0;
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      if (g_inflight_roots > 0) --g_inflight_roots;
    }
    if (cfg_.gchandle_free != nullptr) {
      cfg_.gchandle_free(h);
    }
  }

 private:
  const ResolvedConfig &cfg_;
  uintptr_t handle_;
};

// Re-validate the capture scope: same live OnTap generation, same epoch/mode, still
// permitted and unexpired. Returns false (and never renews the lease) if anything
// changed under us while the original ran or GetWorldLocation was invoked.
bool scope_still_valid(uint64_t gen, uint64_t epoch, int32_t mode) {
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_state_mutex);
  return g_scope_gen == gen && permitted_locked(now) && g_epoch == epoch && g_mode == mode;
}

// Invoke MapView.GetWorldLocation(Vector3) -> boxed LatLng, unbox, read/validate the
// lat/lon doubles and queue the event. Runs entirely on the owner thread; the boxed
// return is temporarily rooted before it is touched. No mutex is held across the
// managed call. On any failure the capture is dropped (or faults on overflow).
void capture_coordinate(const ResolvedConfig &cfg, ScopedRoot &map_root, void *out_vec3,
                        uint64_t gen, uint64_t epoch, int32_t mode, uint64_t tid) {
  float raw[3];
  std::memcpy(raw, out_vec3, sizeof(raw));
  if (!finite_f(raw[0]) || !finite_f(raw[1]) || !finite_f(raw[2])) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    note_drop_locked(PGR_MAP_TAP_RUNTIME_POSITION_NONFINITE, tid);
    return;
  }

  if (!scope_still_valid(gen, epoch, mode)) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    note_drop_locked(PGR_MAP_TAP_RUNTIME_NONE, tid);
    return;
  }

  void *map_obj = map_root.target();
  if (map_obj == nullptr) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    note_drop_locked(PGR_MAP_TAP_RUNTIME_NONE, tid);
    return;
  }

  float vec[3] = {raw[0], raw[1], raw[2]};
  void *params[1] = {vec};
  void *exc = nullptr;
  if (!scope_still_valid(gen, epoch, mode)) return;
  void *boxed = cfg.runtime_invoke(cfg.location_method, map_obj, params, &exc);
  if (exc != nullptr || boxed == nullptr) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    note_drop_locked(PGR_MAP_TAP_RUNTIME_INVOKE_EXCEPTION, tid);
    return;
  }

  ScopedRoot boxed_root(cfg);
  boxed_root.acquire(boxed);
  if (!boxed_root.ok()) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    note_drop_locked(PGR_MAP_TAP_RUNTIME_GCHANDLE_BOXED_FAILED, tid);
    return;
  }

  void *bt = boxed_root.target();
  if (bt == nullptr || cfg.object_get_class(bt) != cfg.latlng_class) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    note_drop_locked(PGR_MAP_TAP_RUNTIME_BOXED_CLASS_MISMATCH, tid);
    return;
  }

  void *data = cfg.object_unbox(bt);
  if (data == nullptr) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    note_drop_locked(PGR_MAP_TAP_RUNTIME_BOXED_NULL, tid);
    return;
  }

  double lat = 0.0;
  double lon = 0.0;
  std::memcpy(&lat, data, sizeof(double));
  std::memcpy(&lon, static_cast<char *>(data) + sizeof(double), sizeof(double));
  if (!finite_d(lat) || !finite_d(lon)) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    note_drop_locked(PGR_MAP_TAP_RUNTIME_LOCATION_NONFINITE, tid);
    return;
  }
  if (std::fabs(lat) > 90.0 || std::fabs(lon) > 180.0) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    note_drop_locked(PGR_MAP_TAP_RUNTIME_LOCATION_BOUNDS, tid);
    return;
  }

  uint64_t now = 0;
  if (!read_boottime_ms(&now)) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    note_drop_locked(PGR_MAP_TAP_RUNTIME_CLOCK_BOOTTIME_FAILED, tid);
    return;
  }

  std::lock_guard<std::mutex> lock(g_state_mutex);
  // The lease may have expired, the epoch changed or the feature disabled while the
  // original ran or GetWorldLocation was invoked: in every such case drop the event.
  if (g_scope_gen != gen || !permitted_locked(now) || g_epoch != epoch || g_mode != mode) {
    note_drop_locked(PGR_MAP_TAP_RUNTIME_NONE, tid);
    return;
  }

  PgrMapTapEvent ev{};
  ev.mode = mode;
  ev.epoch = epoch;
  ev.latitude = lat;
  ev.longitude = lon;
  ev.tid = tid;
  ev.boottime_ms = now;
  if (!enqueue_locked(ev)) {
    // Fail closed rather than quietly misroute a tap: latch a fault and flush. A
    // double-tap whose second half cannot be queued must never be silently dropped
    // into the wrong routing decision.
    g_fault = 1;
    flush_queue_locked();
    ++g_overflow_serial;
    publish_error_locked(PGR_MAP_TAP_RUNTIME_QUEUE_OVERFLOW, tid);
    return;
  }
  ++g_captured_serial;
  g_last_tid = tid;
}

// ---------------------------------------------------------------------------
// Hooks. Each chains its original exactly once with unmodified arguments.
// ---------------------------------------------------------------------------

void hook_tap(void *self, void *sender, void *event_args, const void *method) {
  TapHookFn orig = load_orig(g_orig_tap);
  const uint64_t tid = current_tid();

  ResolvedConfig cfg{};
  bool eligible = false;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    cfg = g_cfg;
    const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
    const bool owner_ok = g_proven_tid != 0 && tid == g_proven_tid && (owner == 0 || owner == tid);
    if (owner_ok) {
      latch_owner_locked(tid);
      uint64_t now = 0;
      if (read_boottime_ms(&now)) {
        eligible = permitted_locked(now);
      }
    }
  }

  // The ground-class check is a managed call, so it runs with no mutex held. Opening a
  // scope for a non-Ground sender would let an unrelated GetGroundPosition capture.
  bool entered = false;
  if (eligible && self != nullptr && cfg.object_get_class(self) == cfg.ground_class) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    ++g_tap_scope_depth;
    if (g_tap_scope_depth == 1U) {
      g_scope_gen = ++g_next_generation;
    }
    entered = true;
  }

  orig(self, sender, event_args, method);

  if (entered) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_tap_scope_depth > 0) --g_tap_scope_depth;
    if (g_tap_scope_depth == 0) g_scope_gen = 0;
  }
}

uint8_t hook_position(void *self, MapFloat2 pos, void *out_vec3, const void *method) {
  PositionHookFn orig = load_orig(g_orig_position);
  const uint64_t tid = current_tid();

  ResolvedConfig cfg{};
  bool eligible = false;
  bool claimed_busy = false;
  uint64_t gen = 0;
  uint64_t cap_epoch = 0;
  int32_t cap_mode = 0;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    cfg = g_cfg;
    const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
    const bool owner_ok = g_proven_tid != 0 && tid == g_proven_tid && (owner == 0 || owner == tid);
    if (owner_ok) {
      latch_owner_locked(tid);
      uint64_t now = 0;
      // Capture only inside a live OnTap scope on this owner thread, while not already
      // capturing: a GetGroundPosition reentered during our capture (g_call_depth != 0)
      // just chains its original, so a tap is never double-captured.
      if (g_scope_gen != 0 && g_call_depth == 0 &&
          read_boottime_ms(&now) && permitted_locked(now)) {
        eligible = true;
        gen = g_scope_gen;
        cap_epoch = g_epoch;
        cap_mode = g_mode;
        ++g_call_depth;
        claimed_busy = true;
      }
    }
  }

  // Validate the target outside the lock: self must be exactly the MapView class and
  // the out Vector3 must be non-null. A mismatch is a normal non-capture, not an error.
  ScopedRoot map_root(cfg);
  bool want = false;
  if (eligible && self != nullptr && out_vec3 != nullptr &&
      cfg.object_get_class(self) == cfg.map_class) {
    map_root.acquire(self);
    if (map_root.ok()) {
      want = true;
    } else {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      note_drop_locked(PGR_MAP_TAP_RUNTIME_GCHANDLE_MAP_FAILED, tid);
    }
  }

  // The original runs exactly once with the unmodified arguments and its real result
  // is returned to the caller regardless of whether we captured anything.
  const uint8_t result = orig(self, pos, out_vec3, method);

  if (want && result == 1U) {
    capture_coordinate(cfg, map_root, out_vec3, gen, cap_epoch, cap_mode, tid);
  }
  map_root.reset();

  if (claimed_busy) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_call_depth > 0) --g_call_depth;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Install.
// ---------------------------------------------------------------------------

bool validate_config(const PgrMapTapConfig *config, int *out_rc) {
  if (config == nullptr) {
    *out_rc = PGR_MAP_TAP_ERR_INVALID_ARG;
    return false;
  }
  if (config->version != PGR_MAP_TAP_CONFIG_VERSION) {
    *out_rc = PGR_MAP_TAP_ERR_BAD_VERSION;
    return false;
  }
  if (config->size != sizeof(PgrMapTapConfig)) {
    *out_rc = PGR_MAP_TAP_ERR_BAD_SIZE;
    return false;
  }
  void *const ptrs[] = {
      config->gchandle_new,    config->gchandle_get_target, config->gchandle_free,
      config->object_get_class, config->runtime_invoke,     config->object_unbox,
      config->tap_address,     config->position_address,    config->location_method,
      config->ground_class,    config->map_class,           config->latlng_class,
  };
  for (void *p : ptrs) {
    if (p == nullptr) {
      *out_rc = PGR_MAP_TAP_ERR_MISSING_PTR;
      return false;
    }
  }
  if (config->tap_address == config->position_address) {
    *out_rc = PGR_MAP_TAP_ERR_DUPLICATE_TARGET;
    return false;
  }
  *out_rc = PGR_MAP_TAP_OK;
  return true;
}

ResolvedConfig resolve_config(const PgrMapTapConfig *c) {
  ResolvedConfig r{};
  r.gchandle_new = reinterpret_cast<GcHandleNewFn>(c->gchandle_new);
  r.gchandle_get_target = reinterpret_cast<GcHandleGetTargetFn>(c->gchandle_get_target);
  r.gchandle_free = reinterpret_cast<GcHandleFreeFn>(c->gchandle_free);
  r.object_get_class = reinterpret_cast<ObjectGetClassFn>(c->object_get_class);
  r.runtime_invoke = reinterpret_cast<RuntimeInvokeFn>(c->runtime_invoke);
  r.object_unbox = reinterpret_cast<ObjectUnboxFn>(c->object_unbox);
  r.tap_address = c->tap_address;
  r.position_address = c->position_address;
  r.location_method = c->location_method;
  r.ground_class = c->ground_class;
  r.map_class = c->map_class;
  r.latlng_class = c->latlng_class;
  return r;
}

int api_install(const PgrMapTapConfig *config) {
  int validation_rc = PGR_MAP_TAP_OK;
  if (!validate_config(config, &validation_rc)) {
    return validation_rc;
  }

  uint64_t boot_probe = 0;
  if (!read_boottime_ms(&boot_probe)) {
    return PGR_MAP_TAP_ERR_CLOCK_FAILURE;
  }

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  const ResolvedConfig resolved = resolve_config(config);

  if (g_installed.load(std::memory_order_acquire)) {
    // Hooks live for the process; re-install of the same targets is idempotent, but the
    // two hook addresses and the resolved metadata may never be rebound.
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_bound_config, config, sizeof(*config)) != 0) {
      return PGR_MAP_TAP_ERR_RECONFIG_ADDRESS_MISMATCH;
    }
    return PGR_MAP_TAP_OK;
  }
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;
  g_bound_config = *config;

  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = resolved;
    g_enabled = false;
    g_lease_until_ms = 0;
    g_proven_tid = 0;
    g_epoch = 0;
    g_mode = 0;
    g_tap_scope_depth = 0;
    g_scope_gen = 0;
    g_call_depth = 0;
    g_fault = 0;
    flush_queue_locked();
  }

  // Install hooks under g_install_mutex, publishing each original before the next. On
  // partial failure retain already-published trampolines (another thread may already be
  // using one) and latch the failure until process restart -- never a Gum fallback.
  void *orig_tap = nullptr;
  if (pgrazor_hook(resolved.tap_address, reinterpret_cast<void *>(&hook_tap), &orig_tap) != 0 ||
      orig_tap == nullptr) {
    return g_install_error = PGR_MAP_TAP_ERR_HOOK_TAP;
  }
  g_orig_tap.store(reinterpret_cast<TapHookFn>(orig_tap), std::memory_order_release);

  void *orig_position = nullptr;
  if (pgrazor_hook(resolved.position_address, reinterpret_cast<void *>(&hook_position),
                   &orig_position) != 0 ||
      orig_position == nullptr) {
    return g_install_error = PGR_MAP_TAP_ERR_HOOK_POSITION;
  }
  g_orig_position.store(reinterpret_cast<PositionHookFn>(orig_position), std::memory_order_release);

  g_installed.store(true, std::memory_order_release);
  return g_install_error = PGR_MAP_TAP_OK;
}

// ---------------------------------------------------------------------------
// Policy update / disable. Safe on any thread: they only arm/clear the gate and flush
// the queue; they never touch the in-flight capture root (owned by the hook frame).
// ---------------------------------------------------------------------------

int api_update(int enabled, uint64_t remaining_lease_ms, uint64_t proven_tid, uint64_t epoch,
               int mode) {
  if (!g_installed.load(std::memory_order_acquire)) {
    return PGR_MAP_TAP_ERR_NOT_INSTALLED;
  }
  if (mode < 0 || mode > 3) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_enabled = false;
    g_lease_until_ms = 0;
    g_scope_gen = 0;
    flush_queue_locked();
    return PGR_MAP_TAP_ERR_BAD_MODE;
  }

  uint64_t now_ms = 0;
  const bool clock_ok = read_boottime_ms(&now_ms);
  bool wrong_thread = false;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
    wrong_thread = owner != 0 && proven_tid != 0 && proven_tid != owner;
    if (wrong_thread) {
      enabled = 0;  // a wrong-thread update can only disable; it never reassigns owner
    } else if (proven_tid != 0) {
      g_proven_tid = proven_tid;
    }

    const bool epoch_changed = g_epoch != epoch || g_mode != mode;
    const bool expired = now_ms >= g_lease_until_ms;
    g_epoch = epoch;
    g_mode = mode;

    if (!clock_ok) {
      g_enabled = false;
      g_lease_until_ms = 0;
      publish_error_locked(PGR_MAP_TAP_RUNTIME_CLOCK_BOOTTIME_FAILED, proven_tid);
      g_scope_gen = 0;
      flush_queue_locked();
    } else {
      const uint64_t clamped = remaining_lease_ms > kMaxLeaseMs ? kMaxLeaseMs : remaining_lease_ms;
      const bool allow = enabled != 0 && clamped > 0 && proven_tid != 0 && !wrong_thread &&
                         mode >= 1 && mode <= 3 && g_fault == 0;
      g_enabled = allow;
      g_lease_until_ms = allow ? (now_ms + clamped) : 0;
      // Flush when the policy stops, or flush the now-stale queued epochs on a change so
      // a tap captured under the old policy can never be drained into the new routing.
      if (!allow || epoch_changed || expired) {
        g_scope_gen = 0;
        flush_queue_locked();
      }
    }
  }

  return wrong_thread ? PGR_MAP_TAP_ERR_INVALID_ARG
                      : clock_ok ? PGR_MAP_TAP_OK : PGR_MAP_TAP_ERR_CLOCK_FAILURE;
}

int api_disable(void) {
  if (!g_installed.load(std::memory_order_acquire)) {
    return PGR_MAP_TAP_ERR_NOT_INSTALLED;
  }
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_enabled = false;
  g_lease_until_ms = 0;
  g_scope_gen = 0;
  flush_queue_locked();
  return PGR_MAP_TAP_OK;
}

// ---------------------------------------------------------------------------
// drain / sample: pure pull. Never free handles, never call managed code, never
// collapse two taps into one -- drain() pops exactly one event per call.
// ---------------------------------------------------------------------------

int api_drain(PgrMapTapEvent *out_event) {
  if (out_event == nullptr) {
    return PGR_MAP_TAP_ERR_INVALID_ARG;
  }
  if (!g_installed.load(std::memory_order_acquire)) {
    return PGR_MAP_TAP_ERR_NOT_INSTALLED;
  }
  std::lock_guard<std::mutex> lock(g_state_mutex);
  uint64_t now = 0;
  if (!read_boottime_ms(&now) || !permitted_locked(now)) flush_queue_locked();
  if (g_q_count == 0) {
    return PGR_MAP_TAP_EMPTY;
  }
  *out_event = g_queue[g_q_head];
  g_q_head = (g_q_head + 1U) % PGR_MAP_TAP_QUEUE_CAPACITY;
  --g_q_count;
  return PGR_MAP_TAP_OK;
}

int api_sample(PgrMapTapSample *out_sample) {
  if (out_sample == nullptr) {
    return PGR_MAP_TAP_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(g_state_mutex);
  out_sample->version = PGR_MAP_TAP_SAMPLE_VERSION;
  out_sample->size = sizeof(PgrMapTapSample);
  out_sample->owned_root_count = owned_root_count_locked();
  out_sample->queued_count = g_q_count;
  out_sample->captured_serial = g_captured_serial;
  out_sample->dropped_serial = g_dropped_serial;
  out_sample->error_serial = g_error_serial;
  out_sample->overflow_serial = g_overflow_serial;
  out_sample->last_error_code = g_last_error_code;
  out_sample->fault = static_cast<uint32_t>(g_fault);
  out_sample->last_tid = g_last_tid;
  out_sample->active_generation = g_scope_gen;
  out_sample->reserved = 0;
  return PGR_MAP_TAP_OK;
}

const PgrMapTapApi kApiTable = {
    PGR_MAP_TAP_API_VERSION,
    static_cast<uint32_t>(sizeof(PgrMapTapApi)),
    &api_install,
    &api_update,
    &api_drain,
    &api_sample,
    &api_disable,
};

}  // namespace

extern "C" const void *pgrazor_map_tap_api(uint32_t version) {
  if (version != PGR_MAP_TAP_API_VERSION) return nullptr;
  return &kApiTable;
}

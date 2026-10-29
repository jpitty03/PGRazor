#include "vgp_actions.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 26 native VGP request actions. See vgp_actions.h for the ABI and the division of labour with
// the host JS. Built -fno-exceptions / -fno-rtti like every sibling: the JS "throw" paths become
// fail-closed error returns; every GC root is freed exactly once on the owner thread; the single
// game-action invoke is issued at most once per call. No mutex is ever held across a managed invoke or a
// GC-handle call. This module installs NO hook and NEVER calls back into JS.
//
// Reentrancy / ownership model:
//   * Each action claims a strictly monotonic token under one lock, sets g_busy, and captures the
//     revocation generation BEFORE any managed work; a reentrant action (e.g. fired from inside a
//     managed callback) finds g_busy and is refused, so the game action happens at most once.
//   * Every managed reference held across a managed call is rooted and re-resolved from its root; no raw
//     pointer is reused across a potential safepoint. For spin, the freshly constructed request is rooted
//     immediately after object_new and kept rooted across the ctor, the field writes and the SendRpc.
//   * The revocation generation is re-checked against a fresh clock IMMEDIATELY before the one game-action
//     invoke; a stop() / disable / re-arm that races the window aborts WITHOUT invoking (and frees every
//     temporary root).
//   * On success the returned promise is rooted into a fresh GC handle and its ownership is TRANSFERRED to
//     the caller via *out_handle (that root is NOT freed here); every other root is freed before return.
//   * If the invoke succeeds but the promise cannot be rooted, the action has already issued: that is
//     reported as PGR_VA_ERR_UNCERTAIN so the host never retries.

static_assert(sizeof(PgrVgpActionsConfig) == 208, "config must be 208 bytes");
static_assert(sizeof(PgrVgpActionsTarget) == 32, "target request must be 32 bytes");
static_assert(sizeof(PgrVgpActionsSpin) == 80, "spin request must be 80 bytes");
static_assert(sizeof(PgrVgpActionsSample) == 72, "sample must be 72 bytes");
static_assert(sizeof(PgrVgpActionsApi) == 64, "table must be 64 bytes");
static_assert(offsetof(PgrVgpActionsConfig, gchandle_new) == 8, "layout");
static_assert(offsetof(PgrVgpActionsConfig, m_encounter) == 80, "layout");
static_assert(offsetof(PgrVgpActionsConfig, class_wild) == 104, "layout");
static_assert(offsetof(PgrVgpActionsConfig, f_fort_id) == 136, "layout");
static_assert(offsetof(PgrVgpActionsConfig, f_req_target_lon) == 192, "layout");
static_assert(offsetof(PgrVgpActionsSpin, target) == 48, "layout");
static_assert(offsetof(PgrVgpActionsSpin, closed_method) == 72, "layout");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_VA_MAX_LEASE_MS;

// --- il2cpp runtime ABI typedefs ---------------------------------------------------
using GcNew = uintptr_t (*)(void *obj, int pinned);
using GcGet = void *(*)(uintptr_t handle);
using GcFree = void (*)(uintptr_t handle);
using ObjClass = void *(*)(void *obj);
using ObjNew = void *(*)(void *klass);
using Invoke = void *(*)(const void *method, void *obj, void **params, void **exc);
using FieldGet = void (*)(void *obj, void *field, void *out);
using FieldSet = void (*)(void *obj, void *field, void *value);

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrVgpActionsConfig g_cfg{};
std::atomic<bool> g_installed{false};
bool g_install_attempted = false;
int g_install_error = PGR_VA_ERR_NOT_INSTALLED;

// Permanent Unity owner, latched once from a genuine on-thread call.
std::atomic<uint64_t> g_owner_tid{0};

// Live GC roots (atomic so sample is lock-free). 0 between calls; nonzero only mid-action.
std::atomic<uint32_t> g_roots{0};

// --- gate / telemetry (guarded by g_state_mutex) -----------------------------------
uint32_t g_mask = 0;           // armed mask (0 == disabled)
uint64_t g_lease_until_ms = 0;
uint64_t g_epoch = 0;          // armed epoch
uint64_t g_gen = 1;            // revocation generation; bumped on disable / epoch / dead-lease / stop
uint64_t g_token_serial = 0;   // monotonic action token
bool g_busy = false;           // an action is in flight

uint64_t g_encounter_serial = 0, g_capture_serial = 0, g_spin_serial = 0, g_refused_serial = 0,
         g_fault_serial = 0, g_last_token = 0, g_last_tid = 0;

// --- clock / tid -------------------------------------------------------------------
uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

// Typed accessors onto the immutable published config.
GcNew gc_new() { return reinterpret_cast<GcNew>(g_cfg.gchandle_new); }
GcGet gc_get() { return reinterpret_cast<GcGet>(g_cfg.gchandle_get_target); }
GcFree gc_free_raw() { return reinterpret_cast<GcFree>(g_cfg.gchandle_free); }
ObjClass obj_class() { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }
ObjNew obj_new() { return reinterpret_cast<ObjNew>(g_cfg.object_new); }
FieldGet field_get() { return reinterpret_cast<FieldGet>(g_cfg.field_get_value); }
FieldSet field_set() { return reinterpret_cast<FieldSet>(g_cfg.field_set_value); }
FieldSet field_set_obj() { return reinterpret_cast<FieldSet>(g_cfg.field_set_value_obj); }
void *managed_invoke(const void *method, void *obj, void **params, void **exc) {
  *exc = nullptr;
  return reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(method, obj, params, exc);
}

bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }

// --- root bookkeeping --------------------------------------------------------------
uintptr_t hold(void *obj) {
  if (obj == nullptr) return 0;
  uintptr_t h = gc_new()(obj, 0);
  if (h != 0) g_roots.fetch_add(1, std::memory_order_relaxed);
  return h;
}
void drop(uintptr_t *h) {
  if (*h == 0) return;
  uintptr_t v = *h;
  *h = 0;  // detach before the (possibly reentrant) free
  gc_free_raw()(v);
  g_roots.fetch_sub(1, std::memory_order_relaxed);
}

void *field_ptr(void *obj, void *field) {
  void *out = nullptr;
  if (obj != nullptr) field_get()(obj, field, &out);
  return out;
}
double field_f64(void *obj, void *field) {
  double v = 0.0;
  if (obj != nullptr) field_get()(obj, field, &v);
  return v;
}

// --- owner latching ----------------------------------------------------------------
bool owner_ready(uint64_t tid) {
  if (tid == 0) return false;
  uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (!(owner == 0 || owner == tid)) return false;
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
  return current_tid() == tid && g_owner_tid.load(std::memory_order_acquire) == tid;
}

// Gate check against a fresh clock. Caller holds g_state_mutex. Fails closed.
bool gate_live_locked(uint64_t req_epoch, uint64_t tid, uint32_t need_mask, uint64_t now,
                      uint64_t generation, bool check_gen) {
  if (check_gen && g_gen != generation) return false;
  if ((g_mask & need_mask) == 0) return false;
  if (!(g_lease_until_ms != 0 && now < g_lease_until_ms)) return false;
  if (g_epoch != req_epoch) return false;
  return current_tid() == tid && g_owner_tid.load(std::memory_order_acquire) == tid;
}

bool coords_ok(double lat, double lon) {
  return std::isfinite(lat) && std::isfinite(lon) && lat >= -90.0 && lat <= 90.0 &&
         lon >= -180.0 && lon <= 180.0;
}

// --- shared action core ------------------------------------------------------------
enum ActionKind { ACT_ENCOUNTER, ACT_CAPTURE, ACT_SPIN };

// Runs on the owner thread with g_busy latched. Performs identity validation, (for spin) request
// construction, the pre-invoke generation re-check and the single game-action invoke, roots the returned
// promise and transfers it via *out_handle. Frees every temporary root before returning. *out_handle is
// written only on PGR_VA_ISSUED.
int do_action(ActionKind kind, const PgrVgpActionsTarget *t, const PgrVgpActionsSpin *sp,
              uint64_t epoch, uint64_t tid, uint64_t generation, uintptr_t *out_handle) {
  void *target_obj = (kind == ACT_SPIN) ? sp->target : t->target;
  uintptr_t target_root = hold(target_obj);
  uintptr_t fort_root = 0, rpc_root = 0, request_root = 0, id_root = 0;
  void *receiver = nullptr, *request_obj = nullptr;
  int rc = PGR_VA_OK;

  if (target_root == 0) { rc = PGR_VA_ERR_GCHANDLE; goto done; }
  if (!is_exact(gc_get()(target_root), kind == ACT_SPIN ? g_cfg.class_stop : g_cfg.class_wild)) { rc = PGR_VA_ERR_IDENTITY; goto done; }

  if (kind == ACT_SPIN) {
    if (sp->closed_method == nullptr) { rc = PGR_VA_ERR_IDENTITY; goto done; }
    if (!coords_ok(sp->lat, sp->lon)) { rc = PGR_VA_ERR_IDENTITY; goto done; }
    fort_root = hold(sp->fort);
    rpc_root = hold(sp->rpc);
    if (fort_root == 0 || rpc_root == 0) { rc = PGR_VA_ERR_GCHANDLE; goto done; }
    if (!is_exact(gc_get()(fort_root), g_cfg.class_fort) ||
        !is_exact(gc_get()(rpc_root), g_cfg.class_rpc)) { rc = PGR_VA_ERR_IDENTITY; goto done; }
    // Fort coordinates (copied into the request) must be finite too.
    if (!coords_ok(field_f64(gc_get()(fort_root), g_cfg.f_fort_lat),
                   field_f64(gc_get()(fort_root), g_cfg.f_fort_lon))) { rc = PGR_VA_ERR_IDENTITY; goto done; }

    // Build the FortSearch request: object_new + ctor + Id + player/fort coordinates. Root the request
    // immediately so it survives every subsequent allocation / managed call.
    {
      void *request = obj_new()(g_cfg.class_request);
      request_root = hold(request);
      if (request_root == 0) { rc = PGR_VA_ERR_GCHANDLE; goto done; }
      if (!is_exact(gc_get()(request_root), g_cfg.class_request)) { rc = PGR_VA_ERR_IDENTITY; goto done; }
      void *exc = nullptr;
      managed_invoke(g_cfg.m_ctor, gc_get()(request_root), nullptr, &exc);
      if (exc != nullptr) { rc = PGR_VA_ERR_MANAGED; goto done; }
      // request.Id = fort.FortId (reference field, write barrier).
      void *fort_id = field_ptr(gc_get()(fort_root), g_cfg.f_fort_id);
      if (!fort_id) { rc = PGR_VA_ERR_IDENTITY; goto done; }
      id_root = hold(fort_id);
      if (!id_root) { rc = PGR_VA_ERR_GCHANDLE; goto done; }
      void *id_value = gc_get()(id_root);
      void *request_value = gc_get()(request_root);
      if (!id_value || !request_value) { rc = PGR_VA_ERR_IDENTITY; goto done; }
      field_set_obj()(request_value, g_cfg.f_req_id, id_value);
      // player coordinates (from the caller) and fort coordinates (copied from the fort).
      double lat = sp->lat, lon = sp->lon;
      field_set()(gc_get()(request_root), g_cfg.f_req_lat, &lat);
      field_set()(gc_get()(request_root), g_cfg.f_req_lon, &lon);
      double flat = field_f64(gc_get()(fort_root), g_cfg.f_fort_lat);
      double flon = field_f64(gc_get()(fort_root), g_cfg.f_fort_lon);
      if (!coords_ok(flat, flon)) { rc = PGR_VA_ERR_IDENTITY; goto done; }
      field_set()(gc_get()(request_root), g_cfg.f_req_target_lat, &flat);
      field_set()(gc_get()(request_root), g_cfg.f_req_target_lon, &flon);
    }
  }

  // Resolve the final managed arguments before the revocation check; GC helpers may re-enter.
  receiver = gc_get()(kind == ACT_SPIN ? rpc_root : target_root);
  if (!is_exact(receiver, kind == ACT_SPIN ? g_cfg.class_rpc : g_cfg.class_wild)) { rc = PGR_VA_ERR_IDENTITY; goto done; }
  if (kind == ACT_SPIN) {
    request_obj = gc_get()(request_root);
    if (!is_exact(request_obj, g_cfg.class_request)) { rc = PGR_VA_ERR_IDENTITY; goto done; }
  }
  // Re-check the gate + generation against a fresh clock IMMEDIATELY before the one game action. A stop /
  // disable / re-arm (or an epoch move) that raced the construction aborts here WITHOUT invoking.
  {
    uint64_t now = 0;
    if (!read_boottime_ms(&now)) { rc = PGR_VA_ERR_CLOCK_FAILURE; goto done; }
    const uint32_t need_mask = (kind == ACT_SPIN) ? PGR_VA_MASK_SPIN : PGR_VA_MASK_CATCH;
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!gate_live_locked(epoch, tid, need_mask, now, generation, true)) { rc = PGR_VA_OK; goto done; }
    g_last_token = ++g_token_serial; g_last_tid = tid;
  }

  // Claim a token and issue the single game action. No lock is held across the invoke.
  {

    void *exc = nullptr;
    void *promise = nullptr;
    if (kind == ACT_ENCOUNTER) {
      promise = managed_invoke(g_cfg.m_encounter, receiver, nullptr, &exc);
    } else if (kind == ACT_CAPTURE) {
      // Fixed 12-byte PokeballThrow: red ball id 1, grade 1.0f, remaining 0. AR metadata = null.
      alignas(8) uint8_t ball[12];
      std::memset(ball, 0, sizeof(ball));
      int32_t ball_id = PGR_VA_BALL_RED;
      float grade = 1.0f;
      std::memcpy(ball + 0, &ball_id, 4);
      std::memcpy(ball + 4, &grade, 4);
      void *params[2] = {ball, nullptr};
      promise = managed_invoke(g_cfg.m_capture, receiver, params, &exc);
    } else {
      int32_t rpc_id = sp->rpc_id, zero = 0, ten_k = 10000;
      void *params[5] = {&rpc_id, request_obj, &zero, &ten_k, &zero};
      promise = managed_invoke(sp->closed_method, receiver, params, &exc);
    }

    if (exc != nullptr) { rc = PGR_VA_ERR_MANAGED; goto done; }
    // The action has been issued. From here a failure is UNCERTAIN (never retried by the host).
    if (promise == nullptr) { rc = PGR_VA_ERR_UNCERTAIN; goto done; }
    uintptr_t promise_root = hold(promise);
    if (promise_root == 0) { rc = PGR_VA_ERR_UNCERTAIN; goto done; }

    // Transfer ownership of the promise handle to the caller exactly once. The caller frees it later via
    // il2cpp_gchandle_free (bypassing drop()), so relinquish our root accounting here without freeing.
    *out_handle = promise_root;
    g_roots.fetch_sub(1, std::memory_order_relaxed);
    rc = PGR_VA_ISSUED;
  }

done:
  drop(&id_root);
  drop(&request_root);
  drop(&rpc_root);
  drop(&fort_root);
  drop(&target_root);
  return rc;
}

// Common prologue: validate installation / header, latch the owner, reject reentrancy, claim g_busy
// under the gate, run do_action, release g_busy. need_mask selects catch vs spin.
int run_action(ActionKind kind, const PgrVgpActionsTarget *t, const PgrVgpActionsSpin *sp,
               uintptr_t *out_handle) {
  if (out_handle != nullptr) *out_handle = 0;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_VA_ERR_NOT_INSTALLED;
  if (out_handle == nullptr) return PGR_VA_ERR_NULL_ARG;

  uint64_t epoch, req_tid;
  if (kind == ACT_SPIN) {
    if (sp == nullptr) return PGR_VA_ERR_NULL_ARG;
    if (sp->version != PGR_VA_SPIN_VERSION) return PGR_VA_ERR_BAD_VERSION;
    if (sp->size != sizeof(PgrVgpActionsSpin)) return PGR_VA_ERR_BAD_SIZE;
    epoch = sp->epoch; req_tid = sp->tid;
  } else {
    if (t == nullptr) return PGR_VA_ERR_NULL_ARG;
    if (t->version != PGR_VA_TARGET_VERSION) return PGR_VA_ERR_BAD_VERSION;
    if (t->size != sizeof(PgrVgpActionsTarget)) return PGR_VA_ERR_BAD_SIZE;
    epoch = t->epoch; req_tid = t->tid;
  }

  const uint64_t tid = current_tid();
  if (req_tid == 0 || tid != req_tid || !owner_ready(tid)) return PGR_VA_OK;  // off-owner: refused
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_VA_ERR_CLOCK_FAILURE;

  const uint32_t need_mask = (kind == ACT_SPIN) ? PGR_VA_MASK_SPIN : PGR_VA_MASK_CATCH;
  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_busy || !gate_live_locked(epoch, tid, need_mask, now, 0, false)) {
      ++g_refused_serial;
      return PGR_VA_OK;  // refused: *out_handle stays 0
    }
    if (g_token_serial >= ((uint64_t{1} << 53) - 1)) { ++g_refused_serial; return PGR_VA_OK; }
    g_busy = true;
    generation = g_gen;
  }

  int rc = do_action(kind, t, sp, epoch, tid, generation, out_handle);

  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_busy = false;
    if (rc == PGR_VA_ISSUED) {
      if (kind == ACT_ENCOUNTER) ++g_encounter_serial;
      else if (kind == ACT_CAPTURE) ++g_capture_serial;
      else ++g_spin_serial;
    } else if (rc == PGR_VA_OK) {
      ++g_refused_serial;
    } else {
      ++g_fault_serial;  // identity / gchandle / managed / uncertain / clock
    }
  }
  return rc;
}

// --- public API --------------------------------------------------------------------
bool validate_config(const PgrVgpActionsConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_VA_ERR_NULL_ARG; return false; }
  if (c->version != PGR_VA_CONFIG_VERSION) { *rc = PGR_VA_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrVgpActionsConfig)) { *rc = PGR_VA_ERR_BAD_SIZE; return false; }
  void *const *ptrs = reinterpret_cast<void *const *>(&c->gchandle_new);
  for (int i = 0; i < 25; ++i) {
    if (ptrs[i] == nullptr) { *rc = PGR_VA_ERR_MISSING_PTR; return false; }
  }
  *rc = PGR_VA_OK;
  return true;
}

int api_install(const PgrVgpActionsConfig *config) {
  int rc = PGR_VA_OK;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_VA_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_cfg, config, sizeof(*config)) != 0) return PGR_VA_ERR_RECONFIG;
    return PGR_VA_OK;
  }
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;
  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;
    g_mask = 0;
    g_lease_until_ms = 0;
    g_epoch = 0;
    g_token_serial = 0;
    g_busy = false;
  }
  g_installed.store(true, std::memory_order_release);
  return g_install_error = PGR_VA_OK;
}

int api_update(uint32_t mask, uint64_t remaining, uint64_t epoch, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_VA_ERR_NOT_INSTALLED;
  uint64_t now = 0;
  const bool clock_ok = read_boottime_ms(&now);
  std::lock_guard<std::mutex> lock(g_state_mutex);
  const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  const bool on_owner = clock_ok && proven_tid != 0 && current_tid() == proven_tid && (owner == 0 || owner == proven_tid);
  // A foreign thread, a bad clock/tid, or a cleared mask can only REVOKE. Arming requires the owner.
  if (!on_owner || mask == 0) {
    if (g_mask || g_lease_until_ms) ++g_gen;
    g_mask = 0;
    g_lease_until_ms = 0;
    if (on_owner) g_epoch = epoch;  // the owner may record a new epoch even while disabling
    return clock_ok ? PGR_VA_OK : PGR_VA_ERR_CLOCK_FAILURE;
  }
  remaining = remaining > kMaxLeaseMs ? kMaxLeaseMs : remaining;
  if (g_mask != mask || g_epoch != epoch || !remaining || !g_lease_until_ms || now >= g_lease_until_ms) ++g_gen;
  g_mask = mask;
  g_lease_until_ms = remaining ? now + remaining : 0;
  g_epoch = epoch;
  return PGR_VA_OK;
}

int api_encounter(const PgrVgpActionsTarget *request, uintptr_t *out_handle) {
  return run_action(ACT_ENCOUNTER, request, nullptr, out_handle);
}
int api_capture(const PgrVgpActionsTarget *request, uintptr_t *out_handle) {
  return run_action(ACT_CAPTURE, request, nullptr, out_handle);
}
int api_spin(const PgrVgpActionsSpin *request, uintptr_t *out_handle) {
  return run_action(ACT_SPIN, nullptr, request, out_handle);
}

int api_stop(uint64_t /*proven_tid*/) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_VA_ERR_NOT_INSTALLED;
  // Immediate revoke from any thread. Does NOT touch g_busy: an in-flight action finishes on its own;
  // the generation bump guarantees no action that has not reached its pre-invoke re-check will invoke,
  // and no new action can arm until a later update(). stop() never re-arms.
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_mask = 0;
  g_lease_until_ms = 0;
  ++g_gen;
  return PGR_VA_OK;
}

int api_sample(PgrVgpActionsSample *out) {
  if (out == nullptr) return PGR_VA_ERR_NULL_ARG;
  const uint32_t roots = g_roots.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_state_mutex);
  out->version = PGR_VA_SAMPLE_VERSION;
  out->size = sizeof(PgrVgpActionsSample);
  out->encounter_serial = g_encounter_serial;
  out->capture_serial = g_capture_serial;
  out->spin_serial = g_spin_serial;
  out->refused_serial = g_refused_serial;
  out->fault_serial = g_fault_serial;
  out->last_token = g_last_token;
  out->last_tid = g_last_tid;
  out->roots = roots;
  out->busy = g_busy ? 1u : 0u;
  return PGR_VA_OK;
}

const PgrVgpActionsApi kApiTable = {
    PGR_VA_API_VERSION, static_cast<uint32_t>(sizeof(PgrVgpActionsApi)),
    &api_install,   &api_update, &api_encounter, &api_capture,
    &api_spin,      &api_stop,   &api_sample,
};

}  // namespace

extern "C" const void *pgrazor_vgp_actions_api(uint32_t version) {
  if (version != PGR_VA_API_VERSION) return nullptr;
  return &kApiTable;
}

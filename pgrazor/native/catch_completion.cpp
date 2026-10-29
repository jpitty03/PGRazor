#include "catch_completion.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 24 native direct Quick Catch completion. See catch_completion.h for the ABI and the
// division of labour with the host JS. Built -fno-exceptions / -fno-rtti like every sibling: the JS
// "throw" paths become fail-closed error returns; the dismissal Action.Invoke() is issued at most
// once per request. No mutex is ever held across runtime_invoke or a field read, so a reentrant
// managed callback can never deadlock. This module installs NO hook and NEVER calls back into JS.
//
// Reentrancy / double-invoke model:
//   * acquire() issues a strictly monotonic token (g_token_serial) only while armed, on the owner
//     thread, and not busy. complete() requires the token to equal the latest issued token AND be
//     greater than the last consumed token; it CONSUMES it (g_consumed = token) and sets g_busy
//     BEFORE any managed work. A callback that re-enters acquire() gets 0 (busy); a reentrant or
//     retried complete() with the same/older token is refused; so the invoke happens at most once.
//   * stop() bumps the revocation generation and disables the gate (any thread). complete() captures
//     the generation after consuming the token and re-checks it immediately before runtime_invoke, so
//     a stop (or an epoch move) that races the window aborts the invoke without a partial callback.
//   * Native roots nothing and stores no managed pointer across calls; the caller guarantees the four
//     raw pointers stay GC-rooted for the duration of the synchronous call. The only managed calls
//     after authorization are the identity re-validation (field reads) and the single Action.Invoke().

static_assert(sizeof(PgrCatchCompletionConfig) == 128, "config must be 128 bytes");
static_assert(sizeof(PgrCatchCompletionRequest) == 64, "request must be 64 bytes");
static_assert(sizeof(PgrCatchCompletionSample) == 72, "sample must be 72 bytes");
static_assert(sizeof(PgrCatchCompletionApi) == 64, "table must be 64 bytes");
static_assert(offsetof(PgrCatchCompletionConfig, object_get_class) == 8, "layout");
static_assert(offsetof(PgrCatchCompletionConfig, class_view) == 32, "layout");
static_assert(offsetof(PgrCatchCompletionConfig, f_active) == 64, "layout");
static_assert(offsetof(PgrCatchCompletionConfig, m_callback_invoke) == 112, "layout");
static_assert(offsetof(PgrCatchCompletionConfig, success_literal) == 120, "layout");
static_assert(offsetof(PgrCatchCompletionRequest, view) == 32, "layout");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_CC_MAX_LEASE_MS;

// --- il2cpp runtime ABI typedefs ---------------------------------------------------
using ObjClass = void *(*)(void *obj);
using FieldGet = void (*)(void *obj, void *field, void *out);
using Invoke = void *(*)(const void *method, void *obj, void **params, void **exc);

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrCatchCompletionConfig g_cfg{};
std::atomic<bool> g_installed{false};
bool g_install_attempted = false;
int g_install_error = PGR_CC_ERR_NOT_INSTALLED;

// Permanent Unity owner, latched once from a genuine on-thread call.
std::atomic<uint64_t> g_owner_tid{0};

// --- gate / token (guarded by g_state_mutex) ---------------------------------------
int g_enabled = 0;
uint64_t g_lease_until_ms = 0;
uint64_t g_epoch = 0;        // the armed epoch
uint64_t g_gen = 1;          // revocation generation; bumped on disable / epoch / dead-lease / stop
uint64_t g_token_serial = 0; // last token handed out by acquire() (monotonic == "issued")
uint64_t g_issued_gen = 0; // generation captured when the latest token was issued
uint64_t g_consumed = 0;     // last token consumed by complete()
bool g_busy = false;         // a complete() invoke is in flight

// --- telemetry mirrors (atomic so sample is lock-free) -----------------------------
std::atomic<uint64_t> g_validate_ok{0}, g_validate_fail{0};
std::atomic<uint64_t> g_complete_ok{0}, g_complete_refused{0}, g_complete_fault{0};
std::atomic<uint64_t> g_last_token{0}, g_last_tid{0};

// --- clock / tid -------------------------------------------------------------------
uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

// Typed accessors onto the immutable published config.
ObjClass obj_class() { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }
FieldGet field_get() { return reinterpret_cast<FieldGet>(g_cfg.field_get_value); }
void *managed_invoke(const void *method, void *obj, void **params, void **exc) {
  *exc = nullptr;
  return reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(method, obj, params, exc);
}

bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }

// --- field readers (object must be rooted by the caller) ---------------------------
void *field_ptr(void *obj, void *field) {
  void *out = nullptr;
  if (obj != nullptr) field_get()(obj, field, &out);
  return out;
}
uint8_t field_u8(void *obj, void *field) {
  uint8_t v = 0;
  if (obj != nullptr) field_get()(obj, field, &v);
  return v;
}
int32_t field_i32(void *obj, void *field) {
  int32_t v = 0;
  if (obj != nullptr) field_get()(obj, field, &v);
  return v;
}
uint64_t field_u64(void *obj, void *field) {
  uint64_t v = 0;
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
bool gate_live_locked(uint64_t req_epoch, uint64_t tid, uint64_t now) {
  if (!g_enabled) return false;
  if (!(g_lease_until_ms != 0 && now < g_lease_until_ms)) return false;
  if (g_epoch != req_epoch) return false;
  return current_tid() == tid && g_owner_tid.load(std::memory_order_acquire) == tid;
}

// --- identity validation (no lock; the caller keeps the pointers rooted) -----------
// Mirrors diagnostics/quick-close.js directCatchValid exactly: exact classes, active view,
// callback.m_target == view, view.dsvw == response, status == CatchSuccess, and 64-bit id equality
// across the expected id, the response CapturedPokemonId and the Pokemon id_ (never numerically
// converted). Returns 1 valid / 0 invalid.
int do_validate(const PgrCatchCompletionRequest *req) {
  if (req->expected_id == 0) return 0;
  void *view = req->view, *response = req->response, *pokemon = req->pokemon, *callback = req->callback;
  if (!is_exact(view, g_cfg.class_view)) return 0;
  if (!is_exact(response, g_cfg.class_response)) return 0;
  if (!is_exact(pokemon, g_cfg.class_pokemon)) return 0;
  if (!is_exact(callback, g_cfg.class_action)) return 0;
  if ((field_u8(view, g_cfg.f_active) & 0xFF) != 1) return 0;          // active view
  if (field_ptr(callback, g_cfg.f_delegate_target) != view) return 0;  // delegate target == view
  if (field_ptr(view, g_cfg.f_view_response) != response) return 0;    // view.dsvw == response
  if (field_i32(response, g_cfg.f_status) != g_cfg.success_literal) return 0;
  const uint64_t rid = field_u64(response, g_cfg.f_captured_id);
  const uint64_t pid = field_u64(pokemon, g_cfg.f_pokemon_id);
  if (rid != req->expected_id || pid != req->expected_id) return 0;
  return 1;
}

// --- public API --------------------------------------------------------------------
bool validate_config(const PgrCatchCompletionConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_CC_ERR_NULL_ARG; return false; }
  if (c->version != PGR_CC_CONFIG_VERSION) { *rc = PGR_CC_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrCatchCompletionConfig)) { *rc = PGR_CC_ERR_BAD_SIZE; return false; }
  void *const *ptrs = reinterpret_cast<void *const *>(&c->object_get_class);
  for (int i = 0; i < 14; ++i) {
    if (ptrs[i] == nullptr) { *rc = PGR_CC_ERR_MISSING_PTR; return false; }
  }
  *rc = PGR_CC_OK;
  return true;
}

int api_install(const PgrCatchCompletionConfig *config) {
  int rc = PGR_CC_OK;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_CC_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_cfg, config, sizeof(*config)) != 0) return PGR_CC_ERR_RECONFIG;
    return PGR_CC_OK;
  }
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;
  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;
    g_enabled = 0;
    g_lease_until_ms = 0;
    g_epoch = 0;
    g_token_serial = 0;
    g_consumed = 0;
    g_busy = false;
  }
  g_installed.store(true, std::memory_order_release);
  return g_install_error = PGR_CC_OK;
}

int api_update(int enabled, uint64_t remaining, uint64_t epoch, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_CC_ERR_NOT_INSTALLED;
  uint64_t now = 0;
  const bool clock_ok = read_boottime_ms(&now);
  std::lock_guard<std::mutex> lock(g_state_mutex);
  const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  const bool on_owner = clock_ok && proven_tid != 0 && current_tid() == proven_tid && (owner == 0 || owner == proven_tid);
  // A foreign thread, a bad clock/tid, or a disable can only REVOKE. Arming requires the owner thread.
  if (!on_owner || !enabled) {
    if (g_enabled || g_lease_until_ms) ++g_gen;
    g_enabled = 0;
    g_lease_until_ms = 0;
    if (on_owner) g_epoch = epoch;  // the owner may record a new epoch even while disabling
    return clock_ok ? PGR_CC_OK : PGR_CC_ERR_CLOCK_FAILURE;
  }
  remaining = remaining > kMaxLeaseMs ? kMaxLeaseMs : remaining;
  if (!g_enabled || g_epoch != epoch || !remaining || !g_lease_until_ms || now >= g_lease_until_ms) ++g_gen;
  g_enabled = 1;
  g_lease_until_ms = remaining ? now + remaining : 0;
  g_epoch = epoch;
  return PGR_CC_OK;
}

int api_validate(const PgrCatchCompletionRequest *request) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_CC_ERR_NOT_INSTALLED;
  if (request == nullptr) return PGR_CC_ERR_NULL_ARG;
  if (request->version != PGR_CC_REQUEST_VERSION) return PGR_CC_ERR_BAD_VERSION;
  if (request->size != sizeof(PgrCatchCompletionRequest)) return PGR_CC_ERR_BAD_SIZE;
  const uint64_t tid = current_tid();
  if (request->tid == 0 || tid != request->tid || !owner_ready(tid)) return PGR_CC_OK;  // off-owner: 0
  const int ok = do_validate(request);
  if (ok) g_validate_ok.fetch_add(1, std::memory_order_relaxed);
  else g_validate_fail.fetch_add(1, std::memory_order_relaxed);
  g_last_tid.store(tid, std::memory_order_relaxed);
  return ok ? PGR_CC_DONE : PGR_CC_OK;
}

int api_acquire(uint64_t epoch, uint64_t proven_tid, uint64_t *out_token) {
  if (out_token != nullptr) *out_token = 0;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_CC_ERR_NOT_INSTALLED;
  if (out_token == nullptr) return PGR_CC_ERR_NULL_ARG;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || !owner_ready(tid)) return PGR_CC_OK;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_CC_ERR_CLOCK_FAILURE;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_busy || !gate_live_locked(epoch, tid, now)) return PGR_CC_OK;  // refused: out_token stays 0
  if (g_token_serial >= ((uint64_t{1} << 53) - 1)) return PGR_CC_OK;   // fail closed near the ceiling
  g_issued_gen = g_gen;
  *out_token = ++g_token_serial;
  return PGR_CC_OK;
}

int api_complete(const PgrCatchCompletionRequest *request, uint64_t token) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_CC_ERR_NOT_INSTALLED;
  if (request == nullptr) return PGR_CC_ERR_NULL_ARG;
  if (request->version != PGR_CC_REQUEST_VERSION) return PGR_CC_ERR_BAD_VERSION;
  if (request->size != sizeof(PgrCatchCompletionRequest)) return PGR_CC_ERR_BAD_SIZE;
  const uint64_t tid = current_tid();
  if (request->tid == 0 || tid != request->tid || !owner_ready(tid)) {
    g_complete_refused.fetch_add(1, std::memory_order_relaxed);
    return PGR_CC_OK;
  }
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_CC_ERR_CLOCK_FAILURE;

  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    // Busy / gate / token gate, all under one lock. Reject a reused or older token.
    if (g_busy || !gate_live_locked(request->epoch, tid, now) ||
        token == 0 || token != g_token_serial || token <= g_consumed || g_issued_gen != g_gen) {
      g_complete_refused.fetch_add(1, std::memory_order_relaxed);
      return PGR_CC_OK;
    }
    // Consume the token and claim the window BEFORE any managed work.
    g_consumed = token;
    g_busy = true;
    generation = g_gen;
  }

  // Re-validate the identity immediately before the callback (no lock held). A mismatch now -- or a
  // generation move from a racing stop()/epoch change -- refuses WITHOUT invoking.
  bool refused = (do_validate(request) != 1);
  if (!refused) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    uint64_t final_now = 0;
    if (!read_boottime_ms(&final_now) || g_gen != generation ||
        !gate_live_locked(request->epoch, tid, final_now)) refused = true;
  }
  if (refused) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_busy = false;
    g_complete_refused.fetch_add(1, std::memory_order_relaxed);
    return PGR_CC_OK;
  }

  // Invoke the captured dismissal Action exactly once. No mutex is held across the managed call.
  void *exc = nullptr;
  managed_invoke(g_cfg.m_callback_invoke, request->callback, nullptr, &exc);

  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_busy = false;
  }
  g_last_token.store(token, std::memory_order_relaxed);
  g_last_tid.store(tid, std::memory_order_relaxed);
  if (exc != nullptr) {
    g_complete_fault.fetch_add(1, std::memory_order_relaxed);
    return PGR_CC_ERR_MANAGED;  // the host runs its safe ordinary-details fallback
  }
  g_complete_ok.fetch_add(1, std::memory_order_relaxed);
  return PGR_CC_DONE;
}

int api_stop(uint64_t /*proven_tid*/) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_CC_ERR_NOT_INSTALLED;
  // Immediate revoke from any thread. Does NOT touch g_busy: an in-flight invoke finishes on its own;
  // the generation bump guarantees no NEW acquire/complete and aborts a complete that has not yet
  // reached its pre-invoke generation re-check.
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_enabled = 0;
  g_lease_until_ms = 0;
  ++g_gen;
  return PGR_CC_OK;
}

int api_sample(PgrCatchCompletionSample *out) {
  if (out == nullptr) return PGR_CC_ERR_NULL_ARG;
  bool busy;
  { std::lock_guard<std::mutex> lock(g_state_mutex); busy = g_busy; }
  out->version = PGR_CC_SAMPLE_VERSION;
  out->size = sizeof(PgrCatchCompletionSample);
  out->validate_ok = g_validate_ok.load(std::memory_order_relaxed);
  out->validate_fail = g_validate_fail.load(std::memory_order_relaxed);
  out->complete_ok = g_complete_ok.load(std::memory_order_relaxed);
  out->complete_refused = g_complete_refused.load(std::memory_order_relaxed);
  out->complete_fault = g_complete_fault.load(std::memory_order_relaxed);
  out->last_token = g_last_token.load(std::memory_order_relaxed);
  out->last_tid = g_last_tid.load(std::memory_order_relaxed);
  out->busy = busy ? 1u : 0u;
  out->fault = 0u;
  return PGR_CC_OK;
}

const PgrCatchCompletionApi kApiTable = {
    PGR_CC_API_VERSION, static_cast<uint32_t>(sizeof(PgrCatchCompletionApi)),
    &api_install,  &api_update, &api_validate, &api_acquire,
    &api_complete, &api_stop,   &api_sample,
};

}  // namespace

extern "C" const void *pgrazor_catch_completion_api(uint32_t version) {
  if (version != PGR_CC_API_VERSION) return nullptr;
  return &kApiTable;
}

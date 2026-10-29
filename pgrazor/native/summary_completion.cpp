#include "summary_completion.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 36 native ordinary summary dismissal. See summary_completion.h for the ABI and the division
// of labour with the host JS. Built -fno-exceptions / -fno-rtti like every sibling: the JS "throw"
// paths become fail-closed error/invalid returns; GuiController.Dismiss() is issued at most once per
// request. No mutex is held across runtime_invoke or a field read, so a reentrant managed callback can
// never deadlock. This module installs NO hook and NEVER calls back into JS. It does not touch
// feature 24 (direct completion) or feature 3 (presentation bridge).
//
// Reentrancy / double-invoke model mirrors feature 24: acquire() hands out a strictly monotonic token
// only while armed, on the owner thread, and not busy; complete() requires it to equal the latest
// issued token AND exceed the last consumed token, CONSUMES it, and sets busy BEFORE any managed work.
// stop() bumps the revocation generation (any thread); complete() captures the generation after
// consuming the token and re-checks it right before the invoke. Borrowed input handles are duplicated
// into native temporary roots, with targets refreshed after allocations and managed calls. No managed
// pointer survives the synchronous call. The managed calls after authorization are validation (field
// reads + the op_Implicit alive probe) and the single Dismiss invoke; the postcondition is read after
// the invoke regardless of whether it threw.

static_assert(sizeof(PgrSummaryCompletionConfig) == 168, "config must be 168 bytes");
static_assert(sizeof(PgrSummaryCompletionRequest) == 72, "request must be 72 bytes");
static_assert(sizeof(PgrSummaryCompletionOutput) == 24, "output must be 24 bytes");
static_assert(sizeof(PgrSummaryCompletionSample) == 72, "sample must be 72 bytes");
static_assert(sizeof(PgrSummaryCompletionApi) == 64, "table must be 64 bytes");
static_assert(offsetof(PgrSummaryCompletionConfig, object_get_class) == 8, "layout");
static_assert(offsetof(PgrSummaryCompletionConfig, class_gui) == 40, "layout");
static_assert(offsetof(PgrSummaryCompletionConfig, f_dscx) == 72, "layout");
static_assert(offsetof(PgrSummaryCompletionConfig, m_alive) == 120, "layout");
static_assert(offsetof(PgrSummaryCompletionConfig, gc_new) == 136, "layout");
static_assert(offsetof(PgrSummaryCompletionConfig, success_literal) == 160, "layout");
static_assert(offsetof(PgrSummaryCompletionConfig, state_open) == 164, "layout");
static_assert(offsetof(PgrSummaryCompletionRequest, expected_key) == 8, "layout");
static_assert(offsetof(PgrSummaryCompletionRequest, gui) == 32, "layout");
static_assert(offsetof(PgrSummaryCompletionRequest, dismissed) == 64, "layout");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_SC_MAX_LEASE_MS;

// --- il2cpp runtime ABI typedefs ---------------------------------------------------
using ObjClass = void *(*)(void *obj);
using FieldGet = void (*)(void *obj, void *field, void *out);
using Invoke = void *(*)(const void *method, void *obj, void **params, void **exc);
using Unbox = void *(*)(void *boxed);

// alive() tri-state.
enum { ALIVE_FALSE = 0, ALIVE_TRUE = 1, ALIVE_ERR = -1 };

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrSummaryCompletionConfig g_cfg{};
std::atomic<bool> g_installed{false};
bool g_install_attempted = false;
int g_install_error = PGR_SC_ERR_NOT_INSTALLED;

// Permanent Unity owner, latched once from a genuine on-thread call.
std::atomic<uint64_t> g_owner_tid{0};

// --- gate / token (guarded by g_state_mutex) ---------------------------------------
int g_enabled = 0;
uint64_t g_lease_until_ms = 0;
uint64_t g_epoch = 0;        // the armed epoch
uint64_t g_gen = 1;          // revocation generation; bumped on disable / epoch / dead-lease / stop
uint64_t g_token_serial = 0; // last token handed out by acquire() (monotonic == "issued")
uint64_t g_issued_gen = 0;   // generation captured when the latest token was issued
uint64_t g_consumed = 0;     // last token consumed by complete()
bool g_busy = false;         // validation/completion, including root cleanup, is in flight

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
Unbox unbox() { return reinterpret_cast<Unbox>(g_cfg.object_unbox); }
void *managed_invoke(const void *method, void *obj, void **params, void **exc) {
  *exc = nullptr;
  return reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(method, obj, params, exc);
}

bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }

struct Root {
  uintptr_t handle=0;
  Root()=default;Root(const Root&)=delete;Root& operator=(const Root&)=delete;
  ~Root(){if(handle)reinterpret_cast<void(*)(uintptr_t)>(g_cfg.gc_free)(handle);}
  void *get()const{return handle?reinterpret_cast<void*(*)(uintptr_t)>(g_cfg.gc_get)(handle):nullptr;}
  bool set(void *object){if(!object)return false;handle=reinterpret_cast<uintptr_t(*)(void*,int)>(g_cfg.gc_new)(object,0);return handle&&get();}
  bool duplicate(void *borrowed){return borrowed&&set(reinterpret_cast<void*(*)(uintptr_t)>(g_cfg.gc_get)(reinterpret_cast<uintptr_t>(borrowed)));}
};
struct Inputs {
  Root gui,state,proto,response,dismissed;
  bool load(const PgrSummaryCompletionRequest *r){
    return gui.duplicate(r->gui)&&(!r->state||state.duplicate(r->state))&&proto.duplicate(r->proto)&&
      response.duplicate(r->response)&&dismissed.duplicate(r->dismissed);
  }
};
// Declared before Inputs so reentrant root cleanup still observes busy=true.
struct BusyExit {~BusyExit(){std::lock_guard<std::mutex> lock(g_state_mutex);g_busy=false;}};

// --- field readers (fresh target from an owned temporary root) ---------------------
void *field_ptr(void *obj, void *field) {
  void *out = nullptr;
  if (obj != nullptr) field_get()(obj, field, &out);
  return out;
}
int32_t field_i32(void *obj, void *field) {
  int32_t v = 0;
  if (obj != nullptr) field_get()(obj, field, &v);
  return v;
}
void field_bytes8(void *obj, void *field, uint8_t out[8]) {
  std::memset(out, 0, 8);
  if (obj != nullptr) field_get()(obj, field, out);
}

// alive(gui): mirrors quick-close.js alive() -- static op_Implicit returns a boxed System.Boolean;
// require the exact BOOL class and an unboxed byte of exactly 0 or 1. Any throw / null / wrong class /
// out-of-range byte fails closed (ALIVE_ERR). Root the box before inspecting it.
int alive_state(const Root &gui) {
  void *object=gui.get();if(!object)return ALIVE_ERR;
  void *params[1] = {object}; // IL2CPP reference argument is the object pointer itself.
  void *exc = nullptr;
  void *boxed = managed_invoke(g_cfg.m_alive, nullptr, params, &exc);
  Root value;
  if (exc != nullptr || !value.set(boxed)) return ALIVE_ERR;
  if (!is_exact(value.get(),g_cfg.class_bool)) return ALIVE_ERR;
  void *raw = unbox()(value.get());
  if (raw == nullptr) return ALIVE_ERR;
  uint8_t v = *static_cast<uint8_t *>(raw);
  if (v > 1) return ALIVE_ERR;
  return v == 1 ? ALIVE_TRUE : ALIVE_FALSE;
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

// --- identity validation (no lock; Inputs keeps native-owned temporary roots) ------
// Mirrors diagnostics/quick-close.js summary branch exactly: exact SGUI, alive gui, proto exact RESP
// with CatchSuccess status and the expected nonzero 8-byte id, response == proto, optional SSTATE with
// dscx == gui / dscy == proto, currentState == STATE_OPEN, and onDismissed non-null == the captured
// callback. On success *out_before (optional) receives the validated currentState. Returns 1/0.
int do_validate(const PgrSummaryCompletionRequest *req, const Inputs &in, int32_t *out_before) {
  // The expected caught id must be nonzero (8 raw bytes).
  bool any = false;
  for (int i = 0; i < 8; ++i) any = any || (req->expected_key[i] != 0);
  if (!any) return 0;

  if (!is_exact(in.gui.get(), g_cfg.class_gui)) return 0;
  if (alive_state(in.gui) != ALIVE_TRUE) return 0;
  if (!is_exact(in.gui.get(),g_cfg.class_gui)) return 0;

  if (!is_exact(in.proto.get(), g_cfg.class_response)) return 0;
  if (in.response.get() != in.proto.get()) return 0;
  if (field_i32(in.proto.get(), g_cfg.f_status) != g_cfg.success_literal) return 0;
  uint8_t key[8];
  field_bytes8(in.proto.get(), g_cfg.f_captured_id, key);
  if (std::memcmp(key, req->expected_key, 8) != 0) return 0;

  if (req->state != nullptr) {
    if (!is_exact(in.state.get(), g_cfg.class_state)) return 0;
    if (field_ptr(in.state.get(), g_cfg.f_dscx) != in.gui.get()) return 0;
    if (field_ptr(in.state.get(), g_cfg.f_dscy) != in.proto.get()) return 0;
  }

  int32_t before = field_i32(in.gui.get(), g_cfg.f_current_state);
  if (before != g_cfg.state_open) return 0;

  void *on_dismissed = field_ptr(in.gui.get(), g_cfg.f_on_dismissed);
  if (on_dismissed == nullptr || on_dismissed != in.dismissed.get()) return 0;

  if (out_before != nullptr) *out_before = before;
  return 1;
}

// Postcondition after the Dismiss invoke, mirroring the JS try/catch at ~1298:
//   if exact(gui, SGUI) && alive(gui) -> after = currentState ; else gone = true
//   a throwing alive probe -> after unknown, gone stays false.
void read_postcondition(const Root &gui, uint32_t *flags, int32_t *after) {
  if (!gui.get())return; // a failed root lookup does not prove Unity destruction
  if (!is_exact(gui.get(), g_cfg.class_gui)) { *flags |= PGR_SC_FLAG_GONE; return; }
  const int a = alive_state(gui);
  if (a == ALIVE_TRUE) {
    if (!is_exact(gui.get(),g_cfg.class_gui)) return;
    *after = field_i32(gui.get(), g_cfg.f_current_state);
    *flags |= PGR_SC_FLAG_AFTER_KNOWN;
  } else if (a == ALIVE_FALSE) {
    *flags |= PGR_SC_FLAG_GONE;
  }
  // ALIVE_ERR: after stays unknown, gone stays clear (matches the JS catch).
}

void init_output(PgrSummaryCompletionOutput *out) {
  out->version = PGR_SC_OUTPUT_VERSION;
  out->size = sizeof(PgrSummaryCompletionOutput);
  out->flags = 0;
  out->status = PGR_SC_OK;
  out->before = 0;
  out->after = 0;
}

// --- public API --------------------------------------------------------------------
bool validate_config(const PgrSummaryCompletionConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_SC_ERR_NULL_ARG; return false; }
  if (c->version != PGR_SC_CONFIG_VERSION) { *rc = PGR_SC_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrSummaryCompletionConfig)) { *rc = PGR_SC_ERR_BAD_SIZE; return false; }
  void *const *ptrs = reinterpret_cast<void *const *>(&c->object_get_class);
  for (int i = 0; i < 19; ++i) {
    if (ptrs[i] == nullptr) { *rc = PGR_SC_ERR_MISSING_PTR; return false; }
  }
  *rc = PGR_SC_OK;
  return true;
}

int api_install(const PgrSummaryCompletionConfig *config) {
  int rc = PGR_SC_OK;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_SC_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_cfg, config, sizeof(*config)) != 0) return PGR_SC_ERR_RECONFIG;
    return PGR_SC_OK;
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
  return g_install_error = PGR_SC_OK;
}

int api_update(int enabled, uint64_t remaining, uint64_t epoch, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SC_ERR_NOT_INSTALLED;
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
    return clock_ok ? PGR_SC_OK : PGR_SC_ERR_CLOCK_FAILURE;
  }
  remaining = remaining > kMaxLeaseMs ? kMaxLeaseMs : remaining;
  if (!g_enabled || g_epoch != epoch || !remaining || !g_lease_until_ms || now >= g_lease_until_ms) ++g_gen;
  g_enabled = 1;
  g_lease_until_ms = remaining ? now + remaining : 0;
  g_epoch = epoch;
  return PGR_SC_OK;
}

int api_validate(const PgrSummaryCompletionRequest *request) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SC_ERR_NOT_INSTALLED;
  if (request == nullptr) return PGR_SC_ERR_NULL_ARG;
  if (request->version != PGR_SC_REQUEST_VERSION) return PGR_SC_ERR_BAD_VERSION;
  if (request->size != sizeof(PgrSummaryCompletionRequest)) return PGR_SC_ERR_BAD_SIZE;
  const uint64_t tid = current_tid();
  if (request->tid == 0 || tid != request->tid || !owner_ready(tid)) return PGR_SC_OK;  // off-owner: 0
  {std::lock_guard<std::mutex> lock(g_state_mutex);if(g_busy)return PGR_SC_OK;g_busy=true;}
  BusyExit busyExit;
  Inputs in;
  const int ok = in.load(request)&&do_validate(request,in,nullptr);
  if (ok) g_validate_ok.fetch_add(1, std::memory_order_relaxed);
  else g_validate_fail.fetch_add(1, std::memory_order_relaxed);
  g_last_tid.store(tid, std::memory_order_relaxed);
  return ok ? PGR_SC_DONE : PGR_SC_OK;
}

int api_acquire(uint64_t epoch, uint64_t proven_tid, uint64_t *out_token) {
  if (out_token != nullptr) *out_token = 0;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SC_ERR_NOT_INSTALLED;
  if (out_token == nullptr) return PGR_SC_ERR_NULL_ARG;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || !owner_ready(tid)) return PGR_SC_OK;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_SC_ERR_CLOCK_FAILURE;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_busy || !gate_live_locked(epoch, tid, now)) return PGR_SC_OK;  // refused: out_token stays 0
  if (g_token_serial >= ((uint64_t{1} << 53) - 1)) return PGR_SC_OK;   // fail closed near the ceiling
  g_issued_gen = g_gen;
  *out_token = ++g_token_serial;
  return PGR_SC_OK;
}

int api_complete(const PgrSummaryCompletionRequest *request, uint64_t token,
                 PgrSummaryCompletionOutput *out) {
  if (out == nullptr) return PGR_SC_ERR_NULL_ARG;  // cannot report; caller must pass a buffer
  init_output(out);
  if (!g_installed.load(std::memory_order_acquire)) return out->status = PGR_SC_ERR_NOT_INSTALLED;
  if (request == nullptr) return out->status = PGR_SC_ERR_NULL_ARG;
  if (request->version != PGR_SC_REQUEST_VERSION) return out->status = PGR_SC_ERR_BAD_VERSION;
  if (request->size != sizeof(PgrSummaryCompletionRequest)) return out->status = PGR_SC_ERR_BAD_SIZE;
  const uint64_t tid = current_tid();
  if (request->tid == 0 || tid != request->tid || !owner_ready(tid)) {
    g_complete_refused.fetch_add(1, std::memory_order_relaxed);
    return out->status = PGR_SC_OK;
  }
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return out->status = PGR_SC_ERR_CLOCK_FAILURE;

  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    // Busy / gate / token gate, all under one lock. Reject a reused or older token.
    if (g_busy || !gate_live_locked(request->epoch, tid, now) ||
        token == 0 || token != g_token_serial || token <= g_consumed || g_issued_gen != g_gen) {
      g_complete_refused.fetch_add(1, std::memory_order_relaxed);
      return out->status = PGR_SC_OK;
    }
    // Consume the token and claim the window BEFORE any managed work.
    g_consumed = token;
    g_busy = true;
    generation = g_gen;
  }

  BusyExit busyExit;
  Inputs in;
  // Re-validate the identity immediately before the invoke (no lock held). A mismatch now -- or a
  // generation move from a racing stop()/epoch change -- refuses WITHOUT invoking.
  int32_t before = 0;
  bool refused = !in.load(request)||do_validate(request,in,&before)!=1;
  if (!refused) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    uint64_t final_now = 0;
    if (!read_boottime_ms(&final_now) || g_gen != generation ||
        !gate_live_locked(request->epoch, tid, final_now)) refused = true;
  }
  if (refused) {
    g_complete_refused.fetch_add(1, std::memory_order_relaxed);
    return out->status = PGR_SC_OK;
  }

  // Commit: record the validated pre-invoke state and that we are about to invoke, then run the
  // ordinary inherited GuiController.Dismiss() exactly once. No mutex is held across the managed call.
  out->before = before;
  out->flags |= PGR_SC_FLAG_BEFORE_KNOWN | PGR_SC_FLAG_INVOKED;
  void *exc = nullptr;
  managed_invoke(g_cfg.m_dismiss, in.gui.get(), nullptr, &exc);

  // Read the postcondition regardless of whether Dismiss threw (mirrors the JS try/catch).
  int32_t after = 0;
  read_postcondition(in.gui, &out->flags, &after);
  if (out->flags & PGR_SC_FLAG_AFTER_KNOWN) out->after = after;

  g_last_token.store(token, std::memory_order_relaxed);
  g_last_tid.store(tid, std::memory_order_relaxed);
  if (exc != nullptr) {
    g_complete_fault.fetch_add(1, std::memory_order_relaxed);
    return out->status = PGR_SC_ERR_MANAGED;  // invoked == true; never retry Dismiss
  }
  g_complete_ok.fetch_add(1, std::memory_order_relaxed);
  return out->status = PGR_SC_DONE;
}

int api_stop(uint64_t /*proven_tid*/) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SC_ERR_NOT_INSTALLED;
  // Immediate revoke from any thread. Does NOT touch g_busy: an in-flight invoke finishes on its own;
  // the generation bump guarantees no NEW acquire/complete and aborts a complete that has not yet
  // reached its pre-invoke generation re-check.
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_enabled = 0;
  g_lease_until_ms = 0;
  ++g_gen;
  return PGR_SC_OK;
}

int api_sample(PgrSummaryCompletionSample *out) {
  if (out == nullptr) return PGR_SC_ERR_NULL_ARG;
  bool busy;
  { std::lock_guard<std::mutex> lock(g_state_mutex); busy = g_busy; }
  out->version = PGR_SC_SAMPLE_VERSION;
  out->size = sizeof(PgrSummaryCompletionSample);
  out->validate_ok = g_validate_ok.load(std::memory_order_relaxed);
  out->validate_fail = g_validate_fail.load(std::memory_order_relaxed);
  out->complete_ok = g_complete_ok.load(std::memory_order_relaxed);
  out->complete_refused = g_complete_refused.load(std::memory_order_relaxed);
  out->complete_fault = g_complete_fault.load(std::memory_order_relaxed);
  out->last_token = g_last_token.load(std::memory_order_relaxed);
  out->last_tid = g_last_tid.load(std::memory_order_relaxed);
  out->busy = busy ? 1u : 0u;
  out->fault = 0u;
  return PGR_SC_OK;
}

const PgrSummaryCompletionApi kApiTable = {
    PGR_SC_API_VERSION, static_cast<uint32_t>(sizeof(PgrSummaryCompletionApi)),
    &api_install,  &api_update, &api_validate, &api_acquire,
    &api_complete, &api_stop,   &api_sample,
};

}  // namespace

extern "C" const void *pgrazor_summary_completion_api(uint32_t version) {
  if (version != PGR_SC_API_VERSION) return nullptr;
  return &kApiTable;
}

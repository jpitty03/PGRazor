#include "details_completion.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 37 native post-catch details completion. See details_completion.h for the ABI and the
// division of labour with the host JS. The gate/token/lease/owner machinery is identical to feature 36
// (summary_completion.cpp); only the identity proof differs (details scene + one-element backing list).
// Built -fno-exceptions / -fno-rtti like every sibling: the JS "throw" paths become fail-closed
// error/invalid returns; the details click is issued at most once per request. No mutex is held across
// runtime_invoke or a field read. This module installs NO hook and NEVER calls back into JS.
//
// Borrowed input handles are duplicated into native temporary roots and targets are re-resolved after
// each allocation / managed call (op_Implicit, list.count(), list.item(), the click). No managed
// pointer survives the synchronous call; integer arguments are passed by address while reference
// arguments are the object pointer itself.

static_assert(sizeof(PgrDetailsCompletionConfig) == 272, "config must be 272 bytes");
static_assert(sizeof(PgrDetailsCompletionRequest) == 80, "request must be 80 bytes");
static_assert(sizeof(PgrDetailsCompletionOutput) == 24, "output must be 24 bytes");
static_assert(sizeof(PgrDetailsCompletionSample) == 72, "sample must be 72 bytes");
static_assert(sizeof(PgrDetailsCompletionApi) == 64, "table must be 64 bytes");
static_assert(offsetof(PgrDetailsCompletionConfig, object_get_class) == 8, "layout");
static_assert(offsetof(PgrDetailsCompletionConfig, class_gui) == 64, "layout");
static_assert(offsetof(PgrDetailsCompletionConfig, m_alive) == 136, "layout");
static_assert(offsetof(PgrDetailsCompletionConfig, f_current_state) == 168, "layout");
static_assert(offsetof(PgrDetailsCompletionConfig, f_list) == 256, "layout");
static_assert(offsetof(PgrDetailsCompletionConfig, success_literal) == 264, "layout");
static_assert(offsetof(PgrDetailsCompletionConfig, state_open) == 268, "layout");
static_assert(offsetof(PgrDetailsCompletionRequest, expected_key) == 8, "layout");
static_assert(offsetof(PgrDetailsCompletionRequest, gui) == 32, "layout");
static_assert(offsetof(PgrDetailsCompletionRequest, view) == 40, "layout");
static_assert(offsetof(PgrDetailsCompletionRequest, dismissed) == 72, "layout");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_DC_MAX_LEASE_MS;
constexpr int kConfigPtrCount = 32;  // validated pointers between the header and the two int32s

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
PgrDetailsCompletionConfig g_cfg{};
std::atomic<bool> g_installed{false};
bool g_install_attempted = false;
int g_install_error = PGR_DC_ERR_NOT_INSTALLED;

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
  Root gui,view,response,data,pokemon,dismissed;
  bool load(const PgrDetailsCompletionRequest *r){
    return gui.duplicate(r->gui)&&view.duplicate(r->view)&&response.duplicate(r->response)&&
      data.duplicate(r->data)&&pokemon.duplicate(r->pokemon)&&dismissed.duplicate(r->dismissed);
  }
};
// Declared before Inputs cleanup so reentrant root cleanup still observes busy=true.
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
uint8_t field_u8(void *obj, void *field) {
  uint8_t v = 0;
  if (obj != nullptr) field_get()(obj, field, &v);
  return v;
}
void field_bytes8(void *obj, void *field, uint8_t out[8]) {
  std::memset(out, 0, 8);
  if (obj != nullptr) field_get()(obj, field, out);
}

// alive(gui): static op_Implicit returns a boxed System.Boolean; require the exact BOOL class and an
// unboxed byte of exactly 0 or 1. Any throw / null / wrong class / out-of-range byte fails closed. The
// reference argument is the object pointer itself (never &pointer-local). Root the box before reading.
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

// --- identity: the scalar/pointer field relationships (no managed calls) -----------
// Re-run before AND after the count()/item() managed calls so a mutation or relocation during those
// calls is caught. Each read resolves its object freshly through the owned temporary root.
bool exact_inputs(const Inputs &in) {
  return is_exact(in.gui.get(),g_cfg.class_gui)&&is_exact(in.view.get(),g_cfg.class_view)&&
    is_exact(in.response.get(),g_cfg.class_response)&&is_exact(in.data.get(),g_cfg.class_data)&&
    is_exact(in.pokemon.get(),g_cfg.class_pokemon)&&is_exact(in.dismissed.get(),g_cfg.class_action);
}
bool check_identities(const PgrDetailsCompletionRequest *req, const Inputs &in) {
  if (!exact_inputs(in)) return false;
  if (field_ptr(in.view.get(), g_cfg.f_view_response) != in.response.get()) return false;
  if (field_i32(in.response.get(), g_cfg.f_status) != g_cfg.success_literal) return false;
  uint8_t key[8];
  field_bytes8(in.response.get(), g_cfg.f_caught_id, key);
  if (std::memcmp(key, req->expected_key, 8) != 0) return false;
  field_bytes8(in.pokemon.get(), g_cfg.f_pokemon_id, key);
  if (std::memcmp(key, req->expected_key, 8) != 0) return false;
  if (field_ptr(in.dismissed.get(), g_cfg.f_delegate_target) != in.view.get()) return false;
  if (field_ptr(in.gui.get(), g_cfg.f_gui_data) != in.data.get()) return false;
  if (field_ptr(in.gui.get(), g_cfg.f_gui_callback) != in.dismissed.get()) return false;
  if (field_ptr(in.data.get(), g_cfg.f_data_callback) != in.dismissed.get()) return false;
  if (field_u8(in.data.get(), g_cfg.f_post) != 1) return false;   // rejects ordinary inventory details
  if (field_i32(in.data.get(), g_cfg.f_index) != 0) return false;
  return true;
}

// --- full details identity validation (no lock; Inputs keeps native-owned roots) ---
// Mirrors diagnostics/quick-close.js details branch: all six objects at exact classes, alive gui, the
// scalar identities, and a one-element backing list whose item(0) is the caught pokemon. require_open
// adds currentState == STATE_OPEN (complete() only; validate() tolerates an opening canvas). On success
// *out_before (optional) receives the read currentState. Returns 1/0.
int do_validate(const PgrDetailsCompletionRequest *req, const Inputs &in,
                bool require_open, int32_t *out_before) {
  // The expected caught id must be nonzero (8 raw bytes).
  bool any = false;
  for (int i = 0; i < 8; ++i) any = any || (req->expected_key[i] != 0);
  if (!any) return 0;

  if (!is_exact(in.gui.get(), g_cfg.class_gui)) return 0;
  if (alive_state(in.gui) != ALIVE_TRUE) return 0;
  if (!is_exact(in.gui.get(), g_cfg.class_gui)) return 0;  // re-check after the managed alive probe

  if (!is_exact(in.view.get(), g_cfg.class_view)) return 0;
  if (!is_exact(in.response.get(), g_cfg.class_response)) return 0;
  if (!is_exact(in.data.get(), g_cfg.class_data)) return 0;
  if (!is_exact(in.pokemon.get(), g_cfg.class_pokemon)) return 0;
  if (!is_exact(in.dismissed.get(), g_cfg.class_action)) return 0;

  if (!check_identities(req, in)) return 0;

  // Root the backing list, then prove count()==1 and item(0)==pokemon. Both are managed calls that may
  // relocate; the list, the boxed count and the item result are rooted immediately.
  Root list;
  if (!list.set(field_ptr(in.data.get(), g_cfg.f_list))) return 0;
  if (!exact_inputs(in) || !is_exact(list.get(), g_cfg.class_list)) return 0;

  {
    void *exc = nullptr;
    void *boxed = managed_invoke(g_cfg.m_count, list.get(), nullptr, &exc);
    Root count_box;
    if (exc != nullptr || !count_box.set(boxed)) return 0;
    if (!is_exact(count_box.get(), g_cfg.class_int)) return 0;
    void *raw = unbox()(count_box.get());
    if (raw == nullptr) return 0;
    if (*static_cast<int32_t *>(raw) != 1) return 0;
  }
  if (!exact_inputs(in) || !is_exact(list.get(),g_cfg.class_list) ||
      !check_identities(req,in) || field_ptr(in.data.get(),g_cfg.f_list)!=list.get()) return 0;
  {
    int32_t index = 0;
    void *params[1] = {&index};  // value-type index passed BY ADDRESS
    void *exc = nullptr;
    void *obj = managed_invoke(g_cfg.m_item, list.get(), params, &exc);
    Root item;
    if (exc != nullptr || !item.set(obj)) return 0;
    if (item.get() != in.pokemon.get()) return 0;
  }

  if (!exact_inputs(in) || !is_exact(list.get(),g_cfg.class_list)) return 0;
  if (require_open && alive_state(in.gui)!=ALIVE_TRUE) return 0;
  if (!exact_inputs(in) || !is_exact(list.get(),g_cfg.class_list)) return 0;

  // count()/item() may have mutated or relocated the scene: re-prove the scalar identities and that
  // data still backs the same rooted list.
  if (!check_identities(req, in)) return 0;
  if (field_ptr(in.data.get(), g_cfg.f_list) != list.get()) return 0;

  int32_t before = field_i32(in.gui.get(), g_cfg.f_current_state);
  if (require_open && before != g_cfg.state_open) return 0;
  if (out_before != nullptr) *out_before = before;
  return 1;
}

// Postcondition after the click invoke. GONE is set ONLY by a definite alive==FALSE: a failed post-root
// lookup, a class-check mismatch, or a throwing alive probe are UNKNOWN (neither AFTER_KNOWN nor GONE),
// never proof of Unity destruction. 'after' is read only when the gui is still the exact class AND the
// probe reports it alive.
void read_postcondition(const Root &gui, uint32_t *flags, int32_t *after) {
  void *obj = gui.get();
  if (!obj) return;                                  // failed root lookup: unknown, not proof gone
  if (!is_exact(obj, g_cfg.class_gui)) return;       // class mismatch: unknown, not proof gone
  const int a = alive_state(gui);
  if (a == ALIVE_TRUE) {
    if (!is_exact(gui.get(), g_cfg.class_gui)) return;  // the alive probe may itself have invalidated it
    *after = field_i32(gui.get(), g_cfg.f_current_state);
    *flags |= PGR_DC_FLAG_AFTER_KNOWN;
  } else if (a == ALIVE_FALSE) {
    *flags |= PGR_DC_FLAG_GONE;                      // the only definite proof of destruction
  }
  // ALIVE_ERR: unknown -- after stays unknown, gone stays clear.
}

void init_output(PgrDetailsCompletionOutput *out) {
  out->version = PGR_DC_OUTPUT_VERSION;
  out->size = sizeof(PgrDetailsCompletionOutput);
  out->flags = 0;
  out->status = PGR_DC_OK;
  out->before = 0;
  out->after = 0;
}

// --- public API --------------------------------------------------------------------
bool validate_config(const PgrDetailsCompletionConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_DC_ERR_NULL_ARG; return false; }
  if (c->version != PGR_DC_CONFIG_VERSION) { *rc = PGR_DC_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrDetailsCompletionConfig)) { *rc = PGR_DC_ERR_BAD_SIZE; return false; }
  void *const *ptrs = reinterpret_cast<void *const *>(&c->object_get_class);
  for (int i = 0; i < kConfigPtrCount; ++i) {
    if (ptrs[i] == nullptr) { *rc = PGR_DC_ERR_MISSING_PTR; return false; }
  }
  *rc = PGR_DC_OK;
  return true;
}

int api_install(const PgrDetailsCompletionConfig *config) {
  int rc = PGR_DC_OK;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_DC_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_cfg, config, sizeof(*config)) != 0) return PGR_DC_ERR_RECONFIG;
    return PGR_DC_OK;
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
  return g_install_error = PGR_DC_OK;
}

int api_update(int enabled, uint64_t remaining, uint64_t epoch, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_DC_ERR_NOT_INSTALLED;
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
    return clock_ok ? PGR_DC_OK : PGR_DC_ERR_CLOCK_FAILURE;
  }
  remaining = remaining > kMaxLeaseMs ? kMaxLeaseMs : remaining;
  if (!g_enabled || g_epoch != epoch || !remaining || !g_lease_until_ms || now >= g_lease_until_ms) ++g_gen;
  g_enabled = 1;
  g_lease_until_ms = remaining ? now + remaining : 0;
  g_epoch = epoch;
  return PGR_DC_OK;
}

int api_validate(const PgrDetailsCompletionRequest *request) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_DC_ERR_NOT_INSTALLED;
  if (request == nullptr) return PGR_DC_ERR_NULL_ARG;
  if (request->version != PGR_DC_REQUEST_VERSION) return PGR_DC_ERR_BAD_VERSION;
  if (request->size != sizeof(PgrDetailsCompletionRequest)) return PGR_DC_ERR_BAD_SIZE;
  const uint64_t tid = current_tid();
  if (request->tid == 0 || tid != request->tid || !owner_ready(tid)) return PGR_DC_OK;  // off-owner: 0
  {std::lock_guard<std::mutex> lock(g_state_mutex);if(g_busy)return PGR_DC_OK;g_busy=true;}
  BusyExit busyExit;
  Inputs in;
  // validate() tolerates an opening canvas: require_open is false here (no STATE_OPEN gate).
  const int ok = in.load(request)&&do_validate(request,in,/*require_open=*/false,nullptr);
  if (ok) g_validate_ok.fetch_add(1, std::memory_order_relaxed);
  else g_validate_fail.fetch_add(1, std::memory_order_relaxed);
  g_last_tid.store(tid, std::memory_order_relaxed);
  return ok ? PGR_DC_DONE : PGR_DC_OK;
}

int api_acquire(uint64_t epoch, uint64_t proven_tid, uint64_t *out_token) {
  if (out_token != nullptr) *out_token = 0;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_DC_ERR_NOT_INSTALLED;
  if (out_token == nullptr) return PGR_DC_ERR_NULL_ARG;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || !owner_ready(tid)) return PGR_DC_OK;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_DC_ERR_CLOCK_FAILURE;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_busy || !gate_live_locked(epoch, tid, now)) return PGR_DC_OK;  // refused: out_token stays 0
  if (g_token_serial >= ((uint64_t{1} << 53) - 1)) return PGR_DC_OK;   // fail closed near the ceiling
  g_issued_gen = g_gen;
  *out_token = ++g_token_serial;
  return PGR_DC_OK;
}

int api_complete(const PgrDetailsCompletionRequest *request, uint64_t token,
                 PgrDetailsCompletionOutput *out) {
  if (out == nullptr) return PGR_DC_ERR_NULL_ARG;  // cannot report; caller must pass a buffer
  init_output(out);
  if (!g_installed.load(std::memory_order_acquire)) return out->status = PGR_DC_ERR_NOT_INSTALLED;
  if (request == nullptr) return out->status = PGR_DC_ERR_NULL_ARG;
  if (request->version != PGR_DC_REQUEST_VERSION) return out->status = PGR_DC_ERR_BAD_VERSION;
  if (request->size != sizeof(PgrDetailsCompletionRequest)) return out->status = PGR_DC_ERR_BAD_SIZE;
  const uint64_t tid = current_tid();
  if (request->tid == 0 || tid != request->tid || !owner_ready(tid)) {
    g_complete_refused.fetch_add(1, std::memory_order_relaxed);
    return out->status = PGR_DC_OK;
  }
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return out->status = PGR_DC_ERR_CLOCK_FAILURE;

  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    // Busy / gate / token gate, all under one lock. Reject a reused or older token.
    if (g_busy || !gate_live_locked(request->epoch, tid, now) ||
        token == 0 || token != g_token_serial || token <= g_consumed || g_issued_gen != g_gen) {
      g_complete_refused.fetch_add(1, std::memory_order_relaxed);
      return out->status = PGR_DC_OK;
    }
    // Consume the token and claim the window BEFORE any managed work.
    g_consumed = token;
    g_busy = true;
    generation = g_gen;
  }

  BusyExit busyExit;
  Inputs in;
  // Re-validate the identity (incl. STATE_OPEN) immediately before the invoke (no lock held). A
  // mismatch now -- or a generation move from a racing stop()/epoch change -- refuses WITHOUT invoking.
  int32_t before = 0;
  bool refused = !in.load(request)||do_validate(request,in,/*require_open=*/true,&before)!=1;
  if (!refused) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    uint64_t final_now = 0;
    if (!read_boottime_ms(&final_now) || g_gen != generation ||
        !gate_live_locked(request->epoch, tid, final_now)) refused = true;
  }
  if (refused) {
    g_complete_refused.fetch_add(1, std::memory_order_relaxed);
    return out->status = PGR_DC_OK;
  }

  // Commit: record the validated pre-invoke state and that we are about to invoke, then run the
  // details click exactly once. No mutex is held across the managed call.
  out->before = before;
  out->flags |= PGR_DC_FLAG_BEFORE_KNOWN | PGR_DC_FLAG_INVOKED;
  void *exc = nullptr;
  managed_invoke(g_cfg.m_click, in.gui.get(), nullptr, &exc);

  // Read the postcondition regardless of whether click threw (mirrors the JS try/catch).
  int32_t after = 0;
  read_postcondition(in.gui, &out->flags, &after);
  if (out->flags & PGR_DC_FLAG_AFTER_KNOWN) out->after = after;

  g_last_token.store(token, std::memory_order_relaxed);
  g_last_tid.store(tid, std::memory_order_relaxed);
  if (exc != nullptr) {
    g_complete_fault.fetch_add(1, std::memory_order_relaxed);
    return out->status = PGR_DC_ERR_MANAGED;  // invoked == true; never retry the click
  }
  g_complete_ok.fetch_add(1, std::memory_order_relaxed);
  return out->status = PGR_DC_DONE;
}

int api_stop(uint64_t /*proven_tid*/) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_DC_ERR_NOT_INSTALLED;
  // Immediate revoke from any thread. Does NOT touch g_busy: an in-flight invoke finishes on its own;
  // the generation bump guarantees no NEW acquire/complete and aborts a complete that has not yet
  // reached its pre-invoke generation re-check.
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_enabled = 0;
  g_lease_until_ms = 0;
  ++g_gen;
  return PGR_DC_OK;
}

int api_sample(PgrDetailsCompletionSample *out) {
  if (out == nullptr) return PGR_DC_ERR_NULL_ARG;
  bool busy;
  { std::lock_guard<std::mutex> lock(g_state_mutex); busy = g_busy; }
  out->version = PGR_DC_SAMPLE_VERSION;
  out->size = sizeof(PgrDetailsCompletionSample);
  out->validate_ok = g_validate_ok.load(std::memory_order_relaxed);
  out->validate_fail = g_validate_fail.load(std::memory_order_relaxed);
  out->complete_ok = g_complete_ok.load(std::memory_order_relaxed);
  out->complete_refused = g_complete_refused.load(std::memory_order_relaxed);
  out->complete_fault = g_complete_fault.load(std::memory_order_relaxed);
  out->last_token = g_last_token.load(std::memory_order_relaxed);
  out->last_tid = g_last_tid.load(std::memory_order_relaxed);
  out->busy = busy ? 1u : 0u;
  out->fault = 0u;
  return PGR_DC_OK;
}

const PgrDetailsCompletionApi kApiTable = {
    PGR_DC_API_VERSION, static_cast<uint32_t>(sizeof(PgrDetailsCompletionApi)),
    &api_install,  &api_update, &api_validate, &api_acquire,
    &api_complete, &api_stop,   &api_sample,
};

}  // namespace

extern "C" const void *pgrazor_details_completion_api(uint32_t version) {
  if (version != PGR_DC_API_VERSION) return nullptr;
  return &kApiTable;
}

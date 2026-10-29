#include "catch_canvas.h"
#include "hook_engine.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 39 native catch-canvas visibility control. See catch_canvas.h for the ABI and the division of
// labour with the host JS. This module owns BOTH the set_CanvasGroupVisible hook (true->false masking
// for owned opening-details roots) and the managed hide/restore operations. The gate/lease/owner/epoch
// machinery mirrors feature 36/37 (summary/details completion); the GC rooting mirrors feature 10's
// spin_camera owned-root slots. Built -fno-exceptions / -fno-rtti like every sibling: a JS "throw"
// becomes a fail-closed refusal. No mutex is held across runtime_invoke, a field read or a GC op; the
// hook NEVER invokes a managed method or JS and NEVER latches the owner.

static_assert(sizeof(PgrCatchCanvasConfig) == 128, "config must be 128 bytes");
static_assert(sizeof(PgrCatchCanvasSample) == 40, "sample must be 40 bytes");
static_assert(sizeof(PgrCatchCanvasApi) == 72, "table must be 72 bytes");
static_assert(offsetof(PgrCatchCanvasConfig, object_get_class) == 8, "layout");
static_assert(offsetof(PgrCatchCanvasConfig, class_summary) == 64, "layout");
static_assert(offsetof(PgrCatchCanvasConfig, f_current_state) == 88, "layout");
static_assert(offsetof(PgrCatchCanvasConfig, canvas_address) == 112, "layout");
static_assert(offsetof(PgrCatchCanvasConfig, state_open) == 120, "layout");
static_assert(offsetof(PgrCatchCanvasConfig, reserved0) == 124, "layout");
static_assert(offsetof(PgrCatchCanvasSample, masked_count) == 16, "layout");
static_assert(offsetof(PgrCatchCanvasSample, hide_count) == 24, "layout");
static_assert(offsetof(PgrCatchCanvasSample, restore_count) == 32, "layout");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_CANVAS_MAX_LEASE_MS;
constexpr int kConfigPtrCount = 14;  // validated pointers between the header and state_open
constexpr uint64_t kTokenCeiling = (uint64_t{1} << 53) - 1;

// --- il2cpp runtime ABI typedefs ---------------------------------------------------
using ObjClass = void *(*)(void *obj);
using FieldGet = void (*)(void *obj, void *field, void *out);
using Invoke = void *(*)(const void *method, void *obj, void **params, void **exc);
using Unbox = void *(*)(void *boxed);
using GcNew = uintptr_t (*)(void *obj, int pinned);
using GcGet = void *(*)(uintptr_t handle);
using GcFree = void (*)(uintptr_t handle);
// set_CanvasGroupVisible(self, visible, MethodInfo*).
using CanvasSetterFn = void (*)(void *self, bool visible, void *method_info);

enum { ALIVE_FALSE = 0, ALIVE_TRUE = 1, ALIVE_ERR = -1 };

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrCatchCanvasConfig g_cfg{};
std::atomic<bool> g_installed{false};
std::mutex g_observer_mutex;
PgrCatchCanvasObserver g_observer{};
std::atomic<bool> g_observer_ready{false};
std::atomic<uint32_t> g_observer_frames{0};
bool g_install_attempted = false;
int g_install_error = PGR_CANVAS_ERR_NOT_INSTALLED;

// The original set_CanvasGroupVisible, published atomically before the bound address goes live.
std::atomic<CanvasSetterFn> g_orig_canvas{nullptr};

// Permanent Unity owner, latched once from a genuine on-thread API call (never by the hook).
std::atomic<uint64_t> g_owner_tid{0};

// --- gate (guarded by g_state_mutex) -----------------------------------------------
int g_enabled = 0;
uint64_t g_lease_until_ms = 0;
uint64_t g_epoch = 0;
uint64_t g_gen = 1;          // revocation generation; bumped on disable / epoch / dead-lease / stop
uint64_t g_token_serial = 0; // last token handed out by hide_opening (monotonic nonzero)
bool g_busy = false;         // a managed hide/restore (incl. root cleanup) is in flight

// --- owner-thread-only slots (one per hidden opening-details GUI) -------------------
struct Slot {
  uintptr_t handle = 0;  // native-owned GC root duplicating the GUI
  uint64_t epoch = 0;
  uint64_t token = 0;
};
Slot g_slots[PGR_CANVAS_MAX_SLOTS]{};

// --- telemetry mirrors (atomic so sample is lock-free) -----------------------------
std::atomic<uint32_t> g_owned{0}, g_fault{0};
std::atomic<uint64_t> g_masked_count{0}, g_hide_count{0}, g_restore_count{0};

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
void *gc_get(uintptr_t handle) {
  return handle ? reinterpret_cast<GcGet>(g_cfg.gc_get)(handle) : nullptr;
}
uintptr_t gc_new(void *obj) {
  return obj ? reinterpret_cast<GcNew>(g_cfg.gc_new)(obj, 0) : 0;
}
void gc_free(uintptr_t handle) {
  if (handle) reinterpret_cast<GcFree>(g_cfg.gc_free)(handle);
}

bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }
bool is_summary_or_details(void *obj) {
  return is_exact(obj, g_cfg.class_summary) || is_exact(obj, g_cfg.class_details);
}

int32_t field_i32(void *obj, void *field) {
  int32_t v = 0;
  if (obj != nullptr) field_get()(obj, field, &v);
  return v;
}

// A native-owned temporary GC root (duplicate of a borrowed handle or a resolved object).
struct Root {
  uintptr_t handle = 0;
  Root() = default;
  Root(const Root &) = delete;
  Root &operator=(const Root &) = delete;
  ~Root() { gc_free(handle); }
  void *get() const { return gc_get(handle); }
  bool set(void *object) {
    if (!object) return false;
    handle = gc_new(object);
    return handle && get();
  }
  bool duplicate(void *borrowed) {
    return borrowed && set(gc_get(reinterpret_cast<uintptr_t>(borrowed)));
  }
  uintptr_t release() {
    uintptr_t h = handle;
    handle = 0;
    return h;
  }
};

// Declared before any Root so reentrant root cleanup still observes busy=true until unwound.
struct BusyExit {
  ~BusyExit() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_busy = false;
  }
};

// alive(gui): static op_Implicit returns a boxed System.Boolean; require the exact BOOL class and an
// unboxed byte of exactly 0 or 1. Any throw / null / wrong class / out-of-range byte fails closed. The
// reference argument is the object pointer itself. Root the box before reading.
int alive_state(const Root &gui) {
  void *object = gui.get();
  if (!object) return ALIVE_ERR;
  void *params[1] = {object};  // IL2CPP reference argument is the object pointer itself.
  void *exc = nullptr;
  void *boxed = managed_invoke(g_cfg.m_alive, nullptr, params, &exc);
  Root value;
  if (exc != nullptr || !value.set(boxed)) return ALIVE_ERR;
  if (!is_exact(value.get(), g_cfg.class_bool)) return ALIVE_ERR;
  void *raw = unbox()(value.get());
  if (raw == nullptr) return ALIVE_ERR;
  uint8_t v = *static_cast<uint8_t *>(raw);
  if (v > 1) return ALIVE_ERR;
  return v == 1 ? ALIVE_TRUE : ALIVE_FALSE;
}

// set_CanvasGroupVisible(gui, visible) through the IL2CPP invoker; the bool is passed BY ADDRESS. The
// hook fires reentrantly on this same owner thread (never masking a false, and -- for restore -- only
// after the slot has already been removed). Returns false iff the managed call threw.
bool set_canvas(void *gui, bool visible) {
  uint8_t value = visible ? 1 : 0;
  void *params[1] = {&value};  // value-type Boolean passed BY ADDRESS
  void *exc = nullptr;
  managed_invoke(g_cfg.m_canvas, gui, params, &exc);
  return exc == nullptr;
}

// --- owner latching ----------------------------------------------------------------
// Latches g_owner_tid on the first genuine on-thread call. Only API entry points call this; the hook
// never does.
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

// --- owner-thread-only slot helpers (no lock; owner thread serialises them) --------
int find_slot_by_object(void *obj) {
  if (obj == nullptr) return -1;
  for (int i = 0; i < PGR_CANVAS_MAX_SLOTS; ++i) {
    if (g_slots[i].handle != 0 && gc_get(g_slots[i].handle) == obj) return i;
  }
  return -1;
}
int find_slot_by_token(uint64_t token) {
  if (token == 0) return -1;
  for (int i = 0; i < PGR_CANVAS_MAX_SLOTS; ++i) {
    if (g_slots[i].handle != 0 && g_slots[i].token == token) return i;
  }
  return -1;
}
int find_free_slot() {
  for (int i = 0; i < PGR_CANVAS_MAX_SLOTS; ++i) {
    if (g_slots[i].handle == 0) return i;
  }
  return -1;
}
void refresh_owned() {
  uint32_t n = 0;
  for (int i = 0; i < PGR_CANVAS_MAX_SLOTS; ++i) {
    if (g_slots[i].handle != 0) ++n;
  }
  g_owned.store(n, std::memory_order_relaxed);
}

// Detach a slot into a local root WITHOUT restoring. The caller owns the returned handle afterwards and
// the hook can no longer observe (or re-hide) this GUI. Owner thread only.
uintptr_t detach_slot(int idx) {
  uintptr_t h = g_slots[idx].handle;
  g_slots[idx] = Slot{};
  refresh_owned();
  return h;
}

// Restore a detached GUI root to visible, ONLY when it is still exact DGUI + Unity alive + STATE_OPEN,
// then free the root. An unknown class / failed root / alive error never assumes the GUI is gone; it is
// just freed safely. The slot MUST already be detached so the hook cannot re-hide during the set-true.
void restore_and_free(uintptr_t handle) {
  Root root;
  root.handle = handle;
  void *obj = root.get();
  if (is_exact(obj, g_cfg.class_details) && alive_state(root) == ALIVE_TRUE) {
    obj = root.get();  // re-resolve after the managed alive probe
    if (is_exact(obj, g_cfg.class_details) && field_i32(obj, g_cfg.f_current_state) == g_cfg.state_open) {
      if (set_canvas(obj, true)) g_restore_count.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // root's destructor frees the handle.
}

// --- the set_CanvasGroupVisible hook ----------------------------------------------
// Chains the original exactly once with the same MethodInfo. Masks a true->false ONLY for an owned
// opening-details root, on the current actual owner thread, under a live gate with the slot's matching
// epoch. Everything else passes through. NEVER invokes a managed method and NEVER latches the owner.
CanvasSetterFn original_canvas() {
  CanvasSetterFn fn = g_orig_canvas.load(std::memory_order_acquire);
  if (!fn) {
    std::lock_guard<std::mutex> lock(g_install_mutex);
    fn = g_orig_canvas.load(std::memory_order_acquire);
  }
  return fn;
}

bool hook_should_mask(void *self) {
  // Only the latched owner thread may inspect the owner-only slots.
  const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (owner == 0 || current_tid() != owner) return false;
  const int idx = find_slot_by_object(self);
  if (idx < 0) return false;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return false;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  return gate_live_locked(g_slots[idx].epoch, owner, now);
}

void hook_set_canvas(void *self, bool visible, void *method_info) {
  CanvasSetterFn orig = original_canvas();
  struct Frame {
    bool active=false;
    ~Frame(){if(active)g_observer_frames.fetch_sub(1);}
  } frame;
  Root observer_root;
  // Immutable after release publication. Predicate cannot allocate or move self.
  if(self&&g_observer_ready.load(std::memory_order_acquire)&&
     g_observer.filter(g_observer.context,self,current_tid())==1){
    g_observer_frames.fetch_add(1);frame.active=true;
    if(observer_root.set(self)){
      try {g_observer.on_enter(g_observer.context,observer_root.get(),current_tid());}
      catch (...) {g_fault.store(1,std::memory_order_relaxed);}
      self=observer_root.get();
    }
  }
  bool masked = visible;
  if (visible && self != nullptr && hook_should_mask(self)) {
    masked = false;
    g_masked_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (orig != nullptr) orig(self, masked, method_info);
}

// --- install -----------------------------------------------------------------------
bool validate_config(const PgrCatchCanvasConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_CANVAS_ERR_NULL_ARG; return false; }
  if (c->version != PGR_CANVAS_CONFIG_VERSION) { *rc = PGR_CANVAS_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrCatchCanvasConfig)) { *rc = PGR_CANVAS_ERR_BAD_SIZE; return false; }
  void *ptrs[kConfigPtrCount];
  std::memcpy(ptrs,reinterpret_cast<const unsigned char*>(c)+offsetof(PgrCatchCanvasConfig,object_get_class),sizeof(ptrs));
  for (int i = 0; i < kConfigPtrCount; ++i) {
    if (ptrs[i] == nullptr) { *rc = PGR_CANVAS_ERR_MISSING_PTR; return false; }
  }
  if (c->reserved0!=0) { *rc=PGR_CANVAS_ERR_BAD_SIZE; return false; }
  *rc = PGR_CANVAS_OK;
  return true;
}

int api_install(const PgrCatchCanvasConfig *config) {
  int rc = PGR_CANVAS_OK;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) { g_fault.store(1, std::memory_order_relaxed); return PGR_CANVAS_ERR_CLOCK_FAILURE; }

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_cfg, config, sizeof(*config)) != 0) return PGR_CANVAS_ERR_RECONFIG;
    return PGR_CANVAS_OK;
  }
  if (g_install_attempted) return g_install_error;  // never re-hook after a partial failure
  g_install_attempted = true;
  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;  // publish the immutable config before the hook can fire
    g_enabled = 0;
    g_lease_until_ms = 0;
    g_epoch = 0;
    g_token_serial = 0;
    g_busy = false;
  }

  // Patch the single setter. The original is published (under g_install_mutex) before the bound address
  // is live; original_canvas() blocks on this mutex if it observes a null during that window.
  void *orig = nullptr;
  const int hook_rc = pgrazor_hook(g_cfg.canvas_address, reinterpret_cast<void *>(&hook_set_canvas), &orig);
  if (hook_rc != 0 || orig == nullptr) {
    if (hook_rc == 0) pgrazor_unhook(g_cfg.canvas_address);
    g_fault.store(1, std::memory_order_relaxed);
    return g_install_error = PGR_CANVAS_ERR_HOOK_FAILED;
  }
  g_orig_canvas.store(reinterpret_cast<CanvasSetterFn>(orig), std::memory_order_release);

  g_installed.store(true, std::memory_order_release);
  return g_install_error = PGR_CANVAS_OK;
}

int api_update(int enabled, uint64_t remaining, uint64_t epoch, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_CANVAS_ERR_NOT_INSTALLED;
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
    if (!clock_ok) g_fault.store(1, std::memory_order_relaxed);
    return clock_ok ? PGR_CANVAS_OK : PGR_CANVAS_ERR_CLOCK_FAILURE;
  }
  remaining = remaining > kMaxLeaseMs ? kMaxLeaseMs : remaining;
  if (!g_enabled || g_epoch != epoch || !remaining || !g_lease_until_ms || now >= g_lease_until_ms) ++g_gen;
  g_enabled = 1;
  g_lease_until_ms = remaining ? now + remaining : 0;
  g_epoch = epoch;
  return PGR_CANVAS_OK;
}

int api_hide_opening(void *gui, uint64_t epoch, uint64_t tid, uint64_t *out_token) {
  if (out_token != nullptr) *out_token = 0;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_CANVAS_ERR_NOT_INSTALLED;
  if (gui == nullptr || out_token == nullptr) return PGR_CANVAS_ERR_NULL_ARG;
  const uint64_t cur = current_tid();
  if (tid == 0 || cur != tid || !owner_ready(tid)) return PGR_CANVAS_OK;  // off-owner: refused, no free
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) { g_fault.store(1, std::memory_order_relaxed); return PGR_CANVAS_ERR_CLOCK_FAILURE; }

  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_busy || !gate_live_locked(epoch, tid, now)) return PGR_CANVAS_OK;  // reentrant / gate: refused
    g_busy = true;
    generation = g_gen;
  }
  BusyExit busy_exit;

  // Duplicate the borrowed GUI into an own root and resolve the object.
  Root g;
  if (!g.duplicate(gui)) return PGR_CANVAS_OK;  // root failure: refused
  void *obj = g.get();

  if (!is_exact(obj,g_cfg.class_details)) return PGR_CANVAS_OK;
  // Reuse: a slot already owning this GUI at the current epoch keeps its token with no redundant set.
  int existing = find_slot_by_object(obj);
  if (existing >= 0) {
    if (g_slots[existing].epoch == epoch) {
      if (alive_state(g)!=ALIVE_TRUE || !is_exact(g.get(),g_cfg.class_details)) return PGR_CANVAS_OK;
      uint64_t current=0;
      std::lock_guard<std::mutex> lock(g_state_mutex);
      if (!read_boottime_ms(&current) || g_gen!=generation || !gate_live_locked(epoch,tid,current)) return PGR_CANVAS_OK;
      *out_token = g_slots[existing].token;
      return PGR_CANVAS_HIDDEN;
    }
    // A stale same-GUI root (older epoch) is our own root on the owner thread: restore+free it and re-hide.
    restore_and_free(detach_slot(existing));
    obj=g.get(); // restoration can relocate the GUI and all temporary roots
  }

  // Identity: exact DGUI + Unity alive. No STATE_OPEN gate (this may run while the canvas is opening).
  if (!is_exact(obj, g_cfg.class_details)) return PGR_CANVAS_OK;
  if (alive_state(g) != ALIVE_TRUE) return PGR_CANVAS_OK;
  obj = g.get();  // re-resolve after the managed alive probe
  if (!is_exact(obj, g_cfg.class_details)) return PGR_CANVAS_OK;

  // Final gate re-check: a racing stop()/epoch change (generation move) or an expired lease refuses.
  {
    uint64_t now2 = 0;
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!read_boottime_ms(&now2)) { g_fault.store(1, std::memory_order_relaxed); return PGR_CANVAS_ERR_CLOCK_FAILURE; }
    if (g_gen != generation || !gate_live_locked(epoch, tid, now2)) return PGR_CANVAS_OK;
    if (g_token_serial >= kTokenCeiling) return PGR_CANVAS_OK;  // fail closed near the ceiling
  }

  const int idx = find_free_slot();
  if (idx < 0) return PGR_CANVAS_OK;  // slot overflow: refuse, leave the canvas visible

  // Insert the owned slot BEFORE the managed set-false so the hook keeps the canvas hidden.
  uint64_t token;
  { std::lock_guard<std::mutex> lock(g_state_mutex); token = ++g_token_serial; }
  g_slots[idx].handle = g.release();
  g_slots[idx].epoch = epoch;
  g_slots[idx].token = token;
  refresh_owned();

  void *slot_obj = gc_get(g_slots[idx].handle);
  if (!set_canvas(slot_obj, false)) {
    // Managed throw: remove the slot first (so the hook cannot re-hide), best-effort restore, free.
    restore_and_free(detach_slot(idx));
    return PGR_CANVAS_OK;  // *out_token stays 0; the host leaves the canvas visible
  }
  g_hide_count.fetch_add(1, std::memory_order_relaxed);
  *out_token = token;
  return PGR_CANVAS_HIDDEN;
}

int api_hide_dismissed(void *gui, uint64_t epoch, uint64_t tid, int32_t expected_after) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_CANVAS_ERR_NOT_INSTALLED;
  if (gui == nullptr) return PGR_CANVAS_ERR_NULL_ARG;
  const uint64_t cur = current_tid();
  if (tid == 0 || cur != tid || !owner_ready(tid)) return PGR_CANVAS_OK;  // off-owner: refused
  if (expected_after == g_cfg.state_open) return PGR_CANVAS_OK;  // only hide once dismissal left STATE_OPEN
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) { g_fault.store(1, std::memory_order_relaxed); return PGR_CANVAS_ERR_CLOCK_FAILURE; }

  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_busy || !gate_live_locked(epoch, tid, now)) return PGR_CANVAS_OK;
    g_busy = true;
    generation = g_gen;
  }
  BusyExit busy_exit;

  // One-shot: own a temporary root for the call only; never a persistent slot, never close().
  Root g;
  if (!g.duplicate(gui)) return PGR_CANVAS_OK;
  void *obj = g.get();
  if (!is_summary_or_details(obj)) return PGR_CANVAS_OK;
  if (alive_state(g) != ALIVE_TRUE) return PGR_CANVAS_OK;
  obj = g.get();  // re-resolve after the alive probe
  if (!is_summary_or_details(obj)) return PGR_CANVAS_OK;
  // Re-read currentState after the alive probe: it must equal the host's proven non-OPEN postcondition.
  if (field_i32(obj, g_cfg.f_current_state) != expected_after) return PGR_CANVAS_OK;

  {
    uint64_t now2 = 0;
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!read_boottime_ms(&now2)) { g_fault.store(1, std::memory_order_relaxed); return PGR_CANVAS_ERR_CLOCK_FAILURE; }
    if (g_gen != generation || !gate_live_locked(epoch, tid, now2)) return PGR_CANVAS_OK;
  }

  if (!set_canvas(g.get(), false)) return PGR_CANVAS_OK;  // managed throw: refused
  g_hide_count.fetch_add(1, std::memory_order_relaxed);
  return PGR_CANVAS_HIDDEN;
}

int api_release(uint64_t token, uint64_t tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_CANVAS_ERR_NOT_INSTALLED;
  const uint64_t cur = current_tid();
  if (tid == 0 || cur != tid || !owner_ready(tid)) return PGR_CANVAS_ERR_OWNER;  // caller must retain its token
  if (token == 0) return PGR_CANVAS_OK;  // idempotent
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_busy) return PGR_CANVAS_ERR_BUSY;  // caller must retry cleanup after the live frame
    g_busy = true;
  }
  BusyExit busy_exit;

  const int idx = find_slot_by_token(token);  // ignores enable/lease; only the token matters
  if (idx < 0) return PGR_CANVAS_OK;  // unknown token: idempotent
  // Remove the slot BEFORE restoring so the hook cannot re-hide; always free the own root.
  restore_and_free(detach_slot(idx));
  return PGR_CANVAS_OK;
}

int api_tick(uint64_t tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_CANVAS_ERR_NOT_INSTALLED;
  const uint64_t cur = current_tid();
  if (tid == 0 || cur != tid || !owner_ready(tid)) return PGR_CANVAS_OK;  // foreign thread: no free
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_busy) return PGR_CANVAS_OK;  // bounded, non-reentrant
    g_busy = true;
  }
  BusyExit busy_exit;

  // Snapshot the gate: a slot survives only while enabled + lease-live + its epoch still armed. Runs even
  // after stop() (enabled == 0 -> everything pruned); never holds the lock across a managed restore.
  uint64_t armed_epoch;
  bool permitted;
  {
    uint64_t now = 0;
    const bool clock_ok = read_boottime_ms(&now);
    std::lock_guard<std::mutex> lock(g_state_mutex);
    armed_epoch = g_epoch;
    permitted = clock_ok && g_enabled && g_lease_until_ms != 0 && now < g_lease_until_ms;
  }
  for (int i = 0; i < PGR_CANVAS_MAX_SLOTS; ++i) {
    if (g_slots[i].handle == 0) continue;
    if (!permitted || g_slots[i].epoch != armed_epoch) {
      restore_and_free(detach_slot(i));
    }
  }
  return PGR_CANVAS_OK;
}

int api_stop() {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_CANVAS_ERR_NOT_INSTALLED;
  // Immediate revoke from any thread. Slots are freed/restored only on the owner thread via tick()/
  // release(); the original stays patched so the hook keeps passing through safely.
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_enabled = 0;
  g_lease_until_ms = 0;
  ++g_gen;
  return PGR_CANVAS_OK;
}

int api_sample(PgrCatchCanvasSample *out) {
  if (out == nullptr) return PGR_CANVAS_ERR_NULL_ARG;
  out->version = PGR_CANVAS_SAMPLE_VERSION;
  out->size = sizeof(PgrCatchCanvasSample);
  out->owned = g_owned.load(std::memory_order_relaxed)+g_observer_frames.load(std::memory_order_relaxed);
  out->fault = g_fault.load(std::memory_order_relaxed);
  out->masked_count = g_masked_count.load(std::memory_order_relaxed);
  out->hide_count = g_hide_count.load(std::memory_order_relaxed);
  out->restore_count = g_restore_count.load(std::memory_order_relaxed);
  return PGR_CANVAS_OK;
}

const PgrCatchCanvasApi kApiTable = {
    PGR_CANVAS_API_VERSION, static_cast<uint32_t>(sizeof(PgrCatchCanvasApi)),
    &api_install,  &api_update, &api_hide_opening, &api_hide_dismissed,
    &api_release,  &api_tick,   &api_stop,         &api_sample,
};

int api_subscribe(const PgrCatchCanvasObserver *observer){
 if(!observer)return PGR_CANVAS_ERR_NULL_ARG;
 if(observer->version!=1)return PGR_CANVAS_ERR_BAD_VERSION;
 if(observer->size!=sizeof(*observer))return PGR_CANVAS_ERR_BAD_SIZE;
 if(!observer->filter||!observer->on_enter)return PGR_CANVAS_ERR_MISSING_PTR;
 if(!g_installed.load(std::memory_order_acquire))return PGR_CANVAS_ERR_NOT_INSTALLED;
 std::lock_guard<std::mutex> lock(g_observer_mutex);
 if(g_observer_ready.load())return g_observer.filter==observer->filter&&g_observer.on_enter==observer->on_enter&&g_observer.context==observer->context?PGR_CANVAS_OK:PGR_CANVAS_ERR_RECONFIG;
 g_observer=*observer;g_observer_ready.store(true,std::memory_order_release);return PGR_CANVAS_OK;
}
const PgrCatchCanvasApi2 kApi2{2,sizeof(PgrCatchCanvasApi2),api_subscribe};

}  // namespace

extern "C" const void *pgrazor_catch_canvas_api(uint32_t version) {
  if(version==2)return &kApi2;
  if (version != PGR_CANVAS_API_VERSION) return nullptr;
  return &kApiTable;
}

// PGRazor native migration: raid / gym / Dynamax attack & dodge controller (feature id 19).
// See battles.h for the ABI, the "installs no hooks" rationale (the RVCHARGE ObserveBattleState
// entry is shared with the still-Gum Rocket controller) and the observe()/gym_begin()/gym_end()
// forwarding contract. Built -fno-exceptions / -fno-rtti like every sibling: a managed throw
// becomes a fail-closed `failed` latch + a battle-error event, and RAII destructors free each
// transient GC root exactly once. No mutex is ever held across a managed call or a GC-handle call.
//
// Lifetime discipline carried over from features 17/18:
//   * Every object used across a managed call is pinned in a transient RAII Root FIRST and
//     re-resolved from its handle after each runtime_invoke -- no raw scope/gesture/button pointer
//     is held across another managed call (a GC move would dangle it).
//   * Every enter/exit/clear bumps a scope epoch. After each managed call the tick re-checks the
//     epoch (and the stop flag); a reentrant exit / new scope cancels the stale action WITHOUT
//     clearing the newer state, and a nested stop defers the root free to the outermost frame.
//   * A final clock / lease / epoch / rocket re-check precedes EVERY attack/dodge mutation.
//   * Free functions detach (null the member) BEFORE gc_free, so a reentrant free double-frees
//     nothing. owned_roots (durable handles + active frames) stays nonzero while any frame runs.

#include "battles.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

static_assert(sizeof(pgr_battle_scope_desc) == 16, "scope desc must be 16 bytes");
static_assert(sizeof(pgr_battle_swap_desc) == 24, "swap desc must be 24 bytes");
static_assert(sizeof(pgr_battle_config_v1) == 512, "config must be 512 bytes");
static_assert(sizeof(pgr_battle_sample_v1) == 64, "sample must be 64 bytes");
static_assert(sizeof(pgr_battle_event_v1) == 16, "event must be 16 bytes");
static_assert(sizeof(pgr_battle_api_v1) == 72, "table must be 72 bytes");
static_assert(offsetof(pgr_battle_config_v1, gchandle_new) == 8, "layout");
static_assert(offsetof(pgr_battle_config_v1, m_current_state) == 64, "layout");
static_assert(offsetof(pgr_battle_config_v1, m_vnext_visible) == 176, "layout");
static_assert(offsetof(pgr_battle_config_v1, f_director) == 184, "layout");
static_assert(offsetof(pgr_battle_config_v1, f_gym_warning_state) == 272, "layout");
static_assert(offsetof(pgr_battle_config_v1, class_battlestate) == 280, "layout");
static_assert(offsetof(pgr_battle_config_v1, class_bool) == 360, "layout");
static_assert(offsetof(pgr_battle_config_v1, scopes) == 368, "layout");
static_assert(offsetof(pgr_battle_config_v1, swaps) == 416, "layout");
static_assert(offsetof(pgr_battle_config_v1, status_active) == 488, "layout");
static_assert(offsetof(pgr_battle_config_v1, reserved1) == 508, "layout");
static_assert(offsetof(pgr_battle_sample_v1, owned_roots) == 56, "sample owned_roots @56");

namespace {

constexpr uint64_t kMaxLeaseMs  = PGR_BATTLE_MAX_LEASE_MS;
constexpr uint32_t kMaxList     = PGR_BATTLE_MAX_LIST;
constexpr uint64_t kQuickMs     = PGR_BATTLE_QUICK_MS;
constexpr uint64_t kChargedMs   = PGR_BATTLE_CHARGED_MS;
constexpr uint64_t kMaxMs       = PGR_BATTLE_MAX_MS;
constexpr uint64_t kDodgeCueMs  = PGR_BATTLE_DODGE_CUE_MS;
constexpr uint64_t kDodgeNextMs = PGR_BATTLE_DODGE_NEXT_MS;
constexpr uint64_t kDodgeGapMs  = PGR_BATTLE_DODGE_GAP_MS;
constexpr uint64_t kSwapGraceMs = PGR_BATTLE_SWAP_GRACE_MS;
constexpr uint64_t kQuickLogMs  = PGR_BATTLE_QUICK_LOG_MS;

// --- lifetime / publication ---------------------------------------------------------
std::mutex           g_install_mutex;
pgr_battle_config_v1 g_cfg{};
std::atomic<bool>    g_installed{false};
bool                 g_install_attempted = false;
int                  g_install_error = PGR_BATTLE_ERR_NOT_INSTALLED;

// --- thread identity ----------------------------------------------------------------
std::atomic<uint64_t> g_unity_tid{0};  // permanent Unity identity, latched once from tick

// --- per-tick policy gate (any thread writes via update; owner thread reads) --------
std::mutex g_policy_mutex;
std::atomic<uint64_t> g_policy_generation{1};
uint64_t g_tick_policy = 0;
uint32_t g_tick_mask = 0;
std::atomic<uint32_t> g_mask{0};            // battleMask (exact source bits)
std::atomic<uint64_t> g_lease_until_ms{0};  // absolute CLOCK_BOOTTIME lease deadline
std::atomic<uint32_t> g_rocket_active{0};   // Rocket mutual-exclusion gate

// --- telemetry (atomic; pull-only; any thread) --------------------------------------
std::atomic<uint32_t> g_scope_active{0};
std::atomic<int32_t>  g_mode_pub{-1};
std::atomic<uint32_t> g_failed_pub{0};
std::atomic<uint32_t> g_swap_blocked_pub{0};
std::atomic<uint32_t> g_charges_pub{0};
std::atomic<uint32_t> g_maxmoves_pub{0};
std::atomic<uint64_t> g_action_serial{0};
std::atomic<uint64_t> g_scope_serial{0};
std::atomic<uint32_t> g_fault_serial{0};
std::atomic<uint32_t> g_dropped_events{0};
std::atomic<uint32_t> g_durable_roots{0};
std::atomic<uint32_t> g_active_frames{0};

// Fail-closed telemetry latch: set when the bounded FIFO drops an event; while set it BLOCKS every
// new attack/dodge mutation (final_ok() consults it). Cleared once the FIFO drains empty again.
std::atomic<bool> g_actions_locked{false};

// Stop request: set (mask disarmed) immediately on any tick(stopping != 0), even a nested one;
// drained on the OUTERMOST tick frame. NOT terminal -- a later tick(0) resumes normal operation.
std::atomic<bool> g_stop_requested{false};

// --- bounded telemetry FIFO (owner thread writes + drains) --------------------------
std::mutex          g_fifo_mutex;
pgr_battle_event_v1 g_fifo[PGR_BATTLE_EVENT_CAP]{};
uint32_t            g_fifo_head = 0, g_fifo_tail = 0, g_fifo_size = 0;

// --- owner-thread-only battle state (mirrors battleState in battles.js) -------------
uint64_t g_epoch = 0;         // bumped on every clear/enter/exit; cancels straddling actions
bool     g_in_tick = false;   // non-reentrant tick body guard (observe/gym are NOT gated)
void    *g_scope_handle = nullptr;
int32_t  g_mode = -1;
void    *g_gesture_handle = nullptr;
bool     g_legacy = false;
void    *g_swap_handle = nullptr;
void    *g_swap_show = nullptr;  // MethodInfo* from the swap descriptor at open
uint64_t g_swap_until_ms = 0;
void    *g_charges[PGR_BATTLE_MAX_LIST] = {nullptr};
uint32_t g_charge_count = 0;
void    *g_max_moves[PGR_BATTLE_MAX_LIST] = {nullptr};
uint32_t g_max_count = 0;
uint64_t g_next_ms = 0;
uint64_t g_dodge_until_ms = 0;
uint64_t g_dodge_next_ms = 0;
bool     g_failed = false;
uint64_t g_last_log_ms = 0;

// Single-slot gym-warning token (MoveNext reentrancy overwrites; outer tokens are rejected at end).
uint64_t g_gym_token = 0;
uint64_t g_gym_token_src = 0;
uint64_t g_gym_epoch = 0;

// --- clock / tid --------------------------------------------------------------------
uint64_t boottime_ms() {
  struct timespec ts;
#if defined(CLOCK_BOOTTIME)
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return 0;
#elif defined(CLOCK_MONOTONIC)
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
#else
  if (timespec_get(&ts, TIME_UTC) == 0) return 0;
#endif
  if (ts.tv_sec < 0 || ts.tv_nsec < 0) return 0;
  return static_cast<uint64_t>(ts.tv_sec) * 1000ull + static_cast<uint64_t>(ts.tv_nsec) / 1000000ull;
}
inline uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

// A managed entry point is on the owner thread IFF the owner has been latched AND the caller's
// self-reported proven_tid equals BOTH the latched owner AND the live OS tid.
inline bool on_unity(uint64_t proven_tid) {
  const uint64_t owner = g_unity_tid.load(std::memory_order_acquire);
  return owner != 0 && proven_tid == owner && current_tid() == proven_tid;
}

// --- il2cpp primitives over the validated config ------------------------------------
inline void *gc_new(void *o)    { return o ? reinterpret_cast<void *(*)(void *, int)>(g_cfg.gchandle_new)(o, 0) : nullptr; }
inline void *gc_target(void *h) { return h ? reinterpret_cast<void *(*)(void *)>(g_cfg.gchandle_get_target)(h) : nullptr; }
inline void  gc_free(void *h)   { if (h) reinterpret_cast<void (*)(void *)>(g_cfg.gchandle_free)(h); }
inline void *obj_class(void *o) { return o ? reinterpret_cast<void *(*)(void *)>(g_cfg.object_get_class)(o) : nullptr; }
inline void *obj_unbox(void *o) { return o ? reinterpret_cast<void *(*)(void *)>(g_cfg.object_unbox)(o) : nullptr; }
inline bool  exact(void *o, void *k) { return o != nullptr && k != nullptr && obj_class(o) == k; }
inline void  field_get(void *o, void *f, void *out) {
  reinterpret_cast<void (*)(void *, void *, void *)>(g_cfg.field_get_value)(o, f, out);
}
inline void   *read_ref(void *o, void *f) { void *v = nullptr; if (o) field_get(o, f, &v); return v; }
inline int32_t read_s32(void *o, void *f) { int32_t v = 0; if (o) field_get(o, f, &v); return v; }
// present(obj, field, 1): the bool FastAttackInputEnabled field read == 1 (a value > 1 is false).
inline bool    field_present(void *o, void *f) { uint8_t v = 0; if (o) field_get(o, f, &v); return v == 1; }

// Invoke a managed method; on a managed exception set *err and return null.
inline void *invoke(void *method, void *obj, void **params, bool *err) {
  void *exc = nullptr;
  void *r = reinterpret_cast<void *(*)(void *, void *, void **, void **)>(g_cfg.runtime_invoke)(
      method, obj, params, &exc);
  if (exc != nullptr) { *err = true; return nullptr; }
  return r;
}

// Boxed bool read: root the box across unbox + exact-class check, a value > 1 is a fault.
bool invoke_bool(void *method, void *obj, void **params, bool *err) {
  void *box = invoke(method, obj, params, err);
  if (*err) return false;
  void *h = gc_new(box);
  if (box != nullptr && h == nullptr) { *err = true; return false; }
  void *b = gc_target(h);
  bool ok = exact(b, g_cfg.class_bool);
  uint8_t v = 0;
  if (ok) { void *raw = obj_unbox(gc_target(h)); if (raw) v = *reinterpret_cast<uint8_t *>(raw); else ok = false; }
  gc_free(h);
  if (!ok || v > 1) { *err = true; return false; }
  return v != 0;
}
// get_isActiveAndEnabled on an instance.
inline bool m_is_active(void *obj, bool *err) { return invoke_bool(g_cfg.m_active, obj, nullptr, err); }
// UnityEngine.Object op_Implicit(obj) -> bool (static).
inline bool m_is_alive(void *obj, bool *err) { void *ap[1] = {obj}; return invoke_bool(g_cfg.m_alive, nullptr, ap, err); }

// --- RAII transient root ------------------------------------------------------------
struct Root {
  void *h = nullptr;
  Root() = default;
  Root(const Root &) = delete;
  Root &operator=(const Root &) = delete;
  void acquire(void *obj, bool *err) {
    reset();
    if (obj == nullptr) { *err = true; return; }
    h = gc_new(obj);
    if (h == nullptr) *err = true;
  }
  void *target() const { return h ? gc_target(h) : nullptr; }
  void *release() { void *x=h; h=nullptr; return x; }
  void reset() { void *x = h; h = nullptr; if (x) gc_free(x); }
  ~Root() { reset(); }
};

// --- frame guard --------------------------------------------------------------------
void drain_stop();
struct FrameGuard {
  FrameGuard() { g_active_frames.fetch_add(1, std::memory_order_relaxed); }
  ~FrameGuard() { if (g_active_frames.load() == 1) drain_stop(); g_active_frames.fetch_sub(1, std::memory_order_relaxed); }
};

// --- durable-root accounting --------------------------------------------------------
void refresh_root_count() {
  uint32_t n = 0;
  if (g_scope_handle) ++n;
  if (g_gesture_handle) ++n;
  if (g_swap_handle) ++n;
  n += g_charge_count + g_max_count;
  g_durable_roots.store(n, std::memory_order_release);
  g_scope_active.store(g_scope_handle ? 1u : 0u, std::memory_order_release);
  g_mode_pub.store(g_mode, std::memory_order_release);
  g_failed_pub.store(g_failed ? 1u : 0u, std::memory_order_release);
  g_charges_pub.store(g_charge_count, std::memory_order_release);
  g_maxmoves_pub.store(g_max_count, std::memory_order_release);
}

// --- bounded FIFO (owner thread) ----------------------------------------------------
void push_event(uint32_t code, int32_t id, int32_t aux) {
  std::lock_guard<std::mutex> lock(g_fifo_mutex);
  if (g_fifo_size >= PGR_BATTLE_EVENT_CAP) {
    g_dropped_events.fetch_add(1, std::memory_order_relaxed);
    g_actions_locked.store(true, std::memory_order_release);  // fail-closed: block new actions
    return;
  }
  g_fifo[g_fifo_tail] = pgr_battle_event_v1{code, id, aux, 0};
  g_fifo_tail = (g_fifo_tail + 1) % PGR_BATTLE_EVENT_CAP;
  ++g_fifo_size;
}
void note_fault(int32_t mode) {
  g_fault_serial.fetch_add(1, std::memory_order_release);
  push_event(PGR_BATTLE_EVT_ERROR, mode, PGR_BATTLE_REASON_INVOKE);
}

// --- list helpers (owner thread) ----------------------------------------------------
// battleCapture: prune dead entries, dedupe by live object identity, append if under the cap.
bool observation_stale(uint64_t epoch) {
  return epoch != g_epoch || g_stop_requested.load();
}
void list_capture(void **list, uint32_t *count, void *obj) {
  const uint64_t epoch=g_epoch; bool err=false;
  Root incoming; incoming.acquire(obj,&err);
  if(err || observation_stale(epoch)) return;
  for (int i=static_cast<int>(*count)-1;i>=0;--i) {
    void *h=list[i]; Root entry; entry.acquire(gc_target(h),&err);
    if(observation_stale(epoch)) return;
    bool alive=!err && m_is_alive(entry.target(),&err);
    if(observation_stale(epoch)) return;
    if(err || !alive) {
      for(uint32_t j=i;j+1<*count;++j) list[j]=list[j+1];
      list[--(*count)]=nullptr; gc_free(h);
      if(observation_stale(epoch)) return;
    }
    err=false;
  }
  for(uint32_t i=0;i<*count;++i) {
    void *candidate=gc_target(list[i]); void *value=incoming.target();
    if(observation_stale(epoch)) return;
    if(candidate==value) return;
  }
  if(*count<kMaxList) list[(*count)++]=incoming.release();
}
void list_remove(void **list, uint32_t *count, void *obj) {
  const uint64_t epoch=g_epoch;
  for(uint32_t i=0;i<*count;++i) {
    void *target=gc_target(list[i]);
    if(observation_stale(epoch)) return;
    if(target==obj) {
      void *h=list[i];
      for(uint32_t j=i;j+1<*count;++j) list[j]=list[j+1];
      list[--(*count)]=nullptr; gc_free(h); return;
    }
  }
}

// --- battle state teardown (owner thread) -------------------------------------------
void drop_gesture() {
  void *g = g_gesture_handle; g_gesture_handle = nullptr;
  g_dodge_until_ms = 0; refresh_root_count(); gc_free(g);
}
// battleClear: drop gesture + swap + scope + both lists, reset timers/latch, bump the epoch so any
// in-flight action re-check (and any outstanding gym token) is invalidated.
void battle_clear() {
  // Detach the complete old state before the first callback-capable free.
  void *old[3 + 2 * kMaxList] = {g_gesture_handle, g_swap_handle, g_scope_handle};
  uint32_t n = 3;
  for (uint32_t i=0;i<g_charge_count;++i) old[n++]=g_charges[i];
  for (uint32_t i=0;i<g_max_count;++i) old[n++]=g_max_moves[i];
  g_gesture_handle=g_swap_handle=g_scope_handle=nullptr;
  g_swap_show=nullptr; g_swap_until_ms=0; g_legacy=false;
  std::memset(g_charges,0,sizeof(g_charges)); std::memset(g_max_moves,0,sizeof(g_max_moves));
  g_charge_count=g_max_count=0;
  g_mode=-1; g_next_ms=g_dodge_until_ms=g_dodge_next_ms=0; g_failed=false;
  g_gym_token=0; g_last_log_ms=0; ++g_epoch; g_swap_blocked_pub.store(0);
  refresh_root_count();
  for(uint32_t i=0;i<n;++i) gc_free(old[i]);
}
void drain_stop() {
  if (!g_stop_requested.load()) return;
  // Keep the latch set while freeing, so callbacks cannot rearm the controller.
  battle_clear();
  g_stop_requested.store(false);
}

// battleDodgeCue: arm a dodge window iff the scope/mode match, within lease, mask bit set, past the
// dodge-next cooldown and no pending dodge. Reads the policy gate live (CLOCK_BOOTTIME).
void dodge_cue(int32_t mode) {
  const uint64_t now = boottime_ms();
  const uint32_t mask = g_mask.load(std::memory_order_acquire);
  const uint64_t lease = g_lease_until_ms.load(std::memory_order_acquire);
  if (mode < 0 || mode > 2 || g_stop_requested.load() || g_actions_locked.load() ||
      g_rocket_active.load() || !g_scope_handle || g_mode != mode || g_failed) return;
  if (lease == 0 || now == 0 || now >= lease) return;
  if ((mask & (128u << mode)) == 0) return;
  if (now < g_dodge_next_ms || g_dodge_until_ms != 0) return;
  g_dodge_until_ms = now + kDodgeCueMs;
  push_event(PGR_BATTLE_EVT_DODGE_CUE, mode, 0);
}

// --- config validation + install ----------------------------------------------------
bool config_ok(const pgr_battle_config_v1 *c) {
  const void *const ptrs[] = {
      c->gchandle_new, c->gchandle_get_target, c->gchandle_free, c->object_get_class, c->object_unbox,
      c->runtime_invoke, c->field_get_value, c->m_current_state, c->m_active, c->m_press,
      c->m_interactable, c->m_alive, c->m_dodge, c->m_legacy_dodge, c->m_raid_fast, c->m_max_fast,
      c->m_legacy_fast, c->m_legacy_charge, c->m_legacy_charged, c->m_vnext_charge, c->m_vnext_charged,
      c->m_vnext_visible, c->f_director, c->f_battle_status, c->f_max_gesture, c->f_legacy_flick,
      c->f_legacy_tap, c->f_legacy_button, c->f_max_button, c->f_max_type, c->f_vnext_enabled,
      c->f_vnext_tap, c->f_vnext_button, c->f_gym_warning_state, c->class_battlestate,
      c->class_legacy_charge, c->class_max_gesture, c->class_raid_gesture, c->class_legacy_gesture,
      c->class_max_widget, c->class_rvcharge, c->class_raid_warning, c->class_max_warning,
      c->class_gym_iterator, c->class_bool};
  for (const void *p : ptrs) if (p == nullptr) return false;
  for (int i = 0; i < PGR_BATTLE_NUM_SCOPES; ++i) {
    if (c->scopes[i].klass == nullptr) return false;
    if (c->scopes[i].mode < 0 || c->scopes[i].mode > 2 || c->scopes[i].reserved != 0) return false;
  }
  for (int i = 0; i < PGR_BATTLE_NUM_SWAPS; ++i) {
    if (c->swaps[i].klass == nullptr || c->swaps[i].show == nullptr) return false;
    if (c->swaps[i].mode < 0 || c->swaps[i].mode > 2 || c->swaps[i].reserved != 0) return false;
  }
  if (c->reserved0 != 0 || c->reserved1 != 0) return false;
  return true;
}

int api_install(const pgr_battle_config_v1 *config) {
  if (config == nullptr) return PGR_BATTLE_ERR_NULL_ARG;
  std::lock_guard<std::mutex> lock(g_install_mutex);
  if (config->version != PGR_BATTLE_API_VERSION ||
      config->size != sizeof(pgr_battle_config_v1) || !config_ok(config))
    return PGR_BATTLE_ERR_BAD_CONFIG;
  if (g_installed.load(std::memory_order_acquire))
    return (std::memcmp(&g_cfg, config, sizeof(g_cfg)) == 0) ? PGR_BATTLE_OK
                                                             : PGR_BATTLE_ERR_ALREADY_BOUND;
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;
  if (boottime_ms() == 0) return g_install_error = PGR_BATTLE_ERR_NO_CLOCK;
  g_cfg = *config;
  g_installed.store(true, std::memory_order_release);
  push_event(PGR_BATTLE_EVT_READY, 0, 1);
  return g_install_error = PGR_BATTLE_OK;
}

int api_update(uint32_t mask, uint64_t residual_lease_ms, uint32_t rocket_active,
               uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_BATTLE_ERR_NOT_INSTALLED;
  // Policy update is allowed off-thread, but once the owner is latched a foreign thread can only
  // disarm (it must not be able to arm the mask/lease it does not own).
  std::lock_guard<std::mutex> lock(g_policy_mutex);
  const uint64_t owner = g_unity_tid.load(std::memory_order_acquire);
  const bool wrong_thread = owner != 0 && proven_tid != 0 && proven_tid != owner;
  if (wrong_thread || proven_tid == 0) {
    ++g_policy_generation;
    g_mask.store(0, std::memory_order_release);
    g_lease_until_ms.store(0, std::memory_order_release);
    g_rocket_active.store(0, std::memory_order_release);
    return wrong_thread ? PGR_BATTLE_ERR_NULL_ARG : PGR_BATTLE_OK;
  }
  const uint64_t clamped = residual_lease_ms > kMaxLeaseMs ? kMaxLeaseMs : residual_lease_ms;
  const uint64_t now = boottime_ms();
  if (mask != g_mask.load() || !!rocket_active != !!g_rocket_active.load() ||
      !clamped || now >= g_lease_until_ms.load()) ++g_policy_generation;
  g_mask.store(mask, std::memory_order_release);
  g_lease_until_ms.store((clamped > 0 && now != 0) ? now + clamped : 0, std::memory_order_release);
  g_rocket_active.store(rocket_active ? 1u : 0u, std::memory_order_release);
  return PGR_BATTLE_OK;
}

// --- observations (owner thread; exact-class identity checks live here) -------------
int api_observe(uint32_t kind, void *obj, int32_t descriptor, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire) || !on_unity(proven_tid)) return PGR_BATTLE_OK;
  if (g_stop_requested.load()) return PGR_BATTLE_OK;
  FrameGuard guard;
  const uint64_t now = boottime_ms();
  const uint64_t observation = ++g_epoch;
  switch (kind) {
    case PGR_BATTLE_OBS_SCOPE_ENTER: {
      bool err=false; Root incoming;
      if(descriptor<0 || descriptor>=PGR_BATTLE_NUM_SCOPES ||
         !exact(obj,g_cfg.scopes[descriptor].klass)) break;
      incoming.acquire(obj,&err);
      if(err || observation_stale(observation)) break;
      battle_clear();
      const uint64_t cleared=observation+1;
      if(observation_stale(cleared)) break;
      g_scope_handle=incoming.release();
      g_mode=g_cfg.scopes[descriptor].mode;
      g_scope_serial.fetch_add(1);
      push_event(PGR_BATTLE_EVT_SCOPE,g_mode,1);
      refresh_root_count();
      break;
    }
    case PGR_BATTLE_OBS_SCOPE_EXIT: {
      void *current=g_scope_handle?gc_target(g_scope_handle):nullptr;
      if (!observation_stale(observation) && current && current == obj) {
        push_event(PGR_BATTLE_EVT_SCOPE, g_mode, 0);
        battle_clear();  // bumps epoch
      }
      break;
    }
    case PGR_BATTLE_OBS_GESTURE: {
      if (!g_scope_handle) break;
      const bool legacy = descriptor != 0;
      if (legacy && g_mode != PGR_BATTLE_MODE_GYM) break;  // legacy gesture only in gym
      void *k = legacy ? g_cfg.class_legacy_gesture
                       : (g_mode == PGR_BATTLE_MODE_MAX ? g_cfg.class_max_gesture
                                                        : g_cfg.class_raid_gesture);
      if (!exact(obj, k)) break;
      bool err=false; Root incoming; incoming.acquire(obj,&err);
      if(err || observation_stale(observation)) break;
      drop_gesture();
      if(observation_stale(observation)) break;
      g_gesture_handle = incoming.release();
      g_legacy = legacy;
      push_event(PGR_BATTLE_EVT_INPUT, g_mode, legacy ? 1 : 0);
      refresh_root_count();
      break;
    }
    case PGR_BATTLE_OBS_GESTURE_DROP: {
      void *current=g_gesture_handle?gc_target(g_gesture_handle):nullptr;
      if (!observation_stale(observation) && current && current == obj) { drop_gesture(); refresh_root_count(); }
      break;
    }
    case PGR_BATTLE_OBS_CHARGE: {
      if (!g_scope_handle) break;
      if (descriptor != 0) {  // legacy Init (9308): gym mode only
        if (g_mode != PGR_BATTLE_MODE_GYM || !exact(obj, g_cfg.class_legacy_charge)) break;
      } else {                // shared RVCHARGE ObserveBattleState
        if (!exact(obj, g_cfg.class_rvcharge)) break;
      }
      list_capture(g_charges, &g_charge_count, obj);
      refresh_root_count();
      break;
    }
    case PGR_BATTLE_OBS_MAX_REFRESH: {
      if (!g_scope_handle || g_mode != PGR_BATTLE_MODE_MAX || !exact(obj, g_cfg.class_max_widget)) break;
      list_capture(g_max_moves, &g_max_count, obj);
      refresh_root_count();
      break;
    }
    case PGR_BATTLE_OBS_MAX_CLEANUP: {
      list_remove(g_max_moves, &g_max_count, obj);
      refresh_root_count();
      break;
    }
    case PGR_BATTLE_OBS_SWAP_OPEN: {
      if (!g_scope_handle || descriptor < 0 || descriptor >= PGR_BATTLE_NUM_SWAPS) break;
      if (g_mode != g_cfg.swaps[descriptor].mode || !exact(obj, g_cfg.swaps[descriptor].klass)) break;
      bool err=false; Root incoming; incoming.acquire(obj,&err);
      if(err || observation_stale(observation)) break;
      void *old = g_swap_handle; g_swap_handle = nullptr; refresh_root_count(); gc_free(old);
      if(observation_stale(observation)) break;
      g_swap_handle = incoming.release();
      g_swap_show = g_cfg.swaps[descriptor].show;
      g_swap_until_ms = now + kSwapGraceMs;
      g_dodge_until_ms = 0;
      g_swap_blocked_pub.store(1, std::memory_order_release);
      push_event(PGR_BATTLE_EVT_SWITCH, g_mode, 1);
      refresh_root_count();
      break;
    }
    case PGR_BATTLE_OBS_SWAP_CLOSE: {
      void *current=g_swap_handle?gc_target(g_swap_handle):nullptr;
      if (!observation_stale(observation) && current && current == obj) {
        void *old = g_swap_handle; g_swap_handle = nullptr; g_swap_show = nullptr; refresh_root_count(); gc_free(old);
        if(observation_stale(observation)) break;
        g_swap_until_ms = now + kSwapGraceMs;
        if (g_next_ms < g_swap_until_ms) g_next_ms = g_swap_until_ms;
        g_dodge_until_ms = 0;
        push_event(PGR_BATTLE_EVT_SWITCH, g_mode, 0);
        refresh_root_count();
      }
      break;
    }
    case PGR_BATTLE_OBS_WARNING: {
      if(descriptor != PGR_BATTLE_MODE_RAID && descriptor != PGR_BATTLE_MODE_MAX) break;
      void *k = (descriptor == PGR_BATTLE_MODE_RAID) ? g_cfg.class_raid_warning : g_cfg.class_max_warning;
      if (exact(obj, k) && !observation_stale(observation)) dodge_cue(descriptor);
      break;
    }
    default: break;
  }
  return PGR_BATTLE_OK;
}

uint64_t api_gym_begin(void *obj, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire) || !on_unity(proven_tid)) return 0;
  FrameGuard guard;
  const uint64_t epoch=g_epoch;
  if(g_stop_requested.load() || g_mode != PGR_BATTLE_MODE_GYM || !exact(obj, g_cfg.class_gym_iterator)) return 0;
  const int32_t state = read_s32(obj, g_cfg.f_gym_warning_state);
  if(observation_stale(epoch) || g_gym_token_src==UINT64_MAX) return 0;
  if (state != 0 && state != 1) return 0;  // eligible iterator state 0 or 1
  uint64_t token = ++g_gym_token_src;
  if (token == 0) token = ++g_gym_token_src;  // never hand back 0
  g_gym_token = token;
  g_gym_epoch = g_epoch;
  return token;
}

int api_gym_end(uint64_t token, int ret_is_zero, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_BATTLE_ERR_NOT_INSTALLED;
  if (!on_unity(proven_tid) || token == 0) return PGR_BATTLE_ERR_BAD_TOKEN;
  FrameGuard guard;
  if (token != g_gym_token) return PGR_BATTLE_ERR_BAD_TOKEN;  // stale / replayed / superseded
  g_gym_token = 0;  // consume once
  if (g_gym_epoch == g_epoch && ret_is_zero) dodge_cue(PGR_BATTLE_MODE_GYM);
  return PGR_BATTLE_OK;
}

// --- tick helpers -------------------------------------------------------------------
// A reentrant exit / new scope (epoch bump) or a nested stop invalidates an in-flight action.
inline bool stale(uint64_t local_epoch) {
  return g_epoch != local_epoch || g_stop_requested.load(std::memory_order_acquire);
}
// Final gate before EVERY attack/dodge mutation: clock, lease, epoch, stop, rocket, fail-closed.
bool final_ok(uint64_t, uint64_t local_epoch) {
  const uint64_t now = boottime_ms();
  std::lock_guard<std::mutex> lock(g_policy_mutex);
  if (g_tick_policy != g_policy_generation.load() || g_tick_mask != g_mask.load()) return false;
  if (stale(local_epoch)) return false;
  if (!g_scope_handle || g_failed) return false;
  if (g_actions_locked.load(std::memory_order_acquire)) return false;
  const uint64_t lease = g_lease_until_ms.load(std::memory_order_acquire);
  if (lease == 0 || now == 0 || now >= lease) return false;
  if (g_rocket_active.load(std::memory_order_acquire)) return false;
  return true;
}

// battleSwapBlocked: re-query the picker's real visibility so an implicit close cannot leave
// automation permanently paused. Returns true while a picker is open or inside its grace window.
bool swap_blocked(uint64_t now, uint64_t local_epoch) {
  if (now < g_swap_until_ms) return true;
  if (!g_swap_handle) return false;
  bool err = false;
  Root o_r; o_r.acquire(gc_target(g_swap_handle), &err);
  if (err) { /* dead picker: fall through to implicit close */ }
  else {
    bool alive = m_is_alive(o_r.target(), &err);
    bool showing = alive && !err ? invoke_bool(g_swap_show, o_r.target(), nullptr, &err) : false;
    if (stale(local_epoch) || g_swap_handle == nullptr) return true;  // reentrant clear/close
    if (!err && alive && showing) { g_dodge_until_ms = 0; return true; }
  }
  if (stale(local_epoch) || !g_swap_handle) return true;
  void *old = g_swap_handle; g_swap_handle = nullptr; g_swap_show = nullptr; refresh_root_count(); gc_free(old);
  if(stale(local_epoch)) return true;
  g_swap_until_ms = now + kSwapGraceMs;
  if (g_next_ms < g_swap_until_ms) g_next_ms = g_swap_until_ms;
  g_dodge_until_ms = 0;
  g_swap_blocked_pub.store(1, std::memory_order_release);
  refresh_root_count();
  return true;
}

// The normal scope-driven decision pass (battleTick lines 25-89).
void run_tick_body(uint64_t now) {
  uint32_t mask_policy;
  { std::lock_guard<std::mutex> lock(g_policy_mutex);
    g_tick_policy = g_policy_generation.load();
    mask_policy = g_tick_mask = g_mask.load(); }
  const uint64_t lease = g_lease_until_ms.load(std::memory_order_acquire);
  const bool lease_ok = lease != 0 && now != 0 && now < lease;
  const bool rocket = g_rocket_active.load(std::memory_order_acquire) != 0;

  // dodge expiry reset (battles.js:27): clear a stale dodge window out of lease / mask / past-due.
  if (!lease_ok || g_mode < 0 || (mask_policy & (128u << g_mode)) == 0 || now > g_dodge_until_ms)
    g_dodge_until_ms = 0;
  // top gate: no scope / failed / no lease / Rocket active -> nothing to do.
  if (!g_scope_handle || g_failed || !lease_ok || rocket) { g_swap_blocked_pub.store(0, std::memory_order_release); return; }

  const uint64_t local_epoch = g_epoch;
  const uint32_t mask = (mask_policy >> (g_mode * 2)) & (g_mode == PGR_BATTLE_MODE_MAX ? 7u : 3u);
  bool err = false;

  // (1) Dynamax scope-gesture recovery (mode 2, no gesture yet): pin the scope, read its gesture,
  // and adopt it if it is the exact Max gesture class and alive+active. Re-check the epoch before
  // mutating so a reentrant exit cannot make us adopt a gesture into a torn-down scope.
  if (g_mode == PGR_BATTLE_MODE_MAX && g_gesture_handle == nullptr) {
    Root scope_r; scope_r.acquire(gc_target(g_scope_handle), &err);
    if (!err) {
      bool alive = m_is_alive(scope_r.target(), &err);
      if (!err && alive) {
        void *g = read_ref(scope_r.target(), g_cfg.f_max_gesture);
        if (g && exact(g, g_cfg.class_max_gesture)) {
          Root g_r; g_r.acquire(g, &err);
          bool g_alive = !err && m_is_alive(g_r.target(), &err);
          bool g_active = !err && g_alive && m_is_active(g_r.target(), &err);
          if (!err && g_active && !stale(local_epoch) && g_mode == PGR_BATTLE_MODE_MAX &&
              g_gesture_handle == nullptr && g_scope_handle != nullptr) {
            g_gesture_handle = g_r.release();
            g_legacy = false;
            push_event(PGR_BATTLE_EVT_INPUT, PGR_BATTLE_MODE_MAX, 0 | 2);  // bit1: scope source
            refresh_root_count();
          }
        }
      }
    }
    if (err) { if(stale(local_epoch)) return; g_failed = true; note_fault(g_mode); refresh_root_count(); return; }
    if (stale(local_epoch)) return;
  }

  // (2) Live Active-status gate (non-legacy only). A battle scope survives the victory animation;
  // require BattleStatus.Active before any action. UI active/enabled alone is insufficient.
  if (!g_legacy) {
    if (g_gesture_handle == nullptr) return;
    Root g_r; g_r.acquire(gc_target(g_gesture_handle), &err);
    if (err) return;
    bool g_active = m_is_alive(g_r.target(), &err) && !err && m_is_active(g_r.target(), &err);
    if (err || !g_active) { if (err) { if(stale(local_epoch)) return; g_failed = true; note_fault(g_mode); refresh_root_count(); } return; }
    void *director = read_ref(g_r.target(), g_cfg.f_director);
    if (director == nullptr) return;
    Root dir_r; dir_r.acquire(director, &err);
    if (err) return;
    void *state = invoke(g_cfg.m_current_state, dir_r.target(), nullptr, &err);
    if (err) { if(stale(local_epoch)) return; g_failed = true; note_fault(g_mode); refresh_root_count(); return; }
    Root state_r; state_r.acquire(state, &err);
    int32_t status = 0;
    bool is_state = !err && exact(state_r.target(), g_cfg.class_battlestate);
    if (is_state) status = read_s32(state_r.target(), g_cfg.f_battle_status);
    state_r.reset();
    if (err || !is_state) return;
    // A reentrant exit / new scope during the getter cancels the stale decision WITHOUT clearing
    // the newer state (do not battle_clear a scope we no longer own).
    if (stale(local_epoch)) return;
    if (status != g_cfg.status_active) {
      g_dodge_until_ms = 0;
      if (status != g_cfg.status_unset) {
        push_event(PGR_BATTLE_EVT_ENDED, g_mode, status);
        battle_clear();
      }
      return;
    }
  }

  // (3) Manual picker grace: all actions yield to a real picker.
  if (swap_blocked(now, local_epoch)) { if(!stale(local_epoch)) {g_dodge_until_ms = 0;g_swap_blocked_pub.store(1);} return; }
  if(stale(local_epoch)) return;
  g_swap_blocked_pub.store(0, std::memory_order_release);

  // (4) Dodge (highest priority once cued).
  if (g_dodge_until_ms != 0 && g_gesture_handle) {
    Root g_r; g_r.acquire(gc_target(g_gesture_handle), &err);
    if (err) return;
    bool g_active = m_is_alive(g_r.target(), &err) && !err && m_is_active(g_r.target(), &err);
    bool enabled = !err && g_active && (g_legacy || field_present(g_r.target(), g_cfg.f_vnext_enabled));
    if (err) { if(stale(local_epoch)) return; g_failed = true; note_fault(g_mode); refresh_root_count(); return; }
    if (g_active && enabled) {
      void *flick = nullptr;
      if (g_legacy) { flick = read_ref(g_r.target(), g_cfg.f_legacy_flick); if (flick == nullptr) return; }
      Root flick_r;
      if(g_legacy) { flick_r.acquire(flick,&err); if(err || stale(local_epoch)) return; }
      void *action_target = g_r.target();
      if(g_legacy) flick=flick_r.target();
      if(stale(local_epoch)) return;
      if (!action_target || (g_legacy && !flick) || !final_ok(now, local_epoch)) return;
      g_dodge_until_ms = 0; g_dodge_next_ms = now + kDodgeNextMs; g_next_ms = now + kDodgeGapMs;
      int32_t dl = g_cfg.dodge_left;
      void *legacy_params[2] = {flick, nullptr};  // (sender=flick, EventArgs=null)
      void *modern_params[1] = {&dl};              // (Direction=Left)
      invoke(g_legacy ? g_cfg.m_legacy_dodge : g_cfg.m_dodge, action_target,
             g_legacy ? legacy_params : modern_params, &err);
      if (err) { if(stale(local_epoch)) return; g_failed = true; note_fault(g_mode); refresh_root_count(); return; }
      g_action_serial.fetch_add(1, std::memory_order_release);
      push_event(PGR_BATTLE_EVT_ACTION, g_mode, PGR_BATTLE_ACT_DODGE);
    }
    return;
  }

  // (5) Action cadence gate.
  if (now < g_next_ms || mask == 0) return;

  // (6) Max attack (mode 2, mask bit 4): press the first BreadAttack move's interactable button.
  if (g_mode == PGR_BATTLE_MODE_MAX && (mask & 4u)) {
    for (uint32_t i = 0; i < g_max_count; ++i) {
      if (i >= g_max_count) break;
      Root o_r; o_r.acquire(gc_target(g_max_moves[i]), &err);
      if (err) { err = false; continue; }
      bool alive = m_is_alive(o_r.target(), &err);
      if (err || !alive) { err = false; continue; }
      if (read_s32(o_r.target(), g_cfg.f_max_type) != g_cfg.max_attack) continue;
      void *b = read_ref(o_r.target(), g_cfg.f_max_button);
      if (b == nullptr) continue;
      Root b_r; b_r.acquire(b, &err);
      if (err) { err = false; continue; }
      bool b_alive = m_is_alive(b_r.target(), &err);
      bool b_active = !err && b_alive && m_is_active(b_r.target(), &err);
      bool b_inter = !err && b_active && invoke_bool(g_cfg.m_interactable, b_r.target(), nullptr, &err);
      if (err) { if(stale(local_epoch)) return; g_failed = true; note_fault(g_mode); refresh_root_count(); return; }
      if (b_alive && b_active && b_inter) {
        void *action_target = b_r.target();
        if (!action_target || !final_ok(now, local_epoch)) return;
        g_next_ms = now + kMaxMs;
        invoke(g_cfg.m_press, action_target, nullptr, &err);
        if (err) { if(stale(local_epoch)) return; g_failed = true; note_fault(g_mode); refresh_root_count(); return; }
        g_action_serial.fetch_add(1, std::memory_order_release);
        push_event(PGR_BATTLE_EVT_ACTION, g_mode, PGR_BATTLE_ACT_MAX);
        return;
      }
    }
  }

  // (7) Charged attack (mask bit 2): first charged+visible candidate with an interactable button.
  if (mask & 2u) {
    for (uint32_t i = 0; i < g_charge_count; ++i) {
      if (i >= g_charge_count) break;
      Root c_r; c_r.acquire(gc_target(g_charges[i]), &err);
      if (err) { err = false; continue; }
      bool alive = m_is_alive(c_r.target(), &err);
      bool active = !err && alive && m_is_active(c_r.target(), &err);
      if (err) { if(stale(local_epoch)) return; g_failed = true; note_fault(g_mode); refresh_root_count(); return; }
      if (!alive || !active) continue;
      const bool legacyC = exact(c_r.target(), g_cfg.class_legacy_charge);
      bool charged = invoke_bool(legacyC ? g_cfg.m_legacy_charged : g_cfg.m_vnext_charged,
                                 c_r.target(), nullptr, &err);
      bool visible = (!err && !legacyC) ? invoke_bool(g_cfg.m_vnext_visible, c_r.target(), nullptr, &err) : true;
      if (err) { if(stale(local_epoch)) return; g_failed = true; note_fault(g_mode); refresh_root_count(); return; }
      if (!charged || (!legacyC && !visible)) continue;
      void *b = read_ref(c_r.target(), legacyC ? g_cfg.f_legacy_button : g_cfg.f_vnext_button);
      if (b == nullptr) continue;
      Root b_r; b_r.acquire(b, &err);
      if (err) { err = false; continue; }
      bool b_alive = m_is_alive(b_r.target(), &err);
      bool b_active = !err && b_alive && m_is_active(b_r.target(), &err);
      bool b_inter = !err && b_active && invoke_bool(g_cfg.m_interactable, b_r.target(), nullptr, &err);
      if (err) { if(stale(local_epoch)) return; g_failed = true; note_fault(g_mode); refresh_root_count(); return; }
      if (!b_alive || !b_active || !b_inter) continue;
      void *action_target = c_r.target();
      if (!action_target || !final_ok(now, local_epoch)) return;
      g_next_ms = now + kChargedMs;
      invoke(legacyC ? g_cfg.m_legacy_charge : g_cfg.m_vnext_charge, action_target, nullptr, &err);
      if (err) { if(stale(local_epoch)) return; g_failed = true; note_fault(g_mode); refresh_root_count(); return; }
      g_action_serial.fetch_add(1, std::memory_order_release);
      push_event(PGR_BATTLE_EVT_ACTION, g_mode, PGR_BATTLE_ACT_CHARGED);
      return;
    }
  }

  // (8) Quick / fast attack (mask bit 1): raid (no args), gym/Max (tap-reference + null EventArgs).
  if ((mask & 1u) == 0 || g_gesture_handle == nullptr) return;
  Root g_r; g_r.acquire(gc_target(g_gesture_handle), &err);
  if (err) return;
  bool g_active = m_is_alive(g_r.target(), &err) && !err && m_is_active(g_r.target(), &err);
  if (err) { if(stale(local_epoch)) return; g_failed = true; note_fault(g_mode); refresh_root_count(); return; }
  if (!g_active) return;
  if (!g_legacy && !field_present(g_r.target(), g_cfg.f_vnext_enabled)) return;
  void *tap = read_ref(g_r.target(), g_legacy ? g_cfg.f_legacy_tap : g_cfg.f_vnext_tap);
  if (tap == nullptr) return;
  Root tap_r;tap_r.acquire(tap,&err);if(err || stale(local_epoch)) return;
  void *action_target = g_r.target();
  tap=tap_r.target();
  if(stale(local_epoch)) return;
  if (!action_target || !tap || !final_ok(now, local_epoch)) return;
  void *method = g_legacy ? g_cfg.m_legacy_fast
                          : (g_mode == PGR_BATTLE_MODE_MAX ? g_cfg.m_max_fast : g_cfg.m_raid_fast);
  void *params[2] = {tap, nullptr};  // (sender=tap, EventArgs=null)
  g_next_ms = now + kQuickMs;
  // raid quick takes no args (params NULL); gym/Max quick take the sender + null EventArgs.
  invoke(method, action_target, g_mode == PGR_BATTLE_MODE_RAID ? nullptr : params, &err);
  if (err) { if(stale(local_epoch)) return; g_failed = true; note_fault(g_mode); refresh_root_count(); return; }
  g_action_serial.fetch_add(1, std::memory_order_release);
  if (!g_last_log_ms || (now >= g_last_log_ms && now - g_last_log_ms > kQuickLogMs)) {  // 5 s de-dupe of the quick telemetry event
    g_last_log_ms = now;
    push_event(PGR_BATTLE_EVT_ACTION, g_mode, PGR_BATTLE_ACT_QUICK);
  }
}

int api_tick(uint64_t proven_tid, int stopping) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_BATTLE_ERR_NOT_INSTALLED;
  if (proven_tid == 0 || current_tid() != proven_tid) return PGR_BATTLE_OK;
  uint64_t expected = 0;
  g_unity_tid.compare_exchange_strong(expected, proven_tid, std::memory_order_acq_rel);
  if (g_unity_tid.load(std::memory_order_acquire) != proven_tid) return PGR_BATTLE_OK;

  // Record a stop + disarm IMMEDIATELY (even nested) so no action can start between now and the
  // outermost drain. A nested tick returns here; the outer frame already running drains the stop.
  if (stopping) {
    g_stop_requested.store(true, std::memory_order_release);
    ++g_policy_generation;
    g_mask.store(0, std::memory_order_release);
    g_lease_until_ms.store(0, std::memory_order_release);
  }
  if (g_in_tick || g_active_frames.load() != 0) return PGR_BATTLE_OK;

  g_in_tick = true;
  FrameGuard guard;
  const uint64_t now = boottime_ms();
  if (!g_stop_requested.load(std::memory_order_acquire)) run_tick_body(now);
  g_in_tick = false;
  return PGR_BATTLE_OK;
}

int api_sample(pgr_battle_sample_v1 *out) {
  if (out == nullptr) return PGR_BATTLE_ERR_NULL_ARG;
  out->version = PGR_BATTLE_API_VERSION;
  out->size = sizeof(pgr_battle_sample_v1);
  out->scope_active = g_scope_active.load(std::memory_order_acquire);
  out->mode = g_mode_pub.load(std::memory_order_acquire);
  out->failed = g_failed_pub.load(std::memory_order_acquire);
  out->swap_blocked = g_swap_blocked_pub.load(std::memory_order_acquire);
  out->charges = g_charges_pub.load(std::memory_order_acquire);
  out->max_moves = g_maxmoves_pub.load(std::memory_order_acquire);
  out->action_serial = g_action_serial.load(std::memory_order_acquire);
  out->scope_serial = g_scope_serial.load(std::memory_order_acquire);
  out->fault_serial = g_fault_serial.load(std::memory_order_acquire);
  out->dropped_events = g_dropped_events.load(std::memory_order_acquire);
  // Conservative finish guard: durable GC handles + owner-thread reentrancy depth (upper bound).
  out->owned_roots = g_durable_roots.load(std::memory_order_acquire) +
                     g_active_frames.load(std::memory_order_acquire);
  out->reserved = 0;
  return PGR_BATTLE_OK;
}

int api_next_event(pgr_battle_event_v1 *out) {
  if (out == nullptr) return 0;
  std::lock_guard<std::mutex> lock(g_fifo_mutex);
  if (g_fifo_size == 0) { g_actions_locked.store(false, std::memory_order_release); return 0; }
  *out = g_fifo[g_fifo_head];
  g_fifo_head = (g_fifo_head + 1) % PGR_BATTLE_EVENT_CAP;
  --g_fifo_size;
  if (g_fifo_size == 0) g_actions_locked.store(false, std::memory_order_release);
  return 1;
}

const pgr_battle_api_v1 g_api = {
    PGR_BATTLE_API_VERSION,
    static_cast<uint32_t>(sizeof(pgr_battle_api_v1)),
    &api_install,
    &api_update,
    &api_observe,
    &api_gym_begin,
    &api_gym_end,
    &api_tick,
    &api_sample,
    &api_next_event,
};

}  // namespace

const void *pgrazor_battles_api(uint32_t version) {
  if (version != PGR_BATTLE_API_VERSION) return nullptr;
  return &g_api;
}

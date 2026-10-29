// PGRazor native migration: Team GO Rocket combat controller (feature id 21).
// See rocket_combat.h for the ABI, the "installs no hooks" rationale (the RVCHARGE
// ObserveBattleState entry is shared with the still-Gum battle controller feature 19) and the
// observe()/begin_ui()/end_ui() forwarding contract. Built -fno-exceptions / -fno-rtti like every
// sibling: a managed throw becomes a fail-closed `failed` latch (+ a feature-20 stop + a
// rocket-error event), and RAII destructors free each transient GC root exactly once. No mutex is
// ever held across a managed call or a GC-handle call.
//
// Lifetime discipline carried over from features 17/18/19:
//   * Every object used across a managed call is pinned in a transient RAII Root FIRST and
//     re-resolved from its handle (or from the durable member handle) after each runtime_invoke --
//     no raw gui/charge/button/gesture/tap pointer is held across another managed call.
//   * Every scope enter/exit/clear bumps the scope epoch. After each managed call the tick
//     re-checks the epoch (and the stop flag) and re-reads the durable member handles; a reentrant
//     exit / new scope cancels the stale action WITHOUT clearing the newer state, and a nested stop
//     defers the root free to the outermost frame.
//   * A reentrant observation while a tick frame is live also revokes the in-flight feature-20
//     ticket (pol_begin bumps the policy generation) so no action can straddle a world change.
//   * The final feature-20 claim() happens AFTER all final target look-ups, immediately before the
//     runtime_invoke, with no GC call in between.
//   * Free helpers detach (null the member) BEFORE gc_free. owned_roots (durable handles + active
//     frames) stays nonzero while any frame runs.

#include "rocket_combat.h"
#include "rocket_actions.h"  // feature 20 policy table, reused internally (never its JS controller)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

static_assert(sizeof(pgr_rocket_combat_config_v1) == 344, "config must be 344 bytes");
static_assert(sizeof(pgr_rocket_combat_sample_v1) == 64, "sample must be 64 bytes");
static_assert(sizeof(pgr_rocket_combat_event_v1) == 16, "event must be 16 bytes");
static_assert(sizeof(pgr_rocket_combat_api_v1) == 72, "table must be 72 bytes");
static_assert(offsetof(pgr_rocket_combat_config_v1, gchandle_new) == 8, "layout");
static_assert(offsetof(pgr_rocket_combat_config_v1, m_director) == 64, "layout");
static_assert(offsetof(pgr_rocket_combat_config_v1, m_alive) == 160, "layout");
static_assert(offsetof(pgr_rocket_combat_config_v1, f_first) == 168, "layout");
static_assert(offsetof(pgr_rocket_combat_config_v1, f_current_state) == 248, "layout");
static_assert(offsetof(pgr_rocket_combat_config_v1, class_rocket) == 256, "layout");
static_assert(offsetof(pgr_rocket_combat_config_v1, class_bool) == 320, "layout");
static_assert(offsetof(pgr_rocket_combat_config_v1, state_open) == 328, "layout");
static_assert(offsetof(pgr_rocket_combat_config_v1, reserved2) == 340, "layout");
static_assert(offsetof(pgr_rocket_combat_sample_v1, owned_roots) == 56, "sample owned_roots @56");

namespace {

constexpr uint64_t kMaxLeaseMs = 2500;  // feature 20 lease cap (clamp here too for the top gate)

// --- lifetime / publication ---------------------------------------------------------
std::mutex                    g_install_mutex;
pgr_rocket_combat_config_v1   g_cfg{};
std::atomic<bool>             g_installed{false};
bool                          g_install_attempted = false;
int                           g_install_error = PGR_RC_ERR_NOT_INSTALLED;
const pgr_rocket_actions_api_v1 *g_pol = nullptr;  // feature 20 action policy table

// --- thread identity ----------------------------------------------------------------
std::atomic<uint64_t> g_unity_tid{0};  // permanent Unity identity, latched once from tick

// --- per-tick policy gate (any thread writes via update; owner thread reads) --------
std::mutex            g_policy_mutex;
std::atomic<uint32_t> g_mask{0};            // rocketMask (bits 1/2/4)
std::atomic<uint64_t> g_lease_until_ms{0};  // absolute CLOCK_BOOTTIME lease deadline

// --- telemetry (atomic; pull-only; any thread) --------------------------------------
std::atomic<uint32_t> g_scope_active{0};
std::atomic<uint32_t> g_failed_pub{0};
std::atomic<uint32_t> g_ui_pub{0};
std::atomic<uint32_t> g_charges_pub{0};
std::atomic<uint32_t> g_shield_pub{0};
std::atomic<int32_t>  g_gate_pub{-1};
std::atomic<uint64_t> g_action_serial{0};
std::atomic<uint64_t> g_scope_serial{0};
std::atomic<uint32_t> g_fault_serial{0};
std::atomic<uint32_t> g_dropped_events{0};
std::atomic<uint32_t> g_durable_roots{0};
std::atomic<uint32_t> g_active_frames{0};

// Fail-closed telemetry latch: set when the bounded FIFO drops an event; while set it BLOCKS every
// new action. Cleared once the FIFO drains empty again.
std::atomic<bool> g_actions_locked{false};

// Stop request: set (mask disarmed + feature 20 stopped) immediately on any tick(stopping != 0),
// even a nested one; drained (rocketClear) on the OUTERMOST frame. NOT terminal.
std::atomic<bool> g_stop_requested{false};

// --- bounded telemetry FIFO (owner thread writes + drains) --------------------------
std::mutex                    g_fifo_mutex;
pgr_rocket_combat_event_v1    g_fifo[PGR_RC_EVENT_CAP]{};
uint32_t                      g_fifo_head = 0, g_fifo_tail = 0, g_fifo_size = 0;

// --- owner-thread-only Rocket state (mirrors rocketState in rocket.js) --------------
uint64_t g_epoch = 0;        // bumped on every clear/enter/exit; cancels straddling actions
bool     g_in_tick = false;  // non-reentrant tick body guard (observe/begin_ui/end_ui are NOT gated)
void    *g_state_handle = nullptr;   // InvasionCombatState scope
void    *g_ui_handle = nullptr;      // legacy CombatUI
void    *g_vui_handle = nullptr;     // vNext gui
void    *g_shield_handle = nullptr;  // shield modal
void    *g_charges[PGR_RC_MAX_CHARGES] = {nullptr};
uint32_t g_charge_count = 0;
bool     g_shield_used = false;
bool     g_failed = false;
int32_t  g_gate = -1;         // last emitted attack-gate reason (-1 == none, like rocketState.gate=null)
uint64_t g_last_fast_ms = 0;  // 5 s de-dupe of the quick telemetry event

// Generation-safe paired-UI publication (CombatUI Initialize onEnter/onLeave).
void    *g_pending_ui = nullptr;
uint64_t g_pending_ui_token = 0;
uint64_t g_pending_ui_epoch = 0;
uint64_t g_token_src = 0;

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

// --- feature 20 (rocket_actions) thin wrappers --------------------------------------
inline int      pol_update(uint32_t mask, uint64_t lease, uint64_t tid) { return g_pol ? g_pol->update(mask, lease, tid) : 0; }
inline int      pol_observe(uint32_t kind, uint64_t tid)                { return g_pol ? g_pol->observe(kind, tid) : 0; }
inline uint64_t pol_begin(uint64_t tid)                                 { return g_pol ? g_pol->begin(tid) : 0; }
inline int      pol_claim(uint64_t t, uint32_t a, uint64_t tid)         { return g_pol ? g_pol->claim(t, a, tid) : 0; }
inline void     pol_stop(uint64_t tid)                                  { if (g_pol) g_pol->stop(tid); }

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
// present(obj, bool-field): the bool backing field read == 1 (a value > 1 is false).
inline bool    field_present(void *o, void *f) { uint8_t v = 0; if (o) field_get(o, f, &v); return v == 1; }

// Invoke a managed method; on a managed exception set *err and return null.
inline void *invoke(void *method, void *obj, void **params, bool *err) {
  void *exc = nullptr;
  void *r = reinterpret_cast<void *(*)(void *, void *, void **, void **)>(g_cfg.runtime_invoke)(
      method, obj, params, &exc);
  if (exc != nullptr) { *err = true; return nullptr; }
  return r;
}

// Boxed bool read: root the box across unbox + exact-class check; a value > 1 is a fault.
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
// UnityEngine.Object op_Implicit(obj) -> bool (static liveness probe).
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
  void *release() { void *x = h; h = nullptr; return x; }
  void reset() { void *x = h; h = nullptr; if (x) gc_free(x); }
  ~Root() { reset(); }
};

// --- durable-root accounting + publication ------------------------------------------
void refresh() {
  uint32_t n = 0;
  if (g_state_handle) ++n;
  if (g_ui_handle) ++n;
  if (g_vui_handle) ++n;
  if (g_shield_handle) ++n;
  if (g_pending_ui) ++n;
  n += g_charge_count;
  g_durable_roots.store(n, std::memory_order_release);
  g_scope_active.store(g_state_handle ? 1u : 0u, std::memory_order_release);
  g_failed_pub.store(g_failed ? 1u : 0u, std::memory_order_release);
  g_ui_pub.store((g_ui_handle || g_vui_handle) ? 1u : 0u, std::memory_order_release);
  g_charges_pub.store(g_charge_count, std::memory_order_release);
  g_shield_pub.store(g_shield_handle ? 1u : 0u, std::memory_order_release);
  g_gate_pub.store(g_gate, std::memory_order_release);
}

// --- bounded FIFO (owner thread) ----------------------------------------------------
void push_event(uint32_t code, int32_t id, int32_t aux) {
  std::lock_guard<std::mutex> lock(g_fifo_mutex);
  if (g_fifo_size >= PGR_RC_EVENT_CAP) {
    g_dropped_events.fetch_add(1, std::memory_order_relaxed);
    g_actions_locked.store(true, std::memory_order_release);  // fail-closed: block new actions
    return;
  }
  g_fifo[g_fifo_tail] = pgr_rocket_combat_event_v1{code, id, aux, 0};
  g_fifo_tail = (g_fifo_tail + 1) % PGR_RC_EVENT_CAP;
  ++g_fifo_size;
}
void note_fault() {
  g_fault_serial.fetch_add(1, std::memory_order_release);
  push_event(PGR_RC_EVT_ERROR, 0, 0);
}
void gate(int32_t reason) {
  if (g_gate != reason) { g_gate = reason; g_gate_pub.store(reason, std::memory_order_release);
    push_event(PGR_RC_EVT_GATE, reason, 0); }
}

inline bool stale(uint64_t epoch) { return g_epoch != epoch || g_stop_requested.load(std::memory_order_acquire); }

// --- root teardown helpers (detach BEFORE gc_free) ----------------------------------
void drop_ui()  { void *h = g_ui_handle;  g_ui_handle = nullptr;  refresh(); gc_free(h); }
void drop_vui() { void *h = g_vui_handle; g_vui_handle = nullptr; refresh(); gc_free(h); }
// rocketDrop('shield'): notify feature 20 PROMPT_CLOSE first, then detach + free.
void drop_shield(uint64_t tid) {
  if (!g_shield_handle) return;
  pol_observe(PGR_ROCKET_PROMPT_CLOSE, tid);
  void *h = g_shield_handle; g_shield_handle = nullptr; refresh(); gc_free(h);
}
// rocketClear: notify feature 20 of scope exit, detach the complete old state before the first
// callback-capable free, reset flags/gate, bump the scope epoch (invalidating in-flight actions and
// any pending UI token).
void rocket_clear(uint64_t tid) {
  pol_observe(PGR_ROCKET_EXIT, tid);
  void *old[5 + PGR_RC_MAX_CHARGES]; uint32_t n = 0;
  if (g_state_handle) old[n++] = g_state_handle;
  if (g_ui_handle) old[n++] = g_ui_handle;
  if (g_vui_handle) old[n++] = g_vui_handle;
  if (g_shield_handle) old[n++] = g_shield_handle;
  if (g_pending_ui) old[n++] = g_pending_ui;
  for (uint32_t i = 0; i < g_charge_count; ++i) old[n++] = g_charges[i];
  g_state_handle = g_ui_handle = g_vui_handle = g_shield_handle = g_pending_ui = nullptr;
  std::memset(g_charges, 0, sizeof(g_charges)); g_charge_count = 0;
  g_pending_ui_token = 0; g_shield_used = false; g_failed = false; g_gate = -1; g_last_fast_ms = 0;
  ++g_epoch;
  refresh();
  for (uint32_t i = 0; i < n; ++i) gc_free(old[i]);
}

void fault(uint64_t tid) {
  g_failed = true;
  pol_stop(tid);
  note_fault();
  refresh();
}

// --- charge-candidate list (owner thread) -------------------------------------------
// vm.observe: prune dead entries, dedupe by live identity, append if under the cap (<= 4).
void charge_capture(void *obj, uint64_t epoch) {
  bool err=false; Root incoming; incoming.acquire(obj,&err);
  if(err||stale(epoch))return;
  for(int i=int(g_charge_count)-1;i>=0;--i){
    void *h=g_charges[i];
    void *current=gc_target(h);if(stale(epoch))return;
    bool live=m_is_alive(current,&err);if(stale(epoch))return;
    if(err){fault(g_unity_tid.load());return;}
    if(!live){
      for(uint32_t j=i;j+1<g_charge_count;++j)g_charges[j]=g_charges[j+1];
      g_charges[--g_charge_count]=nullptr;refresh();gc_free(h);if(stale(epoch))return;
    }
  }
  for(uint32_t i=0;i<g_charge_count;++i){
    void *candidate=gc_target(g_charges[i]);if(stale(epoch))return;
    void *in=incoming.target();if(stale(epoch))return;
    if(candidate==in)return;
  }
  if(g_charge_count<PGR_RC_MAX_CHARGES)g_charges[g_charge_count++]=incoming.release();
}

// --- frame guard --------------------------------------------------------------------
void drain_stop();
struct FrameGuard {
  FrameGuard() { g_active_frames.fetch_add(1, std::memory_order_relaxed); }
  ~FrameGuard() { if (g_active_frames.load() == 1) drain_stop(); g_active_frames.fetch_sub(1, std::memory_order_relaxed); }
};
void drain_stop() {
  if (!g_stop_requested.load(std::memory_order_acquire)) return;
  // Keep the latch set while freeing, so a callback cannot rearm the controller.
  rocket_clear(g_unity_tid.load(std::memory_order_acquire));
  g_stop_requested.store(false, std::memory_order_release);
}

// Revoke any in-flight feature-20 ticket when an observation reenters a live tick frame.
inline void cancel_inflight(uint64_t tid) { if (g_in_tick) pol_begin(tid); }

// --- config validation + install ----------------------------------------------------
bool config_ok(const pgr_rocket_combat_config_v1 *c) {
  const void *const ptrs[] = {
      c->gchandle_new, c->gchandle_get_target, c->gchandle_free, c->object_get_class, c->object_unbox,
      c->runtime_invoke, c->field_get_value, c->m_director, c->m_active, c->m_fast, c->m_charge,
      c->m_visible, c->m_interactable, c->m_shield, c->vm_blocked, c->vm_charged, c->vm_visible,
      c->vm_charge, c->vm_fast, c->m_alive, c->f_first, c->f_second, c->f_charged, c->f_input,
      c->f_shields, c->f_button, c->vf_gesture, c->vf_enabled, c->vf_tap, c->vf_button,
      c->f_current_state, c->class_rocket, c->class_rui, c->class_rdir, c->class_rcharge,
      c->class_rshield, c->class_rvui, c->class_rvgest, c->class_rvcharge, c->class_bool};
  for (const void *p : ptrs) if (p == nullptr) return false;
  if (c->state_open == PGR_RC_CLOSED_STATE || c->state_open < 0) return false;
  if (c->reserved0 != 0 || c->reserved1 != 0 || c->reserved2 != 0) return false;
  return true;
}

int api_install(const pgr_rocket_combat_config_v1 *config) {
  if (config == nullptr) return PGR_RC_ERR_NULL_ARG;
  std::lock_guard<std::mutex> lock(g_install_mutex);
  if (config->version != PGR_RC_API_VERSION ||
      config->size != sizeof(pgr_rocket_combat_config_v1) || !config_ok(config))
    return PGR_RC_ERR_BAD_CONFIG;
  if (g_installed.load(std::memory_order_acquire))
    return (std::memcmp(&g_cfg, config, sizeof(g_cfg)) == 0) ? PGR_RC_OK : PGR_RC_ERR_ALREADY_BOUND;
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;
  const pgr_rocket_actions_api_v1 *pol =
      static_cast<const pgr_rocket_actions_api_v1 *>(pgrazor_rocket_actions_api(1));
  if (pol == nullptr || pol->size != sizeof(pgr_rocket_actions_api_v1))
    return g_install_error = PGR_RC_ERR_NO_POLICY;
  if (boottime_ms() == 0) return g_install_error = PGR_RC_ERR_NO_CLOCK;
  g_cfg = *config;
  g_pol = pol;
  g_installed.store(true, std::memory_order_release);
  push_event(PGR_RC_EVT_READY, 0, 1);
  return g_install_error = PGR_RC_OK;
}

int api_update(uint32_t mask, uint64_t lease_ms, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_RC_ERR_NOT_INSTALLED;
  std::lock_guard<std::mutex> lock(g_policy_mutex);
  const uint64_t owner = g_unity_tid.load(std::memory_order_acquire);
  const bool wrong_thread = !proven_tid || current_tid()!=proven_tid || (owner && proven_tid!=owner);
  if(g_stop_requested.load(std::memory_order_acquire)){
    g_mask.store(0);g_lease_until_ms.store(0);pol_update(0,0,proven_tid);return PGR_RC_OK;
  }
  if (wrong_thread || proven_tid == 0) {
    g_mask.store(0, std::memory_order_release);
    g_lease_until_ms.store(0, std::memory_order_release);
    pol_update(0, 0, proven_tid);  // feature 20 disarms on foreign / zero tid too
    return wrong_thread ? PGR_RC_ERR_NULL_ARG : PGR_RC_OK;
  }
  const uint32_t bits = mask & 7u;
  const uint64_t clamped = lease_ms > kMaxLeaseMs ? kMaxLeaseMs : lease_ms;
  const uint64_t now = boottime_ms();
  g_mask.store(bits, std::memory_order_release);
  g_lease_until_ms.store((clamped && now) ? now + clamped : 0, std::memory_order_release);
  pol_update(bits, lease_ms, proven_tid);  // feature 20 clamps the lease itself
  return PGR_RC_OK;
}

// --- observations (owner thread; exact-class identity checks live here) -------------
// Every observation invalidates scans before its first callback-capable operation.
// Incoming roots are secured before old slots are detached; publishing is conditional
// on the observation still being current after every allocation or free.
int api_observe(uint32_t kind,void *obj,int32_t aux,uint64_t tid){
  if(!g_installed.load()||!on_unity(tid)||g_stop_requested.load())return PGR_RC_OK;
  FrameGuard guard;const uint64_t epoch=++g_epoch;cancel_inflight(tid);
  const bool capture=kind==PGR_RC_OBS_SCOPE_ENTER||kind==PGR_RC_OBS_UI_UPDATE||
    kind==PGR_RC_OBS_VNEXT_UPDATE||kind==PGR_RC_OBS_SHIELD_PROMPT;
  void *klass=kind==PGR_RC_OBS_SCOPE_ENTER?g_cfg.class_rocket:
    kind==PGR_RC_OBS_UI_UPDATE?g_cfg.class_rui:
    kind==PGR_RC_OBS_VNEXT_UPDATE?g_cfg.class_rvui:g_cfg.class_rshield;
  bool err=false;Root incoming;
  if(capture){
    if((kind!=PGR_RC_OBS_SCOPE_ENTER&&!g_state_handle)||!exact(obj,klass)||stale(epoch))return PGR_RC_OK;
    incoming.acquire(obj,&err);if(stale(epoch))return PGR_RC_OK;
    if(err){fault(tid);return PGR_RC_OK;}
  }
  switch(kind){
    case PGR_RC_OBS_SCOPE_ENTER:{
      rocket_clear(tid);
      if(stale(epoch+1))break;
      g_state_handle=incoming.release();pol_observe(PGR_ROCKET_ENTER,tid);
      g_scope_serial.fetch_add(1);refresh();push_event(PGR_RC_EVT_SCOPE,0,1);break;
    }
    case PGR_RC_OBS_SCOPE_EXIT:{
      void *cur=gc_target(g_state_handle);if(stale(epoch))break;
      if(cur&&cur==obj){rocket_clear(tid);if(!stale(epoch+1))push_event(PGR_RC_EVT_SCOPE,0,0);}break;
    }
    case PGR_RC_OBS_UI_UPDATE:{
      void *cur=gc_target(g_ui_handle);if(stale(epoch))break;
      void *in=incoming.target();if(stale(epoch))break;
      if(cur!=in){
        void *old=g_ui_handle,*pending=g_pending_ui;
        g_ui_handle=g_pending_ui=nullptr;g_pending_ui_token=0;refresh();
        gc_free(old);gc_free(pending);if(stale(epoch))break;
        g_ui_handle=incoming.release();refresh();push_event(PGR_RC_EVT_UI,0,PGR_RC_UI_LIVE);
      }break;
    }
    case PGR_RC_OBS_UI_CLOSE:{
      void *cur=gc_target(g_ui_handle);if(stale(epoch))break;
      void *pending=gc_target(g_pending_ui);if(stale(epoch))break;
      void *old=nullptr,*oldPending=nullptr;
      if(cur&&cur==obj){old=g_ui_handle;g_ui_handle=nullptr;}
      if(pending&&pending==obj){oldPending=g_pending_ui;g_pending_ui=nullptr;g_pending_ui_token=0;}
      refresh();gc_free(old);gc_free(oldPending);break;
    }
    case PGR_RC_OBS_VNEXT_UPDATE:{
      void *cur=gc_target(g_vui_handle);if(stale(epoch))break;
      void *in=incoming.target();if(stale(epoch))break;
      if(cur!=in){drop_vui();if(stale(epoch))break;g_vui_handle=incoming.release();refresh();push_event(PGR_RC_EVT_UI,0,PGR_RC_UI_VNEXT);}break;
    }
    case PGR_RC_OBS_VNEXT_CLOSE:{
      void *cur=gc_target(g_vui_handle);if(stale(epoch))break;
      if(cur&&cur==obj){
        void *old[1+PGR_RC_MAX_CHARGES];uint32_t n=0;old[n++]=g_vui_handle;g_vui_handle=nullptr;
        for(uint32_t i=0;i<g_charge_count;++i)old[n++]=g_charges[i];
        std::memset(g_charges,0,sizeof(g_charges));g_charge_count=0;refresh();
        for(uint32_t i=0;i<n;++i)gc_free(old[i]);
      }break;
    }
    case PGR_RC_OBS_CHARGE:
      if(g_state_handle&&exact(obj,g_cfg.class_rvcharge)&&!stale(epoch))charge_capture(obj,epoch);
      refresh();break;
    case PGR_RC_OBS_SHIELD_PROMPT:
      drop_shield(tid);if(stale(epoch))break;g_shield_used=false;
      if(aux){g_shield_handle=incoming.release();pol_observe(PGR_ROCKET_PROMPT_OPEN,tid);}refresh();break;
    case PGR_RC_OBS_SHIELD_MANUAL:{
      void *cur=gc_target(g_shield_handle);if(stale(epoch))break;
      if(cur&&cur==obj){g_shield_used=true;pol_observe(PGR_ROCKET_SHIELD_USED,tid);}break;
    }
    case PGR_RC_OBS_SHIELD_CLOSE:{
      void *cur=gc_target(g_shield_handle);if(stale(epoch))break;
      if(cur&&cur==obj)drop_shield(tid);break;
    }
    default:break;
  }
  return PGR_RC_OK;
}

uint64_t api_begin_ui(void *obj,uint64_t tid){
  if(!g_installed.load()||!on_unity(tid)||g_stop_requested.load())return 0;
  FrameGuard guard;const uint64_t epoch=++g_epoch;cancel_inflight(tid);
  if(!g_state_handle||!exact(obj,g_cfg.class_rui)||stale(epoch))return 0;
  bool err=false;Root in;in.acquire(obj,&err);if(stale(epoch))return 0;
  if(err){fault(tid);return 0;}
  void *old=g_pending_ui,*ui=g_ui_handle;g_pending_ui=g_ui_handle=nullptr;g_pending_ui_token=0;refresh();
  gc_free(old);gc_free(ui);if(stale(epoch))return 0;
  if(g_token_src==UINT64_MAX){fault(tid);return 0;}
  g_pending_ui=in.release();g_pending_ui_epoch=epoch;g_pending_ui_token=++g_token_src;refresh();return g_pending_ui_token;
}

int api_end_ui(uint64_t token,void *,uint64_t tid){
  if(!g_installed.load()||!on_unity(tid)||!token)return PGR_RC_OK;
  FrameGuard guard;cancel_inflight(tid);
  if(token!=g_pending_ui_token||!g_pending_ui)return PGR_RC_OK;
  // The unique token identifies the rooted pending object; never trust an onEnter
  // raw pointer carried across a managed ShowUI call (the GC may move it).
  const bool publish=g_pending_ui_epoch==g_epoch&&!g_stop_requested.load()&&g_state_handle;
  ++g_epoch;
  void *pending=g_pending_ui;g_pending_ui=nullptr;g_pending_ui_token=0;
  void *old=nullptr;
  if(publish){old=g_ui_handle;g_ui_handle=pending;}else old=pending;
  refresh();gc_free(old);return PGR_RC_OK;
}

// --- tick helpers -------------------------------------------------------------------
// Lightweight feature-21 gate before each action (feature 20 claim() re-checks authoritatively).
bool final_ok(uint64_t epoch) {
  if (stale(epoch) || g_failed || !g_state_handle) return false;
  if (g_actions_locked.load(std::memory_order_acquire)) return false;
  const uint64_t now = boottime_ms();
  const uint64_t lease = g_lease_until_ms.load(std::memory_order_acquire);
  return now != 0 && lease != 0 && now < lease;
}
void emit_action(int32_t act, int32_t aux) {
  g_action_serial.fetch_add(1, std::memory_order_release);
  push_event(PGR_RC_EVT_ACTION, act, aux);
}

// vNext decision (rocketVNextTick). Returns always (the caller returns after it).
void vnext_tick(uint64_t tid, uint64_t epoch, uint32_t mask, uint64_t ticket) {
  bool err = false;
  void *gui = gc_target(g_vui_handle);if(stale(epoch))return;
  Root uiRoot;uiRoot.acquire(gui,&err);if(stale(epoch))return;if(err){fault(tid);return;}
  gui=uiRoot.target();if(stale(epoch))return;
  bool live = gui && m_is_alive(gui, &err);
  if (err) { if(!stale(epoch))fault(tid); return; }
  if (stale(epoch) || !g_vui_handle) return;
  if (!live) { drop_vui(); return; }
  gui = uiRoot.target();if(stale(epoch))return;
  bool blocked = invoke_bool(g_cfg.vm_blocked, gui, nullptr, &err);
  if (err) { if(!stale(epoch))fault(tid); return; }
  if (stale(epoch) || !g_vui_handle) return;
  if (blocked) { gate(PGR_RC_GATE_VN_BLOCKED); return; }

  if (mask & 2u) {
    for (uint32_t i = 0; i < g_charge_count; ++i) {
      if (stale(epoch)) return;
      if (i >= g_charge_count) break;
      void *candidate=gc_target(g_charges[i]);if(stale(epoch))return;
      Root c;c.acquire(candidate,&err);if(stale(epoch))return;if(err){fault(tid);return;}
      bool calive = m_is_alive(c.target(), &err);
      bool charged = !err && calive && invoke_bool(g_cfg.vm_charged, c.target(), nullptr, &err);
      bool cvis = !err && charged && invoke_bool(g_cfg.vm_visible, c.target(), nullptr, &err);
      if (err) { if(!stale(epoch))fault(tid); return; }
      if (stale(epoch)) return;
      if (!calive || !charged || !cvis) continue;
      void *button = read_ref(c.target(), g_cfg.vf_button);
      if (!button) continue;
      Root b; b.acquire(button, &err);if(stale(epoch))return;if(err){fault(tid);return;}
      bool balive = m_is_alive(b.target(), &err);
      bool binter = !err && balive && invoke_bool(g_cfg.m_interactable, b.target(), nullptr, &err);
      if (err) { if(!stale(epoch))fault(tid); return; }
      if (stale(epoch)) return;
      if (!balive || !binter) continue;
      void *target = c.target();  // final lookup (c is rooted -> current)
      if (!target || !final_ok(epoch)) return;
      if (!pol_claim(ticket, PGR_ROCKET_VNEXT_CHARGE, tid)) return;
      invoke(g_cfg.vm_charge, target, nullptr, &err);
      if (err) { if(!stale(epoch))fault(tid); return; }
      emit_action(PGR_RC_ACT_CHARGED, 2);  // bit1: vNext path
      return;
    }
  }

  gui = gc_target(g_vui_handle);
  if (!gui || stale(epoch)) return;
  void *gesture = read_ref(gui, g_cfg.vf_gesture);
  if (!gesture || !exact(gesture, g_cfg.class_rvgest)) { gate(PGR_RC_GATE_VN_NOFAST); return; }
  Root g; g.acquire(gesture, &err); if (err) { if(!stale(epoch))fault(tid); return; }
  bool galive = m_is_alive(g.target(), &err);
  if (err) { if(!stale(epoch))fault(tid); return; }
  if (stale(epoch) || !g_vui_handle) return;
  bool genabled = galive && field_present(g.target(), g_cfg.vf_enabled);
  if (!galive || !genabled) { gate(PGR_RC_GATE_VN_NOFAST); return; }
  gate(PGR_RC_GATE_VN_ACTIVE);
  if (mask & 1u) {
    void *tap = read_ref(g.target(), g_cfg.vf_tap);
    if (!tap) return;
    Root tp; tp.acquire(tap, &err); if (err) { if(!stale(epoch))fault(tid); return; }
    if (stale(epoch)) return;
    void *target = g.target();      // gesture is the invoke target
    void *tapv = tp.target();        // final look-ups before claim
    if (!target || !tapv || !final_ok(epoch)) return;
    if (!pol_claim(ticket, PGR_ROCKET_QUICK, tid)) return;
    void *params[1] = {tapv};        // vNext fast takes ONE tap-reference argument (no EventArgs)
    invoke(g_cfg.vm_fast, target, params, &err);
    if (err) { if(!stale(epoch))fault(tid); return; }
    g_action_serial.fetch_add(1, std::memory_order_release);
    const uint64_t now = boottime_ms();
    if (!g_last_fast_ms || (now >= g_last_fast_ms && now - g_last_fast_ms > 5000)) {
      g_last_fast_ms = now;
      push_event(PGR_RC_EVT_ACTION, PGR_RC_ACT_QUICK, 2);  // bit1: vNext path, no accepted flag
    }
  }
}

void tick_body(uint64_t tid, uint64_t now) {
  const uint32_t mask = g_mask.load(std::memory_order_acquire);
  const uint64_t lease = g_lease_until_ms.load(std::memory_order_acquire);
  if (g_failed || !g_state_handle || mask == 0 || now == 0 || now >= lease) return;
  if (g_actions_locked.load(std::memory_order_acquire)) return;

  const uint64_t epoch = g_epoch;
  const uint64_t ticket = pol_begin(tid);
  if (!ticket) return;
  bool err = false;

  void *scope=gc_target(g_state_handle);if(stale(epoch))return;
  const bool scope_ok=exact(scope,g_cfg.class_rocket);if(stale(epoch))return;
  if(!scope_ok){rocket_clear(tid);return;}

  // (1) shield first.
  if (g_shield_handle) {
    void *gui = gc_target(g_shield_handle);if(stale(epoch))return;
    Root shieldRoot;shieldRoot.acquire(gui,&err);if(stale(epoch))return;if(err){fault(tid);return;}
    gui=shieldRoot.target();if(stale(epoch))return;
    bool live = gui && m_is_alive(gui, &err);
    if (err) { if(!stale(epoch))fault(tid); return; }
    if (stale(epoch) || !g_shield_handle) return;
    if (!live) {
      drop_shield(tid);
    } else {
      gui = shieldRoot.target();if(stale(epoch))return; // re-resolve after liveness
      int32_t cs = read_s32(gui, g_cfg.f_current_state);
      if (cs == PGR_RC_CLOSED_STATE) {
        drop_shield(tid);
      } else if ((mask & 4u) && !g_shield_used && cs == g_cfg.state_open &&
                 read_s32(gui, g_cfg.f_shields) > 0) {
        void *button = read_ref(gui, g_cfg.f_button);
        if (button) {
          Root b; b.acquire(button, &err); if (err) { if(!stale(epoch))fault(tid); return; }
          bool balive = m_is_alive(b.target(), &err);
          bool binter = !err && balive && invoke_bool(g_cfg.m_interactable, b.target(), nullptr, &err);
          if (err) { if(!stale(epoch))fault(tid); return; }
          if (stale(epoch) || !g_shield_handle) return;
          if (balive && binter) {
            void *target = gc_target(g_shield_handle);  // final lookup
            if (!target || !final_ok(epoch)) return;
            if (!pol_claim(ticket, PGR_ROCKET_SHIELD, tid)) return;
            g_shield_used = true;
            invoke(g_cfg.m_shield, target, nullptr, &err);
            if (err) { if(!stale(epoch))fault(tid); return; }
            emit_action(PGR_RC_ACT_SHIELD, 0);
            return;
          }
        }
      }
    }
  }

  // (2) vNext path (if a vNext gui is held).
  if (g_vui_handle) { vnext_tick(tid, epoch, mask, ticket); return; }

  // (3) legacy CombatUI path.
  if (!g_ui_handle) { gate(PGR_RC_GATE_WAIT_UI); return; }
  {
    void *gui = gc_target(g_ui_handle);if(stale(epoch))return;
    Root uiRoot;uiRoot.acquire(gui,&err);if(stale(epoch))return;if(err){fault(tid);return;}
    gui=uiRoot.target();if(stale(epoch))return;
    bool live = gui && m_is_alive(gui, &err);
    if (err) { if(!stale(epoch))fault(tid); return; }
    if (stale(epoch) || !g_ui_handle) return;
    if (!live) { drop_ui(); return; }
  }
  // director look-up + exact-class + active gate (rooted across the active invoke).
  void *legacyGui=gc_target(g_ui_handle);if(stale(epoch))return;
  Root legacyRoot;legacyRoot.acquire(legacyGui,&err);if(stale(epoch))return;if(err){fault(tid);return;}
  legacyGui=legacyRoot.target();if(stale(epoch))return;
  void *dir = invoke(g_cfg.m_director, legacyGui, nullptr, &err);
  if (err) { if(!stale(epoch))fault(tid); return; }
  Root dh; dh.acquire(dir, &err);
  bool is_dir = !err && exact(dh.target(), g_cfg.class_rdir);
  if (err) { if(!stale(epoch))fault(tid); return; }
  if (stale(epoch) || !g_ui_handle) return;
  if (!is_dir) { gate(PGR_RC_GATE_DIRECTOR); return; }
  bool active = invoke_bool(g_cfg.m_active, dh.target(), nullptr, &err);
  if (err) { if(!stale(epoch))fault(tid); return; }
  if (stale(epoch) || !g_ui_handle) return;
  if (!active) { gate(PGR_RC_GATE_INACTIVE); return; }
  gate(PGR_RC_GATE_ACTIVE);

  // (4) legacy charged: first charged+inputEnabled+visible candidate in [first, second].
  if (mask & 2u) {
    void *const fields[2] = {g_cfg.f_first, g_cfg.f_second};
    for (int i = 0; i < 2; ++i) {
      void *gui = gc_target(g_ui_handle);  // re-resolve each iteration (a visible invoke may move it)
      if (!gui || stale(epoch)) return;
      void *charge = read_ref(gui, fields[i]);
      if (!charge || !exact(charge, g_cfg.class_rcharge)) continue;
      Root c; c.acquire(charge, &err); if (err) { if(!stale(epoch))fault(tid); return; }
      bool calive = m_is_alive(c.target(), &err);
      if (err) { if(!stale(epoch))fault(tid); return; }
      if (stale(epoch)) return;
      if (!calive) continue;
      bool ch = field_present(c.target(), g_cfg.f_charged);
      bool in = ch && field_present(c.target(), g_cfg.f_input);
      bool vis = in && invoke_bool(g_cfg.m_visible, c.target(), nullptr, &err);
      if (err) { if(!stale(epoch))fault(tid); return; }
      if (stale(epoch)) return;
      if (!(ch && in && vis)) continue;
      void *target = c.target();  // final lookup
      if (!target || !final_ok(epoch)) return;
      if (!pol_claim(ticket, PGR_ROCKET_LEGACY_CHARGE, tid)) return;
      invoke(g_cfg.m_charge, target, nullptr, &err);
      if (err) { if(!stale(epoch))fault(tid); return; }
      emit_action(PGR_RC_ACT_CHARGED, 0);  // legacy path
      return;
    }
  }

  // (5) legacy quick: TryFastAttack on the director (returns a CombatActionProto pointer).
  if (mask & 1u) {
    void *target = dh.target();  // final lookup (director rooted)
    if (!target || !final_ok(epoch)) return;
    if (!pol_claim(ticket, PGR_ROCKET_QUICK, tid)) return;
    void *res = invoke(g_cfg.m_fast, target, nullptr, &err);
    if (err) { if(!stale(epoch))fault(tid); return; }
    const bool accepted = res != nullptr;  // accepted == !isNull
    g_action_serial.fetch_add(1, std::memory_order_release);
    if (!g_last_fast_ms || (now >= g_last_fast_ms && now - g_last_fast_ms > 5000)) {
      g_last_fast_ms = now;
      push_event(PGR_RC_EVT_ACTION, PGR_RC_ACT_QUICK, accepted ? 1 : 0);  // bit0: accepted
    }
  }
}

int api_tick(uint64_t proven_tid, int stopping) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_RC_ERR_NOT_INSTALLED;
  if(stopping){
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    g_stop_requested.store(true);g_mask.store(0);g_lease_until_ms.store(0);pol_stop(proven_tid);
  }
  if (proven_tid == 0 || current_tid() != proven_tid) return PGR_RC_OK;
  uint64_t expected = 0;
  g_unity_tid.compare_exchange_strong(expected, proven_tid, std::memory_order_acq_rel);
  if (g_unity_tid.load(std::memory_order_acquire) != proven_tid) return PGR_RC_OK;

  // Record a stop + disarm IMMEDIATELY (even nested) so no action can start before the outermost
  // drain. A nested tick returns here; the outer frame already running drains the stop.
  if (stopping) {
    g_stop_requested.store(true, std::memory_order_release);
    g_mask.store(0, std::memory_order_release);
    g_lease_until_ms.store(0, std::memory_order_release);
    pol_update(0, 0, proven_tid);
    pol_stop(proven_tid);
  }
  if (g_in_tick || g_active_frames.load() != 0) return PGR_RC_OK;

  g_in_tick = true;
  FrameGuard guard;
  const uint64_t now = boottime_ms();
  if (!g_stop_requested.load(std::memory_order_acquire)) tick_body(proven_tid, now);
  g_in_tick = false;
  return PGR_RC_OK;
}

int api_sample(pgr_rocket_combat_sample_v1 *out) {
  if (out == nullptr) return PGR_RC_ERR_NULL_ARG;
  out->version = PGR_RC_API_VERSION;
  out->size = sizeof(pgr_rocket_combat_sample_v1);
  out->scope_active = g_scope_active.load(std::memory_order_acquire);
  out->failed = g_failed_pub.load(std::memory_order_acquire);
  out->ui_active = g_ui_pub.load(std::memory_order_acquire);
  out->charges = g_charges_pub.load(std::memory_order_acquire);
  out->shield = g_shield_pub.load(std::memory_order_acquire);
  out->gate = g_gate_pub.load(std::memory_order_acquire);
  out->action_serial = g_action_serial.load(std::memory_order_acquire);
  out->scope_serial = g_scope_serial.load(std::memory_order_acquire);
  out->fault_serial = g_fault_serial.load(std::memory_order_acquire);
  out->dropped_events = g_dropped_events.load(std::memory_order_acquire);
  // Conservative finish guard: durable GC handles + owner-thread reentrancy depth (upper bound).
  out->owned_roots = g_durable_roots.load(std::memory_order_acquire) +
                     g_active_frames.load(std::memory_order_acquire);
  out->reserved = 0;
  return PGR_RC_OK;
}

int api_next_event(pgr_rocket_combat_event_v1 *out) {
  if (out == nullptr) return 0;
  std::lock_guard<std::mutex> lock(g_fifo_mutex);
  if (g_fifo_size == 0) { g_actions_locked.store(false, std::memory_order_release); return 0; }
  *out = g_fifo[g_fifo_head];
  g_fifo_head = (g_fifo_head + 1) % PGR_RC_EVENT_CAP;
  --g_fifo_size;
  if (g_fifo_size == 0) g_actions_locked.store(false, std::memory_order_release);
  return 1;
}

const pgr_rocket_combat_api_v1 g_api = {
    PGR_RC_API_VERSION,
    static_cast<uint32_t>(sizeof(pgr_rocket_combat_api_v1)),
    &api_install,
    &api_update,
    &api_observe,
    &api_begin_ui,
    &api_end_ui,
    &api_tick,
    &api_sample,
    &api_next_event,
};

}  // namespace

const void *pgrazor_rocket_combat_api(uint32_t version) {
  if (version != PGR_RC_API_VERSION) return nullptr;
  return &g_api;
}

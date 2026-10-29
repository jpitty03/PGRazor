// Standalone focused test of the native Rocket combat controller (feature 21). Like the hook-less
// siblings this module installs NO Dobby/Gum hooks. By default the harness loads the compiled
// library and queries feature 21 (which internally reuses the feature-20 action table that is
// linked into the same library); PGR_RC_DIRECT_TEST links rocket_combat.cpp + rocket_actions.cpp
// directly. It drives a compact fake IL2CPP (runtime_invoke over the Rocket methods, boxed bool
// returns, field get) through a proper handle table (a "GC move" relocates an object while its
// handle stays valid) and forks per scenario for clean process-lifetime state.
//
// Coverage: legacy quick (TryFastAttack accepted flag) and charged (first+second candidate list);
// shield-first fire (open GUI state + >0 shields + interactable button, once) and manual-shield
// lock-out; vNext quick (ONE tap-reference arg) and charged (interactable button); director
// exact-class + active + waiting-for-ui gates; scope-exit clear with balanced roots; STOP clear +
// reject; shield not-open fall-through; generation-safe paired-UI begin/end (a stale onLeave can
// never overwrite a live replacement, a clean begin/end publishes); charge dedupe/cap/prune;
// managed-invoke fault latch; wrong-thread no-op; feature-20 lease expiry; feature-20 cadence; a
// director rooted across a managed call that MOVES, refreshed at use. Real CLOCK_BOOTTIME is used
// for the lease/cadence scenarios.
//
// A bounded reentrancy-injection layer (g_cb, fired once from inside gc_new / gc_get / gc_free /
// runtime_invoke, with a one-level recursion guard) then re-enters the controller in exactly the
// window a real GC callback or a game thread could, to exercise lifetime edges: a newer scope
// winning inside the first scope's gc_new (preserved, then STOP-drained); a newer legacy UI winning
// while the superseded ui is being freed; stale / replayed begin_ui/end_ui tokens and a close before
// the matching onLeave; a STOP (+ a rejected rearm) injected inside a tick getter and inside an
// observe's free; a vNext close reentering charge-candidate pruning; a reentrant observation revoking
// the in-flight feature-20 ticket (then a reenable); a genuine foreign std::thread that can only
// disarm; and the vNext charge target relocating during its charged-getter. gc_free counts any
// double / out-of-range free (g_bad_free) and every injection scenario asserts it stayed zero and
// all roots balanced. No test claims are made about having run this; it is NOT built into the app.
#include "rocket_combat.h"
#include "rocket_actions.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#ifndef PGR_RC_DIRECT_TEST
#include "pgrazor_native_api.h"
#include <dlfcn.h>
static const pgrazor_native_api_v2 *loaded_core = nullptr;
#endif

namespace {

// ---- symbolic il2cpp identities (compared by address only) ------------------------
int m_director, m_active, m_fast, m_charge, m_visible, m_interactable, m_shield;
int vm_blocked, vm_charged, vm_visible, vm_charge, vm_fast, m_alive;
int f_first, f_second, f_charged, f_input, f_shields, f_button;
int vf_gesture, vf_enabled, vf_tap, vf_button, f_current_state;
int c_rocket, c_rui, c_rdir, c_rcharge, c_rshield, c_rvui, c_rvgest, c_rvcharge, c_bool, c_sel, c_tap;

struct Obj {
  void   *klass = nullptr;
  int     alive = 1, active = 1, interactable = 1, blocked = 0;
  uint8_t charged = 1, input = 1, visible = 1, vcharged = 1, vvisible = 1, enabled = 1;
  int32_t currentState = 2, shields = 1;
  void   *first = nullptr, *second = nullptr, *director = nullptr, *button = nullptr;
  void   *gesture = nullptr, *tap = nullptr, *fastResult = nullptr;
  int     tapped = 0, shieldClicked = 0, fastTried = 0;
};
inline Obj *O(void *o) { return static_cast<Obj *>(o); }

struct Box { void *klass; uint8_t v; };
Box g_box{};

// ---- managed-call fault + move injection ------------------------------------------
void *g_fault_method = nullptr;  // runtime_invoke raises on this MethodInfo*
void *g_move_method  = nullptr;  // on this invoke, relocate g_move_obj -> g_move_dest (once)
void *g_move_obj     = nullptr;
Obj   g_move_dest{};

// ---- handle table (a move updates the slot; handle stays valid) --------------------
constexpr int kSlots = 512;
void *g_slot[kSlots];
bool  g_used[kSlots];
int   g_live = 0;
bool  g_new_fail = false;
int   g_bad_free = 0;  // non-null handle freed twice / out of range -> a controller lifetime bug

// ---- bounded reentrancy injection at gc_new / gc_get / gc_free / runtime_invoke -----
// A scenario arms g_cb to re-enter the controller from inside one of these primitives
// (exactly the window a real GC callback or a game thread could reenter). g_in_cb is a
// one-level recursion guard: the reentrant call's own primitive hits do NOT re-fire.
// Each callback is self-bounded via g_cb_done. g_cb_arg passes the obj (NEW), the
// resolved target (GET), the handle (FREE) or the MethodInfo* (INVOKE).
enum { SITE_NEW, SITE_GET, SITE_FREE, SITE_INVOKE };
struct Scene;
void (*g_cb)(int, void *) = nullptr;
bool      g_in_cb = false;
int       g_cb_done = 0;
uint64_t  g_cb_tid = 0;
Scene    *g_scene = nullptr;
void fire(int site, void *arg) {
  if (!g_cb || g_in_cb) return;
  g_in_cb = true; g_cb(site, arg); g_in_cb = false;
}

void *gc_new(void *o, int) {
  if (!o || g_new_fail) return nullptr;
  for (int i = 0; i < kSlots; ++i)
    if (!g_used[i]) { g_used[i] = true; g_slot[i] = o; ++g_live; void *h = &g_slot[i]; fire(SITE_NEW, o); return h; }
  return nullptr;
}
void *gc_get(void *h) { void *t = h ? *static_cast<void **>(h) : nullptr; fire(SITE_GET, t); return t; }
void  gc_free(void *h) {
  if (!h) return;
  int i = static_cast<int>(static_cast<void **>(h) - g_slot);
  if (i >= 0 && i < kSlots && g_used[i]) { g_used[i] = false; g_slot[i] = nullptr; --g_live; }
  else ++g_bad_free;  // double / invalid free: assert, never silently ignore
  fire(SITE_FREE, h);
}
void *obj_class(void *o) { return o ? *static_cast<void **>(o) : nullptr; }
void *obj_unbox(void *o) { return &static_cast<Box *>(o)->v; }
void  field_get(void *o, void *f, void *out) {
  Obj *x = O(o);
  if (f == &f_first)        *static_cast<void **>(out) = x->first;
  else if (f == &f_second)  *static_cast<void **>(out) = x->second;
  else if (f == &f_button || f == &vf_button) *static_cast<void **>(out) = x->button;
  else if (f == &vf_gesture) *static_cast<void **>(out) = x->gesture;
  else if (f == &vf_tap)    *static_cast<void **>(out) = x->tap;
  else if (f == &f_charged) *static_cast<uint8_t *>(out) = x->charged;
  else if (f == &f_input)   *static_cast<uint8_t *>(out) = x->input;
  else if (f == &vf_enabled) *static_cast<uint8_t *>(out) = x->enabled;
  else if (f == &f_shields) *static_cast<int32_t *>(out) = x->shields;
  else if (f == &f_current_state) *static_cast<int32_t *>(out) = x->currentState;
}

void do_move(void *obj) {
  std::memcpy(&g_move_dest, obj, sizeof(Obj));
  for (int i = 0; i < kSlots; ++i) if (g_used[i] && g_slot[i] == obj) g_slot[i] = &g_move_dest;
}
void *box_bool(int v) { g_box.klass = &c_bool; g_box.v = static_cast<uint8_t>(v ? 1 : 0); return &g_box; }

void *rt_invoke(void *method, void *obj, void **params, void **exc) {
  *exc = nullptr;
  fire(SITE_INVOKE, method);
  if (method == g_fault_method) { *exc = reinterpret_cast<void *>(1); return nullptr; }
  if (method == g_move_method && g_move_obj) { g_move_method = nullptr; do_move(g_move_obj); }
  if (method == &m_alive)        { void *o = params ? params[0] : nullptr; return box_bool(o ? O(o)->alive : 0); }
  if (method == &m_active)       return box_bool(O(obj)->active);
  if (method == &m_interactable) return box_bool(O(obj)->interactable);
  if (method == &m_visible)      return box_bool(O(obj)->visible);
  if (method == &vm_blocked)     return box_bool(O(obj)->blocked);
  if (method == &vm_charged)     return box_bool(O(obj)->vcharged);
  if (method == &vm_visible)     return box_bool(O(obj)->vvisible);
  if (method == &m_director)     return O(obj)->director;
  if (method == &m_fast)         { O(obj)->fastTried++; return O(obj)->fastResult; }
  if (method == &m_charge)       { O(obj)->tapped++; return nullptr; }
  if (method == &vm_charge)      { O(obj)->tapped++; return nullptr; }
  if (method == &vm_fast)        { O(obj)->tapped++; return nullptr; }
  if (method == &m_shield)       { O(obj)->shieldClicked++; return nullptr; }
  return nullptr;
}

const pgr_rocket_combat_api_v1 *api = nullptr;

void reset_runtime() {
  std::memset(g_used, 0, sizeof(g_used)); std::memset(g_slot, 0, sizeof(g_slot));
  g_live = 0; g_new_fail = false; g_fault_method = nullptr; g_move_method = nullptr; g_move_obj = nullptr;
  g_bad_free = 0; g_cb = nullptr; g_in_cb = false; g_cb_done = 0; g_cb_tid = 0; g_scene = nullptr;
}

void fill_config(pgr_rocket_combat_config_v1 *c) {
  std::memset(c, 0, sizeof(*c));
  c->version = 1; c->size = sizeof(*c);
  c->gchandle_new = reinterpret_cast<void *>(&gc_new);
  c->gchandle_get_target = reinterpret_cast<void *>(&gc_get);
  c->gchandle_free = reinterpret_cast<void *>(&gc_free);
  c->object_get_class = reinterpret_cast<void *>(&obj_class);
  c->object_unbox = reinterpret_cast<void *>(&obj_unbox);
  c->runtime_invoke = reinterpret_cast<void *>(&rt_invoke);
  c->field_get_value = reinterpret_cast<void *>(&field_get);
  c->m_director = &m_director; c->m_active = &m_active; c->m_fast = &m_fast; c->m_charge = &m_charge;
  c->m_visible = &m_visible; c->m_interactable = &m_interactable; c->m_shield = &m_shield;
  c->vm_blocked = &vm_blocked; c->vm_charged = &vm_charged; c->vm_visible = &vm_visible;
  c->vm_charge = &vm_charge; c->vm_fast = &vm_fast; c->m_alive = &m_alive;
  c->f_first = &f_first; c->f_second = &f_second; c->f_charged = &f_charged; c->f_input = &f_input;
  c->f_shields = &f_shields; c->f_button = &f_button;
  c->vf_gesture = &vf_gesture; c->vf_enabled = &vf_enabled; c->vf_tap = &vf_tap; c->vf_button = &vf_button;
  c->f_current_state = &f_current_state;
  c->class_rocket = &c_rocket; c->class_rui = &c_rui; c->class_rdir = &c_rdir; c->class_rcharge = &c_rcharge;
  c->class_rshield = &c_rshield; c->class_rvui = &c_rvui; c->class_rvgest = &c_rvgest;
  c->class_rvcharge = &c_rvcharge; c->class_bool = &c_bool;
  c->state_open = 2;
}

// Fully-wired fake scene (fresh per scenario / fork).
struct Scene {
  Obj scope, scope2, ui, ui2, ui3, dir, dir2, charge1, charge2, shield, button, vui, vgest, vcharge, vbutton, tap;
  Obj vc2, vc3, vc4, vc5;
  Scene() {
    scope.klass = &c_rocket; scope2.klass = &c_rocket;
    ui.klass = &c_rui; ui2.klass = &c_rui; ui3.klass = &c_rui; dir.klass = &c_rdir; dir2.klass = &c_rdir;
    charge1.klass = &c_rcharge; charge2.klass = &c_rcharge; shield.klass = &c_rshield; button.klass = &c_sel;
    vui.klass = &c_rvui; vgest.klass = &c_rvgest; vcharge.klass = &c_rvcharge; vbutton.klass = &c_sel; tap.klass = &c_tap;
    vc2.klass = vc3.klass = vc4.klass = vc5.klass = &c_rvcharge;
    ui.director = &dir; ui.first = &charge1; ui.second = &charge2;
    ui2.director = &dir2; dir2.active = 0;
    ui3.director = &dir;  // ui3 shares the active director so a replacement win can fire
    dir.active = 1; dir.fastResult = &dir;  // non-null -> accepted
    shield.button = &button; shield.currentState = 2; shield.shields = 1;
    vui.gesture = &vgest; vgest.tap = &tap; vgest.enabled = 1; vcharge.button = &vbutton;
  }
};

void arm(void *scope, uint32_t mask, uint64_t tid) {
  api->update(mask, 2500, tid);
  api->tick(tid, 0);  // latch the Unity identity before any observation is honoured
  api->observe(PGR_RC_OBS_SCOPE_ENTER, scope, 0, tid);
  api->update(mask, 2500, tid);
}
int32_t gate_now() { pgr_rocket_combat_sample_v1 s{}; api->sample(&s); return s.gate; }
uint32_t roots_now() { pgr_rocket_combat_sample_v1 s{}; api->sample(&s); return s.owned_roots; }
uint32_t scope_now() { pgr_rocket_combat_sample_v1 s{}; api->sample(&s); return s.scope_active; }
uint32_t failed_now() { pgr_rocket_combat_sample_v1 s{}; api->sample(&s); return s.failed; }
uint32_t charges_now() { pgr_rocket_combat_sample_v1 s{}; api->sample(&s); return s.charges; }
uint32_t uiact_now() { pgr_rocket_combat_sample_v1 s{}; api->sample(&s); return s.ui_active; }

// ---- bounded reentrancy callbacks (one shot each via g_cb_done) ---------------------
// A newer SCOPE_ENTER fires while the first scope is still being rooted in gc_new.
void cb_scope_new(int site, void *arg) {
  if (site == SITE_NEW && arg == &g_scene->scope && !g_cb_done) {
    g_cb_done = 1; api->observe(PGR_RC_OBS_SCOPE_ENTER, &g_scene->scope2, 0, g_cb_tid);
  }
}
void cb_scope_free(int site,void *){
  if(site==SITE_FREE&&!g_cb_done){g_cb_done=1;api->observe(PGR_RC_OBS_SCOPE_ENTER,&g_scene->scope2,0,g_cb_tid);}
}
// A newer UI_UPDATE fires while the old legacy ui handle is being freed.
void cb_ui_free(int site, void *) {
  if (site == SITE_FREE && !g_cb_done) {
    g_cb_done = 1; api->observe(PGR_RC_OBS_UI_UPDATE, &g_scene->ui3, 0, g_cb_tid);
  }
}
// A STOP (and a rejected rearm) fires while the scope handle is being dereferenced in a tick.
void cb_stop_get(int site, void *arg) {
  if (site == SITE_GET && arg == &g_scene->scope && !g_cb_done) {
    g_cb_done = 1; api->tick(g_cb_tid, 1); api->update(1, 2500, g_cb_tid);
  }
}
// A STOP (and a rejected rearm) fires while the old ui handle is being freed in an observe.
void cb_stop_free(int site, void *) {
  if (site == SITE_FREE && !g_cb_done) {
    g_cb_done = 1; api->tick(g_cb_tid, 1); api->update(1, 2500, g_cb_tid);
  }
}
// A VNEXT_CLOSE fires while charge_capture is probing candidate liveness (its first m_alive).
void cb_vclose_prune(int site, void *arg) {
  if (site == SITE_INVOKE && arg == &m_alive && !g_cb_done) {
    g_cb_done = 1; api->observe(PGR_RC_OBS_VNEXT_CLOSE, &g_scene->vui, 0, g_cb_tid);
  }
}
// A reentrant observation fires during the tick's director getter (revokes the in-flight ticket).
void cb_revoke(int site, void *arg) {
  if (site == SITE_INVOKE && arg == &m_active && !g_cb_done) {
    g_cb_done = 1; api->observe(PGR_RC_OBS_UI_UPDATE, &g_scene->ui, 0, g_cb_tid);
  }
}

int scenario(int n) {
  reset_runtime();
  const uint64_t tid = static_cast<uint64_t>(gettid());
  Scene sc;
  switch (n) {
    case 0:  // legacy quick
      arm(&sc.scope, 1, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      api->tick(tid, 0);
      if (sc.dir.fastTried != 1) return 1;
      break;
    case 1:  // legacy charged (first candidate)
      arm(&sc.scope, 2, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      api->tick(tid, 0);
      if (sc.charge1.tapped != 1 || sc.charge2.tapped != 0) return 2;
      break;
    case 2:  // shield once, then manual lock-out
      arm(&sc.scope, 4, tid);
      api->observe(PGR_RC_OBS_SHIELD_PROMPT, &sc.shield, 1, tid);
      api->tick(tid, 0);
      if (sc.shield.shieldClicked != 1) return 3;
      api->update(4, 2500, tid); api->tick(tid, 0);
      if (sc.shield.shieldClicked != 1) return 4;  // shield_used blocks a second press
      break;
    case 3:  // vNext quick
      arm(&sc.scope, 1, tid);
      api->observe(PGR_RC_OBS_VNEXT_UPDATE, &sc.vui, 0, tid);
      api->tick(tid, 0);
      if (sc.vgest.tapped != 1) return 5;
      break;
    case 4:  // vNext charged
      arm(&sc.scope, 2, tid);
      api->observe(PGR_RC_OBS_VNEXT_UPDATE, &sc.vui, 0, tid);
      api->observe(PGR_RC_OBS_CHARGE, &sc.vcharge, 0, tid);
      api->tick(tid, 0);
      if (sc.vcharge.tapped != 1) return 6;
      break;
    case 5:  // battle-not-active gate, no action
      arm(&sc.scope, 1, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      sc.dir.active = 0;
      api->tick(tid, 0);
      if (sc.dir.fastTried != 0 || gate_now() != PGR_RC_GATE_INACTIVE) return 7;
      break;
    case 6:  // scope exit clears + balanced roots
      arm(&sc.scope, 1, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      if (scope_now() != 1) return 8;
      api->observe(PGR_RC_OBS_SCOPE_EXIT, &sc.scope, 0, tid);
      if (scope_now() != 0 || roots_now() != 0 || g_live != 0) return 9;
      break;
    case 7:  // STOP clears and rejects rearm
      arm(&sc.scope, 1, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      api->tick(tid, 1);  // stop
      if (scope_now() != 0 || g_live != 0) return 10;
      api->update(1, 2500, tid); api->tick(tid, 0);
      if (sc.dir.fastTried != 0) return 11;  // no scope/ui after the stop clear
      break;
    case 8:  // shield not open -> no fire (falls through)
      arm(&sc.scope, 4, tid);
      sc.shield.currentState = 5;  // neither STATE_OPEN(2) nor closed(3)
      api->observe(PGR_RC_OBS_SHIELD_PROMPT, &sc.shield, 1, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      api->tick(tid, 0);
      if (sc.shield.shieldClicked != 0) return 12;
      break;
    case 9: {  // paired UI: stale onLeave must not overwrite a live replacement
      arm(&sc.scope, 1, tid);
      uint64_t token = api->begin_ui(&sc.ui, tid);
      if (!token) return 13;
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui2, 0, tid);  // live replacement supersedes the pending
      api->end_ui(token, &sc.ui, tid);                      // stale -> must NOT publish sc.ui
      api->tick(tid, 0);
      if (sc.dir.fastTried != 0) return 14;  // live ui is ui2 (dir2 inactive) -> no fire on dir
      // a clean begin/end with no intervening update publishes and fires
      uint64_t t2 = api->begin_ui(&sc.ui, tid);
      api->end_ui(t2, &sc.ui, tid);
      api->update(1, 2500, tid); api->tick(tid, 0);
      if (sc.dir.fastTried != 1) return 15;
      break;
    }
    case 10:  // charge dedupe / cap / prune
      arm(&sc.scope, 2, tid);
      api->observe(PGR_RC_OBS_VNEXT_UPDATE, &sc.vui, 0, tid);
      api->observe(PGR_RC_OBS_CHARGE, &sc.vcharge, 0, tid);
      api->observe(PGR_RC_OBS_CHARGE, &sc.vcharge, 0, tid);  // dedupe
      if (charges_now() != 1) return 16;
      api->observe(PGR_RC_OBS_CHARGE, &sc.vc2, 0, tid);
      api->observe(PGR_RC_OBS_CHARGE, &sc.vc3, 0, tid);
      api->observe(PGR_RC_OBS_CHARGE, &sc.vc4, 0, tid);
      api->observe(PGR_RC_OBS_CHARGE, &sc.vc5, 0, tid);  // over cap -> dropped
      if (charges_now() != 4) return 17;
      sc.vcharge.alive = 0;  // kill a tracked candidate
      api->observe(PGR_RC_OBS_CHARGE, &sc.vc2, 0, tid);  // capture prunes the dead one first
      if (charges_now() != 3) return 18;
      break;
    case 11:  // managed-invoke fault latch
      arm(&sc.scope, 1, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      g_fault_method = &m_active;
      api->tick(tid, 0);
      if (sc.dir.fastTried != 0 || failed_now() != 1) return 19;
      g_fault_method = nullptr;  // fault cleared but the latch persists until scope clear
      api->update(1, 2500, tid); api->tick(tid, 0);
      if (sc.dir.fastTried != 0) return 20;
      break;
    case 12:  // wrong OS thread tick is a no-op
      arm(&sc.scope, 1, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      api->tick(tid + 1, 0);  // proven_tid != live tid
      if (sc.dir.fastTried != 0) return 21;
      api->update(1, 2500, tid); api->tick(tid, 0);
      if (sc.dir.fastTried != 1) return 22;
      break;
    case 13:  // feature-20 lease expiry
      api->update(1, 10, tid); api->tick(tid, 0);
      api->observe(PGR_RC_OBS_SCOPE_ENTER, &sc.scope, 0, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      api->update(1, 10, tid);
      usleep(30000);
      api->tick(tid, 0);
      if (sc.dir.fastTried != 0) return 23;
      break;
    case 14:  // feature-20 cadence
      arm(&sc.scope, 1, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      api->tick(tid, 0);
      if (sc.dir.fastTried != 1) return 24;
      api->update(1, 2500, tid); api->tick(tid, 0);
      if (sc.dir.fastTried != 1) return 25;  // within the 150 ms quick cadence
      usleep(170000);
      api->update(1, 2500, tid); api->tick(tid, 0);
      if (sc.dir.fastTried != 2) return 26;
      break;
    case 15:  // director rooted across a managed call that MOVES, refreshed at use
      arm(&sc.scope, 1, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      g_move_method = &m_active; g_move_obj = &sc.dir;  // dir relocates during get_IsInActiveState
      api->tick(tid, 0);
      if (g_move_dest.fastTried != 1 || sc.dir.fastTried != 0) return 27;
      break;
    case 16: {  // newer scope supersedes the first inside its gc_new root; preserved, then STOP clears
      g_scene = &sc; g_cb_tid = tid;
      api->update(1, 2500, tid); api->tick(tid, 0);  // latch the owner first (no scope yet)
      g_cb = &cb_scope_new;
      api->observe(PGR_RC_OBS_SCOPE_ENTER, &sc.scope, 0, tid);  // scope2 wins reentrantly in gc_new
      g_cb = nullptr;
      if (scope_now() != 1 || g_bad_free) return 28;
      api->observe(PGR_RC_OBS_SCOPE_EXIT, &sc.scope, 0, tid);   // stale scope must NOT clear the newer
      if (scope_now() != 1 || roots_now() == 0) return 29;      // newer scope preserved (nonzero roots)
      api->tick(tid, 1);                                        // STOP drains the preserved scope
      if (scope_now() != 0 || roots_now() != 0 || g_live != 0 || g_bad_free) return 30;
      break;
    }
    case 17: {  // a newer UI_UPDATE wins while the superseded legacy ui handle is being freed
      g_scene = &sc; g_cb_tid = tid;
      arm(&sc.scope, 1, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      g_cb = &cb_ui_free;
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui2, 0, tid);  // during the old-ui free, ui3 supersedes
      g_cb = nullptr;
      if (uiact_now() != 1 || g_bad_free) return 31;
      api->update(1, 2500, tid); api->tick(tid, 0);         // ui3 is the live ui (shares active dir)
      if (sc.dir.fastTried != 1) return 32;
      api->observe(PGR_RC_OBS_UI_CLOSE, &sc.ui2, 0, tid);   // stale replacement -> no-op
      if (uiact_now() != 1) return 33;
      api->observe(PGR_RC_OBS_UI_CLOSE, &sc.ui3, 0, tid);   // live -> drop
      if (uiact_now() != 0) return 34;
      api->observe(PGR_RC_OBS_SCOPE_EXIT, &sc.scope, 0, tid);
      if (roots_now() != 0 || g_live != 0 || g_bad_free) return 35;
      break;
    }
    case 18: {  // stale begin_ui token dropped; newest publishes; replay after publish is inert
      arm(&sc.scope, 1, tid);
      uint64_t t1 = api->begin_ui(&sc.ui, tid);
      uint64_t t2 = api->begin_ui(&sc.ui, tid);  // supersedes t1 (its pending root freed)
      if (!t1 || !t2 || t1 == t2) return 36;
      api->end_ui(t1, &sc.ui, tid);              // stale token -> dropped, nothing published
      api->update(1, 2500, tid); api->tick(tid, 0);
      if (sc.dir.fastTried != 0 || uiact_now() != 0) return 37;
      api->end_ui(t2, &sc.ui, tid);              // newest token -> publishes the live ui
      api->tick(tid, 0);
      if (sc.dir.fastTried != 1) return 38;
      api->end_ui(t2, &sc.ui, tid);              // replay after publish -> inert
      api->tick(tid, 0);
      if (sc.dir.fastTried != 1 || g_bad_free) return 39;  // within quick cadence -> unchanged
      api->observe(PGR_RC_OBS_SCOPE_EXIT, &sc.scope, 0, tid);
      if (roots_now() != 0 || g_live != 0 || g_bad_free) return 40;
      break;
    }
    case 19: {  // UI_CLOSE before the matching end_ui drops the pending root; end_ui is then inert
      arm(&sc.scope, 1, tid);
      uint64_t t = api->begin_ui(&sc.ui, tid);
      if (!t) return 41;
      api->observe(PGR_RC_OBS_UI_CLOSE, &sc.ui, 0, tid);  // close arrives before onLeave
      api->end_ui(t, &sc.ui, tid);                        // pending already gone -> no publish
      api->update(1, 2500, tid); api->tick(tid, 0);
      if (sc.dir.fastTried != 0 || uiact_now() != 0 || g_bad_free) return 42;
      api->observe(PGR_RC_OBS_SCOPE_EXIT, &sc.scope, 0, tid);
      if (roots_now() != 0 || g_live != 0 || g_bad_free) return 43;
      break;
    }
    case 20: {  // STOP (deferred) + a rejected rearm fire while a tick dereferences the scope handle
      g_scene = &sc; g_cb_tid = tid;
      arm(&sc.scope, 1, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      g_cb = &cb_stop_get;
      api->tick(tid, 0);  // getter hit -> STOP injected; cleared on the outermost frame
      g_cb = nullptr;
      if (sc.dir.fastTried != 0 || scope_now() != 0 || roots_now() != 0 || g_live != 0 || g_bad_free) return 44;
      arm(&sc.scope, 1, tid);  // the rearm during the stop was rejected -> a fresh scope is required
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      api->tick(tid, 0);
      if (sc.dir.fastTried != 1) return 45;
      api->observe(PGR_RC_OBS_SCOPE_EXIT, &sc.scope, 0, tid);
      if (roots_now() != 0 || g_live != 0 || g_bad_free) return 46;
      break;
    }
    case 21: {  // STOP (deferred) + a rejected rearm fire while the superseded ui handle is freed
      g_scene = &sc; g_cb_tid = tid;
      arm(&sc.scope, 1, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      g_cb = &cb_stop_free;
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui2, 0, tid);  // mid old-ui free -> STOP injected
      g_cb = nullptr;
      if (scope_now() != 0 || roots_now() != 0 || g_live != 0 || g_bad_free) return 47;
      break;
    }
    case 22: {  // VNEXT_CLOSE reenters while charge_capture probes liveness; no leak / no double free
      g_scene = &sc; g_cb_tid = tid;
      arm(&sc.scope, 2, tid);
      api->observe(PGR_RC_OBS_VNEXT_UPDATE, &sc.vui, 0, tid);
      api->observe(PGR_RC_OBS_CHARGE, &sc.vcharge, 0, tid);
      api->observe(PGR_RC_OBS_CHARGE, &sc.vc2, 0, tid);
      if (charges_now() != 2) return 48;
      g_cb = &cb_vclose_prune;
      api->observe(PGR_RC_OBS_CHARGE, &sc.vc3, 0, tid);  // prune probe -> VNEXT_CLOSE drops vui+charges
      g_cb = nullptr;
      if (charges_now() != 0 || uiact_now() != 0 || g_live != 1 || g_bad_free) return 49;
      api->observe(PGR_RC_OBS_SCOPE_EXIT, &sc.scope, 0, tid);
      if (roots_now() != 0 || g_live != 0 || g_bad_free) return 50;
      break;
    }
    case 23: {  // a reentrant observation during the director getter revokes the ticket; reenable restores
      g_scene = &sc; g_cb_tid = tid;
      arm(&sc.scope, 1, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      g_cb = &cb_revoke;
      api->tick(tid, 0);  // reentrant observe during get_IsInActiveState -> action cancelled
      g_cb = nullptr;
      if (sc.dir.fastTried != 0 || g_bad_free) return 51;
      api->update(1, 2500, tid); api->tick(tid, 0);  // reenable -> action proceeds
      if (sc.dir.fastTried != 1) return 52;
      api->observe(PGR_RC_OBS_SCOPE_EXIT, &sc.scope, 0, tid);
      if (roots_now() != 0 || g_live != 0 || g_bad_free) return 53;
      break;
    }
    case 24: {  // a genuine foreign std::thread can only disarm; its tick never acts nor latches
      arm(&sc.scope, 1, tid);
      api->observe(PGR_RC_OBS_UI_UPDATE, &sc.ui, 0, tid);
      std::thread th([tid] {
        api->update(1, 2500, tid);  // foreign OS thread with the owner tid -> wrong-thread disarm
        api->tick(tid, 0);          // foreign tick -> no-op (live tid != proven tid)
      });
      th.join();
      api->tick(tid, 0);  // owner tick, but the mask was disarmed by the foreign update
      if (sc.dir.fastTried != 0) return 54;
      api->update(1, 2500, tid); api->tick(tid, 0);  // owner rearms -> acts
      if (sc.dir.fastTried != 1) return 55;
      api->observe(PGR_RC_OBS_SCOPE_EXIT, &sc.scope, 0, tid);
      if (roots_now() != 0 || g_live != 0 || g_bad_free) return 56;
      break;
    }
    case 25: {  // the vNext charge target RELOCATES during its charged-getter; the final tap hits the moved obj
      arm(&sc.scope, 2, tid);
      api->observe(PGR_RC_OBS_VNEXT_UPDATE, &sc.vui, 0, tid);
      api->observe(PGR_RC_OBS_CHARGE, &sc.vcharge, 0, tid);
      g_move_method = &vm_charged; g_move_obj = &sc.vcharge;  // relocate on get_IsCharged
      api->tick(tid, 0);
      if (g_move_dest.tapped != 1 || sc.vcharge.tapped != 0 || g_bad_free) return 57;
      api->observe(PGR_RC_OBS_SCOPE_EXIT, &sc.scope, 0, tid);
      if (roots_now() != 0 || g_live != 0 || g_bad_free) return 58;
      break;
    }
    case 26: {
      g_scene=&sc;g_cb_tid=tid;arm(&sc.scope,1,tid);
      api->observe(PGR_RC_OBS_UI_UPDATE,&sc.ui,0,tid);
      Obj incoming;incoming.klass=&c_rocket;g_cb=&cb_scope_free;
      api->observe(PGR_RC_OBS_SCOPE_ENTER,&incoming,0,tid);g_cb=nullptr;
      if(!g_cb_done||scope_now()!=1||g_live!=1||g_bad_free)return 59;
      api->observe(PGR_RC_OBS_SCOPE_EXIT,&incoming,0,tid);
      if(scope_now()!=1)return 60;
      api->observe(PGR_RC_OBS_SCOPE_EXIT,&sc.scope2,0,tid);
      break;
    }
  }
  g_cb=nullptr;api->tick(tid,1);
  if(roots_now()!=0||g_live!=0||g_bad_free)return 90;
  return 0;
}

constexpr int kScenarios = 27;

}  // namespace

int main(int argc, char **argv) {
#ifdef PGR_RC_DIRECT_TEST
  (void)argc; (void)argv;
  api = static_cast<const pgr_rocket_combat_api_v1 *>(pgrazor_rocket_combat_api(1));
  if (!api || pgrazor_rocket_combat_api(99) != nullptr) { std::puts("FAIL exact-version"); return 2; }
#else
  if (argc != 2) return 80;
  void *lib = dlopen(argv[1], RTLD_NOW); if (!lib) return 81;
  auto get = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib, "pgrazor_native_api"));
  loaded_core = get ? static_cast<const pgrazor_native_api_v2 *>(get(2)) : nullptr;
  if (!loaded_core || loaded_core->version != 2 || !loaded_core->query_feature) return 82;
  api = static_cast<const pgr_rocket_combat_api_v1 *>(loaded_core->query_feature(21, 1));
  if (!api || api->size != 72 || loaded_core->query_feature(21, 99)) return 83;
#endif

  pgr_rocket_combat_config_v1 cfg;
  fill_config(&cfg);
  reset_runtime();
  int rc = api->install(&cfg);
  if (rc != PGR_RC_OK) { std::printf("FAIL install %d\n", rc); return 3; }
  // Idempotent re-install of the identical config; a mutated one is refused.
  if (api->install(&cfg) != PGR_RC_OK) { std::puts("FAIL reinstall"); return 4; }
  pgr_rocket_combat_config_v1 bad = cfg; bad.state_open = PGR_RC_CLOSED_STATE;
  if (api->install(&bad) != PGR_RC_ERR_BAD_CONFIG) { std::puts("FAIL bad-config"); return 5; }

  for (int n = 0; n < kScenarios; ++n) {
    pid_t pid = fork();
    if (pid == 0) _exit(scenario(n));
    int status = 0; waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
      std::printf("FAIL Rocket combat %d code %d\n", n, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
      return 1;
    }
  }
  std::printf("PASS %d native Rocket combat cases: legacy/vNext quick+charged, shield once/lockout, "
              "gates, scope-exit balanced roots, STOP reject, shield-not-open fall-through, "
              "generation-safe paired UI, charge dedupe/cap/prune, fault latch, wrong-thread, "
              "feature-20 lease/cadence, moved-target refresh; reentrancy-injection: scope wins in "
              "gc_new + STOP drain, ui wins in old free, stale/replay begin/end_ui, close-before-leave, "
              "STOP in getter/free + rearm reject, vNext-close in charge prune, ticket revoke/reenable, "
              "foreign-thread disarm, moved vNext charge target (zero leaked/double-freed roots)\n",
              kScenarios);
  return 0;
}

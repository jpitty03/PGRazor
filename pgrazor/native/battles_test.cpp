// Standalone focused test of the native battle controller (feature 19). Like the hook-less berry
// sibling, this module installs NO Dobby/Gum hooks. By default the harness loads the compiled
// library and queries feature 19; PGR_BATTLE_DIRECT_TEST allows isolated direct linking.
// It drives a compact fake IL2CPP (runtime_invoke over the battle methods, boxed
// bool returns, field get) through a proper handle table (a "GC move" relocates an object while
// its handle stays valid) and forks per scenario for clean process-lifetime state.
//
// Coverage includes real CLOCK_BOOTTIME expiry and a second OS thread:
//   all modes quick gating + both input ABIs (raid no-args vs gym/Max sender+null EventArgs);
//   charged (vNext + legacy) and Max attack (excludes other moves); Rocket exclusion;
//   Active/Unset/terminal BattleStatus; dodge arm + both dodge ABIs (modern Left value vs legacy
//   flick+null); manual-swap block + scope-exit root release; scope-owned Dynamax gesture recovery
//   (+ inactive yield); STOP clear with balanced roots; wrong-thread no-op; gesture disable/re-arm;
//   a reentrant scope-exit fired from inside a managed getter (stale action cancelled, no clear of
//   newer state, no crash); stale/replayed gym token; a moving rooted gesture refreshed at use;
//   bounded-FIFO overflow blocking actions until drained. No test claims are made about having run
//   this; it is NOT built into the app.
#include "battles.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#ifndef PGR_BATTLE_DIRECT_TEST
#include "pgrazor_native_api.h"
#include <dlfcn.h>
static const pgrazor_native_api_v2 *loaded_core=nullptr;
extern "C" const void *pgrazor_battles_api(uint32_t version){return loaded_core?loaded_core->query_feature(19,version):nullptr;}
#endif

namespace {

// ---- symbolic il2cpp identities (compared by address only) ------------------------
int m_current_state, m_active, m_press, m_interactable, m_alive, m_dodge, m_legacy_dodge,
    m_raid_fast, m_max_fast, m_legacy_fast, m_legacy_charge, m_legacy_charged, m_vnext_charge,
    m_vnext_charged, m_vnext_visible, m_swap_show;
int f_director, f_battle_status, f_max_gesture, f_legacy_flick, f_legacy_tap, f_legacy_button,
    f_max_button, f_max_type, f_vnext_enabled, f_vnext_tap, f_vnext_button, f_gym_warning_state;
int c_battlestate, c_legacy_charge, c_max_gesture, c_raid_gesture, c_legacy_gesture, c_max_widget,
    c_rvcharge, c_raid_warning, c_max_warning, c_gym_iterator, c_bool;
int c_scope0, c_scope1, c_scope2, c_swap0, c_swap1, c_swap2;

struct Obj {
  void   *klass = nullptr;
  bool    dead = false, active = true, interactable = true, charged = true, visible = true, open = true;
  void   *director = nullptr, *state = nullptr, *max_gesture = nullptr;
  void   *legacy_flick = nullptr, *legacy_tap = nullptr, *vnext_tap = nullptr;
  void   *legacy_button = nullptr, *max_button = nullptr, *vnext_button = nullptr;
  int32_t battle_status = 1, max_type = 0, gym_state = 0;
  uint8_t vnext_enabled = 1;
};
inline Obj *O(void *o) { return static_cast<Obj *>(o); }

struct Box { void *klass; union { uint8_t b; int32_t i; } v; };

// ---- handle table (a move updates the slot; handle stays valid) --------------------
constexpr int kSlots = 256;
void *g_slots[kSlots];
bool  g_slot_used[kSlots];
int   g_live_roots = 0;
void (*g_on_free)() = nullptr;
void (*g_on_new)() = nullptr;

void *gc_new(void *o, int) {
  if (!o) return nullptr;
  for (int i = 0; i < kSlots; ++i)
    if (!g_slot_used[i]) { g_slot_used[i] = true; g_slots[i] = o; ++g_live_roots;
      if(g_on_new){auto cb=g_on_new;g_on_new=nullptr;cb();} return &g_slots[i]; }
  return nullptr;
}
void *gc_target(void *h) { return h ? *static_cast<void **>(h) : nullptr; }
void  gc_free(void *h) {
  if (!h) return;
  void **slot = static_cast<void **>(h);
  for (int i = 0; i < kSlots; ++i)
    if (&g_slots[i] == slot && g_slot_used[i]) { g_slot_used[i] = false; g_slots[i] = nullptr; --g_live_roots;
      if(g_on_free){auto cb=g_on_free;g_on_free=nullptr;cb();} return; }
}
void gc_move(void *from, void *to) { for (int i = 0; i < kSlots; ++i) if (g_slot_used[i] && g_slots[i] == from) g_slots[i] = to; }

void *obj_class(void *o) { return o ? O(o)->klass : nullptr; }
void *object_unbox(void *o) { return &static_cast<Box *>(o)->v; }

// ---- scenario knobs / observations -------------------------------------------------
const pgr_battle_api_v1 *g_api = nullptr;
Box  g_box;
void *g_act_method = nullptr, *g_act_obj = nullptr, *g_act_p0 = nullptr, *g_act_p1 = nullptr;
bool g_act_params_null = false;
bool g_unrooted_sender = false;
int32_t g_act_dir = 0;
// Reentrancy injection: when set, the next m_active invoke fires observe(SCOPE_EXIT) on g_reenter_scope.
int   g_reenter = 0;
uint32_t g_test_mask=1;
void *g_reenter_scope = nullptr;

void *box_bool(bool v) { g_box.klass = &c_bool; g_box.v.b = v ? 1 : 0; return &g_box; }

void field_get(void *o, void *f, void *out) {
  if      (f == &f_director)      *static_cast<void **>(out) = O(o)->director;
  else if (f == &f_battle_status) *static_cast<int32_t *>(out) = O(o)->battle_status;
  else if (f == &f_max_gesture)   *static_cast<void **>(out) = O(o)->max_gesture;
  else if (f == &f_legacy_flick)  *static_cast<void **>(out) = O(o)->legacy_flick;
  else if (f == &f_legacy_tap)    *static_cast<void **>(out) = O(o)->legacy_tap;
  else if (f == &f_legacy_button) *static_cast<void **>(out) = O(o)->legacy_button;
  else if (f == &f_max_button)    *static_cast<void **>(out) = O(o)->max_button;
  else if (f == &f_max_type)      *static_cast<int32_t *>(out) = O(o)->max_type;
  else if (f == &f_vnext_enabled) *static_cast<uint8_t *>(out) = O(o)->vnext_enabled;
  else if (f == &f_vnext_tap)     *static_cast<void **>(out) = O(o)->vnext_tap;
  else if (f == &f_vnext_button)  *static_cast<void **>(out) = O(o)->vnext_button;
  else if (f == &f_gym_warning_state) *static_cast<int32_t *>(out) = O(o)->gym_state;
}

void record_action(void *method, void *obj, void **params) {
  g_act_method = method; g_act_obj = obj; g_act_params_null = (params == nullptr);
  g_act_p0 = g_act_p1 = nullptr; g_act_dir = 0;
  if (method == &m_dodge) { if (params) g_act_dir = *static_cast<int32_t *>(params[0]); }
  else if (method == &m_legacy_dodge || method == &m_max_fast || method == &m_legacy_fast) {
    if (params) { g_act_p0 = params[0]; g_act_p1 = params[1]; }
    bool rooted=false;for(int i=0;i<kSlots;++i)if(g_slot_used[i]&&g_slots[i]==g_act_p0)rooted=true;
    if(!rooted)g_unrooted_sender=true;
  }
}

void *runtime_invoke(void *method, void *obj, void **params, void **exc) {
  *exc = nullptr;
  if (method == &m_alive)        return box_bool(params && params[0] && !O(params[0])->dead);
  if (method == &m_active) {
    if (g_reenter == 1) { g_reenter = 0; g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, g_reenter_scope,
                                                        0, static_cast<uint64_t>(gettid())); }
    if(g_reenter==2) { g_reenter=0; g_api->update(0,0,0,gettid()); g_api->update(g_test_mask,2500,0,gettid()); }
    if(g_reenter==3) { g_reenter=0; g_api->tick(gettid(),1); }
    if(g_reenter==4) { g_reenter=0; usleep(30000); }
    if(g_reenter==5) { g_reenter=0; g_api->update(0,2500,0,gettid()); }
    return box_bool(obj && O(obj)->active && !O(obj)->dead);
  }
  if (method == &m_interactable)  return box_bool(obj && O(obj)->interactable);
  if (method == &m_legacy_charged || method == &m_vnext_charged) return box_bool(obj && O(obj)->charged);
  if (method == &m_vnext_visible) return box_bool(obj && O(obj)->visible);
  if (method == &m_swap_show)     return box_bool(obj && O(obj)->open);
  if (method == &m_current_state) return obj ? O(obj)->state : nullptr;
  // action / dodge methods return void.
  record_action(method, obj, params);
  return nullptr;
}

pgr_battle_config_v1 make_config() {
  pgr_battle_config_v1 c{};
  c.version = PGR_BATTLE_API_VERSION; c.size = sizeof(c);
  c.gchandle_new = (void *)&gc_new; c.gchandle_get_target = (void *)&gc_target; c.gchandle_free = (void *)&gc_free;
  c.object_get_class = (void *)&obj_class; c.object_unbox = (void *)&object_unbox;
  c.runtime_invoke = (void *)&runtime_invoke; c.field_get_value = (void *)&field_get;
  c.m_current_state = &m_current_state; c.m_active = &m_active; c.m_press = &m_press;
  c.m_interactable = &m_interactable; c.m_alive = &m_alive; c.m_dodge = &m_dodge;
  c.m_legacy_dodge = &m_legacy_dodge; c.m_raid_fast = &m_raid_fast; c.m_max_fast = &m_max_fast;
  c.m_legacy_fast = &m_legacy_fast; c.m_legacy_charge = &m_legacy_charge; c.m_legacy_charged = &m_legacy_charged;
  c.m_vnext_charge = &m_vnext_charge; c.m_vnext_charged = &m_vnext_charged; c.m_vnext_visible = &m_vnext_visible;
  c.f_director = &f_director; c.f_battle_status = &f_battle_status; c.f_max_gesture = &f_max_gesture;
  c.f_legacy_flick = &f_legacy_flick; c.f_legacy_tap = &f_legacy_tap; c.f_legacy_button = &f_legacy_button;
  c.f_max_button = &f_max_button; c.f_max_type = &f_max_type; c.f_vnext_enabled = &f_vnext_enabled;
  c.f_vnext_tap = &f_vnext_tap; c.f_vnext_button = &f_vnext_button; c.f_gym_warning_state = &f_gym_warning_state;
  c.class_battlestate = &c_battlestate; c.class_legacy_charge = &c_legacy_charge;
  c.class_max_gesture = &c_max_gesture; c.class_raid_gesture = &c_raid_gesture;
  c.class_legacy_gesture = &c_legacy_gesture; c.class_max_widget = &c_max_widget;
  c.class_rvcharge = &c_rvcharge; c.class_raid_warning = &c_raid_warning;
  c.class_max_warning = &c_max_warning; c.class_gym_iterator = &c_gym_iterator; c.class_bool = &c_bool;
  c.scopes[0] = {&c_scope0, 0, 0}; c.scopes[1] = {&c_scope1, 1, 0}; c.scopes[2] = {&c_scope2, 2, 0};
  c.swaps[0] = {&c_swap0, &m_swap_show, 1, 0};  // gym picker
  c.swaps[1] = {&c_swap1, &m_swap_show, 0, 0};  // raid picker
  c.swaps[2] = {&c_swap2, &m_swap_show, 2, 0};  // Dynamax picker
  c.status_active = 1; c.status_unset = 0; c.dodge_left = 7; c.max_attack = 5;
  return c;
}

pgr_battle_sample_v1 snap() { pgr_battle_sample_v1 s{}; g_api->sample(&s); return s; }
void arm(uint32_t mask, uint64_t tid, uint32_t rocket = 0) { g_api->update(mask, 2500, rocket, tid); }
void drain_all() { pgr_battle_event_v1 e{}; while (g_api->next_event(&e) == 1) {} }

// Wire a non-legacy gesture to a live Active battle director (so the status gate admits it).
void wire_active(Obj *gesture, Obj *director, Obj *state) {
  state->klass = &c_battlestate; state->battle_status = 1;  // Active
  director->state = state;
  gesture->director = director;
}

int scenario(int which) {
  const uint64_t tid = static_cast<uint64_t>(gettid());
  pgr_battle_config_v1 cfg = make_config();
  if (g_api->install(&cfg) != PGR_BATTLE_OK) return 90;
  g_api->tick(tid, 0);  // latch the Unity owner identity before any observation

  Obj director{}, state{};

  switch (which) {
  case 0: {  // raid quick: no-args ABI, cadence blocks the second same-instant tick
    Obj scope{&c_scope0}, g{&c_raid_gesture}, tap{};
    wire_active(&g, &director, &state); g.vnext_tap = &tap; g.vnext_enabled = 1;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);
    arm(1u << 0, tid);                              // raid quick bit
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != &m_raid_fast || !g_act_params_null) return 1;  // raid: params NULL
    uint64_t s1 = snap().action_serial;
    g_api->tick(tid, 0);                            // same CLOCK_BOOTTIME instant -> now < next
    if (snap().action_serial != s1) return 2;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 0, tid);
    if (g_live_roots != 0) return 3;
    return 0;
  }
  case 1: {  // gym quick: legacy ABI (sender=tap, EventArgs=null); legacy skips the status gate
    Obj scope{&c_scope1}, g{&c_legacy_gesture}, tap{};
    g.legacy_tap = &tap;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 1, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 1, tid);   // legacy=1
    arm(1u << 2, tid);                              // gym mode: policy bit (1*2+0)=2
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != &m_legacy_fast || g_act_params_null) return 1;
    if (g_act_p0 != &tap || g_act_p1 != nullptr) return 2;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 1, tid);
    if (g_live_roots != 0) return 3;
    return 0;
  }
  case 2: {  // Max quick: sender+null ABI
    Obj scope{&c_scope2}, g{&c_max_gesture}, tap{};
    wire_active(&g, &director, &state); g.vnext_tap = &tap;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 2, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);
    arm(1u << 4, tid);                              // Max mode: policy bit (2*2+0)=4
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != &m_max_fast || g_act_p0 != &tap || g_act_p1 != nullptr) return 1;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 2, tid);
    if (g_live_roots != 0) return 2;
    return 0;
  }
  case 3: {  // charged vNext: charged+visible+interactable button -> ChargeAttackButtonTapped
    Obj scope{&c_scope0}, g{&c_raid_gesture}, charge{&c_rvcharge}, button{};
    wire_active(&g, &director, &state);
    charge.charged = true; charge.visible = true; charge.vnext_button = &button;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_CHARGE, &charge, 0, tid);   // vNext RVCHARGE
    arm(1u << 1, tid);                              // raid charged bit (policy bit 1)
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != &m_vnext_charge || g_act_obj != &charge) return 1;
    charge.charged = false; g_act_method = nullptr; g_api->tick(tid, 0);  // not charged -> no action
    if (g_act_method != nullptr) return 2;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 0, tid);
    if (g_live_roots != 0) return 3;
    return 0;
  }
  case 4: {  // charged legacy (gym): Init-captured, no visibility requirement
    Obj scope{&c_scope1}, g{&c_legacy_gesture}, charge{&c_legacy_charge}, button{}, tap{};
    g.legacy_tap = &tap; charge.charged = true; charge.legacy_button = &button;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 1, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 1, tid);
    g_api->observe(PGR_BATTLE_OBS_CHARGE, &charge, 1, tid);   // legacy Init
    arm(1u << 3, tid);                              // gym charged (policy bit 1*2+1=3)
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != &m_legacy_charge || g_act_obj != &charge) return 1;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 1, tid);
    if (g_live_roots != 0) return 2;
    return 0;
  }
  case 5: {  // Max attack: BreadAttack button pressed; excludes other moves; wrong type yields
    Obj scope{&c_scope2}, g{&c_max_gesture}, widget{&c_max_widget}, button{};
    wire_active(&g, &director, &state);
    widget.max_type = 5; widget.max_button = &button;  // 5 == max_attack
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 2, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_MAX_REFRESH, &widget, 0, tid);
    arm(1u << 6, tid);                              // Max attack bit (policy bit 2*2+2=6)
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != &m_press || g_act_obj != &button) return 1;
    widget.max_type = 4; g_act_method = nullptr; g_api->tick(tid, 0);  // not BreadAttack
    if (g_act_method != nullptr) return 2;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 2, tid);
    if (g_live_roots != 0) return 3;
    return 0;
  }
  case 6: {  // Rocket exclusion suppresses all actions
    Obj scope{&c_scope0}, g{&c_raid_gesture}, tap{};
    wire_active(&g, &director, &state); g.vnext_tap = &tap;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);
    arm(1u << 0, tid, 1);                           // rocket_active = 1
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != nullptr) return 1;
    arm(1u << 0, tid, 0);                           // clear rocket -> action resumes
    g_api->tick(tid, 0);
    if (g_act_method != &m_raid_fast) return 2;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 0, tid);
    return 0;
  }
  case 7: {  // BattleStatus gate: Unset no-op (no clear), terminal -> battle-ended + clear
    Obj scope{&c_scope0}, g{&c_raid_gesture}, tap{};
    wire_active(&g, &director, &state); g.vnext_tap = &tap;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);
    arm(1u << 0, tid);
    state.battle_status = 0;                        // Unset
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != nullptr || snap().scope_active != 1) return 1;  // no action, scope kept
    state.battle_status = 1; g_api->tick(tid, 0);   // Active -> fire
    if (g_act_method != &m_raid_fast) return 2;
    state.battle_status = 3;                        // terminal
    g_api->tick(tid, 0);
    if (snap().scope_active != 0 || g_live_roots != 0) return 3;  // battle-ended + full clear
    return 0;
  }
  case 8: {  // dodge: modern Left value ABI, then post-dodge clears the window
    Obj scope{&c_scope0}, g{&c_raid_gesture}, warn{&c_raid_warning}, tap{};
    wire_active(&g, &director, &state); g.vnext_tap = &tap; g.vnext_enabled = 1;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);
    arm((1u << 0) | (128u << 0), tid);              // quick + dodge bits
    g_api->observe(PGR_BATTLE_OBS_WARNING, &warn, 0, tid);  // raid warning cue
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != &m_dodge || g_act_dir != 7) return 1;  // modern Direction.Left == 7
    g_act_method = nullptr; g_api->tick(tid, 0);    // window consumed -> next tick is not a dodge
    if (g_act_method == &m_dodge) return 2;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 0, tid);
    return 0;
  }
  case 9: {  // dodge: legacy flick+null ABI driven by the gym warning begin/end
    Obj scope{&c_scope1}, g{&c_legacy_gesture}, iter{&c_gym_iterator}, flick{}, tap{};
    g.legacy_tap = &tap; g.legacy_flick = &flick;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 1, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 1, tid);
    arm((1u << 2) | (128u << 1), tid);              // gym quick + gym dodge
    iter.gym_state = 0;
    uint64_t tok = g_api->gym_begin(&iter, tid);
    if (tok == 0) return 1;
    if (g_api->gym_end(tok, 1, tid) != PGR_BATTLE_OK) return 2;  // ret false -> cue armed
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != &m_legacy_dodge || g_act_p0 != &flick || g_act_p1 != nullptr) return 3;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 1, tid);
    return 0;
  }
  case 10: {  // manual picker blocks all actions; scope exit releases the picker root
    Obj scope{&c_scope0}, g{&c_raid_gesture}, picker{&c_swap1}, tap{};
    wire_active(&g, &director, &state); g.vnext_tap = &tap; picker.open = true;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);
    arm(1u << 0, tid);
    g_api->observe(PGR_BATTLE_OBS_SWAP_OPEN, &picker, 1, tid);  // raid picker descriptor index 1
    if (snap().swap_blocked != 1) return 1;
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != nullptr) return 2;          // picker open blocks the quick attack
    g_api->observe(PGR_BATTLE_OBS_SWAP_CLOSE, &picker, 1, tid);  // close by identity, still in grace
    g_api->tick(tid, 0);
    if (g_act_method != nullptr) return 3;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 0, tid);
    if (g_live_roots != 0) return 4;                // picker + scope + gesture roots all freed
    return 0;
  }
  case 11: {  // Dynamax scope-owned gesture recovery (no SetChargeAttackButtons callback)
    Obj scope{&c_scope2}, g{&c_max_gesture}, tap{};
    wire_active(&g, &director, &state); g.vnext_tap = &tap; scope.max_gesture = &g;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 2, tid);  // NO gesture observe
    arm(1u << 4, tid);
    g_act_method = nullptr; g_api->tick(tid, 0);    // recovery adopts the scope-owned gesture
    if (g_act_method != &m_max_fast) return 1;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 2, tid);
    if (g_live_roots != 0) return 2;
    // Inactive scope-owned handler yields (no recovery, no action).
    Obj scope2{&c_scope2}, g2{&c_max_gesture};
    wire_active(&g2, &director, &state); g2.active = false; scope2.max_gesture = &g2;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope2, 2, tid);
    arm(1u << 4, tid); g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != nullptr) return 3;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope2, 2, tid);
    return 0;
  }
  case 12: {  // STOP clears the battle; roots balance to zero
    Obj scope{&c_scope0}, g{&c_raid_gesture}, charge{&c_rvcharge}, tap{};
    wire_active(&g, &director, &state); g.vnext_tap = &tap; charge.vnext_button = &g;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_CHARGE, &charge, 0, tid);
    arm(1u << 0, tid);
    g_api->tick(tid, 1);                            // stopping -> clear
    if (snap().scope_active != 0 || g_live_roots != 0) return 1;
    return 0;
  }
  case 13: {  // wrong thread: a foreign proven_tid cannot drive observations
    Obj scope{&c_scope0}, g{&c_raid_gesture}, tap{};
    wire_active(&g, &director, &state); g.vnext_tap = &tap;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 0, tid + 1);  // foreign tid -> inert
    if (snap().scope_active != 0) return 1;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);
    arm(1u << 0, tid + 1);                          // foreign update only disarms
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != nullptr) return 2;          // disarmed mask -> no action
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 0, tid);
    return 0;
  }
  case 14: {  // gesture disable then re-arm
    Obj scope{&c_scope0}, g{&c_raid_gesture}, tap{};
    wire_active(&g, &director, &state); g.vnext_tap = &tap;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE_DROP, &g, 0, tid);  // OnDisable
    arm(1u << 0, tid);
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != nullptr) return 1;          // no gesture -> no action
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);       // re-enable
    g_api->tick(tid, 0);
    if (g_act_method != &m_raid_fast) return 2;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 0, tid);
    if (g_live_roots != 0) return 3;
    return 0;
  }
  case 15: {  // reentrant scope-exit fired INSIDE a managed getter cancels the stale action
    Obj scope{&c_scope0}, g{&c_raid_gesture}, tap{};
    wire_active(&g, &director, &state); g.vnext_tap = &tap;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);
    arm(1u << 0, tid);
    g_reenter = 1; g_reenter_scope = &scope;        // m_active will fire observe(SCOPE_EXIT)
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != nullptr) return 1;          // stale action cancelled, no crash
    if (snap().scope_active != 0 || g_live_roots != 0) return 2;  // the newer (cleared) state holds
    return 0;
  }
  case 16: {  // stale / replayed / ineligible gym token
    Obj scope{&c_scope1}, iter{&c_gym_iterator};
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 1, tid);
    arm((1u << 2) | (128u << 1), tid);
    iter.gym_state = 2;                             // ineligible state -> token 0
    if (g_api->gym_begin(&iter, tid) != 0) return 1;
    iter.gym_state = 1;
    uint64_t tok = g_api->gym_begin(&iter, tid);
    if (tok == 0) return 2;
    if (g_api->gym_end(tok + 1, 1, tid) != PGR_BATTLE_ERR_BAD_TOKEN) return 3;  // wrong token
    if (g_api->gym_end(tok, 1, tid) != PGR_BATTLE_OK) return 4;                 // consume once
    if (g_api->gym_end(tok, 1, tid) != PGR_BATTLE_ERR_BAD_TOKEN) return 5;      // replay rejected
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 1, tid);
    return 0;
  }
  case 17: {  // a moving rooted gesture is refreshed at use
    Obj scope{&c_scope0}, g{&c_raid_gesture}, relocated{}, tap{};
    wire_active(&g, &director, &state); g.vnext_tap = &tap;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);
    relocated = g;                                  // same wired fields at the new location
    gc_move(&g, &relocated);                        // GC relocates the gesture
    arm(1u << 0, tid);
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != &m_raid_fast || g_act_obj != &relocated) return 1;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 0, tid);
    return 0;
  }
  case 18: {  // bounded-FIFO overflow blocks actions until the queue drains
    Obj scope{&c_scope0}, g{&c_raid_gesture}, tap{};
    wire_active(&g, &director, &state); g.vnext_tap = &tap;
    for (int i = 0; i < 200; ++i) {                 // flood the FIFO past its cap with scope events
      Obj sc{&c_scope0}; g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &sc, 0, tid);
      g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &sc, 0, tid);
    }
    if (snap().dropped_events == 0) return 1;       // overflow observed
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER, &scope, 0, tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE, &g, 0, tid);
    arm(1u << 0, tid);
    g_act_method = nullptr; g_api->tick(tid, 0);
    if (g_act_method != nullptr) return 2;          // fail-closed: locked actions
    drain_all();                                    // telemetry caught up -> unlock
    g_api->tick(tid, 0);
    if (g_act_method != &m_raid_fast) return 3;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT, &scope, 0, tid);
    if (g_live_roots != 0) return 4;
    return 0;
  }
  case 19: case 20: case 21: case 22: {
    // Revoke/re-enable, nested STOP, actual expiry, or removal of the selected action bit.
    Obj scope{&c_scope0}, g{&c_raid_gesture}, tap{};
    wire_active(&g,&director,&state); g.vnext_tap=&tap;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER,&scope,0,tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE,&g,0,tid);
    g_api->update(1,which==21?10:2500,0,tid);
    g_reenter=which-17;
    g_api->tick(tid,0);
    if(g_act_method) return 1;
    if(which==20 && (snap().scope_active || g_live_roots)) return 2;
    g_api->tick(tid,1);
    if(g_live_roots) return 3;
    return 0;
  }
  case 23: {
    // A foreign OS thread cannot act by supplying the proven Unity thread id.
    Obj scope{&c_scope0}, g{&c_raid_gesture}, tap{};
    wire_active(&g,&director,&state); g.vnext_tap=&tap;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER,&scope,0,tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE,&g,0,tid); arm(1,tid);
    std::thread t([&](){g_api->tick(tid,0); g_api->tick(gettid(),0);
      g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT,&scope,0,gettid());}); t.join();
    if(g_act_method || !snap().scope_active) return 1;
    g_api->tick(tid,0); if(g_act_method!=&m_raid_fast) return 2;
    g_api->tick(tid,1); return g_live_roots?3:0;
  }
  case 24: case 25: case 26: {
    Obj scope{&c_scope0}, replacement{&c_scope0}, g{&c_raid_gesture}, tap{};
    wire_active(&g,&director,&state);g.vnext_tap=&tap;
    g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER,&scope,0,tid);
    g_api->observe(PGR_BATTLE_OBS_GESTURE,&g,0,tid);
    g_reenter_scope=&replacement;
    auto replace=[](){g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER,g_reenter_scope,0,gettid());};
    if(which==26){g_on_new=replace;g_api->observe(PGR_BATTLE_OBS_SCOPE_ENTER,&scope,0,tid);}
    else {g_on_free=replace;if(which==24)g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT,&scope,0,tid);else g_api->tick(tid,1);}
    if(which==25){if(snap().scope_active||g_live_roots)return 1;}
    else {if(!snap().scope_active||g_live_roots!=1)return 2;
      g_api->observe(PGR_BATTLE_OBS_SCOPE_EXIT,&replacement,0,tid);if(g_live_roots)return 3;}
    return 0;
  }
  default: return 0;
  }
}

}  // namespace

int main(int argc,char **argv) {
#ifndef PGR_BATTLE_DIRECT_TEST
  if(argc!=2)return 80;
  void *lib=dlopen(argv[1],RTLD_NOW);if(!lib)return 81;
  auto get=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));
  loaded_core=get?static_cast<const pgrazor_native_api_v2 *>(get(2)):nullptr;
  if(!loaded_core)return 82;
#endif
  g_api = static_cast<const pgr_battle_api_v1 *>(pgrazor_battles_api(1));
  if (!g_api || g_api->size != sizeof(pgr_battle_api_v1)) { std::puts("FAIL table"); return 2; }
  if (pgrazor_battles_api(99) != nullptr) { std::puts("FAIL exact-version"); return 2; }

  // Config validation (parent process; no hooks are ever installed).
  pgr_battle_config_v1 c = make_config(), bad;
  if (g_api->install(nullptr) != PGR_BATTLE_ERR_NULL_ARG) { std::puts("FAIL null"); return 3; }
  bad = c; bad.version = 2;
  if (g_api->install(&bad) != PGR_BATTLE_ERR_BAD_CONFIG) { std::puts("FAIL version"); return 3; }
  bad = c; bad.m_press = nullptr;
  if (g_api->install(&bad) != PGR_BATTLE_ERR_BAD_CONFIG) { std::puts("FAIL missing-ptr"); return 3; }
  bad = c; bad.swaps[2].show = nullptr;
  if (g_api->install(&bad) != PGR_BATTLE_ERR_BAD_CONFIG) { std::puts("FAIL missing-show"); return 3; }
  bad = c; bad.scopes[0].mode = 5;
  if (g_api->install(&bad) != PGR_BATTLE_ERR_BAD_CONFIG) { std::puts("FAIL bad-mode"); return 3; }
  bad = c; bad.reserved0 = 1;
  if (g_api->install(&bad) != PGR_BATTLE_ERR_BAD_CONFIG) { std::puts("FAIL reserved"); return 3; }

  const int cases = 27;
  for (int i = 0; i < cases; ++i) {
    pid_t pid = fork();
    if (pid == 0) {const int result=scenario(i);_exit(result?result:(g_unrooted_sender?79:0));}
    int status = 0; waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      std::printf("FAIL battle scenario %d (code %d)\n", i,
                  WIFEXITED(status) ? WEXITSTATUS(status) : -1);
      return 1;
    }
  }
  std::puts("PASS native battles: raid/gym/Max quick ABIs, vNext/legacy charged, Max attack, "
            "Rocket exclusion, Active/Unset/terminal status, modern+legacy dodge ABIs, manual "
            "picker block + root release, Dynamax scope recovery (+inactive yield), STOP clear, "
            "wrong-thread no-op, gesture re-arm, reentrant exit-in-getter, stale/replay gym token, "
            "moving rooted gesture, FIFO overflow fail-closed, policy revocation/rearm, nested STOP, real lease expiry, real foreign thread");
  return 0;
}

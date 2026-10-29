// Standalone focused test of the native encounter-berry controller (feature 18). Unlike the
// hook-installing siblings, this module installs NO Dobby/Gum hooks, so the harness links the
// module translation unit directly and calls pgrazor_encounter_berries_api(1) -- no dlopen, no
// registry wiring (which is coordinator-owned integration), no NOP-padded targets. It drives a
// compact fake IL2CPP (object_new, runtime_invoke over the berry methods, field get/set, boxed
// value returns) through a proper handle table (so a "GC move" relocates an object while its
// handle stays valid), and forks per scenario for clean process-lifetime state.
//
// This revision reworks the state machine into three disjoint slots (held attempt / unclaimed
// queue / claimed resume) and hardens the lifetime per the coordinator review. The suite is now
// adversarial: it exercises owner-thread binding and spoofed-tid rejection, reentrant exit during
// a preflight getter, a disarm+re-arm during the quantity scan, stale/replayed/nested result
// tokens, a begin_resume cancellation (exit mid-claim, never a submit), roots kept across the
// claimed JS original and zeroed only at finish, the exact throw-word preservation across a
// resume, first-stocked priority, the duplicate/MasterBall/ball-drop/photo bypasses, the
// callback done-latch taken only after the (synchronous) result, the stop-resume vs exit-cancel
// split, a moving rooted ball refreshed at resume, a feed-request exception latching failed, and
// FIFO overflow failing closed (blocks a NEW feed) while still releasing a claimed throw.
//
// Managed objects are simulated. The timeout and cross-thread cases use the real
// monotonic clock and a second OS thread; they do not replace live gameplay checks.
#include "encounter_berries.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#ifndef PGR_BERRY_DIRECT_TEST
#include "pgrazor_native_api.h"
#include <dlfcn.h>
static const pgrazor_native_api_v2 *loaded_core = nullptr;
extern "C" const void *pgrazor_encounter_berries_api(uint32_t version) {
  return loaded_core ? loaded_core->query_feature(18, version) : nullptr;
}
#endif

namespace {

// ---- symbolic il2cpp identities (compared by address only) ------------------------
int c_pokeball, c_scope, c_bag, c_bool, c_int, c_callback, c_ui, c_item;
int m_ball_type, m_bread, m_can, m_photo, m_quantity, m_direct, m_alive;
int f_scope_bag, f_scope_ui, f_ui_state, f_cb_scope, f_cb_berry;

struct Obj      { void *klass; };
struct Ui       { void *klass; int32_t state; };
struct Scope    { void *klass; void *bag; void *ui; };
struct Callback { void *klass; void *scope; int32_t berry; };
struct Box      { void *klass; union { uint8_t b; int32_t i; } v; };

// ---- handle table (models il2cpp GC handles; a move updates the slot, handle stays valid) ----
constexpr int kSlots = 64;
void *g_slots[kSlots];
bool  g_slot_used[kSlots];
int   g_live_roots = 0;
void *g_move_on_free = nullptr, *g_move_destination = nullptr;
void *g_target_trigger = nullptr;
int g_target_action = 0, g_arm_callback_target = 0;
void after_target(void *obj);

void *gc_new(void *o, int) {
  if (!o) return nullptr;
  for (int i = 0; i < kSlots; ++i)
    if (!g_slot_used[i]) { g_slot_used[i] = true; g_slots[i] = o; ++g_live_roots; return &g_slots[i]; }
  return nullptr;  // table full -> allocation failure path
}
void *gc_target(void *h) { if(h)after_target(*static_cast<void **>(h));return h ? *static_cast<void **>(h) : nullptr; }
void  gc_free(void *h) {
  if (!h) return;
  void **slot = static_cast<void **>(h);
  for (int i = 0; i < kSlots; ++i)
    if (&g_slots[i] == slot && g_slot_used[i]) {
      void *old=g_slots[i];g_slot_used[i] = false; g_slots[i] = nullptr; --g_live_roots;
      if(old&&old==g_move_on_free){g_move_on_free=nullptr;
        for(int j=0;j<kSlots;++j)if(g_slot_used[j]&&g_slots[j]==old)g_slots[j]=g_move_destination;
      }return;
    }
}
// Relocate every handle pointing at `from` to `to` (a simulated GC move).
void gc_move(void *from, void *to) { for (int i = 0; i < kSlots; ++i) if (g_slot_used[i] && g_slots[i] == from) g_slots[i] = to; }

void *obj_class(void *o) { return o ? *static_cast<void **>(o) : nullptr; }
void *object_unbox(void *o) { return &static_cast<Box *>(o)->v; }

// ---- scenario knobs / observations -------------------------------------------------
const pgr_berry_api_v1 *g_api = nullptr;
bool    g_bread = false, g_can = true, g_photo = false, g_alive = true;
int32_t g_ball_type = 1;
int32_t g_stock[2048];       // quantity by item id
bool    g_sync_result = false;   // m_direct synchronously fires a matched result
bool    g_direct_throws = false; // m_direct raises a managed exception
int     g_direct_calls = 0;
Callback g_cb_pool;          // single reused callback instance (object_new resets it)
Box      g_box;
void after_target(void *obj){
  if(!obj||obj!=g_target_trigger)return;
  g_target_trigger=nullptr;
  if(g_target_action==1)g_api->update(nullptr,0,0,static_cast<uint64_t>(gettid()));
  if(g_target_action==2)for(int i=0;i<kSlots;++i)if(g_slot_used[i]&&g_slots[i]==obj)g_slots[i]=nullptr;
}

// ---- reentrancy injection: a managed invoke can re-enter the table ONCE --------------
// `g_reenter_method` is the method-identity pointer that triggers a one-shot reentrant call into
// the public API while the original invoke is still on the stack. This models the game calling an
// observer (enter/exit) or the host re-arming policy in the middle of a managed call -- exactly
// the window where a raw pointer held across runtime_invoke, or arming read mid-flight, would be
// unsafe. It never recurses (the reentrant ops issue no managed invoke) and fires at most once.
void    *g_reenter_method = nullptr;
int      g_reenter_mode = 0;   // 1 observe_exit, 2 update disarm+rearm
void    *g_reenter_scope = nullptr;
const int32_t *g_reenter_ids = nullptr; uint32_t g_reenter_n = 0;
bool     g_reenter_fired = false;

void *object_new(void *klass) { g_cb_pool = Callback{klass, nullptr, 0}; return &g_cb_pool; }

void field_get(void *o, void *f, void *out) {
  if      (f == &f_scope_bag) *static_cast<void **>(out)   = static_cast<Scope *>(o)->bag;
  else if (f == &f_scope_ui)  *static_cast<void **>(out)   = static_cast<Scope *>(o)->ui;
  else if (f == &f_ui_state)  *static_cast<int32_t *>(out) = static_cast<Ui *>(o)->state;
  else if (f == &f_cb_scope)  *static_cast<void **>(out)   = static_cast<Callback *>(o)->scope;
  else if (f == &f_cb_berry) {
    *static_cast<int32_t *>(out) = static_cast<Callback *>(o)->berry;
    if(g_arm_callback_target){g_target_trigger=o;g_target_action=g_arm_callback_target;g_arm_callback_target=0;}
  }
}
void field_set_value(void *o, void *f, void *v) {
  if (f == &f_cb_berry) static_cast<Callback *>(o)->berry = *static_cast<int32_t *>(v);
}
void field_set_value_object(void *o, void *f, void *v) {
  if (f == &f_cb_scope) static_cast<Callback *>(o)->scope = v;
}

void maybe_reenter(void *method) {
  if (g_reenter_fired || method != g_reenter_method || g_reenter_method == nullptr) return;
  g_reenter_fired = true;
  const uint64_t tid = static_cast<uint64_t>(gettid());
  if (g_reenter_mode == 1) {
    g_api->observe_exit(g_reenter_scope, tid);
  } else if (g_reenter_mode == 2) {
    g_api->update(nullptr, 0, 0, tid);                 // disable
    g_api->update(g_reenter_ids, g_reenter_n, 2500, tid);  // re-enable
  } else if (g_reenter_mode == 3) {
    g_api->tick(tid, 1);
  } else if (g_reenter_mode == 4) {
    g_api->observe_enter(g_reenter_scope, tid);
  }
}

void *runtime_invoke(void *method, void *obj, void **params, void **exc) {
  *exc = nullptr;
  maybe_reenter(method);
  if (method == &m_bread)  { g_box.klass = &c_bool; g_box.v.b = g_bread ? 1 : 0; (void)obj; return &g_box; }
  if (method == &m_can)    { g_box.klass = &c_bool; g_box.v.b = g_can ? 1 : 0;   return &g_box; }
  if (method == &m_photo)  { g_box.klass = &c_bool; g_box.v.b = g_photo ? 1 : 0; return &g_box; }
  if (method == &m_alive)  { g_box.klass = &c_bool; g_box.v.b = g_alive ? 1 : 0; return &g_box; }
  if (method == &m_ball_type) { g_box.klass = &c_item; g_box.v.i = g_ball_type;   return &g_box; }
  if (method == &m_quantity) {
    int32_t id = params ? *static_cast<int32_t *>(params[0]) : 0;
    g_box.klass = &c_int; g_box.v.i = (id >= 0 && id < 2048) ? g_stock[id] : 0; return &g_box;
  }
  if (method == &m_direct) {
    ++g_direct_calls;
    if (g_direct_throws) { static int e; *exc = &e; return nullptr; }
    if (g_sync_result) {  // the game fires the result callback inside the feed request
      uint64_t tok = g_api->result_begin(&g_cb_pool, static_cast<uint64_t>(gettid()));
      if (tok) g_api->result_end(tok, static_cast<uint64_t>(gettid()));
    }
    return nullptr;
  }
  return nullptr;
}

pgr_berry_config_v1 make_config() {
  pgr_berry_config_v1 c{};
  c.version = PGR_BERRY_API_VERSION; c.size = sizeof(c);
  c.gchandle_new = (void *)&gc_new; c.gchandle_get_target = (void *)&gc_target; c.gchandle_free = (void *)&gc_free;
  c.object_get_class = (void *)&obj_class; c.object_unbox = (void *)&object_unbox; c.object_new = (void *)&object_new;
  c.runtime_invoke = (void *)&runtime_invoke; c.field_get_value = (void *)&field_get;
  c.field_set_value = (void *)&field_set_value; c.field_set_value_object = (void *)&field_set_value_object;
  c.m_ball_type = &m_ball_type; c.m_bread = &m_bread; c.m_can = &m_can; c.m_photo = &m_photo;
  c.m_quantity = &m_quantity; c.m_direct = &m_direct; c.m_alive = &m_alive;
  c.class_pokeball = &c_pokeball; c.class_scope = &c_scope; c.class_bag = &c_bag;
  c.class_bool = &c_bool; c.class_int = &c_int; c.class_callback = &c_callback;
  c.class_ball_type = &c_item;
  c.f_scope_bag = &f_scope_bag; c.f_scope_ui = &f_scope_ui; c.f_ui_state = &f_ui_state;
  c.f_callback_scope = &f_cb_scope; c.f_callback_berry = &f_cb_berry;
  c.ball_type_master = 4; c.state_open = 7; c.reserved0 = 0; c.reserved1 = 0;
  return c;
}

pgr_berry_sample_v1 snap() { pgr_berry_sample_v1 s{}; g_api->sample(&s); return s; }

// Arm the priority list + a live lease on the proven thread.
void arm(const int32_t *ids, uint32_t n, uint64_t tid) { g_api->update(ids, n, 2500, tid); }

int scenario(int which) {
  const uint64_t tid = static_cast<uint64_t>(gettid());
  for (int i = 0; i < 2048; ++i) g_stock[i] = 0;
  pgr_berry_config_v1 cfg = make_config();
  if (g_api->install(&cfg) != PGR_BERRY_OK) return 90;
  // Latch the permanent Unity owner (the realistic first heartbeat) so the managed entry points
  // are live. Case 12 deliberately tests the pre-latch inert behaviour and binds its own owner.
  if (which != 12) g_api->tick(tid, 0);

  Scope scope{&c_scope, nullptr, nullptr};
  Obj bag{&c_bag}; scope.bag = &bag;
  Ui ui{&c_ui, 7}; scope.ui = &ui;
  Obj ball{&c_pokeball};
  const int32_t ids3[3] = {701, 703, 705};

  switch (which) {
  case 0: {  // exact word preservation: queue, resume (no stock), verify ball/words/method
    g_api->observe_enter(&scope, tid);
    arm(ids3, 3, tid);
    const uint64_t first = 0x1122334455667788ull, second = 0x00000000000000AAull;  // not drop, not master
    void *method = reinterpret_cast<void *>(0xDEADBEEF);
    if (g_api->submit(&ball, first, second, method, tid) != PGR_BERRY_HOLD) return 1;
    g_api->tick(tid, 0);                               // no stock -> preflight queues a misc resume
    pgr_berry_resume_v1 out{};
    uint64_t ticket = g_api->begin_resume(tid, &out);
    if (ticket == 0) return 2;
    if (out.ball != &ball || out.first != first || out.second != second || out.method != method) return 3;
    if (out.cause != PGR_BERRY_RESUME_MISC) return 4;
    // A second begin_resume before finish must NOT hand out another ticket (single outstanding).
    pgr_berry_resume_v1 out2{};
    if (g_api->begin_resume(tid, &out2) != 0) return 7;
    if (g_api->finish_resume(ticket, tid) != PGR_BERRY_OK) return 5;
    if (g_live_roots != 1) return 6;                   // only the scope root remains
    return 0;
  }
  case 1: {  // first-stocked priority: 701 empty, 703 stocked -> 703 fed
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid);
    g_stock[703] = 4;
    g_api->submit(&ball, 1, 0, nullptr, tid);
    g_api->tick(tid, 0);                               // preflight picks 703, issues the feed
    if (g_cb_pool.berry != 703) return 1;
    if (snap().request_serial != 1 || g_direct_calls != 1) return 2;
    return 0;
  }
  case 2: {  // bypass: duplicate ball HOLD, MasterBall/ball-drop PASS, photo preflight resume
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid);
    if (g_api->submit(&ball, 1, 0, nullptr, tid) != PGR_BERRY_HOLD) return 1;
    if (g_api->submit(&ball, 1, 0, nullptr, tid) != PGR_BERRY_HOLD) return 2;  // same held ball
    Obj other{&c_pokeball};
    if (g_api->submit(&other, 1, 0, nullptr, tid) != PGR_BERRY_PASS) return 3; // different ball
    // MasterBall (first==4) and ball-drop ((second>>16)&0xff != 0) pass through on a fresh site.
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid);
    if (g_api->submit(&ball, 4, 0, nullptr, tid) != PGR_BERRY_PASS) return 4;
    if (g_api->submit(&ball, 1, 0x10000, nullptr, tid) != PGR_BERRY_PASS) return 5;
    // photo mode -> queued but preflight resumes without feeding.
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid); g_stock[701] = 9; g_photo = true;
    g_api->submit(&ball, 1, 0, nullptr, tid);
    g_api->tick(tid, 0);
    pgr_berry_resume_v1 out{};
    if (g_api->begin_resume(tid, &out) == 0 || g_direct_calls != 0) return 6;
    return 0;
  }
  case 3: {  // callback match + done after the original (synchronous result), DONE resume
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid); g_stock[701] = 9; g_sync_result = true;
    g_api->submit(&ball, 1, 0, nullptr, tid);
    g_api->tick(tid, 0);                               // issue feed; result fires synchronously
    g_api->tick(tid, 0);                               // next pass sees done -> queue DONE resume
    pgr_berry_resume_v1 out{};
    uint64_t ticket = g_api->begin_resume(tid, &out);
    if (ticket == 0 || out.cause != PGR_BERRY_RESUME_DONE) return 1;
    g_api->finish_resume(ticket, tid);
    if (g_live_roots != 1) return 2;
    return 0;
  }
  case 4: {  // 8 s timeout -> single resume, failed latched, subsequent throws pass through
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid); g_stock[701] = 9;
    g_api->submit(&ball, 1, 0, nullptr, tid);
    g_api->tick(tid, 0);                               // feed issued, awaiting result
    // Can't advance CLOCK_BOOTTIME by 8 s in a unit test; assert the deadline path indirectly:
    // without a result the tick leaves it pending (no resume yet).
    pgr_berry_resume_v1 out{};
    if (g_api->begin_resume(tid, &out) != 0) return 1; // nothing queued while waiting
    return 0;
  }
  case 5: {  // stop resumes the held throw; a fresh encounter then exits (cancel, no submit)
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid);
    g_api->submit(&ball, 1, 0, nullptr, tid);
    g_api->tick(tid, 1);                               // stopping -> queue a STOP resume
    pgr_berry_resume_v1 out{};
    uint64_t ticket = g_api->begin_resume(tid, &out);
    if (ticket == 0 || out.cause != PGR_BERRY_RESUME_STOP) return 1;
    g_api->finish_resume(ticket, tid);
    // Exit path: queue then exit cancels without a submit (no resume claimable).
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid);
    Obj ball2{&c_pokeball};
    g_api->submit(&ball2, 1, 0, nullptr, tid);
    int before = g_live_roots;
    g_api->observe_exit(&scope, tid);
    if (g_api->begin_resume(tid, &out) != 0) return 2; // nothing to resume: cancelled
    if (g_live_roots >= before) return 3;              // ball + scope roots freed
    return 0;
  }
  case 6: {  // wrong thread: submit PASS, tick no-op
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid);
    if (g_api->submit(&ball, 1, 0, nullptr, tid + 1) != PGR_BERRY_PASS) return 1;
    g_api->tick(tid + 1, 0);
    if (snap().overlay_busy != 0) return 2;
    return 0;
  }
  case 7: {  // a new enter cancels a stale pending without a submit
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid);
    g_api->submit(&ball, 1, 0, nullptr, tid);
    if (snap().overlay_busy != 1) return 1;
    Scope scope2{&c_scope, &bag, &ui};
    g_api->observe_enter(&scope2, tid);                // berryClear at enter
    if (snap().overlay_busy != 0) return 2;
    pgr_berry_resume_v1 out{};
    if (g_api->begin_resume(tid, &out) != 0) return 3;
    return 0;
  }
  case 8: {  // a moving rooted ball is refreshed at resume
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid);
    g_api->submit(&ball, 0xAAAA, 0, nullptr, tid);
    Obj relocated{&c_pokeball};
    gc_move(&ball, &relocated);                        // GC relocates the ball
    g_api->tick(tid, 0);                               // no stock -> resume
    pgr_berry_resume_v1 out{};
    uint64_t ticket = g_api->begin_resume(tid, &out);
    if (ticket == 0 || out.ball != &relocated || out.first != 0xAAAA) return 1;
    g_api->finish_resume(ticket, tid);
    return 0;
  }
  case 9: {  // a feed-request exception latches failed and resumes once
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid); g_stock[701] = 9; g_direct_throws = true;
    g_api->submit(&ball, 1, 0, nullptr, tid);
    g_api->tick(tid, 0);                               // request throws -> failed + resume
    if (snap().failed != 1 || snap().fault_serial == 0) return 1;
    pgr_berry_resume_v1 out{};
    uint64_t ticket = g_api->begin_resume(tid, &out);
    if (ticket == 0) return 2;
    g_api->finish_resume(ticket, tid);
    // After failed, a fresh submit in the same encounter passes through.
    Obj ball2{&c_pokeball};
    if (g_api->submit(&ball2, 1, 0, nullptr, tid) != PGR_BERRY_PASS) return 3;
    return 0;
  }
  case 10: {  // FIFO overflow fails closed: still releases a claimed throw, then blocks a NEW feed
    // (1) Queue + CLAIM a resume BEFORE any overflow; the claimed frame (the released throw) is
    //     handed to JS and must survive regardless of telemetry state.
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid);
    g_api->submit(&ball, 0x55, 0, nullptr, tid);       // queue a throw (held)
    g_api->tick(tid, 0);                               // no stock -> queue a resume
    pgr_berry_resume_v1 out{};
    uint64_t ticket = g_api->begin_resume(tid, &out);  // JS claims the released throw
    if (ticket == 0 || out.first != 0x55) return 1;
    // (2) Flood the FIFO past its cap via throwaway enters (each emits an overlay event). These
    //     cancel only the UNCLAIMED work -- the claimed frame is untouched -- and latch fail-closed.
    for (int i = 0; i < 300; ++i) { Scope sc{&c_scope, &bag, &ui}; g_api->observe_enter(&sc, tid); }
    if (snap().dropped_events == 0) return 2;          // overflow observed (fail-closed latched)
    // (3) The already-claimed throw still finishes -- overflow never strands a held/claimed throw.
    if (g_api->finish_resume(ticket, tid) != PGR_BERRY_OK) return 3;
    if (snap().resume_serial != 1 || snap().overlay_busy != 0) return 4;
    // (4) With the frame released and telemetry STILL fail-closed (FIFO never drained), a NEW
    //     submit is blocked: PASS, no fresh hold, no feed queued.
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid); g_stock[701] = 9;
    Obj ball2{&c_pokeball};
    if (g_api->submit(&ball2, 1, 0, nullptr, tid) != PGR_BERRY_PASS) return 5;  // fail-closed hold block
    if (snap().overlay_busy != 0) return 6;            // nothing held
    pgr_berry_resume_v1 out2{};
    if (g_api->begin_resume(tid, &out2) != 0) return 7;  // and nothing to resume
    return 0;
  }
  case 11: {  // resume-ticket double finish is rejected; roots stay balanced
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid);
    g_api->submit(&ball, 1, 0, nullptr, tid);
    g_api->tick(tid, 0);
    pgr_berry_resume_v1 out{};
    uint64_t ticket = g_api->begin_resume(tid, &out);
    if (ticket == 0) return 1;
    if (g_api->finish_resume(ticket, tid) != PGR_BERRY_OK) return 2;
    if (g_api->finish_resume(ticket, tid) != PGR_BERRY_ERR_BAD_TOKEN) return 3; // no replay/free
    if (g_live_roots != 1) return 4;                   // scope only
    return 0;
  }
  case 12: {  // owner binding: pre-latch inert; after latch a non-owner proven_tid is rejected
    // Before any tick the owner is unlatched: a self-reported tid cannot drive the machine.
    g_api->observe_enter(&scope, tid);                 // no owner yet -> no-op (scope not rooted)
    arm(ids3, 3, tid);
    if (g_api->submit(&ball, 1, 0, nullptr, tid) != PGR_BERRY_PASS) return 1;  // inert -> PASS
    if (snap().overlay_busy != 0 || g_live_roots != 0) return 2;               // nothing held/rooted
    // Latch the owner, then a correctly-proven call works.
    g_api->tick(tid, 0);
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid);
    if (g_api->submit(&ball, 1, 0, nullptr, tid) != PGR_BERRY_HOLD) return 3;
    // A call naming a DIFFERENT (non-owner) tid is rejected even though it names a tid.
    Obj other{&c_pokeball};
    if (g_api->submit(&other, 1, 0, nullptr, tid + 1) != PGR_BERRY_PASS) return 4;
    pgr_berry_resume_v1 out{};
    if (g_api->begin_resume(tid + 1, &out) != 0) return 5;       // off-owner begin inert
    if (g_api->finish_resume(1, tid + 1) != PGR_BERRY_ERR_BAD_TOKEN) return 6;
    return 0;
  }
  case 13: {  // reentrant exit DURING a preflight getter: no feed, no stranded roots, no submit
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid); g_stock[701] = 9;
    g_api->submit(&ball, 1, 0, nullptr, tid);          // HELD
    g_reenter_method = &m_can; g_reenter_mode = 1; g_reenter_scope = &scope; g_reenter_fired = false;
    g_api->tick(tid, 0);                               // CanUseBerry getter re-enters observe_exit
    if (!g_reenter_fired) return 1;                    // the reentry actually fired
    if (g_direct_calls != 0 || snap().request_serial != 0) return 2;  // never fed the closed scope
    pgr_berry_resume_v1 out{};
    if (g_api->begin_resume(tid, &out) != 0) return 3; // the attempt was cancelled, not resumed
    if (g_live_roots != 0) return 4;                   // ball + scope roots all freed (no leak)
    return 0;
  }
  case 14: {  // disarm + re-arm revokes this feed but releases the held manual throw once
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid); g_stock[703] = 4;  // 701 empty, 703 stocked
    g_api->submit(&ball, 1, 0, nullptr, tid);          // snapshots {701,703,705}
    g_reenter_method = &m_quantity; g_reenter_mode = 2; g_reenter_ids = ids3; g_reenter_n = 3;
    g_reenter_fired = false;
    g_api->tick(tid, 0);                               // first quantity(701) re-enters update x2
    if (!g_reenter_fired) return 1;
    if (snap().request_serial != 0 || g_direct_calls != 0) return 3;
    pgr_berry_resume_v1 out{}; const auto ticket=g_api->begin_resume(tid,&out);
    if (!ticket || out.ball!=&ball || out.first!=1) return 4;
    if (g_api->begin_resume(tid,&out)!=0) return 5;
    if (g_api->finish_resume(ticket,tid)!=PGR_BERRY_OK) return 6;
    return 0;
  }
  case 15: {  // result token bound to generation: stale cross-encounter, nested, and replay reject
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid); g_stock[701] = 9;
    g_api->submit(&ball, 1, 0, nullptr, tid);
    g_api->tick(tid, 0);                               // attempt1 FEEDING
    uint64_t tok1 = g_api->result_begin(&g_cb_pool, tid);
    if (tok1 == 0) return 1;
    // A new encounter cancels attempt1; attempt2 gets a fresh generation.
    Scope scope2{&c_scope, &bag, &ui};
    g_api->observe_enter(&scope2, tid); arm(ids3, 3, tid); g_stock[701] = 9;
    g_api->submit(&ball, 1, 0, nullptr, tid);
    g_api->tick(tid, 0);                               // attempt2 FEEDING
    uint64_t tok2 = g_api->result_begin(&g_cb_pool, tid);
    if (tok2 == 0 || tok2 == tok1) return 2;           // distinct generation token
    if (g_api->result_begin(&g_cb_pool, tid) != 0) return 3;              // nested begin rejected
    if (g_api->result_end(tok1, tid) != PGR_BERRY_ERR_BAD_TOKEN) return 4; // stale generation
    if (g_api->result_end(tok2, tid) != PGR_BERRY_OK) return 5;           // the live token latches
    if (g_api->result_end(tok2, tid) != PGR_BERRY_ERR_BAD_TOKEN) return 6; // replay consumed once
    return 0;
  }
  case 16: {  // begin_resume cancellation: exit mid-claim -> no ticket, no submit, roots freed
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid);
    g_api->submit(&ball, 1, 0, nullptr, tid);
    g_api->tick(tid, 0);                               // queue a resume (unclaimed)
    // Arm a one-shot exit DURING begin_resume's liveness invoke (m_alive).
    g_reenter_method = &m_alive; g_reenter_mode = 1; g_reenter_scope = &scope; g_reenter_fired = false;
    pgr_berry_resume_v1 out{};
    uint64_t ticket = g_api->begin_resume(tid, &out);  // scope dies mid-claim -> cancel, no submit
    if (!g_reenter_fired) return 1;
    if (ticket != 0) return 2;                         // never a claim ticket on a dead encounter
    if (g_direct_calls != 0) return 3;                 // never a submit of the held throw
    if (g_live_roots != 0) return 4;                   // queue + scope freed (no stranded roots)
    return 0;
  }
  case 17: {  // roots kept across the claimed JS original; zeroed only at finish; begin once
    g_api->observe_enter(&scope, tid); arm(ids3, 3, tid);
    g_api->submit(&ball, 1, 0, nullptr, tid);
    g_api->tick(tid, 0);                               // queue a resume
    int before = g_live_roots;                         // scope + queued ball (+callback none here)
    pgr_berry_resume_v1 out{};
    uint64_t ticket = g_api->begin_resume(tid, &out);
    if (ticket == 0) return 1;
    // While JS holds the ticket (and would be calling the original), the ball root is KEPT.
    if (g_live_roots != before) return 2;              // claim transferred the root, did not free
    if (out.ball != &ball) return 3;
    // An exit now does NOT free the claimed frame (deferred to finish); roots unchanged.
    g_api->observe_exit(&scope, tid);
    if (g_live_roots != 1) return 4;                   // scope freed; the claimed ball root remains
    if (g_api->finish_resume(ticket, tid) != PGR_BERRY_OK) return 5;
    if (g_live_roots != 0) return 6;                   // only now is the claimed ball root freed
    return 0;
  }
  case 18: { // real second thread reporting its own OS tid cannot operate the bound controller
    g_api->observe_enter(&scope,tid); arm(ids3,3,tid);
    int decision=-1;
    std::thread other([&](){const auto foreign=static_cast<uint64_t>(gettid());
      g_api->observe_exit(&scope,foreign); decision=g_api->submit(&ball,1,0,nullptr,foreign);
      g_api->tick(foreign,1);
    }); other.join();
    if (decision!=PGR_BERRY_PASS || g_live_roots!=1) return 1;
    g_api->observe_exit(&scope,tid); return g_live_roots?2:0;
  }
  case 19: { // nested STOP during preflight never feeds, but releases the held throw once
    g_api->observe_enter(&scope,tid); arm(ids3,3,tid); g_stock[701]=9;
    g_api->submit(&ball,1,0,nullptr,tid);
    g_reenter_method=&m_quantity;g_reenter_mode=3;g_reenter_fired=false;
    g_api->tick(tid,0);
    if (!g_reenter_fired || g_direct_calls || snap().request_serial) return 1;
    pgr_berry_resume_v1 out{};const auto ticket=g_api->begin_resume(tid,&out);
    if (!ticket || out.ball!=&ball) return 2;
    g_api->finish_resume(ticket,tid);g_api->tick(tid,1);
    return g_live_roots?3:0;
  }
  case 20: { // actual deadline expiration releases a throw without retrying an uncertain feed
    g_api->observe_enter(&scope,tid);arm(ids3,3,tid);g_stock[701]=9;
    g_api->submit(&ball,1,0,nullptr,tid);g_api->tick(tid,0);
    if (g_direct_calls!=1) return 1;
    usleep(8100000);g_api->tick(tid,0);
    pgr_berry_resume_v1 out{};const auto ticket=g_api->begin_resume(tid,&out);
    if (!ticket || out.cause!=PGR_BERRY_RESUME_TIMEOUT || g_direct_calls!=1) return 2;
    g_api->finish_resume(ticket,tid);arm(ids3,3,tid);
    if (g_api->submit(&ball,1,0,nullptr,tid)!=PGR_BERRY_PASS) return 3;
    g_api->observe_exit(&scope,tid);return g_live_roots?4:0;
  }
  case 21: { // replacement during the initial alive getter preserves new scope and cancels old throw
    g_api->observe_enter(&scope,tid);arm(ids3,3,tid);g_stock[701]=9;
    g_api->submit(&ball,1,0,nullptr,tid);
    Scope replacement{&c_scope,&bag,&ui};g_reenter_scope=&replacement;
    g_reenter_method=&m_alive;g_reenter_mode=4;g_reenter_fired=false;
    g_api->tick(tid,0);
    if (!g_reenter_fired || g_direct_calls || g_live_roots!=1) return 1;
    pgr_berry_resume_v1 out{};if(g_api->begin_resume(tid,&out))return 2;
    g_api->observe_exit(&replacement,tid);return g_live_roots?3:0;
  }
  case 22: { // relocating a ball while temporary roots are freed must refresh the exported pointer
    g_api->observe_enter(&scope,tid);arm(ids3,3,tid);
    g_api->submit(&ball,1,0,nullptr,tid);g_api->tick(tid,0);
    Obj moved{&c_pokeball};g_move_on_free=&ball;g_move_destination=&moved;
    pgr_berry_resume_v1 out{};const auto ticket=g_api->begin_resume(tid,&out);
    if(!ticket||out.ball!=&moved)return 1;
    g_api->finish_resume(ticket,tid);g_api->observe_exit(&scope,tid);return g_live_roots?2:0;
  }
  case 23: case 24: { // final callback-target lookup revocation or disappearance must not feed
    g_api->observe_enter(&scope,tid);arm(ids3,3,tid);g_stock[701]=9;
    g_api->submit(&ball,1,0,nullptr,tid);g_arm_callback_target=which==23?1:2;
    g_api->tick(tid,0);
    if(g_arm_callback_target||g_target_trigger||g_direct_calls||snap().request_serial)return 1;
    pgr_berry_resume_v1 out{};const auto ticket=g_api->begin_resume(tid,&out);
    if(!ticket||out.ball!=&ball)return 2;
    g_api->finish_resume(ticket,tid);g_api->observe_exit(&scope,tid);return g_live_roots?3:0;
  }
  default: return 0;
  }
}

}  // namespace

int main(int argc, char **argv) {
#ifndef PGR_BERRY_DIRECT_TEST
  if(argc!=2){std::puts("Expected native core library path");return 2;}
  void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
  if(!library){std::puts(dlerror());return 2;}
  using GetApi=const void *(*)(uint32_t);
  auto get=reinterpret_cast<GetApi>(dlsym(library,"pgrazor_native_api"));
  loaded_core=get?static_cast<const pgrazor_native_api_v2 *>(get(2)):nullptr;
  if(!loaded_core||loaded_core->version!=2||loaded_core->size!=sizeof(*loaded_core)||!loaded_core->query_feature)return 2;
#endif
  g_api = static_cast<const pgr_berry_api_v1 *>(pgrazor_encounter_berries_api(1));
  if (!g_api || g_api->size != sizeof(pgr_berry_api_v1)) { std::puts("FAIL table"); return 2; }
  if (pgrazor_encounter_berries_api(99) != nullptr) { std::puts("FAIL exact-version"); return 2; }

  // Config validation (parent process; no hooks are ever installed).
  pgr_berry_config_v1 c = make_config(), bad;
  if (g_api->install(nullptr) != PGR_BERRY_ERR_NULL_ARG) { std::puts("FAIL null"); return 3; }
  bad = c; bad.version = 2;
  if (g_api->install(&bad) != PGR_BERRY_ERR_BAD_CONFIG) { std::puts("FAIL version"); return 3; }
  bad = c; bad.m_direct = nullptr;
  if (g_api->install(&bad) != PGR_BERRY_ERR_BAD_CONFIG) { std::puts("FAIL missing"); return 3; }
  bad = c; bad.reserved0 = 1;
  if (g_api->install(&bad) != PGR_BERRY_ERR_BAD_CONFIG) { std::puts("FAIL reserved"); return 3; }

  const int cases = 25;
  for (int i = 0; i < cases; ++i) {
    pid_t pid = fork();
    if (pid == 0) _exit(scenario(i));
    int status = 0; waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      std::printf("FAIL berry scenario %d (code %d)\n", i,
                  WIFEXITED(status) ? WEXITSTATUS(status) : -1);
      return 1;
    }
  }
  std::puts("PASS native encounter berries: word preservation + single begin, stock priority, "
            "duplicate/special/photo bypass, callback match + post-original done, pending wait, "
            "stop-resume vs exit-cancel, wrong-thread no-op, new-enter cancel, moving-ball refresh, "
            "request-fault latch, FIFO fail-closed (blocks feed, releases claimed throw), "
            "double-finish reject, owner-bind/spoofed-tid reject, reentrant-exit preflight safety, "
            "disarm+rearm cancellation, real second thread, nested STOP, real timeout, scope replacement, stale/nested/replay result token, begin_resume cancel no-submit, "
            "roots kept across claimed original then zeroed at finish");
  return 0;
}

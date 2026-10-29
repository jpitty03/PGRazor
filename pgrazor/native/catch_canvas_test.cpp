#include "catch_canvas.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <set>
#include <thread>
#include <unistd.h>

// Production feature 39 ABI/behaviour harness (native CATCH CANVAS visibility control). It resolves the
// table through the exported pgrazor_native_api v2 registry (query_feature(39,1)) and drives the module
// two ways at once:
//   * a Dobby hook fixture -- a padded, noinline set_CanvasGroupVisible target in THIS executable (see
//     spin_camera_test.cpp) that the module patches; a game-originated set-true on an owned opening
//     details root is masked to false, everything else passes through, and the original is chained once
//     with the exact self/MethodInfo.
//   * a fake moving IL2CPP heap of borrowed GC handles with op_Implicit (boxed Boolean) alive probes and
//     a set_CanvasGroupVisible invoked by runtime_invoke -- routed back through the SAME hooked compiled
//     function so a managed set-false is never masked and a post-release set-true is never re-hidden.
// The module installs its one hook and NEVER calls back into JS; the coordinator builds and runs this
// against libpgrazor.so after registering query_feature case 39 and linking catch_canvas.cpp (+ a Dobby
// bridge if the host test harness needs one).

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class / field / method tokens (distinct addresses) --------------------------
int summaryClass, detailsClass, boolClass, otherClass;
int f_current_state, m_alive, m_canvas_method;

constexpr int32_t kStateOpen = 2;      // GuiController currentState == STATE_OPEN
constexpr int32_t kStateOpening = 1;   // a still-opening canvas (hide_opening tolerates it)
constexpr int32_t kStateDismissed = 5; // a dismissed, advanced-off-OPEN state

// ----- fake managed objects (first member is always the Il2CppClass*) --------------
struct Gui { void *klass; int32_t state; };
struct BoolBox { void *klass; uint8_t value; };

Gui g_guis[8];       // scene slots for relocation
int g_cur = 0;       // the live scene for the GUI under test
Gui &cur() { return g_guis[g_cur]; }
BoolBox g_bool{&boolClass, 1};

// ----- runtime shims ----------------------------------------------------------------
const PgrCatchCanvasApi *g_api = nullptr;
uint64_t g_owner = 0;

bool g_live = true;          // unboxed Boolean op_Implicit returns
int g_alive_fault = 0;       // 0 none / 1 throw / 2 wrong boxed class / 3 out-of-range byte
bool g_move_alive = false;   // relocate the scene inside op_Implicit
bool g_throw_canvas = false; // the managed set_CanvasGroupVisible reports a throw
uint64_t g_runtime_invoke_calls = 0;  // every op_Implicit / set invoke (NOT touched by the hook)

std::map<uintptr_t, void *> g_handles;
std::set<uintptr_t> g_host;
uintptr_t g_seq = 0x1000;

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

void relocate() {
  assert(g_cur < 7);
  Gui &old = g_guis[g_cur];
  Gui &next = g_guis[++g_cur];
  next = old;
  for (auto &e : g_handles) {
    if (e.second == &old) e.second = &next;
  }
  old.klass = &otherClass;  // poison the abandoned copy
  old.state = 99;
}

uintptr_t fake_gc_new(void *o, int) {
  if (!o) return 0;
  g_handles[++g_seq] = o;
  return g_seq;
}
void *fake_gc_get(uintptr_t h) { auto i = g_handles.find(h); return i == g_handles.end() ? nullptr : i->second; }
void fake_gc_free(uintptr_t h) { assert(!g_host.count(h)); assert(g_handles.erase(h) == 1); }
void *borrow(void *o) { uintptr_t h = ++g_seq; g_handles[h] = o; g_host.insert(h); return reinterpret_cast<void *>(h); }
void balanced() { assert(g_handles.size() == g_host.size()); for (uintptr_t h : g_host) assert(g_handles.count(h)); }

void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }
void *fake_unbox(void *boxed) { return boxed ? static_cast<char *>(boxed) + sizeof(void *) : nullptr; }

void fake_field_get(void *obj, void *field, void *out) {
  assert(obj && fake_obj_class(obj) != &otherClass);  // poisoned copies must never be read
  if (field == &f_current_state) { *static_cast<int32_t *>(out) = static_cast<Gui *>(obj)->state; return; }
  assert(false && "unknown field token");
}

// The real compiled set_CanvasGroupVisible the module patches. Padded + noinline so Dobby can trampoline.
struct CanvasLog { int count = 0; void *last_self = nullptr; void *last_mi = nullptr; bool last_visible = false; };
CanvasLog g_canvas;
bool g_original_throw=false;
unsigned g_observer_calls=0;
bool g_observer_move=false;
int observer_filter(void*,void *obj,uint64_t tid){return tid==g_owner&&fake_obj_class(obj)==&detailsClass?1:0;}
void observer_enter(void*,void *obj,uint64_t tid){
 assert(tid==g_owner&&obj==&cur());++g_observer_calls;
 PgrCatchCanvasSample s{};assert(g_api->sample(&s)==0&&s.owned==1);
 if(g_observer_move){g_observer_move=false;relocate();}
}

#define PGR_NOP8() asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;")
__attribute__((noinline)) void target_set_canvas(void *self, bool visible, void *method_info) {
  PGR_NOP8();
  ++g_canvas.count;
  g_canvas.last_self = self;
  g_canvas.last_mi = method_info;
  g_canvas.last_visible = visible;
  if(g_original_throw){g_original_throw=false;throw 123;}
}
void (*volatile call_set_canvas)(void *, bool, void *) = &target_set_canvas;

int g_invoke_mi = 0x5151;  // MethodInfo the runtime_invoke path forwards
int g_game_mi = 0x6262;    // MethodInfo a direct game call forwards

void *fake_runtime_invoke(const void *method, void *obj, void **params, void **exc) {
  if (exc) *exc = nullptr;
  ++g_runtime_invoke_calls;
  if (method == &m_alive) {
    assert(obj == nullptr && params && params[0] != nullptr);  // reference arg is the GUI, not &ptr
    void *k = fake_obj_class(params[0]);
    assert(k == &detailsClass || k == &summaryClass);          // never a poisoned / stray object
    if (g_move_alive) { g_move_alive = false; relocate(); }
    if (g_alive_fault == 1) { if (exc) *exc = reinterpret_cast<void *>(0x2); return nullptr; }
    g_bool.klass = (g_alive_fault == 2) ? &otherClass : &boolClass;
    g_bool.value = (g_alive_fault == 3) ? 7 : static_cast<uint8_t>(g_live ? 1 : 0);
    return &g_bool;
  }
  assert(method == &m_canvas_method && obj != nullptr && params != nullptr);
  const bool v = *static_cast<uint8_t *>(params[0]) != 0;
  if (g_throw_canvas) { g_throw_canvas = false; if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
  // Route the managed set through the SAME hooked compiled function, exactly as production does.
  call_set_canvas(obj, v, &g_invoke_mi);
  return nullptr;
}

PgrCatchCanvasConfig make_config() {
  PgrCatchCanvasConfig c{};
  c.version = 1; c.size = sizeof(c);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c.object_unbox = reinterpret_cast<void *>(&fake_unbox);
  c.gc_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gc_get = reinterpret_cast<void *>(&fake_gc_get);
  c.gc_free = reinterpret_cast<void *>(&fake_gc_free);
  c.class_summary = &summaryClass; c.class_details = &detailsClass; c.class_bool = &boolClass;
  c.f_current_state = &f_current_state; c.m_alive = &m_alive; c.m_canvas = &m_canvas_method;
  c.canvas_address = reinterpret_cast<void *>(&target_set_canvas);
  c.state_open = kStateOpen; c.reserved0 = 0;
  return c;
}

// Reset the heap and place one GUI (default DetailsGuiController, OPEN) in scene 0; return a borrowed
// handle. Does NOT arm the gate.
void *build_gui(void *klass = &detailsClass, int32_t state = kStateOpen) {
  assert(g_handles.size() == g_host.size());
  g_handles.clear(); g_host.clear();
  g_cur = 0;
  g_live = true; g_alive_fault = 0; g_move_alive = false; g_throw_canvas = false;
  g_guis[0] = {klass, state};
  return borrow(&g_guis[0]);
}

void arm(uint64_t epoch = 7) { assert(g_api->update(1, 2500, epoch, g_owner) == PGR_CANVAS_OK); }

PgrCatchCanvasSample sample() {
  PgrCatchCanvasSample s{}; assert(g_api->sample(&s) == PGR_CANVAS_OK);
  assert(s.version == 1 && s.size == sizeof(s)); return s;
}

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) { std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror()); return 1; }
  auto getter = reinterpret_cast<GetApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == sizeof(pgrazor_native_api_v2));
  assert(core->query_feature(39, 99) == nullptr);
  const auto *api = static_cast<const PgrCatchCanvasApi *>(core->query_feature(39, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrCatchCanvasApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu sample=%zu table=%zu\n",
              sizeof(PgrCatchCanvasConfig), sizeof(PgrCatchCanvasSample), sizeof(PgrCatchCanvasApi));
  assert(sizeof(PgrCatchCanvasConfig) == 128 && sizeof(PgrCatchCanvasSample) == 40 &&
         sizeof(PgrCatchCanvasApi) == 72);
  assert(offsetof(PgrCatchCanvasConfig, state_open) == 120 &&
         offsetof(PgrCatchCanvasConfig, reserved0) == 124);

  const uint64_t tid = this_tid();
  g_owner = tid;
  PgrCatchCanvasConfig cfg = make_config();

  // 1. Config validation, not-installed, idempotence, reconfig.
  assert(api->install(nullptr) == PGR_CANVAS_ERR_NULL_ARG);
  PgrCatchCanvasConfig bad = cfg; bad.version = 2;
  assert(api->install(&bad) == PGR_CANVAS_ERR_BAD_VERSION);
  bad = cfg; bad.size = sizeof(cfg) - 1; assert(api->install(&bad) == PGR_CANVAS_ERR_BAD_SIZE);
  bad = cfg; bad.m_canvas = nullptr; assert(api->install(&bad) == PGR_CANVAS_ERR_MISSING_PTR);
  bad = cfg; bad.canvas_address = nullptr; assert(api->install(&bad) == PGR_CANVAS_ERR_MISSING_PTR);
  { void *h = build_gui();
    uint64_t t = 7;
    assert(api->hide_opening(h, 7, tid, &t) == PGR_CANVAS_ERR_NOT_INSTALLED && t == 0);
    assert(api->update(1, 1000, 7, tid) == PGR_CANVAS_ERR_NOT_INSTALLED);
    g_handles.clear(); g_host.clear(); }
  assert(api->install(&cfg) == PGR_CANVAS_OK);
  assert(api->install(&cfg) == PGR_CANVAS_OK);  // idempotent
  bad = cfg; bad.state_open = 9; assert(api->install(&bad) == PGR_CANVAS_ERR_RECONFIG);

  // 2. hide_opening: HIDDEN, nonzero token, slot owned, managed set-false reached the setter unmasked.
  { void *h = build_gui(&detailsClass, kStateOpening);  // still opening: no STATE_OPEN gate
    arm();
    g_canvas = {};
    uint64_t t = 0;
    assert(api->hide_opening(h, 7, tid, &t) == PGR_CANVAS_HIDDEN && t != 0);
    assert(g_canvas.count == 1 && g_canvas.last_visible == false && g_canvas.last_self == &cur());
    PgrCatchCanvasSample s = sample();
    assert(s.owned == 1 && s.hide_count == 1 && s.fault == 0);

    // 3. Masking: a game set-true on this owned root is forced to false, chaining orig once with the
    //    exact self/MethodInfo, with NO managed invoke by the hook.
    g_canvas = {};
    const uint64_t inv_before = g_runtime_invoke_calls;
    const uint64_t masked_before = sample().masked_count;
    call_set_canvas(&cur(), true, &g_game_mi);
    assert(g_canvas.count == 1 && g_canvas.last_self == &cur() && g_canvas.last_mi == &g_game_mi);
    assert(g_canvas.last_visible == false);                       // masked true -> false
    assert(sample().masked_count == masked_before + 1);
    assert(g_runtime_invoke_calls == inv_before);                 // hook invoked no managed method

    // 3b. A set-false passes straight through (never masked).
    g_canvas = {};
    call_set_canvas(&cur(), false, &g_game_mi);
    assert(g_canvas.count == 1 && g_canvas.last_visible == false);

    // 4. Duplicate hide reuses the token with no redundant managed set.
    g_canvas = {};
    uint64_t t2 = 0;
    assert(api->hide_opening(h, 7, tid, &t2) == PGR_CANVAS_HIDDEN && t2 == t);
    assert(g_canvas.count == 0 && sample().hide_count == 1);

    // 5. release while OPEN restores: slot removed BEFORE the set-true, so the hook does not re-hide.
    cur().state = kStateOpen;
    g_canvas = {};
    assert(api->release(t, tid) == PGR_CANVAS_OK);
    assert(g_canvas.count == 1 && g_canvas.last_visible == true);  // restored, not re-masked
    assert(sample().owned == 0 && sample().restore_count == 1);
    assert(api->release(t, tid) == PGR_CANVAS_OK);                 // idempotent
    assert(api->release(0, tid) == PGR_CANVAS_OK);                 // zero token idempotent
    balanced(); }

  // 6. Unrelated GUI and foreign thread both pass through unmasked.
  { void *h = build_gui(); arm();
    uint64_t t = 0; assert(api->hide_opening(h, 7, tid, &t) == PGR_CANVAS_HIDDEN);
    Gui other{&detailsClass, kStateOpen};
    g_canvas = {};
    call_set_canvas(&other, true, &g_game_mi);               // not an owned root
    assert(g_canvas.count == 1 && g_canvas.last_visible == true);
    g_canvas = {};
    std::thread([&]() { call_set_canvas(&cur(), true, &g_game_mi); }).join();  // foreign thread
    assert(g_canvas.count == 1 && g_canvas.last_visible == true);  // never masked off the owner thread
    assert(api->stop() == PGR_CANVAS_OK); assert(api->tick(tid) == PGR_CANVAS_OK); balanced(); }

  // 7. Mask only under a LIVE gate + matching epoch: an epoch change stops masking and tick prunes+
  //    restores the now-stale slot (OPEN).
  { void *h = build_gui(&detailsClass, kStateOpen); arm(7);
    uint64_t t = 0; assert(api->hide_opening(h, 7, tid, &t) == PGR_CANVAS_HIDDEN);
    assert(api->update(1, 2500, 8, tid) == PGR_CANVAS_OK);   // new epoch; slot epoch 7 now stale
    g_canvas = {};
    call_set_canvas(&cur(), true, &g_game_mi);
    assert(g_canvas.last_visible == true);                   // epoch mismatch -> not masked
    g_canvas = {};
    assert(api->tick(tid) == PGR_CANVAS_OK);
    assert(sample().owned == 0 && g_canvas.last_visible == true);  // pruned + restored
    assert(api->stop() == PGR_CANVAS_OK); balanced(); }

  // 8. Lease expiry and stop() each prune via tick; stop never blocks cleanup.
  { void *h = build_gui(&detailsClass, kStateOpen); arm();
    uint64_t t = 0; assert(api->hide_opening(h, 7, tid, &t) == PGR_CANVAS_HIDDEN);
    assert(api->update(1, 1, 7, tid) == PGR_CANVAS_OK);  // re-arm with a 1ms residual lease
    ::usleep(4000);
    g_canvas = {};
    assert(api->tick(tid) == PGR_CANVAS_OK);
    assert(sample().owned == 0 && g_canvas.last_visible == true);  // expired -> pruned + restored
    balanced(); }
  { void *h = build_gui(&detailsClass, kStateOpen); arm();
    uint64_t t = 0; assert(api->hide_opening(h, 7, tid, &t) == PGR_CANVAS_HIDDEN);
    assert(api->stop() == PGR_CANVAS_OK);
    g_canvas = {};
    assert(api->tick(tid) == PGR_CANVAS_OK);
    assert(sample().owned == 0 && g_canvas.last_visible == true); balanced(); }

  // 9. Non-OPEN is never restored: release/tick free the root without a set-true.
  { void *h = build_gui(&detailsClass, kStateOpen); arm();
    uint64_t t = 0; assert(api->hide_opening(h, 7, tid, &t) == PGR_CANVAS_HIDDEN);
    cur().state = kStateDismissed;  // advanced off OPEN before release
    const uint64_t restore_before = sample().restore_count;
    g_canvas = {};
    assert(api->release(t, tid) == PGR_CANVAS_OK);
    assert(g_canvas.count == 0 && sample().restore_count == restore_before && sample().owned == 0);
    assert(api->stop() == PGR_CANVAS_OK); balanced(); }

  // 10. Off-owner thread never hides, never frees.
  std::thread([&]() {
    void *h = build_gui(); arm();
    uint64_t t = 7;
    assert(g_api->hide_opening(h, 7, this_tid() + 1, &t) == PGR_CANVAS_OK && t == 0);  // mismatched tid
    assert(g_api->release(1, this_tid() + 1) == PGR_CANVAS_ERR_OWNER);
  }).join();
  { // the thread above latched no owner of its own; clean its heap on the real owner.
    assert(api->stop() == PGR_CANVAS_OK); g_handles.clear(); g_host.clear(); }

  // 11. hide_dismissed (one-shot, no slot): SGUI/DGUI + alive + currentState == expected_after (!=OPEN).
  { void *h = build_gui(&summaryClass, kStateDismissed); arm();
    g_canvas = {};
    assert(api->hide_dismissed(h, 7, tid, kStateDismissed) == PGR_CANVAS_HIDDEN);
    assert(g_canvas.count == 1 && g_canvas.last_visible == false);
    assert(sample().owned == 0 && sample().hide_count >= 1);  // never creates a persistent slot
    // STATE_OPEN expected_after is refused outright.
    assert(api->hide_dismissed(h, 7, tid, kStateOpen) == PGR_CANVAS_OK);
    // A currentState that does not match expected_after is refused.
    assert(api->hide_dismissed(h, 7, tid, kStateDismissed + 1) == PGR_CANVAS_OK);
    assert(api->stop() == PGR_CANVAS_OK); balanced(); }

  // 12. Slot overflow refuses the 9th hide and leaves its canvas visible.
  { assert(g_handles.size() == g_host.size()); g_handles.clear(); g_host.clear();
    arm();
    void *held[PGR_CANVAS_MAX_SLOTS];
    for (int i = 0; i < PGR_CANVAS_MAX_SLOTS; ++i) {
      g_guis[i] = {&detailsClass, kStateOpen};
      uintptr_t hb = ++g_seq; g_handles[hb] = &g_guis[i]; g_host.insert(hb); held[i] = reinterpret_cast<void *>(hb);
      uint64_t t = 0; assert(api->hide_opening(held[i], 7, tid, &t) == PGR_CANVAS_HIDDEN && t != 0);
    }
    assert(sample().owned == PGR_CANVAS_MAX_SLOTS);
    Gui overflow{&detailsClass, kStateOpen};
    uintptr_t ho = ++g_seq; g_handles[ho] = &overflow; g_host.insert(ho);
    g_canvas = {};
    uint64_t t = 7;
    assert(api->hide_opening(reinterpret_cast<void *>(ho), 7, tid, &t) == PGR_CANVAS_OK && t == 0);
    assert(g_canvas.count == 0);  // overflow: refused, canvas left visible
    assert(api->stop() == PGR_CANVAS_OK); assert(api->tick(tid) == PGR_CANVAS_OK);
    // scenes[] were not relocated here, so every owned root pruned; only host handles remain.
    for (int i = 0; i < PGR_CANVAS_MAX_SLOTS; ++i) (void)held[i];
    balanced(); }

  // 13. Moving GC inside the alive probe: hide_opening re-resolves its root and still hides once.
  { void *h = build_gui(&detailsClass, kStateOpen); arm();
    g_move_alive = true;
    g_canvas = {};
    uint64_t t = 0;
    assert(api->hide_opening(h, 7, tid, &t) == PGR_CANVAS_HIDDEN && t != 0 && g_cur == 1);
    assert(g_canvas.count == 1 && g_canvas.last_visible == false && g_canvas.last_self == &cur());
    assert(api->release(t, tid) == PGR_CANVAS_OK); balanced(); }

  // 14. Managed set-false throws: refused, token zeroed, slot removed, roots balanced, no HIDDEN count.
  { void *h = build_gui(&detailsClass, kStateOpen); arm();
    const uint64_t hide_before = sample().hide_count;
    g_throw_canvas = true;
    uint64_t t = 7;
    assert(api->hide_opening(h, 7, tid, &t) == PGR_CANVAS_OK && t == 0);
    assert(sample().hide_count == hide_before && sample().owned == 0);
    balanced();
    // alive reporting not-alive also refuses without hiding.
    h = build_gui(&detailsClass, kStateOpen); arm();
    g_live = false; t = 7;
    assert(api->hide_opening(h, 7, tid, &t) == PGR_CANVAS_OK && t == 0 && sample().owned == 0);
    balanced(); }

  // 15. Wrong class refuses (hide_opening requires exact DGUI).
  { void *h = build_gui(&summaryClass, kStateOpen); arm();
    uint64_t t = 7;
    assert(api->hide_opening(h, 7, tid, &t) == PGR_CANVAS_OK && t == 0 && sample().owned == 0);
    assert(api->stop() == PGR_CANVAS_OK); balanced(); }

  { assert(api->stop() == PGR_CANVAS_OK); g_handles.clear(); g_host.clear(); }
  //16. Immutable native onEnter fan-out uses the SAME Dobby owner. Callback GC
  //relocates receiver; original sees refreshed identity exactly once. Its native
  //exception unwinds the39 temporary root and observed-frame gauge.
  const auto *observer_api=static_cast<const PgrCatchCanvasApi2*>(core->query_feature(39,2));
  assert(observer_api&&observer_api->version==2&&observer_api->size==16);
  PgrCatchCanvasObserver observer{1,sizeof(observer),observer_filter,observer_enter,nullptr};
  assert(observer_api->subscribe(nullptr)==PGR_CANVAS_ERR_NULL_ARG);
  assert(observer_api->subscribe(&observer)==0&&observer_api->subscribe(&observer)==0);
  auto changed_observer=observer;changed_observer.context=&g_canvas;
  assert(observer_api->subscribe(&changed_observer)==PGR_CANVAS_ERR_RECONFIG);
  {build_gui(&detailsClass,kStateOpen);g_canvas={};g_observer_move=true;
   call_set_canvas(&cur(),true,&g_game_mi);
   assert(g_observer_calls==1&&g_cur==1&&g_canvas.count==1&&g_canvas.last_self==&cur()&&g_canvas.last_visible&&g_canvas.last_mi==&g_game_mi);
   balanced();assert(sample().owned==0);
  }
  {build_gui(&detailsClass,kStateOpen);g_canvas={};g_observer_move=true;g_original_throw=true;bool caught=false;
   try{call_set_canvas(&cur(),true,&g_game_mi);}catch(int value){assert(value==123);caught=true;}
   assert(caught&&g_observer_calls==2&&g_canvas.count==1&&g_canvas.last_self==&cur());balanced();assert(sample().owned==0);
  }
  {build_gui(&otherClass,kStateOpen);g_canvas={};call_set_canvas(&cur(),false,&g_game_mi);
   assert(g_observer_calls==2&&g_canvas.count==1&&g_canvas.last_self==&cur()&&!g_canvas.last_visible);balanced();assert(sample().owned==0);
  }
  std::printf("catch_canvas_test passed: ABI/validation/idempotence/reconfig, hide_opening (opening "
              "state, set-false unmasked, nonzero token), hook masking (owned/owner/live-epoch only, "
              "orig once with exact self+MethodInfo, no managed invoke), set-false pass-through, "
              "duplicate reuse, release restore OPEN-only (slot removed before set-true), unrelated/"
              "foreign pass-through, epoch/expiry/stop prune+restore via tick, non-OPEN never restored, "
              "off-owner refusal, hide_dismissed one-shot (SGUI/DGUI, expected_after!=OPEN), overflow "
              "refused-visible, moving-GC re-resolution, managed throw/not-alive/wrong-class refusal with "
              "balanced roots\n");
  ::dlclose(lib);
  return 0;
}

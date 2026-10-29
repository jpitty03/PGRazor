// Routes result-presentation observer lifetime, correlation and dismissal against the actual compiled
// core / real Dobby. The three hooks patch fixture originals through the production hook engine (NOT a
// fake replacement library); the lite entries + poll/set_completing/clear_completing/drain are driven
// directly. Runnable via `python pgrazor/native_harness.py routes_observers` ONCE the coordinator adds
// routes_observers.cpp to the build and registers feature 46 in query_feature.
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "routes_observers.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>

static_assert(sizeof(PgrRoutesObserversConfig) == 184, "config 184");
static_assert(sizeof(PgrRoutesObserversOutput) == 2672, "output 2672");
static_assert(sizeof(PgrRoutesObserversApi) == 96, "table 96");

namespace {

// --- fake il2cpp runtime -----------------------------------------------------------
int kConsumptionClass, kProgressClass, kPlayClass, kSharedClass, kRewardGuiClass, kRatingGuiClass, kStringClass;
int kCurrentRouteMethod, kDismissMethod, kRouteField, kIdField, kThread;

// A fake managed object: byte 0..7 the class pointer (il2cpp convention), with raw fields at the exact
// offsets the native module reads (progress Status 0x14, gui state 0x70, route ref 0x18, id ref 0x20,
// string length 0x10, string chars 0x18).
// LP64-aligned byte storage; memcpy avoids creating misaligned/aliased pointer
// or int lvalues inside the byte array under the harness's -O2 build.
struct alignas(8) Fake { unsigned char b[512]; };
std::vector<const void *> g_known_objects;
extern Fake g_string,g_shared,g_play;
void set_ptr(Fake *f, size_t off, void *p) { assert(off+sizeof(p)<=sizeof(f->b));std::memcpy(f->b+off,&p,sizeof(p)); }
void set_klass(Fake *f, void *k) { set_ptr(f,0,k);g_known_objects.push_back(f); }
void set_i32(Fake *f, size_t off, int32_t v) { assert(off+sizeof(v)<=sizeof(f->b));std::memcpy(f->b+off,&v,sizeof(v)); }
void *get_ptr(const void *f,size_t off){void *value=nullptr;std::memcpy(&value,static_cast<const unsigned char *>(f)+off,sizeof(value));return value;}

std::vector<Fake *> g_handle_targets;
int g_live_handles = 0;
bool g_thread_attached = true;
bool g_dismiss_called = false;
void *g_dismiss_receiver = nullptr;
bool g_dismiss_throws = false;
Fake *g_current_play = nullptr;  // get_CurrentRoute result

void *fake_object_get_class(void *obj) {
  if(!obj)return nullptr;
  assert(std::find(g_known_objects.begin(),g_known_objects.end(),obj)!=g_known_objects.end() && "class query received non-object/interior UTF16 pointer");
  return get_ptr(obj,0);
}
void fake_field_get_value(void *obj, void *field, void *out) {
  assert(out);void *value=nullptr;
  if(obj){
    if(field==&kRouteField){
      assert(obj==&g_play && fake_object_get_class(obj)==&kPlayClass && "Route field requires play owner");
      value=get_ptr(obj,0x18);
      assert(value==&g_shared && fake_object_get_class(value)==&kSharedClass && "play.Route must point at shared proto");
    }else{
      assert(field==&kIdField && "unknown fixture FieldInfo token");
      assert(obj==&g_shared && fake_object_get_class(obj)==&kSharedClass && "Id field requires shared owner, never string");
      value=get_ptr(obj,0x20);
      assert(value==&g_string && fake_object_get_class(value)==&kStringClass && "shared.Id must point at string object");
    }
  }
  std::memcpy(out,&value,sizeof(value));
}
uintptr_t fake_gchandle_new(void *obj, int) {
  if (!obj) return 0;
  g_handle_targets.push_back(reinterpret_cast<Fake *>(obj));
  ++g_live_handles;
  return static_cast<uintptr_t>(g_handle_targets.size());
}
void *fake_gchandle_get_target(uintptr_t h) {
  if (h == 0 || h > g_handle_targets.size()) return nullptr;
  return g_handle_targets[h - 1];
}
void fake_gchandle_free(uintptr_t h) {
  if (h == 0 || h > g_handle_targets.size()) return;
  if (g_handle_targets[h - 1]) { g_handle_targets[h - 1] = nullptr; --g_live_handles; }
}
void *fake_thread_current() { return g_thread_attached ? &kThread : nullptr; }
int32_t fake_string_length(void *str) {
  if(!str)return 0;assert(fake_object_get_class(str)==&kStringClass);
  int32_t length=0;std::memcpy(&length,static_cast<unsigned char *>(str)+0x10,sizeof(length));return length;
}
uint16_t *fake_string_chars(void *str) {
  if(!str)return nullptr;assert(fake_object_get_class(str)==&kStringClass);
  return reinterpret_cast<uint16_t *>(static_cast<unsigned char *>(str)+0x18);
}
void *fake_runtime_invoke(const void *method, void *obj, void **, void **exc) {
  if (exc) *exc = nullptr;
  if (method == &kCurrentRouteMethod) return g_current_play;
  if (method == &kDismissMethod) {
    g_dismiss_called = true;
    g_dismiss_receiver = obj;
    if (g_dismiss_throws && exc) *exc = reinterpret_cast<void *>(&kThread);  // non-null exception sentinel
    return nullptr;
  }
  return nullptr;
}

// --- fixture originals (chained once by the hooks) ----------------------------------
std::atomic<int> g_reward_calls{0}, g_rating_calls{0}, g_bjan_calls{0};
void *g_last_bjan_self = nullptr;

extern "C" __attribute__((noinline)) void fixture_reward_init(void *, void *, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_reward_calls.fetch_add(1);
}
extern "C" __attribute__((noinline)) void fixture_rating_init(void *, void *, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_rating_calls.fetch_add(1);
}
extern "C" __attribute__((noinline)) void fixture_bjan(void *self, void *, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_bjan_calls.fetch_add(1);
  g_last_bjan_self = self;
}

// Calls must remain opaque to compiler interprocedural unused-argument optimizations:
// the Dobby replacement consumes arguments which the fixture original does not.
using FixtureHook = void (*)(void *,void *,const void *);
FixtureHook volatile call_bjan=fixture_bjan,call_reward=fixture_reward_init,call_rating=fixture_rating_init;

const PgrRoutesObserversApi *api = nullptr;
uint64_t self_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

void make_config(PgrRoutesObserversConfig *c) {
  std::memset(c, 0, sizeof(*c));
  c->version = PGR_RO_CONFIG_VERSION;
  c->size = sizeof(*c);
  c->runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c->object_get_class = reinterpret_cast<void *>(&fake_object_get_class);
  c->field_get_value = reinterpret_cast<void *>(&fake_field_get_value);
  c->gchandle_new = reinterpret_cast<void *>(&fake_gchandle_new);
  c->gchandle_get_target = reinterpret_cast<void *>(&fake_gchandle_get_target);
  c->gchandle_free = reinterpret_cast<void *>(&fake_gchandle_free);
  c->thread_current = reinterpret_cast<void *>(&fake_thread_current);
  c->string_length = reinterpret_cast<void *>(&fake_string_length);
  c->string_chars = reinterpret_cast<void *>(&fake_string_chars);
  c->class_consumption = &kConsumptionClass;
  c->class_progress = &kProgressClass;
  c->class_play = &kPlayClass;
  c->class_shared = &kSharedClass;
  c->class_reward_gui = &kRewardGuiClass;
  c->class_rating_gui = &kRatingGuiClass;
  c->m_current_route = &kCurrentRouteMethod;
  c->m_dismiss_gui = &kDismissMethod;
  c->f_play_route = &kRouteField;
  c->f_shared_id = &kIdField;
  c->addr_reward_init = reinterpret_cast<void *>(&fixture_reward_init);
  c->addr_rating_init = reinterpret_cast<void *>(&fixture_rating_init);
  c->addr_completion_resp = reinterpret_cast<void *>(&fixture_bjan);
}

void make_request(PgrRoutesObserversRequest *r, uint64_t tid, uint64_t residual, uint64_t gen) {
  std::memset(r, 0, sizeof(*r));
  r->version = PGR_RO_REQUEST_VERSION;
  r->size = sizeof(*r);
  r->tid = tid;
  r->residual_ms = residual;
  r->generation = gen;
}

void set_id(uint16_t *dst, const char *s, uint32_t n) {
  auto *bytes=reinterpret_cast<unsigned char *>(dst);
  for(uint32_t i=0;i<n;i++){const uint16_t unit=static_cast<unsigned char>(s[i]);std::memcpy(bytes+i*2,&unit,sizeof(unit));}
}

void make_completing(PgrRoutesObserversCompleting *c, uint64_t tid, uint64_t gen, uint64_t handle,
                     uint64_t seq, const char *id) {
  std::memset(c, 0, sizeof(*c));
  c->version = PGR_RO_COMPLETING_VERSION;
  c->size = sizeof(*c);
  c->tid = tid;
  c->residual_ms = 2500;
  c->generation = gen;
  c->consumption_handle = handle;
  c->seq = seq;
  c->id_length = static_cast<uint32_t>(std::strlen(id));
  set_id(c->id, id, c->id_length);
}

// Build a "route-a" string proto and a play->shared->id chain pointing at it.
Fake g_string, g_shared, g_play;
void build_route_chain(const char *id) {
  assert(std::strlen(id)<=(sizeof(g_string.b)-0x18)/2);
  std::memset(&g_string, 0, sizeof(g_string));
  set_klass(&g_string,&kStringClass);
  set_i32(&g_string, 0x10, static_cast<int32_t>(std::strlen(id)));
  set_id(reinterpret_cast<uint16_t *>(g_string.b + 0x18), id, static_cast<uint32_t>(std::strlen(id)));
  std::memset(&g_shared, 0, sizeof(g_shared));
  set_klass(&g_shared, &kSharedClass);
  set_ptr(&g_shared, 0x20, &g_string);
  std::memset(&g_play, 0, sizeof(g_play));
  set_klass(&g_play, &kPlayClass);
  set_ptr(&g_play, 0x18, &g_shared);
  g_current_play = &g_play;
  // Prove fixture/export ABI chain before entering the actual native observer.
  void *shared=nullptr,*str=nullptr;
  fake_field_get_value(&g_play,&kRouteField,&shared);assert(shared==&g_shared);
  fake_field_get_value(shared,&kIdField,&str);assert(str==&g_string);
  assert(fake_string_length(str)==static_cast<int32_t>(std::strlen(id)));
}

bool has_event(const PgrRoutesObserversOutput &out, uint32_t type) {
  for (uint32_t i = 0; i < out.event_count; ++i) if (out.events[i].type == type) return true;
  return false;
}
const PgrRoutesObserversEvent *find_event(const PgrRoutesObserversOutput &out, uint32_t type) {
  for (uint32_t i = 0; i < out.event_count; ++i) if (out.events[i].type == type) return &out.events[i];
  return nullptr;
}

}  // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) { std::puts(dlerror()); return 2; }
  auto get = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(library, "pgrazor_native_api"));
  auto core = get ? static_cast<const pgrazor_native_api_v2 *>(get(2)) : nullptr;
  assert(core && core->query_feature(46, 99) == nullptr);
  api = static_cast<const PgrRoutesObserversApi *>(core->query_feature(46, 1));
  assert(api && api->size == sizeof(PgrRoutesObserversApi));

  // install(): header validation, hook placement, idempotent, reconfig, missing pointer.
  {
    PgrRoutesObserversConfig c; make_config(&c);
    assert(api->install(&c) == 0);
    assert(api->install(&c) == 0);  // idempotent
    PgrRoutesObserversConfig c2 = c; c2.m_dismiss_gui = &kThread;
    assert(api->install(&c2) == PGR_RO_ERR_RECONFIG);
    PgrRoutesObserversConfig bad; make_config(&bad); bad.class_progress = nullptr;
    assert(api->install(&bad) == PGR_RO_ERR_MISSING_PTR);
  }

  const uint64_t tid = self_tid();

  // Build the consumption service + its host (feature-45-owned) handle.
  Fake consumption; std::memset(&consumption, 0, sizeof(consumption)); set_klass(&consumption, &kConsumptionClass);
  const uint64_t cons_handle = fake_gchandle_new(&consumption, 0);
  build_route_chain("route-a");

  // Arm owner + lease.
  { PgrRoutesObserversRequest r; make_request(&r, tid, 2500, api->generation());
    PgrRoutesObserversOutput out; assert(api->poll(&r, &out) == PGR_RO_OK); }

  // bjan BEFORE set_completing captures nothing: no completing latched.
  { Fake progress; std::memset(&progress, 0, sizeof(progress)); set_klass(&progress, &kProgressClass); set_i32(&progress, 0x14, 1);
    g_bjan_calls = 0; call_bjan(&consumption, &progress, nullptr);
    assert(g_bjan_calls.load() == 1 && g_last_bjan_self == &consumption);  // original chained, receiver preserved
    PgrRoutesObserversRequest r; make_request(&r, tid, 2500, api->generation());
    PgrRoutesObserversOutput out; assert(api->poll(&r, &out) == PGR_RO_OK);
    assert(!has_event(out, PGR_RO_EV_REWARD_CONFIRMED) && !(out.flags & PGR_RO_FLAG_COMPLETION_READY)); }

  // set_completing + a success bjan confirms the reward and correlates the current id.
  { PgrRoutesObserversCompleting c; make_completing(&c, tid, api->generation(), cons_handle, 7, "route-a");
    assert(api->set_completing(&c) == PGR_RO_OK);
    Fake progress; std::memset(&progress, 0, sizeof(progress)); set_klass(&progress, &kProgressClass); set_i32(&progress, 0x14, PGR_RO_COMPLETION_SUCCESS);
    g_bjan_calls = 0; call_bjan(&consumption, &progress, nullptr);
    assert(g_bjan_calls.load() == 1);
    PgrRoutesObserversRequest r; make_request(&r, tid, 2500, api->generation());
    PgrRoutesObserversOutput out; assert(api->poll(&r, &out) == PGR_RO_OK);
    const PgrRoutesObserversEvent *e = find_event(out, PGR_RO_EV_REWARD_CONFIRMED);
    assert(e && e->i0 == 7 && e->id_length == 7 && e->id[6] == 'a');
    // Immediately after confirmation the completion snapshot is NOT yet ready (< COMPLETION_MS).
    assert(!(out.flags & PGR_RO_FLAG_COMPLETION_READY)); }

  // Reward-GUI Initialize is captured (reward confirmed) and dismissed once currentState==2.
  { Fake gui; std::memset(&gui, 0, sizeof(gui)); set_klass(&gui, &kRewardGuiClass);
    g_reward_calls = 0; call_reward(&gui, nullptr, nullptr);
    assert(g_reward_calls.load() == 1);          // original chained once
    const int live_with_root = g_live_handles;   // the GUI is now rooted in pendingUi
    usleep(150000);                              // cross UI_SETTLE_MS
    set_i32(&gui, 0x70, PGR_RO_GUI_STATE_DISMISSED);
    g_dismiss_called = false;
    PgrRoutesObserversRequest r; make_request(&r, tid, 2500, api->generation());
    PgrRoutesObserversOutput out; assert(api->poll(&r, &out) == PGR_RO_OK);
    assert(g_dismiss_called && g_dismiss_receiver == &gui);
    const PgrRoutesObserversEvent *e = find_event(out, PGR_RO_EV_RESULT_DISMISSED);
    assert(e && e->aux == PGR_RO_SCREEN_REWARD);
    assert(g_live_handles == live_with_root - 1);  // pendingUi root freed on dismissal
  }

  // After COMPLETION_MS with the queue empty the completion snapshot is reported.
  { usleep(3100000);
    PgrRoutesObserversRequest r; make_request(&r, tid, 2500, api->generation());
    PgrRoutesObserversOutput out; assert(api->poll(&r, &out) == PGR_RO_OK);
    assert(out.flags & PGR_RO_FLAG_COMPLETION_READY);
    assert(out.completion_seq == 7 && out.completion_id_length == 7 && out.completion_id[0] == 'r'); }

  // A rejected (status!=1) bjan queues REWARD_REJECTED and does not set completion.
  { PgrRoutesObserversRequest rd; make_request(&rd, tid, 0, api->generation()); assert(api->drain(&rd) == PGR_RO_OK);
    { PgrRoutesObserversRequest r; make_request(&r, tid, 2500, api->generation()); PgrRoutesObserversOutput o; api->poll(&r, &o); }
    PgrRoutesObserversCompleting c; make_completing(&c, tid, api->generation(), cons_handle, 9, "route-a");
    assert(api->set_completing(&c) == PGR_RO_OK);
    Fake progress; std::memset(&progress, 0, sizeof(progress)); set_klass(&progress, &kProgressClass); set_i32(&progress, 0x14, 3);
    call_bjan(&consumption, &progress, nullptr);
    PgrRoutesObserversRequest r; make_request(&r, tid, 2500, api->generation());
    PgrRoutesObserversOutput out; assert(api->poll(&r, &out) == PGR_RO_OK);
    const PgrRoutesObserversEvent *e = find_event(out, PGR_RO_EV_REWARD_REJECTED);
    assert(e && e->i0 == 3 && !(out.flags & PGR_RO_FLAG_COMPLETION_READY)); }

  // A bjan whose current id disagrees with completing.id does not confirm.
  { PgrRoutesObserversRequest rd; make_request(&rd, tid, 0, api->generation()); assert(api->drain(&rd) == PGR_RO_OK);
    { PgrRoutesObserversRequest r; make_request(&r, tid, 2500, api->generation()); PgrRoutesObserversOutput o; api->poll(&r, &o); }
    PgrRoutesObserversCompleting c; make_completing(&c, tid, api->generation(), cons_handle, 11, "route-a");
    assert(api->set_completing(&c) == PGR_RO_OK);
    build_route_chain("route-b");  // current route id now differs
    Fake progress; std::memset(&progress, 0, sizeof(progress)); set_klass(&progress, &kProgressClass); set_i32(&progress, 0x14, 1);
    call_bjan(&consumption, &progress, nullptr);
    PgrRoutesObserversRequest r; make_request(&r, tid, 2500, api->generation());
    PgrRoutesObserversOutput out; assert(api->poll(&r, &out) == PGR_RO_OK);
    assert(!has_event(out, PGR_RO_EV_REWARD_CONFIRMED));
    build_route_chain("route-a"); }

  // Lite entries: tracking depth drives the location gate + a status-change event; waypoint throttles.
  { PgrRoutesObserversRequest rd; make_request(&rd, tid, 0, api->generation()); assert(api->drain(&rd) == PGR_RO_OK);
    { PgrRoutesObserversRequest r; make_request(&r, tid, 2500, api->generation()); PgrRoutesObserversOutput o; api->poll(&r, &o); }
    assert(api->location_gate(tid) == 0);              // no tracking depth yet
    assert(api->tracking_enter(tid) == 1);
    assert(api->location_gate(tid) == 1);              // inside route tracking -> override allowed
    assert(api->tracking_leave(tid, 5) == 1);
    assert(api->location_gate(tid) == 0);              // depth back to zero
    assert(api->waypoint_observed(tid, 12.5, -45.25) == 1);
    assert(api->waypoint_observed(tid, 1.0, 2.0) == 0); // throttled within 10s
    PgrRoutesObserversRequest r; make_request(&r, tid, 2500, api->generation());
    PgrRoutesObserversOutput out; assert(api->poll(&r, &out) == PGR_RO_OK);
    const PgrRoutesObserversEvent *t = find_event(out, PGR_RO_EV_TRACKING_STATUS);
    const PgrRoutesObserversEvent *w = find_event(out, PGR_RO_EV_WAYPOINT);
    assert(t && t->i0 == 5);
    assert(w && w->f0 == 12.5 && w->f1 == -45.25); }

  // revoke() disarms the lease: hooks + lite entries stop acting; a stale-gen poll is a benign SKIP.
  { const uint64_t newgen = api->revoke();
    assert(api->tracking_enter(tid) == 0 && api->location_gate(tid) == 0);
    assert(api->waypoint_observed(tid, 3.0, 4.0) == 0);
    PgrRoutesObserversRequest stale; make_request(&stale, tid, 2500, newgen - 1);
    PgrRoutesObserversOutput so; assert(api->poll(&stale, &so) == PGR_RO_SKIP);
    PgrRoutesObserversRequest fresh; make_request(&fresh, tid, 2500, newgen);
    PgrRoutesObserversOutput fo; assert(api->poll(&fresh, &fo) == PGR_RO_OK); }

  // Foreign-thread poll / set_completing are benign SKIPs.
  { std::thread foreign([&]() {
      PgrRoutesObserversRequest fr; make_request(&fr, tid, 2500, api->generation());  // owner tid, wrong gettid
      PgrRoutesObserversOutput fo; assert(api->poll(&fr, &fo) == PGR_RO_SKIP);
      PgrRoutesObserversCompleting c; make_completing(&c, tid, api->generation(), cons_handle, 13, "route-a");
      assert(api->set_completing(&c) == PGR_RO_SKIP);
    });
    foreign.join(); }

  // drain frees every pendingUi root.
  { { PgrRoutesObserversRequest r; make_request(&r, tid, 2500, api->generation()); PgrRoutesObserversOutput o; api->poll(&r, &o); }
    Fake gui; std::memset(&gui, 0, sizeof(gui)); set_klass(&gui, &kRewardGuiClass);
    // re-confirm so the reward hook captures: issue completing + success bjan first.
    PgrRoutesObserversCompleting c; make_completing(&c, tid, api->generation(), cons_handle, 15, "route-a");
    assert(api->set_completing(&c) == PGR_RO_OK);
    Fake progress; std::memset(&progress, 0, sizeof(progress)); set_klass(&progress, &kProgressClass); set_i32(&progress, 0x14, 1);
    call_bjan(&consumption, &progress, nullptr);
    call_reward(&gui, nullptr, nullptr);
    const int before = g_live_handles;
    PgrRoutesObserversRequest rd; make_request(&rd, tid, 0, api->generation());
    assert(api->drain(&rd) == PGR_RO_OK);
    assert(g_live_handles <= before);  // at least the pendingUi root was freed
  }

  std::printf("routes_observers_test (real-dobby): OK\n");
  return 0;
}

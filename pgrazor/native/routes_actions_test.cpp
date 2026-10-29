#include "routes_actions.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>

// Real-API fake-IL2CPP harness for feature 43 (native Routes managed state/actions). It resolves the
// table through the exported pgrazor_native_api v2 registry (query_feature(43,1)) -- the coordinator
// registers the hidden getter there -- and drives CURRENT / DISCOVER / START / COMPLETE / PAUSE against
// a fake managed heap: a consumption service, a routes service, a RoutePlayProto, a SharedRouteProto,
// its id String and a RoutesConsumptionStartData built by object_new. gchandle_new/get/free back a
// handle table so leak accounting is exact; fake_gc_free ASSERTS a borrowed host handle is never
// freed. runtime_invoke serves the boxed-bool getters and records the mutating invokes, and can throw,
// re-enter (busy) or relocate a rooted object mid-call to exercise re-resolution. The module installs
// NO hook and NEVER calls back into JS.

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class / method / field tokens (distinct addresses) --------------------------
int cls_consumption, cls_routes, cls_play, cls_shared, cls_startData, cls_bool, cls_string;
int m_currentRoute, m_hasStarted, m_inRangeOfEnd, m_inRangeToResume, m_isCompleting, m_flagInitialized,
    m_enabledForPlayer, m_isConsuming, m_isPaused, m_start, m_confirmComplete, m_pause, m_requestNearby,
    m_ctor;
int f_playRoute, f_playPaused, f_playCompletedWalk, f_playAcquiredReward, f_playTravelInReverse,
    f_sharedId, f_sharedReversible, f_dataPublishedRoute, f_dataReversed;

// ----- fake managed objects --------------------------------------------------------
struct FakeString { void *klass; uint16_t chars[PGR_RA_ID_MAX]; int32_t len; };
struct FakeShared { void *klass; void *id; uint8_t reversible; };
struct FakePlay {
  void *klass; void *route;
  uint8_t paused, completedWalk, acquiredReward, travelInReverse;
};
struct FakeConsumption {
  void *klass;
  uint8_t flagInitialized, enabledForPlayer, isConsuming, isPaused;
  uint8_t hasStarted, inRangeOfEnd, inRangeToResume, isCompleting;
  void *currentPlay;  // RoutePlayProto or null
};
struct FakeRoutes { void *klass; };
struct FakeStartData { void *klass; void *published; uint8_t reversed; uint8_t ctor; };
struct FakeBox { void *klass; uint8_t val; };

FakeBox g_true_box{&cls_bool, 1};
FakeBox g_false_box{&cls_bool, 0};

FakeString make_string(const char *ascii) {
  FakeString s{&cls_string, {}, 0};
  for (int i = 0; ascii[i] && i < PGR_RA_ID_MAX; ++i) { s.chars[i] = static_cast<uint16_t>(ascii[i]); ++s.len; }
  return s;
}

// ----- gc handle table -------------------------------------------------------------
std::map<uintptr_t, void *> g_handles;
std::set<uintptr_t> g_host_handles;  // borrowed handles the module must NEVER free
uintptr_t g_handle_seq = 0x900;
uint64_t g_gcnew_calls = 0;
uint64_t g_gcnew_fail_index = 0;  // fail the Nth module gchandle_new (1-based); 0 = never

uintptr_t insert_host_handle(void *obj) { uintptr_t h = ++g_handle_seq; g_handles[h] = obj; g_host_handles.insert(h); return h; }

uintptr_t fake_gc_new(void *obj, int) {
  ++g_gcnew_calls;
  if (obj == nullptr) return 0;
  if (g_gcnew_fail_index != 0 && g_gcnew_calls == g_gcnew_fail_index) return 0;
  uintptr_t h = ++g_handle_seq;
  g_handles[h] = obj;
  return h;
}
void *fake_gc_get(uintptr_t h) { auto it = g_handles.find(h); return it == g_handles.end() ? nullptr : it->second; }
void fake_gc_free(uintptr_t h) {
  assert(g_host_handles.find(h) == g_host_handles.end() && "module freed a borrowed host handle");
  g_handles.erase(h);
}

void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }
void *fake_unbox(void *box) { return box ? &static_cast<FakeBox *>(box)->val : nullptr; }

void *g_thread = reinterpret_cast<void *>(0x7700);  // non-null attached thread (clear to simulate detach)
void *fake_thread_current() { return g_thread; }

// ----- object_new ------------------------------------------------------------------
FakeStartData *g_last_data = nullptr;
bool g_objnew_null = false;
void *fake_obj_new(void *klass) {
  if (g_objnew_null) { g_objnew_null = false; return nullptr; }
  assert(klass == &cls_startData);
  g_last_data = new FakeStartData{&cls_startData, nullptr, 0, 0};
  return g_last_data;
}

// ----- field get / set -------------------------------------------------------------
void fake_field_get(void *obj, void *field, void *out) {
  if (field == &f_playRoute) { *static_cast<void **>(out) = static_cast<FakePlay *>(obj)->route; return; }
  if (field == &f_playPaused) { *static_cast<uint8_t *>(out) = static_cast<FakePlay *>(obj)->paused; return; }
  if (field == &f_playCompletedWalk) { *static_cast<uint8_t *>(out) = static_cast<FakePlay *>(obj)->completedWalk; return; }
  if (field == &f_playAcquiredReward) { *static_cast<uint8_t *>(out) = static_cast<FakePlay *>(obj)->acquiredReward; return; }
  if (field == &f_playTravelInReverse) { *static_cast<uint8_t *>(out) = static_cast<FakePlay *>(obj)->travelInReverse; return; }
  if (field == &f_sharedId) { *static_cast<void **>(out) = static_cast<FakeShared *>(obj)->id; return; }
  if (field == &f_sharedReversible) { *static_cast<uint8_t *>(out) = static_cast<FakeShared *>(obj)->reversible; return; }
  assert(false && "unknown field_get token");
}
void fake_field_set(void *obj, void *field, void *value) {
  assert(field == &f_dataReversed);
  static_cast<FakeStartData *>(obj)->reversed = *static_cast<uint8_t *>(value);
}
void fake_field_set_obj(void *obj, void *field, void *value) {
  assert(field == &f_dataPublishedRoute);
  static_cast<FakeStartData *>(obj)->published = value;
}

int32_t fake_str_len(void *s) { return static_cast<FakeString *>(s)->len; }
uint16_t *fake_str_chars(void *s) { return static_cast<FakeString *>(s)->chars; }

// ----- runtime_invoke --------------------------------------------------------------
int g_start_calls = 0, g_complete_calls = 0, g_discover_calls = 0, g_pause_calls = 0, g_ctor_calls = 0;
void *g_start_arg = nullptr;
uint8_t g_pause_arg = 0xee;
int g_throw_method = 0;                 // token (as int*) that throws; 0 = none
void *g_throw_token = nullptr;
std::function<void()> g_invoke_oneshot; // fired once at the top of the next qualifying invoke
void *g_oneshot_method = nullptr;       // only fire the oneshot for this method token

void *fake_runtime_invoke(const void *method, void *self, void **params, void **exc) {
  if (exc) *exc = nullptr;
  if (g_invoke_oneshot && method == g_oneshot_method) { auto fn = g_invoke_oneshot; g_invoke_oneshot = nullptr; fn(); }
  if (g_throw_token && method == g_throw_token) { g_throw_token = nullptr; if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }

  if (method == &m_currentRoute) return static_cast<FakeConsumption *>(self)->currentPlay;
  auto getter = [&](uint8_t v) -> void * { return v ? static_cast<void *>(&g_true_box) : static_cast<void *>(&g_false_box); };
  FakeConsumption *c = static_cast<FakeConsumption *>(self);
  if (method == &m_hasStarted) return getter(c->hasStarted);
  if (method == &m_inRangeOfEnd) return getter(c->inRangeOfEnd);
  if (method == &m_inRangeToResume) return getter(c->inRangeToResume);
  if (method == &m_isCompleting) return getter(c->isCompleting);
  if (method == &m_flagInitialized) return getter(c->flagInitialized);
  if (method == &m_enabledForPlayer) return getter(c->enabledForPlayer);
  if (method == &m_isConsuming) return getter(c->isConsuming);
  if (method == &m_isPaused) return getter(c->isPaused);
  if (method == &m_ctor) { ++g_ctor_calls; static_cast<FakeStartData *>(self)->ctor = 1; return nullptr; }
  if (method == &m_start) { ++g_start_calls; g_start_arg = params ? params[0] : nullptr; return nullptr; }
  if (method == &m_confirmComplete) { ++g_complete_calls; return nullptr; }
  if (method == &m_requestNearby) { ++g_discover_calls; return nullptr; }
  if (method == &m_pause) { ++g_pause_calls; g_pause_arg = params ? *static_cast<uint8_t *>(params[0]) : 0xff; return nullptr; }
  assert(false && "unknown method token");
  return nullptr;
}

PgrRoutesActionsConfig make_config() {
  PgrRoutesActionsConfig c{};
  c.version = 1; c.size = sizeof(c);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.object_unbox = reinterpret_cast<void *>(&fake_unbox);
  c.gchandle_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gchandle_get_target = reinterpret_cast<void *>(&fake_gc_get);
  c.gchandle_free = reinterpret_cast<void *>(&fake_gc_free);
  c.thread_current = reinterpret_cast<void *>(&fake_thread_current);
  c.object_new = reinterpret_cast<void *>(&fake_obj_new);
  c.field_set_value_object = reinterpret_cast<void *>(&fake_field_set_obj);
  c.field_set_value = reinterpret_cast<void *>(&fake_field_set);
  c.class_consumption = &cls_consumption; c.class_routes = &cls_routes; c.class_play = &cls_play;
  c.class_shared = &cls_shared; c.class_startData = &cls_startData; c.class_bool = &cls_bool;
  c.class_string = &cls_string;
  c.m_currentRoute = &m_currentRoute; c.m_hasStarted = &m_hasStarted; c.m_inRangeOfEnd = &m_inRangeOfEnd;
  c.m_inRangeToResume = &m_inRangeToResume; c.m_isCompleting = &m_isCompleting;
  c.m_flagInitialized = &m_flagInitialized; c.m_enabledForPlayer = &m_enabledForPlayer;
  c.m_isConsuming = &m_isConsuming; c.m_isPaused = &m_isPaused; c.m_start = &m_start;
  c.m_confirmComplete = &m_confirmComplete; c.m_pause = &m_pause; c.m_requestNearby = &m_requestNearby;
  c.m_ctor = &m_ctor;
  c.f_playRoute = &f_playRoute; c.f_playPaused = &f_playPaused; c.f_playCompletedWalk = &f_playCompletedWalk;
  c.f_playAcquiredReward = &f_playAcquiredReward; c.f_playTravelInReverse = &f_playTravelInReverse;
  c.f_sharedId = &f_sharedId; c.f_sharedReversible = &f_sharedReversible;
  c.f_dataPublishedRoute = &f_dataPublishedRoute; c.f_dataReversed = &f_dataReversed;
  c.string_length = reinterpret_cast<void *>(&fake_str_len);
  c.string_chars = reinterpret_cast<void *>(&fake_str_chars);
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

const PgrRoutesActionsApi *g_api = nullptr;
uint64_t g_seq = 0;  // monotonic across the process (the module's high-water never resets)

// Build a request. For a mutating action pass a strictly increasing seq via next_seq(); CURRENT uses 0.
PgrRoutesActionsRequest make_req(uint32_t action, uint64_t tid, uint64_t residual, uint64_t svc_handle,
                                 uint64_t route_handle, uint64_t seq, const char *id, bool reversed) {
  PgrRoutesActionsRequest r{};
  r.version = 1; r.size = sizeof(r);
  r.tid = tid; r.residual_ms = residual; r.generation = g_api->generation();
  r.service_handle = svc_handle; r.route_handle = route_handle; r.seq = seq;
  r.action = action; r.reversed = reversed ? 1 : 0;
  if (id) { for (int i = 0; id[i] && i < PGR_RA_ID_MAX; ++i) { r.id[i] = static_cast<uint16_t>(id[i]); ++r.id_length; } }
  return r;
}
uint64_t next_seq() { return ++g_seq; }

// Assert no module-owned handles leaked: only the borrowed host handles remain.
void assert_no_leak() { assert(g_handles.size() == g_host_handles.size()); }

void reset_invoke_state() {
  g_start_calls = g_complete_calls = g_discover_calls = g_pause_calls = g_ctor_calls = 0;
  g_start_arg = nullptr; g_pause_arg = 0xee; g_throw_token = nullptr;
  g_invoke_oneshot = nullptr; g_oneshot_method = nullptr; g_objnew_null = false; g_gcnew_fail_index = 0;
}

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) { std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror()); return 1; }
  auto getter = reinterpret_cast<GetApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->query_feature);
  assert(core->query_feature(43, 99) == nullptr);
  const auto *api = static_cast<const PgrRoutesActionsApi *>(core->query_feature(43, 1));
  assert(api && api->version == 1 && api->size == sizeof(PgrRoutesActionsApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu request=%zu output=%zu table=%zu\n",
              sizeof(PgrRoutesActionsConfig), sizeof(PgrRoutesActionsRequest),
              sizeof(PgrRoutesActionsOutput), sizeof(PgrRoutesActionsApi));
  assert(sizeof(PgrRoutesActionsConfig) == 352 && sizeof(PgrRoutesActionsRequest) == 328 &&
         sizeof(PgrRoutesActionsOutput) == 280 && sizeof(PgrRoutesActionsApi) == 40);

  const uint64_t tid = this_tid();
  PgrRoutesActionsConfig cfg = make_config();
  PgrRoutesActionsOutput out{};

  // 1. Validation + not-installed + idempotence + reconfig. generation starts at 1.
  assert(api->generation() == 1);
  assert(api->install(nullptr) == PGR_RA_ERR_NULL_ARG);
  { PgrRoutesActionsConfig bad = cfg; bad.version = 2; assert(api->install(&bad) == PGR_RA_ERR_BAD_VERSION); }
  { PgrRoutesActionsConfig bad = cfg; bad.size = sizeof(cfg) - 1; assert(api->install(&bad) == PGR_RA_ERR_BAD_SIZE); }
  { PgrRoutesActionsConfig bad = cfg; bad.m_start = nullptr; assert(api->install(&bad) == PGR_RA_ERR_MISSING_PTR); }
  { PgrRoutesActionsRequest r = make_req(PGR_RA_ACT_CURRENT, tid, 2000, 1, 0, 0, nullptr, false);
    assert(api->execute(&r, &out) == PGR_RA_ERR_NOT_INSTALLED); }
  assert(api->install(&cfg) == PGR_RA_OK);
  assert(api->install(&cfg) == PGR_RA_OK);  // idempotent
  { PgrRoutesActionsConfig bad = cfg; bad.class_play = &cls_shared; assert(api->install(&bad) == PGR_RA_ERR_RECONFIG); }

  // Fake heap: a consumption service (eligible play) + a routes service + a route definition.
  FakeString route_id = make_string("route-abc");
  FakeShared shared{&cls_shared, &route_id, /*reversible=*/1};
  FakePlay play{&cls_play, &shared, /*paused=*/0, /*completedWalk=*/1, /*acquired=*/0, /*reverse=*/0};
  FakeConsumption cons{&cls_consumption, /*init*/1, /*enabled*/1, /*consuming*/1, /*paused*/0,
                       /*started*/1, /*inEnd*/1, /*inResume*/0, /*completing*/0, &play};
  FakeRoutes routes{&cls_routes};
  const uintptr_t hCons = insert_host_handle(&cons);
  const uintptr_t hRoutes = insert_host_handle(&routes);
  const uintptr_t hShared = insert_host_handle(&shared);

  // 2. ABI header rejections (null request / bad version / bad size).
  { PgrRoutesActionsRequest r = make_req(PGR_RA_ACT_CURRENT, tid, 2000, hCons, 0, 0, nullptr, false);
    r.version = 2; assert(api->execute(&r, &out) == PGR_RA_ERR_BAD_VERSION);
    r = make_req(PGR_RA_ACT_CURRENT, tid, 2000, hCons, 0, 0, nullptr, false); r.size = 1;
    assert(api->execute(&r, &out) == PGR_RA_ERR_BAD_SIZE);
    assert(api->execute(nullptr, &out) == PGR_RA_ERR_NULL_ARG); }

  // 3. CURRENT: populated snapshot with every flag mapped, id decoded. Latches the owner.
  { reset_invoke_state();
    cons.hasStarted = 1; cons.inRangeOfEnd = 1; cons.inRangeToResume = 1; cons.isCompleting = 1;
    play.paused = 1; play.completedWalk = 1; play.acquiredReward = 1; play.travelInReverse = 1;
    PgrRoutesActionsRequest r = make_req(PGR_RA_ACT_CURRENT, tid, 2000, hCons, 0, 0, nullptr, false);
    assert(api->execute(&r, &out) == PGR_RA_SUCCESS && out.reason == PGR_RA_REASON_NONE);
    const uint32_t expect = PGR_RA_FLAG_PRESENT | PGR_RA_FLAG_STARTED | PGR_RA_FLAG_PAUSED |
      PGR_RA_FLAG_COMPLETED_WALK | PGR_RA_FLAG_ACQUIRED_REWARD | PGR_RA_FLAG_IN_RANGE_END |
      PGR_RA_FLAG_IN_RANGE_RESUME | PGR_RA_FLAG_IS_COMPLETING | PGR_RA_FLAG_REVERSED;
    assert(out.flags == expect);
    assert(out.id_length == route_id.len && std::memcmp(out.id, route_id.chars, out.id_length * 2) == 0);
    assert_no_leak(); }

  // 3b. CURRENT empty when no play (legacy preservation): flags 0, id_length 0, success.
  { reset_invoke_state(); cons.currentPlay = nullptr;
    PgrRoutesActionsRequest r = make_req(PGR_RA_ACT_CURRENT, tid, 2000, hCons, 0, 0, nullptr, false);
    assert(api->execute(&r, &out) == PGR_RA_SUCCESS && out.flags == 0 && out.id_length == 0);
    cons.currentPlay = &play; assert_no_leak(); }

  // 3c. CURRENT getter exception -> that bool is false (safeBool); the snapshot still publishes.
  { reset_invoke_state();
    cons.hasStarted = 1; play.paused = 0; play.completedWalk = 0; play.acquiredReward = 0; play.travelInReverse = 0;
    cons.inRangeOfEnd = 0; cons.inRangeToResume = 0; cons.isCompleting = 0;
    g_throw_token = &m_hasStarted;  // one throw -> STARTED clears, not a fault
    PgrRoutesActionsRequest r = make_req(PGR_RA_ACT_CURRENT, tid, 2000, hCons, 0, 0, nullptr, false);
    assert(api->execute(&r, &out) == PGR_RA_SUCCESS);
    assert((out.flags & PGR_RA_FLAG_PRESENT) && !(out.flags & PGR_RA_FLAG_STARTED));
    assert_no_leak(); }

  // 3d. Relocating collector: the play object moves during a getter invoke; re-resolution still reads
  //     the live copy, not a stale pointer.
  { reset_invoke_state();
    cons.hasStarted = 0; cons.inRangeOfEnd = 1; cons.inRangeToResume = 0; cons.isCompleting = 0;
    FakePlay *moved = new FakePlay(play);
    g_oneshot_method = &m_hasStarted;
    g_invoke_oneshot = [&]() {
      // repoint every handle at `play` to a fresh copy, then poison the old storage
      for (auto &kv : g_handles) if (kv.second == &play) kv.second = moved;
      cons.currentPlay = moved; play.route = nullptr; play.completedWalk = 0;  // stale read would miss the route + walk
      moved->completedWalk = 1;
    };
    PgrRoutesActionsRequest r = make_req(PGR_RA_ACT_CURRENT, tid, 2000, hCons, 0, 0, nullptr, false);
    assert(api->execute(&r, &out) == PGR_RA_SUCCESS);
    assert((out.flags & PGR_RA_FLAG_PRESENT) && (out.flags & PGR_RA_FLAG_COMPLETED_WALK) && out.id_length == route_id.len);
    cons.currentPlay = &play; play.route = &shared; play.completedWalk = 1; delete moved; assert_no_leak(); }

  // 4. DISCOVER: requestNearby on the routes service, submitted. Wrong service class -> awaiting-service.
  { reset_invoke_state();
    PgrRoutesActionsRequest r = make_req(PGR_RA_ACT_DISCOVER, tid, 2000, hRoutes, 0, next_seq(), nullptr, false);
    assert(api->execute(&r, &out) == PGR_RA_SUCCESS && g_discover_calls == 1);
    // consumption handle supplied for DISCOVER -> wrong class -> awaiting-service.
    r = make_req(PGR_RA_ACT_DISCOVER, tid, 2000, hCons, 0, next_seq(), nullptr, false);
    assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_AWAITING_SVC);
    assert_no_leak(); }

  // 5. START: full path builds start data and invokes StartRoute with the data reference directly.
  { reset_invoke_state();
    cons.isConsuming = 0;  // not already active
    PgrRoutesActionsRequest r = make_req(PGR_RA_ACT_START, tid, 2000, hCons, hShared, next_seq(), "route-abc", true);
    assert(api->execute(&r, &out) == PGR_RA_SUCCESS);
    assert(g_ctor_calls == 1 && g_start_calls == 1 && g_last_data && g_last_data->ctor == 1);
    assert(g_last_data->published == &shared && g_last_data->reversed == 1);
    assert(g_start_arg == g_last_data);  // argument[0] is the data object itself, not a pointer-to-pointer
    assert_no_leak(); }

  // 5a. START refusals, each with its reason; none reaches StartRoute.
  { reset_invoke_state();
    cons.isConsuming = 0;
    auto start = [&](const char *id, bool rev) { return make_req(PGR_RA_ACT_START, tid, 2000, hCons, hShared, next_seq(), id, rev); };
    cons.flagInitialized = 0;
    { auto r = start("route-abc", false); assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_NOT_INIT); }
    cons.flagInitialized = 1; cons.enabledForPlayer = 0;
    { auto r = start("route-abc", false); assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_NOT_PERMITTED); }
    cons.enabledForPlayer = 1; cons.isConsuming = 1;
    { auto r = start("route-abc", false); assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_ALREADY_ACTIVE); }
    cons.isConsuming = 0;
    { auto r = make_req(PGR_RA_ACT_START, tid, 2000, hCons, 0, next_seq(), "route-abc", false);
      assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_NOT_DISCOVERED); }
    { auto r = start("route-xyz", false); assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_ID_MISMATCH); }
    shared.reversible = 0;
    { auto r = start("route-abc", true); assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_NOT_REVERSIBLE); }
    shared.reversible = 1;
    assert(g_start_calls == 0 && g_ctor_calls == 0);
    // alloc-failed: object_new returns null -> FAULT, reason alloc-failed, before StartRoute.
    g_objnew_null = true;
    { auto r = start("route-abc", false); assert(api->execute(&r, &out) == PGR_RA_FAULT && out.reason == PGR_RA_REASON_ALLOC_FAILED); }
    assert(g_start_calls == 0); assert_no_leak(); }

  // 5b. START with a stale route handle (resolves null) -> route-stale.
  { reset_invoke_state(); cons.isConsuming = 0;
    FakeShared tmp{&cls_shared, &route_id, 1};
    uintptr_t hTmp = insert_host_handle(&tmp);
    g_handles[hTmp] = nullptr;  // simulate a collected target
    auto r = make_req(PGR_RA_ACT_START, tid, 2000, hCons, hTmp, next_seq(), "route-abc", false);
    assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_ROUTE_STALE);
    g_handles.erase(hTmp); g_host_handles.erase(hTmp); }

  // 6. COMPLETE: eligibility matrix + confirmComplete once. cons.currentPlay/play drive the state.
  { reset_invoke_state();
    cons.currentPlay = &play;
    play.paused = 0; play.completedWalk = 1; play.acquiredReward = 0; cons.isCompleting = 0; cons.hasStarted = 1; cons.inRangeOfEnd = 1;
    auto complete = [&](const char *id) { return make_req(PGR_RA_ACT_COMPLETE, tid, 2000, hCons, 0, next_seq(), id, false); };
    { auto r = complete("route-abc"); assert(api->execute(&r, &out) == PGR_RA_SUCCESS && g_complete_calls == 1); }
    // id mismatch.
    { auto r = complete("route-xyz"); assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_ID_MISMATCH); }
    // no current.
    cons.currentPlay = nullptr;
    { auto r = complete("route-abc"); assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_NO_CURRENT); }
    cons.currentPlay = &play;
    // already completing / rewarded / not eligible.
    cons.isCompleting = 1; { auto r = complete("route-abc"); assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_COMPLETING); }
    cons.isCompleting = 0; play.acquiredReward = 1; { auto r = complete("route-abc"); assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_REWARDED); }
    play.acquiredReward = 0; play.completedWalk = 0; { auto r = complete("route-abc"); assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_NOT_ELIGIBLE); }
    play.completedWalk = 1; cons.inRangeOfEnd = 0; { auto r = complete("route-abc"); assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_NOT_ELIGIBLE); }
    cons.inRangeOfEnd = 1;
    assert(g_complete_calls == 1); assert_no_leak(); }

  // 7. PAUSE: isConsuming && !isPaused -> PauseRoute(true); plus refusals.
  { reset_invoke_state();
    cons.isConsuming = 1; cons.isPaused = 0;
    { auto r = make_req(PGR_RA_ACT_PAUSE, tid, 2000, hCons, 0, next_seq(), nullptr, false);
      assert(api->execute(&r, &out) == PGR_RA_SUCCESS && g_pause_calls == 1 && g_pause_arg == 1); }
    cons.isConsuming = 0;
    { auto r = make_req(PGR_RA_ACT_PAUSE, tid, 2000, hCons, 0, next_seq(), nullptr, false);
      assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_NO_CURRENT); }
    cons.isConsuming = 1; cons.isPaused = 1;
    { auto r = make_req(PGR_RA_ACT_PAUSE, tid, 2000, hCons, 0, next_seq(), nullptr, false);
      assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_ALREADY_PAUSED); }
    cons.isPaused = 0; assert(g_pause_calls == 1); assert_no_leak(); }

  // 8. Lease / gate: residual 0, expired residual, detached thread, awaiting-service, generation revoke.
  { reset_invoke_state(); cons.isConsuming = 1;
    { auto r = make_req(PGR_RA_ACT_PAUSE, tid, 0, hCons, 0, next_seq(), nullptr, false);
      assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_LEASE_EXPIRED && g_pause_calls == 0); }
    // Expiry DURING the read: a getter invoke sleeps past the residual budget, so the next
    // preflight aborts the CURRENT read -> refuse lease-expired (deadline is sampled at entry from
    // residual_ms, so the sleep must happen inside the call, not before it).
    { reset_invoke_state();
      g_oneshot_method = &m_hasStarted; g_invoke_oneshot = [&]() { ::usleep(20000); };
      auto r = make_req(PGR_RA_ACT_CURRENT, tid, 5, hCons, 0, 0, nullptr, false);
      assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_LEASE_EXPIRED); }
    // detached thread.
    g_thread = nullptr;
    { auto r = make_req(PGR_RA_ACT_CURRENT, tid, 2000, hCons, 0, 0, nullptr, false);
      assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_LEASE_EXPIRED); }
    g_thread = reinterpret_cast<void *>(0x7700);
    // awaiting-service (null service target).
    { uintptr_t hNull = insert_host_handle(nullptr);
      auto r = make_req(PGR_RA_ACT_CURRENT, tid, 2000, hNull, 0, 0, nullptr, false);
      assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_AWAITING_SVC);
      g_handles.erase(hNull); g_host_handles.erase(hNull); }
    // revoke: a request stamped with the prior generation now refuses lease-expired.
    PgrRoutesActionsRequest stale = make_req(PGR_RA_ACT_CURRENT, tid, 2000, hCons, 0, 0, nullptr, false);
    const uint64_t g1 = api->generation(); assert(api->revoke() == g1 + 1 && api->generation() == g1 + 1);
    assert(api->execute(&stale, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_LEASE_EXPIRED);
    // a freshly-stamped request works again.
    { auto r = make_req(PGR_RA_ACT_CURRENT, tid, 2000, hCons, 0, 0, nullptr, false);
      assert(api->execute(&r, &out) == PGR_RA_SUCCESS); }
    assert_no_leak(); }

  // 9. Stale-seq: a repeated / backward mutating seq is refused; CURRENT (seq 0) is exempt. The
  //    high-water is consumed even when the subsequent action refuses.
  { reset_invoke_state(); cons.isConsuming = 1; cons.isPaused = 0;
    const uint64_t s = next_seq();
    { auto r = make_req(PGR_RA_ACT_PAUSE, tid, 2000, hCons, 0, s, nullptr, false);
      assert(api->execute(&r, &out) == PGR_RA_SUCCESS); }
    { auto r = make_req(PGR_RA_ACT_PAUSE, tid, 2000, hCons, 0, s, nullptr, false);  // replay
      assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_STALE_SEQ); }
    { auto r = make_req(PGR_RA_ACT_PAUSE, tid, 2000, hCons, 0, s - 1, nullptr, false);  // backward
      assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_STALE_SEQ); }
    // a refused mutation (awaiting-service) still consumes its seq: a replay is stale.
    cons.isPaused = 0; const uint64_t s2 = next_seq();
    { uintptr_t hNull = insert_host_handle(nullptr);
      auto r = make_req(PGR_RA_ACT_PAUSE, tid, 2000, hNull, 0, s2, nullptr, false);
      assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_AWAITING_SVC);
      g_handles.erase(hNull); g_host_handles.erase(hNull); }
    { auto r = make_req(PGR_RA_ACT_PAUSE, tid, 2000, hCons, 0, s2, nullptr, false);  // seq already consumed
      assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_STALE_SEQ); }
    // CURRENT ignores the high-water entirely.
    { auto r = make_req(PGR_RA_ACT_CURRENT, tid, 2000, hCons, 0, 0, nullptr, false);
      assert(api->execute(&r, &out) == PGR_RA_SUCCESS); }
    assert_no_leak(); }

  // 10. Off-owner thread: refused (owner already latched to main), no mutation, no seq consumed.
  reset_invoke_state();
  std::thread([&]() {
    PgrRoutesActionsOutput o{};
    auto r = make_req(PGR_RA_ACT_PAUSE, this_tid(), 2000, hCons, 0, next_seq(), nullptr, false);
    assert(g_api->execute(&r, &o) == PGR_RA_REFUSE && o.reason == PGR_RA_REASON_LEASE_EXPIRED);
    assert(g_pause_calls == 0);
  }).join();

  // 11. Managed throw on the mutating invoke -> FAULT native-error; roots freed; no retry implied.
  { reset_invoke_state(); cons.isConsuming = 1; cons.isPaused = 0;
    g_throw_token = &m_pause;
    auto r = make_req(PGR_RA_ACT_PAUSE, tid, 2000, hCons, 0, next_seq(), nullptr, false);
    assert(api->execute(&r, &out) == PGR_RA_FAULT && out.reason == PGR_RA_REASON_NATIVE_ERROR);
    assert(g_pause_calls == 0 && g_throw_token == nullptr); assert_no_leak(); }

  // 12. Reentrancy (busy): a nested CURRENT fired from inside StartRoute is refused; the outer START
  //     still submits exactly once (the one invoke already happened).
  { reset_invoke_state(); cons.isConsuming = 0;
    int nested_rc = 99; PgrRoutesActionsOutput nested{};
    g_oneshot_method = &m_start;
    g_invoke_oneshot = [&]() {
      auto nr = make_req(PGR_RA_ACT_CURRENT, tid, 2000, hCons, 0, 0, nullptr, false);
      nested_rc = g_api->execute(&nr, &nested);
    };
    auto r = make_req(PGR_RA_ACT_START, tid, 2000, hCons, hShared, next_seq(), "route-abc", false);
    assert(api->execute(&r, &out) == PGR_RA_SUCCESS && g_start_calls == 1);
    assert(nested_rc == PGR_RA_FAULT && nested.reason == PGR_RA_REASON_NATIVE_ERROR);  // busy refused
    assert_no_leak(); }

  // 13. Revoke mid-construction: a revoke fired from inside the ctor aborts START BEFORE StartRoute
  //     (pre-invoke must be eligible-fresh); every root is freed.
  { reset_invoke_state(); cons.isConsuming = 0;
    g_oneshot_method = &m_ctor;
    g_invoke_oneshot = [&]() { g_api->revoke(); };
    auto r = make_req(PGR_RA_ACT_START, tid, 2000, hCons, hShared, next_seq(), "route-abc", false);
    assert(api->execute(&r, &out) == PGR_RA_REFUSE && out.reason == PGR_RA_REASON_LEASE_EXPIRED);
    assert(g_ctor_calls == 1 && g_start_calls == 0);  // aborted without invoking StartRoute
    assert_no_leak(); }

  // 14. Revoke DURING the mutating invoke: already issued -> still SUBMITTED, never un-done.
  { reset_invoke_state(); cons.isConsuming = 1; cons.isPaused = 0;
    g_oneshot_method = &m_pause;
    g_invoke_oneshot = [&]() { g_api->revoke(); };  // fires at the top of the pause invoke
    auto r = make_req(PGR_RA_ACT_PAUSE, tid, 2000, hCons, 0, next_seq(), nullptr, false);
    assert(api->execute(&r, &out) == PGR_RA_SUCCESS && g_pause_calls == 1);
    assert_no_leak(); }

  // 15. Root-allocation failure before the action fails closed (no invoke).
  { reset_invoke_state(); cons.isConsuming = 1; cons.isPaused = 0;
    g_gcnew_calls = 0; g_gcnew_fail_index = 1;  // first module root (the service) fails
    auto r = make_req(PGR_RA_ACT_PAUSE, tid, 2000, hCons, 0, next_seq(), nullptr, false);
    assert(api->execute(&r, &out) == PGR_RA_FAULT && out.reason == PGR_RA_REASON_NATIVE_ERROR);
    assert(g_pause_calls == 0); assert_no_leak(); }

  puts("PASS routes actions: ABI(352/328/280/40)+asserts, install/idempotent/reconfig, CURRENT "
       "flags/empty/safeBool/relocation, DISCOVER, START full+refusals+alloc+stale-handle, COMPLETE "
       "eligibility, PAUSE, lease/residual/thread/awaiting-service/generation-revoke, stale-seq "
       "high-water, off-owner refuse, managed throw, busy reentry, revoke mid-ctor(abort) vs "
       "mid-invoke(submitted), root-alloc failure; borrowed handles never freed, zero leaks");
  dlclose(lib);
  return 0;
}

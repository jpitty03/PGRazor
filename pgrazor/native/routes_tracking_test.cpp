// Route-tracking / location-override / waypoint hook ABI, nesting, off-owner, revoke, waypoint proof and
// stale/no-heartbeat behaviour against the actual compiled core / real Dobby. Feature 50's three hooks are
// placed on fixture originals through the production hook engine (NOT a fake replacement library); the
// forwarded state is observed through the real feature-46 routes_observers table. Runnable via
// `python pgrazor/native_harness.py routes_tracking` ONCE the coordinator adds routes_tracking.cpp to the
// build and registers features 46 + 50 in query_feature.
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

#include "routes_tracking.h"
#include "routes_observers.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>

static_assert(sizeof(PgrRoutesTrackingConfig) == 64, "config 64");
static_assert(sizeof(PgrRoutesTrackingApi) == 24, "table 24");

namespace {

// --- minimal fake il2cpp runtime (enough to install + lease + poll feature 46) -----
int kConsumptionClass, kProgressClass, kPlayClass, kSharedClass, kRewardGuiClass, kRatingGuiClass;
int kCurrentRouteMethod, kDismissMethod, kRouteField, kIdField, kThread;
int kWaypointClass;  // feature-50 RouteWaypointProto class sentinel

bool g_thread_attached = true;

void *fake_object_get_class(void *obj) { return obj ? *reinterpret_cast<void **>(obj) : nullptr; }
void fake_field_get_value(void *, void *, void *out) { *reinterpret_cast<void **>(out) = nullptr; }
uintptr_t fake_gchandle_new(void *obj, int) { return obj ? reinterpret_cast<uintptr_t>(obj) : 0; }
void *fake_gchandle_get_target(uintptr_t h) { return reinterpret_cast<void *>(h); }
void fake_gchandle_free(uintptr_t) {}
void *fake_thread_current() { return g_thread_attached ? &kThread : nullptr; }
int32_t fake_string_length(void *) { return 0; }
uint16_t *fake_string_chars(void *) { return nullptr; }
void *fake_runtime_invoke(const void *, void *, void **, void **exc) { if (exc) *exc = nullptr; return nullptr; }

// --- feature-46 hook fixtures (patched by feature 46's install; never called here) --
extern "C" __attribute__((noinline)) void obs_fixture_reward(void *, void *, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
}
extern "C" __attribute__((noinline)) void obs_fixture_rating(void *, void *, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
}
extern "C" __attribute__((noinline)) void obs_fixture_bjan(void *, void *, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
}

// --- feature-50 hooked originals ---------------------------------------------------
// The tracking original internally invokes the (patched) location icall so the nested location override is
// exercised exactly as the real CheckTrackingErrorStatus calls IsServiceEnabledByUser.
extern "C" __attribute__((noinline)) uint8_t fixture_location(void);

std::atomic<int32_t> g_tracking_code{0};
std::atomic<uint8_t> g_location_base{0};
std::atomic<uint8_t> g_loc_seen_inside_tracking{0};
std::atomic<int> g_tracking_calls{0}, g_waypoint_calls{0}, g_location_calls{0};
std::atomic<void *> g_last_wp_self{nullptr}, g_last_wp_arg{nullptr};
const void *g_last_wp_method = nullptr;

using LocationCall=uint8_t(*)(void);
LocationCall volatile call_location=fixture_location;
extern "C" __attribute__((noinline)) int32_t fixture_tracking(void *self, const void *method) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_tracking_calls.fetch_add(1);
  (void)self; (void)method;
  g_loc_seen_inside_tracking.store(call_location());  // nested icall, mirrors the real body
  return g_tracking_code.load();
}
extern "C" __attribute__((noinline)) void fixture_waypoint(void *self, void *waypoint, const void *method) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_waypoint_calls.fetch_add(1);
  g_last_wp_self.store(self);
  g_last_wp_arg.store(waypoint);
  g_last_wp_method = method;
}
extern "C" __attribute__((noinline)) uint8_t fixture_location(void) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_location_calls.fetch_add(1);
  return g_location_base.load();
}

// A fake managed RouteWaypointProto: class pointer at byte 0, LatDegrees @0x18, LngDegrees @0x20.
struct FakeWaypoint { unsigned char b[64]; };
void set_klass(void *o, void *k) { *reinterpret_cast<void **>(o) = k; }
void set_f64(void *o, size_t off, double v) { *reinterpret_cast<double *>(reinterpret_cast<unsigned char *>(o) + off) = v; }

const PgrRoutesObserversApi *obs = nullptr;
const PgrRoutesTrackingApi *trk = nullptr;
uint64_t self_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

void make_obs_config(PgrRoutesObserversConfig *c) {
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
  c->addr_reward_init = reinterpret_cast<void *>(&obs_fixture_reward);
  c->addr_rating_init = reinterpret_cast<void *>(&obs_fixture_rating);
  c->addr_completion_resp = reinterpret_cast<void *>(&obs_fixture_bjan);
}

void make_trk_config(PgrRoutesTrackingConfig *c) {
  std::memset(c, 0, sizeof(*c));
  c->version = PGR_RT_CONFIG_VERSION;
  c->size = sizeof(*c);
  c->object_get_class = reinterpret_cast<void *>(&fake_object_get_class);
  c->class_waypoint = &kWaypointClass;
  c->addr_tracking = reinterpret_cast<void *>(&fixture_tracking);
  c->addr_waypoint = reinterpret_cast<void *>(&fixture_waypoint);
  c->addr_location_icall = reinterpret_cast<void *>(&fixture_location);
  c->lat_offset = PGR_RT_WAYPOINT_LAT_OFFSET;
  c->lng_offset = PGR_RT_WAYPOINT_LNG_OFFSET;
}

void make_request(PgrRoutesObserversRequest *r, uint64_t tid, uint64_t residual, uint64_t gen) {
  std::memset(r, 0, sizeof(*r));
  r->version = PGR_RO_REQUEST_VERSION;
  r->size = sizeof(*r);
  r->tid = tid;
  r->residual_ms = residual;
  r->generation = gen;
}

const PgrRoutesObserversEvent *find_event(const PgrRoutesObserversOutput &out, uint32_t type) {
  for (uint32_t i = 0; i < out.event_count; ++i) if (out.events[i].type == type) return &out.events[i];
  return nullptr;
}
bool has_event(const PgrRoutesObserversOutput &out, uint32_t type) { return find_event(out, type) != nullptr; }

// Lease + drain helpers for feature 46.
void obs_poll(uint64_t tid, uint64_t residual, PgrRoutesObserversOutput *out) {
  PgrRoutesObserversRequest r; make_request(&r, tid, residual, obs->generation());
  obs->poll(&r, out);
}
void obs_lease(uint64_t tid) { PgrRoutesObserversOutput o; obs_poll(tid, 2500, &o); }
void obs_drain(uint64_t tid) { PgrRoutesObserversRequest r; make_request(&r, tid, 0, obs->generation()); obs->drain(&r); }

}  // namespace

using TrackingCall=int32_t(*)(void *,const void *);
using WaypointCall=void(*)(void *,void *,const void *);
TrackingCall volatile call_tracking=fixture_tracking;
WaypointCall volatile call_waypoint=fixture_waypoint;

int main(int argc, char **argv) {
  assert(argc == 2);
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) { std::puts(dlerror()); return 2; }
  auto get = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(library, "pgrazor_native_api"));
  auto core = get ? static_cast<const pgrazor_native_api_v2 *>(get(2)) : nullptr;
  assert(core && core->query_feature(50, 99) == nullptr);
  obs = static_cast<const PgrRoutesObserversApi *>(core->query_feature(46, 1));
  trk = static_cast<const PgrRoutesTrackingApi *>(core->query_feature(50, 1));
  assert(obs && obs->size == sizeof(PgrRoutesObserversApi));
  assert(trk && trk->size == sizeof(PgrRoutesTrackingApi));

  const uint64_t tid = self_tid();

  // Install feature 46 first so the forwarded lite state has somewhere to live, then feature 50.
  { PgrRoutesObserversConfig oc; make_obs_config(&oc); assert(obs->install(&oc) == 0); }

  // install(): header validation, idempotent, reconfig, missing pointer, bad offset, dup address.
  { PgrRoutesTrackingConfig tc; make_trk_config(&tc);
    assert(trk->installed() == 0);
    assert(trk->install(&tc) == PGR_RT_OK);
    assert(trk->installed() == 1);
    assert(trk->install(&tc) == PGR_RT_OK);                       // idempotent
    PgrRoutesTrackingConfig re = tc; re.lat_offset = 0x28; assert(trk->install(&re) == PGR_RT_ERR_RECONFIG);
    PgrRoutesTrackingConfig miss; make_trk_config(&miss); miss.class_waypoint = nullptr;
    assert(trk->install(&miss) == PGR_RT_ERR_MISSING_PTR);
    PgrRoutesTrackingConfig badoff; make_trk_config(&badoff); badoff.lng_offset = badoff.lat_offset;
    assert(trk->install(&badoff) == PGR_RT_ERR_BAD_OFFSET);
    PgrRoutesTrackingConfig badoff2; make_trk_config(&badoff2); badoff2.lat_offset = 0x19;  // unaligned
    assert(trk->install(&badoff2) == PGR_RT_ERR_BAD_OFFSET);
    PgrRoutesTrackingConfig dup; make_trk_config(&dup); dup.addr_waypoint = dup.addr_tracking;
    assert(trk->install(&dup) == PGR_RT_ERR_DUP_ADDR);
    assert(trk->install(&tc) == PGR_RT_OK); }                     // still the original config

  // --- passthrough ABI (feature 46 NOT leased): originals chained + returns/args exact, no override ----
  obs_drain(tid);  // depth/lease cleared; no heartbeat
  g_tracking_code.store(7); g_location_base.store(0);
  g_tracking_calls = 0; g_location_calls = 0;
  assert(call_tracking(nullptr, nullptr) == 7);               // exact ErrorType returned
  assert(g_tracking_calls.load() == 1);
  // tracking body invoked the nested location icall once; with no lease the override must NOT fire.
  assert(g_location_calls.load() == 1 && g_loc_seen_inside_tracking.load() == 0);
  assert(call_location() == 0);                               // outside tracking, unchanged
  { FakeWaypoint wp; std::memset(&wp, 0, sizeof(wp)); set_klass(&wp, &kWaypointClass);
    set_f64(&wp, 0x18, 51.5); set_f64(&wp, 0x20, -0.125);
    g_waypoint_calls = 0; int sentinel = 0;
    call_waypoint(&wp, &wp, &sentinel);
    assert(g_waypoint_calls.load() == 1);                        // original chained
    assert(g_last_wp_self.load() == &wp && g_last_wp_arg.load() == &wp && g_last_wp_method == &sentinel); }
  { PgrRoutesObserversOutput o; obs_poll(tid, 0, &o);
    assert(!has_event(o, PGR_RO_EV_WAYPOINT) && !has_event(o, PGR_RO_EV_TRACKING_STATUS)); }

  // --- nested tracking + location override + tracking-status event (feature 46 leased) ----
  obs_drain(tid); obs_lease(tid);
  g_tracking_code.store(5); g_location_base.store(0);
  assert(obs->location_gate(tid) == 0);                          // no depth outside a tracking call
  assert(call_tracking(nullptr, nullptr) == 5);               // original return preserved
  // The nested icall saw the override (depth>0 && lease live -> IsServiceEnabledByUser forced true).
  assert(g_loc_seen_inside_tracking.load() == 1);
  assert(obs->location_gate(tid) == 0);                          // depth balanced back to zero after leave
  assert(call_location() == 0);                               // outside tracking: original unchanged
  { PgrRoutesObserversOutput o; obs_poll(tid, 2500, &o);
    const PgrRoutesObserversEvent *t = find_event(o, PGR_RO_EV_TRACKING_STATUS);
    assert(t && t->i0 == 5); }

  // --- waypoint coordinates proof + 10s throttle ----
  obs_drain(tid); obs_lease(tid);
  { FakeWaypoint wp; std::memset(&wp, 0, sizeof(wp)); set_klass(&wp, &kWaypointClass);
    set_f64(&wp, 0x18, 12.5); set_f64(&wp, 0x20, -45.25);
    g_waypoint_calls = 0;
    call_waypoint(&wp, &wp, nullptr);
    assert(g_waypoint_calls.load() == 1);                        // original still chained
    // A second waypoint within 10s is throttled by feature 46 (no second event), original still chained.
    set_f64(&wp, 0x18, 1.0); set_f64(&wp, 0x20, 2.0);
    call_waypoint(&wp, &wp, nullptr);
    assert(g_waypoint_calls.load() == 2);
    PgrRoutesObserversOutput o; obs_poll(tid, 2500, &o);
    const PgrRoutesObserversEvent *w = find_event(o, PGR_RO_EV_WAYPOINT);
    assert(w && w->f0 == 12.5 && w->f1 == -45.25);               // first coords, not the throttled second
    PgrRoutesObserversOutput o2; obs_poll(tid, 2500, &o2);
    assert(!has_event(o2, PGR_RO_EV_WAYPOINT)); }

  // --- wrong-class waypoint arg: no event, original still chained with args unchanged ----
  obs_drain(tid); obs_lease(tid);
  { FakeWaypoint wp; std::memset(&wp, 0, sizeof(wp)); set_klass(&wp, &kProgressClass);  // not RouteWaypointProto
    set_f64(&wp, 0x18, 9.0); set_f64(&wp, 0x20, 9.0);
    g_waypoint_calls = 0; int sentinel = 0;
    call_waypoint(&wp, &wp, &sentinel);
    assert(g_waypoint_calls.load() == 1 && g_last_wp_arg.load() == &wp && g_last_wp_method == &sentinel);
    PgrRoutesObserversOutput o; obs_poll(tid, 2500, &o);
    assert(!has_event(o, PGR_RO_EV_WAYPOINT)); }

  // --- off-owner (non-attached thread): feature 46's lite entries gate on thread attachment, so an
  // unattached caller forwards nothing and triggers no override, yet the originals are still chained. ----
  obs_drain(tid); obs_lease(tid);
  { std::thread foreign([&]() {
      g_thread_attached = false;                                 // il2cpp_thread_current()==null for this thread
      g_tracking_code.store(3);
      assert(call_tracking(nullptr, nullptr) == 3);           // original preserved off an attached thread
      assert(g_loc_seen_inside_tracking.load() == 0);            // no nested override (depth never counted)
      FakeWaypoint wp; std::memset(&wp, 0, sizeof(wp)); set_klass(&wp, &kWaypointClass);
      set_f64(&wp, 0x18, 7.0); set_f64(&wp, 0x20, 8.0);
      g_waypoint_calls = 0; call_waypoint(&wp, &wp, nullptr);
      assert(g_waypoint_calls.load() == 1);
      g_thread_attached = true;
    });
    foreign.join();
    // The owner thread sees no tracking/waypoint event from the unattached foreign activity.
    PgrRoutesObserversOutput o; obs_poll(tid, 2500, &o);
    assert(!has_event(o, PGR_RO_EV_WAYPOINT)); }

  // --- revoke disarms: nested override stops, waypoint stops; originals untouched ----
  obs_lease(tid);
  obs->revoke();
  g_tracking_code.store(4); g_location_base.store(0);
  assert(call_tracking(nullptr, nullptr) == 4);               // original return preserved
  assert(g_loc_seen_inside_tracking.load() == 0);                // no override after revoke
  assert(call_location() == 0);
  { FakeWaypoint wp; std::memset(&wp, 0, sizeof(wp)); set_klass(&wp, &kWaypointClass);
    set_f64(&wp, 0x18, 3.0); set_f64(&wp, 0x20, 4.0);
    g_waypoint_calls = 0; call_waypoint(&wp, &wp, nullptr);
    assert(g_waypoint_calls.load() == 1);
    PgrRoutesObserversOutput o;
    PgrRoutesObserversRequest r; make_request(&r, tid, 2500, obs->generation());  // fresh gen
    obs->poll(&r, &o);
    assert(!has_event(o, PGR_RO_EV_WAYPOINT)); }

  // --- stale / no heartbeat: drained (lease off) -> override + waypoint inert, originals exact ----
  obs_drain(tid);  // no poll refresh afterwards => lease expired
  g_tracking_code.store(6); g_location_base.store(1);            // original now reports "enabled"
  assert(call_tracking(nullptr, nullptr) == 6);
  assert(g_loc_seen_inside_tracking.load() == 1);                // == the ORIGINAL value, not an override
  assert(call_location() == 1);                               // unchanged original
  { FakeWaypoint wp; std::memset(&wp, 0, sizeof(wp)); set_klass(&wp, &kWaypointClass);
    set_f64(&wp, 0x18, 5.0); set_f64(&wp, 0x20, 6.0);
    g_waypoint_calls = 0; call_waypoint(&wp, &wp, nullptr);
    assert(g_waypoint_calls.load() == 1);
    PgrRoutesObserversOutput o;
    PgrRoutesObserversRequest r; make_request(&r, tid, 2500, obs->generation());
    obs->poll(&r, &o);
    assert(!has_event(o, PGR_RO_EV_WAYPOINT)); }

  std::printf("routes_tracking_test (real-dobby): OK\n");
  return 0;
}

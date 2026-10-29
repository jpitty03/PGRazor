#include "routes_discovery.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <thread>
#include <unistd.h>
#include <vector>

// Real-API fake-IL2CPP harness for feature 44 (native Routes discovery reader + rooted route cache). It
// resolves the table through the exported pgrazor_native_api v2 registry (query_feature(44,1)) and drives
// discover() / points() / lookup() / drain() against a fake managed heap: a RoutesService -> didn ->
// NearbyRoutesInfo -> List<SharedRouteProto>, each proto with an id/name String, reversible/distance/stats
// fields, start/end RoutePoiAnchor -> RouteWaypointProto, and a RepeatedField<RouteWaypointProto>.
// gchandle_new/get/free back a handle table so leak accounting is exact; fake_gc_free ASSERTS a borrowed
// host handle is never freed. runtime_invoke serves the boxed Int32 counts + the collection get_Item calls
// and can throw or relocate a rooted proto mid-call to exercise re-resolution. The module installs NO hook
// and NEVER calls back into JS. The implementing worker cannot run this; the coordinator builds and runs
// it (python pgrazor/native_harness.py routes_discovery).

namespace {
using GetApiFn = const void *(*)(uint32_t);

// class / method / field tokens (distinct addresses). List / RepeatedField classes are NOT validated by
// the module (it only invokes on them), so they carry the opaque cls_other token.
int cls_routes, cls_nearby, cls_shared, cls_anchor, cls_waypoint, cls_stats, cls_string, cls_int32, cls_other, cls_poison;
int m_listCount, m_listItem, m_wpCount, m_wpItem;
int f_didn, f_nearbyRoutes, f_id, f_name, f_reversible, f_distance, f_stats, f_waypoints, f_startPoi,
    f_endPoi, f_anchorWp, f_lat, f_lon, f_cooldown;

struct FakeString { void *klass; uint16_t chars[PGR_RD_NAME_MAX]; int32_t len; };
struct FakeWaypoint { void *klass; double lat, lon; };
struct FakeAnchor { void *klass; void *waypoint; };
struct FakeStats { void *klass; int64_t cooldown; };
struct FakeRepeated { void *klass; std::vector<FakeWaypoint *> items; };
struct FakeShared {
  void *klass; void *id; void *name; uint8_t reversible; int64_t distance; void *stats;
  void *waypoints; void *startPoi; void *endPoi;
};
struct FakeList { void *klass; std::vector<FakeShared *> items; };
struct FakeNearby { void *klass; void *list; };
struct FakeRoutes { void *klass; void *nearby; };
struct FakeBox { void *klass; int32_t val; };

FakeString make_string(const char *ascii) {
  FakeString s{&cls_string, {}, 0};
  for (int i = 0; ascii[i] && i < PGR_RD_NAME_MAX; ++i) { s.chars[s.len] = static_cast<uint16_t>(ascii[i]); ++s.len; }
  return s;
}

// gc handle table.
std::map<uintptr_t, void *> g_handles;
std::map<void *, uintptr_t> g_host;  // borrowed host handles the module must NEVER free
uintptr_t g_seq = 0x900;
uint64_t g_gcnew_calls = 0, g_gcnew_fail = 0;
uintptr_t insert_host(void *obj) { uintptr_t h = ++g_seq; g_handles[h] = obj; g_host[obj] = h; return h; }
uintptr_t fake_gc_new(void *obj, int) {
  ++g_gcnew_calls;
  if (!obj) return 0;
  if (g_gcnew_fail && g_gcnew_calls == g_gcnew_fail) return 0;
  uintptr_t h = ++g_seq; g_handles[h] = obj; return h;
}
void *fake_gc_get(uintptr_t h) { auto it = g_handles.find(h); return it == g_handles.end() ? nullptr : it->second; }
void fake_gc_free(uintptr_t h) {
  auto it = g_handles.find(h);
  if (it != g_handles.end()) {
    auto hostit = g_host.find(it->second);
    assert((hostit == g_host.end() || hostit->second != h) && "module freed a borrowed host handle");
  }
  g_handles.erase(h);
}
// module roots == every live handle that is not a borrowed host handle.
size_t module_roots() {
  size_t n = 0;
  for (auto &kv : g_handles) { auto it = g_host.find(kv.second); if (!(it != g_host.end() && it->second == kv.first)) ++n; }
  return n;
}

void *fake_obj_class(void *o) { return o ? *static_cast<void **>(o) : nullptr; }
void *fake_unbox(void *b) { return b ? &static_cast<FakeBox *>(b)->val : nullptr; }
void *g_thread = reinterpret_cast<void *>(0x7700);
void *fake_thread_current() { return g_thread; }

void fake_field_get(void *o, void *field, void *out) {
  if (field == &f_didn) { *static_cast<void **>(out) = static_cast<FakeRoutes *>(o)->nearby; return; }
  if (field == &f_nearbyRoutes) { *static_cast<void **>(out) = static_cast<FakeNearby *>(o)->list; return; }
  if (field == &f_id) { *static_cast<void **>(out) = static_cast<FakeShared *>(o)->id; return; }
  if (field == &f_name) { *static_cast<void **>(out) = static_cast<FakeShared *>(o)->name; return; }
  if (field == &f_reversible) { *static_cast<uint8_t *>(out) = static_cast<FakeShared *>(o)->reversible; return; }
  if (field == &f_distance) { *static_cast<int64_t *>(out) = static_cast<FakeShared *>(o)->distance; return; }
  if (field == &f_stats) { *static_cast<void **>(out) = static_cast<FakeShared *>(o)->stats; return; }
  if (field == &f_waypoints) { *static_cast<void **>(out) = static_cast<FakeShared *>(o)->waypoints; return; }
  if (field == &f_startPoi) { *static_cast<void **>(out) = static_cast<FakeShared *>(o)->startPoi; return; }
  if (field == &f_endPoi) { *static_cast<void **>(out) = static_cast<FakeShared *>(o)->endPoi; return; }
  if (field == &f_anchorWp) { *static_cast<void **>(out) = static_cast<FakeAnchor *>(o)->waypoint; return; }
  if (field == &f_lat) { *static_cast<double *>(out) = static_cast<FakeWaypoint *>(o)->lat; return; }
  if (field == &f_lon) { *static_cast<double *>(out) = static_cast<FakeWaypoint *>(o)->lon; return; }
  if (field == &f_cooldown) { *static_cast<int64_t *>(out) = static_cast<FakeStats *>(o)->cooldown; return; }
  assert(false && "unknown field token");
}

int32_t fake_str_len(void *s) { return static_cast<FakeString *>(s)->len; }
uint16_t *fake_str_chars(void *s) { return static_cast<FakeString *>(s)->chars; }

uint64_t g_invokes = 0;
void *g_throw_token = nullptr;            // method token that throws once; 0 none
std::function<void()> g_oneshot;          // fired once at the top of the next qualifying invoke
void *g_oneshot_method = nullptr;
FakeBox g_box{&cls_int32, 0};
FakeBox g_wide_box{&cls_poison, 0};       // a non-Int32 count box (metadata fault)
bool g_wide_count = false;

void *fake_invoke(const void *method, void *self, void **params, void **exc) {
  if (exc) *exc = nullptr;
  ++g_invokes;
  if (g_oneshot && method == g_oneshot_method) { auto fn = g_oneshot; g_oneshot = nullptr; fn(); }
  if (g_throw_token && method == g_throw_token) { g_throw_token = nullptr; if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
  if (method == &m_listCount) { g_box.val = static_cast<int32_t>(static_cast<FakeList *>(self)->items.size()); return &g_box; }
  if (method == &m_wpCount) {
    int32_t n = static_cast<int32_t>(static_cast<FakeRepeated *>(self)->items.size());
    if (g_wide_count) { g_wide_count = false; g_wide_box.val = n; return &g_wide_box; }  // non-Int32 box
    g_box.val = n; return &g_box;
  }
  if (method == &m_listItem) { int32_t i = *static_cast<int32_t *>(params[0]); auto &v = static_cast<FakeList *>(self)->items; return (i >= 0 && i < (int)v.size()) ? static_cast<void *>(v[i]) : nullptr; }
  if (method == &m_wpItem) { int32_t i = *static_cast<int32_t *>(params[0]); auto &v = static_cast<FakeRepeated *>(self)->items; return (i >= 0 && i < (int)v.size()) ? static_cast<void *>(v[i]) : nullptr; }
  assert(false && "unknown method token");
  return nullptr;
}

PgrRoutesDiscoveryConfig make_config() {
  PgrRoutesDiscoveryConfig c{};
  c.version = 1; c.size = sizeof(c);
  c.runtime_invoke = (void *)&fake_invoke; c.object_get_class = (void *)&fake_obj_class;
  c.field_get_value = (void *)&fake_field_get; c.object_unbox = (void *)&fake_unbox;
  c.gchandle_new = (void *)&fake_gc_new; c.gchandle_get_target = (void *)&fake_gc_get;
  c.gchandle_free = (void *)&fake_gc_free; c.thread_current = (void *)&fake_thread_current;
  c.string_length = (void *)&fake_str_len; c.string_chars = (void *)&fake_str_chars;
  c.class_routes = &cls_routes; c.class_nearby = &cls_nearby; c.class_shared = &cls_shared;
  c.class_anchor = &cls_anchor; c.class_waypoint = &cls_waypoint; c.class_playerStats = &cls_stats;
  c.class_string = &cls_string; c.class_int32 = &cls_int32;
  c.m_listCount = &m_listCount; c.m_listItem = &m_listItem; c.m_wpCount = &m_wpCount; c.m_wpItem = &m_wpItem;
  c.f_routes_didn = &f_didn; c.f_nearby_routes = &f_nearbyRoutes; c.f_shared_id = &f_id;
  c.f_shared_name = &f_name; c.f_shared_reversible = &f_reversible; c.f_shared_distance = &f_distance;
  c.f_shared_stats = &f_stats; c.f_shared_waypoints = &f_waypoints; c.f_shared_startPoi = &f_startPoi;
  c.f_shared_endPoi = &f_endPoi; c.f_anchor_anchor = &f_anchorWp; c.f_wp_lat = &f_lat; c.f_wp_lon = &f_lon;
  c.f_stats_cooldown = &f_cooldown;
  c.class_list=&cls_other;c.class_waypoints=&cls_other;
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }
const PgrRoutesDiscoveryApi *g_api = nullptr;

PgrRoutesDiscoveryRequest req(uint64_t tid, uint64_t residual, uint64_t svc, uint32_t index) {
  PgrRoutesDiscoveryRequest r{}; r.version = 1; r.size = sizeof(r); r.tid = tid; r.residual_ms = residual;
  r.generation = g_api->generation(); r.service_handle = svc; r.index = index; return r;
}
PgrRoutesDiscoveryLookup lk(uint64_t tid, uint64_t residual, uint64_t svc, const char *id) {
  PgrRoutesDiscoveryLookup l{}; l.version = 1; l.size = sizeof(l); l.tid = tid; l.residual_ms = residual;
  l.generation = g_api->generation(); l.service_handle = svc;
  for (int i = 0; id[i] && i < PGR_RD_ID_MAX; ++i) { l.id[i] = (uint16_t)id[i]; ++l.id_length; }
  return l;
}

void reset_invoke() { g_invokes = 0; g_throw_token = nullptr; g_oneshot = nullptr; g_oneshot_method = nullptr; g_gcnew_fail = 0; g_gcnew_calls = 0; g_wide_count = false; }
void drain(uint64_t tid, uint64_t svc) { auto r = req(tid, 0, svc, 0); assert(g_api->drain(&r) == PGR_RD_OK); }

bool id_is(const PgrRoutesDiscoveryRoute &h, const char *expect) {
  int n = 0; while (expect[n]) ++n;
  if ((int)h.id_length != n) return false;
  for (int i = 0; i < n; ++i) if (h.id[i] != (uint16_t)expect[i]) return false;
  return true;
}
}  // namespace

int main(int argc, char **argv) {
  const char *so = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so, RTLD_NOW | RTLD_LOCAL);
  if (!lib) { std::fprintf(stderr, "dlopen(%s) failed: %s\n", so, ::dlerror()); return 1; }
  auto getter = reinterpret_cast<GetApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->query_feature);
  assert(core->query_feature(44, 99) == nullptr);
  const auto *api = static_cast<const PgrRoutesDiscoveryApi *>(core->query_feature(44, 1));
  assert(api && api->version == 1 && api->size == sizeof(PgrRoutesDiscoveryApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu request=%zu lookup=%zu route=%zu output=%zu points=%zu table=%zu\n",
              sizeof(PgrRoutesDiscoveryConfig), sizeof(PgrRoutesDiscoveryRequest), sizeof(PgrRoutesDiscoveryLookup),
              sizeof(PgrRoutesDiscoveryRoute), sizeof(PgrRoutesDiscoveryOutput), sizeof(PgrRoutesDiscoveryPoints),
              sizeof(PgrRoutesDiscoveryApi));
  assert(sizeof(PgrRoutesDiscoveryConfig) == 312 && sizeof(PgrRoutesDiscoveryRequest) == 48 &&
         sizeof(PgrRoutesDiscoveryLookup) == 304 && sizeof(PgrRoutesDiscoveryRoute) == 832 &&
         sizeof(PgrRoutesDiscoveryOutput) == 83216 && sizeof(PgrRoutesDiscoveryPoints) == 80016 &&
         sizeof(PgrRoutesDiscoveryApi) == 64);

  const uint64_t tid = this_tid();
  PgrRoutesDiscoveryConfig cfg = make_config();
  static PgrRoutesDiscoveryOutput out;    // 83 KB: never on the stack
  static PgrRoutesDiscoveryPoints pts;     // 80 KB: never on the stack

  // 1. install validation / not-installed / idempotent / reconfig.
  assert(api->generation() == 1);
  assert(api->install(nullptr) == PGR_RD_ERR_NULL_ARG);
  { auto bad = cfg; bad.version = 2; assert(api->install(&bad) == PGR_RD_ERR_BAD_VERSION); }
  { auto bad = cfg; bad.size = sizeof(cfg) - 1; assert(api->install(&bad) == PGR_RD_ERR_BAD_SIZE); }
  { auto bad = cfg; bad.m_listItem = nullptr; assert(api->install(&bad) == PGR_RD_ERR_MISSING_PTR); }
  { auto r = req(tid, 2000, 0, 0); assert(api->discover(&r, &out) == PGR_RD_ERR_NOT_INSTALLED && out.status == PGR_RD_ERR_NOT_INSTALLED); }
  assert(api->install(&cfg) == 0);
  assert(api->install(&cfg) == 0);  // idempotent
  { auto bad = cfg; bad.class_shared = &cls_nearby; assert(api->install(&bad) == PGR_RD_ERR_RECONFIG); }

  // Fake scene: two routes. route-a: 3 points, name, stats, start + end anchors, reversible. route-b: 2
  // points, no name/stats/anchors, not reversible.
  FakeString idA = make_string("route-a"), nameA = make_string("Alpha"), idB = make_string("route-b");
  FakeWaypoint wa0{&cls_waypoint, 1, 2}, wa1{&cls_waypoint, 3, 4}, wa2{&cls_waypoint, 5, 6};
  FakeWaypoint wb0{&cls_waypoint, 7, 8}, wb1{&cls_waypoint, 9, 10};
  FakeWaypoint sA{&cls_waypoint, 11, 12}, eA{&cls_waypoint, 13, 14};
  FakeAnchor startA{&cls_anchor, &sA}, endA{&cls_anchor, &eA};
  FakeStats statsA{&cls_stats, 99};
  FakeRepeated wpsA{&cls_other, {&wa0, &wa1, &wa2}}, wpsB{&cls_other, {&wb0, &wb1}};
  FakeShared shA{&cls_shared, &idA, &nameA, 1, 1234, &statsA, &wpsA, &startA, &endA};
  FakeShared shB{&cls_shared, &idB, nullptr, 0, 0, nullptr, &wpsB, nullptr, nullptr};
  FakeList list{&cls_other, {&shA, &shB}};
  FakeNearby nearby{&cls_nearby, &list};
  FakeRoutes routes{&cls_routes, &nearby};
  const uintptr_t hRoutes = insert_host(&routes);

  // 2. discover: full snapshot + cache; headers decode; points() per route; lookup() returns a handle that
  //    resolves to the cached proto; borrowed host handle never freed; module roots balanced after drain.
  { reset_invoke();
    auto r = req(tid, 2000, hRoutes, 0);
    assert(api->discover(&r, &out) == PGR_RD_OK && out.status == PGR_RD_OK && out.count == 2);
    const auto &a = out.routes[0];
    assert(id_is(a, "route-a") && a.name_length == 5 && a.point_count == 3);
    assert(a.flags == (PGR_RD_FLAG_REVERSIBLE | PGR_RD_FLAG_HAS_START | PGR_RD_FLAG_HAS_END));
    assert(a.distance_meters == 1234.0 && a.cooldown_finish_ms == 99.0);
    assert(a.start_lat == 11 && a.start_lon == 12 && a.end_lat == 13 && a.end_lon == 14);
    const auto &b = out.routes[1];
    assert(id_is(b, "route-b") && b.name_length == 0 && b.point_count == 2 && b.flags == 0 && b.cooldown_finish_ms == 0.0);
    // points()
    auto pr = req(tid, 2000, hRoutes, 0);
    assert(api->points(&pr, 0, &pts) == PGR_RD_OK && pts.count == 3);
    assert(pts.points[0].lat == 1 && pts.points[2].lon == 6);
    pr = req(tid, 2000, hRoutes, 1);
    assert(api->points(&pr, 1, &pts) == PGR_RD_OK && pts.count == 2 && pts.points[1].lat == 9);
    // points() out of range
    pr = req(tid, 2000, hRoutes, 9); assert(api->points(&pr, 9, &pts) == PGR_RD_ERR_BAD_INDEX);
    // lookup()
    { auto l = lk(tid, 2000, hRoutes, "route-a"); uint64_t h = 0;
      assert(api->lookup(&l, &h) == PGR_RD_OK && h != 0);
      assert(fake_gc_get(static_cast<uintptr_t>(h)) == &shA); }
    { auto l = lk(tid, 2000, hRoutes, "route-b"); uint64_t h = 0; assert(api->lookup(&l, &h) == PGR_RD_OK && h != 0); }
    { auto l = lk(tid, 2000, hRoutes, "route-z"); uint64_t h = 7; assert(api->lookup(&l, &h) == PGR_RD_SKIP && h == 0); }
    assert(module_roots() > 0);  // the cache retains proto + service roots
    drain(tid, hRoutes);
    assert(module_roots() == 0 && g_handles.count(hRoutes) == 1);  // only the borrowed host handle remains
  }

  // 3. service-identity binding: a lookup through a DIFFERENT RoutesService is refused (stale cache).
  { reset_invoke();
    FakeRoutes routes2{&cls_routes, &nearby};
    const uintptr_t hRoutes2 = insert_host(&routes2);
    auto r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_OK && out.count == 2);
    auto l = lk(tid, 2000, hRoutes2, "route-a"); uint64_t h = 5;
    assert(api->lookup(&l, &h) == PGR_RD_SKIP && h == 0);  // bound service != request service
    drain(tid, hRoutes);
    g_handles.erase(hRoutes2); g_host.erase(&routes2);
  }

  // 4. dedup + prune (transactional replacement). Two protos share an id -> one entry; re-discover with a
  //    shorter list drops the removed route from the cache.
  { reset_invoke();
    FakeShared dupA{&cls_shared, &idA, nullptr, 0, 0, nullptr, &wpsB, nullptr, nullptr};
    list.items = {&shA, &dupA, &shB};
    auto r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_OK && out.count == 2);  // dup dropped
    assert(id_is(out.routes[0], "route-a") && id_is(out.routes[1], "route-b"));
    list.items = {&shA};  // route-b removed
    r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_OK && out.count == 1 && id_is(out.routes[0], "route-a"));
    { auto l = lk(tid, 2000, hRoutes, "route-b"); uint64_t h = 1; assert(api->lookup(&l, &h) == PGR_RD_SKIP && h == 0); }
    list.items = {&shA, &shB};
    drain(tid, hRoutes); assert(module_roots() == 0);
  }

  // 5. valid EMPTY snapshot clears the cache (null nearby / empty list).
  { reset_invoke();
    auto r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_OK && out.count == 2);
    void *saved = routes.nearby; routes.nearby = nullptr;  // didn null -> empty snapshot
    r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_OK && out.count == 0);
    { auto l = lk(tid, 2000, hRoutes, "route-a"); uint64_t h = 2; assert(api->lookup(&l, &h) == PGR_RD_SKIP && h == 0); }
    routes.nearby = saved;
    drain(tid, hRoutes); assert(module_roots() == 0);
  }

  // 6. malformed routes are skipped (fail closed), the rest admitted: waypoint count < 2, a non-Int32 count
  //    box, a wrong-class waypoint element, a reversible byte > 1, and an over-long / empty id.
  { reset_invoke();
    FakeRepeated oneWp{&cls_other, {&wa0}};           // count 1 -> skip route
    FakeShared shShort{&cls_shared, &idB, nullptr, 0, 0, nullptr, &oneWp, nullptr, nullptr};
    FakeString idEmpty = make_string("");
    FakeShared shEmptyId{&cls_shared, &idEmpty, nullptr, 1, 0, nullptr, &wpsB, nullptr, nullptr};
    FakeString idLong; idLong.klass = &cls_string; idLong.len = PGR_RD_ID_MAX + 1; for (int i = 0; i < idLong.len; ++i) idLong.chars[i % PGR_RD_NAME_MAX] = 'x';
    FakeShared shLongId{&cls_shared, &idLong, nullptr, 1, 0, nullptr, &wpsB, nullptr, nullptr};
    FakeShared shBadBool{&cls_shared, &idB, nullptr, 2, 0, nullptr, &wpsB, nullptr, nullptr};  // reversible byte 2
    list.items = {&shA, &shShort, &shEmptyId, &shLongId, &shBadBool};
    auto r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_OK && out.count == 1 && id_is(out.routes[0], "route-a"));
    // wrong-class waypoint element is skipped as a point, route still admitted.
    FakeWaypoint bad; bad.klass = &cls_poison; bad.lat = 0; bad.lon = 0;
    FakeRepeated mixed{&cls_other, {&wb0, &bad, &wb1}};
    FakeShared shMixed{&cls_shared, &idB, nullptr, 0, 0, nullptr, &mixed, nullptr, nullptr};
    list.items = {&shMixed};
    r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_OK && out.count == 1 && out.routes[0].point_count == 2);
    // non-Int32 count box -> whole discover is a metadata fault (fail closed, no publish).
    list.items = {&shB}; g_wide_count = true;
    r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_ERR_GCHANDLE && out.count == 0);
    g_wide_count = false;
    list.items = {&shA, &shB};
    drain(tid, hRoutes); assert(module_roots() == 0);
  }

  // 7. managed exceptions: a list-level throw is a fault (no publish); a per-route waypoint throw skips only
  //    that route.
  { reset_invoke();
    g_throw_token = &m_listCount;
    auto r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_ERR_MANAGED && out.count == 0);
    reset_invoke();
    g_throw_token = &m_wpCount;  // first route's wpCount throws -> route-a skipped, route-b admitted
    r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_OK && out.count == 1 && id_is(out.routes[0], "route-b"));
    drain(tid, hRoutes); assert(module_roots() == 0);
  }

  // 8. moving GC: the proto relocates during its wpCount invoke and the original is poisoned; the module
  //    re-resolves from its root so the admit + lookup still see the live (shadow) object.
  { reset_invoke();
    list.items = {&shA};
    static FakeShared shadow;
    g_oneshot_method = &m_wpCount;
    g_oneshot = [&]() {
      shadow = shA;
      for (auto &kv : g_handles) if (kv.second == &shA) kv.second = &shadow;
      shA.klass = &cls_poison;  // a stale read of the original would fail the class check
    };
    auto r = req(tid, 2000, hRoutes, 0);
    assert(api->discover(&r, &out) == PGR_RD_OK && out.count == 1 && id_is(out.routes[0], "route-a") && out.routes[0].point_count == 3);
    { auto l = lk(tid, 2000, hRoutes, "route-a"); uint64_t h = 0; assert(api->lookup(&l, &h) == PGR_RD_OK && fake_gc_get((uintptr_t)h) == &shadow); }
    shA.klass = &cls_shared; list.items = {&shA, &shB};
    drain(tid, hRoutes); assert(module_roots() == 0);
  }

  // 9. lease / owner / thread gates: residual 0, detached thread, and a wrong tid refuse with NO managed
  //    work (no invoke after a blocked lease).
  { reset_invoke();
    auto r = req(tid, 0, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_SKIP && g_invokes == 0);
    g_thread = nullptr; r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_SKIP && g_invokes == 0);
    g_thread = reinterpret_cast<void *>(0x7700);
    assert(module_roots() == 0);
  }

  // 10. revoke invalidates the published cache and cannot be undone by renewing the lease: after a revoke a
  //     lookup/points stamped with the new generation is refused (cache published under the old gen), and a
  //     stale-generation request is refused regardless of residual. drain still reclaims the roots.
  { reset_invoke();
    auto r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_OK && out.count == 2);
    const uint64_t g0 = api->generation();
    assert(api->revoke() == g0 + 1 && api->generation() == g0 + 1);
    { auto l = lk(tid, 2000, hRoutes, "route-a"); uint64_t h = 3; assert(api->lookup(&l, &h) == PGR_RD_SKIP && h == 0); }
    { auto pr = req(tid, 2000, hRoutes, 0); assert(api->points(&pr, 0, &pts) == PGR_RD_SKIP); }
    // a stale-generation discover refuses even with a full lease.
    { auto sr = req(tid, 2000, hRoutes, 0); sr.generation = g0; assert(api->discover(&sr, &out) == PGR_RD_SKIP); }
    // re-discover with the current generation republishes and the cache works again.
    r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_OK && out.count == 2);
    { auto l = lk(tid, 2000, hRoutes, "route-a"); uint64_t h = 0; assert(api->lookup(&l, &h) == PGR_RD_OK && h != 0); }
    drain(tid, hRoutes); assert(module_roots() == 0);
  }

  // 11. off-owner thread: a foreign tid is refused and frees nothing; the owner cache is untouched.
  { reset_invoke();
    auto r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_OK && out.count == 2);
    std::thread([&]() {
      PgrRoutesDiscoveryOutput o{};
      auto fr = req(this_tid(), 2000, hRoutes, 0);  // this_tid() here is the foreign worker tid
      assert(g_api->discover(&fr, &o) == PGR_RD_SKIP && o.count == 0);
      auto dr = req(this_tid(), 0, hRoutes, 0);
      assert(g_api->drain(&dr) == PGR_RD_SKIP);  // off-owner drain frees nothing
    }).join();
    { auto l = lk(tid, 2000, hRoutes, "route-a"); uint64_t h = 0; assert(api->lookup(&l, &h) == PGR_RD_OK && h != 0); }  // cache intact
    drain(tid, hRoutes); assert(module_roots() == 0);
  }

  // 12. root-allocation failure fails closed (no publish); the previous (empty) cache is retained.
  { reset_invoke();
    g_gcnew_calls = 0; g_gcnew_fail = 1;  // the service-binding root fails
    auto r = req(tid, 2000, hRoutes, 0); assert(api->discover(&r, &out) == PGR_RD_ERR_GCHANDLE && out.count == 0);
    assert(module_roots() == 0);
  }

  // Final: only the borrowed host handle remains; no module root leaked anywhere.
  drain(tid, hRoutes);
  assert(module_roots() == 0 && g_handles.size() == g_host.size());

  std::puts("PASS routes discovery: ABI(312/48/304/832/83216/80016/64), install/idempotent/reconfig, "
            "snapshot+headers+points+lookup, service-identity binding, dedup+prune(transactional), empty "
            "clears cache, malformed/throw skip-vs-fault, moving-GC re-resolution, lease/thread/owner gates "
            "(no managed work after a blocked lease), revoke invalidates+cannot-undo, off-owner refuse, "
            "root-alloc failure; borrowed host handle never freed, zero module-root leaks");
  dlclose(lib);
  return 0;
}

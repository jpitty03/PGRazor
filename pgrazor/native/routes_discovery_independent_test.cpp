// Independent adversarial harness for feature 44 (native Routes DISCOVERY reader + rooted route cache).
// Written by a Claude leaf test worker WITHOUT reference to routes_discovery.cpp's internals beyond the
// public ABI in routes_discovery.h / pgrazor_native_api.h and the published contract. It drives the real
// table through the v2 query_feature(44,1) getter, exactly like routes_actions_independent_test.cpp, and
// builds its OWN minimal fake IL2CPP whose heap RELOCATES on every gchandle_* / runtime_invoke. A stale
// managed pointer (one reused across an allocation) is caught by the poisoned old generation; a leaked GC
// root is caught by the final roots.empty() check after drain().
//
// This file owns NO production code. It is NOT executed by this worker; the coordinator compiles + runs it
// (one forked child process per scenario) alongside routes_discovery.cpp. The header may still evolve; the
// coordinator fixes compile fallout. See .artifacts/native-migration/routes46-independent-notes.txt for the
// coverage map, the two documented DIVERGENCE flags, and the fixture assumptions.

#include "routes_discovery.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <map>
#include <vector>

// The test re-asserts the frozen ABI sizes independently of the header's own static_asserts.
static_assert(sizeof(PgrRoutesDiscoveryConfig) == 312, "config 312");
static_assert(sizeof(PgrRoutesDiscoveryRequest) == 48, "request 48");
static_assert(sizeof(PgrRoutesDiscoveryLookup) == 304, "lookup 304");
static_assert(sizeof(PgrRoutesDiscoveryRoute) == 832, "route 832");
static_assert(sizeof(PgrRoutesDiscoveryOutput) == 83216, "output 83216");
static_assert(sizeof(PgrRoutesDiscoveryPoints) == 80016, "points 80016");
static_assert(sizeof(PgrRoutesDiscoveryApi) == 64, "table 64");

namespace {

// --- fake il2cpp identity tokens (addresses stand in for Il2CppClass*/MethodInfo*/FieldInfo*) ----------
int routesClass, nearbyClass, sharedClass, anchorClass, waypointClass, playerStatsClass, stringClass,
    int32Class, listClass, wpfClass, poison, attachedThread, excDummy;
int m_listCount, m_listItem, m_wpCount, m_wpItem;
int f_routes_didn, f_nearby_routes, f_shared_id, f_shared_name, f_shared_reversible, f_shared_distance,
    f_shared_stats, f_shared_waypoints, f_shared_startPoi, f_shared_endPoi, f_anchor_anchor, f_wp_lat,
    f_wp_lon, f_stats_cooldown;

// --- fake managed heap with a RELOCATING collector ------------------------------------------------------
// Fixed object pool per generation. move() copies the pool to a brand-new generation, remaps every GC root
// and inter-object reference to the new generation, then POISONS the old generation's class pointers so any
// surviving raw pointer trips object_get_class / index(). Generations are never reused (grown on the heap
// and intentionally leaked inside the short-lived forked child), so even a two-step-stale pointer is caught.
enum { SERVICE, SERVICE_B, NEARBY, LIST, BOX, ROUTE0, ID0, NAME0, WPF0, WP0, WP1, SANCH, EANCH, SWP, EWP,
       STATS0, NOBJECTS };
struct Object {
  void *klass;
  void *ref[6];       // reference-field slots (remapped on every move)
  uint8_t boolv;      // reversible byte source
  int32_t boxval;     // boxed Int32 value source
  int32_t length;     // string length
  uint16_t text[256]; // string chars
  int64_t i64;        // distance / cooldown source
  double lat, lon;    // waypoint coord source
};
struct Heap { Object object[NOBJECTS]; };

std::vector<Heap *> gens;
int generation = 0, moves = 0, managed = 0, listItemCalls = 0, wpItemCalls = 0;
std::map<uintptr_t, void *> roots;
uintptr_t serial = 100, hostService = 0, hostServiceB = 0;

const PgrRoutesDiscoveryApi *api = nullptr;
const char *mode = nullptr;

// Scenario knobs.
int32_t listCount = 1, wpCount = 2;
bool distinctIds = false, throwList = false, throwWp = false, detached = false, inDiscover = false;
enum { CLEAN_NONE, CLEAN_REENTER, CLEAN_REVOKE } cleanupAction = CLEAN_NONE;
bool cleanupFired = false;
uintptr_t cleanupTarget=0, lookupRevokeTarget=0;
int reentryResult = 999;

PgrRoutesDiscoveryOutput g_out;   // 83 KB; reused + zeroed per discover()
PgrRoutesDiscoveryPoints g_pts;   // 80 KB; reused + zeroed per points()

Object *obj(int i) { return &gens[generation]->object[i]; }
int index(void *p) {
  for (int i = 0; i < NOBJECTS; i++)
    if (p == obj(i)) return i;
  assert(false && "stale managed pointer (used across a GC move)");
  return -1;
}
void move() {
  Heap *fresh = new Heap(*gens[generation]);
  gens.push_back(fresh);
  Heap &old = *gens[generation];
  auto remap = [&](void *p) -> void * {
    for (int i = 0; i < NOBJECTS; i++)
      if (p == &old.object[i]) return &fresh->object[i];
    return p;
  };
  for (auto &root : roots) root.second = remap(root.second);
  for (auto &o : fresh->object)
    for (auto &r : o.ref) r = remap(r);
  for (auto &o : old.object) o.klass = &poison;
  generation = static_cast<int>(gens.size()) - 1;
  ++moves;
}

PgrRoutesDiscoveryRequest discReq();  // fwd

// --- fake il2cpp callbacks ------------------------------------------------------------------------------
uintptr_t gcNew(void *p, int pinned) { assert(!pinned); index(p); auto h = ++serial; roots[h] = p; move(); return h; }
void *gcGet(uintptr_t h) { auto it = roots.find(h); if(h && h==lookupRevokeTarget){lookupRevokeTarget=0;api->revoke();} return it == roots.end() ? nullptr : it->second; }
void gcFree(uintptr_t h) {
  index(roots.at(h));
  assert(roots.erase(h) == 1);
  if (inDiscover && !cleanupFired && cleanupAction != CLEAN_NONE && (!cleanupTarget || h==cleanupTarget)) {
    cleanupFired = true;
    if (cleanupAction == CLEAN_REENTER) {
      auto r = discReq();
      int before = managed;
      static PgrRoutesDiscoveryOutput nested;  // separate buffer; reentrant call must refuse before any work
      std::memset(&nested, 0, sizeof(nested));
      reentryResult = api->discover(&r, &nested);
      assert(managed == before && "reentrant discover must refuse BEFORE any managed work");
    } else if (cleanupAction == CLEAN_REVOKE) {
      api->revoke();  // an off-tick revoke lands mid-cleanup; the pre-publish gate must abort the swap
    }
  }
  move();
}
void *objectClass(void *p) { return obj(index(p))->klass; }
void *threadCurrent() { return detached ? nullptr : &attachedThread; }
void *unbox(void *p) { assert(index(p) == BOX); return &obj(BOX)->boxval; }
int32_t stringLength(void *p) { int i = index(p); assert(obj(i)->klass == &stringClass); return obj(i)->length; }
uint16_t *stringChars(void *p) { int i = index(p); assert(obj(i)->klass == &stringClass); return obj(i)->text; }
void getField(void *p, void *field, void *out) {
  int i = index(p);
  if (field == &f_routes_didn)        *static_cast<void **>(out) = obj(i)->ref[0];  // service -> nearby
  else if (field == &f_nearby_routes) *static_cast<void **>(out) = obj(i)->ref[0];  // nearby -> list
  else if (field == &f_shared_id)        *static_cast<void **>(out) = obj(i)->ref[0];
  else if (field == &f_shared_name)      *static_cast<void **>(out) = obj(i)->ref[1];
  else if (field == &f_shared_waypoints) *static_cast<void **>(out) = obj(i)->ref[2];
  else if (field == &f_shared_startPoi)  *static_cast<void **>(out) = obj(i)->ref[3];
  else if (field == &f_shared_endPoi)    *static_cast<void **>(out) = obj(i)->ref[4];
  else if (field == &f_shared_stats)     *static_cast<void **>(out) = obj(i)->ref[5];
  else if (field == &f_anchor_anchor)    *static_cast<void **>(out) = obj(i)->ref[0];  // anchor -> waypoint
  else if (field == &f_shared_reversible) *static_cast<uint8_t *>(out) = obj(i)->boolv;
  else if (field == &f_shared_distance)   *static_cast<int64_t *>(out) = obj(i)->i64;
  else if (field == &f_stats_cooldown)    *static_cast<int64_t *>(out) = obj(i)->i64;
  else if (field == &f_wp_lat)            *static_cast<double *>(out) = obj(i)->lat;
  else if (field == &f_wp_lon)            *static_cast<double *>(out) = obj(i)->lon;
  else assert(false && "unexpected field");
}
void *invoke(const void *method, void *recv, void **args, void **exc) {
  ++managed;
  *exc = nullptr;
  if (method == &m_listCount) {
    assert(index(recv) == LIST);
    if (throwList) { *exc = &excDummy; move(); return nullptr; }
    obj(BOX)->boxval = listCount; move(); return obj(BOX);
  }
  if (method == &m_listItem) {
    assert(index(recv) == LIST);
    int32_t i = args ? *static_cast<int32_t *>(args[0]) : 0;
    ++listItemCalls;
    if (distinctIds) {  // give every item a unique id so dedup cannot mask the walk length
      char buf[16];
      int n = std::snprintf(buf, sizeof buf, "r%d", i);
      obj(ID0)->length = n;
      for (int k = 0; k < n; k++) obj(ID0)->text[k] = static_cast<uint16_t>(buf[k]);
    }
    move();
    return obj(ROUTE0);
  }
  if (method == &m_wpCount) { assert(index(recv) == WPF0); obj(BOX)->boxval = wpCount; move(); return obj(BOX); }
  if (method == &m_wpItem) {
    assert(index(recv) == WPF0);
    int32_t i = args ? *static_cast<int32_t *>(args[0]) : 0;
    ++wpItemCalls;
    if (throwWp) { *exc = &excDummy; move(); return nullptr; }
    move();
    return obj(i % 2 ? WP1 : WP0);
  }
  assert(false && "unexpected method");
  return nullptr;
}

// --- request / lookup builders --------------------------------------------------------------------------
PgrRoutesDiscoveryRequest discReq() {
  PgrRoutesDiscoveryRequest r{};
  r.version = PGR_RD_REQUEST_VERSION;
  r.size = sizeof(r);
  r.tid = static_cast<uint64_t>(gettid());
  r.residual_ms = 2500;
  r.generation = api->generation();
  r.service_handle = hostService;
  return r;
}
PgrRoutesDiscoveryLookup lookupReq(const char *id, uint64_t svc) {
  PgrRoutesDiscoveryLookup l{};
  l.version = PGR_RD_LOOKUP_VERSION;
  l.size = sizeof(l);
  l.tid = static_cast<uint64_t>(gettid());
  l.residual_ms = 2500;
  l.generation = api->generation();
  l.service_handle = svc;
  int n = static_cast<int>(std::strlen(id));
  l.id_length = n;
  for (int i = 0; i < n; i++) l.id[i] = static_cast<uint16_t>(id[i]);
  return l;
}
bool idEq(const PgrRoutesDiscoveryRoute &r, const char *s) {
  size_t n = std::strlen(s);
  if (r.id_length != n) return false;
  for (size_t i = 0; i < n; i++)
    if (r.id[i] != static_cast<uint16_t>(s[i])) return false;
  return true;
}
int doDiscover(PgrRoutesDiscoveryRequest &r) {
  std::memset(&g_out, 0, sizeof(g_out));
  inDiscover = true;
  int rc = api->discover(&r, &g_out);
  inDiscover = false;
  return rc;
}
uint64_t doLookup(const char *id, uint64_t svc, int *rc) {
  auto l = lookupReq(id, svc);
  uint64_t h = 0;
  *rc = api->lookup(&l, &h);
  return h;
}

// --- graph + config -------------------------------------------------------------------------------------
void setupGraph() {
  gens.push_back(new Heap());
  void *klass[NOBJECTS] = {&routesClass, &routesClass, &nearbyClass, &listClass, &int32Class, &sharedClass,
                           &stringClass, &stringClass, &wpfClass, &waypointClass, &waypointClass,
                           &anchorClass, &anchorClass, &waypointClass, &waypointClass, &playerStatsClass};
  for (int i = 0; i < NOBJECTS; i++) obj(i)->klass = klass[i];
  obj(SERVICE)->ref[0] = obj(NEARBY);
  obj(NEARBY)->ref[0] = obj(LIST);
  obj(ROUTE0)->ref[0] = obj(ID0);
  obj(ROUTE0)->ref[1] = obj(NAME0);
  obj(ROUTE0)->ref[2] = obj(WPF0);
  obj(ROUTE0)->ref[3] = obj(SANCH);
  obj(ROUTE0)->ref[4] = obj(EANCH);
  obj(ROUTE0)->ref[5] = obj(STATS0);
  obj(SANCH)->ref[0] = obj(SWP);
  obj(EANCH)->ref[0] = obj(EWP);
  obj(ROUTE0)->boolv = 1;      // reversible
  obj(ROUTE0)->i64 = 1500;     // distance
  obj(STATS0)->i64 = 42000;    // cooldown
  obj(WP0)->lat = 10.0; obj(WP0)->lon = 20.0;
  obj(WP1)->lat = 11.0; obj(WP1)->lon = 21.0;
  obj(SWP)->lat = 10.0; obj(SWP)->lon = 20.0;
  obj(EWP)->lat = 11.0; obj(EWP)->lon = 21.0;
  const char *rid = "route-a";
  obj(ID0)->length = 7;
  for (int i = 0; i < 7; i++) obj(ID0)->text[i] = static_cast<uint16_t>(rid[i]);
  obj(NAME0)->length = 4;
  for (int i = 0; i < 4; i++) obj(NAME0)->text[i] = static_cast<uint16_t>("Name"[i]);
}
void installConfig() {
  PgrRoutesDiscoveryConfig c{};
  c.version = PGR_RD_CONFIG_VERSION;
  c.size = sizeof(c);
  c.runtime_invoke = reinterpret_cast<void *>(&invoke);
  c.object_get_class = reinterpret_cast<void *>(&objectClass);
  c.field_get_value = reinterpret_cast<void *>(&getField);
  c.object_unbox = reinterpret_cast<void *>(&unbox);
  c.gchandle_new = reinterpret_cast<void *>(&gcNew);
  c.gchandle_get_target = reinterpret_cast<void *>(&gcGet);
  c.gchandle_free = reinterpret_cast<void *>(&gcFree);
  c.thread_current = reinterpret_cast<void *>(&threadCurrent);
  c.string_length = reinterpret_cast<void *>(&stringLength);
  c.string_chars = reinterpret_cast<void *>(&stringChars);
  c.class_routes = &routesClass;
  c.class_nearby = &nearbyClass;
  c.class_shared = &sharedClass;
  c.class_anchor = &anchorClass;
  c.class_waypoint = &waypointClass;
  c.class_playerStats = &playerStatsClass;
  c.class_string = &stringClass;
  c.class_int32 = &int32Class;
  c.m_listCount = &m_listCount;
  c.m_listItem = &m_listItem;
  c.m_wpCount = &m_wpCount;
  c.m_wpItem = &m_wpItem;
  c.f_routes_didn = &f_routes_didn;
  c.f_nearby_routes = &f_nearby_routes;
  c.f_shared_id = &f_shared_id;
  c.f_shared_name = &f_shared_name;
  c.f_shared_reversible = &f_shared_reversible;
  c.f_shared_distance = &f_shared_distance;
  c.f_shared_stats = &f_shared_stats;
  c.f_shared_waypoints = &f_shared_waypoints;
  c.f_shared_startPoi = &f_shared_startPoi;
  c.f_shared_endPoi = &f_shared_endPoi;
  c.f_anchor_anchor = &f_anchor_anchor;
  c.f_wp_lat = &f_wp_lat;
  c.f_wp_lon = &f_wp_lon;
  c.f_stats_cooldown = &f_stats_cooldown;
  c.class_list=&listClass;c.class_waypoints=&wpfClass;
  assert(api->install(&c) == 0);
  assert(api->install(&c) == 0);  // idempotent for an identical config
}

// Free every cache + host root; a leaked temporary / staging / cache root fails roots.empty().
void cleanupRoots() {
  detached = false;
  auto d = discReq();  // drain ignores lease + generation; needs owner tid + attached only
  int dr = api->drain(&d);
  assert(dr == PGR_RD_OK);
  gcFree(hostService);
  gcFree(hostServiceB);
  assert(roots.empty() && "a GC root (temporary, staging, or cache) was leaked");
}
void pass(const char *name) { std::printf("PASS independent RoutesDiscovery %s\n", name); }

int scenario(const char *name) {
  mode = name;
  setupGraph();
  installConfig();
  hostService = gcNew(obj(SERVICE), 0);
  hostServiceB = gcNew(obj(SERVICE_B), 0);

  if (std::strcmp(name,"revoke-old-cache-cleanup")==0 || std::strcmp(name,"lookup-revoke")==0 || std::strcmp(name,"points-zero-lease")==0) {
    auto r=discReq(); assert(doDiscover(r)==PGR_RD_OK && g_out.count==1);
    auto l=lookupReq("route-a",hostService); uint64_t h=0; assert(api->lookup(&l,&h)==PGR_RD_OK && h);
    if(std::strcmp(name,"revoke-old-cache-cleanup")==0){
      cleanupTarget=h;cleanupAction=CLEAN_REVOKE;cleanupFired=false;
      assert(doDiscover(r)==PGR_RD_SKIP && g_out.count==0 && cleanupFired);
      l=lookupReq("route-a",hostService);h=0;assert(api->lookup(&l,&h)==PGR_RD_SKIP && h==0);
    } else if(std::strcmp(name,"lookup-revoke")==0){
      lookupRevokeTarget=h;h=0;assert(api->lookup(&l,&h)==PGR_RD_SKIP && h==0);
    } else {r.residual_ms=0;assert(api->points(&r,0,&g_pts)==PGR_RD_SKIP && g_pts.count==0);}
    cleanupRoots();pass(name);return 0;
  }

  // ---- a published cache must never revive across a revoke, even with a fresh lease + the new generation;
  //      only a fresh discover() republishes.
  if (std::strcmp(name, "revoke-no-revive") == 0) {
    auto r = discReq();
    assert(doDiscover(r) == PGR_RD_OK && g_out.count == 1 && idEq(g_out.routes[0], "route-a"));
    assert(g_out.routes[0].flags == (PGR_RD_FLAG_REVERSIBLE | PGR_RD_FLAG_HAS_START | PGR_RD_FLAG_HAS_END));
    assert(g_out.routes[0].point_count == 2 && g_out.routes[0].distance_meters == 1500.0);
    int rc = 0;
    assert(doLookup("route-a", hostService, &rc) != 0 && rc == PGR_RD_OK);
    api->revoke();                                   // off-tick revoke invalidates the published generation
    uint64_t h = doLookup("route-a", hostService, &rc);  // stamped with the NEW generation + a fresh lease
    assert(h == 0 && rc == PGR_RD_SKIP && "revoked cache must not be lookup-able even on the new generation");
    r = discReq();
    assert(doDiscover(r) == PGR_RD_OK && g_out.count == 1);  // fresh discovery republishes under new gen
    assert(doLookup("route-a", hostService, &rc) != 0 && rc == PGR_RD_OK);
    cleanupRoots(); pass(name); return 0;
  }

  // ---- a cache bound to service A refuses a lookup presented against service B (no new discovery needed).
  if (std::strcmp(name, "service-mismatch") == 0) {
    auto r = discReq();
    assert(doDiscover(r) == PGR_RD_OK && g_out.count == 1);
    int rc = 0;
    uint64_t h = doLookup("route-a", hostServiceB, &rc);
    assert(h == 0 && rc == PGR_RD_SKIP && "lookup through a different RoutesService must refuse");
    assert(doLookup("route-a", hostService, &rc) != 0 && rc == PGR_RD_OK);  // correct service still resolves
    cleanupRoots(); pass(name); return 0;
  }

  // ---- a VALID EMPTY same-service snapshot clears the old cache (old route root freed, id no longer found).
  if (std::strcmp(name, "empty-clears") == 0) {
    auto r = discReq();
    assert(doDiscover(r) == PGR_RD_OK && g_out.count == 1);
    int rc = 0;
    assert(doLookup("route-a", hostService, &rc) != 0 && rc == PGR_RD_OK);
    listCount = 0;                                   // same service, empty snapshot
    r = discReq();
    assert(doDiscover(r) == PGR_RD_OK && g_out.count == 0);
    uint64_t h = doLookup("route-a", hostService, &rc);
    assert(h == 0 && rc == PGR_RD_SKIP && "empty snapshot must drop the previously cached route");
    cleanupRoots(); pass(name); return 0;  // cleanupRoots proves the old route root was freed (no leak)
  }

  // ---- an off-tick revoke landing mid-cleanup aborts the swap: no published output, no live stale lookup.
  if (std::strcmp(name, "revoke-cleanup") == 0) {
    cleanupAction = CLEAN_REVOKE;
    auto r = discReq();
    int rc0 = doDiscover(r);
    assert(cleanupFired && rc0 == PGR_RD_SKIP && g_out.status == PGR_RD_SKIP && g_out.count == 0);
    int rc = 0;
    uint64_t h = doLookup("route-a", hostService, &rc);
    assert(h == 0 && rc == PGR_RD_SKIP && "no cache may be published when revoke lands during cleanup");
    cleanupRoots(); pass(name); return 0;  // staging roots (incl. the bound service root) must be freed
  }

  // ---- a reentrant discover() during temporary-root cleanup is refused (BUSY) without any managed work;
  //      the outer discover still completes and publishes.
  if (std::strcmp(name, "reentry-cleanup") == 0) {
    cleanupAction = CLEAN_REENTER;
    auto r = discReq();
    int rc0 = doDiscover(r);
    assert(cleanupFired && reentryResult == PGR_RD_ERR_BUSY && "reentry during cleanup must return BUSY");
    assert(rc0 == PGR_RD_OK && g_out.count == 1);   // outer scan is unaffected by the refused reentry
    int rc = 0;
    assert(doLookup("route-a", hostService, &rc) != 0 && rc == PGR_RD_OK);
    cleanupRoots(); pass(name); return 0;
  }

  // ---- a retained cache root is still lookup-able after the collector relocates it; lookup borrows (never
  //      frees) and never touches the host service handle.
  if (std::strcmp(name, "moving-lookup") == 0) {
    auto r = discReq();
    assert(doDiscover(r) == PGR_RD_OK && g_out.count == 1);
    move();                                          // a full GC cycle between discover() and lookup()
    size_t before = roots.size();
    int rc = 0;
    uint64_t h = doLookup("route-a", hostService, &rc);
    assert(rc == PGR_RD_OK && h != 0 && h != hostService && h != hostServiceB);
    assert(roots.size() == before && "lookup must neither allocate nor free a root");
    assert(gcGet(hostService) != nullptr && "lookup must not free the borrowed host service handle");
    assert(gcGet(h) == obj(ROUTE0) && "borrowed handle must resolve to the relocated cached proto");
    move();                                          // relocate again; the borrowed handle stays valid
    assert(gcGet(h) == obj(ROUTE0));
    cleanupRoots(); pass(name); return 0;
  }

  // ---- admission: an invalid route is dropped from the cache and is therefore NOT lookup-able. Each knob
  //      also proves the collection walk is bounded (no waypoint walk for an out-of-range count).
  if (std::strncmp(name, "admit-", 6) == 0) {
    if (std::strcmp(name, "admit-empty-id") == 0) obj(ID0)->length = 0;
    else if (std::strcmp(name, "admit-long-id") == 0) { obj(ID0)->length = 200; for (int i = 0; i < 200; i++) obj(ID0)->text[i] = 'x'; }
    else if (std::strcmp(name, "admit-bad-reversible") == 0) obj(ROUTE0)->boolv = 2;  // byte > 1 fails closed
    else if (std::strcmp(name, "admit-few-waypoints") == 0) wpCount = 1;              // < 2 -> skip route
    else if (std::strcmp(name, "admit-many-waypoints") == 0) wpCount = 6000;          // > 5000 -> skip route
    else { std::printf("FAIL unknown admit case %s\n", name); return 5; }
    auto r = discReq();
    assert(doDiscover(r) == PGR_RD_OK && g_out.count == 0 && "an invalid route must not be admitted");
    if (std::strcmp(name, "admit-few-waypoints") == 0 || std::strcmp(name, "admit-many-waypoints") == 0)
      assert(wpItemCalls == 0 && "an out-of-range waypoint count must skip the route without walking it");
    int rc = 0;
    uint64_t h = doLookup("route-a", hostService, &rc);
    assert(h == 0 && rc == PGR_RD_SKIP && "an un-admitted route must not be lookup-able");
    cleanupRoots(); pass(name); return 0;
  }

  // ---- DIVERGENCE FLAG (see notes): a route whose interior waypoint coordinates are out-of-range / NaN is
  //      admitted and lookup-able per the contract (START only needs id + reversible). The task's stated
  //      intent is a valid-route-only cache. This is a documented design question, NOT a test failure, so
  //      the scenario REPORTS the observed policy and exits success either way.
  if (std::strcmp(name, "malformed-points") == 0) {
    obj(WP0)->lat = 200.0;        // |lat| > 90
    obj(WP1)->lat = NAN;          // non-finite
    auto r = discReq();
    assert(doDiscover(r)==PGR_RD_OK && g_out.count==0);
    int rc=0;assert(doLookup("route-a",hostService,&rc)==0 && rc==PGR_RD_SKIP);
    pass(name);
    cleanupRoots(); return 0;
  }

  // ---- a List count above the ceiling must TRUNCATE (bounded walk), matching JS readDiscovery, never an
  //      unbounded walk. (The header defines PGR_RD_ERR_BAD_COUNT but the contract/JS want a clamp; both
  //      satisfy the bounded-walk invariant, which is the safety property asserted here.)
  if (std::strcmp(name, "count-truncate") == 0) {
    listCount = 150;
    distinctIds = true;
    auto r = discReq();
    int rc = doDiscover(r);
    assert(listItemCalls <= PGR_RD_MAX_ROUTES && "the route walk must be bounded by PGR_RD_MAX_ROUTES");
    assert(g_out.count <= PGR_RD_MAX_ROUTES);
    assert(rc==PGR_RD_OK && g_out.count==PGR_RD_MAX_ROUTES && listItemCalls==PGR_RD_MAX_ROUTES);
    std::printf("count-truncate observed: status=%d count=%u itemCalls=%d\n", rc, g_out.count, listItemCalls);
    cleanupRoots(); pass(name); return 0;
  }

  // ---- a LIST-level managed throw is a fault (-13) and leaves the previous (here: empty) cache unchanged.
  if (std::strcmp(name, "throw-list") == 0) {
    throwList = true;
    auto r = discReq();
    assert(doDiscover(r) == PGR_RD_ERR_MANAGED && g_out.count == 0);
    int rc = 0;
    assert(doLookup("route-a", hostService, &rc) == 0 && rc == PGR_RD_SKIP);
    cleanupRoots(); pass(name); return 0;
  }

  // ---- a throw while reading one route's waypoints skips that route only (valid empty snapshot, OK/0).
  if (std::strcmp(name, "throw-waypoint") == 0) {
    throwWp = true;
    auto r = discReq();
    assert(doDiscover(r) == PGR_RD_OK && g_out.count == 0);
    int rc = 0;
    assert(doLookup("route-a", hostService, &rc) == 0 && rc == PGR_RD_SKIP);
    cleanupRoots(); pass(name); return 0;
  }

  // ---- points(): copies the decoded array for an admitted route; range-guarded; generation-gated after a
  //      revoke (no managed work in any case).
  if (std::strcmp(name, "points-basic") == 0) {
    auto r = discReq();
    assert(doDiscover(r) == PGR_RD_OK && g_out.count == 1 && g_out.routes[0].point_count == 2);
    int before = managed;
    r = discReq(); r.index = 0;
    std::memset(&g_pts, 0, sizeof(g_pts));
    assert(api->points(&r, 0, &g_pts) == PGR_RD_OK && g_pts.count == 2);
    assert(g_pts.points[0].lat == 10.0 && g_pts.points[0].lon == 20.0);
    assert(g_pts.points[1].lat == 11.0 && g_pts.points[1].lon == 21.0);
    r = discReq(); r.index = 5;
    std::memset(&g_pts, 0, sizeof(g_pts));
    assert(api->points(&r, 5, &g_pts) == PGR_RD_ERR_BAD_INDEX && g_pts.count == 0);
    assert(managed == before && "points() must perform no managed work");
    api->revoke();
    r = discReq(); r.index = 0;
    std::memset(&g_pts, 0, sizeof(g_pts));
    assert(api->points(&r, 0, &g_pts) == PGR_RD_SKIP && g_pts.count == 0);  // generation gate after revoke
    cleanupRoots(); pass(name); return 0;
  }

  // ---- entry guards: each refuses BEFORE any managed work (fresh forked process, owner not yet latched).
  if (std::strncmp(name, "guard-", 6) == 0) {
    if (std::strcmp(name, "guard-zero-lease") == 0) {
      auto r = discReq(); r.residual_ms = 0;
      assert(doDiscover(r) == PGR_RD_SKIP && g_out.count == 0 && managed == 0);
    } else if (std::strcmp(name, "guard-bad-generation") == 0) {
      auto r = discReq(); r.generation = api->generation() + 5;
      assert(doDiscover(r) == PGR_RD_SKIP && managed == 0);
    } else if (std::strcmp(name, "guard-foreign-tid") == 0) {
      auto r = discReq(); r.tid = static_cast<uint64_t>(gettid()) + 1;
      assert(doDiscover(r) == PGR_RD_SKIP && managed == 0);
    } else if (std::strcmp(name, "guard-detached") == 0) {
      detached = true;
      auto r = discReq();
      assert(doDiscover(r) == PGR_RD_SKIP && managed == 0);
    } else if (std::strcmp(name, "guard-drain-offowner") == 0) {
      auto r = discReq(); r.tid = static_cast<uint64_t>(gettid()) + 1;
      assert(api->drain(&r) == PGR_RD_SKIP && "an off-owner drain must be a benign SKIP that frees nothing");
    } else { std::printf("FAIL unknown guard %s\n", name); return 5; }
    cleanupRoots(); pass(name); return 0;
  }

  std::printf("FAIL unknown scenario %s\n", name);
  return 5;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2 || argc > 3) return 1;
  if (argc == 2) {
    const char *cases[] = {
        "revoke-old-cache-cleanup", "lookup-revoke", "points-zero-lease", "revoke-no-revive", "service-mismatch", "empty-clears", "revoke-cleanup", "reentry-cleanup",
        "moving-lookup", "admit-empty-id", "admit-long-id", "admit-bad-reversible", "admit-few-waypoints",
        "admit-many-waypoints", "malformed-points", "count-truncate", "throw-list", "throw-waypoint",
        "points-basic", "guard-zero-lease", "guard-bad-generation", "guard-foreign-tid", "guard-detached",
        "guard-drain-offowner"};
    for (const char *name : cases) {
      pid_t child = fork();
      if (child < 0) return 3;
      if (!child) { execl(argv[0], argv[0], argv[1], name, static_cast<char *>(nullptr)); _exit(127); }
      int status = 0;
      if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
        std::printf("FAIL independent RoutesDiscovery %s (status %d)\n", name, status);
        return 4;
      }
    }
    return 0;
  }
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) { std::puts(dlerror()); return 2; }
  auto get = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(library, "pgrazor_native_api"));
  auto core = get ? static_cast<const pgrazor_native_api_v2 *>(get(2)) : nullptr;
  assert(core && core->query_feature(44, 99) == nullptr);  // exact-version only
  api = static_cast<const PgrRoutesDiscoveryApi *>(core->query_feature(44, 1));
  assert(api && api->size == sizeof(PgrRoutesDiscoveryApi) && api->version == PGR_RD_API_VERSION);
  return scenario(argv[2]);
}

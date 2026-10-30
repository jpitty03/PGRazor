#include "vgp_candidates.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <string>
#include <unistd.h>

// Real-API fake-IL2CPP harness for feature 29 (native VGP candidate inspection). It resolves the table
// through the exported pgrazor_native_api v2 registry (query_feature(29,1)) and drives inspect() against
// a fake managed heap: a wild pokemon, a stop map object and a fort with a FortId string. gchandle_new/
// get/free back a handle table so every temporary root and the transferred fort handle are accounted
// exactly (roots == 0 between calls). runtime_invoke dispatches by method token and can throw / return a
// wrong-class box. object_get_virtual_method resolves to the base token (identity vtable). The module
// installs NO hook and NEVER calls back into JS.

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- tokens (distinct addresses) ------------------------------------------------
int cls_wild, cls_stop, cls_fort, cls_bool, cls_latlng, cls_uint64, cls_string, cls_other;
int m_alive, m_location, m_id, m_visible, m_active, m_range, m_closed, m_cooling, m_fort;
int f_fort_id;

// ----- fake managed objects -------------------------------------------------------
struct BoolBox { void *klass; uint8_t v; };
struct LatLngBox { void *klass; double lat; double lon; };
struct U64Box { void *klass; uint64_t v; };
// System.String layout: klass @0, (sync) @8, length @16, chars @20.
struct FakeString { void *klass; uint64_t pad; int32_t length; uint16_t chars[300]; };
struct FakeFort { void *klass; void *id; };
struct FakeCandidate {
  void *klass;
  bool alive;
  bool visible;
  double lat, lon;
  uint64_t id;
  bool active, range, closed, cooling;
  void *fort;
};

BoolBox g_bool{&cls_bool, 0};
LatLngBox g_latlng{&cls_latlng, 0, 0};
U64Box g_u64{&cls_uint64, 0};
FakeString g_string{&cls_string, 0, 0, {}};
FakeFort g_fort{&cls_fort, &g_string};
FakeCandidate g_wild{&cls_wild, true, true, 0, 0, 0, false, false, false, false, nullptr};
FakeCandidate g_stop{&cls_stop, true, false, 0, 0, 0, true, true, false, false, &g_fort};
FakeCandidate g_moved_wild{}, g_moved_manual{};

// Configurable anomalies.
void *g_throw_method = nullptr;        // this method token throws
void *g_visible_box = &g_bool;         // box returned by the bool getters (wrong-class override)
void *g_location_box = &g_latlng;      // box returned by get_Location (wrong-class override)
void *g_id_box = &g_u64;               // box returned by get_EncounterId (wrong-class override)
int g_invokes = 0;
std::function<void()> g_oneshot_visible;  // fired inside get_VisibleOnMap (reentrancy / expiry)
std::function<void()> g_oneshot_alive;    // relocating collection inside the first managed call

// ----- gc handle table ------------------------------------------------------------
std::map<uintptr_t, void *> g_handles;
uintptr_t g_seq = 0x900;
int g_gcnew_calls = 0, g_gcnew_fail = 0;  // fail the Nth gchandle_new (1-based); 0 = never

uintptr_t fake_gc_new(void *obj, int) {
  ++g_gcnew_calls;
  if (!obj) return 0;
  if (g_gcnew_fail && g_gcnew_calls == g_gcnew_fail) return 0;
  uintptr_t h = ++g_seq;
  g_handles[h] = obj;
  return h;
}
void *fake_gc_get(uintptr_t h) { auto it = g_handles.find(h); return it == g_handles.end() ? nullptr : it->second; }
void fake_gc_free(uintptr_t h) { g_handles.erase(h); }
void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }
void *fake_virtual(void *, void *base) { return base; }  // identity vtable
void *fake_unbox(void *obj) {
  if (fake_obj_class(obj) == &cls_bool) return &static_cast<BoolBox *>(obj)->v;
  if (fake_obj_class(obj) == &cls_latlng) return &static_cast<LatLngBox *>(obj)->lat;
  if (fake_obj_class(obj) == &cls_uint64) return &static_cast<U64Box *>(obj)->v;
  return nullptr;
}
void fake_field_get(void *obj, void *field, void *out) {
  assert(field == &f_fort_id);
  *static_cast<void **>(out) = static_cast<FakeFort *>(obj)->id;
}

void *fake_invoke(void *method, void *obj, void **args, void **exc) {
  ++g_invokes;
  if (exc) *exc = nullptr;
  if (method == g_throw_method) { if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
  if (method == &m_alive) {
    g_bool.v = static_cast<FakeCandidate *>(args[0])->alive ? 1 : 0;
    if (g_oneshot_alive) { auto fn=g_oneshot_alive; g_oneshot_alive=nullptr; fn(); }
    return &g_bool;
  }
  if (method == &m_visible) {
    if (g_oneshot_visible) { auto fn = g_oneshot_visible; g_oneshot_visible = nullptr; fn(); }
    g_bool.v = static_cast<FakeCandidate *>(obj)->visible ? 1 : 0; return g_visible_box;
  }
  if (method == &m_active) { g_bool.v = static_cast<FakeCandidate *>(obj)->active ? 1 : 0; return &g_bool; }
  if (method == &m_range) { g_bool.v = static_cast<FakeCandidate *>(obj)->range ? 1 : 0; return &g_bool; }
  if (method == &m_closed) { g_bool.v = static_cast<FakeCandidate *>(obj)->closed ? 1 : 0; return &g_bool; }
  if (method == &m_cooling) { g_bool.v = static_cast<FakeCandidate *>(obj)->cooling ? 1 : 0; return &g_bool; }
  if (method == &m_fort) { return static_cast<FakeCandidate *>(obj)->fort; }
  if (method == &m_location) {
    g_latlng.lat = static_cast<FakeCandidate *>(obj)->lat;
    g_latlng.lon = static_cast<FakeCandidate *>(obj)->lon;
    return g_location_box;
  }
  if (method == &m_id) { g_u64.v = static_cast<FakeCandidate *>(obj)->id; return g_id_box; }
  assert(false && "unknown method token");
  return nullptr;
}

PgrVgpCandidateConfig make_config() {
  PgrVgpCandidateConfig c{};
  c.version = PGR_VC_CONFIG_VERSION;
  c.size = sizeof(c);
  c.gchandle_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gchandle_get_target = reinterpret_cast<void *>(&fake_gc_get);
  c.gchandle_free = reinterpret_cast<void *>(&fake_gc_free);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_invoke);
  c.object_unbox = reinterpret_cast<void *>(&fake_unbox);
  c.object_get_virtual_method = reinterpret_cast<void *>(&fake_virtual);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.m_alive = &m_alive;
  c.m_location = &m_location;
  c.m_id = &m_id;
  c.m_visible = &m_visible;
  c.m_active = &m_active;
  c.m_range = &m_range;
  c.m_closed = &m_closed;
  c.m_cooling = &m_cooling;
  c.m_fort = &m_fort;
  c.f_fort_id = &f_fort_id;
  c.class_wild = &cls_wild;
  c.class_stop = &cls_stop;
  c.class_fort = &cls_fort;
  c.class_bool = &cls_bool;
  c.class_latlng = &cls_latlng;
  c.class_uint64 = &cls_uint64;
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

PgrVgpCandidateRequest make_request(uint32_t action, void *target, void *manual,
                                    double lat, double lon, uint64_t tid, uint64_t lease) {
  PgrVgpCandidateRequest r{};
  r.version = PGR_VC_REQUEST_VERSION;
  r.size = sizeof(r);
  r.action = action;
  r.owner_tid = tid;
  r.lease_ms = lease;
  r.lat = lat;
  r.lon = lon;
  r.target = target;
  r.manual = manual;
  return r;
}

void set_string(const uint16_t *units, int32_t len) {
  g_string.length = len;
  for (int i = 0; i < len && i < 300; ++i) g_string.chars[i] = units[i];
}

const PgrVgpCandidateApi *g_api = nullptr;

void reset() {
  assert(g_handles.empty());
  g_wild = {&cls_wild, true, true, 0.0, 0.0, 7, false, false, false, false, nullptr};
  g_stop = {&cls_stop, true, false, 0, 0, 0, true, true, false, false, &g_fort};
  g_fort.klass = &cls_fort;
  g_fort.id = &g_string;
  const uint16_t id_chars[] = {'A', 'B', '1', '2'};
  set_string(id_chars, 4);
  g_throw_method = nullptr;
  g_visible_box = &g_bool;
  g_location_box = &g_latlng;
  g_id_box = &g_u64;
  g_invokes = 0;
  g_gcnew_calls = 0;
  g_gcnew_fail = 0;
  g_oneshot_visible = nullptr;
  g_oneshot_alive = nullptr;
}

std::string key_of(const PgrVgpCandidateResult &out) { return std::string(out.key, out.key_len); }

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) { std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror()); return 1; }
  auto getter = reinterpret_cast<GetApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == 48);
  assert(core->query_feature(29, 99) == nullptr);
  const auto *api = static_cast<const PgrVgpCandidateApi *>(core->query_feature(29, 1));
  assert(api && api->version == 1 && api->size == sizeof(PgrVgpCandidateApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu request=%zu result=%zu table=%zu\n",
              sizeof(PgrVgpCandidateConfig), sizeof(PgrVgpCandidateRequest),
              sizeof(PgrVgpCandidateResult), sizeof(PgrVgpCandidateApi));
  assert(sizeof(PgrVgpCandidateConfig) == 200 && sizeof(PgrVgpCandidateRequest) == 64 &&
         sizeof(PgrVgpCandidateResult) == 1072 && sizeof(PgrVgpCandidateApi) == 24);

  const uint64_t tid = this_tid();
  PgrVgpCandidateConfig cfg = make_config();
  PgrVgpCandidateResult out{};

  // 1. install validation + not-installed + idempotence + reconfig.
  assert(api->install(nullptr) == PGR_VC_ERR_NULL_ARG);
  { PgrVgpCandidateConfig b = cfg; b.version = 2; assert(api->install(&b) == PGR_VC_ERR_BAD_VERSION); }
  { PgrVgpCandidateConfig b = cfg; b.size = sizeof(cfg) - 1; assert(api->install(&b) == PGR_VC_ERR_BAD_SIZE); }
  { PgrVgpCandidateConfig b = cfg; b.m_fort = nullptr; assert(api->install(&b) == PGR_VC_ERR_MISSING_PTR); }
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ERR_NOT_INSTALLED); }
  assert(api->install(&cfg) == 0);
  assert(api->install(&cfg) == 0);  // idempotent
  { PgrVgpCandidateConfig b = cfg; b.class_wild = &cls_stop; assert(api->install(&b) == PGR_VC_ERR_RECONFIG); }

  // 2. Early-rejection guards invoke NOTHING managed.
  reset();
  { PgrVgpCandidateRequest r = make_request(5, &g_wild, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ERR_BAD_ACTION && g_invokes == 0); }
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, nullptr, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ERR_NULL_ARG && g_invokes == 0); }
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid + 123, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ERR_THREAD && g_invokes == 0); }
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, 0, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ERR_THREAD && g_invokes == 0); }
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 200.0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ERR_COORDS && g_invokes == 0); }
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, std::nan(""), 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ERR_COORDS && g_invokes == 0); }
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 0);  // closed lease
    assert(api->inspect(&r, &out) == PGR_VC_NOT && out.reason == PGR_VC_REASON_INVALID && g_invokes == 0); }
  assert(g_handles.empty());

  // 3. Catch eligible: UInt64 key, no fort handle, roots clean.
  reset();
  g_wild.id = 123456789ULL;
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ELIGIBLE);
    assert(out.eligible == 1 && out.reason == PGR_VC_REASON_NONE && out.fort_handle == 0);
    assert(out.version == 1 && out.size == sizeof(out));
    assert(key_of(out) == "p123456789" && out.key[out.key_len] == '\0'); }
  assert(g_handles.empty());

  // exact id: UINT64_MAX is preserved digit-for-digit.
  reset();
  g_wild.id = UINT64_MAX;
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ELIGIBLE && key_of(out) == "p18446744073709551615"); }
  assert(g_handles.empty());

  // 4. Catch rejections: wrong class, not alive, manual, hidden -- each short-circuits further getters.
  reset();
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_stop, nullptr, 0, 0, tid, 2000);  // stop as wild
    assert(api->inspect(&r, &out) == PGR_VC_NOT && out.reason == PGR_VC_REASON_INVALID && g_invokes == 0); }
  reset();
  g_wild.alive = false;
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_NOT && out.reason == PGR_VC_REASON_INVALID && g_invokes == 1); }
  reset();
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, &g_wild, 0, 0, tid, 2000);  // manual==target
    assert(api->inspect(&r, &out) == PGR_VC_NOT && out.reason == PGR_VC_REASON_MANUAL);
    assert(g_invokes == 1 && out.fort_handle == 0); }
  reset();
  g_wild.visible = false;
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_NOT && out.reason == PGR_VC_REASON_HIDDEN); }
  // a non-identical manual target does NOT exclude.
  reset();
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, &g_stop, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ELIGIBLE); }
  assert(g_handles.empty());

  // Both incoming references must be rooted BEFORE alive can collect. Alias exclusion must compare
  // current handle targets, never a relocated target against a stale raw manual pointer.
  for (bool aliases : {true, false}) {
    reset();
    g_oneshot_alive = [aliases]() {
      int targetRoots=0, manualRoots=0;
      for (const auto &entry:g_handles) {
        if(entry.second==&g_wild)++targetRoots;
        if(entry.second==&g_stop)++manualRoots;
      }
      assert(targetRoots==(aliases?2:1));
      assert(aliases||manualRoots==1);
      g_moved_wild=g_wild;g_moved_manual=g_stop;
      for(auto &entry:g_handles) {
        if(entry.second==&g_wild)entry.second=&g_moved_wild;
        else if(entry.second==&g_stop)entry.second=&g_moved_manual;
      }
      // Old storage remains readable as poisoned memory so stale-pointer regressions fail reliably.
      g_wild.klass=&cls_other;g_wild.visible=false;g_stop.klass=&cls_other;
    };
    PgrVgpCandidateRequest r=make_request(PGR_VC_ACTION_CATCH,&g_wild,aliases?static_cast<void*>(&g_wild):static_cast<void*>(&g_stop),0,0,tid,2000);
    const int rc=api->inspect(&r,&out);
    if(aliases)assert(rc==PGR_VC_NOT&&out.reason==PGR_VC_REASON_MANUAL&&g_invokes==1);
    else assert(rc==PGR_VC_ELIGIBLE&&key_of(out)=="p7");
    assert(out.fort_handle==0&&g_handles.empty());
  }

  // 5. 40 m boundary: in-range (~0 m, ~33 m) eligible, out-of-range (~50 m) rejected.
  reset();
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 2000);
    g_wild.lat = 0; g_wild.lon = 0; assert(api->inspect(&r, &out) == PGR_VC_ELIGIBLE); }
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 2000);
    g_wild.lat = 0.00030; assert(api->inspect(&r, &out) == PGR_VC_ELIGIBLE); }  // ~33 m
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 2000);
    g_wild.lat = 0.00045;  // ~50 m
    assert(api->inspect(&r, &out) == PGR_VC_NOT && out.reason == PGR_VC_REASON_OUT_OF_RANGE); }
  assert(g_handles.empty());

  // Validate the candidate's own location before haversine: 360 degrees otherwise wraps to zero
  // Smart mode may attempt loaded visible spawns beyond the legacy 40 m cutoff.
  for(double latitude:{0.00045,0.00072,0.0018}){
    reset();g_wild.lat=latitude;
    auto r=make_request(PGR_VC_ACTION_SMART_CATCH,&g_wild,nullptr,0,0,tid,2000);
    assert(api->inspect(&r,&out)==PGR_VC_ELIGIBLE&&g_handles.empty());
    g_wild.visible=false;assert(api->inspect(&r,&out)==PGR_VC_NOT&&out.reason==PGR_VC_REASON_HIDDEN);
    g_wild.visible=true;r.manual=&g_wild;assert(api->inspect(&r,&out)==PGR_VC_NOT&&out.reason==PGR_VC_REASON_MANUAL);
    assert(g_handles.empty());
  }

  // Validate the candidate's own location before haversine: 360 degrees otherwise wraps to zero
  // distance. Nonfinite and out-of-domain coordinates are benign range rejections, never pauses.
  const double malformed[][2]={{NAN,0},{INFINITY,0},{-INFINITY,0},{0,NAN},{0,INFINITY},
    {90.0001,0},{-90.0001,0},{0,180.0001},{0,-180.0001},{360,0},{0,360}};
  for(const auto &location:malformed) {
    reset();g_wild.lat=location[0];g_wild.lon=location[1];
    PgrVgpCandidateRequest r=make_request(PGR_VC_ACTION_CATCH,&g_wild,nullptr,0,0,tid,2000);
    assert(api->inspect(&r,&out)==PGR_VC_NOT&&out.reason==PGR_VC_REASON_OUT_OF_RANGE);
    assert(out.eligible==0&&out.fort_handle==0&&g_handles.empty());
    r.action=PGR_VC_ACTION_SMART_CATCH;
    assert(api->inspect(&r,&out)==PGR_VC_NOT&&out.reason==PGR_VC_REASON_OUT_OF_RANGE&&g_handles.empty());
  }

  // 6. Spin eligible: fort handle transferred (owned), ASCII key, roots clean once the caller frees it.
  reset();
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_SPIN, &g_stop, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ELIGIBLE);
    assert(out.eligible == 1 && out.reason == PGR_VC_REASON_NONE && out.fort_handle != 0);
    assert(key_of(out) == "sAB12" && out.key[out.key_len] == '\0');
    assert(fake_gc_get(out.fort_handle) == &g_fort);  // handle targets the FortData
    assert(g_handles.size() == 1);                    // exactly the transferred handle
    fake_gc_free(out.fort_handle); }
  assert(g_handles.empty());

  // 7. Spin predicate rejections (each -> INELIGIBLE), none transfers a handle.
  for (int which = 0; which < 4; ++which) {
    reset();
    if (which == 0) g_stop.active = false;
    else if (which == 1) g_stop.range = false;
    else if (which == 2) g_stop.closed = true;
    else g_stop.cooling = true;
    PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_SPIN, &g_stop, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_NOT && out.reason == PGR_VC_REASON_INELIGIBLE && out.fort_handle == 0);
  }
  assert(g_handles.empty());

  // 8. Spin fort faults: wrong fort class, null FortId, malformed length (0, -1, 257) -> INELIGIBLE.
  reset();
  g_stop.fort=nullptr;
  { PgrVgpCandidateRequest r=make_request(PGR_VC_ACTION_SPIN,&g_stop,nullptr,0,0,tid,2000);
    assert(api->inspect(&r,&out)==PGR_VC_NOT&&out.reason==PGR_VC_REASON_INELIGIBLE);
    assert(out.eligible==0&&out.fort_handle==0&&g_handles.empty()); }
  reset();
  g_fort.klass = &cls_other;
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_SPIN, &g_stop, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_NOT && out.reason == PGR_VC_REASON_INELIGIBLE); }
  reset();
  g_fort.id = nullptr;
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_SPIN, &g_stop, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_NOT && out.reason == PGR_VC_REASON_INELIGIBLE); }
  for (int32_t bad : {0, -1, 257, -2147483647 - 1}) {
    reset();
    g_string.length = bad;
    PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_SPIN, &g_stop, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_NOT && out.reason == PGR_VC_REASON_INELIGIBLE);  // never truncated
  }
  // exact max length (256 units) is accepted, not rejected.
  reset();
  { uint16_t big[256]; for (auto &u : big) u = 'x'; set_string(big, 256);
    PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_SPIN, &g_stop, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ELIGIBLE && out.key_len == 257 && out.key[0] == 's');
    fake_gc_free(out.fort_handle); }
  assert(g_handles.empty());

  // 9. Unicode FortId: BMP + a surrogate pair convert to exact UTF-8 bytes.
  reset();
  { const uint16_t uni[] = {'A', 0x00E9, 0xD83D, 0xDE00};  // 'A', e-acute, emoji U+1F600
    set_string(uni, 4);
    PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_SPIN, &g_stop, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ELIGIBLE);
    const unsigned char expect[] = {'s', 'A', 0xC3, 0xA9, 0xF0, 0x9F, 0x98, 0x80};
    assert(out.key_len == sizeof(expect));
    assert(std::memcmp(out.key, expect, sizeof(expect)) == 0);
    fake_gc_free(out.fort_handle); }
  // a lone high surrogate degrades to U+FFFD rather than corrupting the key.
  reset();
  { const uint16_t lone[] = {0xD83D, 'Z'};
    set_string(lone, 2);
    PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_SPIN, &g_stop, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ELIGIBLE);
    const unsigned char expect[] = {'s', 0xEF, 0xBF, 0xBD, 'Z'};
    assert(out.key_len == sizeof(expect) && std::memcmp(out.key, expect, sizeof(expect)) == 0);
    fake_gc_free(out.fort_handle); }
  assert(g_handles.empty());

  // 10. Value-class / metadata anomalies -> negative ERR_METADATA, roots freed.
  reset();
  g_location_box = &g_u64;  // get_Location returns a non-LatLng box
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ERR_METADATA); }
  reset();
  g_id_box = &g_latlng;  // get_EncounterId returns a non-UInt64 box
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ERR_METADATA); }
  reset();
  g_visible_box = &g_latlng;  // get_VisibleOnMap returns a non-Boolean box
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ERR_METADATA); }
  assert(g_handles.empty());

  // 11. Managed exception from any getter -> ERR_MANAGED, roots freed.
  for (void *throwing : {static_cast<void *>(&m_alive), static_cast<void *>(&m_visible),
                         static_cast<void *>(&m_location), static_cast<void *>(&m_id)}) {
    reset();
    g_throw_method = throwing;
    PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ERR_MANAGED);
  }
  for (void *throwing : {static_cast<void *>(&m_active), static_cast<void *>(&m_range),
                         static_cast<void *>(&m_closed), static_cast<void *>(&m_cooling),
                         static_cast<void *>(&m_fort)}) {
    reset();
    g_throw_method = throwing;
    PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_SPIN, &g_stop, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ERR_MANAGED);
  }
  assert(g_handles.empty());

  // 12. Fail each gchandle_new in turn -> negative (GCHANDLE), never a false eligible, roots freed.
  for (int n = 1; n <= 4; ++n) {
    reset();
    g_gcnew_fail = n;
    g_wild.id = 42;
    PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, &g_stop, 0, 0, tid, 2000);
    int rc = api->inspect(&r, &out);
    if (n <= g_gcnew_calls) assert(rc < 0 && out.eligible == 0);
    assert(g_handles.empty());
  }
  for (int n = 1; n <= 3; ++n) {
    reset();
    g_gcnew_fail = n;
    PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_SPIN, &g_stop, nullptr, 0, 0, tid, 2000);
    int rc = api->inspect(&r, &out);
    if (n <= g_gcnew_calls) assert(rc < 0 && out.eligible == 0 && out.fort_handle == 0);
    assert(g_handles.empty());
  }

  // 13. Reentrant inspect (fired inside a getter) is refused as busy; outer call still succeeds.
  reset();
  int nested = 99;
  g_oneshot_visible = [&]() {
    PgrVgpCandidateResult inner{};
    PgrVgpCandidateRequest ir = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, this_tid(), 2000);
    nested = g_api->inspect(&ir, &inner);
  };
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 2000);
    assert(api->inspect(&r, &out) == PGR_VC_ELIGIBLE);
    assert(nested == PGR_VC_ERR_THREAD); }  // busy guard
  assert(g_handles.empty());

  // 14. Lease expiry discovered mid-call: a slow getter pushes past the deadline -> benign NOT/INVALID.
  reset();
  g_oneshot_visible = [&]() { ::usleep(4000); };  // fired inside get_VisibleOnMap, after the 1 ms lease
  { PgrVgpCandidateRequest r = make_request(PGR_VC_ACTION_CATCH, &g_wild, nullptr, 0, 0, tid, 1);
    int rc = api->inspect(&r, &out);
    assert(rc == PGR_VC_NOT && out.reason == PGR_VC_REASON_INVALID && out.eligible == 0); }
  assert(g_handles.empty());

  puts("PASS VGP candidate inspection: ABI, install/validation/idempotence/reconfig, catch eligible "
       "(UInt64/exact-max id), wrong-class/dead/manual/hidden, 40 m boundary, spin eligible (owned fort "
       "handle + ASCII key), every stop predicate, fort class/null/malformed-length (no truncation), "
       "256-unit + Unicode/surrogate/lone-surrogate keys, value-class metadata, managed throws, "
       "gchandle failures, relocation/manual rooting, malformed locations/null fort, reentrancy busy, lease expiry, roots zero");
  dlclose(lib);
  return 0;
}

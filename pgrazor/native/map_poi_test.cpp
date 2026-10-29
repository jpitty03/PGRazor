#include "map_poi.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <unistd.h>

// Real-API fake-IL2CPP harness for feature 42 (native map POI snapshot). It resolves the table through the
// exported pgrazor_native_api v2 registry (query_feature(42,1)) and drives scan() against a fake managed
// heap: PokeStop/Gym candidates, each exposing a virtually-resolved Fort (System.String id + lat/lon +
// optional gym display + raid info), boxed Boolean alive() results, a boxed team enum, and bounded
// UnityEngine.Object[] arrays whose element data begins at offset 32. gchandle_new/get/free back a handle
// table so every temporary root is accounted exactly (the heap MUST be empty between calls -- no managed
// pointer or handle ever leaves the module; the output holds only copied scalars + copied UTF16).
// runtime_invoke dispatches by method token and can throw. The module installs NO hook and NEVER calls back
// into JS; it only reads. NOTE: install is a process-once commit, so the harness commits a FULL config (all
// optional groups enabled) and exercises partial-group REJECTION before that commit.

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class tokens (distinct addresses) ------------------------------------------
int cls_stop, cls_gym, cls_fort, cls_raid, cls_bool, cls_string, cls_type, cls_objarray,
    cls_teamBox, cls_gymDisplay, cls_boss, cls_other;
// ----- method / field tokens ------------------------------------------------------
int m_find, m_alive, m_fort, m_fort_resolved, m_team;
int f_fortId, f_lat, f_lon, f_raidInfo, f_spawn, f_end, f_battle, f_hidden,
    f_gymDisplay, f_slots, f_level, f_boss, f_bossId;

// ----- fake managed objects -------------------------------------------------------
struct FakeString { void *klass; int32_t length; uint16_t chars[64]; };
struct FakeAliveBox { void *klass; uint8_t value; };        // unbox -> &value
struct FakeTeamBox { void *klass; int32_t value; };         // unbox -> &value
struct FakeBoss { void *klass; int32_t bossId; };
struct FakeGymDisplay { void *klass; int32_t slots; };
struct FakeRaid { void *klass; uint8_t hidden; uint64_t spawn, end, battle; int32_t level; void *boss; };
struct FakeFort { void *klass; void *id; double lat, lon; void *raidInfo; void *gymDisplay; };
struct FakeCandidate { void *klass; uint8_t alive; void *fort; int32_t teamId; };
// LP64 Object[]: klass @0, padding, length @24, element data @32.
struct FakeArray { void *klass; uint64_t pad0; uint64_t pad1; uint64_t length; void *data[32]; };

void set_id(FakeString &s, const char *ascii) {
  s.klass = &cls_string;
  int32_t n = static_cast<int32_t>(std::strlen(ascii));
  s.length = n;
  for (int32_t i = 0; i < n && i < 64; ++i) s.chars[i] = static_cast<uint16_t>(ascii[i]);
}

FakeAliveBox g_aliveBox{&cls_bool, 1};
FakeTeamBox g_teamBox{&cls_teamBox, 0};
void *g_team_ret = &g_teamBox;  // what m_team returns (null to simulate no team)

// configurable arrays returned per class
void *g_stopArray = nullptr;
void *g_gymArray = nullptr;

void *g_throw_method = nullptr;
int g_invokes = 0;
std::function<void()> g_oneshot;       // fired inside the method named by g_oneshot_on
void *g_oneshot_on = nullptr;
std::function<void()> g_oneshot_arrlen; // fired inside array_length
std::function<void()> g_oneshot_gcnew;  // fired inside gchandle_new when the rooted obj == g_gcnew_fire_on
void *g_gcnew_fire_on = nullptr;        // target object that triggers g_oneshot_gcnew (relocation)

// ----- gc handle table ------------------------------------------------------------
std::map<uintptr_t, void *> g_handles;
uintptr_t g_seq = 0x900;
int g_gcnew_calls = 0, g_gcnew_fail = 0;  // fail the Nth gchandle_new (1-based); 0 = never
size_t g_max_live = 0;                    // peak live handle count during a scan

uintptr_t fake_gc_new(void *obj, int) {
  ++g_gcnew_calls;
  if (g_oneshot_gcnew && obj == g_gcnew_fire_on) { auto fn = g_oneshot_gcnew; g_oneshot_gcnew = nullptr; fn(); }
  if (!obj) return 0;
  if (g_gcnew_fail && g_gcnew_calls == g_gcnew_fail) return 0;
  uintptr_t h = ++g_seq;
  g_handles[h] = obj;
  if (g_handles.size() > g_max_live) g_max_live = g_handles.size();
  return h;
}
void *fake_gc_get(uintptr_t h) { auto it = g_handles.find(h); return it == g_handles.end() ? nullptr : it->second; }
void fake_gc_free(uintptr_t h) { g_handles.erase(h); }
void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }
void *g_thread = reinterpret_cast<void *>(0x7700);  // attached Unity thread sentinel (null = detached)
void *fake_thread_current() { return g_thread; }

std::map<void *, void *> g_typeObjs;  // class token -> FakeType (identity-ish)
struct FakeType { void *klass; void *represents; };
void *fake_class_get_type(void *klass) { return klass; }  // type token == class token
void *fake_type_get_object(void *type) {
  auto it = g_typeObjs.find(type);
  if (it != g_typeObjs.end()) return it->second;
  FakeType *t = new FakeType{&cls_type, type};
  g_typeObjs[type] = t;
  return t;
}
uintptr_t fake_array_length(void *a) {
  uintptr_t len = static_cast<uintptr_t>(static_cast<FakeArray *>(a)->length);
  if (g_oneshot_arrlen) { auto fn = g_oneshot_arrlen; g_oneshot_arrlen = nullptr; fn(); }
  return len;
}
void *fake_virtual_method(void *, void *method) { return method == &m_fort ? &m_fort_resolved : nullptr; }
int32_t fake_string_length(void *s) { return static_cast<FakeString *>(s)->length; }
uint16_t *fake_string_chars(void *s) { return static_cast<FakeString *>(s)->chars; }

void *fake_object_unbox(void *obj) {
  void *klass = fake_obj_class(obj);
  if (klass == &cls_bool) return &static_cast<FakeAliveBox *>(obj)->value;
  if (klass == &cls_teamBox) return &static_cast<FakeTeamBox *>(obj)->value;
  return nullptr;
}

void fake_field_get_value(void *obj, void *field, void *out) {
  if (field == &f_fortId) { *static_cast<void **>(out) = static_cast<FakeFort *>(obj)->id; return; }
  if (field == &f_lat) { *static_cast<double *>(out) = static_cast<FakeFort *>(obj)->lat; return; }
  if (field == &f_lon) { *static_cast<double *>(out) = static_cast<FakeFort *>(obj)->lon; return; }
  if (field == &f_raidInfo) { *static_cast<void **>(out) = static_cast<FakeFort *>(obj)->raidInfo; return; }
  if (field == &f_gymDisplay) { *static_cast<void **>(out) = static_cast<FakeFort *>(obj)->gymDisplay; return; }
  if (field == &f_hidden) { *static_cast<uint8_t *>(out) = static_cast<FakeRaid *>(obj)->hidden; return; }
  if (field == &f_spawn) { *static_cast<uint64_t *>(out) = static_cast<FakeRaid *>(obj)->spawn; return; }
  if (field == &f_end) { *static_cast<uint64_t *>(out) = static_cast<FakeRaid *>(obj)->end; return; }
  if (field == &f_battle) { *static_cast<uint64_t *>(out) = static_cast<FakeRaid *>(obj)->battle; return; }
  if (field == &f_level) { *static_cast<int32_t *>(out) = static_cast<FakeRaid *>(obj)->level; return; }
  if (field == &f_boss) { *static_cast<void **>(out) = static_cast<FakeRaid *>(obj)->boss; return; }
  if (field == &f_slots) { *static_cast<int32_t *>(out) = static_cast<FakeGymDisplay *>(obj)->slots; return; }
  if (field == &f_bossId) { *static_cast<int32_t *>(out) = static_cast<FakeBoss *>(obj)->bossId; return; }
  assert(false && "unknown field token");
}

void *fake_invoke(const void *method, void *obj, void **args, void **exc) {
  ++g_invokes;
  if (exc) *exc = nullptr;
  if (method == g_throw_method) { if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
  if (g_oneshot && method == g_oneshot_on) { auto fn = g_oneshot; g_oneshot = nullptr; fn(); }
  if (method == &m_find) {
    void *rep = static_cast<FakeType *>(args[0])->represents;
    if (rep == &cls_stop) return g_stopArray;
    if (rep == &cls_gym) return g_gymArray;
    assert(false && "unexpected find type");
    return nullptr;
  }
  if (method == &m_alive) {
    g_aliveBox.value = static_cast<FakeCandidate *>(args[0])->alive;
    return &g_aliveBox;
  }
  if (method == &m_fort_resolved) { return static_cast<FakeCandidate *>(obj)->fort; }
  if (method == &m_team) {
    if (!g_team_ret) return nullptr;
    g_teamBox.value = static_cast<FakeCandidate *>(obj)->teamId;
    return g_team_ret;
  }
  assert(false && "unknown method token");
  return nullptr;
}

PgrMapPoiConfig make_config(bool team, bool slots, bool level, bool boss) {
  PgrMapPoiConfig c{};
  c.version = PGR_MP_CONFIG_VERSION;
  c.size = sizeof(c);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_invoke);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get_value);
  c.object_unbox = reinterpret_cast<void *>(&fake_object_unbox);
  c.gchandle_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gchandle_get_target = reinterpret_cast<void *>(&fake_gc_get);
  c.gchandle_free = reinterpret_cast<void *>(&fake_gc_free);
  c.thread_current = reinterpret_cast<void *>(&fake_thread_current);
  c.class_get_type = reinterpret_cast<void *>(&fake_class_get_type);
  c.type_get_object = reinterpret_cast<void *>(&fake_type_get_object);
  c.array_length = reinterpret_cast<void *>(&fake_array_length);
  c.object_get_virtual_method = reinterpret_cast<void *>(&fake_virtual_method);
  c.string_length = reinterpret_cast<void *>(&fake_string_length);
  c.string_chars = reinterpret_cast<void *>(&fake_string_chars);
  c.class_stop = &cls_stop;
  c.class_gym = &cls_gym;
  c.class_fort = &cls_fort;
  c.class_raid = &cls_raid;
  c.class_bool = &cls_bool;
  c.class_string = &cls_string;
  c.m_find = &m_find;
  c.m_alive = &m_alive;
  c.m_fort = &m_fort;
  c.f_fortId = &f_fortId;
  c.f_lat = &f_lat;
  c.f_lon = &f_lon;
  c.f_raidInfo = &f_raidInfo;
  c.f_spawn = &f_spawn;
  c.f_end = &f_end;
  c.f_battle = &f_battle;
  c.f_hidden = &f_hidden;
  if (team) { c.m_team = &m_team; c.class_teamBox = &cls_teamBox; }
  if (slots) { c.f_gymDisplay = &f_gymDisplay; c.class_gymDisplay = &cls_gymDisplay; c.f_slots = &f_slots; }
  if (level) { c.f_level = &f_level; }
  if (boss) { c.f_boss = &f_boss; c.class_boss = &cls_boss; c.f_bossId = &f_bossId; }
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

PgrMapPoiRequest make_request(uint64_t tid, uint64_t remaining, uint64_t wall) {
  PgrMapPoiRequest r{};
  r.version = PGR_MP_REQUEST_VERSION;
  r.size = sizeof(r);
  r.tid = tid;
  r.remaining_ms = remaining;
  r.wall_ms = wall;
  return r;
}

void relocate(void *from, void *to) {
  for (auto &e : g_handles) if (e.second == from) e.second = to;
}

void reset() {
  assert(g_handles.empty());
  g_stopArray = nullptr;
  g_gymArray = nullptr;
  g_team_ret = &g_teamBox;
  g_throw_method = nullptr;
  g_invokes = 0;
  g_gcnew_fail = 0;
  g_gcnew_calls = 0;
  g_max_live = 0;
  g_oneshot = nullptr;
  g_oneshot_on = nullptr;
  g_oneshot_arrlen = nullptr;
  g_oneshot_gcnew = nullptr;
  g_gcnew_fire_on = nullptr;
  g_thread = reinterpret_cast<void *>(0x7700);
}

bool id_is(const PgrMapPoiPoint &p, const char *ascii) {
  size_t n = std::strlen(ascii);
  if (p.id_length != n) return false;
  for (size_t i = 0; i < n; ++i) if (p.id[i] != static_cast<uint16_t>(ascii[i])) return false;
  return true;
}

const PgrMapPoiApi *g_api = nullptr;
const uint64_t WALL = 1000000;  // host wall clock for the raid window

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) { std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror()); return 1; }
  auto getter = reinterpret_cast<GetApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == 48);
  assert(core->query_feature(42, 99) == nullptr);
  const auto *api = static_cast<const PgrMapPoiApi *>(core->query_feature(42, 1));
  assert(api && api->version == 1 && api->size == sizeof(PgrMapPoiApi));
  g_api = api;

  // 0. ABI layout.
  std::printf("ABI sizes: config=%zu request=%zu point=%zu output=%zu table=%zu\n",
              sizeof(PgrMapPoiConfig), sizeof(PgrMapPoiRequest), sizeof(PgrMapPoiPoint),
              sizeof(PgrMapPoiOutput), sizeof(PgrMapPoiApi));
  assert(sizeof(PgrMapPoiConfig) == 328 && sizeof(PgrMapPoiRequest) == 32 &&
         sizeof(PgrMapPoiPoint) == 576 && sizeof(PgrMapPoiOutput) == 294928 &&
         sizeof(PgrMapPoiApi) == 24);
  assert(offsetof(PgrMapPoiConfig, f_bossId) == 320);
  assert(offsetof(PgrMapPoiRequest, wall_ms) == 24);
  assert(offsetof(PgrMapPoiPoint, id) == 64 && offsetof(PgrMapPoiPoint, raid_end) == 32);
  assert(offsetof(PgrMapPoiOutput, points) == 16 && offsetof(PgrMapPoiOutput, count) == 12);

  const uint64_t tid = this_tid();
  // The output is heap-allocated (294928 bytes); never a stack object inside the module or the harness.
  PgrMapPoiOutput *out = new PgrMapPoiOutput{};
  PgrMapPoiConfig full = make_config(true, true, true, true);

  // 1. install validation: header, mandatory ptr, partial optional groups (rejected, NOT committed).
  assert(api->install(nullptr) == PGR_MP_ERR_NULL_ARG);
  { PgrMapPoiConfig b = full; b.version = 2; assert(api->install(&b) == PGR_MP_ERR_BAD_VERSION); }
  { PgrMapPoiConfig b = full; b.size = sizeof(b) - 1; assert(api->install(&b) == PGR_MP_ERR_BAD_SIZE); }
  { PgrMapPoiConfig b = full; b.runtime_invoke = nullptr; assert(api->install(&b) == PGR_MP_ERR_MISSING_PTR); }
  { PgrMapPoiConfig b = full; b.f_hidden = nullptr; assert(api->install(&b) == PGR_MP_ERR_MISSING_PTR); }
  { PgrMapPoiConfig b = full; b.m_team = nullptr; assert(api->install(&b) == PGR_MP_ERR_BAD_CONFIG); }      // team partial
  { PgrMapPoiConfig b = full; b.f_slots = nullptr; assert(api->install(&b) == PGR_MP_ERR_BAD_CONFIG); }     // slots partial
  { PgrMapPoiConfig b = full; b.class_boss = nullptr; assert(api->install(&b) == PGR_MP_ERR_BAD_CONFIG); }  // boss partial
  // not-installed: scan refuses with an initialised empty output.
  {
    PgrMapPoiRequest r = make_request(tid, 2000, WALL);
    assert(api->scan(&r, out) == PGR_MP_ERR_NOT_INSTALLED);
    assert(out->version == 1 && out->size == sizeof(*out) && out->count == 0 && out->status == PGR_MP_ERR_NOT_INSTALLED);
  }
  assert(api->install(&full) == 0);
  assert(api->install(&full) == 0);  // idempotent
  { PgrMapPoiConfig b = full; b.class_gym = &cls_other; assert(api->install(&b) == PGR_MP_ERR_RECONFIG); }
  // A config that disables optional groups (all-null) is a DIFFERENT config -> reconfig, never silently swapped.
  { PgrMapPoiConfig b = make_config(false, false, false, false); assert(api->install(&b) == PGR_MP_ERR_RECONFIG); }

  // 2. owner / attached-thread / lease gates: no managed work, output empty.
  reset();
  {
    PgrMapPoiRequest bad = make_request(tid + 7, 2000, WALL);
    assert(api->scan(&bad, out) == PGR_MP_SKIP && g_invokes == 0 && out->count == 0);
    PgrMapPoiRequest zero = make_request(0, 2000, WALL);
    assert(api->scan(&zero, out) == PGR_MP_SKIP && g_invokes == 0);
    PgrMapPoiRequest closed = make_request(tid, 0, WALL);
    assert(api->scan(&closed, out) == PGR_MP_SKIP && g_invokes == 0);  // latches owner, lease closed
    g_thread = nullptr;  // Unity thread not attached
    PgrMapPoiRequest r = make_request(tid, 2000, WALL);
    assert(api->scan(&r, out) == PGR_MP_SKIP && g_invokes == 0);
    g_thread = reinterpret_cast<void *>(0x7700);
    assert(api->scan(nullptr, out) == PGR_MP_ERR_NULL_ARG);
    assert(api->scan(&r, nullptr) == PGR_MP_ERR_NULL_ARG);
    assert(g_handles.empty());
  }

  // Shared fixtures for the snapshot cases.
  static FakeString id_s0, id_s1, id_g0;
  set_id(id_s0, "stopA"); set_id(id_s1, "stopB"); set_id(id_g0, "gymA");
  static FakeFort fort_s0{&cls_fort, &id_s0, 10.0, 20.0, nullptr, nullptr};
  static FakeFort fort_s1{&cls_fort, &id_s1, -11.0, -21.0, nullptr, nullptr};
  static FakeGymDisplay disp{&cls_gymDisplay, 3};
  static FakeBoss bossPkm{&cls_boss, 150};
  static FakeRaid raid{&cls_raid, 0, WALL - 1000, WALL + 1000, WALL - 500, 5, &bossPkm};  // active, hatched
  static FakeFort fort_g0{&cls_fort, &id_g0, 30.0, 40.0, &raid, &disp};
  static FakeCandidate stop0{&cls_stop, 1, &fort_s0, 0};
  static FakeCandidate stop1{&cls_stop, 1, &fort_s1, 0};
  static FakeCandidate gym0{&cls_gym, 1, &fort_g0, 2};

  // 3. basic stop + gym snapshot; gym enrichment (team / slots / active hatched raid with boss + level).
  reset();
  {
    static FakeArray sa{&cls_objarray, 0, 0, 2, {&stop0, &stop1}};
    static FakeArray ga{&cls_objarray, 0, 0, 1, {&gym0}};
    g_stopArray = &sa; g_gymArray = &ga;
    PgrMapPoiRequest r = make_request(tid, 2000, WALL);
    assert(api->scan(&r, out) == PGR_MP_OK && out->count == 3 && out->status == PGR_MP_OK);
    assert(out->points[0].kind == PGR_MP_KIND_STOP && id_is(out->points[0], "stopA"));
    assert(out->points[0].lat == 10.0 && out->points[0].lon == 20.0 && out->points[0].flags == 0);
    assert(out->points[1].kind == PGR_MP_KIND_STOP && id_is(out->points[1], "stopB"));
    const PgrMapPoiPoint &g = out->points[2];
    assert(g.kind == PGR_MP_KIND_GYM && id_is(g, "gymA"));
    assert((g.flags & PGR_MP_FLAG_TEAM) && g.team_id == 2);
    assert((g.flags & PGR_MP_FLAG_SLOTS) && g.slots == 3);
    assert((g.flags & PGR_MP_FLAG_LEVEL) && g.level == 5);
    assert((g.flags & PGR_MP_FLAG_BOSS) && g.boss == 150);
    assert(!(g.flags & PGR_MP_FLAG_EGG));
    assert(g.raid_end == static_cast<double>(WALL + 1000) && g.raid_start == static_cast<double>(WALL - 500));
    assert(g_handles.empty());
  }

  // 4. dedup across stop + gym: a gym fort re-using stopA's id is dropped.
  reset();
  {
    static FakeString dupId; set_id(dupId, "stopA");
    static FakeFort dupFort{&cls_fort, &dupId, 1.0, 2.0, nullptr, nullptr};
    static FakeCandidate dupGym{&cls_gym, 1, &dupFort, 0};
    static FakeArray sa{&cls_objarray, 0, 0, 1, {&stop0}};
    static FakeArray ga{&cls_objarray, 0, 0, 1, {&dupGym}};
    g_stopArray = &sa; g_gymArray = &ga;
    PgrMapPoiRequest r = make_request(tid, 2000, WALL);
    assert(api->scan(&r, out) == PGR_MP_OK && out->count == 1 && id_is(out->points[0], "stopA"));
    assert(g_handles.empty());
  }

  // 5. skips: null element / wrong-class element / dead object / non-fort / null-id / bad id-length /
  //    invalid (non-finite / out-of-range) coords. None faults; the valid stop still lands.
  reset();
  {
    static FakeCandidate wrong{&cls_other, 1, &fort_s0, 0};
    static FakeCandidate dead{&cls_stop, 0, &fort_s0, 0};
    static FakeFort nonFort{&cls_other, &id_s0, 1, 2, nullptr, nullptr};
    static FakeCandidate nf{&cls_stop, 1, &nonFort, 0};
    static FakeFort nullId{&cls_fort, nullptr, 1, 2, nullptr, nullptr};
    static FakeCandidate ni{&cls_stop, 1, &nullId, 0};
    static FakeString empty; empty.klass = &cls_string; empty.length = 0;
    static FakeFort emptyIdFort{&cls_fort, &empty, 1, 2, nullptr, nullptr};
    static FakeCandidate ei{&cls_stop, 1, &emptyIdFort, 0};
    static FakeString badCoordId; set_id(badCoordId, "bad");
    static FakeFort badCoord{&cls_fort, &badCoordId, 99.0, 0.0, nullptr, nullptr};  // |lat|>90
    static FakeCandidate bc{&cls_stop, 1, &badCoord, 0};
    static FakeString nanId; set_id(nanId, "nan");
    static FakeFort nanFort{&cls_fort, &nanId, std::nan(""), 0.0, nullptr, nullptr};
    static FakeCandidate nc{&cls_stop, 1, &nanFort, 0};
    static FakeArray sa{&cls_objarray, 0, 0, 9,
                        {nullptr, &wrong, &dead, &nf, &ni, &ei, &bc, &nc, &stop0}};
    g_stopArray = &sa;
    PgrMapPoiRequest r = make_request(tid, 2000, WALL);
    assert(api->scan(&r, out) == PGR_MP_OK && out->count == 1 && id_is(out->points[0], "stopA"));
    assert(g_handles.empty());
  }

  // 6. per-class array > 2048 -> BAD_COUNT fault, empty output.
  reset();
  {
    static FakeArray big{&cls_objarray, 0, 0, static_cast<uint64_t>(PGR_MP_MAX_ARRAY) + 1, {}};
    g_stopArray = &big;
    PgrMapPoiRequest r = make_request(tid, 2000, WALL);
    assert(api->scan(&r, out) == PGR_MP_ERR_BAD_COUNT && out->count == 0);
    assert(g_handles.empty());
  }

  // 7. 512-point cap: a stop array of 2048 unique stops yields exactly 512 points. The element data is laid
  //    contiguously from offset 32 (klass@0,pad@8,pad@16,length@24,slots@32), matching the native indexing.
  reset();
  {
    static FakeString ids[2048];
    static FakeFort forts[2048];
    static FakeCandidate cands[2048];
    static struct { void *klass; uint64_t pad0, pad1, length; void *slots[2048]; } big;
    big.klass = &cls_objarray; big.length = 2048;
    for (int i = 0; i < 2048; ++i) {
      char buf[16]; std::snprintf(buf, sizeof(buf), "s%d", i);
      set_id(ids[i], buf);
      forts[i] = FakeFort{&cls_fort, &ids[i], 1.0, 2.0, nullptr, nullptr};
      cands[i] = FakeCandidate{&cls_stop, 1, &forts[i], 0};
      big.slots[i] = &cands[i];
    }
    g_stopArray = &big;
    PgrMapPoiRequest r = make_request(tid, 2500, WALL);
    assert(api->scan(&r, out) == PGR_MP_OK && out->count == 512);
    assert(g_handles.empty());
  }

  // 8. raid states: hidden / future spawn / expired end / unsafe timestamp -> ordinary gym (no raid fields);
  //    egg (battle > wall) -> EGG flag, raidStart set, NO boss even though boss data exists.
  auto gym_with_raid = [&](FakeRaid rd, bool &egg_out, bool &has_raid_out) {
    reset();
    static FakeString gid; set_id(gid, "gymR");
    static FakeRaid rr; rr = rd;
    static FakeGymDisplay gd{&cls_gymDisplay, 1};
    static FakeFort gf{&cls_fort, &gid, 5.0, 6.0, &rr, &gd};
    static FakeCandidate gc{&cls_gym, 1, &gf, 1};
    static FakeArray ga{&cls_objarray, 0, 0, 1, {&gc}};
    g_gymArray = &ga;
    PgrMapPoiRequest r = make_request(tid, 2000, WALL);
    assert(api->scan(&r, out) == PGR_MP_OK && out->count == 1);
    const PgrMapPoiPoint &p = out->points[0];
    has_raid_out = p.raid_end != 0.0;
    egg_out = (p.flags & PGR_MP_FLAG_EGG) != 0;
    assert(g_handles.empty());
  };
  { bool egg, hr; gym_with_raid(FakeRaid{&cls_raid, 1, WALL - 1, WALL + 1, WALL - 1, 5, &bossPkm}, egg, hr); assert(!hr); }          // hidden
  { bool egg, hr; gym_with_raid(FakeRaid{&cls_raid, 0, WALL + 100, WALL + 1000, WALL + 50, 5, &bossPkm}, egg, hr); assert(!hr); }   // future
  { bool egg, hr; gym_with_raid(FakeRaid{&cls_raid, 0, WALL - 100, WALL, WALL - 50, 5, &bossPkm}, egg, hr); assert(!hr); }          // expired (end==wall, not >)
  { bool egg, hr; gym_with_raid(FakeRaid{&cls_raid, 0, WALL - 1, static_cast<uint64_t>(1) << 60, WALL - 1, 5, &bossPkm}, egg, hr); assert(!hr); } // unsafe ts
  {
    bool egg, hr;
    gym_with_raid(FakeRaid{&cls_raid, 0, WALL - 1000, WALL + 1000, WALL + 500, 5, &bossPkm}, egg, hr);  // egg
    assert(hr && egg);
    assert(out->points[0].raid_start == static_cast<double>(WALL + 500));
    assert(!(out->points[0].flags & PGR_MP_FLAG_BOSS));  // egg not hatched -> no boss
    assert(out->points[0].flags & PGR_MP_FLAG_LEVEL);
  }

  // 9. optional isolation: team throws, gym display is the wrong class, slots out of range, boss out of range.
  //    Each only omits its field; the point and the other good fields survive.
  reset();
  {
    static FakeString gid; set_id(gid, "gymIso");
    static FakeGymDisplay badDisp{&cls_other, 3};  // wrong class -> no slots
    static FakeBoss badBoss{&cls_boss, 0};         // 0 out of range -> no boss
    static FakeRaid rr{&cls_raid, 0, WALL - 1000, WALL + 1000, WALL - 500, 7, &badBoss};
    static FakeFort gf{&cls_fort, &gid, 5.0, 6.0, &rr, &badDisp};
    static FakeCandidate gc{&cls_gym, 1, &gf, 4};
    static FakeArray ga{&cls_objarray, 0, 0, 1, {&gc}};
    g_gymArray = &ga;
    g_throw_method = &m_team;  // team invoke throws -> omit team only (NOT a fault)
    PgrMapPoiRequest r = make_request(tid, 2000, WALL);
    assert(api->scan(&r, out) == PGR_MP_OK && out->count == 1);
    const PgrMapPoiPoint &p = out->points[0];
    assert(!(p.flags & PGR_MP_FLAG_TEAM));
    assert(!(p.flags & PGR_MP_FLAG_SLOTS));
    assert(!(p.flags & PGR_MP_FLAG_BOSS));
    assert(p.flags & PGR_MP_FLAG_LEVEL && p.level == 7);  // level still read
    assert(p.raid_end == static_cast<double>(WALL + 1000));
    assert(g_handles.empty());
  }

  // 9b. enrichment absence: a gym with null raid / null gym display / null team box carries no optional flags.
  reset();
  {
    static FakeString gid; set_id(gid, "gymBare");
    static FakeFort gf{&cls_fort, &gid, 5.0, 6.0, nullptr, nullptr};
    static FakeCandidate gc{&cls_gym, 1, &gf, 9};
    static FakeArray ga{&cls_objarray, 0, 0, 1, {&gc}};
    g_gymArray = &ga;
    g_team_ret = nullptr;  // team invoke returns null box -> no team
    PgrMapPoiRequest r = make_request(tid, 2000, WALL);
    assert(api->scan(&r, out) == PGR_MP_OK && out->count == 1);
    assert(out->points[0].flags == 0 && out->points[0].raid_end == 0.0);
    assert(g_handles.empty());
  }

  // 10. GC moving heap: relocate the array during array_length; element reads use the re-resolved base.
  reset();
  {
    static FakeArray sa{&cls_objarray, 0, 0, 2, {&stop0, &stop1}};
    static FakeArray moved{};
    g_stopArray = &sa;
    g_oneshot_arrlen = [&]() {
      moved = sa;
      relocate(&sa, &moved);
      sa.klass = &cls_other;
      sa.data[0] = reinterpret_cast<void *>(0xdeadbeef);  // poison the stale slot
    };
    PgrMapPoiRequest r = make_request(tid, 2000, WALL);
    assert(api->scan(&r, out) == PGR_MP_OK && out->count == 2);
    assert(id_is(out->points[0], "stopA") && id_is(out->points[1], "stopB"));
    assert(g_handles.empty());
  }

  // 10b. GC moving heap: relocate the fort while rooting the id string; lat/lon re-resolve from the root.
  reset();
  {
    static FakeString gid; set_id(gid, "gymMove");
    static FakeRaid rr{&cls_raid, 0, WALL - 1000, WALL + 1000, WALL - 500, 2, &bossPkm};
    static FakeFort gf{&cls_fort, &gid, 12.0, 34.0, &rr, &disp};
    static FakeFort moved{};
    static FakeCandidate gc{&cls_gym, 1, &gf, 1};
    static FakeArray ga{&cls_objarray, 0, 0, 1, {&gc}};
    g_gymArray = &ga;
    // Fire when the id string is rooted -- i.e. AFTER the fort root exists. The fort handle is repointed to a
    // relocated copy and the original is poisoned; lat/lon/raid must all read through the re-resolved root.
    g_gcnew_fire_on = &gid;
    g_oneshot_gcnew = [&]() {
      moved = gf;
      relocate(&gf, &moved);
      gf.klass = &cls_other;
      gf.lat = -999.0;  // poison the stale fort
    };
    PgrMapPoiRequest r = make_request(tid, 2000, WALL);
    assert(api->scan(&r, out) == PGR_MP_OK && out->count == 1);
    assert(out->points[0].lat == 12.0 && out->points[0].lon == 34.0);
    assert(out->points[0].raid_end == static_cast<double>(WALL + 1000));
    assert(g_handles.empty());
  }

  // 11. gchandle_new failure -> fault, no partial publish, roots balanced.
  for (int n = 1; n <= 6; ++n) {
    reset();
    static FakeArray sa{&cls_objarray, 0, 0, 1, {&stop0}};
    g_stopArray = &sa;
    g_gcnew_fail = n;
    PgrMapPoiRequest r = make_request(tid, 2000, WALL);
    int rc = api->scan(&r, out);
    if (n <= g_gcnew_calls) { assert(rc < 0 && out->count == 0); }
    else { assert(rc == PGR_MP_OK); }
    g_gcnew_fail = 0;
    assert(g_handles.empty());
  }

  // 12. managed exceptions on the main path (find / alive / fort) -> MANAGED fault, empty output.
  for (void *thrower : {static_cast<void *>(&m_find), static_cast<void *>(&m_alive),
                        static_cast<void *>(&m_fort_resolved)}) {
    reset();
    static FakeArray sa{&cls_objarray, 0, 0, 1, {&stop0}};
    g_stopArray = &sa;
    g_throw_method = thrower;
    PgrMapPoiRequest r = make_request(tid, 2000, WALL);
    assert(api->scan(&r, out) == PGR_MP_ERR_MANAGED && out->count == 0 && g_handles.empty());
  }

  // 13. lease expiry discovered after the walk -> benign SKIP, empty output, no partial publish.
  reset();
  {
    static FakeArray sa{&cls_objarray, 0, 0, 1, {&stop0}};
    g_stopArray = &sa;
    g_oneshot_arrlen = []() { ::usleep(4000); };  // blow past the 1 ms lease before the final gate
    PgrMapPoiRequest r = make_request(tid, 1, WALL);
    assert(api->scan(&r, out) == PGR_MP_SKIP && out->count == 0 && g_handles.empty());
  }

  // 14. reentrancy: a nested scan fired inside alive() is refused BUSY; the outer still completes.
  reset();
  {
    static FakeArray sa{&cls_objarray, 0, 0, 1, {&stop0}};
    g_stopArray = &sa;
    int nested = 99;
    g_oneshot_on = &m_alive;
    g_oneshot = [&]() {
      PgrMapPoiOutput *inner = new PgrMapPoiOutput{};
      PgrMapPoiRequest ri = make_request(this_tid(), 2000, WALL);
      nested = g_api->scan(&ri, inner);
      delete inner;
    };
    PgrMapPoiRequest r = make_request(tid, 2000, WALL);
    assert(api->scan(&r, out) == PGR_MP_OK && nested == PGR_MP_ERR_BUSY && out->count == 1);
    assert(g_handles.empty());
  }

  // 15. bounded live roots: during a large snapshot the handle table stays small (per-record RAII scope),
  //    independent of how many points are already committed.
  reset();
  {
    static struct { void *klass; uint64_t pad0, pad1, length; void *slots[600]; } big;
    static FakeString ids[600];
    static FakeFort forts[600];
    static FakeCandidate cands[600];
    big.klass = &cls_objarray; big.length = 600;
    for (int i = 0; i < 600; ++i) {
      char buf[16]; std::snprintf(buf, sizeof(buf), "b%d", i);
      set_id(ids[i], buf);
      forts[i] = FakeFort{&cls_fort, &ids[i], 1.0, 2.0, nullptr, nullptr};
      cands[i] = FakeCandidate{&cls_stop, 1, &forts[i], 0};
      big.slots[i] = &cands[i];
    }
    g_stopArray = &big;
    PgrMapPoiRequest r = make_request(tid, 2500, WALL);
    assert(api->scan(&r, out) == PGR_MP_OK && out->count == 512);
    assert(g_max_live <= 8 && "live roots must be bounded per record, not grow with point count");
    assert(g_handles.empty());
  }

  // 16. owner latch: a foreign OS thread (its own nonzero tid) is refused once main has latched.
  reset();
  {
    int worker_rc = 123;
    std::thread worker([&]() {
      PgrMapPoiOutput *wout = new PgrMapPoiOutput{};
      PgrMapPoiRequest wr = make_request(this_tid(), 2000, WALL);
      worker_rc = g_api->scan(&wr, wout);
      delete wout;
    });
    worker.join();
    assert(worker_rc == PGR_MP_SKIP && g_handles.empty());
  }

  delete out;
  puts("PASS map poi: ABI (config 328 / request 32 / point 576 / output 294928 / table 24) + offsets, "
       "install validation/mandatory/partial-group reject/idempotence/reconfig, owner+attached+lease gates, "
       "stop+gym snapshot with team/slots/level/boss enrichment, dedup, skips (null/wrong-class/dead/non-fort/"
       "null-id/bad-length/invalid-coords), 2048 per-class cap fault, 512 total cap, raid hidden/future/expired/"
       "unsafe-ts/egg(no-boss), optional isolation (throw/bad-class/out-of-range) + absence, GC array+fort "
       "relocation re-resolve, gchandle failures (no partial), main-path throws fault, lease expiry SKIP, "
       "reentrancy BUSY, bounded live roots, foreign-thread refuse, all roots balance");
  dlclose(lib);
  return 0;
}

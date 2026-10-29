#include "vgp_actions.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <thread>
#include <unistd.h>

// Real-API fake-IL2CPP harness for feature 26 (native VGP request actions). It resolves the table
// through the exported pgrazor_native_api v2 registry (query_feature(26,1)) and drives the three actions
// against a fake managed heap: a wild target, a fort, an rpc service and a FortSearch request built by
// object_new. gchandle_new/get/free back a handle table so handle adoption + leak accounting are exact.
// runtime_invoke captures the PokeballThrow payload and the SendRpc params, and can throw / return null /
// re-enter. The module installs NO hook and NEVER calls back into JS.

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class / field / method tokens (distinct addresses) --------------------------
int cls_stop, cls_wild, cls_fort, cls_rpc, cls_request, cls_promise, cls_string;
int f_fort_id, f_fort_lat, f_fort_lon, f_req_id, f_req_lat, f_req_lon, f_req_tlat, f_req_tlon;
int m_encounter, m_capture, m_ctor, m_sendrpc;

// ----- fake managed objects --------------------------------------------------------
struct FakeWild { void *klass; };
struct FakeFort { void *klass; double lat; double lon; void *id; };
struct FakeRpc { void *klass; };
struct FakeString { void *klass; };
struct FakePromise { void *klass; };
struct FakeRequest { void *klass; void *id; double plat; double plon; double flat; double flon; bool ctor; };

FakeString g_fort_id_str{&cls_string};
FakePromise g_promise{&cls_promise};

// ----- gc handle table -------------------------------------------------------------
std::map<uintptr_t, void *> g_handles;
uintptr_t g_handle_seq = 0x900;
uint64_t g_gcnew_calls = 0;
uint64_t g_gcnew_fail_index = 0;  // fail the Nth gchandle_new (1-based); 0 = never
uint64_t g_gcnew_fire_index = 0;  // fire g_gcnew_oneshot on the Nth gchandle_new (1-based); 0 = never
std::function<void()> g_gcnew_oneshot;

uintptr_t fake_gc_new(void *obj, int /*pinned*/) {
  ++g_gcnew_calls;
  if (g_gcnew_oneshot && g_gcnew_calls == g_gcnew_fire_index) { auto fn = g_gcnew_oneshot; g_gcnew_oneshot = nullptr; fn(); }
  if (obj == nullptr) return 0;
  if (g_gcnew_fail_index != 0 && g_gcnew_calls == g_gcnew_fail_index) return 0;
  uintptr_t h = ++g_handle_seq;
  g_handles[h] = obj;
  return h;
}
uint64_t g_get_calls=0,g_get_fire=0;
std::function<void()> g_get_oneshot;
void *fake_gc_get(uintptr_t h) { ++g_get_calls; if(g_get_oneshot&&g_get_calls==g_get_fire){auto fn=g_get_oneshot;g_get_oneshot=nullptr;fn();} auto it = g_handles.find(h); return it == g_handles.end() ? nullptr : it->second; }
void fake_gc_free(uintptr_t h) { g_handles.erase(h); }

void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }

// ----- object_new ------------------------------------------------------------------
FakeRequest *g_last_request = nullptr;
bool g_objnew_null = false;
std::function<void()> g_objnew_oneshot;
void *fake_obj_new(void *klass) {
  if (g_objnew_oneshot) { auto fn = g_objnew_oneshot; g_objnew_oneshot = nullptr; fn(); }
  if (g_objnew_null) { g_objnew_null = false; return nullptr; }
  assert(klass == &cls_request);
  g_last_request = new FakeRequest{&cls_request, nullptr, 0, 0, 0, 0, false};
  return g_last_request;
}

// ----- field get / set -------------------------------------------------------------
void fake_field_get(void *obj, void *field, void *out) {
  if (field == &f_fort_lat) { *static_cast<double *>(out) = static_cast<FakeFort *>(obj)->lat; return; }
  if (field == &f_fort_lon) { *static_cast<double *>(out) = static_cast<FakeFort *>(obj)->lon; return; }
  if (field == &f_fort_id) { *static_cast<void **>(out) = static_cast<FakeFort *>(obj)->id; return; }
  assert(false && "unknown field_get token");
}
void fake_field_set(void *obj, void *field, void *value) {
  FakeRequest *r = static_cast<FakeRequest *>(obj);
  if (field == &f_req_lat) { r->plat = *static_cast<double *>(value); return; }
  if (field == &f_req_lon) { r->plon = *static_cast<double *>(value); return; }
  if (field == &f_req_tlat) { r->flat = *static_cast<double *>(value); return; }
  if (field == &f_req_tlon) { r->flon = *static_cast<double *>(value); return; }
  assert(false && "unknown field_set token");
}
void fake_field_set_obj(void *obj, void *field, void *value) {
  assert(field == &f_req_id);
  static_cast<FakeRequest *>(obj)->id = value;
}

// ----- runtime_invoke --------------------------------------------------------------
int g_encounter_calls = 0, g_capture_calls = 0, g_ctor_calls = 0, g_sendrpc_calls = 0;
uint8_t g_ball[12];
void *g_capture_ar = reinterpret_cast<void *>(0x1);  // sentinel; must be set to null on a capture
int32_t g_spin_rpcid = -1, g_spin_p2 = -1, g_spin_p3 = -1, g_spin_p4 = -1;
void *g_spin_request = nullptr;
bool g_throw_on_action = false;    // the game-action invoke reports a managed exception
bool g_throw_on_ctor = false;      // the request ctor reports a managed exception
bool g_null_promise = false;       // the game-action invoke returns a null promise
std::function<void()> g_action_oneshot;  // fired once inside the game-action invoke

void *fake_runtime_invoke(const void *method, void *, void **params, void **exc) {
  if (exc) *exc = nullptr;
  if (method == &m_ctor) {
    ++g_ctor_calls;
    if (g_throw_on_ctor) { g_throw_on_ctor = false; if (exc) *exc = reinterpret_cast<void *>(0x1); }
    return nullptr;
  }
  if (method == &m_encounter || method == &m_capture || method == &m_sendrpc) {
    if (method == &m_encounter) ++g_encounter_calls;
    else if (method == &m_capture) {
      ++g_capture_calls;
      std::memcpy(g_ball, params[0], 12);
      g_capture_ar = params[1];
    } else {
      ++g_sendrpc_calls;
      g_spin_rpcid = *static_cast<int32_t *>(params[0]);
      g_spin_request = params[1];
      g_spin_p2 = *static_cast<int32_t *>(params[2]);
      g_spin_p3 = *static_cast<int32_t *>(params[3]);
      g_spin_p4 = *static_cast<int32_t *>(params[4]);
    }
    if (g_action_oneshot) { auto fn = g_action_oneshot; g_action_oneshot = nullptr; fn(); }
    if (g_throw_on_action) { g_throw_on_action = false; if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
    if (g_null_promise) { g_null_promise = false; return nullptr; }
    return &g_promise;
  }
  assert(false && "unknown method token");
  return nullptr;
}

PgrVgpActionsConfig make_config() {
  PgrVgpActionsConfig c{};
  c.version = 1;
  c.size = sizeof(c);
  c.gchandle_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gchandle_get_target = reinterpret_cast<void *>(&fake_gc_get);
  c.gchandle_free = reinterpret_cast<void *>(&fake_gc_free);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.object_new = reinterpret_cast<void *>(&fake_obj_new);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.field_set_value = reinterpret_cast<void *>(&fake_field_set);
  c.field_set_value_obj = reinterpret_cast<void *>(&fake_field_set_obj);
  c.m_encounter = &m_encounter;
  c.m_capture = &m_capture;
  c.m_ctor = &m_ctor;
  c.class_wild = &cls_wild;
  c.class_stop = &cls_stop;
  c.class_fort = &cls_fort;
  c.class_rpc = &cls_rpc;
  c.class_request = &cls_request;
  c.f_fort_id = &f_fort_id;
  c.f_fort_lat = &f_fort_lat;
  c.f_fort_lon = &f_fort_lon;
  c.f_req_id = &f_req_id;
  c.f_req_lat = &f_req_lat;
  c.f_req_lon = &f_req_lon;
  c.f_req_target_lat = &f_req_tlat;
  c.f_req_target_lon = &f_req_tlon;
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

const PgrVgpActionsApi *g_api = nullptr;
uint64_t g_owner = 0;

PgrVgpActionsSample take_sample() {
  PgrVgpActionsSample s{};
  assert(g_api->sample(&s) == PGR_VA_OK);
  assert(s.version == 1 && s.size == sizeof(PgrVgpActionsSample));
  return s;
}

PgrVgpActionsTarget make_target(void *obj, uint64_t epoch, uint64_t tid) {
  PgrVgpActionsTarget r{};
  r.version = 1; r.size = sizeof(r); r.epoch = epoch; r.tid = tid; r.target = obj;
  return r;
}
PgrVgpActionsSpin make_spin(void *target, void *fort, void *rpc, uint64_t epoch, uint64_t tid,
                            double lat, double lon) {
  PgrVgpActionsSpin r{};
  r.version = 1; r.size = sizeof(r); r.epoch = epoch; r.tid = tid;
  r.rpc_id = 42; r.lat = lat; r.lon = lon;
  r.target = target; r.fort = fort; r.rpc = rpc; r.closed_method = &m_sendrpc;
  return r;
}

void reset_action_counters() {
  g_encounter_calls = g_capture_calls = g_ctor_calls = g_sendrpc_calls = 0;
  g_throw_on_action = g_throw_on_ctor = g_null_promise = false;
  g_objnew_null = false; g_gcnew_fail_index = 0; g_gcnew_fire_index = 0; g_gcnew_calls = 0;
  g_action_oneshot = nullptr; g_objnew_oneshot = nullptr; g_gcnew_oneshot = nullptr;
}

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) { std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror()); return 1; }
  auto getter = reinterpret_cast<GetApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == 48);
  assert(core->query_feature(26, 99) == nullptr);
  const auto *api = static_cast<const PgrVgpActionsApi *>(core->query_feature(26, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrVgpActionsApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu target=%zu spin=%zu sample=%zu table=%zu\n",
              sizeof(PgrVgpActionsConfig), sizeof(PgrVgpActionsTarget), sizeof(PgrVgpActionsSpin),
              sizeof(PgrVgpActionsSample), sizeof(PgrVgpActionsApi));
  assert(sizeof(PgrVgpActionsConfig) == 208 && sizeof(PgrVgpActionsTarget) == 32 &&
         sizeof(PgrVgpActionsSpin) == 80 && sizeof(PgrVgpActionsSample) == 72 &&
         sizeof(PgrVgpActionsApi) == 64);

  const uint64_t tid = this_tid();
  g_owner = tid;
  PgrVgpActionsConfig cfg = make_config();

  FakeWild wild{&cls_wild};
  FakeWild stop{&cls_stop};
  FakeFort fort{&cls_fort, 40.5, -73.5, &g_fort_id_str};
  FakeRpc rpc{&cls_rpc};
  uintptr_t h = 0;

  // 1. Validation + not-installed + idempotence + reconfig.
  assert(api->install(nullptr) == PGR_VA_ERR_NULL_ARG);
  PgrVgpActionsConfig bad = cfg; bad.version = 2;
  assert(api->install(&bad) == PGR_VA_ERR_BAD_VERSION);
  bad = cfg; bad.size = sizeof(cfg) - 1;
  assert(api->install(&bad) == PGR_VA_ERR_BAD_SIZE);
  bad = cfg; bad.m_capture = nullptr;
  assert(api->install(&bad) == PGR_VA_ERR_MISSING_PTR);
  { PgrVgpActionsTarget r = make_target(&wild, 1, tid);
    assert(api->encounter(&r, &h) == PGR_VA_ERR_NOT_INSTALLED); }

  assert(api->install(&cfg) == PGR_VA_OK);
  assert(api->install(&cfg) == PGR_VA_OK);  // idempotent
  bad = cfg; bad.class_wild = &cls_fort;
  assert(api->install(&bad) == PGR_VA_ERR_RECONFIG);

  // 2a. encounter: ISSUED, handle adopts the game promise, invoked once, roots clean afterwards.
  { reset_action_counters();
    assert(api->update(PGR_VA_MASK_CATCH, 2000, 1, tid) == PGR_VA_OK);
    PgrVgpActionsTarget r = make_target(&wild, 1, tid);
    h = 0;
    assert(api->encounter(&r, &h) == PGR_VA_ISSUED);
    assert(h != 0 && fake_gc_get(h) == &g_promise && g_encounter_calls == 1);
    PgrVgpActionsSample sm = take_sample();
    assert(sm.encounter_serial == 1 && sm.roots == 0 && sm.busy == 0 && sm.last_token != 0);
    fake_gc_free(h);
    assert(g_handles.empty());  // no leak once the caller frees the adopted handle
  }

  // 2b. capture: exact 12-byte PokeballThrow (id 1, 1.0f, 0) + null AR metadata; ISSUED, roots clean.
  { reset_action_counters();
    assert(api->update(PGR_VA_MASK_CATCH, 2000, 1, tid) == PGR_VA_OK);
    PgrVgpActionsTarget r = make_target(&wild, 1, tid);
    h = 0;
    std::memset(g_ball, 0xee, sizeof(g_ball)); g_capture_ar = reinterpret_cast<void *>(0x1);
    assert(api->capture(&r, &h) == PGR_VA_ISSUED);
    assert(g_capture_calls == 1 && h != 0 && fake_gc_get(h) == &g_promise);
    int32_t ball_id; float grade; int32_t remaining;
    std::memcpy(&ball_id, g_ball + 0, 4); std::memcpy(&grade, g_ball + 4, 4); std::memcpy(&remaining, g_ball + 8, 4);
    assert(ball_id == 1 && grade == 1.0f && remaining == 0 && g_capture_ar == nullptr);
    assert(take_sample().capture_serial == 1 && take_sample().roots == 0);
    fake_gc_free(h);
  }

  // 2c. spin: builds the FortSearch request (ctor + Id + coords) and invokes SendRpc with rpcId/0/10000/0.
  { reset_action_counters();
    assert(api->update(PGR_VA_MASK_SPIN, 2000, 1, tid) == PGR_VA_OK);
    PgrVgpActionsSpin r = make_spin(&stop, &fort, &rpc, 1, tid, 12.25, -45.75);
    h = 0;
    assert(api->spin(&r, &h) == PGR_VA_ISSUED);
    assert(g_ctor_calls == 1 && g_sendrpc_calls == 1 && h != 0 && fake_gc_get(h) == &g_promise);
    assert(g_spin_rpcid == 42 && g_spin_p2 == 0 && g_spin_p3 == 10000 && g_spin_p4 == 0);
    assert(g_last_request != nullptr && g_last_request->ctor == false);  // native does not set a .ctor flag
    assert(g_last_request->id == &g_fort_id_str);       // request.Id = fort.FortId
    assert(g_last_request->plat == 12.25 && g_last_request->plon == -45.75);  // player coords
    assert(g_last_request->flat == 40.5 && g_last_request->flon == -73.5);    // fort coords copied
    assert(g_spin_request == g_last_request);           // SendRpc received the built request
    assert(take_sample().spin_serial == 1 && take_sample().roots == 0);
    fake_gc_free(h);
  }

  // 3. Rejections: mask off, wrong mask bit, wrong epoch, expired lease, wrong thread. None invokes.
  { reset_action_counters();
    PgrVgpActionsTarget r = make_target(&wild, 1, tid);
    // mask off.
    assert(api->update(0, 0, 1, tid) == PGR_VA_OK);
    h = 7; assert(api->encounter(&r, &h) == PGR_VA_OK && h == 0);
    // wrong mask bit: spin needs bit2, only bit1 armed.
    assert(api->update(PGR_VA_MASK_CATCH, 2000, 1, tid) == PGR_VA_OK);
    PgrVgpActionsSpin sr = make_spin(&stop, &fort, &rpc, 1, tid, 1.0, 2.0);
    h = 7; assert(api->spin(&sr, &h) == PGR_VA_OK && h == 0);
    // encounter needs bit1, only bit2 armed.
    assert(api->update(PGR_VA_MASK_SPIN, 2000, 1, tid) == PGR_VA_OK);
    h = 7; assert(api->encounter(&r, &h) == PGR_VA_OK && h == 0);
    // wrong epoch.
    assert(api->update(PGR_VA_MASK_CATCH, 2000, 2, tid) == PGR_VA_OK);
    PgrVgpActionsTarget wrong_epoch = make_target(&wild, 1, tid);
    h = 7; assert(api->encounter(&wrong_epoch, &h) == PGR_VA_OK && h == 0);
    // expired lease.
    assert(api->update(PGR_VA_MASK_CATCH, 1, 3, tid) == PGR_VA_OK);
    ::usleep(3000);
    PgrVgpActionsTarget r3 = make_target(&wild, 3, tid);
    h = 7; assert(api->encounter(&r3, &h) == PGR_VA_OK && h == 0);
    assert(g_encounter_calls == 0 && g_sendrpc_calls == 0);
  }
  // wrong thread (owner already latched here).
  std::thread([&]() {
    PgrVgpActionsTarget r = make_target(&wild, 1, this_tid());
    uintptr_t hh = 7;
    assert(g_api->encounter(&r, &hh) == PGR_VA_OK && hh == 0);
  }).join();

  // 4. Identity: wrong target class / bad coords / non-finite fort coords / null closed method.
  { reset_action_counters();
    assert(api->update(PGR_VA_MASK_CATCH, 2000, 1, tid) == PGR_VA_OK);
    FakeFort not_wild{&cls_fort, 0, 0, &g_fort_id_str};
    PgrVgpActionsTarget wrong = make_target(&not_wild, 1, tid);
    h = 7; assert(api->encounter(&wrong, &h) == PGR_VA_ERR_IDENTITY && h == 0 && g_encounter_calls == 0);

    assert(api->update(PGR_VA_MASK_SPIN, 2000, 1, tid) == PGR_VA_OK);
    PgrVgpActionsSpin bad_coords = make_spin(&stop, &fort, &rpc, 1, tid, 200.0, 0.0);  // lat out of range
    h = 7; assert(api->spin(&bad_coords, &h) == PGR_VA_ERR_IDENTITY && h == 0 && g_sendrpc_calls == 0);

    PgrVgpActionsSpin nan_coords = make_spin(&stop, &fort, &rpc, 1, tid, std::nan(""), 0.0);
    h = 7; assert(api->spin(&nan_coords, &h) == PGR_VA_ERR_IDENTITY && h == 0);

    FakeFort nan_fort{&cls_fort, std::nan(""), 0, &g_fort_id_str};
    PgrVgpActionsSpin nf = make_spin(&stop, &nan_fort, &rpc, 1, tid, 1.0, 2.0);
    h = 7; assert(api->spin(&nf, &h) == PGR_VA_ERR_IDENTITY && h == 0 && g_ctor_calls == 0);

    PgrVgpActionsSpin no_method = make_spin(&stop, &fort, &rpc, 1, tid, 1.0, 2.0);
    no_method.closed_method = nullptr;
    h = 7; assert(api->spin(&no_method, &h) == PGR_VA_ERR_IDENTITY && h == 0);
    assert(take_sample().roots == 0);  // every identity failure freed its roots
  }

  // 5. throw / null promise / root failure: reported, action-at-most-once, roots freed, handle 0.
  { reset_action_counters();
    assert(api->update(PGR_VA_MASK_CATCH, 2000, 1, tid) == PGR_VA_OK);
    PgrVgpActionsTarget r = make_target(&wild, 1, tid);
    g_throw_on_action = true; h = 7;
    assert(api->encounter(&r, &h) == PGR_VA_ERR_MANAGED && h == 0 && g_encounter_calls == 1);
    assert(take_sample().roots == 0 && g_handles.empty());

    reset_action_counters(); assert(api->update(PGR_VA_MASK_CATCH, 2000, 1, tid) == PGR_VA_OK);
    g_null_promise = true; h = 7;
    assert(api->encounter(&r, &h) == PGR_VA_ERR_UNCERTAIN && h == 0 && g_encounter_calls == 1);
    assert(take_sample().roots == 0 && g_handles.empty());

    // Root failure for the promise (2nd gchandle_new): action issued, promise unrootable -> UNCERTAIN.
    reset_action_counters(); assert(api->update(PGR_VA_MASK_CATCH, 2000, 1, tid) == PGR_VA_OK);
    g_gcnew_calls = 0; g_gcnew_fail_index = 2; h = 7;
    assert(api->encounter(&r, &h) == PGR_VA_ERR_UNCERTAIN && h == 0 && g_encounter_calls == 1);
    assert(take_sample().roots == 0 && g_handles.empty());

    // Root failure for the target (1st gchandle_new): fail closed BEFORE the action.
    reset_action_counters(); assert(api->update(PGR_VA_MASK_CATCH, 2000, 1, tid) == PGR_VA_OK);
    g_gcnew_calls = 0; g_gcnew_fail_index = 1; h = 7;
    assert(api->encounter(&r, &h) == PGR_VA_ERR_GCHANDLE && h == 0 && g_encounter_calls == 0);
    assert(take_sample().roots == 0 && g_handles.empty());
    // Request allocation failure (object_new null): fail closed before SendRpc.
    reset_action_counters(); assert(api->update(PGR_VA_MASK_SPIN, 2000, 1, tid) == PGR_VA_OK);
    g_objnew_null = true; h = 7;
    PgrVgpActionsSpin sr = make_spin(&stop, &fort, &rpc, 1, tid, 1.0, 2.0);
    assert(api->spin(&sr, &h) == PGR_VA_ERR_GCHANDLE && h == 0 && g_sendrpc_calls == 0);
    assert(take_sample().roots == 0 && g_handles.empty());
  }

  // 6. Reentrant stop mid-construction: a stop fired from inside object_new (just before the ctor and the
  //    pre-send gate re-check) aborts the spin WITHOUT issuing SendRpc; every temporary root is freed.
  { reset_action_counters();
    assert(api->update(PGR_VA_MASK_SPIN, 2000, 1, tid) == PGR_VA_OK);
    g_objnew_oneshot = [&]() { g_api->stop(g_owner); };
    PgrVgpActionsSpin sr = make_spin(&stop, &fort, &rpc, 1, tid, 1.0, 2.0);
    h = 7;
    assert(api->spin(&sr, &h) == PGR_VA_OK && h == 0 && g_sendrpc_calls == 0);  // revoked mid-construction
    assert(take_sample().roots == 0 && g_handles.empty());

    // Reentrant stop during the target ROOT (fired from inside the first gchandle_new): the pre-invoke
    // gate re-check aborts the encounter WITHOUT invoking; the rooted target is freed.
    reset_action_counters(); assert(api->update(PGR_VA_MASK_CATCH, 2000, 1, tid) == PGR_VA_OK);
    PgrVgpActionsTarget rr = make_target(&wild, 1, tid);
    g_gcnew_fire_index = 1; g_gcnew_oneshot = [&]() { g_api->stop(g_owner); };
    h = 7;
    assert(api->encounter(&rr, &h) == PGR_VA_OK && h == 0 && g_encounter_calls == 0);  // revoked mid-root
    assert(take_sample().roots == 0 && g_handles.empty());

    // A re-arm after the abort restores operation (stop never silently re-armed on its own).
    reset_action_counters(); assert(api->update(PGR_VA_MASK_CATCH, 2000, 1, tid) == PGR_VA_OK);
    PgrVgpActionsTarget r = make_target(&wild, 1, tid);
    h = 0; assert(api->encounter(&r, &h) == PGR_VA_ISSUED && g_encounter_calls == 1);
    fake_gc_free(h);
  }

  // 7. Reentrancy during the game action: a nested action is refused (busy); the outer invokes once; and a
  //    stop fired during the invoke cannot un-issue nor cause a second invoke.
  { reset_action_counters();
    assert(api->update(PGR_VA_MASK_CATCH, 2000, 1, tid) == PGR_VA_OK);
    PgrVgpActionsTarget r = make_target(&wild, 1, tid);
    int nested_rc = -99; uintptr_t nested_h = 7;
    g_action_oneshot = [&]() {
      PgrVgpActionsTarget nr = make_target(&wild, 1, g_owner);
      nested_rc = g_api->encounter(&nr, &nested_h);  // busy -> refused
      g_api->stop(g_owner);                          // revoke from inside the invoke
    };
    h = 0;
    assert(api->encounter(&r, &h) == PGR_VA_ISSUED);  // already issued; the in-invoke stop cannot un-issue
    assert(g_encounter_calls == 1);                    // single invoke despite reentrancy
    assert(nested_rc == PGR_VA_OK && nested_h == 0);   // nested refused (busy), no 2nd invoke
    assert(h != 0 && fake_gc_get(h) == &g_promise);
    fake_gc_free(h);
    // After the in-invoke stop, the gate is revoked until re-armed.
    PgrVgpActionsTarget r2 = make_target(&wild, 1, tid);
    uintptr_t hh = 7; assert(api->encounter(&r2, &hh) == PGR_VA_OK && hh == 0);
    assert(take_sample().roots == 0 && g_handles.empty());
  }

  // 8. stop() immediate revoke; re-arm restores; stop never silently re-arms.
  { reset_action_counters();
    assert(api->update(PGR_VA_MASK_CATCH, 2000, 1, tid) == PGR_VA_OK);
    assert(api->stop(tid) == PGR_VA_OK);
    PgrVgpActionsTarget r = make_target(&wild, 1, tid);
    h = 7; assert(api->encounter(&r, &h) == PGR_VA_OK && h == 0 && g_encounter_calls == 0);
    assert(api->update(PGR_VA_MASK_CATCH, 2000, 1, tid) == PGR_VA_OK);
    h = 0; assert(api->encounter(&r, &h) == PGR_VA_ISSUED && g_encounter_calls == 1);
    fake_gc_free(h);
    assert(take_sample().roots == 0 && g_handles.empty());
  }

  // Coordinator: the last argument lookup may revoke and rearm; no dispatch after that.
  for(bool rearm : {false,true}) {
    reset_action_counters(); api->update(PGR_VA_MASK_CATCH,2000,1,tid);
    auto r=make_target(&wild,1,tid); g_get_calls=0;g_get_fire=2;
    g_get_oneshot=[&](){api->stop(tid);if(rearm)api->update(PGR_VA_MASK_CATCH,2000,1,tid);};
    h=0;assert(api->encounter(&r,&h)==0&&h==0&&g_encounter_calls==0);
    assert(g_handles.empty()&&take_sample().roots==0);
  }
  // Coordinator: actual stop class differs from wild; invalid fort data never issues RPC.
  {
    reset_action_counters();api->update(PGR_VA_MASK_SPIN,2000,1,tid);
    auto r=make_spin(&wild,&fort,&rpc,1,tid,1,2);
    assert(api->spin(&r,&h)==PGR_VA_ERR_IDENTITY&&g_sendrpc_calls==0);
    r.target=&stop;void *saved=fort.id;fort.id=nullptr;
    assert(api->spin(&r,&h)==PGR_VA_ERR_IDENTITY&&g_sendrpc_calls==0);fort.id=saved;
    double savedLat=fort.lat;
    g_objnew_oneshot=[&](){fort.lat=std::nan("");};
    assert(api->spin(&r,&h)==PGR_VA_ERR_IDENTITY&&g_sendrpc_calls==0);fort.lat=savedLat;
    assert(g_handles.empty()&&take_sample().roots==0);
  }
  puts("PASS coordinator VGP actions: distinct stop class, final-lookup revocation/rearm, null fort ID, mutated coordinates, zero roots");

  std::printf("vgp_actions_test passed: ABI/validation/idempotence/reconfig, encounter/capture/spin "
              "ISSUED (payload: red-ball 12-byte PokeballThrow + null AR, FortSearch ctor+Id+coords, "
              "SendRpc rpcId/0/10000/0, handle adoption + no leak), rejections (mask off/wrong-bit/epoch/"
              "expired/thread), identity (class/coords/fort-coords/null-method), throw/null-promise/root-"
              "failure (reported, once, roots freed), reentrant stop mid-construction (no action), busy "
              "reentrancy (single invoke), stop revoke + re-arm\n");
  return 0;
}

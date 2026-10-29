#include "camera_sampler.h"
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
#include <thread>
#include <unistd.h>

// Real-API fake-IL2CPP harness for feature 41 (native continuous camera sampling). It resolves the table
// through the exported pgrazor_native_api v2 registry (query_feature(41,1)) and drives sample() against a
// fake managed heap: a MainCameraRig receiver, the returned UnityEngine.Camera (which Camera.get_main also
// returns), a Transform subclass, a Vector3 with x/y/z fields, and boxed Int32 results for NameToLayer,
// get_cullingMask and GetInstanceID. gchandle_new/get/free back a handle table so every temporary root is
// accounted exactly (the heap must be empty between calls -- rig/camera cross as RAW pointers, never
// handles). runtime_invoke dispatches by method token and can throw. The module installs NO hook and NEVER
// calls back into JS; it only reads. NOTE: the WorldMap layer is process-cached after the first verified
// == 13, so the layer-mismatch case runs BEFORE any successful sample.

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class tokens (distinct addresses) ------------------------------------------
int cls_camera, cls_transform, cls_transform_sub, cls_vector, cls_rig, cls_int, cls_string, cls_other;
// ----- method / field tokens ------------------------------------------------------
int m_main, m_transform, m_forward, m_nametolayer, m_culling, m_instanceid;
int f_x, f_y, f_z;

// ----- fake managed objects -------------------------------------------------------
struct FakeObj { void *klass; };
struct FakeVector { void *klass; float x, y, z; };
struct FakeBox { void *klass; int32_t value; };  // object_unbox returns &value (offset 8)

FakeObj g_rig{&cls_rig};
FakeObj g_badRig{&cls_other};
FakeObj g_camera{&cls_camera};
FakeObj g_otherCamera{&cls_camera};
FakeObj g_transform{&cls_transform_sub};
FakeObj g_string{&cls_string};
FakeVector g_vector{&cls_vector, 1.0f, 0.0f, 0.0f};
FakeVector g_moved_vector{};
FakeBox g_layerBox{&cls_int, 13};
FakeBox g_maskBox{&cls_int, (1 << 13)};
FakeBox g_idBox{&cls_int, 777};

// Configurable returns / anomalies.
void *g_ret_main = &g_camera;
void *g_ret_transform = &g_transform;
void *g_ret_forward = &g_vector;
void *g_ret_string = &g_string;
void *g_thread = reinterpret_cast<void *>(0x7700);  // attached Unity thread sentinel (null to simulate detach)
void *g_throw_method = nullptr;
int g_invokes = 0;
int g_nmlayer_calls = 0;  // cumulative NameToLayer invocations (NOT reset: proves the layer cache)
void *g_cam_now = &g_camera;  // the camera a re-resolved root should currently see
std::function<void()> g_oneshot;  // fired inside the method named by g_oneshot_on
void *g_oneshot_on = nullptr;
std::function<void()> g_oneshot_field;  // fired inside field_get_value on f_x

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
bool fake_assignable(void *expected, void *actual) {
  if (expected == actual) return true;
  return expected == &cls_transform && actual == &cls_transform_sub;  // Transform <- a subclass
}
void *fake_unbox(void *obj) { return obj ? static_cast<char *>(obj) + offsetof(FakeBox, value) : nullptr; }
void *fake_thread_current() { return g_thread; }
void *fake_string_new(const char *s) { assert(s && std::strcmp(s, "WorldMap") == 0); return g_ret_string; }

void fake_field_get(void *obj, void *field, void *out) {
  size_t off = field == &f_x ? offsetof(FakeVector, x) : field == &f_y ? offsetof(FakeVector, y)
                                                                       : offsetof(FakeVector, z);
  if (field == &f_x && g_oneshot_field) { auto fn = g_oneshot_field; g_oneshot_field = nullptr; fn(); }
  std::memcpy(out, static_cast<char *>(obj) + off, sizeof(float));
}

void *fake_invoke(const void *method, void *obj, void **args, void **exc) {
  ++g_invokes;
  if (exc) *exc = nullptr;
  if (method == g_throw_method) { if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
  void *ret = nullptr;
  if (method == &m_main) { assert(obj == nullptr); ret = g_ret_main; }
  else if (method == &m_nametolayer) {
    ++g_nmlayer_calls;
    assert(obj == nullptr && args && fake_obj_class(args[0]) == &cls_string);
    ret = &g_layerBox;
  }
  // Instance getters on the camera must receive the re-resolved current camera target.
  else if (method == &m_transform) { assert(obj == g_cam_now); ret = g_ret_transform; }
  else if (method == &m_culling) { assert(obj == g_cam_now); ret = &g_maskBox; }
  else if (method == &m_instanceid) { assert(obj == g_cam_now); ret = &g_idBox; }
  else if (method == &m_forward) { assert(obj == g_ret_transform); ret = g_ret_forward; }
  else { assert(false && "unknown method token"); }
  // Fire the one-shot AFTER this call's receiver assertion so an in-flight relocation only affects LATER
  // queries (the current call legitimately saw the pre-relocation receiver).
  if (g_oneshot && method == g_oneshot_on) { auto fn = g_oneshot; g_oneshot = nullptr; fn(); }
  return ret;
}

PgrCameraSamplerConfig make_config() {
  PgrCameraSamplerConfig c{};
  c.version = PGR_CS_CONFIG_VERSION;
  c.size = sizeof(c);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_invoke);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.class_is_assignable_from = reinterpret_cast<void *>(&fake_assignable);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.object_unbox = reinterpret_cast<void *>(&fake_unbox);
  c.gchandle_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gchandle_get_target = reinterpret_cast<void *>(&fake_gc_get);
  c.gchandle_free = reinterpret_cast<void *>(&fake_gc_free);
  c.thread_current = reinterpret_cast<void *>(&fake_thread_current);
  c.string_new = reinterpret_cast<void *>(&fake_string_new);
  c.class_camera = &cls_camera;
  c.class_transform = &cls_transform;
  c.class_vector = &cls_vector;
  c.class_rig = &cls_rig;
  c.class_int = &cls_int;
  c.class_string = &cls_string;
  c.m_main = &m_main;
  c.m_transform = &m_transform;
  c.m_forward = &m_forward;
  c.m_nametolayer = &m_nametolayer;
  c.m_culling = &m_culling;
  c.m_instanceid = &m_instanceid;
  c.f_x = &f_x;
  c.f_y = &f_y;
  c.f_z = &f_z;
  c.expected_layer = PGR_CS_WORLD_MAP_LAYER;
  c.reserved0 = 0;
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

void reset() {
  assert(g_handles.empty());
  g_rig = {&cls_rig};
  g_camera = {&cls_camera};
  g_otherCamera = {&cls_camera};
  g_transform = {&cls_transform_sub};
  g_string = {&cls_string};
  g_vector = {&cls_vector, 1.0f, 0.0f, 0.0f};
  g_layerBox = {&cls_int, 13};
  g_maskBox = {&cls_int, (1 << 13)};
  g_idBox = {&cls_int, 777};
  g_ret_main = &g_camera;
  g_ret_transform = &g_transform;
  g_ret_forward = &g_vector;
  g_ret_string = &g_string;
  g_thread = reinterpret_cast<void *>(0x7700);
  g_throw_method = nullptr;
  g_invokes = 0;
  g_gcnew_calls = 0;
  g_gcnew_fail = 0;
  g_cam_now = &g_camera;
  g_oneshot = nullptr;
  g_oneshot_on = nullptr;
  g_oneshot_field = nullptr;
}

void relocate(void *from, void *to) {
  for (auto &e : g_handles) if (e.second == from) e.second = to;
  if (from == &g_camera) g_cam_now = to;
}

const PgrCameraSamplerApi *g_api = nullptr;

PgrCameraSamplerRequest make_request(uint64_t tid, uint64_t remaining, void *rig, void *camera) {
  PgrCameraSamplerRequest r{};
  r.version = PGR_CS_REQUEST_VERSION;
  r.size = sizeof(r);
  r.tid = tid;
  r.remaining_ms = remaining;
  r.rig = rig;
  r.camera = camera;
  return r;
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
  assert(core->query_feature(41, 99) == nullptr);
  const auto *api = static_cast<const PgrCameraSamplerApi *>(core->query_feature(41, 1));
  assert(api && api->version == 1 && api->size == sizeof(PgrCameraSamplerApi));
  g_api = api;

  // 0. ABI layout: fixed config/request/output/table sizes and field offsets.
  std::printf("ABI sizes: config=%zu request=%zu output=%zu table=%zu\n",
              sizeof(PgrCameraSamplerConfig), sizeof(PgrCameraSamplerRequest),
              sizeof(PgrCameraSamplerOutput), sizeof(PgrCameraSamplerApi));
  assert(sizeof(PgrCameraSamplerConfig) == 216 && sizeof(PgrCameraSamplerRequest) == 40 &&
         sizeof(PgrCameraSamplerOutput) == 72 && sizeof(PgrCameraSamplerApi) == 24);
  assert(offsetof(PgrCameraSamplerConfig, expected_layer) == 208 && offsetof(PgrCameraSamplerConfig, reserved0) == 212);
  assert(offsetof(PgrCameraSamplerRequest, rig) == 24 && offsetof(PgrCameraSamplerRequest, camera) == 32);
  assert(offsetof(PgrCameraSamplerOutput, status) == 8 && offsetof(PgrCameraSamplerOutput, reason) == 12 &&
         offsetof(PgrCameraSamplerOutput, camera_id) == 16 && offsetof(PgrCameraSamplerOutput, mask) == 20 &&
         offsetof(PgrCameraSamplerOutput, layer) == 24 && offsetof(PgrCameraSamplerOutput, x) == 32 &&
         offsetof(PgrCameraSamplerOutput, time_ms) == 64);
  assert(PGR_CS_CONFIG_VERSION == 1 && PGR_CS_WORLD_MAP_LAYER == 13 && PGR_CS_MAX_LEASE_MS == 2500);

  const uint64_t tid = this_tid();
  PgrCameraSamplerConfig cfg = make_config();
  PgrCameraSamplerOutput out{};

  // 1. install validation + not-installed + idempotence + reconfig.
  assert(api->install(nullptr) == PGR_CS_ERR_NULL_ARG);
  { PgrCameraSamplerConfig b = cfg; b.version = 2; assert(api->install(&b) == PGR_CS_ERR_BAD_VERSION); }
  { PgrCameraSamplerConfig b = cfg; b.size = sizeof(cfg) - 1; assert(api->install(&b) == PGR_CS_ERR_BAD_SIZE); }
  { PgrCameraSamplerConfig b = cfg; b.runtime_invoke = nullptr; assert(api->install(&b) == PGR_CS_ERR_MISSING_PTR); }
  { PgrCameraSamplerConfig b = cfg; b.f_z = nullptr; assert(api->install(&b) == PGR_CS_ERR_MISSING_PTR); }
  { PgrCameraSamplerConfig b = cfg; b.class_rig = nullptr; assert(api->install(&b) == PGR_CS_ERR_MISSING_PTR); }
  { PgrCameraSamplerConfig b = cfg; b.expected_layer = 12; assert(api->install(&b) == PGR_CS_ERR_BAD_CONFIG); }
  { PgrCameraSamplerConfig b = cfg; b.reserved0 = 1; assert(api->install(&b) == PGR_CS_ERR_BAD_CONFIG); }
  {
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_ERR_NOT_INSTALLED);
    assert(out.version == 1 && out.size == sizeof(out) && out.status == PGR_CS_ERR_NOT_INSTALLED && out.x == 0);
  }
  assert(api->install(&cfg) == 0);
  assert(api->install(&cfg) == 0);  // idempotent
  { PgrCameraSamplerConfig b = cfg; b.class_rig = &cls_other; assert(api->install(&b) == PGR_CS_ERR_RECONFIG); }
  assert(api->sample(nullptr, &out) == PGR_CS_ERR_NULL_ARG);
  assert(api->sample(reinterpret_cast<const PgrCameraSamplerRequest *>(&cfg), nullptr) == PGR_CS_ERR_NULL_ARG);

  // 2. owner / lease gate: no managed work; output carries only status/reason (+ time_ms once the clock runs).
  reset();
  {
    PgrCameraSamplerRequest foreign = make_request(tid + 7, 2000, &g_rig, &g_camera);
    assert(api->sample(&foreign, &out) == PGR_CS_UNAVAILABLE && out.reason == 4 && g_invokes == 0);
    PgrCameraSamplerRequest zero = make_request(0, 2000, &g_rig, &g_camera);
    assert(api->sample(&zero, &out) == PGR_CS_UNAVAILABLE && out.reason == 4 && g_invokes == 0);
    g_thread = nullptr;  // attached-thread gate
    PgrCameraSamplerRequest detached = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&detached, &out) == PGR_CS_UNAVAILABLE && out.reason == 4 && g_invokes == 0);
    g_thread = reinterpret_cast<void *>(0x7700);
    PgrCameraSamplerRequest off = make_request(tid, 0, &g_rig, &g_camera);  // latches owner, lease closed
    assert(api->sample(&off, &out) == PGR_CS_UNAVAILABLE && out.reason == 2 && g_invokes == 0);
    assert(g_handles.empty());
  }

  // 3. non-map-rig: the receiver is not the exact MainCameraRig -> benign 0, no allocation, no invoke.
  reset();
  {
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_badRig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_UNAVAILABLE && out.reason == 5 && g_invokes == 0 && g_gcnew_calls == 0);
  }
  // null returned camera -> benign 0, no invoke.
  reset();
  {
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, nullptr);
    assert(api->sample(&r, &out) == PGR_CS_UNAVAILABLE && out.reason == 6 && g_invokes == 0);
  }
  // camera mismatch: get_main returns a different camera -> benign 0 (before the layer lookup, so no cache).
  reset();
  {
    g_ret_main = &g_otherCamera;
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_UNAVAILABLE && out.reason == 7 && g_nmlayer_calls == 0 && g_handles.empty());
  }

  // 4. LAYER MISMATCH (must precede any successful sample so nothing caches). NameToLayer != 13 -> fault.
  reset();
  {
    g_layerBox.value = 14;
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_ERR_LAYER && out.reason == 14 && g_handles.empty());
    assert(out.status == PGR_CS_ERR_LAYER && out.x == 0 && out.time_ms == 0);  // no garbage on fault
  }

  // 5. first successful sample: full reading, every field, caches the WorldMap layer.
  reset();
  {
    const int nml = g_nmlayer_calls;
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_SAMPLE && out.status == 1 && out.reason == 1);
    assert(out.camera_id == 777 && out.mask == (1u << 13) && out.layer == 13);
    assert(std::fabs(out.x - 1.0) < 1e-9 && out.y == 0.0 && out.z == 0.0);
    assert(std::fabs(out.yaw - 90.0) < 1e-6 && out.time_ms > 0.0);
    assert(g_nmlayer_calls == nml + 1 && g_handles.empty());  // layer computed once
  }

  // 6. layer cache: a second good sample does NOT re-invoke NameToLayer.
  reset();
  {
    const int nml = g_nmlayer_calls;
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_SAMPLE && g_nmlayer_calls == nml && g_handles.empty());
  }

  // 7. map-hidden: WorldMap bit clear in the culling mask -> 0, output carries mask + layer.
  reset();
  {
    g_maskBox.value = 0x4;  // bit 13 clear
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_UNAVAILABLE && out.reason == 9);
    assert(out.mask == 0x4u && out.layer == 13 && out.x == 0.0 && g_handles.empty());
  }
  // null transform -> 0 reason null-transform.
  reset();
  {
    g_ret_transform = nullptr;
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_UNAVAILABLE && out.reason == 8 && g_handles.empty());
  }

  // 8. degenerate forward vectors are faults (no garbage output).
  reset();
  {
    g_vector = {&cls_vector, INFINITY, 0.0f, 0.0f};
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_ERR_VECTOR && out.reason == 15 && out.x == 0.0 && g_handles.empty());
  }
  reset();
  {
    g_vector = {&cls_vector, 0.0f, 1.0f, 0.0f};  // purely vertical: hypot(x,z)==0
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_ERR_VECTOR && out.reason == 16 && g_handles.empty());
  }

  // 9. yaw quadrants: atan2(x,z) in degrees, normalised to [0,360).
  struct Quad { float x, z; double yaw; };
  for (const Quad &q : {Quad{0, 1, 0.0}, Quad{1, 0, 90.0}, Quad{0, -1, 180.0}, Quad{-1, 0, 270.0}, Quad{1, 1, 45.0}}) {
    reset();
    g_vector = {&cls_vector, q.x, 0.0f, q.z};
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_SAMPLE && std::fabs(out.yaw - q.yaw) < 1e-6);
  }

  // 10. managed throw at any getter -> ERR_MANAGED, all roots freed, no garbage.
  for (void *thrower : {&m_main, &m_culling, &m_transform, &m_forward, &m_instanceid}) {
    reset();
    g_throw_method = thrower;
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_ERR_MANAGED && out.reason == 12 && out.x == 0.0 && g_handles.empty());
  }

  // 11. gchandle_new failure at each root allocation -> negative, no partial handle; a failure beyond the
  // last allocation still samples cleanly.
  for (int n = 1; n <= 8; ++n) {
    reset();
    g_gcnew_fail = n;
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    int rc = api->sample(&r, &out);
    if (n <= g_gcnew_calls) { assert(rc == PGR_CS_ERR_GCHANDLE); assert(out.x == 0.0); }
    else { assert(rc == PGR_CS_SAMPLE); }
    assert(g_handles.empty());
  }

  // 12. relocation between the Vector3 axis reads: y/z come from the re-resolved vector root.
  reset();
  {
    g_oneshot_field = []() {
      g_moved_vector = {&cls_vector, 1.0f, 5.0f, 7.0f};
      relocate(&g_vector, &g_moved_vector);
      g_vector.y = -99.0f; g_vector.z = -99.0f;  // poison the stale slot
    };
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_SAMPLE);
    assert(std::fabs(out.y - 5.0) < 1e-9 && std::fabs(out.z - 7.0) < 1e-9);  // from the relocated vector
    assert(g_handles.empty());
  }

  // 13. relocation of the camera root between queries: the instance getters receive the moved camera.
  reset();
  {
    static FakeObj moved_cam{&cls_camera};
    g_oneshot_on = &m_culling;
    g_oneshot = []() { relocate(&g_camera, &moved_cam); };  // fires inside get_cullingMask
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_SAMPLE && out.camera_id == 777 && g_handles.empty());
  }

  // 14. lease expiry discovered at the fresh pre-publication gate -> benign 0 reason lease.
  reset();
  {
    g_oneshot_on = &m_culling;
    g_oneshot = []() { ::usleep(4000); };  // blow past the 1 ms lease before publication
    PgrCameraSamplerRequest r = make_request(tid, 1, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_UNAVAILABLE && out.reason == 3 && out.x == 0.0 && g_handles.empty());
  }

  // 15. reentrancy: a nested sample fired inside get_main is refused busy; the outer still succeeds.
  reset();
  {
    int nested = 99;
    g_oneshot_on = &m_main;
    g_oneshot = [&]() {
      PgrCameraSamplerOutput inner{};
      PgrCameraSamplerRequest ri = make_request(this_tid(), 2000, &g_rig, &g_camera);
      nested = g_api->sample(&ri, &inner);
    };
    PgrCameraSamplerRequest r = make_request(tid, 2000, &g_rig, &g_camera);
    assert(api->sample(&r, &out) == PGR_CS_SAMPLE && nested == PGR_CS_ERR_BUSY && g_handles.empty());
  }

  // 16. owner latch: a foreign OS thread (its own nonzero tid) is refused once main has latched.
  reset();
  {
    int worker_rc = 123;
    uint32_t worker_reason = 0;
    std::thread worker([&]() {
      PgrCameraSamplerOutput wout{};
      PgrCameraSamplerRequest wr = make_request(this_tid(), 2000, &g_rig, &g_camera);
      worker_rc = g_api->sample(&wr, &wout);
      worker_reason = wout.reason;
    });
    worker.join();
    assert(worker_rc == PGR_CS_UNAVAILABLE && worker_reason == 4 && g_handles.empty());
  }

  puts("PASS camera sampler: ABI (config 216 / request 40 / output 72 / table 24) + offsets, install/"
       "validation/bad-layer/idempotence/reconfig, owner+attach+lease gate, non-map-rig/null-camera/"
       "camera-mismatch, layer-mismatch fault + cache, full reading + every field, map-hidden mask/layer, "
       "null-transform, nonfinite/vertical faults, yaw quadrants, managed throws, gchandle failures (no "
       "partials), vector + camera relocation re-resolve, lease-expiry fresh-clock, reentrancy busy, owner "
       "latch (foreign thread refused), all roots balance");
  dlclose(lib);
  return 0;
}

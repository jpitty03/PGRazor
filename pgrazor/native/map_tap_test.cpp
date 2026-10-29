#include "map_tap.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

// Real-API fake-IL2CPP harness for feature 13 (map-tap). It resolves the table through
// the exported pgrazor_native_api v2 registry (query_feature(13,1)), installs against
// two real NOP-padded "methods" (so Dobby has room) with the exact by-value Vector2 +
// out Vector3 + MethodInfo ABI, and drives capture with a fake managed heap. No test
// claims are made here; the coordinator builds and runs this against libpgrazor.so.

namespace {

using GetHookApiFn = const void *(*)(uint32_t version);

// Vector2 by value: a homogeneous float aggregate, identical ABI to the native side.
struct TestFloat2 { float x, y; };
static_assert(sizeof(TestFloat2) == 8, "Vector2 by-value must be 8 bytes");

// Fake managed object headers; every object starts with a class pointer.
struct FakeMap { void *klass; };
struct FakeGround { void *klass; };
struct FakeLatLng { void *klass; double lat; double lon; };  // boxed value type

int g_cls_ground = 1, g_cls_map = 2, g_cls_latlng = 3, g_cls_other = 4;
int g_method_location = 20;

FakeLatLng g_box_latlng{&g_cls_latlng, 37.5, -122.3};

// Fake Vector3 the game's GetGroundPosition writes into the out parameter.
float g_out_source[3] = {11.0f, 22.0f, 33.0f};
uint8_t g_position_result = 1;

// GC handle bookkeeping: assert owner-only, balanced alloc/free.
bool g_fail_root = false;
uintptr_t g_next_handle = 0x1000;
std::unordered_map<uintptr_t, void *> g_live;

uintptr_t fake_gchandle_new(void *obj, int /*pinned*/) {
  if (obj == nullptr || g_fail_root) return 0;
  const uintptr_t id = g_next_handle++;
  g_live[id] = obj;
  return id;
}
void *fake_gchandle_get_target(uintptr_t h) {
  auto it = g_live.find(h);
  return it != g_live.end() ? it->second : nullptr;
}
void fake_gchandle_free(uintptr_t h) {
  auto it = g_live.find(h);
  assert(it != g_live.end() && "double or invalid gchandle_free");
  g_live.erase(it);
}
void *fake_object_get_class(void *obj) {
  return obj ? *static_cast<void **>(obj) : nullptr;
}
void *fake_object_unbox(void *obj) { return &static_cast<FakeLatLng *>(obj)->lat; }

// Hooks the capture path can drive mid-call (lease expiry / epoch change / disable).
const PgrMapTapApi *g_api = nullptr;
uint64_t g_owner = 0;
bool g_invoke_bumps_epoch = false;
bool g_invoke_disables = false;
bool g_invoke_reenables = false;
bool g_invoke_throws = false;
uint64_t g_invoke_epoch = 0;
int g_invoke_mode = 1;

void *fake_runtime_invoke(const void *method, void *obj, void **params, void **exc) {
  if (exc) *exc = nullptr;
  if (method == &g_method_location) {
    // Validate the by-value Vector3 was forwarded as a pointer to three floats.
    float *vec = static_cast<float *>(params[0]);
    assert(vec[0] == g_out_source[0] && vec[1] == g_out_source[1] && vec[2] == g_out_source[2]);
    (void)obj;
    if (g_invoke_bumps_epoch) { g_invoke_bumps_epoch = false; g_api->update(1, 2000, g_owner, ++g_invoke_epoch, g_invoke_mode); }
    if (g_invoke_disables) { g_invoke_disables = false; g_api->disable(); if(g_invoke_reenables) {g_invoke_reenables=false; g_api->update(1,2000,g_owner,12,1);} }
    if (g_invoke_throws) { g_invoke_throws = false; if (exc) *exc = &g_method_location; return nullptr; }
    return &g_box_latlng;
  }
  return nullptr;
}

#define PGR_NOP8() asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;")

// Records of what the originals received, to prove the ABI forwards verbatim.
struct TapLog { int count = 0; void *self = nullptr, *sender = nullptr, *ea = nullptr, *method = nullptr; };
struct PosLog { int count = 0; void *self = nullptr, *out = nullptr, *method = nullptr; float x = 0, y = 0; };
TapLog g_tap_log;
PosLog g_pos_log;

// When set, the OnTap original drives GetGroundPosition (as the real game does inside
// its tap handler), so the position hook observes a live OnTap scope.
int g_tap_drives_position = 0; FakeMap g_drive_map{&g_cls_map};
TestFloat2 g_drive_pos{1.5f, -2.5f};
void *g_drive_out = g_out_source;
bool g_slow_position = false;
bool g_position_reenters = false;

void (*volatile call_tap)(void *, void *, void *, void *) = nullptr;
uint8_t (*volatile call_position)(void *, TestFloat2, void *, void *) = nullptr;

__attribute__((noinline)) void target_tap(void *self, void *sender, void *ea, void *method) {
  PGR_NOP8();
  ++g_tap_log.count;
  g_tap_log.self = self; g_tap_log.sender = sender; g_tap_log.ea = ea; g_tap_log.method = method;
  if (g_tap_drives_position > 0) {
    --g_tap_drives_position;
    call_position(&g_drive_map, g_drive_pos, g_drive_out, method);
  }
}

__attribute__((noinline)) uint8_t target_position(void *self, TestFloat2 pos, void *out, void *method) {
  PGR_NOP8();
  ++g_pos_log.count;
  g_pos_log.self = self; g_pos_log.out = out; g_pos_log.method = method;
  g_pos_log.x = pos.x; g_pos_log.y = pos.y;
  // The game fills the out Vector3 before returning its bool result.
  std::memcpy(out, g_out_source, sizeof(g_out_source));
  if (g_slow_position) { struct timespec ts{0, 15 * 1000 * 1000}; nanosleep(&ts, nullptr); }
  if (g_position_reenters) { g_position_reenters = false; call_position(self, pos, out, method); }
  return g_position_result;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

PgrMapTapConfig make_config() {
  PgrMapTapConfig c{};
  c.version = 1;
  c.size = sizeof(PgrMapTapConfig);
  c.gchandle_new = reinterpret_cast<void *>(&fake_gchandle_new);
  c.gchandle_get_target = reinterpret_cast<void *>(&fake_gchandle_get_target);
  c.gchandle_free = reinterpret_cast<void *>(&fake_gchandle_free);
  c.object_get_class = reinterpret_cast<void *>(&fake_object_get_class);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c.object_unbox = reinterpret_cast<void *>(&fake_object_unbox);
  c.tap_address = reinterpret_cast<void *>(&target_tap);
  c.position_address = reinterpret_cast<void *>(&target_position);
  c.location_method = &g_method_location;
  c.ground_class = &g_cls_ground;
  c.map_class = &g_cls_map;
  c.latlng_class = &g_cls_latlng;
  return c;
}

PgrMapTapSample take_sample(const PgrMapTapApi *api) {
  PgrMapTapSample s{};
  assert(api->sample(&s) == PGR_MAP_TAP_OK);
  assert(s.version == 1 && s.size == sizeof(PgrMapTapSample));
  return s;
}

int drain_all(const PgrMapTapApi *api, PgrMapTapEvent *out, int max) {
  int n = 0;
  PgrMapTapEvent ev{};
  while (n < max && api->drain(&ev) == PGR_MAP_TAP_OK) out[n++] = ev;
  // No further events remain.
  assert(api->drain(&ev) == PGR_MAP_TAP_EMPTY);
  return n;
}

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror());
    return 1;
  }

  call_tap = &target_tap;
  call_position = &target_position;

  auto getter = reinterpret_cast<GetHookApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == 48);
  assert(core->query_feature(13, 99) == nullptr);  // exact version only
  const auto *api = static_cast<const PgrMapTapApi *>(core->query_feature(13, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrMapTapApi));
  g_api = api;

  const uint64_t tid = this_tid();
  g_owner = tid;
  PgrMapTapConfig cfg = make_config();

  // Validation before install.
  assert(api->install(nullptr) == PGR_MAP_TAP_ERR_INVALID_ARG);
  PgrMapTapConfig bad = cfg;
  bad.version = 2;
  assert(api->install(&bad) == PGR_MAP_TAP_ERR_BAD_VERSION);
  bad = cfg; bad.size = sizeof(PgrMapTapConfig) - 1;
  assert(api->install(&bad) == PGR_MAP_TAP_ERR_BAD_SIZE);
  bad = cfg; bad.map_class = nullptr;
  assert(api->install(&bad) == PGR_MAP_TAP_ERR_MISSING_PTR);
  bad = cfg; bad.position_address = bad.tap_address;
  assert(api->install(&bad) == PGR_MAP_TAP_ERR_DUPLICATE_TARGET);

  // Not installed yet.
  PgrMapTapEvent ev{};
  assert(api->update(1, 1000, tid, 1, 1) == PGR_MAP_TAP_ERR_NOT_INSTALLED);
  assert(api->drain(&ev) == PGR_MAP_TAP_ERR_NOT_INSTALLED);
  assert(api->disable() == PGR_MAP_TAP_ERR_NOT_INSTALLED);

  // Install, idempotent re-install, refused rebinds.
  assert(api->install(&cfg) == PGR_MAP_TAP_OK);
  assert(api->install(&cfg) == PGR_MAP_TAP_OK);
  bad = cfg; bad.latlng_class = &g_cls_other;
  assert(api->install(&bad) == PGR_MAP_TAP_ERR_RECONFIG_ADDRESS_MISMATCH);

  FakeGround ground{&g_cls_ground};
  FakeMap map{&g_cls_map};
  int dummy_sender = 0, dummy_ea = 0, dummy_method = 0;
  g_drive_out = g_out_source;

  // 1. Mode out of range is rejected without arming.
  assert(api->update(1, 1000, tid, 1, 9) == PGR_MAP_TAP_ERR_BAD_MODE);
  assert(api->update(1, 1000, tid, 1, -1) == PGR_MAP_TAP_ERR_BAD_MODE);

  // 2. ABI forwarding: a bare OnTap (not driving position) chains verbatim; a bare
  //    position outside any OnTap scope chains verbatim and emits nothing.
  assert(api->update(1, 2000, tid, 1, 2) == PGR_MAP_TAP_OK);
  g_tap_drives_position = 0;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  assert(g_tap_log.count == 1 && g_tap_log.self == &ground && g_tap_log.sender == &dummy_sender &&
         g_tap_log.ea == &dummy_ea && g_tap_log.method == &dummy_method);
  uint8_t r = call_position(&map, TestFloat2{4.5f, 6.5f}, g_out_source, &dummy_method);
  assert(r == 1 && g_pos_log.count == 1 && g_pos_log.self == &map && g_pos_log.out == g_out_source);
  assert(g_pos_log.x == 4.5f && g_pos_log.y == 6.5f && g_pos_log.method == &dummy_method);
  assert(take_sample(api).captured_serial == 0);  // outside OnTap: no emit
  assert(g_live.empty());

  // 3. Valid tap: OnTap drives GetGroundPosition inside its scope -> one queued event
  //    with the expected mode/epoch and the LatLng that GetWorldLocation returned.
  g_box_latlng = FakeLatLng{&g_cls_latlng, 37.5, -122.3};
  g_tap_drives_position = 1;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  assert(g_pos_log.x == g_drive_pos.x && g_pos_log.y == g_drive_pos.y);  // by-value Vector2 forwarded
  PgrMapTapSample s = take_sample(api);
  assert(s.captured_serial == 1 && s.queued_count == 1 && s.owned_root_count == 0);
  PgrMapTapEvent got[PGR_MAP_TAP_QUEUE_CAPACITY];
  int n = drain_all(api, got, PGR_MAP_TAP_QUEUE_CAPACITY);
  assert(n == 1 && got[0].mode == 2 && got[0].epoch == 1);
  assert(std::fabs(got[0].latitude - 37.5) < 1e-9 && std::fabs(got[0].longitude + 122.3) < 1e-9);
  assert(got[0].tid == tid && g_live.empty());

  // 4. Two quick taps are BOTH retained (never collapsed to one latest sample): drain
  //    returns them in order with their distinct coordinates.
  g_box_latlng.lat = 10.0; g_tap_drives_position = 1;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  usleep(50000); // timestamps must reflect captures, not the later batched drain
  g_box_latlng.lat = 20.0; g_tap_drives_position = 1;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  assert(take_sample(api).queued_count == 2);
  n = drain_all(api, got, PGR_MAP_TAP_QUEUE_CAPACITY);
  assert(n == 2 && std::fabs(got[0].latitude - 10.0) < 1e-9 && std::fabs(got[1].latitude - 20.0) < 1e-9);
  assert(got[0].boottime_ms > 0 && got[1].boottime_ms >= got[0].boottime_ms + 40);
  g_box_latlng.lat = 37.5;

  // 5. Wrong self class: position inside the scope but self is not MapView -> no emit.
  FakeGround not_map{&g_cls_other};
  g_tap_drives_position = 0;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);  // opens+closes a scope
  g_tap_drives_position = 0;
  r = call_position(&not_map, g_drive_pos, g_out_source, &dummy_method);  // outside scope anyway
  (void)r;
  assert(take_sample(api).captured_serial == 3 && g_live.empty());

  // 6. Non-finite ground position: capture dropped with a recorded error, roots balanced.
  float saved_src[3]; std::memcpy(saved_src, g_out_source, sizeof(saved_src));
  g_out_source[1] = NAN;
  g_tap_drives_position = 1;
  int dropped_before = take_sample(api).dropped_serial;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  s = take_sample(api);
  assert(s.dropped_serial == dropped_before + 1 && s.last_error_code == PGR_MAP_TAP_RUNTIME_POSITION_NONFINITE);
  assert(s.captured_serial == 3 && g_live.empty());
  std::memcpy(g_out_source, saved_src, sizeof(saved_src));

  // 7. Out-of-bounds location: GetWorldLocation returns an impossible latitude -> dropped.
  g_box_latlng.lat = 123.0;  // |lat| > 90
  g_tap_drives_position = 1;
  dropped_before = take_sample(api).dropped_serial;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  s = take_sample(api);
  assert(s.dropped_serial == dropped_before + 1 && s.last_error_code == PGR_MAP_TAP_RUNTIME_LOCATION_BOUNDS);
  g_box_latlng.lat = 37.5;

  // 8. Lease expires while the ORIGINAL runs: the re-validation revokes the event.
  assert(api->update(1, 5, tid, 1, 1) == PGR_MAP_TAP_OK);
  g_slow_position = true; g_tap_drives_position = 1;
  int captured_before = take_sample(api).captured_serial;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  assert(take_sample(api).captured_serial == captured_before && g_live.empty());
  g_slow_position = false;

  // 9. Epoch change DURING the GetWorldLocation invoke drops the event.
  assert(api->update(1, 2000, tid, 5, 1) == PGR_MAP_TAP_OK);
  g_invoke_epoch = 5; g_invoke_mode = 1; g_invoke_bumps_epoch = true;
  g_tap_drives_position = 1; captured_before = take_sample(api).captured_serial;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  assert(take_sample(api).captured_serial == captured_before && g_live.empty());

  // 10. Disable DURING the invoke drops the event too.
  assert(api->update(1, 2000, tid, 6, 1) == PGR_MAP_TAP_OK);
  g_invoke_disables = true; g_tap_drives_position = 1; captured_before = take_sample(api).captured_serial;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  assert(take_sample(api).captured_serial == captured_before && g_live.empty());

  // 11. Managed exception from GetWorldLocation is surfaced and dropped.
  assert(api->update(1, 2000, tid, 7, 1) == PGR_MAP_TAP_OK);
  g_invoke_throws = true; g_tap_drives_position = 1; dropped_before = take_sample(api).dropped_serial;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  s = take_sample(api);
  assert(s.dropped_serial == dropped_before + 1 && s.last_error_code == PGR_MAP_TAP_RUNTIME_INVOKE_EXCEPTION);
  assert(g_live.empty());

  // 12. Reentrancy: a GetGroundPosition reentered from inside the original must not
  //     double-capture, and all roots stay balanced.
  assert(api->update(1, 2000, tid, 8, 1) == PGR_MAP_TAP_OK);
  g_position_reenters = true; g_tap_drives_position = 1; captured_before = take_sample(api).captured_serial;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  s = take_sample(api);
  assert(s.captured_serial == captured_before + 1 && g_live.empty());
  (void)drain_all(api, got, PGR_MAP_TAP_QUEUE_CAPACITY);

  // 13. A foreign thread cannot acquire the owner and performs no GC: its update is
  //     refused and a position call on it chains the original only.
  assert(api->update(1, 2000, tid, 9, 1) == PGR_MAP_TAP_OK);
  std::thread([&]() {
    assert(api->update(1, 2000, this_tid(), 9, 1) == PGR_MAP_TAP_ERR_INVALID_ARG);
    (void)call_position(&map, g_drive_pos, g_out_source, &dummy_method);  // not owner: chains only
    PgrMapTapSample fs{};
    api->sample(&fs);  // pure sample must not free anything
  }).join();
  assert(g_live.empty());

  // Root allocation failure must not emit or leak.
  assert(api->update(1, 2000, tid, 10, 1) == PGR_MAP_TAP_OK);
  g_fail_root = true; g_tap_drives_position = 1;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  g_fail_root = false;
  assert(g_live.empty() && take_sample(api).queued_count == 0);
  // Wrong class INSIDE the tap scope.
  g_drive_map.klass = &g_cls_other; g_tap_drives_position = 1;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  g_drive_map.klass = &g_cls_map;
  assert(g_live.empty() && take_sample(api).queued_count == 0);

  // Disable flushes the queue; drain reports empty.
  g_tap_drives_position = 1;
  assert(api->update(1, 2000, tid, 10, 1) == PGR_MAP_TAP_OK);
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  assert(take_sample(api).queued_count == 1);
  assert(api->disable() == PGR_MAP_TAP_OK);
  assert(api->drain(&ev) == PGR_MAP_TAP_EMPTY && take_sample(api).queued_count == 0);

  assert(api->update(1, 10, tid, 11, 1) == PGR_MAP_TAP_OK);
  g_tap_drives_position = 1;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  usleep(20000);
  assert(api->drain(&ev) == PGR_MAP_TAP_EMPTY && "expired queue must not deliver movement");
  assert(api->update(1, 2000, tid, 12, 1) == PGR_MAP_TAP_OK);
  g_invoke_disables=true; g_invoke_reenables=true; g_tap_drives_position=1;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  assert(api->drain(&ev)==PGR_MAP_TAP_EMPTY);
  g_tap_drives_position=1;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  assert(api->update(1,2000,tid,12,2)==PGR_MAP_TAP_OK);
  assert(api->drain(&ev)==PGR_MAP_TAP_EMPTY);
  const uint64_t large_epoch=0x100000002ULL;
  assert(api->update(1,2000,tid,large_epoch,3)==PGR_MAP_TAP_OK);
  g_tap_drives_position=1;
  call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  assert(api->drain(&ev)==PGR_MAP_TAP_OK && ev.epoch==large_epoch && ev.mode==3);
  for(int i=0;i<PGR_MAP_TAP_QUEUE_CAPACITY+1;++i){
    g_tap_drives_position=1;
    call_tap(&ground, &dummy_sender, &dummy_ea, &dummy_method);
  }
  assert(take_sample(api).fault==1 && api->drain(&ev)==PGR_MAP_TAP_EMPTY);
  assert(g_live.empty() && take_sample(api).owned_root_count==0);

  std::printf("map_tap_test passed: ABI forward, outside-scope no emit, valid tap, two taps retained, "
              "wrong class, non-finite, bounds, lease expiry, epoch/disable mid-callback, invoke exception, "
              "reentrancy, wrong-thread no GC, queue flush\n");
  return 0;
}

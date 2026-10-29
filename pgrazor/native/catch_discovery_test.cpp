#include "catch_discovery.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <thread>
#include <unistd.h>

// Real-API fake-IL2CPP harness for feature 40 (native post-catch candidate discovery). It resolves the
// table through the exported pgrazor_native_api v2 registry (query_feature(40,1)) and drives top()/scene()
// against a fake managed heap: a GuiLayerService whose TopGui() returns a DetailsGuiController (or null / a
// wrong class), and a bounded UnityEngine.Object[] of DetailsGuiController whose element data begins at
// offset 32. gchandle_new/get/free back a handle table so every temporary root and every transferred
// handle is accounted exactly (roots == the test's borrowed handle between calls, plus any adopted
// candidate). runtime_invoke dispatches by method token and can throw. The module installs NO hook and
// NEVER calls back to JS; it only enumerates and roots -- the host re-verifies identity (feature 38).

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class tokens (distinct addresses) ------------------------------------------
int cls_layer, cls_details, cls_type, cls_objectArray, cls_other;
// ----- method tokens --------------------------------------------------------------
int m_top, m_find;

// ----- fake managed objects -------------------------------------------------------
struct FakeType { void *klass; void *represents; };  // type_get_object result
struct FakeObj { void *klass; };
// LP64 Object[]: klass @0, header padding, element data @ offset 32 (8-byte slots).
struct FakeArray { void *klass; uint64_t pad0; uint64_t pad1; uint64_t length; void *data[16]; };

FakeObj g_layer{&cls_layer};
FakeObj g_badLayer{&cls_other};
FakeObj g_details{&cls_details};   // the DGUI returned by TopGui
FakeObj g_nonDetails{&cls_other};  // a non-Details TopGui result
FakeObj g_d0{&cls_details}, g_d1{&cls_details}, g_d2{&cls_details};
FakeArray g_array{&cls_objectArray, 0, 0, 3, {&g_d0, &g_d1, &g_d2}};
FakeArray g_moved_array{};

// Configurable returns / anomalies.
void *g_ret_top = &g_details;  // TopGui() result
void *g_ret_array = &g_array;  // FindObjectsOfType() result
void *g_throw_method = nullptr;
int g_invokes = 0;
std::function<void()> g_oneshot;  // fired inside the method named by g_oneshot_on
void *g_oneshot_on = nullptr;
std::function<void()> g_oneshot_arrlen;  // fired inside array_length

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

std::map<void *, FakeType *> g_types;
void *fake_class_get_type(void *klass) { return klass; }  // identity (type token == class token)
void *fake_type_get_object(void *type) {
  auto it = g_types.find(type);
  if (it != g_types.end()) return it->second;
  FakeType *t = new FakeType{&cls_type, type};
  g_types[type] = t;
  return t;
}
uintptr_t fake_array_length(void *a) {
  uintptr_t len = static_cast<uintptr_t>(static_cast<FakeArray *>(a)->length);
  if (g_oneshot_arrlen) { auto fn = g_oneshot_arrlen; g_oneshot_arrlen = nullptr; fn(); }
  return len;
}

void *fake_invoke(const void *method, void *obj, void **args, void **exc) {
  ++g_invokes;
  if (exc) *exc = nullptr;
  if (method == g_throw_method) { if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
  if (g_oneshot && method == g_oneshot_on) { auto fn = g_oneshot; g_oneshot = nullptr; fn(); }
  if (method == &m_top) { (void)obj; return g_ret_top; }
  if (method == &m_find) {
    void *rep = static_cast<FakeType *>(args[0])->represents;
    assert(rep == &cls_details && "FindObjectsOfType must receive the Details type");
    return g_ret_array;
  }
  assert(false && "unknown method token");
  return nullptr;
}

PgrCatchDiscoveryConfig make_config() {
  PgrCatchDiscoveryConfig c{};
  c.version = PGR_CD_CONFIG_VERSION;
  c.size = sizeof(c);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_invoke);
  c.gchandle_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gchandle_get_target = reinterpret_cast<void *>(&fake_gc_get);
  c.gchandle_free = reinterpret_cast<void *>(&fake_gc_free);
  c.class_get_type = reinterpret_cast<void *>(&fake_class_get_type);
  c.type_get_object = reinterpret_cast<void *>(&fake_type_get_object);
  c.array_length = reinterpret_cast<void *>(&fake_array_length);
  c.class_layer = &cls_layer;
  c.class_details = &cls_details;
  c.m_top = &m_top;
  c.m_find = &m_find;
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

void reset() {
  assert(g_handles.empty());
  g_layer = {&cls_layer};
  g_details = {&cls_details};
  g_nonDetails = {&cls_other};
  g_d0 = {&cls_details}; g_d1 = {&cls_details}; g_d2 = {&cls_details};
  g_array = {&cls_objectArray, 0, 0, 3, {&g_d0, &g_d1, &g_d2}};
  g_ret_top = &g_details;
  g_ret_array = &g_array;
  g_throw_method = nullptr;
  g_invokes = 0;
  g_gcnew_calls = 0;
  g_gcnew_fail = 0;
  g_oneshot = nullptr;
  g_oneshot_on = nullptr;
  g_oneshot_arrlen = nullptr;
}

// Repoint every live handle that targets `from` to `to` (fake relocating collection).
void relocate(void *from, void *to) {
  for (auto &e : g_handles) if (e.second == from) e.second = to;
}

const PgrCatchDiscoveryApi *g_api = nullptr;

// Mint a borrowed GC handle for the layer (the host would hold this; native duplicates it). Returns the
// handle id written into request->layer. The test frees it after the call.
uintptr_t borrow(void *obj) { return fake_gc_new(obj, 0); }

PgrCatchDiscoveryRequest make_request(uint64_t tid, uint64_t remaining, void *layer) {
  PgrCatchDiscoveryRequest r{};
  r.version = PGR_CD_REQUEST_VERSION;
  r.size = sizeof(r);
  r.tid = tid;
  r.remaining_ms = remaining;
  r.layer = layer;
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
  assert(core->query_feature(40, 99) == nullptr);
  const auto *api = static_cast<const PgrCatchDiscoveryApi *>(core->query_feature(40, 1));
  assert(api && api->version == 1 && api->size == sizeof(PgrCatchDiscoveryApi));
  g_api = api;

  // 0. ABI layout: fixed config/request/output/table sizes and field offsets.
  std::printf("ABI sizes: config=%zu request=%zu output=%zu table=%zu\n",
              sizeof(PgrCatchDiscoveryConfig), sizeof(PgrCatchDiscoveryRequest),
              sizeof(PgrCatchDiscoveryOutput), sizeof(PgrCatchDiscoveryApi));
  assert(sizeof(PgrCatchDiscoveryConfig) == 104 && sizeof(PgrCatchDiscoveryRequest) == 32 &&
         sizeof(PgrCatchDiscoveryOutput) == 144 && sizeof(PgrCatchDiscoveryApi) == 40);
  assert(offsetof(PgrCatchDiscoveryConfig, m_find) == 96);
  assert(offsetof(PgrCatchDiscoveryRequest, tid) == 8 && offsetof(PgrCatchDiscoveryRequest, remaining_ms) == 16 &&
         offsetof(PgrCatchDiscoveryRequest, layer) == 24);
  assert(offsetof(PgrCatchDiscoveryOutput, count) == 8 && offsetof(PgrCatchDiscoveryOutput, handles) == 16);
  assert(PGR_CD_CONFIG_VERSION == 1 && PGR_CD_MAX_CANDIDATES == 16);

  const uint64_t tid = this_tid();
  PgrCatchDiscoveryConfig cfg = make_config();
  PgrCatchDiscoveryOutput out{};

  // 1. install validation + not-installed + idempotence + reconfig.
  assert(api->install(nullptr) == PGR_CD_ERR_NULL_ARG);
  { PgrCatchDiscoveryConfig b = cfg; b.version = 2; assert(api->install(&b) == PGR_CD_ERR_BAD_VERSION); }
  { PgrCatchDiscoveryConfig b = cfg; b.size = sizeof(cfg) - 1; assert(api->install(&b) == PGR_CD_ERR_BAD_SIZE); }
  { PgrCatchDiscoveryConfig b = cfg; b.m_find = nullptr; assert(api->install(&b) == PGR_CD_ERR_MISSING_PTR); }
  { PgrCatchDiscoveryConfig b = cfg; b.object_get_class = nullptr; assert(api->install(&b) == PGR_CD_ERR_MISSING_PTR); }
  { PgrCatchDiscoveryConfig b = cfg; b.class_layer = nullptr; assert(api->install(&b) == PGR_CD_ERR_MISSING_PTR); }
  {
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, reinterpret_cast<void *>(0x1));
    assert(api->top(&r, &out) == PGR_CD_ERR_NOT_INSTALLED);
    assert(out.version == 1 && out.size == sizeof(out) && out.count == 0 && out.handles[0] == nullptr);
    assert(api->scene(&r, &out) == PGR_CD_ERR_NOT_INSTALLED);
  }
  assert(api->install(&cfg) == 0);
  assert(api->install(&cfg) == 0);  // idempotent
  { PgrCatchDiscoveryConfig b = cfg; b.class_layer = &cls_details; assert(api->install(&b) == PGR_CD_ERR_RECONFIG); }

  // 2. Owner / lease gate: no managed work, output initialised, no handles. (A failed owner check never
  // latches; a closed-lease call with the real tid DOES latch the owner to this thread.)
  reset();
  {
    PgrCatchDiscoveryRequest bad = make_request(tid + 7, 2000, reinterpret_cast<void *>(0x1));
    assert(api->top(&bad, &out) == PGR_CD_SKIP && g_invokes == 0);
    PgrCatchDiscoveryRequest zero = make_request(0, 2000, reinterpret_cast<void *>(0x1));
    assert(api->top(&zero, &out) == PGR_CD_SKIP && g_invokes == 0);
    PgrCatchDiscoveryRequest closed = make_request(tid, 0, reinterpret_cast<void *>(0x1));
    assert(api->top(&closed, &out) == PGR_CD_SKIP && g_invokes == 0);        // latches owner, lease closed
    assert(api->scene(&closed, &out) == PGR_CD_SKIP && g_invokes == 0);
    assert(api->top(nullptr, &out) == PGR_CD_ERR_NULL_ARG);
    assert(api->top(&closed, nullptr) == PGR_CD_ERR_NULL_ARG);
    assert(g_handles.empty());
  }

  // 3. top: valid DGUI -> one owned handle; the borrowed layer survives, the candidate is adopted.
  reset();
  {
    uintptr_t layer = borrow(&g_layer);
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, reinterpret_cast<void *>(layer));
    assert(api->top(&r, &out) == PGR_CD_OK);
    assert(out.count == 1 && out.handles[0] != nullptr && out.version == 1 && out.size == sizeof(out));
    assert(fake_gc_get(reinterpret_cast<uintptr_t>(out.handles[0])) == &g_details);
    assert(g_handles.size() == 2);  // the borrowed layer + the one transferred candidate
    assert(api->release(&out) == PGR_CD_OK);
    assert(out.count == 0 && out.handles[0] == nullptr);
    assert(api->release(&out) == PGR_CD_OK);  // release is repeat-safe
    fake_gc_free(layer);
    assert(g_handles.empty());
  }

  // top: null TopGui and a non-Details TopGui are benign refuses (host falls through to scene).
  for (int which = 0; which < 2; ++which) {
    reset();
    g_ret_top = which == 0 ? nullptr : static_cast<void *>(&g_nonDetails);
    uintptr_t layer = borrow(&g_layer);
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, reinterpret_cast<void *>(layer));
    assert(api->top(&r, &out) == PGR_CD_SKIP && out.count == 0 && out.handles[0] == nullptr);
    fake_gc_free(layer);
    assert(g_handles.empty());
  }

  // top: a wrong-class layer is a metadata fault; TopGui is never dispatched.
  reset();
  {
    uintptr_t layer = borrow(&g_badLayer);
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, reinterpret_cast<void *>(layer));
    assert(api->top(&r, &out) == PGR_CD_ERR_METADATA && g_invokes == 0 && out.count == 0);
    fake_gc_free(layer);
    assert(g_handles.empty());
  }

  // top: null layer argument -> NULL_ARG (top requires the borrowed layer handle).
  reset();
  {
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, nullptr);
    assert(api->top(&r, &out) == PGR_CD_ERR_NULL_ARG && out.count == 0);
    assert(g_handles.empty());
  }

  // top: managed throw from TopGui -> ERR_MANAGED, roots freed, no handle.
  reset();
  {
    g_throw_method = &m_top;
    uintptr_t layer = borrow(&g_layer);
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, reinterpret_cast<void *>(layer));
    assert(api->top(&r, &out) == PGR_CD_ERR_MANAGED && out.count == 0 && out.handles[0] == nullptr);
    fake_gc_free(layer);
    assert(g_handles.empty());
  }

  // top: gchandle_new failure at the candidate root -> negative, no false OK, no leak.
  reset();
  {
    uintptr_t layer = borrow(&g_layer);
    g_gcnew_fail = g_gcnew_calls + 1;  // next gchandle_new (the layer duplicate) fails
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, reinterpret_cast<void *>(layer));
    assert(api->top(&r, &out) == PGR_CD_ERR_GCHANDLE && out.count == 0);
    g_gcnew_fail = 0;
    fake_gc_free(layer);
    assert(g_handles.empty());
  }

  // 4. scene: valid array of 3 Details -> three owned handles in order.
  reset();
  {
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, nullptr);  // scene ignores layer
    assert(api->scene(&r, &out) == PGR_CD_OK && out.count == 3);
    assert(fake_gc_get(reinterpret_cast<uintptr_t>(out.handles[0])) == &g_d0 &&
           fake_gc_get(reinterpret_cast<uintptr_t>(out.handles[1])) == &g_d1 &&
           fake_gc_get(reinterpret_cast<uintptr_t>(out.handles[2])) == &g_d2);
    assert(g_handles.size() == 3);
    assert(api->release(&out) == PGR_CD_OK && out.count == 0);
    assert(g_handles.empty());
  }

  // scene: null array -> benign SKIP; empty array -> OK count 0 (no handles).
  reset();
  {
    g_ret_array = nullptr;
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, nullptr);
    assert(api->scene(&r, &out) == PGR_CD_SKIP && out.count == 0 && g_handles.empty());
  }
  reset();
  {
    g_array.length = 0;
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, nullptr);
    assert(api->scene(&r, &out) == PGR_CD_OK && out.count == 0 && out.handles[0] == nullptr);
    assert(g_handles.empty());  // nothing transferred
  }

  // scene: exactly one, and exactly the 16-slot maximum, both succeed.
  reset();
  {
    g_array.length = 1;
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, nullptr);
    assert(api->scene(&r, &out) == PGR_CD_OK && out.count == 1);
    assert(fake_gc_get(reinterpret_cast<uintptr_t>(out.handles[0])) == &g_d0);
    assert(api->release(&out) == PGR_CD_OK);
    assert(g_handles.empty());
  }
  reset();
  {
    static FakeObj full[16];
    for (int i = 0; i < 16; ++i) { full[i].klass = &cls_details; g_array.data[i] = &full[i]; }
    g_array.length = 16;
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, nullptr);
    assert(api->scene(&r, &out) == PGR_CD_OK && out.count == 16);
    for (int i = 0; i < 16; ++i) assert(fake_gc_get(reinterpret_cast<uintptr_t>(out.handles[i])) == &full[i]);
    assert(g_handles.size() == 16);
    assert(api->release(&out) == PGR_CD_OK && g_handles.empty());
  }

  // scene: 17 elements -> rejected (never truncated).
  reset();
  {
    g_array.length = PGR_CD_MAX_CANDIDATES + 1;
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, nullptr);
    assert(api->scene(&r, &out) == PGR_CD_ERR_BAD_COUNT && out.count == 0 && g_handles.empty());
  }

  // scene: null elements are skipped (not counted); remaining Details are packed contiguously.
  reset();
  {
    g_array.data[1] = nullptr;  // middle slot null
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, nullptr);
    assert(api->scene(&r, &out) == PGR_CD_OK && out.count == 2);
    assert(fake_gc_get(reinterpret_cast<uintptr_t>(out.handles[0])) == &g_d0 &&
           fake_gc_get(reinterpret_cast<uintptr_t>(out.handles[1])) == &g_d2);
    assert(api->release(&out) == PGR_CD_OK && g_handles.empty());
  }

  // scene: a nonnull wrong-class element (from a typed query) is a metadata fault; all roots released.
  reset();
  {
    g_d1.klass = &cls_other;
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, nullptr);
    assert(api->scene(&r, &out) == PGR_CD_ERR_METADATA && out.count == 0 && g_handles.empty());
  }

  // scene: wrong array object class is irrelevant to the walk, but the element scan still proves Details;
  // a managed throw from FindObjectsOfType is a fault.
  reset();
  {
    g_throw_method = &m_find;
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, nullptr);
    assert(api->scene(&r, &out) == PGR_CD_ERR_MANAGED && out.count == 0 && g_handles.empty());
  }

  // 5. relocation inside array_length: element reads use the re-resolved array base.
  reset();
  {
    g_oneshot_arrlen = []() {
      g_moved_array = g_array;
      relocate(&g_array, &g_moved_array);
      g_array.klass = &cls_other;
      g_array.data[0] = reinterpret_cast<void *>(0xdeadbeef);  // poison the stale slot
    };
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, nullptr);
    assert(api->scene(&r, &out) == PGR_CD_OK && out.count == 3);
    assert(fake_gc_get(reinterpret_cast<uintptr_t>(out.handles[0])) == &g_d0);  // from relocated array
    assert(api->release(&out) == PGR_CD_OK && g_handles.empty());
  }

  // 6. gchandle_new failure at each scene allocation (type root then the three candidate roots) ->
  // negative, no partial handles; a failure beyond the last allocation still succeeds cleanly.
  for (int n = 1; n <= 8; ++n) {
    reset();
    g_gcnew_fail = n;
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, nullptr);
    int rc = api->scene(&r, &out);
    if (n <= g_gcnew_calls) { assert(rc < 0); assert(out.count == 0); }
    else { assert(rc == PGR_CD_OK); assert(api->release(&out) == PGR_CD_OK); }
    assert(g_handles.empty());
  }

  // 7. lease expiry discovered after the scene walk -> benign SKIP, fresh-clock gate, no handles.
  reset();
  {
    g_oneshot_arrlen = []() { ::usleep(4000); };  // blow past the 1 ms lease before publication
    PgrCatchDiscoveryRequest r = make_request(tid, 1, nullptr);
    assert(api->scene(&r, &out) == PGR_CD_SKIP && out.count == 0 && g_handles.empty());
  }

  // 8. reentrancy: a nested discovery call fired inside TopGui is refused busy; the outer still succeeds.
  reset();
  {
    int nested = 99;
    g_oneshot_on = &m_top;
    g_oneshot = [&]() {
      PgrCatchDiscoveryOutput inner{};
      PgrCatchDiscoveryRequest ri = make_request(this_tid(), 2000, nullptr);
      nested = g_api->scene(&ri, &inner);
    };
    uintptr_t layer = borrow(&g_layer);
    PgrCatchDiscoveryRequest r = make_request(tid, 2000, reinterpret_cast<void *>(layer));
    assert(api->top(&r, &out) == PGR_CD_OK && nested == PGR_CD_ERR_BUSY);
    assert(api->release(&out) == PGR_CD_OK);
    fake_gc_free(layer);
    assert(g_handles.empty());
  }

  // 9. owner latch: a different OS thread (its own nonzero tid) is refused once main has latched.
  reset();
  {
    int worker_rc = 123;
    std::thread worker([&]() {
      PgrCatchDiscoveryOutput wout{};
      PgrCatchDiscoveryRequest wr = make_request(this_tid(), 2000, nullptr);
      worker_rc = g_api->scene(&wr, &wout);
    });
    worker.join();
    assert(worker_rc == PGR_CD_SKIP && g_handles.empty());  // owner mismatch -> benign refuse
  }

  puts("PASS catch discovery: ABI (config 104 / request 32 / output 144 / table 40) + offsets, "
       "install/validation/idempotence/reconfig, owner/lease gate + owner latch (foreign thread refused), "
       "top valid/null/non-Details/wrong-layer/null-layer/throw/gchandle, scene null/0/1/16/17-bound/"
       "null-element/wrong-class/throw, array + top relocation re-resolve, gchandle failures (no partials), "
       "lease expiry fresh-clock, reentrancy busy, release idempotence, all roots balance");
  dlclose(lib);
  return 0;
}

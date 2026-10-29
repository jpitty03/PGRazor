#include "vgp_maintenance.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <unistd.h>

// Real-API fake-IL2CPP harness for feature 32 (native VGP maintenance). It resolves the table through the
// exported pgrazor_native_api v2 registry (query_feature(32,1)) and drives balls()/cooldown() against a
// fake managed heap: a service object whose item-bag reference field yields an item bag, an Int32 count
// box, a Boolean alive box, a stop object with a virtual SetCooldownTime slot, and a spin-result object
// carrying an Int32 result code plus an Int64 until field. gchandle_new/get/free back a handle table so
// every temporary root is accounted exactly (roots == 0 between calls -- neither entry point transfers a
// handle). runtime_invoke dispatches by method token and can throw / return a wrong-class box. The module
// installs NO hook and NEVER calls back into JS. No real game action is ever issued.

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class tokens (distinct addresses) ------------------------------------------
int cls_service, cls_itemBag, cls_int32, cls_bool, cls_stop, cls_spinResult, cls_other;
// ----- method / field tokens ------------------------------------------------------
int m_itemCount, m_alive, m_cooldown, m_cooldownResolved;
int f_itemBag, f_until, f_result;

// ----- fake managed objects -------------------------------------------------------
struct Int32Box { void *klass; int32_t v; };
struct BoolBox { void *klass; uint8_t v; };
struct FakeService { void *klass; void *itemBag; };
struct FakeObj { void *klass; };
struct FakeSpinResult { void *klass; int32_t result; int64_t until; };

FakeObj g_itemBag{&cls_itemBag};
FakeService g_service{&cls_service, &g_itemBag};
FakeService g_moved_service{};
FakeObj g_moved_itemBag{};
FakeObj g_stop{&cls_stop};
FakeObj g_moved_stop{};
FakeSpinResult g_spin{&cls_spinResult, 0, 0};
Int32Box g_intBox{&cls_int32, 0};
BoolBox g_boolBox{&cls_bool, 1};

// Configurable returns / anomalies.
void *g_count_box = nullptr;   // override the Int32 box returned by GetItemCount (wrong-class test)
void *g_alive_box = nullptr;   // override the Boolean box returned by op_Implicit
void *g_throw_method = nullptr;
int32_t g_balls = 5;
int g_spinSuccess = 7;         // the configured spin-success sentinel
int g_invokes = 0;
int g_setter_calls = 0;
void *g_setter_obj = nullptr;  // receiver the setter was invoked on
int64_t g_setter_until = 0;    // until argument the setter received
void *g_vmethod_obj = nullptr; // object object_get_virtual_method was resolved against
std::function<void()> g_oneshot;        // fired inside the method named by g_oneshot_on
void *g_oneshot_on = nullptr;

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
void *fake_unbox(void *obj) {
  void *klass = fake_obj_class(obj);
  if (klass == &cls_int32) return &static_cast<Int32Box *>(obj)->v;
  if (klass == &cls_bool) return &static_cast<BoolBox *>(obj)->v;
  return nullptr;
}
void fake_field_get(void *obj, void *field, void *out) {
  if (field == &f_itemBag) { *static_cast<void **>(out) = static_cast<FakeService *>(obj)->itemBag; return; }
  if (field == &f_result) { *static_cast<int32_t *>(out) = static_cast<FakeSpinResult *>(obj)->result; return; }
  if (field == &f_until) { *static_cast<int64_t *>(out) = static_cast<FakeSpinResult *>(obj)->until; return; }
  assert(false && "unknown field token");
}
void *fake_virtual(void *obj, void *base) {
  g_vmethod_obj = obj;
  return base == &m_cooldown ? static_cast<void *>(&m_cooldownResolved) : nullptr;
}

void *fake_invoke(void *method, void *obj, void **args, void **exc) {
  ++g_invokes;
  if (exc) *exc = nullptr;
  if (method == g_throw_method) { if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
  if (g_oneshot && method == g_oneshot_on) { auto fn = g_oneshot; g_oneshot = nullptr; fn(); }
  if (method == &m_itemCount) {
    assert(args && *static_cast<int32_t *>(args[0]) == 1);  // GetItemCount(item id 1)
    g_intBox.v = g_balls;
    return g_count_box ? g_count_box : &g_intBox;
  }
  if (method == &m_alive) {
    assert(obj == nullptr && args);  // static op_Implicit(stop): no receiver, stop as argument
    return g_alive_box ? g_alive_box : &g_boolBox;
  }
  if (method == &m_cooldownResolved) {
    ++g_setter_calls;
    g_setter_obj = obj;
    g_setter_until = *static_cast<int64_t *>(args[0]);
    return nullptr;  // void setter
  }
  assert(false && "unknown method token");
  return nullptr;
}

PgrVgpMaintenanceConfig make_config() {
  PgrVgpMaintenanceConfig c{};
  c.version = PGR_VM_CONFIG_VERSION;
  c.size = sizeof(c);
  c.gchandle_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gchandle_get_target = reinterpret_cast<void *>(&fake_gc_get);
  c.gchandle_free = reinterpret_cast<void *>(&fake_gc_free);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_invoke);
  c.object_unbox = reinterpret_cast<void *>(&fake_unbox);
  c.object_get_virtual_method = reinterpret_cast<void *>(&fake_virtual);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.m_item_count = &m_itemCount;
  c.m_alive = &m_alive;
  c.m_cooldown = &m_cooldown;
  c.f_item_bag = &f_itemBag;
  c.f_until = &f_until;
  c.f_result = &f_result;
  c.class_service = &cls_service;
  c.class_item_bag = &cls_itemBag;
  c.class_int32 = &cls_int32;
  c.class_bool = &cls_bool;
  c.class_stop = &cls_stop;
  c.class_spin_result = &cls_spinResult;
  c.spin_success = g_spinSuccess;
  c.reserved = 0;
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

void reset() {
  assert(g_handles.empty());
  g_itemBag = {&cls_itemBag};
  g_service = {&cls_service, &g_itemBag};
  g_stop = {&cls_stop};
  g_spin = {&cls_spinResult, g_spinSuccess, 0};
  g_intBox = {&cls_int32, 0};
  g_boolBox = {&cls_bool, 1};
  g_count_box = nullptr;
  g_alive_box = nullptr;
  g_throw_method = nullptr;
  g_balls = 5;
  g_invokes = 0;
  g_setter_calls = 0;
  g_setter_obj = nullptr;
  g_setter_until = 0;
  g_vmethod_obj = nullptr;
  g_oneshot = nullptr;
  g_oneshot_on = nullptr;
  g_gcnew_calls = 0;
  g_gcnew_fail = 0;
}

// Repoint every live handle that targets `from` to `to` (fake relocating collection).
void relocate(void *from, void *to) {
  for (auto &e : g_handles) if (e.second == from) e.second = to;
}

const PgrVgpMaintenanceApi *g_api = nullptr;

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) { std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror()); return 1; }
  auto getter = reinterpret_cast<GetApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == 48);
  assert(core->query_feature(32, 99) == nullptr);
  const auto *api = static_cast<const PgrVgpMaintenanceApi *>(core->query_feature(32, 1));
  assert(api && api->version == 1 && api->size == sizeof(PgrVgpMaintenanceApi));
  g_api = api;

  // 0. ABI layout: fixed config/table sizes and the published trailing-field offsets.
  std::printf("ABI sizes: config=%zu table=%zu\n",
              sizeof(PgrVgpMaintenanceConfig), sizeof(PgrVgpMaintenanceApi));
  assert(sizeof(PgrVgpMaintenanceConfig) == 176 && sizeof(PgrVgpMaintenanceApi) == 32);
  assert(PGR_VM_CONFIG_VERSION == 1 && PGR_VM_API_VERSION == 1);
  assert(offsetof(PgrVgpMaintenanceConfig, spin_success) == 168 &&
         offsetof(PgrVgpMaintenanceConfig, reserved) == 172);

  const uint64_t tid = this_tid();
  PgrVgpMaintenanceConfig cfg = make_config();
  int32_t count = -99;

  // 1. install validation + not-installed + idempotence + reconfig.
  assert(api->install(nullptr) == PGR_VM_ERR_NULL_ARG);
  { PgrVgpMaintenanceConfig b = cfg; b.version = 2; assert(api->install(&b) == PGR_VM_ERR_BAD_VERSION); }
  { PgrVgpMaintenanceConfig b = cfg; b.size = sizeof(cfg) - 1; assert(api->install(&b) == PGR_VM_ERR_BAD_SIZE); }
  { PgrVgpMaintenanceConfig b = cfg; b.f_until = nullptr; assert(api->install(&b) == PGR_VM_ERR_MISSING_PTR); }
  { PgrVgpMaintenanceConfig b = cfg; b.gchandle_new = nullptr; assert(api->install(&b) == PGR_VM_ERR_MISSING_PTR); }
  { PgrVgpMaintenanceConfig b = cfg; b.class_spin_result = nullptr; assert(api->install(&b) == PGR_VM_ERR_MISSING_PTR); }
  assert(api->balls(&g_service, tid, 2000, &count) == PGR_VM_ERR_NOT_INSTALLED && count == 0);
  assert(api->cooldown(&g_stop, &g_spin, tid, 2000) == PGR_VM_ERR_NOT_INSTALLED);
  assert(api->install(&cfg) == 0);
  assert(api->install(&cfg) == 0);  // idempotent
  { PgrVgpMaintenanceConfig b = cfg; b.class_stop = &cls_other; assert(api->install(&b) == PGR_VM_ERR_RECONFIG); }
  { PgrVgpMaintenanceConfig b = cfg; b.spin_success = g_spinSuccess + 1; assert(api->install(&b) == PGR_VM_ERR_RECONFIG); }

  // 2. Owner / lease gate: no managed work, no handles, out_count initialised.
  reset();
  count = -99;
  assert(api->balls(&g_service, tid + 7, 2000, &count) == PGR_VM_SKIP && g_invokes == 0 && count == 0);
  assert(api->balls(&g_service, 0, 2000, &count) == PGR_VM_SKIP && g_invokes == 0);
  assert(api->balls(&g_service, tid, 0, &count) == PGR_VM_SKIP && g_invokes == 0);  // closed lease
  assert(api->balls(nullptr, tid, 2000, &count) == PGR_VM_ERR_NULL_ARG && g_invokes == 0);
  assert(api->balls(&g_service, tid, 2000, nullptr) == PGR_VM_ERR_NULL_ARG);
  assert(api->cooldown(&g_stop, &g_spin, tid + 7, 2000) == PGR_VM_SKIP && g_invokes == 0);
  assert(api->cooldown(&g_stop, &g_spin, 0, 2000) == PGR_VM_SKIP && g_invokes == 0);
  assert(api->cooldown(&g_stop, &g_spin, tid, 0) == PGR_VM_SKIP && g_invokes == 0);  // closed lease
  assert(api->cooldown(nullptr, &g_spin, tid, 2000) == PGR_VM_ERR_NULL_ARG && g_invokes == 0);
  assert(api->cooldown(&g_stop, nullptr, tid, 2000) == PGR_VM_ERR_NULL_ARG && g_invokes == 0);
  assert(g_handles.empty());

  // 3. balls success: count read, no retained handles. Zero is a valid count.
  reset();
  assert(api->balls(&g_service, tid, 2000, &count) == PGR_VM_OK && count == 5 && g_handles.empty());
  reset();
  g_balls = 0;
  assert(api->balls(&g_service, tid, 2000, &count) == PGR_VM_OK && count == 0 && g_handles.empty());

  // 4. balls: negative count fails closed; a non-Int32 box is a metadata error.
  reset();
  g_balls = -1;
  assert(api->balls(&g_service, tid, 2000, &count) == PGR_VM_ERR_NEG_COUNT && count == 0 && g_handles.empty());
  reset();
  { Int32Box wrong{&cls_other, 3}; g_count_box = &wrong;
    assert(api->balls(&g_service, tid, 2000, &count) == PGR_VM_ERR_METADATA && count == 0 && g_handles.empty()); }

  // 5. balls: wrong service / item-bag class is a metadata error; a null item-bag field is a benign skip.
  reset();
  g_service.klass = &cls_other;
  assert(api->balls(&g_service, tid, 2000, &count) == PGR_VM_ERR_METADATA && g_invokes == 0 && g_handles.empty());
  reset();
  g_itemBag.klass = &cls_other;
  assert(api->balls(&g_service, tid, 2000, &count) == PGR_VM_ERR_METADATA && g_invokes == 0 && g_handles.empty());
  reset();
  g_service.itemBag = nullptr;
  assert(api->balls(&g_service, tid, 2000, &count) == PGR_VM_SKIP && count == 0 && g_handles.empty());

  // 6. balls: a managed throw from GetItemCount -> ERR_MANAGED, roots freed, no count written.
  reset();
  g_throw_method = &m_itemCount;
  assert(api->balls(&g_service, tid, 2000, &count) == PGR_VM_ERR_MANAGED && count == 0 && g_handles.empty());

  // 7. balls: gchandle_new failure at each allocation -> negative, never a false OK, roots freed.
  for (int n = 1; n <= 3; ++n) {
    reset();
    g_gcnew_fail = n;
    int rc = api->balls(&g_service, tid, 2000, &count);
    if (n <= g_gcnew_calls) { assert(rc < 0 && count == 0); }
    assert(g_handles.empty());
  }

  // 8. balls: relocation of the item bag inside GetItemCount -> the receiver is re-resolved from its root.
  reset();
  g_oneshot_on = &m_itemCount;
  g_oneshot = []() {
    g_moved_itemBag = g_itemBag;
    relocate(&g_itemBag, &g_moved_itemBag);
    g_itemBag.klass = &cls_other;  // poison the stale storage so a stale read would fail loudly
  };
  assert(api->balls(&g_service, tid, 2000, &count) == PGR_VM_OK && count == 5 && g_handles.empty());

  // 9. balls: lease expiry discovered after the read -> benign SKIP, fresh-clock gate, no count.
  reset();
  g_oneshot_on = &m_itemCount;
  g_oneshot = []() { ::usleep(4000); };  // blow past the 1 ms lease before the final clock sample
  assert(api->balls(&g_service, tid, 1, &count) == PGR_VM_SKIP && count == 0 && g_handles.empty());

  // 10. cooldown success: alive, successful result -> setter invoked exactly once on the stop with until.
  reset();
  g_spin.until = 123456789;
  assert(api->cooldown(&g_stop, &g_spin, tid, 2000) == PGR_VM_OK);
  assert(g_setter_calls == 1 && g_setter_obj == &g_stop && g_setter_until == 123456789 && g_handles.empty());

  // Full 64 bits of until survive the field read and reach the setter unmodified.
  reset();
  g_spin.until = static_cast<int64_t>(0x7FEEDDCCBBAA9988ULL);
  assert(api->cooldown(&g_stop, &g_spin, tid, 2000) == PGR_VM_OK);
  assert(g_setter_until == static_cast<int64_t>(0x7FEEDDCCBBAA9988ULL) && g_handles.empty());

  // 11. cooldown: a non-success result applies nothing (benign skip); alive is never consulted.
  reset();
  g_spin.result = g_spinSuccess + 1;
  assert(api->cooldown(&g_stop, &g_spin, tid, 2000) == PGR_VM_SKIP && g_setter_calls == 0 && g_handles.empty());

  // 12. cooldown: a dead stop (op_Implicit false) applies nothing (benign skip).
  reset();
  g_boolBox.v = 0;
  assert(api->cooldown(&g_stop, &g_spin, tid, 2000) == PGR_VM_SKIP && g_setter_calls == 0 && g_handles.empty());

  // 13. cooldown: wrong stop / response class, and a non-Boolean alive box, are metadata errors.
  reset();
  g_stop.klass = &cls_other;
  assert(api->cooldown(&g_stop, &g_spin, tid, 2000) == PGR_VM_ERR_METADATA && g_invokes == 0 && g_handles.empty());
  reset();
  g_spin.klass = &cls_other;
  assert(api->cooldown(&g_stop, &g_spin, tid, 2000) == PGR_VM_ERR_METADATA && g_invokes == 0 && g_handles.empty());
  reset();
  { BoolBox wrong{&cls_other, 1}; g_alive_box = &wrong;
    assert(api->cooldown(&g_stop, &g_spin, tid, 2000) == PGR_VM_ERR_METADATA && g_setter_calls == 0 && g_handles.empty()); }

  // 14. cooldown: a managed throw from op_Implicit or from the setter -> ERR_MANAGED, roots freed.
  reset();
  g_throw_method = &m_alive;
  assert(api->cooldown(&g_stop, &g_spin, tid, 2000) == PGR_VM_ERR_MANAGED && g_setter_calls == 0 && g_handles.empty());
  reset();
  g_throw_method = &m_cooldownResolved;
  assert(api->cooldown(&g_stop, &g_spin, tid, 2000) == PGR_VM_ERR_MANAGED && g_handles.empty());

  // 15. cooldown: gchandle_new failure at each allocation -> negative, setter never fires, roots freed.
  for (int n = 1; n <= 3; ++n) {
    reset();
    g_gcnew_fail = n;
    int rc = api->cooldown(&g_stop, &g_spin, tid, 2000);
    if (n <= g_gcnew_calls) { assert(rc < 0 && g_setter_calls == 0); }
    assert(g_handles.empty());
  }

  // 16. cooldown: the virtual slot AND the setter receiver are the freshly re-resolved stop after a
  // relocation inside op_Implicit (no stale pointer survives the managed call).
  reset();
  g_spin.until = 42;
  g_oneshot_on = &m_alive;
  g_oneshot = []() {
    g_moved_stop = g_stop;
    relocate(&g_stop, &g_moved_stop);
    g_stop.klass = &cls_other;  // poison the stale storage
  };
  assert(api->cooldown(&g_stop, &g_spin, tid, 2000) == PGR_VM_OK);
  assert(g_vmethod_obj == &g_moved_stop && g_setter_obj == &g_moved_stop && g_setter_until == 42);
  assert(g_handles.empty());

  // 17. cooldown: lease expiry discovered BEFORE the setter -> benign SKIP, setter never fires.
  reset();
  g_oneshot_on = &m_alive;
  g_oneshot = []() { ::usleep(4000); };  // blow past the 1 ms lease before the pre-setter clock gate
  assert(api->cooldown(&g_stop, &g_spin, tid, 1) == PGR_VM_SKIP && g_setter_calls == 0 && g_handles.empty());

  // An exact Boolean box still must contain a canonical 0/1 value.
  for (uint8_t invalid : {uint8_t(2), uint8_t(255)}) {
    reset();
    BoolBox malformed{&cls_bool, invalid};
    g_alive_box = &malformed;
    assert(api->cooldown(&g_stop, &g_spin, tid, 2000) == PGR_VM_ERR_METADATA);
    assert(g_setter_calls == 0 && g_handles.empty());
  }

  // 18. cooldown: expiry DURING the setter still returns OK -- the local setter has already applied, and
  // there is no post-setter clock recheck. The pre-setter gate (lease 30 ms) clears comfortably; the
  // in-setter sleep (60 ms) overruns the lease yet must not retract success.
  reset();
  g_oneshot_on = &m_cooldownResolved;
  g_oneshot = []() { ::usleep(60000); };
  assert(api->cooldown(&g_stop, &g_spin, tid, 30) == PGR_VM_OK && g_setter_calls == 1 && g_handles.empty());

  // 19. Reentrancy: a nested maintenance call fired inside GetItemCount is refused busy; outer succeeds.
  reset();
  int nested = 99;
  g_oneshot_on = &m_itemCount;
  g_oneshot = [&]() {
    int32_t inner = 0;
    nested = g_api->balls(&g_service, this_tid(), 2000, &inner);
  };
  assert(api->balls(&g_service, tid, 2000, &count) == PGR_VM_OK && nested == PGR_VM_ERR_BUSY && g_handles.empty());

  puts("PASS VGP maintenance: ABI (config 176 / table 32) + spin_success/reserved offsets, "
       "install/validation/idempotence/reconfig, owner/lease gate (no managed work), balls count "
       "(zero valid / negative fail-closed / non-Int32 metadata), wrong service/bag class, null-bag skip, "
       "managed throw, gchandle failures (no false OK), item-bag relocation re-resolve, lease expiry "
       "fresh-clock, cooldown success + full-64-bit until, non-success/dead-target skips, wrong stop/resp "
       "class, non-Boolean alive, managed throws, gchandle failures, fresh virtual-slot + setter receiver "
       "after relocation, expiry-before-setter skip, expiry-inside-setter OK, reentrancy busy; roots zero");
  dlclose(lib);
  return 0;
}

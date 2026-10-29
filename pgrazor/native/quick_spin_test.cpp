#include "quick_spin.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

// Real-API fake-IL2CPP harness for feature 11 (quick-spin). It resolves the table
// through the exported pgrazor_native_api v2 registry (query_feature(11,1)), installs
// against four real NOP-padded "methods" (so Dobby has room), and drives the native
// request/dismiss flow with a fake managed heap. No test claims are made here; the
// coordinator builds and runs this against libpgrazor.so.

namespace {

using GetHookApiFn = const void *(*)(uint32_t version);

// Fake managed object headers; every object starts with a class pointer.
struct FakeState { void *klass; void *gui; void *gesture; };
struct FakeGui { void *klass; int32_t current_state; };
struct FakeGesture { void *klass; void *spinner; };
struct FakeSpinner { void *klass; };
struct FakeResult { void *klass; int32_t result; };
struct FakeBool { void *klass; uint8_t value; };

int g_cls_state = 1, g_cls_gui = 2, g_cls_gesture = 3, g_cls_spinner = 4;
int g_cls_result = 5, g_cls_bool = 6, g_cls_other = 7;

int g_field_gui = 10, g_field_gesture = 11, g_field_spinner = 12;
int g_field_result = 13, g_field_current_state = 14;

int g_method_spin = 20, g_method_request = 21, g_method_can = 22;
int g_method_active = 23, g_method_close = 24, g_method_alive = 25;

// Fake managed getters' return values and invoke call counters.
bool g_gui_alive = true, g_is_active = true, g_can_spin = true;
int g_spin_calls = 0, g_request_calls = 0, g_close_calls = 0;
float g_last_spin_speed = 0.0f;
const PgrQuickSpinApi *g_test_api = nullptr;
uint64_t g_test_tick = 100;
bool g_sync_result = false, g_close_reenter = false, g_request_throws = false;
bool g_slow_can = false;
void *g_fail_root = nullptr;
void *g_test_state = nullptr, *g_test_result = nullptr;
void (*g_result_callback)(void *, void *, void *) = nullptr;
void (*g_spin_callback)(void *, float, void *) = nullptr;

FakeBool g_box_true{&g_cls_bool, 1};
FakeBool g_box_false{&g_cls_bool, 0};

uintptr_t g_next_handle = 0x1000;
std::unordered_map<uintptr_t, void *> g_live;

uintptr_t fake_gchandle_new(void *obj, bool /*pinned*/) {
  if (obj == g_fail_root) return 0;
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
void fake_field_get_value(void *obj, void *field, void *out) {
  if (field == &g_field_gui) *static_cast<void **>(out) = static_cast<FakeState *>(obj)->gui;
  else if (field == &g_field_gesture) *static_cast<void **>(out) = static_cast<FakeState *>(obj)->gesture;
  else if (field == &g_field_spinner) *static_cast<void **>(out) = static_cast<FakeGesture *>(obj)->spinner;
  else if (field == &g_field_result) *static_cast<int32_t *>(out) = static_cast<FakeResult *>(obj)->result;
  else if (field == &g_field_current_state) *static_cast<int32_t *>(out) = static_cast<FakeGui *>(obj)->current_state;
}
void *fake_runtime_invoke(const void *method, void *obj, void **params, void **exc) {
  if (exc) *exc = nullptr;
  if (method == &g_method_alive) return g_gui_alive ? &g_box_true : &g_box_false;
  if (method == &g_method_active) return g_is_active ? &g_box_true : &g_box_false;
  if (method == &g_method_can) { if(g_slow_can) usleep(10000); return g_can_spin ? &g_box_true : &g_box_false; }
  if (method == &g_method_spin) {
    ++g_spin_calls;
    g_last_spin_speed = *static_cast<float *>(params[0]);
    assert(g_last_spin_speed == 360.0f);
    if (g_spin_callback) g_spin_callback(obj,g_last_spin_speed,const_cast<void *>(method));
    return nullptr;
  }
  if (method == &g_method_request) {
    ++g_request_calls;
    if (g_sync_result) g_result_callback(g_test_state,g_test_result,const_cast<void *>(method));
    if (g_request_throws) *exc = &g_method_request;
    return nullptr;
  }
  if (method == &g_method_close) {
    ++g_close_calls;
    if (g_close_reenter) {
      g_close_reenter = false;
      g_test_api->tick(gettid(),++g_test_tick,0);
    }
    return nullptr;
  }
  return nullptr;
}
void *fake_object_unbox(void *obj) { return &static_cast<FakeBool *>(obj)->value; }

#define PGR_NOP8() asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;")

struct CallLog { int count = 0; void *last_self = nullptr; };
CallLog g_enter_log, g_exit_log, g_result_log, g_spin_log;
bool g_throw_observed=false;void *g_last_result_arg=nullptr;

__attribute__((noinline)) void target_enter(void *self, void *) {
  PGR_NOP8();
  ++g_enter_log.count;
  g_enter_log.last_self = self;
  if(g_throw_observed)throw 711;
}
__attribute__((noinline)) void target_exit(void *self, void *) {
  PGR_NOP8();
  ++g_exit_log.count;
  g_exit_log.last_self = self;
  if(g_throw_observed)throw 711;
}
__attribute__((noinline)) void target_result(void *self, void *proto, void *) {
  PGR_NOP8();
  ++g_result_log.count;
  g_result_log.last_self = self;
  g_last_result_arg=proto;if(g_throw_observed)throw 711;
}
__attribute__((noinline)) void target_spin(void *self, float, void *) {
  PGR_NOP8();
  ++g_spin_log.count;
  g_spin_log.last_self = self;
}

void (*volatile call_enter)(void *, void *) = &target_enter;
void (*volatile call_exit)(void *, void *) = &target_exit;
void (*volatile call_result)(void *, void *, void *) = &target_result;
void (*volatile call_spin)(void *, float, void *) = &target_spin;

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

PgrQuickSpinConfig make_config() {
  PgrQuickSpinConfig c{};
  c.version = 1;
  c.size = sizeof(PgrQuickSpinConfig);
  c.success_value = 7;
  c.state_open_value = 2;
  c.gchandle_new = reinterpret_cast<void *>(&fake_gchandle_new);
  c.gchandle_get_target = reinterpret_cast<void *>(&fake_gchandle_get_target);
  c.gchandle_free = reinterpret_cast<void *>(&fake_gchandle_free);
  c.object_get_class = reinterpret_cast<void *>(&fake_object_get_class);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get_value);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c.object_unbox = reinterpret_cast<void *>(&fake_object_unbox);
  c.enter_address = reinterpret_cast<void *>(&target_enter);
  c.exit_address = reinterpret_cast<void *>(&target_exit);
  c.result_address = reinterpret_cast<void *>(&target_result);
  c.spin_address = reinterpret_cast<void *>(&target_spin);
  c.spin_method = &g_method_spin;
  c.request_method = &g_method_request;
  c.can_method = &g_method_can;
  c.active_method = &g_method_active;
  c.close_method = &g_method_close;
  c.alive_method = &g_method_alive;
  c.spin_state_class = &g_cls_state;
  c.spin_gui_class = &g_cls_gui;
  c.gesture_class = &g_cls_gesture;
  c.spinner_class = &g_cls_spinner;
  c.spin_result_class = &g_cls_result;
  c.bool_class = &g_cls_bool;
  c.gui_field = &g_field_gui;
  c.gesture_field = &g_field_gesture;
  c.spinner_field = &g_field_spinner;
  c.result_field = &g_field_result;
  c.current_state_field = &g_field_current_state;
  return c;
}

PgrQuickSpinSample take_sample(const PgrQuickSpinApi *api) {
  PgrQuickSpinSample s{};
  assert(api->sample(&s) == PGR_QUICK_SPIN_OK);
  assert(s.version == 1 && s.size == sizeof(PgrQuickSpinSample));
  return s;
}

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror());
    return 1;
  }

  auto getter = reinterpret_cast<GetHookApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == 48);
  assert(core->query_feature(11, 99) == nullptr);  // exact version only
  const auto *api = static_cast<const PgrQuickSpinApi *>(core->query_feature(11, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrQuickSpinApi));

  const uint64_t tid = this_tid();
  PgrQuickSpinConfig cfg = make_config();

  // Validation before install.
  assert(api->install(nullptr) == PGR_QUICK_SPIN_ERR_INVALID_ARG);
  PgrQuickSpinConfig bad = cfg;
  bad.version = 2;
  assert(api->install(&bad) == PGR_QUICK_SPIN_ERR_BAD_VERSION);
  bad = cfg;
  bad.size = sizeof(PgrQuickSpinConfig) - 1;
  assert(api->install(&bad) == PGR_QUICK_SPIN_ERR_BAD_SIZE);
  bad = cfg;
  bad.spinner_class = nullptr;
  assert(api->install(&bad) == PGR_QUICK_SPIN_ERR_MISSING_PTR);
  bad = cfg;
  bad.exit_address = bad.enter_address;
  assert(api->install(&bad) == PGR_QUICK_SPIN_ERR_DUPLICATE_TARGET);

  // Not installed yet: update/tick/clear report NOT_INSTALLED.
  assert(api->update(1, 1000, tid, 1) == PGR_QUICK_SPIN_ERR_NOT_INSTALLED);
  assert(api->tick(tid, 1, 0) == PGR_QUICK_SPIN_ERR_NOT_INSTALLED);
  assert(api->clear() == PGR_QUICK_SPIN_ERR_NOT_INSTALLED);

  // Install, idempotent re-install, and a refused address rebind.
  assert(api->install(&cfg) == PGR_QUICK_SPIN_OK);
  assert(api->install(&cfg) == PGR_QUICK_SPIN_OK);
  bad = cfg;
  bad.spin_address = bad.result_address;  // distinct from install but duplicate-checked first
  assert(api->install(&bad) == PGR_QUICK_SPIN_ERR_DUPLICATE_TARGET);
  bad = cfg;
  bad.enter_address = reinterpret_cast<void *>(&target_spin);
  bad.spin_address = reinterpret_cast<void *>(&target_enter);
  assert(api->install(&bad) == PGR_QUICK_SPIN_ERR_RECONFIG_ADDRESS_MISMATCH);

  int dummy_method = 0;
  FakeSpinner spinner{&g_cls_spinner};
  FakeGesture gesture{&g_cls_gesture, &spinner};
  FakeGui gui{&g_cls_gui, 1};  // currentState = opening
  FakeState state{&g_cls_state, &gui, &gesture};
  FakeResult result{&g_cls_result, 7};  // success value

  // 1. Disabled: enter chains original and creates no record; a valid result still
  //    bumps the cooldown counter on the verified Unity thread.
  assert(api->update(0, 0, tid, 1) == PGR_QUICK_SPIN_OK);
  call_enter(&state, &dummy_method);
  assert(g_enter_log.count == 1 && g_enter_log.last_self == &state);
  PgrQuickSpinSample s = take_sample(api);
  assert(s.active_generation == 0 && s.owned_root_count == 0);
  call_result(&state, &result, &dummy_method);
  assert(g_result_log.count == 1);
  s = take_sample(api);
  assert(s.cooldown_serial == 1);
  assert(g_live.empty());

  // 2. Happy path: enable -> enter roots the state -> tick issues Spin(360)+request ->
  //    result captured -> tick auto-closes on STATE_OPEN success.
  assert(api->update(1, 2000, tid, 2) == PGR_QUICK_SPIN_OK);
  call_enter(&state, &dummy_method);
  s = take_sample(api);
  assert(s.active_generation != 0 && s.owned_root_count == 1);

  const int cooldown_before = take_sample(api).cooldown_serial;
  assert(api->tick(tid, 1, 0) == PGR_QUICK_SPIN_OK);
  assert(g_spin_calls == 1 && g_request_calls == 1);
  s = take_sample(api);
  assert(s.request_serial == 1 && s.last_gui_state == 1);

  // Result arrives on the Unity thread for the active, sent record.
  call_result(&state, &result, &dummy_method);
  s = take_sample(api);
  assert(s.cooldown_serial == cooldown_before + 1);
  assert(s.last_result == 7);

  // Gui reaches STATE_OPEN; the next tick auto-closes and drops the record.
  gui.current_state = 2;
  assert(api->tick(tid, 2, 0) == PGR_QUICK_SPIN_OK);
  assert(g_close_calls == 1);
  s = take_sample(api);
  assert(s.closed_serial == 1 && s.active_generation == 0 && s.owned_root_count == 0);
  assert(g_live.empty());

  // 3. Exit on the owning state drops a live record (no spin issued).
  g_spin_calls = g_request_calls = 0;
  assert(api->update(1, 2000, tid, 3) == PGR_QUICK_SPIN_OK);
  call_enter(&state, &dummy_method);
  assert(take_sample(api).active_generation != 0 && g_live.size() == 1);
  call_exit(&state, &dummy_method);
  s = take_sample(api);
  assert(s.active_generation == 0 && s.owned_root_count == 0);
  assert(g_live.empty());

  // 4. Off-thread update marks cancellation but must NOT free GC handles; the owner
  //    thread frees them on its next tick (owner-only cleanup, no leak, no double free).
  assert(api->update(1, 2000, tid, 4) == PGR_QUICK_SPIN_OK);
  call_enter(&state, &dummy_method);
  assert(g_live.size() == 1);
  std::thread([&]() {
    assert(api->update(0, 0, tid, 4) == PGR_QUICK_SPIN_OK);  // off-thread disable
  }).join();
  assert(g_live.size() == 1);  // handle retired but not yet freed off-thread
  assert(api->tick(tid, 3, 0) == PGR_QUICK_SPIN_OK);  // owner drains retired list
  assert(g_live.empty());
  s = take_sample(api);
  assert(s.active_generation == 0 && s.owned_root_count == 0);

  // 5. clear() on the owner thread drops and frees an active record.
  assert(api->update(1, 2000, tid, 5) == PGR_QUICK_SPIN_OK);
  call_enter(&state, &dummy_method);
  assert(g_live.size() == 1);
  assert(api->clear() == PGR_QUICK_SPIN_OK);
  assert(g_live.empty());
  assert(take_sample(api).active_generation == 0);

  g_test_api = api;
  g_test_state = &state; g_test_result = &result;
  g_result_callback = call_result; g_spin_callback = call_spin;
  auto step = [&]() { assert(api->tick(tid,++g_test_tick,0) == PGR_QUICK_SPIN_OK); };

  // Synchronous result during request; reentrant tick from close must not close twice.
  assert(api->update(1,2000,tid,6) == PGR_QUICK_SPIN_OK);
  g_sync_result = true; g_close_reenter = true;
  int before_close = g_close_calls;
  int before_request = g_request_calls;
  call_enter(&state,&dummy_method); step(); step();
  assert(g_request_calls == before_request + 1 && g_close_calls == before_close + 1);
  assert(g_live.empty()); g_sync_result = false;

  // A foreign thread supplying its own real tid cannot acquire the permanent owner.
  assert(api->update(1,2000,tid,7) == PGR_QUICK_SPIN_OK);
  call_enter(&state,&dummy_method);
  assert(g_live.size() == 1);
  std::thread([&]() {
    assert(api->update(1,2000,this_tid(),7) == PGR_QUICK_SPIN_ERR_INVALID_ARG);
    api->tick(this_tid(),9999,0);
    call_enter(&state,&dummy_method);
    api->sample(&s); // pure sample must not free the retired handle
  }).join();
  assert(g_live.size() == 1); step(); assert(g_live.empty());

  // Metadata cannot change under live process-lifetime hooks, even with identical targets.
  bad = cfg; bad.can_method = &g_method_active;
  assert(api->install(&bad) == PGR_QUICK_SPIN_ERR_RECONFIG_ADDRESS_MISMATCH);

  // An uncertain request is counted, then never repeated within this encounter.
  assert(api->update(1,2000,tid,8) == PGR_QUICK_SPIN_OK);
  g_request_throws = true;
  before_request = g_request_calls;
  const auto serial = take_sample(api).request_serial;
  call_enter(&state,&dummy_method); step(); step();
  assert(g_request_calls == before_request + 1);
  assert(take_sample(api).request_serial == serial + 1 && g_live.empty());
  g_request_throws = false;

  // A manual spin marks the request as already sent, suppressing automatic submission.
  assert(api->update(1,2000,tid,9) == PGR_QUICK_SPIN_OK);
  before_request = g_request_calls;
  call_enter(&state,&dummy_method); call_spin(&spinner,17.0f,&dummy_method); step();
  assert(g_request_calls == before_request);
  api->clear(); assert(g_live.empty());

  // Expiry during a managed getter must revoke the action, not renew the cached policy.
  assert(api->update(1,5,tid,10) == PGR_QUICK_SPIN_OK);
  g_slow_can = true; before_request = g_request_calls;
  call_enter(&state,&dummy_method); step();
  assert(g_request_calls == before_request && g_live.empty()); g_slow_can = false;

  // Partial temporary-root allocation failure cleans up the persistent state root.
  assert(api->update(1,2000,tid,11) == PGR_QUICK_SPIN_OK);
  g_fail_root = &gui; call_enter(&state,&dummy_method); step();
  assert(g_request_calls == before_request && g_live.empty()); g_fail_root = nullptr;

  std::printf("quick_spin_test passed: chaining, success close, off-thread cleanup, synchronous result, reentrant close, owner binding, metadata binding, uncertain request, manual spin, expiry, partial root failure\n");
  return 0;
}

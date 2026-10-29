#include "catch_completion.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <thread>
#include <unistd.h>

// Real-API fake-IL2CPP harness for feature 24 (native direct Quick Catch completion). It resolves the
// table through the exported pgrazor_native_api v2 registry (query_feature(24,1)) and drives the
// controller against a fake managed heap (view/response/pokemon/action objects, field reads and a
// System.Action.Invoke() that can throw or re-enter). The module installs NO hook and NEVER calls back
// into JS. The coordinator builds and runs this against libpgrazor.so after registering query_feature
// case 24 and linking catch_completion.cpp.

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class / field / method tokens (distinct addresses) --------------------------
int cls_view, cls_response, cls_pokemon, cls_action;
int f_active, f_delegate_target, f_view_response, f_status, f_captured_id, f_pokemon_id;
int m_callback_invoke;

constexpr int32_t kSuccess = 3;  // arbitrary non-zero CatchSuccess literal

// ----- fake managed objects --------------------------------------------------------
struct FakeView { void *klass; uint8_t active; void *dsvw; };
struct FakeResponse { void *klass; int32_t status; uint64_t captured_id; };
struct FakePokemon { void *klass; uint64_t id; };
struct FakeAction { void *klass; void *target; };

// ----- runtime shims ----------------------------------------------------------------
const PgrCatchCompletionApi *g_api = nullptr;
uint64_t g_owner = 0;

int g_invoke_calls = 0;
bool g_throw_on_invoke = false;      // Action.Invoke reports a thrown managed exception
std::function<void()> g_invoke_oneshot;  // fired once inside Action.Invoke (reentrancy injection)

void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }

std::function<void()> g_field_oneshot;
void fake_field_get(void *obj, void *field, void *out) {
  if(g_field_oneshot){auto fn=g_field_oneshot;g_field_oneshot=nullptr;fn();}
  if (field == &f_active) { *static_cast<uint8_t *>(out) = static_cast<FakeView *>(obj)->active; return; }
  if (field == &f_delegate_target) { *static_cast<void **>(out) = static_cast<FakeAction *>(obj)->target; return; }
  if (field == &f_view_response) { *static_cast<void **>(out) = static_cast<FakeView *>(obj)->dsvw; return; }
  if (field == &f_status) { *static_cast<int32_t *>(out) = static_cast<FakeResponse *>(obj)->status; return; }
  if (field == &f_captured_id) { *static_cast<uint64_t *>(out) = static_cast<FakeResponse *>(obj)->captured_id; return; }
  if (field == &f_pokemon_id) { *static_cast<uint64_t *>(out) = static_cast<FakePokemon *>(obj)->id; return; }
  assert(false && "unknown field token");
}

void *fake_runtime_invoke(const void *method, void *, void **, void **exc) {
  if (exc) *exc = nullptr;
  if (method == &m_callback_invoke) {
    ++g_invoke_calls;
    if (g_invoke_oneshot) { auto fn = g_invoke_oneshot; g_invoke_oneshot = nullptr; fn(); }
    if (g_throw_on_invoke) { g_throw_on_invoke = false; if (exc) *exc = reinterpret_cast<void *>(0x1); }
    return nullptr;
  }
  assert(false && "unknown method token");
  return nullptr;
}

PgrCatchCompletionConfig make_config() {
  PgrCatchCompletionConfig c{};
  c.version = 1;
  c.size = sizeof(PgrCatchCompletionConfig);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c.class_view = &cls_view;
  c.class_response = &cls_response;
  c.class_pokemon = &cls_pokemon;
  c.class_action = &cls_action;
  c.f_active = &f_active;
  c.f_delegate_target = &f_delegate_target;
  c.f_view_response = &f_view_response;
  c.f_status = &f_status;
  c.f_captured_id = &f_captured_id;
  c.f_pokemon_id = &f_pokemon_id;
  c.m_callback_invoke = &m_callback_invoke;
  c.success_literal = kSuccess;
  c.reserved = 0;
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

PgrCatchCompletionSample take_sample() {
  PgrCatchCompletionSample s{};
  assert(g_api->sample(&s) == PGR_CC_OK);
  assert(s.version == 1 && s.size == sizeof(PgrCatchCompletionSample));
  return s;
}

uint64_t g_id_seq = 0x5000;
uint64_t next_id() { return ++g_id_seq; }

// A fully-wired confirmed direct summary-return scene.
struct Scene {
  FakeView view{&cls_view, 1, nullptr};
  FakeResponse resp{&cls_response, kSuccess, 0};
  FakePokemon pk{&cls_pokemon, 0};
  FakeAction cb{&cls_action, nullptr};
};
void wire(Scene &s, uint64_t id) {
  s.view.active = 1;
  s.view.dsvw = &s.resp;
  s.resp.status = kSuccess;
  s.resp.captured_id = id;
  s.pk.id = id;
  s.cb.target = &s.view;
}
PgrCatchCompletionRequest make_req(Scene &s, uint64_t id, uint64_t epoch, uint64_t tid) {
  PgrCatchCompletionRequest r{};
  r.version = 1;
  r.size = sizeof(r);
  r.expected_id = id;
  r.epoch = epoch;
  r.tid = tid;
  r.view = &s.view;
  r.response = &s.resp;
  r.pokemon = &s.pk;
  r.callback = &s.cb;
  return r;
}

uint64_t acquire(uint64_t epoch, uint64_t tid) {
  uint64_t tok = 0;
  assert(g_api->acquire(epoch, tid, &tok) == PGR_CC_OK);
  return tok;
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
  assert(core->query_feature(24, 99) == nullptr);
  const auto *api = static_cast<const PgrCatchCompletionApi *>(core->query_feature(24, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrCatchCompletionApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu request=%zu sample=%zu table=%zu\n",
              sizeof(PgrCatchCompletionConfig), sizeof(PgrCatchCompletionRequest),
              sizeof(PgrCatchCompletionSample), sizeof(PgrCatchCompletionApi));
  assert(sizeof(PgrCatchCompletionConfig) == 128 && sizeof(PgrCatchCompletionRequest) == 64 &&
         sizeof(PgrCatchCompletionSample) == 72 && sizeof(PgrCatchCompletionApi) == 64);

  const uint64_t tid = this_tid();
  g_owner = tid;
  PgrCatchCompletionConfig cfg = make_config();
  uint64_t tok = 0;

  // 1. Validation + not-installed + idempotence + reconfig.
  assert(api->install(nullptr) == PGR_CC_ERR_NULL_ARG);
  PgrCatchCompletionConfig bad = cfg; bad.version = 2;
  assert(api->install(&bad) == PGR_CC_ERR_BAD_VERSION);
  bad = cfg; bad.size = sizeof(PgrCatchCompletionConfig) - 1;
  assert(api->install(&bad) == PGR_CC_ERR_BAD_SIZE);
  bad = cfg; bad.m_callback_invoke = nullptr;
  assert(api->install(&bad) == PGR_CC_ERR_MISSING_PTR);
  { Scene s; wire(s, next_id()); PgrCatchCompletionRequest r = make_req(s, s.pk.id, 1, tid);
    assert(api->validate(&r) == PGR_CC_ERR_NOT_INSTALLED);
    assert(api->complete(&r, 1) == PGR_CC_ERR_NOT_INSTALLED); }

  assert(api->install(&cfg) == PGR_CC_OK);
  assert(api->install(&cfg) == PGR_CC_OK);  // idempotent
  bad = cfg; bad.success_literal = 9;
  assert(api->install(&bad) == PGR_CC_ERR_RECONFIG);

  // 2. Valid first catch: validate VALID, acquire a token, complete DONE, callback invoked exactly once.
  { Scene s; wire(s, next_id());
    assert(api->update(1, 2000, 1, tid) == PGR_CC_OK);
    PgrCatchCompletionRequest r = make_req(s, s.pk.id, 1, tid);
    assert(api->validate(&r) == PGR_CC_DONE);
    tok = acquire(1, tid); assert(tok != 0);
    g_invoke_calls = 0;
    assert(api->complete(&r, tok) == PGR_CC_DONE);
    assert(g_invoke_calls == 1);
    PgrCatchCompletionSample sm = take_sample();
    assert(sm.validate_ok >= 1 && sm.complete_ok == 1 && sm.last_token == tok && sm.busy == 0);
  }

  // 3. Wrong id / class / active / success: validate INVALID (0) and complete REFUSED without invoking.
  { Scene s; wire(s, next_id());
    assert(api->update(1, 2000, 1, tid) == PGR_CC_OK);
    g_invoke_calls = 0;

    PgrCatchCompletionRequest wrong_id = make_req(s, s.pk.id + 1, 1, tid);  // expected id != response/pokemon
    assert(api->validate(&wrong_id) == PGR_CC_OK);
    uint64_t t = acquire(1, tid); assert(t != 0);
    assert(api->complete(&wrong_id, t) == PGR_CC_OK);  // refused: re-validation fails

    FakeResponse other{&cls_response, kSuccess, s.pk.id};  // wrong class in the response slot
    PgrCatchCompletionRequest wrong_class = make_req(s, s.pk.id, 1, tid);
    wrong_class.response = &s.cb;  // an Action object where a response is expected
    assert(api->validate(&wrong_class) == PGR_CC_OK);

    s.view.active = 0;  // inactive view
    PgrCatchCompletionRequest inactive = make_req(s, s.pk.id, 1, tid);
    assert(api->validate(&inactive) == PGR_CC_OK);
    s.view.active = 1;

    s.resp.status = kSuccess + 1;  // not CatchSuccess
    PgrCatchCompletionRequest bad_status = make_req(s, s.pk.id, 1, tid);
    assert(api->validate(&bad_status) == PGR_CC_OK);
    s.resp.status = kSuccess;

    s.cb.target = &s.resp;  // delegate target is not the view
    PgrCatchCompletionRequest bad_target = make_req(s, s.pk.id, 1, tid);
    assert(api->validate(&bad_target) == PGR_CC_OK);
    s.cb.target = &s.view;

    PgrCatchCompletionRequest zero = make_req(s, 0, 1, tid);  // zero expected id
    assert(api->validate(&zero) == PGR_CC_OK);

    (void)other;
    assert(g_invoke_calls == 0);  // nothing was ever invoked
  }

  // 4. Gate: off / expired lease / wrong epoch -> acquire yields no token and complete is refused.
  { Scene s; wire(s, next_id());
    PgrCatchCompletionRequest r = make_req(s, s.pk.id, 1, tid);
    // Disabled.
    assert(api->update(0, 0, 1, tid) == PGR_CC_OK);
    assert(acquire(1, tid) == 0);
    assert(api->complete(&r, 999) == PGR_CC_OK);  // no live token
    // Armed but wrong epoch.
    assert(api->update(1, 2000, 2, tid) == PGR_CC_OK);
    assert(acquire(1, tid) == 0);  // epoch mismatch
    uint64_t t = acquire(2, tid); assert(t != 0);
    PgrCatchCompletionRequest r2 = make_req(s, s.pk.id, 1, tid);  // request epoch 1 != armed 2
    g_invoke_calls = 0;
    assert(api->complete(&r2, t) == PGR_CC_OK && g_invoke_calls == 0);  // refused on epoch
    // Expired lease.
    assert(api->update(1, 1, 3, tid) == PGR_CC_OK);
    ::usleep(3000);
    assert(acquire(3, tid) == 0);
    assert(g_invoke_calls == 0);
  }

  // 5. Repeated / older token: a token completes at most once; reusing it or an older one is refused.
  { Scene s; wire(s, next_id());
    assert(api->update(1, 2000, 1, tid) == PGR_CC_OK);
    PgrCatchCompletionRequest r = make_req(s, s.pk.id, 1, tid);
    uint64_t t1 = acquire(1, tid); assert(t1 != 0);
    uint64_t t2 = acquire(1, tid); assert(t2 == t1 + 1);  // strictly monotonic
    g_invoke_calls = 0;
    assert(api->complete(&r, t2) == PGR_CC_DONE && g_invoke_calls == 1);  // latest token completes
    assert(api->complete(&r, t2) == PGR_CC_OK && g_invoke_calls == 1);    // reused: refused, no 2nd invoke
    assert(api->complete(&r, t1) == PGR_CC_OK && g_invoke_calls == 1);    // older: refused
    uint64_t t3 = acquire(1, tid); assert(t3 == t2 + 1);
    assert(api->complete(&r, t3) == PGR_CC_DONE && g_invoke_calls == 2);  // a fresh token works again
  }

  // 6. Callback exception: complete reports ERR_MANAGED (host safe fallback); it still invoked once.
  { Scene s; wire(s, next_id());
    assert(api->update(1, 2000, 1, tid) == PGR_CC_OK);
    PgrCatchCompletionRequest r = make_req(s, s.pk.id, 1, tid);
    uint64_t t = acquire(1, tid); assert(t != 0);
    g_invoke_calls = 0; g_throw_on_invoke = true;
    assert(api->complete(&r, t) == PGR_CC_ERR_MANAGED && g_invoke_calls == 1);
    assert(take_sample().complete_fault >= 1 && take_sample().busy == 0);
  }

  // 7. Reentrancy inside the callback: a nested acquire gets no token (busy) and a nested complete is
  //    refused; a nested stop() cannot cause a second invoke either. The outer invoke happens once.
  { Scene s; wire(s, next_id());
    assert(api->update(1, 2000, 1, tid) == PGR_CC_OK);
    PgrCatchCompletionRequest r = make_req(s, s.pk.id, 1, tid);
    uint64_t t = acquire(1, tid); assert(t != 0);
    g_invoke_calls = 0;
    bool reentrant_token = true; int reentrant_complete = -1;
    g_invoke_oneshot = [&]() {
      uint64_t nested = 7;
      assert(g_api->acquire(1, g_owner, &nested) == PGR_CC_OK);
      reentrant_token = (nested != 0);                 // must be 0 while busy
      reentrant_complete = g_api->complete(&r, t);     // reused token + busy -> refused
      g_api->stop(g_owner);                            // immediate revoke from inside the callback
    };
    assert(api->complete(&r, t) == PGR_CC_DONE);
    assert(g_invoke_calls == 1);                       // single invoke despite reentrancy
    assert(!reentrant_token);                          // nested acquire refused (busy)
    assert(reentrant_complete == PGR_CC_OK);           // nested complete refused
    // After the in-callback stop(), the gate is revoked until re-armed.
    assert(acquire(1, tid) == 0);
  }

  // 8. Wrong thread cannot validate, acquire, or complete (owner already latched on this thread).
  std::thread([&]() {
    Scene s; wire(s, next_id());
    PgrCatchCompletionRequest r = make_req(s, s.pk.id, 1, this_tid());
    assert(g_api->validate(&r) == PGR_CC_OK);       // off-owner validate -> 0
    uint64_t t = 5;
    assert(g_api->acquire(1, this_tid(), &t) == PGR_CC_OK && t == 0);
    assert(g_api->complete(&r, 1) == PGR_CC_OK);    // off-owner complete refused
  }).join();
  { Scene s; wire(s, next_id());
    assert(api->update(1, 2000, 1, tid) == PGR_CC_OK);
    PgrCatchCompletionRequest r = make_req(s, s.pk.id, 1, tid + 1);  // mismatched proven_tid
    uint64_t t = 5;
    assert(api->acquire(1, tid + 1, &t) == PGR_CC_OK && t == 0);
    assert(api->complete(&r, 1) == PGR_CC_OK);
  }

  // 9. stop(): immediate revoke; a token acquired before the stop no longer completes.
  { Scene s; wire(s, next_id());
    assert(api->update(1, 2000, 1, tid) == PGR_CC_OK);
    PgrCatchCompletionRequest r = make_req(s, s.pk.id, 1, tid);
    uint64_t t = acquire(1, tid); assert(t != 0);
    assert(api->stop(tid) == PGR_CC_OK);
    g_invoke_calls = 0;
    assert(api->complete(&r, t) == PGR_CC_OK && g_invoke_calls == 0);  // revoked: refused, no invoke
    // Re-arming restores operation.
    assert(api->update(1, 2000, 1, tid) == PGR_CC_OK);
    uint64_t t2 = acquire(1, tid); assert(t2 != 0);
    assert(api->complete(&r, t2) == PGR_CC_DONE && g_invoke_calls == 1);
  }

  // Coordinator regression: a previously issued token cannot survive same-epoch off/on.
  {
    Scene s; wire(s,next_id()); auto r=make_req(s,s.pk.id,1,tid);
    api->update(1,2000,1,tid); auto t=acquire(1,tid); assert(t);
    api->update(0,0,1,tid); api->update(1,2000,1,tid);
    g_invoke_calls=0; assert(api->complete(&r,t)==0 && g_invoke_calls==0);
    // Pure validation must remain available after stop for ordinary-screen recovery.
    api->stop(tid); assert(api->validate(&r)==1);
  }
  // Coordinator regression: lease can expire during identity validation.
  {
    Scene s; wire(s,next_id()); auto r=make_req(s,s.pk.id,1,tid);
    api->update(1,10,1,tid); auto t=acquire(1,tid); assert(t);
    g_field_oneshot=[](){usleep(30000);};
    g_invoke_calls=0; assert(api->complete(&r,t)==0 && g_invoke_calls==0);
    assert(take_sample().busy==0);
  }
  // Coordinator regression: same-epoch revoke/rearm during a field read also aborts.
  {
    Scene s; wire(s,next_id()); auto r=make_req(s,s.pk.id,1,tid);
    api->update(1,2000,1,tid); auto t=acquire(1,tid); assert(t);
    g_field_oneshot=[&](){api->stop(tid);api->update(1,2000,1,tid);};
    g_invoke_calls=0; assert(api->complete(&r,t)==0 && g_invoke_calls==0);
    assert(take_sample().busy==0);
  }
  std::puts("PASS coordinator: stale token rearm, validation after STOP, expiry during validation, reentrant revocation");

  std::printf("catch_completion_test passed: ABI/validation/idempotence/reconfig, valid first catch "
              "(validate+acquire+complete, single invoke), wrong id/class/active/success/target/zero "
              "(invalid, refused, no invoke), gate off/epoch/expired (no token, refused), repeated/older "
              "token (complete once, reuse+older refused, fresh works), callback exception (ERR_MANAGED, "
              "one invoke), reentrant acquire/complete/stop inside the callback (single invoke), wrong "
              "thread, stop immediate revoke\n");
  return 0;
}

#include "summary_completion.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <unistd.h>

// Real-API fake-IL2CPP harness for feature 36 (native ordinary summary dismissal). It resolves the
// table through the exported pgrazor_native_api v2 registry (query_feature(36,1)) and drives the
// controller against a fake managed heap (gui/state/response/action objects, field reads, a static
// op_Implicit "alive" probe that returns a boxed System.Boolean, and a GuiController.Dismiss() that
// advances currentState and can throw or re-enter). The module installs NO hook and NEVER calls back
// into JS. The coordinator builds and runs this against libpgrazor.so after registering query_feature
// case 36 and linking summary_completion.cpp.

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class / field / method tokens (distinct addresses) --------------------------
int cls_gui, cls_state, cls_response, cls_bool, cls_action;
int f_dscx, f_dscy, f_status, f_captured_id, f_current_state, f_on_dismissed;
int m_alive, m_dismiss;

constexpr int32_t kSuccess = 3;    // arbitrary non-zero CatchSuccess literal
constexpr int32_t kStateOpen = 2;  // GuiController currentState == STATE_OPEN

// ----- fake managed objects --------------------------------------------------------
struct FakeGui { void *klass; int32_t current_state; void *on_dismissed; };
struct FakeState { void *klass; void *dscx; void *dscy; };
struct FakeResponse { void *klass; int32_t status; uint8_t captured[8]; };
struct FakeAction { void *klass; };
struct FakeBool { void *klass; uint8_t value; };

// ----- runtime shims ----------------------------------------------------------------
const PgrSummaryCompletionApi *g_api = nullptr;
uint64_t g_owner = 0;

int g_alive_calls = 0;
int g_alive_value = 1;            // 0/1 unboxed Boolean returned by op_Implicit
int g_alive_fault = 0;           // 0 none / 1 throw / 2 wrong boxed class / 3 out-of-range byte
int g_dismiss_calls = 0;
int32_t g_dismiss_next_state = 3; // currentState Dismiss leaves behind (advanced off STATE_OPEN)
bool g_throw_on_dismiss = false;  // Dismiss reports a thrown managed exception
std::function<void()> g_dismiss_oneshot;  // fired once inside Dismiss (reentrancy / gone injection)

FakeBool g_boxed{&cls_bool, 1};
std::map<uintptr_t,void*> g_handles;
std::set<uintptr_t> g_borrowed;
uintptr_t g_handle_seq=0x1000;
uintptr_t fake_gc_new(void *o,int){if(!o)return 0;g_handles[++g_handle_seq]=o;return g_handle_seq;}
void *fake_gc_get(uintptr_t h){auto i=g_handles.find(h);return i==g_handles.end()?nullptr:i->second;}
void fake_gc_free(uintptr_t h){assert(!g_borrowed.count(h));assert(g_handles.erase(h)==1);}
void *borrow(void *o){uintptr_t h=fake_gc_new(o,0);g_borrowed.insert(h);return reinterpret_cast<void*>(h);}
void balanced(){assert(g_handles.size()==g_borrowed.size());}

void reset_runtime() {
  balanced();g_handles.clear();g_borrowed.clear();
  g_alive_value = 1; g_alive_fault = 0; g_dismiss_next_state = 3;
  g_throw_on_dismiss = false; g_dismiss_oneshot = nullptr;
}

void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }

std::function<void()> g_field_oneshot;
void fake_field_get(void *obj, void *field, void *out) {
  if (g_field_oneshot) { auto fn = g_field_oneshot; g_field_oneshot = nullptr; fn(); }
  if (field == &f_dscx) { *static_cast<void **>(out) = static_cast<FakeState *>(obj)->dscx; return; }
  if (field == &f_dscy) { *static_cast<void **>(out) = static_cast<FakeState *>(obj)->dscy; return; }
  if (field == &f_status) { *static_cast<int32_t *>(out) = static_cast<FakeResponse *>(obj)->status; return; }
  if (field == &f_captured_id) { std::memcpy(out, static_cast<FakeResponse *>(obj)->captured, 8); return; }
  if (field == &f_current_state) { *static_cast<int32_t *>(out) = static_cast<FakeGui *>(obj)->current_state; return; }
  if (field == &f_on_dismissed) { *static_cast<void **>(out) = static_cast<FakeGui *>(obj)->on_dismissed; return; }
  assert(false && "unknown field token");
}

void *fake_unbox(void *boxed) { return boxed ? &static_cast<FakeBool *>(boxed)->value : nullptr; }

void *fake_runtime_invoke(const void *method, void *obj, void **params, void **exc) {
  if (exc) *exc = nullptr;
  if (method == &m_alive) {
    ++g_alive_calls;
    assert(params&&params[0]&&fake_obj_class(params[0])==&cls_gui); // reference is actual GUI, not &pointer
    if (g_alive_fault == 1) { if (exc) *exc = reinterpret_cast<void *>(0x2); return nullptr; }
    g_boxed.klass = (g_alive_fault == 2) ? &cls_response : &cls_bool;
    g_boxed.value = (g_alive_fault == 3) ? 7 : static_cast<uint8_t>(g_alive_value);
    return &g_boxed;
  }
  if (method == &m_dismiss) {
    ++g_dismiss_calls;
    if (obj) static_cast<FakeGui *>(obj)->current_state = g_dismiss_next_state;
    if (g_dismiss_oneshot) { auto fn = g_dismiss_oneshot; g_dismiss_oneshot = nullptr; fn(); }
    if (g_throw_on_dismiss) { g_throw_on_dismiss = false; if (exc) *exc = reinterpret_cast<void *>(0x1); }
    return nullptr;
  }
  assert(false && "unknown method token");
  return nullptr;
}

PgrSummaryCompletionConfig make_config() {
  PgrSummaryCompletionConfig c{};
  c.version = 1;
  c.size = sizeof(PgrSummaryCompletionConfig);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c.object_unbox = reinterpret_cast<void *>(&fake_unbox);
  c.class_gui = &cls_gui;
  c.class_state = &cls_state;
  c.class_response = &cls_response;
  c.class_bool = &cls_bool;
  c.f_dscx = &f_dscx;
  c.f_dscy = &f_dscy;
  c.f_status = &f_status;
  c.f_captured_id = &f_captured_id;
  c.f_current_state = &f_current_state;
  c.f_on_dismissed = &f_on_dismissed;
  c.m_alive = &m_alive;
  c.m_dismiss = &m_dismiss;
  c.gc_new=reinterpret_cast<void*>(fake_gc_new);c.gc_get=reinterpret_cast<void*>(fake_gc_get);
  c.gc_free=reinterpret_cast<void*>(fake_gc_free);
  c.success_literal = kSuccess;
  c.state_open = kStateOpen;
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

PgrSummaryCompletionSample take_sample() {
  PgrSummaryCompletionSample s{};
  assert(g_api->sample(&s) == PGR_SC_OK);
  assert(s.version == 1 && s.size == sizeof(PgrSummaryCompletionSample));
  return s;
}

uint64_t g_id_seq = 0x5000;
uint64_t next_id() { return ++g_id_seq; }

void put_id(uint8_t out[8], uint64_t id) { for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>((id >> (8 * i)) & 0xff); }

// A fully-wired confirmed summary-return scene.
struct Scene {
  FakeGui gui{&cls_gui, kStateOpen, nullptr};
  FakeState state{&cls_state, nullptr, nullptr};
  FakeResponse resp{&cls_response, kSuccess, {0}};
  FakeAction cb{&cls_action};
};
void wire(Scene &s, uint64_t id) {
  s.gui.current_state = kStateOpen;
  s.gui.on_dismissed = &s.cb;
  s.state.dscx = &s.gui;
  s.state.dscy = &s.resp;
  s.resp.status = kSuccess;
  put_id(s.resp.captured, id);
}
PgrSummaryCompletionRequest make_req(Scene &s, uint64_t id, uint64_t epoch, uint64_t tid, bool with_state = true) {
  PgrSummaryCompletionRequest r{};
  r.version = 1;
  r.size = sizeof(r);
  put_id(r.expected_key, id);
  r.epoch = epoch;
  r.tid = tid;
  r.gui = borrow(&s.gui);
  r.state = with_state ? borrow(&s.state) : nullptr;
  r.proto = borrow(&s.resp);
  r.response = borrow(&s.resp);
  r.dismissed = borrow(&s.cb);
  return r;
}

uint64_t acquire(uint64_t epoch, uint64_t tid) {
  uint64_t tok = 0;
  assert(g_api->acquire(epoch, tid, &tok) == PGR_SC_OK);
  return tok;
}

int complete(PgrSummaryCompletionRequest &r, uint64_t tok, PgrSummaryCompletionOutput &out) {
  std::memset(&out, 0, sizeof(out));
  const int rc=g_api->complete(&r,tok,&out);balanced();return rc;
}

// By-value so the request is an addressable lvalue (validate() reads it without side effects).
int vok(PgrSummaryCompletionRequest r) { return g_api->validate(&r); }

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) { std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror()); return 1; }
  auto getter = reinterpret_cast<GetApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == 48);
  assert(core->query_feature(36, 99) == nullptr);
  const auto *api = static_cast<const PgrSummaryCompletionApi *>(core->query_feature(36, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrSummaryCompletionApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu request=%zu output=%zu sample=%zu table=%zu\n",
              sizeof(PgrSummaryCompletionConfig), sizeof(PgrSummaryCompletionRequest),
              sizeof(PgrSummaryCompletionOutput), sizeof(PgrSummaryCompletionSample),
              sizeof(PgrSummaryCompletionApi));
  assert(sizeof(PgrSummaryCompletionConfig) == 168 && sizeof(PgrSummaryCompletionRequest) == 72 &&
         sizeof(PgrSummaryCompletionOutput) == 24 && sizeof(PgrSummaryCompletionSample) == 72 &&
         sizeof(PgrSummaryCompletionApi) == 64);

  const uint64_t tid = this_tid();
  g_owner = tid;
  PgrSummaryCompletionConfig cfg = make_config();
  PgrSummaryCompletionOutput out{};

  // 1. Validation + not-installed + idempotence + reconfig.
  assert(api->install(nullptr) == PGR_SC_ERR_NULL_ARG);
  PgrSummaryCompletionConfig bad = cfg; bad.version = 2;
  assert(api->install(&bad) == PGR_SC_ERR_BAD_VERSION);
  bad = cfg; bad.size = sizeof(PgrSummaryCompletionConfig) - 1;
  assert(api->install(&bad) == PGR_SC_ERR_BAD_SIZE);
  bad = cfg; bad.m_dismiss = nullptr;
  assert(api->install(&bad) == PGR_SC_ERR_MISSING_PTR);
  { reset_runtime(); Scene s; wire(s, next_id()); PgrSummaryCompletionRequest r = make_req(s, next_id(), 1, tid);
    assert(api->validate(&r) == PGR_SC_ERR_NOT_INSTALLED);
    assert(api->complete(&r, 1, &out) == PGR_SC_ERR_NOT_INSTALLED && out.status == PGR_SC_ERR_NOT_INSTALLED); }

  assert(api->install(&cfg) == PGR_SC_OK);
  assert(api->install(&cfg) == PGR_SC_OK);  // idempotent
  bad = cfg; bad.state_open = 9;
  assert(api->install(&bad) == PGR_SC_ERR_RECONFIG);
  assert(api->complete(nullptr, 1, nullptr) == PGR_SC_ERR_NULL_ARG);  // no output buffer

  // 2. Valid summary: validate VALID, acquire, complete DONE, Dismiss invoked once, output flags set.
  { reset_runtime(); Scene s; uint64_t id = next_id(); wire(s, id);
    assert(api->update(1, 2000, 1, tid) == PGR_SC_OK);
    PgrSummaryCompletionRequest r = make_req(s, id, 1, tid);
    assert(api->validate(&r) == PGR_SC_DONE);
    uint64_t tok = acquire(1, tid); assert(tok != 0);
    g_dismiss_calls = 0;
    assert(complete(r, tok, out) == PGR_SC_DONE);
    assert(g_dismiss_calls == 1);
    assert(out.version == 1 && out.size == 24 && out.status == PGR_SC_DONE);
    assert((out.flags & PGR_SC_FLAG_INVOKED) && (out.flags & PGR_SC_FLAG_BEFORE_KNOWN));
    assert((out.flags & PGR_SC_FLAG_AFTER_KNOWN) && !(out.flags & PGR_SC_FLAG_GONE));
    assert(out.before == kStateOpen && out.after == 3);  // advanced off STATE_OPEN
    PgrSummaryCompletionSample sm = take_sample();
    assert(sm.validate_ok >= 1 && sm.complete_ok == 1 && sm.last_token == tok && sm.busy == 0);
  }

  // 2b. Optional state absent: proto/response/id still validated; complete still works.
  { reset_runtime(); Scene s; uint64_t id = next_id(); wire(s, id);
    assert(api->update(1, 2000, 1, tid) == PGR_SC_OK);
    PgrSummaryCompletionRequest r = make_req(s, id, 1, tid, /*with_state=*/false);
    assert(r.state == nullptr);
    assert(api->validate(&r) == PGR_SC_DONE);
    uint64_t tok = acquire(1, tid); assert(tok != 0);
    g_dismiss_calls = 0;
    assert(complete(r, tok, out) == PGR_SC_DONE && g_dismiss_calls == 1 && (out.flags & PGR_SC_FLAG_INVOKED));
  }

  // 3. Each identity mismatch -> validate INVALID (0) and complete REFUSED without invoking Dismiss.
  { reset_runtime(); Scene s; uint64_t id = next_id(); wire(s, id);
    assert(api->update(1, 2000, 1, tid) == PGR_SC_OK);
    g_dismiss_calls = 0;

    PgrSummaryCompletionRequest wrong_id = make_req(s, id + 1, 1, tid);  // expected key != response id
    assert(api->validate(&wrong_id) == PGR_SC_OK);
    uint64_t t = acquire(1, tid); assert(t != 0);
    assert(complete(wrong_id, t, out) == PGR_SC_OK && !(out.flags & PGR_SC_FLAG_INVOKED));

    PgrSummaryCompletionRequest wrong_gui = make_req(s, id, 1, tid);
    wrong_gui.gui = borrow(&s.resp);  // a response where a gui is expected
    assert(api->validate(&wrong_gui) == PGR_SC_OK);

    PgrSummaryCompletionRequest wrong_state = make_req(s, id, 1, tid);
    wrong_state.state = borrow(&s.resp);  // non-null state with the wrong class
    assert(api->validate(&wrong_state) == PGR_SC_OK);

    void *saved = s.state.dscx; s.state.dscx = &s.resp;  // dscx != gui
    assert(vok(make_req(s, id, 1, tid)) == PGR_SC_OK);
    s.state.dscx = saved;

    saved = s.state.dscy; s.state.dscy = &s.gui;  // dscy != proto
    assert(vok(make_req(s, id, 1, tid)) == PGR_SC_OK);
    s.state.dscy = saved;

    s.resp.status = kSuccess + 1;  // not CatchSuccess
    assert(vok(make_req(s, id, 1, tid)) == PGR_SC_OK);
    s.resp.status = kSuccess;

    PgrSummaryCompletionRequest resp_split = make_req(s, id, 1, tid);
    FakeResponse other{&cls_response, kSuccess, {0}}; put_id(other.captured, id);
    resp_split.response = borrow(&other);  // response != proto (distinct object)
    assert(api->validate(&resp_split) == PGR_SC_OK);

    s.gui.current_state = 1;  // not STATE_OPEN
    assert(vok(make_req(s, id, 1, tid)) == PGR_SC_OK);
    s.gui.current_state = kStateOpen;

    s.gui.on_dismissed = nullptr;  // onDismissed null
    assert(vok(make_req(s, id, 1, tid)) == PGR_SC_OK);
    s.gui.on_dismissed = &s.cb;

    FakeAction other_cb{&cls_action};
    PgrSummaryCompletionRequest bad_cb = make_req(s, id, 1, tid);
    bad_cb.dismissed = borrow(&other_cb);  // dismissed != onDismissed
    assert(api->validate(&bad_cb) == PGR_SC_OK);

    PgrSummaryCompletionRequest null_cb = make_req(s, id, 1, tid);
    null_cb.dismissed = nullptr;  // dismissed null
    assert(api->validate(&null_cb) == PGR_SC_OK);

    uint8_t zero[8] = {0};
    PgrSummaryCompletionRequest zero_key = make_req(s, id, 1, tid);
    std::memcpy(zero_key.expected_key, zero, 8);  // zero expected id
    assert(api->validate(&zero_key) == PGR_SC_OK);

    g_alive_value = 0;  // gui not alive
    assert(vok(make_req(s, id, 1, tid)) == PGR_SC_OK);
    g_alive_value = 1;
    g_alive_fault = 1;  // alive probe throws -> fail closed invalid
    assert(vok(make_req(s, id, 1, tid)) == PGR_SC_OK);
    g_alive_fault = 2;  // wrong boxed class -> invalid
    assert(vok(make_req(s, id, 1, tid)) == PGR_SC_OK);
    g_alive_fault = 3;  // out-of-range boolean byte -> invalid
    assert(vok(make_req(s, id, 1, tid)) == PGR_SC_OK);
    g_alive_fault = 0;

    assert(g_dismiss_calls == 0);  // nothing was ever invoked
  }

  // 4. Gate: off / expired lease / wrong epoch -> acquire yields no token and complete is refused.
  { reset_runtime(); Scene s; uint64_t id = next_id(); wire(s, id);
    PgrSummaryCompletionRequest r = make_req(s, id, 1, tid);
    assert(api->update(0, 0, 1, tid) == PGR_SC_OK);
    assert(acquire(1, tid) == 0);
    assert(complete(r, 999, out) == PGR_SC_OK && !(out.flags & PGR_SC_FLAG_INVOKED));
    assert(api->update(1, 2000, 2, tid) == PGR_SC_OK);
    assert(acquire(1, tid) == 0);  // epoch mismatch
    uint64_t t = acquire(2, tid); assert(t != 0);
    PgrSummaryCompletionRequest r2 = make_req(s, id, 2, tid); r2.epoch = 1;  // request epoch 1 != armed 2
    g_dismiss_calls = 0;
    assert(complete(r2, t, out) == PGR_SC_OK && g_dismiss_calls == 0);
    assert(api->update(1, 1, 3, tid) == PGR_SC_OK);
    ::usleep(3000);
    assert(acquire(3, tid) == 0);
    assert(g_dismiss_calls == 0);
  }

  // 5. Repeated / older token: a token completes at most once; reuse or older is refused.
  { reset_runtime(); Scene s; uint64_t id = next_id(); wire(s, id);
    assert(api->update(1, 2000, 1, tid) == PGR_SC_OK);
    PgrSummaryCompletionRequest r = make_req(s, id, 1, tid);
    uint64_t t1 = acquire(1, tid); assert(t1 != 0);
    uint64_t t2 = acquire(1, tid); assert(t2 == t1 + 1);  // strictly monotonic
    g_dismiss_calls = 0;
    assert(complete(r, t2, out) == PGR_SC_DONE && g_dismiss_calls == 1);
    s.gui.current_state = kStateOpen;  // reset for the refused retries
    assert(complete(r, t2, out) == PGR_SC_OK && g_dismiss_calls == 1);  // reused: refused
    assert(complete(r, t1, out) == PGR_SC_OK && g_dismiss_calls == 1);  // older: refused
    uint64_t t3 = acquire(1, tid); assert(t3 == t2 + 1);
    assert(complete(r, t3, out) == PGR_SC_DONE && g_dismiss_calls == 2);  // a fresh token works again
  }

  // 6. Dismiss exception: complete reports ERR_MANAGED (host safe fallback) but still invoked once,
  //    with invoked flag set and the postcondition read.
  { reset_runtime(); Scene s; uint64_t id = next_id(); wire(s, id);
    assert(api->update(1, 2000, 1, tid) == PGR_SC_OK);
    PgrSummaryCompletionRequest r = make_req(s, id, 1, tid);
    uint64_t t = acquire(1, tid); assert(t != 0);
    g_dismiss_calls = 0; g_throw_on_dismiss = true;
    assert(complete(r, t, out) == PGR_SC_ERR_MANAGED && g_dismiss_calls == 1);
    assert((out.flags & PGR_SC_FLAG_INVOKED) && out.status == PGR_SC_ERR_MANAGED);
    assert((out.flags & PGR_SC_FLAG_AFTER_KNOWN) && out.after == 3);  // postcondition still read
    assert(take_sample().complete_fault >= 1 && take_sample().busy == 0);
  }

  // 7. Postcondition "gone": Dismiss leaves the gui not-alive -> gone flag, after unknown, DONE.
  { reset_runtime(); Scene s; uint64_t id = next_id(); wire(s, id);
    assert(api->update(1, 2000, 1, tid) == PGR_SC_OK);
    PgrSummaryCompletionRequest r = make_req(s, id, 1, tid);
    uint64_t t = acquire(1, tid); assert(t != 0);
    g_dismiss_oneshot = [](){ g_alive_value = 0; };  // becomes unreachable after Dismiss
    assert(complete(r, t, out) == PGR_SC_DONE);
    assert((out.flags & PGR_SC_FLAG_INVOKED) && (out.flags & PGR_SC_FLAG_GONE));
    assert(!(out.flags & PGR_SC_FLAG_AFTER_KNOWN) && out.before == kStateOpen);
  }

  // 8. Reentrancy inside Dismiss: a nested acquire gets no token (busy), a nested complete is refused,
  //    and a nested stop() cannot cause a second invoke. The outer Dismiss happens once.
  { reset_runtime(); Scene s; uint64_t id = next_id(); wire(s, id);
    assert(api->update(1, 2000, 1, tid) == PGR_SC_OK);
    PgrSummaryCompletionRequest r = make_req(s, id, 1, tid);
    uint64_t t = acquire(1, tid); assert(t != 0);
    g_dismiss_calls = 0;
    bool reentrant_token = true; int reentrant_complete = -1;
    g_dismiss_oneshot = [&]() {
      uint64_t nested = 7;
      assert(g_api->acquire(1, g_owner, &nested) == PGR_SC_OK);
      reentrant_token = (nested != 0);  // must be 0 while busy
      PgrSummaryCompletionOutput nout{};
      reentrant_complete = g_api->complete(&r, t, &nout);  // reused token + busy -> refused
      g_api->stop(g_owner);  // immediate revoke from inside the callback
    };
    assert(complete(r, t, out) == PGR_SC_DONE);
    assert(g_dismiss_calls == 1);
    assert(!reentrant_token);
    assert(reentrant_complete == PGR_SC_OK);
    assert(acquire(1, tid) == 0);  // gate revoked by the in-callback stop()
  }

  // 9. Wrong thread cannot validate, acquire, or complete (owner already latched on this thread).
  std::thread([&]() {
    reset_runtime();
    Scene s; uint64_t id = next_id(); wire(s, id);
    PgrSummaryCompletionRequest r = make_req(s, id, 1, this_tid());
    assert(g_api->validate(&r) == PGR_SC_OK);  // off-owner validate -> 0
    uint64_t t = 5;
    assert(g_api->acquire(1, this_tid(), &t) == PGR_SC_OK && t == 0);
    PgrSummaryCompletionOutput o{};
    assert(g_api->complete(&r, 1, &o) == PGR_SC_OK && !(o.flags & PGR_SC_FLAG_INVOKED));
  }).join();
  { reset_runtime(); Scene s; uint64_t id = next_id(); wire(s, id);
    assert(api->update(1, 2000, 1, tid) == PGR_SC_OK);
    PgrSummaryCompletionRequest r = make_req(s, id, 1, tid + 1);  // mismatched proven_tid
    uint64_t t = 5;
    assert(api->acquire(1, tid + 1, &t) == PGR_SC_OK && t == 0);
    assert(complete(r, 1, out) == PGR_SC_OK && !(out.flags & PGR_SC_FLAG_INVOKED));
  }

  // 10. stop(): immediate revoke; a token acquired before the stop no longer completes.
  { reset_runtime(); Scene s; uint64_t id = next_id(); wire(s, id);
    assert(api->update(1, 2000, 1, tid) == PGR_SC_OK);
    PgrSummaryCompletionRequest r = make_req(s, id, 1, tid);
    uint64_t t = acquire(1, tid); assert(t != 0);
    assert(api->stop(tid) == PGR_SC_OK);
    g_dismiss_calls = 0;
    assert(complete(r, t, out) == PGR_SC_OK && g_dismiss_calls == 0);  // revoked: refused, no invoke
    assert(api->update(1, 2000, 1, tid) == PGR_SC_OK);
    uint64_t t2 = acquire(1, tid); assert(t2 != 0);
    assert(complete(r, t2, out) == PGR_SC_DONE && g_dismiss_calls == 1);
  }

  // Coordinator regression: a previously issued token cannot survive same-epoch off/on.
  { reset_runtime(); Scene s; uint64_t id = next_id(); wire(s, id); auto r = make_req(s, id, 1, tid);
    api->update(1, 2000, 1, tid); auto t = acquire(1, tid); assert(t);
    api->update(0, 0, 1, tid); api->update(1, 2000, 1, tid);
    g_dismiss_calls = 0; assert(complete(r, t, out) == 0 && g_dismiss_calls == 0);
    api->stop(tid); assert(api->validate(&r) == 1);  // validation stays available after stop
  }
  // Coordinator regression: lease can expire during identity validation (a field read).
  { reset_runtime(); Scene s; uint64_t id = next_id(); wire(s, id); auto r = make_req(s, id, 1, tid);
    api->update(1, 10, 1, tid); auto t = acquire(1, tid); assert(t);
    g_field_oneshot = [](){ usleep(30000); };
    g_dismiss_calls = 0; assert(complete(r, t, out) == 0 && g_dismiss_calls == 0);
    assert(take_sample().busy == 0);
  }
  // Coordinator regression: same-epoch revoke/rearm during a field read also aborts.
  { reset_runtime(); Scene s; uint64_t id = next_id(); wire(s, id); auto r = make_req(s, id, 1, tid);
    api->update(1, 2000, 1, tid); auto t = acquire(1, tid); assert(t);
    g_field_oneshot = [&](){ api->stop(tid); api->update(1, 2000, 1, tid); };
    g_dismiss_calls = 0; assert(complete(r, t, out) == 0 && g_dismiss_calls == 0);
    assert(take_sample().busy == 0);
  }
  std::puts("PASS coordinator: stale token rearm, validation after STOP, expiry during validation, reentrant revocation");

  std::printf("summary_completion_test passed: ABI/validation/idempotence/reconfig, valid summary "
              "(validate+acquire+complete, single Dismiss, before/after flags), optional-state absent, "
              "identity mismatches (gui/state/dscx/dscy/status/response!=proto/currentState/onDismissed/"
              "null-cb/zero-id/alive false+faults -> invalid, refused, no invoke), gate off/epoch/expired, "
              "repeated/older token (complete once), Dismiss exception (ERR_MANAGED, invoked, after read), "
              "postcondition gone, reentrant acquire/complete/stop, wrong thread, stop immediate revoke\n");
  return 0;
}

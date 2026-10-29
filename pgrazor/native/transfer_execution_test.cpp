#include "transfer_execution.h"
#include "pgrazor_native_api.h"
#include "transfer_policy.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// Real-API fake-IL2CPP harness for feature 23 (native transfer request lifecycle). It resolves the
// table through the exported pgrazor_native_api v2 registry (query_feature(23,1)) and drives the
// controller against a fake managed heap (classes/methods/fields, GetPokemon identity lookup,
// ReleasePokemon with optional null/throw/synchronous callbacks). The module installs NO hook and
// re-runs the REAL feature-4 transfer_policy decide() in the same library. The 10 s timeout case
// waits 11 s against the production clock. The coordinator builds and runs this against libpgrazor.so
// after registering query_feature case 23 and linking transfer_execution.cpp.

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class / method / field tokens (distinct addresses) -------------------------
int cls_bag, cls_pokemon, cls_callback, cls_release_out, cls_promise;
int m_get, m_release;
int f_id, f_callback_pokemon, f_callback_bag, f_status;

constexpr int32_t kSuccess = 1;

// ----- fake managed objects --------------------------------------------------------
struct FakePokemon { void *klass; uint64_t id; };
struct FakeBag { void *klass; FakePokemon *current; };  // GetPokemon(id) -> current iff ids match
struct FakeCallback { void *klass; FakePokemon *pokemon; FakeBag *bag; };
struct FakeReleaseOut { void *klass; int32_t status; };
struct FakePromise { void *klass; };

// ----- runtime shims ----------------------------------------------------------------
const PgrTransferExecApi *g_api = nullptr;
uint64_t g_owner = 0;

uintptr_t g_next_handle = 0x7000;
std::unordered_map<uintptr_t, void *> g_live;
bool g_fail_root = false;

int g_release_calls = 0;
bool g_release_null = false;        // ReleasePokemon returns a null promise
bool g_stop_on_release = false;     // reentrant tick(stop) inside ReleasePokemon
bool g_sync_complete = false;       // fire a matched begin+end synchronously inside ReleasePokemon
FakeCallback *g_cb = nullptr;
FakeReleaseOut *g_resp = nullptr;
FakePromise *g_promise = nullptr;

// Generic one-shot reentrancy injection (as in outgoing_gifts_test): fire once when a chosen managed
// method is invoked, or make one invoke report a thrown exception.
const void *g_oneshot_method = nullptr;
std::function<void()> g_oneshot;
const void *g_throw_on = nullptr;
// One-shot fired from inside gchandle_free (used to prove a reentrant submit during cleanup is denied).
std::function<void()> g_free_oneshot;
std::function<void()> g_get_oneshot;
int g_get_after = 0;

uintptr_t fake_gc_new(void *obj, int) {
  if (obj == nullptr || g_fail_root) return 0;
  uintptr_t id = g_next_handle++;
  g_live[id] = obj;
  return id;
}
void *fake_gc_get(uintptr_t h) {
  if (g_get_oneshot && --g_get_after == 0) {
    auto fn=g_get_oneshot;g_get_oneshot=nullptr;fn();
  }
  auto it = g_live.find(h); return it != g_live.end() ? it->second : nullptr;
}
void fake_gc_free(uintptr_t h) {
  auto it = g_live.find(h);
  assert(it != g_live.end() && "double or invalid gchandle_free");
  g_live.erase(it);
  if (g_free_oneshot) { auto fn = g_free_oneshot; g_free_oneshot = nullptr; fn(); }
}
void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }

void fake_field_get(void *obj, void *field, void *out) {
  if (field == &f_id) { *static_cast<uint64_t *>(out) = static_cast<FakePokemon *>(obj)->id; return; }
  if (field == &f_callback_pokemon) { *static_cast<void **>(out) = static_cast<FakeCallback *>(obj)->pokemon; return; }
  if (field == &f_callback_bag) { *static_cast<void **>(out) = static_cast<FakeCallback *>(obj)->bag; return; }
  if (field == &f_status) { *static_cast<int32_t *>(out) = static_cast<FakeReleaseOut *>(obj)->status; return; }
  assert(false && "unknown field token");
}

void *fake_runtime_invoke(const void *method, void *obj, void **params, void **exc) {
  if (exc) *exc = nullptr;
  if (g_oneshot_method != nullptr && method == g_oneshot_method && g_oneshot) {
    auto fn = g_oneshot; g_oneshot = nullptr; g_oneshot_method = nullptr; fn();
  }
  if (g_throw_on != nullptr && method == g_throw_on) {
    g_throw_on = nullptr;
    if (exc) *exc = reinterpret_cast<void *>(0x1);
    return nullptr;
  }
  if (method == &m_get) {
    uint64_t want = *static_cast<uint64_t *>(params[0]);
    FakeBag *b = static_cast<FakeBag *>(obj);
    return (b->current && b->current->id == want) ? static_cast<void *>(b->current) : nullptr;
  }
  if (method == &m_release) {
    ++g_release_calls;
    if (g_stop_on_release) { g_stop_on_release = false; g_api->tick(g_owner, 1); }
    if (g_sync_complete) {
      g_sync_complete = false;
      uint64_t t = g_api->observe_begin(g_cb, g_resp, 0, g_owner);
      g_api->observe_end(t, g_owner);
    }
    return g_release_null ? nullptr : static_cast<void *>(g_promise);
  }
  assert(false && "unknown method token");
  return nullptr;
}

PgrTransferExecConfig make_config() {
  PgrTransferExecConfig c{};
  c.version = 1;
  c.size = sizeof(PgrTransferExecConfig);
  c.gchandle_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gchandle_get_target = reinterpret_cast<void *>(&fake_gc_get);
  c.gchandle_free = reinterpret_cast<void *>(&fake_gc_free);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.m_get = &m_get;
  c.m_release = &m_release;
  c.class_bag = &cls_bag;
  c.class_pokemon = &cls_pokemon;
  c.class_callback = &cls_callback;
  c.class_release_out = &cls_release_out;
  c.f_id = &f_id;
  c.f_callback_pokemon = &f_callback_pokemon;
  c.f_callback_bag = &f_callback_bag;
  c.f_status = &f_status;
  c.success_literal = kSuccess;
  c.reserved = 0;
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

PgrTransferExecSample take_sample() {
  PgrTransferExecSample s{};
  assert(g_api->sample(&s) == PGR_TX_OK);
  assert(s.version == 1 && s.size == sizeof(PgrTransferExecSample));
  return s;
}
std::vector<PgrTransferExecEvent> drain_all() {
  std::vector<PgrTransferExecEvent> out;
  PgrTransferExecEvent e{};
  int r;
  while ((r = g_api->next_event(&e)) == 1) out.push_back(e);
  assert(r == 0);
  return out;
}
bool saw(const std::vector<PgrTransferExecEvent> &v, uint32_t kind) {
  for (const auto &e : v) if (e.kind == kind) return true;
  return false;
}
bool saw_complete(const std::vector<PgrTransferExecEvent> &v, bool success) {
  for (const auto &e : v) if (e.kind == PGR_TX_EVENT_COMPLETE && ((e.aux & 1) != 0) == success) return true;
  return false;
}

uint64_t g_id_seq = 0x1000;
uint64_t next_id() { return ++g_id_seq; }

PgrTransferInput base_input() {
  PgrTransferInput v{};
  v.version = 1;
  v.size = sizeof(v);
  v.attack = 0; v.defense = 0; v.stamina = 0;  // 0% IV: well under the 90% keep cutoff
  v.species = 1;
  v.known = 207;  // all required-known bits (rare/shiny/event/background/favorite/egg)
  v.protect = 0; v.flags = 0; v.keep_all = 0; v.keep_count = 0; v.keep_ids = nullptr;
  return v;  // decide() -> 0 (authorized to transfer)
}

struct Scene {
  FakePokemon pk{&cls_pokemon, 0};
  FakeBag bag{&cls_bag, nullptr};
  FakeCallback cb{&cls_callback, nullptr, nullptr};
  FakeReleaseOut ro{&cls_release_out, kSuccess};
  FakePromise prom{&cls_promise};
};
void wire(Scene &s, uint64_t id) {
  s.pk.id = id;
  s.bag.current = &s.pk;        // GetPokemon(id) returns the same rooted pokemon
  s.cb.pokemon = &s.pk;
  s.cb.bag = &s.bag;
  s.ro.status = kSuccess;
}
PgrTransferExecRequest make_req(Scene &s, uint64_t id, const PgrTransferInput &in, uint64_t epoch, uint64_t tid) {
  PgrTransferExecRequest r{};
  r.version = 1; r.size = sizeof(r);
  r.captured_id = id; r.epoch = epoch; r.tid = tid;
  r.bag = &s.bag; r.pokemon = &s.pk; r.input = in;
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
  assert(core->query_feature(23, 99) == nullptr);
  const auto *api = static_cast<const PgrTransferExecApi *>(core->query_feature(23, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrTransferExecApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu request=%zu sample=%zu event=%zu table=%zu\n",
              sizeof(PgrTransferExecConfig), sizeof(PgrTransferExecRequest),
              sizeof(PgrTransferExecSample), sizeof(PgrTransferExecEvent), sizeof(PgrTransferExecApi));
  assert(sizeof(PgrTransferExecConfig) == 144 && sizeof(PgrTransferExecRequest) == 104 &&
         sizeof(PgrTransferExecSample) == 88 && sizeof(PgrTransferExecEvent) == 24 &&
         sizeof(PgrTransferExecApi) == 72);

  const uint64_t tid = this_tid();
  g_owner = tid;
  PgrTransferExecConfig cfg = make_config();
  uint64_t token = 0;

  // 1. Validation + not-installed + idempotence + reconfig.
  assert(api->install(nullptr) == PGR_TX_ERR_NULL_ARG);
  PgrTransferExecConfig bad = cfg; bad.version = 2;
  assert(api->install(&bad) == PGR_TX_ERR_BAD_VERSION);
  bad = cfg; bad.size = sizeof(PgrTransferExecConfig) - 1;
  assert(api->install(&bad) == PGR_TX_ERR_BAD_SIZE);
  bad = cfg; bad.f_status = nullptr;
  assert(api->install(&bad) == PGR_TX_ERR_MISSING_PTR);
  { Scene s; wire(s, next_id()); PgrTransferExecRequest r = make_req(s, s.pk.id, base_input(), 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_ERR_NOT_INSTALLED); }
  assert(api->tick(tid, 0) == PGR_TX_ERR_NOT_INSTALLED);

  assert(api->install(&cfg) == PGR_TX_OK);
  assert(api->install(&cfg) == PGR_TX_OK);  // idempotent
  bad = cfg; bad.success_literal = 9;
  assert(api->install(&bad) == PGR_TX_ERR_RECONFIG);

  // 2. Policy refusal (feature-4 decide != 0): keep-all, protected attribute, invalid IV. No issue,
  //    no event, no seen -- the host runs nothing extra and the id stays usable.
  { Scene s; wire(s, next_id());
    assert(api->update(1, 2000, 1, tid) == PGR_TX_OK);
    g_release_calls = 0;
    PgrTransferInput keep_all = base_input(); keep_all.keep_all = 1;
    PgrTransferExecRequest r = make_req(s, s.pk.id, keep_all, 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_OK && token == 0);
    PgrTransferInput shiny = base_input(); shiny.protect = 1; shiny.flags = 1;  // a protected attribute present
    r = make_req(s, s.pk.id, shiny, 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_OK && token == 0);
    PgrTransferInput invalid = base_input(); invalid.attack = 99;  // IV out of range
    r = make_req(s, s.pk.id, invalid, 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_OK && token == 0);
    assert(g_release_calls == 0);
    PgrTransferExecSample sm = take_sample();
    assert(sm.pending == 0 && sm.seen == 0 && sm.roots == 0 && g_live.empty());
    assert(drain_all().empty());
  }

  // 3. Identity mismatch: pokemon id != captured id; GetPokemon returns a different object; wrong class.
  { Scene s; wire(s, next_id());
    assert(api->update(1, 2000, 1, tid) == PGR_TX_OK);
    PgrTransferExecRequest r = make_req(s, s.pk.id + 1, base_input(), 1, tid);  // captured id != pokemon id
    assert(api->submit(&r, &token) == PGR_TX_ERR_IDENTITY && token == 0);

    FakePokemon imposter{&cls_pokemon, s.pk.id};  // same id, different object
    s.bag.current = &imposter;                      // GetPokemon returns the imposter, not the rooted pk
    r = make_req(s, s.pk.id, base_input(), 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_ERR_IDENTITY && token == 0);
    s.bag.current = &s.pk;

    r = make_req(s, s.pk.id, base_input(), 1, tid);
    r.pokemon = &s.bag;  // wrong class for the pokemon slot
    assert(api->submit(&r, &token) == PGR_TX_ERR_IDENTITY && token == 0);

    r = make_req(s, 0, base_input(), 1, tid);  // zero captured id
    assert(api->submit(&r, &token) == PGR_TX_ERR_IDENTITY && token == 0);
    assert(g_release_calls == 0);
    assert(take_sample().roots == 0 && g_live.empty());
  }

  // 4. Successful result: issue -> REQUEST + token; matched begin/end -> COMPLETE success; reaped.
  { Scene s; wire(s, next_id());
    g_promise = &s.prom;
    assert(api->update(1, 2000, 1, tid) == PGR_TX_OK);
    g_release_calls = 0;
    PgrTransferExecRequest r = make_req(s, s.pk.id, base_input(), 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_ISSUED && token != 0);
    assert(g_release_calls == 1);
    PgrTransferExecSample sm = take_sample();
    assert(sm.pending == 1 && sm.seen == 1 && sm.roots >= 3 && sm.last_token == token);
    assert(saw(drain_all(), PGR_TX_EVENT_REQUEST));
    uint64_t ticket = api->observe_begin(&s.cb, &s.ro, 0, tid);
    assert(ticket != 0);
    assert(api->observe_end(ticket, tid) == PGR_TX_OK);
    sm = take_sample();
    assert(sm.pending == 0 && sm.roots == 0 && g_live.empty());  // reaped at observe_end outer frame
    assert(saw_complete(drain_all(), true));
    g_promise = nullptr;
  }

  // 5. Duplicate: a second submit of an already-seen id is refused even after completion (no retry).
  { Scene s; wire(s, next_id());
    g_promise = &s.prom;
    assert(api->update(1, 2000, 1, tid) == PGR_TX_OK);
    PgrTransferExecRequest r = make_req(s, s.pk.id, base_input(), 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_ISSUED);
    uint64_t ticket = api->observe_begin(&s.cb, &s.ro, 0, tid);
    assert(api->observe_end(ticket, tid) == PGR_TX_OK);
    assert(take_sample().pending == 0 && g_live.empty());
    drain_all();
    // Same id again: quarantined.
    assert(api->submit(&r, &token) == PGR_TX_OK && token == 0);
    assert(take_sample().pending == 0 && g_live.empty());
    g_promise = nullptr;
  }

  // 6. Callback wrong bag / wrong pokemon: observe_begin returns 0, transaction not completed; a
  //    matched error observer (is_error) then completes it as a COMPLETE-error.
  { Scene s; wire(s, next_id());
    g_promise = &s.prom;
    assert(api->update(1, 2000, 1, tid) == PGR_TX_OK);
    PgrTransferExecRequest r = make_req(s, s.pk.id, base_input(), 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_ISSUED);
    drain_all();
    FakeBag other_bag{&cls_bag, &s.pk};
    FakeCallback wrong_bag{&cls_callback, &s.pk, &other_bag};
    assert(api->observe_begin(&wrong_bag, &s.ro, 0, tid) == 0);
    FakePokemon other_pk{&cls_pokemon, s.pk.id};
    FakeCallback wrong_pk{&cls_callback, &other_pk, &s.bag};
    assert(api->observe_begin(&wrong_pk, &s.ro, 0, tid) == 0);
    assert(take_sample().pending == 1);  // still pending, unmatched
    // A matched error observer (is_error=1) completes it as a failure.
    uint64_t ticket = api->observe_begin(&s.cb, nullptr, 1, tid);
    assert(ticket != 0);
    assert(api->observe_end(ticket, tid) == PGR_TX_OK);
    assert(take_sample().pending == 0 && g_live.empty());
    assert(saw_complete(drain_all(), false));
    g_promise = nullptr;
  }

  // 7. Synchronous result before submit returns: a matched begin+end fires inside ReleasePokemon.
  //    submit still returns ISSUED with the token set; COMPLETE is queued (not drained by observers)
  //    and the roots are reaped at submit's outer frame.
  { Scene s; wire(s, next_id());
    g_promise = &s.prom;
    g_cb = &s.cb; g_resp = &s.ro; g_sync_complete = true;
    assert(api->update(1, 2000, 1, tid) == PGR_TX_OK);
    PgrTransferExecRequest r = make_req(s, s.pk.id, base_input(), 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_ISSUED && token != 0);
    PgrTransferExecSample sm = take_sample();
    assert(sm.pending == 0 && sm.roots == 0 && g_live.empty());  // completed synchronously + reaped
    auto ev = drain_all();
    assert(saw(ev, PGR_TX_EVENT_REQUEST) && saw_complete(ev, true));
    g_cb = nullptr; g_resp = nullptr; g_promise = nullptr;
  }

  // 8. STOP during the GetPokemon lookup: the final gate sees the stop and refuses; no release.
  { Scene s; wire(s, next_id());
    assert(api->update(1, 2000, 1, tid) == PGR_TX_OK);
    g_release_calls = 0;
    g_oneshot_method = &m_get;
    g_oneshot = [&]() { g_api->tick(g_owner, 1); };
    PgrTransferExecRequest r = make_req(s, s.pk.id, base_input(), 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_OK && token == 0);  // refused
    assert(g_release_calls == 0);
    assert(take_sample().pending == 0 && take_sample().roots == 0 && g_live.empty());
  }

  // 9. Policy/epoch change during the GetPokemon lookup: the armed epoch moves, the gate's epoch check
  //    fails, submit is refused; no release.
  { Scene s; wire(s, next_id());
    assert(api->update(1, 2000, 1, tid) == PGR_TX_OK);
    g_release_calls = 0;
    g_oneshot_method = &m_get;
    g_oneshot = [&]() { g_api->update(1, 2000, 2, g_owner); };  // bump epoch mid-lookup
    PgrTransferExecRequest r = make_req(s, s.pk.id, base_input(), 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_OK && token == 0);  // refused: epoch mismatch
    assert(g_release_calls == 0);
    assert(take_sample().roots == 0 && g_live.empty());
  }

  // 10. STOP during ReleasePokemon: the release was issued (one call, token set) before the stop
  //     latched; teardown is deferred to the outer frame and frees every root. The id stays seen.
  { Scene s; wire(s, next_id());
    g_promise = &s.prom;
    assert(api->update(1, 2000, 1, tid) == PGR_TX_OK);
    g_release_calls = 0;
    g_stop_on_release = true;
    PgrTransferExecRequest r = make_req(s, s.pk.id, base_input(), 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_ISSUED && token != 0);
    assert(g_release_calls == 1);  // issued before the stop
    PgrTransferExecSample sm = take_sample();
    assert(sm.pending == 0 && sm.roots == 0 && g_live.empty());  // deferred teardown ran
    // No retry: the id is quarantined.
    assert(api->submit(&r, &token) == PGR_TX_OK && token == 0);
    drain_all();
    g_promise = nullptr;
  }

  // 11. Null promise + throw: both retain an uncertain issued transaction (no retry). A stop clears it.
  { Scene s; wire(s, next_id());
    assert(api->update(1, 2000, 1, tid) == PGR_TX_OK);
    g_release_calls = 0;
    g_release_null = true;  // null promise
    PgrTransferExecRequest r = make_req(s, s.pk.id, base_input(), 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_ISSUED && token != 0);
    g_release_null = false;
    assert(g_release_calls == 1);
    assert(take_sample().pending == 1);  // retained, uncertain
    assert(api->submit(&r, &token) == PGR_TX_OK && token == 0);  // no retry
    assert(api->tick(tid, 1) == PGR_TX_OK);  // stop clears
    assert(take_sample().pending == 0 && g_live.empty());
    drain_all();

    Scene s2; wire(s2, next_id());
    assert(api->update(1, 2000, 1, tid) == PGR_TX_OK);
    g_throw_on = &m_release;  // ReleasePokemon throws
    PgrTransferExecRequest r2 = make_req(s2, s2.pk.id, base_input(), 1, tid);
    assert(api->submit(&r2, &token) == PGR_TX_ISSUED && token != 0);  // still issued/uncertain
    assert(take_sample().pending == 1);
    assert(api->tick(tid, 1) == PGR_TX_OK);
    assert(take_sample().pending == 0 && g_live.empty());
    g_throw_on = nullptr;
    drain_all();
  }

  // 12. A reentrant submit DURING cleanup (inside a gchandle_free) is denied: the clean-in-progress
  //     gate refuses it, so no rearm survives the free.
  { Scene s; wire(s, next_id());
    g_promise = &s.prom;
    assert(api->update(1, 2000, 1, tid) == PGR_TX_OK);
    PgrTransferExecRequest r = make_req(s, s.pk.id, base_input(), 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_ISSUED);
    drain_all();
    uint64_t ticket = api->observe_begin(&s.cb, &s.ro, 0, tid);
    Scene s2; wire(s2, next_id());
    int reentrant = 123;
    g_free_oneshot = [&]() {
      PgrTransferExecRequest r2 = make_req(s2, s2.pk.id, base_input(), 1, g_owner);
      uint64_t tk = 999;
      reentrant = g_api->submit(&r2, &tk);
    };
    assert(api->observe_end(ticket, tid) == PGR_TX_OK);  // completes -> reap frees roots (fires oneshot)
    assert(reentrant == PGR_TX_OK);  // refused during cleanup
    assert(take_sample().pending == 0 && g_live.empty());  // rearm did not survive
    drain_all();
    g_promise = nullptr;
  }

  // 13. Wrong thread cannot drive the controller nor steal ownership (owner already latched above).
  std::thread([&]() {
    Scene s; wire(s, next_id());
    PgrTransferExecRequest r = make_req(s, s.pk.id, base_input(), 1, this_tid());
    uint64_t tk = 7;
    assert(g_api->submit(&r, &tk) == PGR_TX_OK && tk == 0);  // not owner: refused
    assert(g_api->observe_begin(&s.cb, &s.ro, 0, this_tid()) == 0);
    assert(g_api->tick(this_tid(), 0) == PGR_TX_OK);  // no-op off owner
  }).join();
  { Scene s; wire(s, next_id());
    PgrTransferExecRequest r = make_req(s, s.pk.id, base_input(), 1, tid + 1);  // mismatched proven_tid
    assert(api->submit(&r, &token) == PGR_TX_OK && token == 0);
  }
  assert(g_live.empty());

  // 14. Real 10 s timeout: an issued request with no callback emits a one-shot TIMEOUT and is retained;
  //     a stop then clears it and frees every root.
  { Scene s; wire(s, next_id());
    g_promise = &s.prom;
    assert(api->update(1, 2000, 1, tid) == PGR_TX_OK);
    PgrTransferExecRequest r = make_req(s, s.pk.id, base_input(), 1, tid);
    assert(api->submit(&r, &token) == PGR_TX_ISSUED);
    drain_all();
    ::usleep((PGR_TX_TIMEOUT_MS + 1000) * 1000);  // 11 s against the production clock
    assert(api->tick(tid, 0) == PGR_TX_OK);
    PgrTransferExecSample sm = take_sample();
    assert(sm.pending == 1 && sm.timeout_serial >= 1);  // retained, timed out once
    assert(saw(drain_all(), PGR_TX_EVENT_TIMEOUT));
    assert(api->tick(tid, 0) == PGR_TX_OK);  // no second timeout for the same request
    assert(drain_all().empty());
    assert(api->tick(tid, 1) == PGR_TX_OK);  // stop clears
    sm = take_sample();
    assert(sm.pending == 0 && sm.roots == 0 && g_live.empty());
    g_promise = nullptr;
    drain_all();
  }

  // Off/on with the same epoch must invalidate an in-flight getter, too.
  { Scene s; wire(s,next_id());g_promise=&s.prom;g_release_calls=0;
    api->update(1,2000,1,tid);
    auto req=make_req(s,s.pk.id,base_input(),1,tid);
    g_oneshot_method=&m_get;g_oneshot=[&](){api->update(0,0,1,tid);api->update(1,2000,1,tid);};
    assert(api->submit(&req,&token)==PGR_TX_OK && token==0);
    assert(g_release_calls==0 && g_live.empty());drain_all();
  }
  // Revoke on the final argument lookup, after GetPokemon returned.
  { Scene s; wire(s,next_id());g_promise=&s.prom;g_release_calls=0;
    api->update(1,2000,1,tid);auto req=make_req(s,s.pk.id,base_input(),1,tid);
    g_oneshot_method=&m_get;g_oneshot=[&](){g_get_after=3;g_get_oneshot=[&](){api->tick(tid,1);};};
    assert(api->submit(&req,&token)==PGR_TX_OK && token==0);
    assert(!g_get_oneshot && g_release_calls==0 && g_live.empty());drain_all();
  }
  // Shutdown invalidates abandoned onEnter tickets without reusing their nonce.
  { Scene s; wire(s,next_id());g_promise=&s.prom;
    api->update(1,2000,1,tid);auto req=make_req(s,s.pk.id,base_input(),1,tid);
    assert(api->submit(&req,&token)==PGR_TX_ISSUED);
    auto stale=api->observe_begin(&s.cb,&s.ro,0,tid);assert(stale);
    assert(take_sample().open_tickets==1);api->tick(tid,1);
    assert(take_sample().open_tickets==0 && g_live.empty());drain_all();
    api->observe_end(stale,tid);assert(drain_all().empty());
  }
  // STOP during one completed transaction's free also clears another pending transaction.
  { Scene a,b;wire(a,next_id());wire(b,next_id());api->update(1,2000,1,tid);
    auto ra=make_req(a,a.pk.id,base_input(),1,tid),rb=make_req(b,b.pk.id,base_input(),1,tid);
    g_promise=&a.prom;assert(api->submit(&ra,&token)==PGR_TX_ISSUED);
    g_promise=&b.prom;assert(api->submit(&rb,&token)==PGR_TX_ISSUED);
    auto ticket=api->observe_begin(&a.cb,&a.ro,0,tid);assert(ticket);
    g_free_oneshot=[&](){api->tick(tid,1);};api->observe_end(ticket,tid);
    assert(take_sample().pending==0 && g_live.empty());drain_all();
  }
  g_promise=nullptr;
  // All roots freed after every path (process-wide).
  assert(g_live.empty());
  assert(take_sample().roots == 0 && take_sample().pending == 0 && take_sample().open_tickets == 0);

  std::printf("transfer_execution_test passed: ABI/validation/idempotence/reconfig, policy refusal "
              "(keep-all/protected/invalid IV), identity mismatch (id/object/class/zero), successful "
              "result (REQUEST+token, matched begin/end COMPLETE, reaped), duplicate quarantine, "
              "unmatched callback (wrong bag/pokemon) + error-observer COMPLETE-error, synchronous "
              "result inside release (issued, token set, reaped), STOP during GetPokemon (refused, no "
              "release), epoch change during GetPokemon (refused), STOP during release (one issued, "
              "deferred teardown, seen retained), null-promise/throw uncertain (no retry), reentrant "
              "submit during cleanup denied, wrong thread, real 10 s timeout (once, retained, stop "
              "clears); all roots freed\n");
  return 0;
}

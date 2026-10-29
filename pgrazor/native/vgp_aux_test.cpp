#include "vgp_aux.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <thread>
#include <unistd.h>

// Real-API fake-IL2CPP harness for feature 34 (VGP consolidation auxiliary). It resolves the table through
// the exported pgrazor_native_api v2 registry (query_feature(34,1)) and drives the two stateless helpers
// against a fake managed heap: an item bag carrying an rpc service, an open generic SendRpc that reflects
// and inflates through MakeGenericMethod, and a transfer path (response -> GetPokemon -> pokemon + bag
// GameMaster). gchandle_new/get/free back a handle table so OWN-handle transfer + leak accounting are
// exact; it can fail a root, relocate a rooted object, throw, or re-enter. No new exact classes are
// introduced beyond the legacy rpc / item-bag / pokemon / response. The module installs NO hook.

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class / field / method tokens (distinct addresses) --------------------------
int cls_ibag, cls_rpc, cls_request, cls_spin, cls_type, cls_pokemon, cls_response, cls_reflection,
    cls_gm, cls_other;
int f_rpc, f_caught_id, f_pokemon_id, f_bag_gm;
int m_generic, m_make, m_closed, m_getpokemon;

// Il2CppType tokens keyed by class (stable; class_get_type returns these).
int type_of_request, type_of_spin;

// ----- fake managed objects --------------------------------------------------------
struct FakeBag { void *klass; void *rpc; };          // item bag (spin)
struct FakeRpc { void *klass; };
struct FakeReflection { void *klass; };              // open + closed Reflection.MethodInfo objects
struct FakeType { void *klass; void *of; };          // System.Type object
struct FakeArray { void *klass; uint8_t pad[24]; void *data[2]; };  // data[] at byte offset 32
struct FakeResponse { void *klass; uint64_t caught_id; };
struct FakePokeBag { void *klass; void *gm; struct FakePokemon *current; };
struct FakePokemon { void *klass; uint64_t id; };
struct FakeGm { void *klass; };

FakeRpc g_rpc{&cls_rpc};
FakeReflection g_reflection{&cls_reflection};
FakeReflection g_closed_reflection{&cls_reflection};
FakeGm g_gm{&cls_gm};

// ----- gc handle table -------------------------------------------------------------
std::map<uintptr_t, void *> g_handles;
uintptr_t g_handle_seq = 0x900;
uint64_t g_gcnew_calls = 0;
uint64_t g_gcnew_fail_index = 0;  // fail the Nth gc_new (1-based); 0 = never

uintptr_t fake_gc_new(void *obj, int /*pinned*/) {
  ++g_gcnew_calls;
  if (obj == nullptr) return 0;
  if (g_gcnew_fail_index != 0 && g_gcnew_calls == g_gcnew_fail_index) return 0;
  uintptr_t h = ++g_handle_seq;
  g_handles[h] = obj;
  return h;
}
void *fake_gc_get(uintptr_t h) { auto it = g_handles.find(h); return it == g_handles.end() ? nullptr : it->second; }
void fake_gc_free(uintptr_t h) { g_handles.erase(h); }

void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }

// ----- field get -------------------------------------------------------------------
void fake_field_get(void *obj, void *field, void *out) {
  if (field == &f_rpc) { *static_cast<void **>(out) = static_cast<FakeBag *>(obj)->rpc; return; }
  if (field == &f_caught_id) { *static_cast<uint64_t *>(out) = static_cast<FakeResponse *>(obj)->caught_id; return; }
  if (field == &f_pokemon_id) { *static_cast<uint64_t *>(out) = static_cast<FakePokemon *>(obj)->id; return; }
  if (field == &f_bag_gm) { *static_cast<void **>(out) = static_cast<FakePokeBag *>(obj)->gm; return; }
  assert(false && "unknown field_get token");
}

// ----- reflection ------------------------------------------------------------------
bool g_generic_null = false, g_generic_not_generic = false;
int g_generic_arity = 5, g_closed_arity = 5;
bool g_make_null = false, g_closed_null = false, g_closed_not_inflated = false;

const void *fake_class_get_method(void *klass, const char *name, int argc) {
  if (klass == &cls_rpc && std::strcmp(name, "SendRpc") == 0 && argc == 5)
    return g_generic_null ? nullptr : &m_generic;
  if (klass == &cls_reflection && std::strcmp(name, "MakeGenericMethod") == 0 && argc == 1)
    return g_make_null ? nullptr : &m_make;
  return nullptr;
}
uint8_t fake_method_is_generic(const void *m) { return (m == &m_generic && !g_generic_not_generic) ? 1 : 0; }
uint8_t fake_method_is_inflated(const void *m) { return (m == &m_closed && !g_closed_not_inflated) ? 1 : 0; }
uint32_t fake_method_param_count(const void *m) {
  if (m == &m_generic) return static_cast<uint32_t>(g_generic_arity);
  if (m == &m_closed) return static_cast<uint32_t>(g_closed_arity);
  return 0;
}
void *fake_method_get_object(const void *m, void *klass) {
  assert(m == &m_generic && klass == &cls_rpc);
  return &g_reflection;
}
const void *fake_unreflect(void *reflection) {
  assert(reflection == &g_closed_reflection);
  return g_closed_null ? nullptr : &m_closed;
}

// ----- array / type / barrier ------------------------------------------------------
std::function<void()> g_type_oneshot;  // fired once inside type_get_object (relocation hook)
FakeArray *g_array = nullptr;
int g_make_calls = 0;
bool g_throw_on_make = false, g_null_on_make = false;
std::function<void()> g_make_oneshot;  // fired once inside the MakeGenericMethod invoke

void *fake_array_new(void *elem, uintptr_t len) {
  assert(elem == &cls_type && len == 2);
  g_array = new FakeArray{};
  g_array->klass = nullptr;
  return g_array;
}
void *fake_class_get_type(void *klass) {
  if (klass == &cls_request) return &type_of_request;
  if (klass == &cls_spin) return &type_of_spin;
  assert(false && "unknown class_get_type token");
  return nullptr;
}
void *fake_type_get_object(void *type) {
  if (g_type_oneshot) { auto fn = g_type_oneshot; g_type_oneshot = nullptr; fn(); }
  FakeType *t = new FakeType{&cls_type, type};
  return t;
}
// Records the array object each barrier write landed in (proves re-resolution after relocation).
void *g_barrier_target = nullptr;
int g_barrier_writes = 0;
void fake_wbarrier(void *obj, void **slot, void *value) {
  g_barrier_target = obj;
  ++g_barrier_writes;
  *slot = value;
}

// ----- runtime_invoke --------------------------------------------------------------
int g_get_calls = 0;
bool g_throw_on_get = false;
FakePokemon *g_bag_current = nullptr;  // GetPokemon result
std::function<void()> g_get_oneshot;   // fired once inside the GetPokemon invoke
uint64_t g_last_get_id = 0;

void *fake_runtime_invoke(const void *method, void *, void **params, void **exc) {
  if (exc) *exc = nullptr;
  if (method == &m_make) {
    ++g_make_calls;
    if (g_make_oneshot) { auto fn = g_make_oneshot; g_make_oneshot = nullptr; fn(); }
    if (g_throw_on_make) { g_throw_on_make = false; if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
    if (g_null_on_make) { g_null_on_make = false; return nullptr; }
    return &g_closed_reflection;
  }
  if (method == &m_getpokemon) {
    ++g_get_calls;
    g_last_get_id = *static_cast<uint64_t *>(params[0]);
    if (g_get_oneshot) { auto fn = g_get_oneshot; g_get_oneshot = nullptr; fn(); }
    if (g_throw_on_get) { g_throw_on_get = false; if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
    return g_bag_current;
  }
  assert(false && "unknown method token");
  return nullptr;
}

PgrVgpAuxConfig make_config() {
  PgrVgpAuxConfig c{};
  c.version = 1;
  c.size = sizeof(c);
  c.gc_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gc_get = reinterpret_cast<void *>(&fake_gc_get);
  c.gc_free = reinterpret_cast<void *>(&fake_gc_free);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.class_get_method_from_name = reinterpret_cast<void *>(&fake_class_get_method);
  c.method_is_generic = reinterpret_cast<void *>(&fake_method_is_generic);
  c.method_get_param_count = reinterpret_cast<void *>(&fake_method_param_count);
  c.method_get_object = reinterpret_cast<void *>(&fake_method_get_object);
  c.array_new = reinterpret_cast<void *>(&fake_array_new);
  c.class_get_type = reinterpret_cast<void *>(&fake_class_get_type);
  c.type_get_object = reinterpret_cast<void *>(&fake_type_get_object);
  c.gc_wbarrier_set_field = reinterpret_cast<void *>(&fake_wbarrier);
  c.method_get_from_reflection = reinterpret_cast<void *>(&fake_unreflect);
  c.method_is_inflated = reinterpret_cast<void *>(&fake_method_is_inflated);
  c.class_rpc = &cls_rpc;
  c.class_request = &cls_request;
  c.class_spin = &cls_spin;
  c.class_type = &cls_type;
  c.class_item_bag = &cls_ibag;
  c.class_pokemon = &cls_pokemon;
  c.class_response = &cls_response;
  c.f_rpc = &f_rpc;
  c.f_caught_id = &f_caught_id;
  c.f_pokemon_id = &f_pokemon_id;
  c.f_bag_gm = &f_bag_gm;
  c.m_get_pokemon = &m_getpokemon;
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

const PgrVgpAuxApi *g_api = nullptr;

void reset_flags() {
  g_gcnew_fail_index = 0; g_gcnew_calls = 0;
  g_generic_null = g_generic_not_generic = false; g_generic_arity = 5; g_closed_arity = 5;
  g_make_null = g_closed_null = g_closed_not_inflated = false;
  g_throw_on_make = g_null_on_make = false; g_make_calls = 0; g_make_oneshot = nullptr;
  g_type_oneshot = nullptr; g_barrier_target = nullptr; g_barrier_writes = 0; g_array = nullptr;
  g_throw_on_get = false; g_get_calls = 0; g_get_oneshot = nullptr; g_last_get_id = 0; g_bag_current = nullptr;
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
  assert(core->query_feature(34, 99) == nullptr);
  const auto *api = static_cast<const PgrVgpAuxApi *>(core->query_feature(34, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrVgpAuxApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu spin=%zu transfer=%zu table=%zu\n",
              sizeof(PgrVgpAuxConfig), sizeof(PgrVgpAuxSpin), sizeof(PgrVgpAuxTransfer),
              sizeof(PgrVgpAuxApi));
  assert(sizeof(PgrVgpAuxConfig) == 232 && sizeof(PgrVgpAuxSpin) == 32 &&
         sizeof(PgrVgpAuxTransfer) == 32 && sizeof(PgrVgpAuxApi) == 32);

  const uint64_t tid = this_tid();
  PgrVgpAuxConfig cfg = make_config();

  FakeBag bag{&cls_ibag, &g_rpc};
  FakePokeBag pbag{&cls_other, &g_gm, nullptr};
  FakePokemon pk{&cls_pokemon, 0};
  FakeResponse resp{&cls_response, 0};
  PgrVgpAuxSpin sp{};
  PgrVgpAuxTransfer tr{};

  // 1. Validation + not-installed + idempotence + reconfig.
  assert(api->install(nullptr) == PGR_VX_ERR_NULL_ARG);
  PgrVgpAuxConfig bad = cfg; bad.version = 2;
  assert(api->install(&bad) == PGR_VX_ERR_BAD_VERSION);
  bad = cfg; bad.size = sizeof(cfg) - 1;
  assert(api->install(&bad) == PGR_VX_ERR_BAD_SIZE);
  bad = cfg; bad.m_get_pokemon = nullptr;
  assert(api->install(&bad) == PGR_VX_ERR_MISSING_PTR);
  assert(api->spin(&bag, tid, 2000, &sp) == PGR_VX_ERR_NOT_INSTALLED);
  assert(api->transfer(&resp, &pbag, tid, 2000, &tr) == PGR_VX_ERR_NOT_INSTALLED);

  assert(api->install(&cfg) == PGR_VX_OK);
  assert(api->install(&cfg) == PGR_VX_OK);  // idempotent
  bad = cfg; bad.class_rpc = &cls_other;
  assert(api->install(&bad) == PGR_VX_ERR_RECONFIG);

  // 2. spin SUCCESS: reflection resolves, 2 Type[] writes, closed inflated method, OWN handles, no leak.
  { reset_flags();
    std::memset(&sp, 0xee, sizeof(sp));
    assert(api->spin(&bag, tid, 2000, &sp) == PGR_VX_SUCCESS);
    assert(sp.version == 1 && sp.size == sizeof(sp));
    assert(sp.rpc_handle != 0 && fake_gc_get(sp.rpc_handle) == &g_rpc);
    assert(sp.reflection_handle != 0 && fake_gc_get(sp.reflection_handle) == &g_closed_reflection);
    assert(sp.closed_method == &m_closed);
    assert(g_make_calls == 1 && g_barrier_writes == 2);
    assert(g_array != nullptr && g_array->data[0] != nullptr && g_array->data[1] != nullptr);
    // Only the two OWN handles remain; every temporary (bag/open-reflection/array/types) was freed.
    assert(g_handles.size() == 2);
    fake_gc_free(sp.rpc_handle); fake_gc_free(sp.reflection_handle);
    assert(g_handles.empty());
  }

  // 3. spin RELOCATION: type_get_object moves the already-rooted Type[] array; the module re-resolves it
  //    from its handle, so both barrier writes land in the NEW array (never the stale pointer).
  { reset_flags();
    FakeArray *moved = new FakeArray{};
    g_type_oneshot = [&]() {
      // Repoint the one live array handle at a fresh object (simulate a relocating collector).
      for (auto &kv : g_handles) if (kv.second == g_array) { kv.second = moved; break; }
    };
    assert(api->spin(&bag, tid, 2000, &sp) == PGR_VX_SUCCESS);
    assert(g_barrier_target == moved && moved->data[0] != nullptr && moved->data[1] != nullptr);
    fake_gc_free(sp.rpc_handle); fake_gc_free(sp.reflection_handle);
    assert(g_handles.empty());
    delete moved;
  }

  // 4. spin reflection/identity/throw/root failures: reported, temporaries freed, OWN handle 0.
  { reset_flags();
    FakeBag wrong{&cls_other, &g_rpc};
    assert(api->spin(&wrong, tid, 2000, &sp) == PGR_VX_ERR_IDENTITY && sp.rpc_handle == 0);
    assert(g_handles.empty());

    reset_flags();  // rpc field not the exact rpc class
    FakeBag bad_rpc{&cls_ibag, &g_closed_reflection};
    assert(api->spin(&bad_rpc, tid, 2000, &sp) == PGR_VX_ERR_IDENTITY && g_handles.empty());

    reset_flags(); g_generic_null = true;
    assert(api->spin(&bag, tid, 2000, &sp) == PGR_VX_ERR_REFLECT && g_handles.empty());
    reset_flags(); g_generic_not_generic = true;
    assert(api->spin(&bag, tid, 2000, &sp) == PGR_VX_ERR_REFLECT && g_handles.empty());
    reset_flags(); g_generic_arity = 4;
    assert(api->spin(&bag, tid, 2000, &sp) == PGR_VX_ERR_REFLECT && g_handles.empty());
    reset_flags(); g_make_null = true;
    assert(api->spin(&bag, tid, 2000, &sp) == PGR_VX_ERR_REFLECT && g_handles.empty());
    reset_flags(); g_throw_on_make = true;
    assert(api->spin(&bag, tid, 2000, &sp) == PGR_VX_ERR_MANAGED && g_make_calls == 1 && g_handles.empty());
    reset_flags(); g_null_on_make = true;
    assert(api->spin(&bag, tid, 2000, &sp) == PGR_VX_ERR_REFLECT && g_handles.empty());

    // Closed-method validation: unreflect null / not-inflated / wrong arity.
    reset_flags(); g_closed_null = true;
    assert(api->spin(&bag, tid, 2000, &sp) == PGR_VX_ERR_REFLECT && g_handles.empty());
    reset_flags(); g_closed_not_inflated = true;
    assert(api->spin(&bag, tid, 2000, &sp) == PGR_VX_ERR_REFLECT && g_handles.empty());
    reset_flags(); g_closed_arity = 6;
    assert(api->spin(&bag, tid, 2000, &sp) == PGR_VX_ERR_REFLECT && g_handles.empty());

    // Partial-root failure: the bag root (1st gc_new) fails -> fail closed before any reflection.
    reset_flags(); g_gcnew_fail_index = 1;
    assert(api->spin(&bag, tid, 2000, &sp) == PGR_VX_ERR_GCHANDLE && g_make_calls == 0 && g_handles.empty());
  }

  // 5. transfer SUCCESS: full 64-bit id, GetPokemon identity, bag GameMaster, OWN handles, no leak.
  { reset_flags();
    const uint64_t big = 0xABCDEF0123456789ULL;  // exercises the full 64 bits
    resp.caught_id = big; pk.id = big; pbag.current = &pk; g_bag_current = &pk;
    std::memset(&tr, 0xee, sizeof(tr));
    assert(api->transfer(&resp, &pbag, tid, 2000, &tr) == PGR_VX_SUCCESS);
    assert(tr.version == 1 && tr.size == sizeof(tr) && tr.caught_id == big);
    assert(g_last_get_id == big && g_get_calls == 1);
    assert(tr.pokemon_handle != 0 && fake_gc_get(tr.pokemon_handle) == &pk);
    assert(tr.gm_handle != 0 && fake_gc_get(tr.gm_handle) == &g_gm);
    assert(g_handles.size() == 2);
    fake_gc_free(tr.pokemon_handle); fake_gc_free(tr.gm_handle);
    assert(g_handles.empty());
  }

  // 6. transfer NOT-READY / SKIP / errors.
  { reset_flags();
    // No inventory yet: GetPokemon returns null -> 0 (retry later).
    resp.caught_id = 42; pbag.current = nullptr; g_bag_current = nullptr;
    assert(api->transfer(&resp, &pbag, tid, 2000, &tr) == PGR_VX_OK && tr.pokemon_handle == 0);
    assert(g_handles.empty());

    // Captured id 0 -> SKIP, GetPokemon never called.
    reset_flags(); resp.caught_id = 0;
    assert(api->transfer(&resp, &pbag, tid, 2000, &tr) == PGR_VX_SKIP && g_get_calls == 0 && g_handles.empty());

    // Identity mismatch: pokemon id != captured id -> SKIP.
    reset_flags(); resp.caught_id = 100; pk.id = 101; pbag.current = &pk; g_bag_current = &pk;
    assert(api->transfer(&resp, &pbag, tid, 2000, &tr) == PGR_VX_SKIP && g_handles.empty());

    // Wrong pokemon class -> SKIP.
    reset_flags(); FakePokemon imposter{&cls_other, 100}; resp.caught_id = 100; g_bag_current = &imposter;
    assert(api->transfer(&resp, &pbag, tid, 2000, &tr) == PGR_VX_SKIP && g_handles.empty());

    // Wrong response class -> IDENTITY error.
    reset_flags(); FakeResponse bad_resp{&cls_other, 7};
    assert(api->transfer(&bad_resp, &pbag, tid, 2000, &tr) == PGR_VX_ERR_IDENTITY && g_handles.empty());

    // GetPokemon throws -> MANAGED error.
    reset_flags(); resp.caught_id = 55; pk.id = 55; g_bag_current = &pk; g_throw_on_get = true;
    assert(api->transfer(&resp, &pbag, tid, 2000, &tr) == PGR_VX_ERR_MANAGED && g_handles.empty());

    // Null bag GameMaster after a valid identity -> IDENTITY error.
    reset_flags(); resp.caught_id = 77; pk.id = 77; g_bag_current = &pk;
    FakePokeBag no_gm{&cls_other, nullptr, &pk};
    assert(api->transfer(&resp, &no_gm, tid, 2000, &tr) == PGR_VX_ERR_IDENTITY && g_handles.empty());
  }

  // 7. Gate: wrong thread / zero owner / zero lease refuse without any managed work.
  { reset_flags();
    resp.caught_id = 9; pk.id = 9; pbag.current = &pk; g_bag_current = &pk; pbag.gm = &g_gm;
    assert(api->spin(&bag, tid, 0, &sp) == PGR_VX_OK && g_make_calls == 0);          // zero lease
    assert(api->spin(&bag, 0, 2000, &sp) == PGR_VX_OK && g_make_calls == 0);         // zero owner
    assert(api->transfer(&resp, &pbag, tid + 1, 2000, &tr) == PGR_VX_OK && g_get_calls == 0);  // wrong tid
    std::thread([&]() {
      PgrVgpAuxSpin s2{};
      assert(g_api->spin(&bag, this_tid() + 1, 2000, &s2) == PGR_VX_OK);  // owner mismatch off-thread
    }).join();
    assert(g_handles.empty());
  }

  // 8. Expiry: the foreground lease is spent DURING the managed work -> refused, nothing transferred.
  { reset_flags();
    resp.caught_id = 123; pk.id = 123; g_bag_current = &pk; pbag.gm = &g_gm;
    g_get_oneshot = [&]() { ::usleep(4000); };  // outlast the 1 ms lease
    assert(api->transfer(&resp, &pbag, tid, 1, &tr) == PGR_VX_OK && tr.pokemon_handle == 0);
    assert(g_handles.empty());
  }

  // 9. Reentry: a nested op fired from inside a managed invoke is refused (busy); the outer completes once.
  { reset_flags();
    resp.caught_id = 321; pk.id = 321; g_bag_current = &pk; pbag.gm = &g_gm;
    int nested_rc = -99;
    g_get_oneshot = [&]() {
      PgrVgpAuxSpin s2{};
      nested_rc = g_api->spin(&bag, this_tid(), 2000, &s2);  // busy -> refused
      assert(s2.rpc_handle == 0);
    };
    assert(api->transfer(&resp, &pbag, tid, 2000, &tr) == PGR_VX_SUCCESS);
    assert(nested_rc == PGR_VX_OK && g_make_calls == 0);  // nested refused, no reflection issued
    fake_gc_free(tr.pokemon_handle); fake_gc_free(tr.gm_handle);
    assert(g_handles.empty());
  }

  std::printf("vgp_aux_test passed: ABI/validation/idempotence/reconfig, spin reflection SUCCESS (Type[] "
              "writes, inflated closed method, OWN rpc+reflection handles, no leak), relocation re-resolve, "
              "reflection/identity/throw/null/closed-method/root failures, transfer full-64 id SUCCESS, "
              "no-inventory refuse, captured-id-0/identity SKIP, response/class/gm errors, gate (thread/"
              "owner/lease), expiry mid-work, reentry busy guard\n");
  return 0;
}

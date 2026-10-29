#include "details_recovery.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <unistd.h>

// Production feature 38 ABI/behaviour harness (native post-catch DETAILS recovery). It resolves the
// table through the exported pgrazor_native_api v2 registry (query_feature(38,1)) and drives the
// controller against a fake IL2CPP heap: a details scene (gui/view/response/data/pokemon/action) plus a
// one-element backing list, a static op_Implicit "alive" probe returning a boxed System.Boolean, a
// list.count() returning a boxed System.Int32, and a list.item(int) returning the pokemon. Feature 38
// install() installs feature 37 with the SAME config (the prerequisite validator), and recover()'s final
// snapshot calls feature 37 validate() -- both read this same fake metadata. The GC is moving:
// alive/count/item can relocate the whole scene, and every native root is re-resolved afterwards. No
// hook, no real game method; the module installs NO hook and NEVER calls back into JS. The coordinator
// builds and runs this against libpgrazor.so after registering query_feature case 38 and linking
// details_recovery.cpp (and details_completion.cpp).

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class / field / method tokens (distinct addresses) --------------------------
int guiClass, viewClass, responseClass, dataClass, pokemonClass, actionClass;
int boolClass, intClass, listClass, otherClass;
int f_current_state, f_status, f_caught_id, f_view_response, f_pokemon_id, f_delegate_target;
int f_gui_data, f_gui_callback, f_data_callback, f_post, f_index, f_list;
int m_alive, m_click, m_count, m_item;

constexpr int32_t kSuccess = 3;    // arbitrary non-zero CatchSuccess literal
constexpr int32_t kStateOpen = 2;  // DGUI currentState == STATE_OPEN
constexpr int32_t kOpening = 1;    // a still-opening canvas (recovery tolerates it)

// ----- fake managed objects (first member is always the Il2CppClass*) --------------
struct Gui { void *klass; int32_t state; void *delegate; void *data; void *callback; };
struct View { void *klass; void *response; };
struct Response { void *klass; int32_t status; uint8_t id[8]; };
struct Data { void *klass; void *on_dismissed; uint8_t post; int32_t index; void *list; };
struct Pokemon { void *klass; uint8_t id[8]; };
struct Action { void *klass; void *target; };
struct List { void *klass; int32_t count; void *item0; };
struct Scene { Gui gui; View view; Response response; Data data; Pokemon pokemon; Action action; List list; };
struct BoolBox { void *klass; uint8_t value; };
struct IntBox { void *klass; int32_t value; };

Scene scenes[8];
int currentScene = 0;
Scene &cur() { return scenes[currentScene]; }
BoolBox g_bool{&boolClass, 1};
IntBox g_int{&intClass, 1};
Pokemon g_stray{&pokemonClass, {}};  // a Pokemon that is NOT the one in the scene list

// ----- runtime shims ----------------------------------------------------------------
const PgrDetailsRecoveryApi *g_api = nullptr;
uint64_t g_owner = 0;

bool g_move_alive = false, g_move_count = false, g_move_item = false;
bool g_live = true;               // unboxed Boolean op_Implicit returns
int32_t g_count_value = 1;        // list.count()
void *g_item_override = nullptr;  // force item(0) to return a non-matching object
int g_new_calls = 0, g_fail_new = 0;
bool g_alloc_failed = false;

std::map<uintptr_t, void *> g_handles;  // all live GC handles -> target
std::set<uintptr_t> g_host;             // borrowed (host-owned) handles: native must never free these
uintptr_t g_seq = 0x1000;

void relocate() {
  assert(currentScene < 7);
  Scene &old = scenes[currentScene];
  Scene &next = scenes[++currentScene];
  next = old;
  next.action.target = &next.view; next.gui.data = &next.data; next.gui.callback = &next.action;
  next.view.response = &next.response; next.data.on_dismissed = &next.action; next.data.list = &next.list;
  next.list.item0 = &next.pokemon;
  for (auto &e : g_handles) {
    void *p = e.second;
    if (p == &old.gui) e.second = &next.gui;
    else if (p == &old.view) e.second = &next.view;
    else if (p == &old.response) e.second = &next.response;
    else if (p == &old.data) e.second = &next.data;
    else if (p == &old.pokemon) e.second = &next.pokemon;
    else if (p == &old.action) e.second = &next.action;
    else if (p == &old.list) e.second = &next.list;
  }
  old.gui.klass = old.view.klass = old.response.klass = old.data.klass = &otherClass;
  old.pokemon.klass = old.action.klass = old.list.klass = &otherClass;
}

uintptr_t fake_gc_new(void *o, int) {
  if (++g_new_calls == g_fail_new) { g_alloc_failed = true; return 0; }
  if (!o) return 0;
  g_handles[++g_seq] = o; return g_seq;
}
void *fake_gc_get(uintptr_t h) { auto i = g_handles.find(h); return i == g_handles.end() ? nullptr : i->second; }
void fake_gc_free(uintptr_t h) { assert(!g_host.count(h)); assert(g_handles.erase(h) == 1); }
void *borrow(void *o) { uintptr_t h = ++g_seq; g_handles[h] = o; g_host.insert(h); return reinterpret_cast<void *>(h); }
void balanced() { assert(g_handles.size() == g_host.size()); for (uintptr_t h : g_host) assert(g_handles.count(h)); }

void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }
void *fake_unbox(void *boxed) { return boxed ? static_cast<char *>(boxed) + sizeof(void *) : nullptr; }

void fake_field_get(void *obj, void *field, void *out) {
  assert(obj && fake_obj_class(obj) != &otherClass);  // poisoned copies must never be read
  if (field == &f_current_state) { *static_cast<int32_t *>(out) = static_cast<Gui *>(obj)->state; return; }
  if (field == &f_status) { *static_cast<int32_t *>(out) = static_cast<Response *>(obj)->status; return; }
  if (field == &f_caught_id) { std::memcpy(out, static_cast<Response *>(obj)->id, 8); return; }
  if (field == &f_view_response) { *static_cast<void **>(out) = static_cast<View *>(obj)->response; return; }
  if (field == &f_pokemon_id) { std::memcpy(out, static_cast<Pokemon *>(obj)->id, 8); return; }
  if (field == &f_delegate_target) { *static_cast<void **>(out) = static_cast<Action *>(obj)->target; assert(fake_obj_class(obj)==&actionClass); return; }
  if (field == &f_gui_data) { *static_cast<void **>(out) = static_cast<Gui *>(obj)->data; return; }
  if (field == &f_gui_callback) { *static_cast<void **>(out) = static_cast<Gui *>(obj)->callback; return; }
  if (field == &f_data_callback) { *static_cast<void **>(out) = static_cast<Data *>(obj)->on_dismissed; return; }
  if (field == &f_post) { *static_cast<uint8_t *>(out) = static_cast<Data *>(obj)->post; return; }
  if (field == &f_index) { *static_cast<int32_t *>(out) = static_cast<Data *>(obj)->index; return; }
  if (field == &f_list) { *static_cast<void **>(out) = static_cast<Data *>(obj)->list; return; }
  assert(false && "unknown field token");
}

void *fake_runtime_invoke(const void *method, void *obj, void **params, void **exc) {
  if (exc) *exc = nullptr;
  if (method == &m_alive) {
    assert(obj == nullptr && params && params[0] == &cur().gui);  // reference arg is the GUI, not &ptr
    if (g_move_alive) { g_move_alive = false; relocate(); }
    g_bool.klass = &boolClass;
    g_bool.value = static_cast<uint8_t>(g_live ? 1 : 0);
    return &g_bool;
  }
  if (method == &m_count) {
    assert(obj == &cur().list && params == nullptr);
    if (g_move_count) { g_move_count = false; relocate(); }
    g_int.klass = &intClass; g_int.value = g_count_value;
    return &g_int;
  }
  if (method == &m_item) {
    assert(obj == &cur().list && params && *static_cast<int32_t *>(params[0]) == 0);  // index BY ADDRESS
    if (g_move_item) { g_move_item = false; relocate(); }
    return g_item_override ? g_item_override : &cur().pokemon;
  }
  assert(false && "recovery must never invoke the click");
  return nullptr;
}

PgrDetailsRecoveryConfig make_config() {
  PgrDetailsRecoveryConfig c{};
  c.version = 1; c.size = sizeof(c);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c.object_unbox = reinterpret_cast<void *>(&fake_unbox);
  c.gc_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gc_get = reinterpret_cast<void *>(&fake_gc_get);
  c.gc_free = reinterpret_cast<void *>(&fake_gc_free);
  c.class_gui = &guiClass; c.class_view = &viewClass; c.class_response = &responseClass;
  c.class_data = &dataClass; c.class_pokemon = &pokemonClass; c.class_action = &actionClass;
  c.class_bool = &boolClass; c.class_int = &intClass; c.class_list = &listClass;
  c.m_alive = &m_alive; c.m_click = &m_click; c.m_count = &m_count; c.m_item = &m_item;
  c.f_current_state = &f_current_state; c.f_status = &f_status; c.f_caught_id = &f_caught_id;
  c.f_view_response = &f_view_response; c.f_pokemon_id = &f_pokemon_id; c.f_delegate_target = &f_delegate_target;
  c.f_gui_data = &f_gui_data; c.f_gui_callback = &f_gui_callback; c.f_data_callback = &f_data_callback;
  c.f_post = &f_post; c.f_index = &f_index; c.f_list = &f_list;
  c.success_literal = kSuccess; c.state_open = kStateOpen;
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }
void put_id(uint8_t out[8], uint64_t id) { for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>((id >> (8 * i)) & 0xff); }

uint64_t g_id_seq = 0x5000;
PgrDetailsRecoveryRequest g_req{};

// Rebuild a fully-wired confirmed details scene on scene 0 and borrow ONLY the gui + response handles.
uint64_t build(int32_t state = kStateOpen) {
  assert(g_handles.size() == g_host.size());
  g_handles.clear(); g_host.clear();
  currentScene = 0; g_new_calls = 0; g_fail_new = 0; g_alloc_failed = false;
  g_move_alive = g_move_count = g_move_item = false;
  g_live = true; g_count_value = 1; g_item_override = nullptr;
  const uint64_t id = ++g_id_seq;
  Scene &s = scenes[0];
  s.gui = {&guiClass, state, &s.view, &s.data, &s.action};
  s.view = {&viewClass, &s.response};
  s.response = {&responseClass, kSuccess, {}};
  s.data = {&dataClass, &s.action, 1, 0, &s.list};
  s.pokemon = {&pokemonClass, {}};
  s.action = {&actionClass, &s.view};
  s.list = {&listClass, 1, &s.pokemon};
  put_id(s.response.id, id); put_id(s.pokemon.id, id);
  g_req = {}; g_req.version = 1; g_req.size = sizeof(g_req);
  put_id(g_req.expected_key, id); g_req.epoch = 7; g_req.tid = g_owner; g_req.remaining_ms = 2500;
  g_req.gui = borrow(&s.gui); g_req.response = borrow(&s.response);
  return id;
}

int recover(PgrDetailsRecoveryOutput &out) {
  std::memset(&out, 0, sizeof(out));
  int rc = g_api->recover(&g_req, &out);
  assert(out.version == 1 && out.size == 64 && out.status == rc && out.reserved == 0);
  return rc;
}

// A recovered output: six distinct, nonzero native-owned handles resolving to the scene objects.
void expect_recovered(PgrDetailsRecoveryOutput &out) {
  assert(recover(out) == PGR_DR_RECOVERED);
  void **slots = &out.gui;
  for (int i = 0; i < 6; ++i) assert(slots[i] != nullptr);
  assert(fake_gc_get(reinterpret_cast<uintptr_t>(out.gui)) == &cur().gui);
  assert(fake_gc_get(reinterpret_cast<uintptr_t>(out.state)) == &cur().view);
  assert(fake_gc_get(reinterpret_cast<uintptr_t>(out.response)) == &cur().response);
  assert(fake_gc_get(reinterpret_cast<uintptr_t>(out.data)) == &cur().data);
  assert(fake_gc_get(reinterpret_cast<uintptr_t>(out.pokemon)) == &cur().pokemon);
  assert(fake_gc_get(reinterpret_cast<uintptr_t>(out.dismissed)) == &cur().action);
}

// Any mismatch must refuse (0) with zeroed handles and no leaked native roots.
void expect_refused() {
  PgrDetailsRecoveryOutput out{};
  assert(recover(out) == PGR_DR_OK);
  void **slots = &out.gui;
  for (int i = 0; i < 6; ++i) assert(slots[i] == nullptr);
  balanced();
}

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) { std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror()); return 1; }
  auto getter = reinterpret_cast<GetApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == sizeof(pgrazor_native_api_v2));
  assert(core->query_feature(38, 99) == nullptr);  // exact version only
  const auto *api = static_cast<const PgrDetailsRecoveryApi *>(core->query_feature(38, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrDetailsRecoveryApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu request=%zu output=%zu table=%zu\n",
              sizeof(PgrDetailsRecoveryConfig), sizeof(PgrDetailsRecoveryRequest),
              sizeof(PgrDetailsRecoveryOutput), sizeof(PgrDetailsRecoveryApi));
  assert(sizeof(PgrDetailsRecoveryConfig) == 272 && sizeof(PgrDetailsRecoveryRequest) == 56 &&
         sizeof(PgrDetailsRecoveryOutput) == 64 && sizeof(PgrDetailsRecoveryApi) == 32);
  assert(offsetof(PgrDetailsRecoveryRequest, remaining_ms) == 32 &&
         offsetof(PgrDetailsRecoveryRequest, response) == 48);
  assert(offsetof(PgrDetailsRecoveryOutput, gui) == 16 &&
         offsetof(PgrDetailsRecoveryOutput, dismissed) == 56);

  const uint64_t tid = this_tid();
  g_owner = tid;
  PgrDetailsRecoveryConfig cfg = make_config();
  PgrDetailsRecoveryOutput out{};

  // 1. Config validation, not-installed, idempotence, reconfig.
  assert(api->install(nullptr) == PGR_DR_ERR_NULL_ARG);
  PgrDetailsRecoveryConfig bad = cfg; bad.version = 2;
  assert(api->install(&bad) == PGR_DR_ERR_BAD_VERSION);
  bad = cfg; bad.size = sizeof(cfg) - 1; assert(api->install(&bad) == PGR_DR_ERR_BAD_SIZE);
  bad = cfg; bad.m_item = nullptr; assert(api->install(&bad) == PGR_DR_ERR_MISSING_PTR);
  bad = cfg; bad.f_list = nullptr; assert(api->install(&bad) == PGR_DR_ERR_MISSING_PTR);
  { build();
    assert(api->recover(&g_req, &out) == PGR_DR_ERR_NOT_INSTALLED && out.status == PGR_DR_ERR_NOT_INSTALLED);
    assert(api->release(&out) == PGR_DR_ERR_NOT_INSTALLED); }
  assert(api->install(&cfg) == PGR_DR_OK);
  assert(api->install(&cfg) == PGR_DR_OK);  // idempotent (installs feature 37 once)
  bad = cfg; bad.state_open = 9; assert(api->install(&bad) == PGR_DR_ERR_RECONFIG);
  assert(api->recover(nullptr, nullptr) == PGR_DR_ERR_NULL_ARG);
  assert(api->release(nullptr) == PGR_DR_ERR_NULL_ARG);

  // 2. Valid recovery (incl. while still OPENING): six native handles, host adoption, release frees and
  //    zeros, release is idempotent, roots return balanced.
  for (int32_t st : {kStateOpen, kOpening}) {
    build(st);
    expect_recovered(out);
    // Host adopts: dup each native handle into its own host handle, then native release() in finally.
    void *adopted[6];
    void **slots = &out.gui;
    for (int i = 0; i < 6; ++i) adopted[i] = borrow(fake_gc_get(reinterpret_cast<uintptr_t>(slots[i])));
    assert(api->release(&out) == PGR_DR_OK);
    for (int i = 0; i < 6; ++i) assert(slots[i] == nullptr);        // zeroed
    assert(api->release(&out) == PGR_DR_OK);                        // idempotent no-op
    for (int i = 0; i < 6; ++i) { g_handles.erase(reinterpret_cast<uintptr_t>(adopted[i])); g_host.erase(reinterpret_cast<uintptr_t>(adopted[i])); }
    g_handles.erase(reinterpret_cast<uintptr_t>(g_req.gui)); g_handles.erase(reinterpret_cast<uintptr_t>(g_req.response));
    g_host.erase(reinterpret_cast<uintptr_t>(g_req.gui)); g_host.erase(reinterpret_cast<uintptr_t>(g_req.response));
    balanced();
  }

  // 3. Every identity mismatch -> refused, zeroed handles, balanced roots, no click (invoke asserts).
  { build(); scenes[0].gui.klass = &otherClass; expect_refused(); }                 // gui class
  { build(); scenes[0].response.klass = &otherClass; expect_refused(); }            // response class
  { build(); scenes[0].response.status = kSuccess + 1; expect_refused(); }          // not CatchSuccess
  { build(); scenes[0].response.id[0] ^= 0xff; expect_refused(); }                  // caughtId mismatch
  { build(); scenes[0].pokemon.id[0] ^= 0xff; expect_refused(); }                   // pokemon.id mismatch
  { build(); scenes[0].data.klass = &otherClass; expect_refused(); }                // data class
  { build(); scenes[0].gui.data = &scenes[0].view; expect_refused(); }              // gui.data != data
  { build(); scenes[0].data.on_dismissed = &scenes[0].view; expect_refused(); }     // onDismissed class/wrong
  { build(); scenes[0].gui.callback = &scenes[0].view; expect_refused(); }          // gui.callback != dismissed
  { build(); scenes[0].action.klass = &otherClass; expect_refused(); }             // dismissed class
  { build(); scenes[0].action.target = &scenes[0].data; expect_refused(); }         // delegateTarget != view
  { build(); scenes[0].view.response = &scenes[0].pokemon; expect_refused(); }      // view.response != response
  { build(); scenes[0].data.post = 0; expect_refused(); }                           // ordinary inventory (post != 1)
  { build(); scenes[0].data.index = 1; expect_refused(); }                          // index != 0
  { build(); scenes[0].list.klass = &otherClass; expect_refused(); }                // list wrong class
  { build(); g_count_value = 2; expect_refused(); }                                 // count != 1
  { build(); g_count_value = 0; expect_refused(); }                                 // empty list
  { build(); g_item_override = &g_stray; expect_refused(); }                        // item(0) != pokemon
  { build(); g_live = false; expect_refused(); }                                    // feature 37: gui not alive
  { build(); std::memset(g_req.expected_key, 0, 8);
    std::memset(scenes[0].response.id, 0, 8); std::memset(scenes[0].pokemon.id, 0, 8);
    expect_refused(); }                                                             // zero caught id

  // 4. Gate: zero remaining, over-limit remaining clamps (still recovers), tid mismatch, foreign thread.
  { build(); g_req.remaining_ms = 0; expect_refused(); }
  { build(); g_req.remaining_ms = 1000000; expect_recovered(out); assert(api->release(&out) == PGR_DR_OK);
    g_handles.erase(reinterpret_cast<uintptr_t>(g_req.gui)); g_handles.erase(reinterpret_cast<uintptr_t>(g_req.response));
    g_host.erase(reinterpret_cast<uintptr_t>(g_req.gui)); g_host.erase(reinterpret_cast<uintptr_t>(g_req.response)); balanced(); }
  { build(); g_req.tid = tid + 1; expect_refused(); }
  { build(); g_req.tid = 0; expect_refused(); }
  std::thread([&]() {
    build(); PgrDetailsRecoveryOutput o{};
    assert(g_api->recover(&g_req, &o) == PGR_DR_OK && o.gui == nullptr);  // off-owner refuse
  }).join();

  // 5. Moving GC: alive / count / item each relocate the whole scene mid-call; native roots refresh so
  //    identity still holds, the recovered handles point at the relocated scene, roots stay balanced.
  { build(); g_move_count = g_move_item = g_move_alive = true;
    expect_recovered(out); assert(currentScene == 3);
    assert(api->release(&out) == PGR_DR_OK);
    g_handles.erase(reinterpret_cast<uintptr_t>(g_req.gui)); g_handles.erase(reinterpret_cast<uintptr_t>(g_req.response));
    g_host.erase(reinterpret_cast<uintptr_t>(g_req.gui)); g_host.erase(reinterpret_cast<uintptr_t>(g_req.response)); balanced(); }

  // 6. Root-allocation failures along recover(): each gc_new may fail; every failure must refuse with
  //    balanced roots and zeroed handles (nothing left unrooted / no partial publish).
  { build(); g_new_calls = 0; expect_recovered(out); const int ra = g_new_calls;
    assert(api->release(&out) == PGR_DR_OK);
    g_handles.erase(reinterpret_cast<uintptr_t>(g_req.gui)); g_handles.erase(reinterpret_cast<uintptr_t>(g_req.response));
    g_host.erase(reinterpret_cast<uintptr_t>(g_req.gui)); g_host.erase(reinterpret_cast<uintptr_t>(g_req.response)); balanced();
    assert(ra >= 6);
    for (int n = 1; n <= ra; ++n) { build(); g_fail_new = n;
      PgrDetailsRecoveryOutput o{}; assert(g_api->recover(&g_req, &o) == PGR_DR_OK);
      assert(g_alloc_failed && o.gui == nullptr); balanced(); } }

  g_handles.clear(); g_host.clear();
  std::printf("details_recovery_test passed: ABI/install(+feature37)/idempotence/reconfig/not-installed, "
              "valid recovery (six native handles, opening-canvas, host adoption, release free+zero+idempotent), "
              "identity mismatches (classes/status/caughtId/pokemon.id/gui.data/callback/onDismissed/delegateTarget/"
              "view.response/post-ordinary-inventory/index/list-class/count/item/not-alive/zero-id), gate "
              "(zero/over-limit remaining, tid mismatch, zero tid, foreign thread), moving-GC relocation, "
              "balanced-root alloc failures\n");
  ::dlclose(lib);
  return 0;
}

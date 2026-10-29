#include "details_completion.h"
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

// Production feature 37 ABI/behaviour harness (native post-catch DETAILS completion). It resolves the
// table through the exported pgrazor_native_api v2 registry (query_feature(37,1)) and drives the
// controller against a fake IL2CPP heap of borrowed GC handles: a details scene (gui/view/response/
// data/pokemon/action) plus a one-element backing list, a static op_Implicit "alive" probe returning a
// boxed System.Boolean, a list.count() returning a boxed System.Int32, a list.item(int) returning the
// pokemon, and a details click that advances currentState and can throw or re-enter. The GC is moving:
// alive/count/item/click can relocate the whole scene, and every input/box/list/item target is held in
// a native-owned root and re-resolved afterwards. No original worker harness, no hook, no real game
// method. The module installs NO hook and NEVER calls back into JS; the coordinator builds and runs
// this against libpgrazor.so after registering query_feature case 37 and linking details_completion.cpp.

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
constexpr int32_t kDoneState = 5;  // currentState a click leaves behind (advanced off STATE_OPEN)

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
const PgrDetailsCompletionApi *g_api = nullptr;
uint64_t g_owner = 0;

int g_click_calls = 0;
bool g_move_alive = false, g_move_count = false, g_move_item = false, g_move_click = false;
int g_alive_fault = 0;            // 0 none / 1 throw / 2 wrong boxed class / 3 out-of-range byte
bool g_live = true;              // unboxed Boolean op_Implicit returns
int32_t g_count_value = 1;       // list.count()
bool g_count_wrong_class = false; // count box class != INT
bool g_throw_click = false;      // click reports a thrown managed exception
void *g_item_override = nullptr; // force item(0) to return a non-matching object
std::function<void()> g_on_alive; // fired once inside op_Implicit
std::function<void()> g_on_click; // fired once inside the click
int g_new_calls = 0, g_fail_new = 0;
bool g_alloc_failed = false;

std::map<uintptr_t, void *> g_handles;
std::set<uintptr_t> g_host;
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
  // Poison the abandoned copy; it remains readable but every field is deliberately wrong.
  old.gui.klass = old.view.klass = old.response.klass = old.data.klass = &otherClass;
  old.pokemon.klass = old.action.klass = old.list.klass = &otherClass;
  old.gui.state = 99;
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
    if (g_on_alive) { auto f = g_on_alive; g_on_alive = nullptr; f(); }
    if (g_alive_fault == 1) { if (exc) *exc = reinterpret_cast<void *>(0x2); return nullptr; }
    g_bool.klass = (g_alive_fault == 2) ? &responseClass : &boolClass;
    g_bool.value = (g_alive_fault == 3) ? 7 : static_cast<uint8_t>(g_live ? 1 : 0);
    return &g_bool;
  }
  if (method == &m_count) {
    assert(obj == &cur().list && params == nullptr);
    if (g_move_count) { g_move_count = false; relocate(); }
    g_int.klass = g_count_wrong_class ? &otherClass : &intClass;
    g_int.value = g_count_value;
    return &g_int;
  }
  if (method == &m_item) {
    assert(obj == &cur().list && params && *static_cast<int32_t *>(params[0]) == 0);  // index BY ADDRESS
    if (g_move_item) { g_move_item = false; relocate(); }
    return g_item_override ? g_item_override : &cur().pokemon;
  }
  assert(method == &m_click && obj == &cur().gui && params == nullptr);
  ++g_click_calls;
  if (g_move_click) { g_move_click = false; relocate(); }
  cur().gui.state = kDoneState;
  if (g_on_click) { auto f = g_on_click; g_on_click = nullptr; f(); }
  if (g_throw_click) { g_throw_click = false; if (exc) *exc = reinterpret_cast<void *>(0x1); }
  return nullptr;
}

PgrDetailsCompletionConfig make_config() {
  PgrDetailsCompletionConfig c{};
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
PgrDetailsCompletionRequest g_req{};

// Rebuild a fully-wired confirmed details scene on scene 0 and borrow one host handle per object.
uint64_t build(bool arm = true) {
  assert(g_handles.size() == g_host.size());
  g_handles.clear(); g_host.clear();
  currentScene = 0; g_click_calls = 0; g_new_calls = 0; g_fail_new = 0; g_alloc_failed = false;
  g_move_alive = g_move_count = g_move_item = g_move_click = false;
  g_alive_fault = 0; g_live = true; g_count_value = 1; g_count_wrong_class = false;
  g_throw_click = false; g_item_override = nullptr; g_on_alive = nullptr; g_on_click = nullptr;
  const uint64_t id = ++g_id_seq;
  Scene &s = scenes[0];
  s.gui = {&guiClass, kStateOpen, &s.view, &s.data, &s.action};
  s.view = {&viewClass, &s.response};
  s.response = {&responseClass, kSuccess, {}};
  s.data = {&dataClass, &s.action, 1, 0, &s.list};
  s.pokemon = {&pokemonClass, {}};
  s.action = {&actionClass, &s.view};
  s.list = {&listClass, 1, &s.pokemon};
  put_id(s.response.id, id); put_id(s.pokemon.id, id);
  g_req = {}; g_req.version = 1; g_req.size = sizeof(g_req);
  put_id(g_req.expected_key, id); g_req.epoch = 7; g_req.tid = g_owner;
  g_req.gui = borrow(&s.gui); g_req.view = borrow(&s.view); g_req.response = borrow(&s.response);
  g_req.data = borrow(&s.data); g_req.pokemon = borrow(&s.pokemon); g_req.dismissed = borrow(&s.action);
  if (arm) assert(g_api->update(1, 2500, 7, g_owner) == PGR_DC_OK);
  return id;
}

uint64_t acquire(uint64_t epoch = 7) {
  uint64_t t = 0; assert(g_api->acquire(epoch, g_owner, &t) == PGR_DC_OK); return t;
}
int validate() { return g_api->validate(&g_req); }
int complete(uint64_t tok, PgrDetailsCompletionOutput &out) {
  std::memset(&out, 0, sizeof(out)); int rc = g_api->complete(&g_req, tok, &out); balanced(); return rc;
}
PgrDetailsCompletionSample sample() {
  PgrDetailsCompletionSample s{}; assert(g_api->sample(&s) == PGR_DC_OK);
  assert(s.version == 1 && s.size == sizeof(s)); return s;
}
// Each mismatch must make validate INVALID and complete REFUSED without ever clicking.
void expect_rejected() {
  assert(validate() == PGR_DC_OK);
  uint64_t t = acquire(); assert(t != 0);
  PgrDetailsCompletionOutput out{};
  assert(complete(t, out) == PGR_DC_OK && !(out.flags & PGR_DC_FLAG_INVOKED));
  assert(g_click_calls == 0);
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
  assert(core->query_feature(37, 99) == nullptr);
  const auto *api = static_cast<const PgrDetailsCompletionApi *>(core->query_feature(37, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrDetailsCompletionApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu request=%zu output=%zu sample=%zu table=%zu\n",
              sizeof(PgrDetailsCompletionConfig), sizeof(PgrDetailsCompletionRequest),
              sizeof(PgrDetailsCompletionOutput), sizeof(PgrDetailsCompletionSample),
              sizeof(PgrDetailsCompletionApi));
  assert(sizeof(PgrDetailsCompletionConfig) == 272 && sizeof(PgrDetailsCompletionRequest) == 80 &&
         sizeof(PgrDetailsCompletionOutput) == 24 && sizeof(PgrDetailsCompletionSample) == 72 &&
         sizeof(PgrDetailsCompletionApi) == 64);
  assert(offsetof(PgrDetailsCompletionConfig, f_list) == 256 &&
         offsetof(PgrDetailsCompletionConfig, state_open) == 268);
  assert(offsetof(PgrDetailsCompletionRequest, dismissed) == 72);

  const uint64_t tid = this_tid();
  g_owner = tid;
  PgrDetailsCompletionConfig cfg = make_config();
  PgrDetailsCompletionOutput out{};

  // 1. Config validation, not-installed, idempotence, reconfig, missing output buffer.
  assert(api->install(nullptr) == PGR_DC_ERR_NULL_ARG);
  PgrDetailsCompletionConfig bad = cfg; bad.version = 2;
  assert(api->install(&bad) == PGR_DC_ERR_BAD_VERSION);
  bad = cfg; bad.size = sizeof(cfg) - 1; assert(api->install(&bad) == PGR_DC_ERR_BAD_SIZE);
  bad = cfg; bad.m_click = nullptr; assert(api->install(&bad) == PGR_DC_ERR_MISSING_PTR);
  bad = cfg; bad.f_list = nullptr; assert(api->install(&bad) == PGR_DC_ERR_MISSING_PTR);
  { build(/*arm=*/false);
    assert(api->validate(&g_req) == PGR_DC_ERR_NOT_INSTALLED);
    assert(api->complete(&g_req, 1, &out) == PGR_DC_ERR_NOT_INSTALLED && out.status == PGR_DC_ERR_NOT_INSTALLED); }
  assert(api->install(&cfg) == PGR_DC_OK);
  assert(api->install(&cfg) == PGR_DC_OK);  // idempotent
  bad = cfg; bad.state_open = 9; assert(api->install(&bad) == PGR_DC_ERR_RECONFIG);
  assert(api->complete(nullptr, 1, nullptr) == PGR_DC_ERR_NULL_ARG);

  // 2. Valid details: validate VALID, acquire, complete DONE, click once, flags, before/after.
  { build(); assert(validate() == PGR_DC_DONE);
    uint64_t t = acquire(); assert(t != 0);
    assert(complete(t, out) == PGR_DC_DONE && g_click_calls == 1);
    assert(out.version == 1 && out.size == 24 && out.status == PGR_DC_DONE);
    assert((out.flags & PGR_DC_FLAG_INVOKED) && (out.flags & PGR_DC_FLAG_BEFORE_KNOWN));
    assert((out.flags & PGR_DC_FLAG_AFTER_KNOWN) && !(out.flags & PGR_DC_FLAG_GONE));
    assert(out.before == kStateOpen && out.after == kDoneState);
    PgrDetailsCompletionSample sm = sample();
    assert(sm.validate_ok >= 1 && sm.complete_ok == 1 && sm.last_token == t && sm.busy == 0); }

  // 2b. transfer-only independence: there is NO dismissEnabled gate, so an auto-transfer-only session
  //     (the host never sets dismissEnabled) completes the details identically. Nothing extra to arm.
  { build(); uint64_t t = acquire(); assert(t != 0);
    assert(complete(t, out) == PGR_DC_DONE && g_click_calls == 1); }

  // 3. Every identity mismatch -> INVALID + REFUSED, no click. Covers the ordinary-inventory rejection
  //    (post != 1) and the list count/item/invalid-callback/id/class mutations.
  { build(); scenes[0].gui.klass = &otherClass; expect_rejected(); }                         // gui class
  { build(); scenes[0].view.klass = &otherClass; expect_rejected(); }                        // view class
  { build(); scenes[0].response.klass = &otherClass; expect_rejected(); }                    // response class
  { build(); scenes[0].data.klass = &otherClass; expect_rejected(); }                        // data class
  { build(); scenes[0].pokemon.klass = &otherClass; expect_rejected(); }                     // pokemon class
  { build(); scenes[0].action.klass = &otherClass; expect_rejected(); }                      // action class
  { build(); scenes[0].view.response = &scenes[0].pokemon; expect_rejected(); }              // view.response != response
  { build(); scenes[0].response.status = kSuccess + 1; expect_rejected(); }                  // not CatchSuccess
  { build(); scenes[0].response.id[0] ^= 0xff; expect_rejected(); }                          // caughtId mismatch
  { build(); scenes[0].pokemon.id[0] ^= 0xff; expect_rejected(); }                           // pokemon.id mismatch
  { build(); scenes[0].action.target = &scenes[0].data; expect_rejected(); }                  // delegateTarget != view
  { build(); scenes[0].gui.data = &scenes[0].view; expect_rejected(); }                      // gui.data != data
  { build(); scenes[0].gui.callback = &scenes[0].view; expect_rejected(); }                  // gui.callback != dismissed
  { build(); scenes[0].data.on_dismissed = &scenes[0].view; expect_rejected(); }             // data.onDismissed != dismissed
  { build(); scenes[0].data.post = 0; expect_rejected(); }                                   // ordinary inventory (post != 1)
  { build(); scenes[0].data.post = 2; expect_rejected(); }                                   // out-of-range post byte
  { build(); scenes[0].data.index = 1; expect_rejected(); }                                  // index != 0
  { build(); scenes[0].list.klass = &otherClass; expect_rejected(); }                        // list wrong class
  { build(); g_count_value = 2; expect_rejected(); }                                         // count != 1
  { build(); g_count_value = 0; expect_rejected(); }                                         // empty list
  { build(); g_count_wrong_class = true; expect_rejected(); }                                // count box not Int32
  { build(); g_item_override = &g_stray; expect_rejected(); }                                // item(0) != pokemon
  { build(); std::memset(g_req.expected_key, 0, 8);
    std::memset(scenes[0].response.id, 0, 8); std::memset(scenes[0].pokemon.id, 0, 8);
    expect_rejected(); }                                                                     // zero caught id
  { build(); g_live = false; expect_rejected(); }                                            // gui not alive
  { build(); g_alive_fault = 1; expect_rejected(); }                                         // alive throws
  { build(); g_alive_fault = 2; expect_rejected(); }                                         // alive wrong boxed class
  { build(); g_alive_fault = 3; expect_rejected(); }                                         // alive out-of-range byte

  // 4. Validation during an OPENING canvas: validate is tolerant (no STATE_OPEN gate), but complete
  //    requires STATE_OPEN and refuses without clicking.
  { build(); scenes[0].gui.state = 1;  // still opening
    assert(validate() == PGR_DC_DONE);
    uint64_t t = acquire(); assert(t != 0);
    assert(complete(t, out) == PGR_DC_OK && g_click_calls == 0 && !(out.flags & PGR_DC_FLAG_INVOKED)); }

  // 5. Gate: off / wrong epoch / expired lease -> no token and complete refused.
  { build(); assert(api->update(0, 0, 7, tid) == PGR_DC_OK);
    assert(acquire() == 0);
    assert(complete(999, out) == PGR_DC_OK && !(out.flags & PGR_DC_FLAG_INVOKED));
    assert(api->update(1, 2000, 8, tid) == PGR_DC_OK);
    assert(acquire(7) == 0);  // epoch mismatch
    uint64_t t = acquire(8); assert(t != 0);
    g_req.epoch = 7;  // request epoch 7 != armed 8
    assert(complete(t, out) == PGR_DC_OK && g_click_calls == 0);
    g_req.epoch = 8; }
  { build(); assert(api->update(1, 1, 7, tid) == PGR_DC_OK); ::usleep(3000);
    assert(acquire() == 0); }

  // 6. Repeated / older token: a token completes at most once.
  { build(); uint64_t t1 = acquire(); uint64_t t2 = acquire(); assert(t2 == t1 + 1);
    assert(complete(t2, out) == PGR_DC_DONE && g_click_calls == 1);
    scenes[0].gui.state = kStateOpen;  // reset for refused retries
    assert(complete(t2, out) == PGR_DC_OK && g_click_calls == 1);  // reused
    assert(complete(t1, out) == PGR_DC_OK && g_click_calls == 1);  // older
    uint64_t t3 = acquire(); assert(t3 == t2 + 1);
    assert(complete(t3, out) == PGR_DC_DONE && g_click_calls == 2); }

  // 7. Click exception -> ERR_MANAGED, invoked once, postcondition still read.
  { build(); uint64_t t = acquire(); g_throw_click = true;
    assert(complete(t, out) == PGR_DC_ERR_MANAGED && g_click_calls == 1);
    assert((out.flags & PGR_DC_FLAG_INVOKED) && out.status == PGR_DC_ERR_MANAGED);
    assert((out.flags & PGR_DC_FLAG_AFTER_KNOWN) && out.after == kDoneState);
    assert(sample().complete_fault >= 1 && sample().busy == 0); }

  // 8. Postcondition "gone": the click leaves the gui not-alive.
  { build(); uint64_t t = acquire();
    g_on_click = [](){ g_live = false; };
    assert(complete(t, out) == PGR_DC_DONE);
    assert((out.flags & PGR_DC_FLAG_INVOKED) && (out.flags & PGR_DC_FLAG_GONE));
    assert(!(out.flags & PGR_DC_FLAG_AFTER_KNOWN) && out.before == kStateOpen); }

  // 8b. Postcondition UNKNOWN (never GONE): a post-invoke class mismatch, or a throwing post alive
  //     probe, leaves after unknown and gone CLEAR -- an inability to read is not proof of destruction.
  { build(); uint64_t t = acquire();
    g_on_click = [](){ scenes[currentScene].gui.klass = &otherClass; };  // class invalid after the click
    assert(complete(t, out) == PGR_DC_DONE && g_click_calls == 1);
    assert((out.flags & PGR_DC_FLAG_INVOKED) && !(out.flags & PGR_DC_FLAG_AFTER_KNOWN) &&
           !(out.flags & PGR_DC_FLAG_GONE)); }
  { build(); uint64_t t = acquire();
    g_on_click = [](){ g_alive_fault = 1; };  // the post-invoke alive probe throws
    assert(complete(t, out) == PGR_DC_DONE && g_click_calls == 1);
    assert((out.flags & PGR_DC_FLAG_INVOKED) && !(out.flags & PGR_DC_FLAG_AFTER_KNOWN) &&
           !(out.flags & PGR_DC_FLAG_GONE)); }

  // 9. Reentrancy inside the click: nested acquire gets no token (busy), nested complete is refused,
  //    and a nested stop() cannot cause a second click. The outer click happens exactly once.
  { build(); uint64_t t = acquire();
    bool reentrant_token = true; int reentrant_complete = -1;
    g_on_click = [&]() {
      uint64_t nested = 7; assert(g_api->acquire(7, g_owner, &nested) == PGR_DC_OK);
      reentrant_token = (nested != 0);
      PgrDetailsCompletionOutput nout{};
      reentrant_complete = g_api->complete(&g_req, t, &nout);
      g_api->stop(g_owner); };
    assert(complete(t, out) == PGR_DC_DONE && g_click_calls == 1);
    assert(!reentrant_token && reentrant_complete == PGR_DC_OK);
    assert(acquire() == 0); }  // gate revoked by the in-click stop()

  // 10. Wrong thread cannot validate, acquire, or complete.
  std::thread([&]() {
    build(/*arm=*/false);
    assert(g_api->validate(&g_req) == PGR_DC_OK);  // off-owner validate -> 0
    uint64_t t = 5; assert(g_api->acquire(7, this_tid(), &t) == PGR_DC_OK && t == 0);
    PgrDetailsCompletionOutput o{};
    assert(g_api->complete(&g_req, 1, &o) == PGR_DC_OK && !(o.flags & PGR_DC_FLAG_INVOKED));
  }).join();
  { build(); g_req.tid = tid + 1;  // mismatched proven_tid
    uint64_t t = 5; assert(api->acquire(7, tid + 1, &t) == PGR_DC_OK && t == 0);
    assert(complete(1, out) == PGR_DC_OK && !(out.flags & PGR_DC_FLAG_INVOKED)); }

  // 11. stop(): immediate revoke; a token acquired before stop no longer completes.
  { build(); uint64_t t = acquire(); assert(api->stop(tid) == PGR_DC_OK);
    assert(complete(t, out) == PGR_DC_OK && g_click_calls == 0);
    assert(api->update(1, 2000, 7, tid) == PGR_DC_OK);
    uint64_t t2 = acquire(); assert(complete(t2, out) == PGR_DC_DONE && g_click_calls == 1);
    api->stop(tid); assert(validate() == PGR_DC_DONE); }  // validation stays available after stop

  // 12. Moving GC: alive / count / item / click each relocate the whole scene mid-call; the native
  //     roots must refresh so identity holds, the click fires once, and roots stay balanced.
  { build(); g_move_alive = g_move_count = g_move_item = true;
    assert(validate() == PGR_DC_DONE && currentScene == 3); balanced(); }
  { build(); g_move_alive = g_move_count = g_move_item = g_move_click = true;
    uint64_t t = acquire();
    assert(complete(t, out) == PGR_DC_DONE && g_click_calls == 1 && currentScene == 4);
    assert((out.flags & PGR_DC_FLAG_AFTER_KNOWN) && out.after == kDoneState); }

  // 13. Allocation (root) failures: every gc_new along validate() / complete() can fail, and each must
  //     fail closed with balanced roots and no click -- proving nothing is left unrooted.
  { build(); g_new_calls = 0; assert(validate() == PGR_DC_DONE); const int va = g_new_calls; balanced();
    assert(va >= 7);  // 6 input duplicates + the list + boxes/item
    for (int n = 1; n <= va; ++n) { build(); g_fail_new = n;
      assert(validate() != PGR_DC_DONE && g_alloc_failed && g_click_calls == 0); balanced(); } }
  { build(); uint64_t t = acquire(); g_new_calls = 0;
    assert(complete(t, out) == PGR_DC_DONE); const int ca = g_new_calls; balanced(); assert(ca >= 7);
    for (int n = 1; n <= ca; ++n) { build(); uint64_t tn = acquire(); g_fail_new = n;
      PgrDetailsCompletionOutput failed{}; g_api->complete(&g_req, tn, &failed); balanced();
      assert(g_alloc_failed && g_click_calls <= 1);
      assert(!(failed.flags & PGR_DC_FLAG_GONE)); } }  // a failed root is not proof of destruction

  { build(); api->stop(tid); }  // leave the gate revoked
  balanced(); g_handles.clear(); g_host.clear();
  std::printf("details_completion_test passed: ABI/validation/idempotence/reconfig, valid details "
              "(validate+acquire+complete, single click, before/after), identity mismatches "
              "(classes/view.response/status/caughtId/pokemon.id/delegateTarget/data/callback/onDismissed/"
              "post!=1 ordinary-inventory/index/list-class/count/item/zero-id/alive false+faults), "
              "validate-during-opening (no STATE_OPEN gate) vs complete STATE_OPEN, transfer-only, "
              "gate off/epoch/expired, repeated/older token, click exception (ERR_MANAGED), postcondition "
              "gone, reentrancy, wrong thread, stop revoke, moving-GC relocation, balanced-root alloc "
              "failures\n");
  ::dlclose(lib);
  return 0;
}

#include "incoming_gifts.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <set>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// Real-API fake-IL2CPP harness for feature 17 (incoming gift-open controller). It resolves the
// table through the exported pgrazor_native_api v2 registry (query_feature(17,1)) and drives the
// controller against a fake managed heap (classes, methods, fields, boxed-bool returns and
// System.String objects). The module installs NO hook, so there is no Dobby surface to exercise.
// No test claims are made here; the coordinator builds and runs this against libpgrazor.so after
// registering query_feature case 17.

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class / method / field tokens (distinct addresses) -------------------------
int cls_gui, cls_details, cls_open_out, cls_sequence, cls_bool, cls_string, cls_selectable, cls_other;
int m_open, m_active, m_interactable, m_alive;
int f_current_state, f_open_details, f_sender, f_open_button, f_seq_gui, f_seq_response, f_seq_state,
    f_open_result;

constexpr int32_t kStateOpen = 2;
constexpr int32_t kOpenSuccess = 5;

// ----- fake managed objects --------------------------------------------------------
struct FakeString { void *klass; int len; };
struct FakeSelectable { void *klass; uint8_t active; uint8_t interactable; };
struct FakeDetails { void *klass; FakeString *sender; };
struct FakeGui { void *klass; int32_t current_state; FakeDetails *details; FakeSelectable *button; };
struct FakeResponse { void *klass; int32_t result; };
struct FakeSequence { void *klass; FakeGui *gui; FakeResponse *response; int32_t state; };

struct FakeBox { void *klass; uint8_t b; };
FakeBox g_box_pool[16];
int g_box_i = 0;
FakeBox *box_bool(bool v) {
  FakeBox *b = &g_box_pool[g_box_i++ & 15];
  b->klass = &cls_bool;
  b->b = v ? 1 : 0;
  return b;
}

// ----- runtime shims ----------------------------------------------------------------
const PgrIncomingGiftsApi *g_api = nullptr;
uint64_t g_owner = 0;
int g_open_invokes = 0;
bool g_open_throws = false;
bool g_active_disables = false;   // on get_isActiveAndEnabled: disable once (revokes before invoke)
bool g_alive_nested_tick = false; // on op_Implicit: re-enter a stopping tick once (deferred teardown)
bool g_close_on_active = false;   // on get_isActiveAndEnabled: reentrantly CLOSE the owned GUI once
bool g_relocate_on_alive = false; // on op_Implicit: relocate the owned GUI object once (moving GC)
void *g_cur_gui = nullptr;        // the GUI currently driven (reentrant close / relocation target)

std::set<void *> g_destroyed;  // objects for which op_Implicit returns false

bool g_fail_root = false;
uintptr_t g_next_handle = 0x4000;
std::unordered_map<uintptr_t, void *> g_live;

bool g_rearm_on_free = false;  // on a matching gchandle_free: reentrantly arm a NEW ready GUI once
void *g_rearm_gui = nullptr;   // the GUI to arm from inside the reentrant free
void *g_rearm_free_target = nullptr;  // only fire the free re-arm on this object (null = any free)
FakeGui *g_reloc_storage = nullptr;  // heap copy the owned GUI relocates to

bool g_rearm_on_new = false;        // on the gc_new of a specific obj: reentrantly arm a NEW READY
void *g_rearm_new_target = nullptr; // the obj whose gc_new triggers the reentrant READY
bool g_disable_on_seq_new = false;  // on the gc_new of a sequence obj: reentrantly disable+reenable
bool g_close_ready_on_alive = false;   // on op_Implicit(g_cur_gui): reentrant CLOSE then new READY
bool g_close_ready_on_seq_state = false; // on the f_seq_state read: reentrant CLOSE then new READY

uintptr_t fake_gc_new(void *obj, int) {
  if (obj == nullptr || g_fail_root) return 0;
  uintptr_t id = g_next_handle++;
  g_live[id] = obj;
  if (g_rearm_on_new && obj == g_rearm_new_target && g_rearm_gui != nullptr) {
    // A reentrant READY nested inside the durable-root gc_new of an outer READY (the "READY during
    // gc_new of READY" case): the outer arm must drop its own root and let this newer arm win.
    g_rearm_on_new = false;
    g_api->observe(PGR_GIFT_OBSERVE_READY, g_rearm_gui, g_owner);
  }
  if (g_disable_on_seq_new && obj != nullptr && *static_cast<void **>(obj) == &cls_sequence) {
    // A disable+reenable straddling the sequence allocation: because the gen is pinned BEFORE the
    // allocation, the re-enabled gate must still revoke this write (no skip).
    g_disable_on_seq_new = false;
    g_api->update(0, 0);
    g_api->update(1, 2000);
  }
  return id;
}
void *fake_gc_get(uintptr_t h) {
  auto it = g_live.find(h);
  return it != g_live.end() ? it->second : nullptr;
}
void fake_gc_free(uintptr_t h) {
  auto it = g_live.find(h);
  assert(it != g_live.end() && "double or invalid gchandle_free");
  void *freed = it->second;
  g_live.erase(it);
  if (g_rearm_on_free && g_rearm_gui != nullptr &&
      (g_rearm_free_target == nullptr || freed == g_rearm_free_target)) {
    // A reentrant READY nested inside a durable-root free (the "ready can reenter during rootfree"
    // case). The erase already happened, so the re-arm allocates a fresh handle. Gated on the freed
    // object so a stop test fires on the teardown free, not an earlier boxed-bool free.
    g_rearm_on_free = false;
    g_api->observe(PGR_GIFT_OBSERVE_READY, g_rearm_gui, g_owner);
  }
}
void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }
void *fake_unbox(void *obj) { return &static_cast<FakeBox *>(obj)->b; }
int fake_string_len(void *s) { return static_cast<FakeString *>(s)->len; }
uint16_t *fake_string_chars(void *) { static uint16_t z = 0; return &z; }

void fake_field_get(void *obj, void *field, void *out) {
  if (field == &f_current_state) { *static_cast<int32_t *>(out) = static_cast<FakeGui *>(obj)->current_state; return; }
  if (field == &f_open_details) { *static_cast<void **>(out) = static_cast<FakeGui *>(obj)->details; return; }
  if (field == &f_sender) { *static_cast<void **>(out) = static_cast<FakeDetails *>(obj)->sender; return; }
  if (field == &f_open_button) { *static_cast<void **>(out) = static_cast<FakeGui *>(obj)->button; return; }
  if (field == &f_seq_gui) { *static_cast<void **>(out) = static_cast<FakeSequence *>(obj)->gui; return; }
  if (field == &f_seq_response) { *static_cast<void **>(out) = static_cast<FakeSequence *>(obj)->response; return; }
  if (field == &f_seq_state) {
    if (g_close_ready_on_seq_state && g_cur_gui != nullptr) {
      // Reentrant CLOSE+READY mid-lookup of the sequence state: the final epoch/automated recheck
      // before the field_set must abort the skip (never mutate a replacement sequence).
      g_close_ready_on_seq_state = false;
      g_api->observe(PGR_GIFT_OBSERVE_CLOSE, g_cur_gui, g_owner);
      if (g_rearm_gui != nullptr) g_api->observe(PGR_GIFT_OBSERVE_READY, g_rearm_gui, g_owner);
    }
    *static_cast<int32_t *>(out) = static_cast<FakeSequence *>(obj)->state; return;
  }
  if (field == &f_open_result) { *static_cast<int32_t *>(out) = static_cast<FakeResponse *>(obj)->result; return; }
  assert(false && "unknown field token");
}
void fake_field_set(void *obj, void *field, void *value) {
  assert(field == &f_seq_state && "only the sequence state is written");
  static_cast<FakeSequence *>(obj)->state = *static_cast<int32_t *>(value);
}

void *fake_runtime_invoke(const void *method, void *obj, void **params, void **exc) {
  if (exc) *exc = nullptr;
  if (method == &m_alive) {
    if (g_alive_nested_tick) {
      g_alive_nested_tick = false;
      g_api->tick(g_owner, 1);  // reentrant stopping tick: revokes now, defers teardown to outer
    }
    if (g_close_ready_on_alive && params[0] == g_cur_gui && g_cur_gui != nullptr) {
      // Reentrant CLOSE of the owned GUI then a fresh READY, both DURING the initial alive getter.
      // The outer tick pass must abandon (epoch changed) WITHOUT clearing the freshly-armed slot.
      g_close_ready_on_alive = false;
      g_api->observe(PGR_GIFT_OBSERVE_CLOSE, g_cur_gui, g_owner);
      if (g_rearm_gui != nullptr) g_api->observe(PGR_GIFT_OBSERVE_READY, g_rearm_gui, g_owner);
    }
    if (g_relocate_on_alive && params[0] == g_cur_gui && g_reloc_storage != nullptr) {
      // Moving GC: copy the owned GUI to a new address and repoint its root, then poison the stale
      // original so any code that reused the pre-move pointer would fail the open-state check.
      g_relocate_on_alive = false;
      *g_reloc_storage = *static_cast<FakeGui *>(g_cur_gui);
      for (auto &kv : g_live)
        if (kv.second == g_cur_gui) kv.second = g_reloc_storage;
      static_cast<FakeGui *>(g_cur_gui)->current_state = 99;  // poison the stale copy
      g_cur_gui = g_reloc_storage;
    }
    void *o = params[0];  // reference arg: the object pointer itself
    return box_bool(o != nullptr && g_destroyed.find(o) == g_destroyed.end());
  }
  if (method == &m_active) {
    if (g_active_disables) { g_active_disables = false; g_api->update(0, 0); }
    if (g_close_on_active && g_cur_gui != nullptr) {
      // Reentrant CLOSE of the owned GUI mid-tick: the final gc_get refresh must see the slot gone.
      g_close_on_active = false;
      g_api->observe(PGR_GIFT_OBSERVE_CLOSE, g_cur_gui, g_owner);
    }
    return box_bool(static_cast<FakeSelectable *>(obj)->active != 0);
  }
  if (method == &m_interactable) return box_bool(static_cast<FakeSelectable *>(obj)->interactable != 0);
  if (method == &m_open) {
    ++g_open_invokes;
    if (g_open_throws) { if (exc) *exc = &m_open; return nullptr; }
    return nullptr;
  }
  assert(false && "unknown method token");
  return nullptr;
}

PgrIncomingGiftsConfig make_config() {
  PgrIncomingGiftsConfig c{};
  c.version = 1;
  c.size = sizeof(PgrIncomingGiftsConfig);
  c.gchandle_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gchandle_get_target = reinterpret_cast<void *>(&fake_gc_get);
  c.gchandle_free = reinterpret_cast<void *>(&fake_gc_free);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.object_unbox = reinterpret_cast<void *>(&fake_unbox);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.field_set_value = reinterpret_cast<void *>(&fake_field_set);
  c.string_length = reinterpret_cast<void *>(&fake_string_len);
  c.string_chars = reinterpret_cast<void *>(&fake_string_chars);
  c.m_open = &m_open;
  c.m_active = &m_active;
  c.m_interactable = &m_interactable;
  c.m_alive = &m_alive;
  c.class_open_gui = &cls_gui;
  c.class_details = &cls_details;
  c.class_open_out = &cls_open_out;
  c.class_sequence = &cls_sequence;
  c.class_bool = &cls_bool;
  c.class_string = &cls_string;
  c.f_current_state = &f_current_state;
  c.f_open_details = &f_open_details;
  c.f_sender = &f_sender;
  c.f_open_button = &f_open_button;
  c.f_seq_gui = &f_seq_gui;
  c.f_seq_response = &f_seq_response;
  c.f_seq_state = &f_seq_state;
  c.f_open_result = &f_open_result;
  c.state_open = kStateOpen;
  c.open_success = kOpenSuccess;
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

PgrIncomingGiftsSample take_sample() {
  PgrIncomingGiftsSample s{};
  assert(g_api->sample(&s) == PGR_GIFT_OK);
  assert(s.version == 1 && s.size == sizeof(PgrIncomingGiftsSample));
  return s;
}

std::vector<PgrIncomingGiftsEvent> drain_all() {
  std::vector<PgrIncomingGiftsEvent> out;
  PgrIncomingGiftsEvent e{};
  int r;
  while ((r = g_api->drain(&e)) == 1) out.push_back(e);
  assert(r == 0);
  return out;
}

bool saw(const std::vector<PgrIncomingGiftsEvent> &v, uint32_t kind, uint32_t reason) {
  for (const auto &e : v)
    if (e.kind == kind && e.reason == reason) return true;
  return false;
}

// Sleep just past the 100 ms attempt throttle, then run one owner-thread tick.
void act_tick() {
  usleep(110000);
  assert(g_api->tick(g_owner, 0) == PGR_GIFT_OK);
}

FakeGui make_ready_gui(FakeString *sender, FakeDetails *details, FakeSelectable *button) {
  sender->klass = &cls_string;
  sender->len = 5;
  button->klass = &cls_selectable;
  button->active = 1;
  button->interactable = 1;
  details->klass = &cls_details;
  details->sender = sender;
  FakeGui g{&cls_gui, kStateOpen, details, button};
  return g;
}

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror());
    return 1;
  }
  auto getter = reinterpret_cast<GetApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == 48);
  assert(core->query_feature(17, 99) == nullptr);  // exact version only
  const auto *api = static_cast<const PgrIncomingGiftsApi *>(core->query_feature(17, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrIncomingGiftsApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu sample=%zu event=%zu table=%zu\n",
              sizeof(PgrIncomingGiftsConfig), sizeof(PgrIncomingGiftsSample),
              sizeof(PgrIncomingGiftsEvent), sizeof(PgrIncomingGiftsApi));
  assert(sizeof(PgrIncomingGiftsConfig) == 240 && sizeof(PgrIncomingGiftsSample) == 88 &&
         sizeof(PgrIncomingGiftsEvent) == 16 && sizeof(PgrIncomingGiftsApi) == 64);

  const uint64_t tid = this_tid();
  g_owner = tid;
  PgrIncomingGiftsConfig cfg = make_config();

  // 1. Validation + not-installed.
  assert(api->install(nullptr) == PGR_GIFT_ERR_INVALID_ARG);
  PgrIncomingGiftsConfig bad = cfg;
  bad.version = 2;
  assert(api->install(&bad) == PGR_GIFT_ERR_BAD_VERSION);
  bad = cfg; bad.size = sizeof(PgrIncomingGiftsConfig) - 1;
  assert(api->install(&bad) == PGR_GIFT_ERR_BAD_SIZE);
  bad = cfg; bad.f_seq_state = nullptr;
  assert(api->install(&bad) == PGR_GIFT_ERR_MISSING_PTR);
  assert(api->update(1, 2000) == PGR_GIFT_ERR_NOT_INSTALLED);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &cfg, tid) == PGR_GIFT_ERR_NOT_INSTALLED);
  assert(api->tick(tid, 0) == PGR_GIFT_ERR_NOT_INSTALLED);
  assert(api->is_open(tid) == 0);

  // Install, idempotent re-install, reconfig mismatch.
  assert(api->install(&cfg) == PGR_GIFT_OK);
  assert(api->install(&cfg) == PGR_GIFT_OK);
  bad = cfg; bad.state_open = 9;
  assert(api->install(&bad) == PGR_GIFT_ERR_RECONFIG_MISMATCH);

  // 2. READY arms the slot and latches the owner.
  FakeString sender; FakeDetails details; FakeSelectable button;
  FakeGui gui = make_ready_gui(&sender, &details, &button);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui, tid) == PGR_GIFT_OK);
  PgrIncomingGiftsSample s = take_sample();
  assert(s.owned == 1 && s.dialog_serial == 1 && s.root_count >= 1 && s.attempted == 0);
  assert(api->is_open(tid) == 1);  // currentState == STATE_OPEN

  // 3. Wait-reason progression: AUTO_OFF, FOREGROUND, OPENING, DETAILS, BUTTON, then ISSUED.
  assert(api->update(0, 2000) == PGR_GIFT_OK);  // auto-open off
  act_tick();
  assert(saw(drain_all(), PGR_GIFT_EVENT_WAIT, PGR_GIFT_WAIT_AUTO_OFF));

  assert(api->update(1, 0) == PGR_GIFT_OK);  // lease 0 -> foreground wait
  act_tick();
  assert(saw(drain_all(), PGR_GIFT_EVENT_WAIT, PGR_GIFT_WAIT_FOREGROUND));

  assert(api->update(1, 2000) == PGR_GIFT_OK);
  gui.current_state = 1;  // not STATE_OPEN
  act_tick();
  assert(saw(drain_all(), PGR_GIFT_EVENT_WAIT, PGR_GIFT_WAIT_OPENING));
  gui.current_state = kStateOpen;

  sender.len = 0;  // invalid sender -> details wait
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  act_tick();
  assert(saw(drain_all(), PGR_GIFT_EVENT_WAIT, PGR_GIFT_WAIT_DETAILS));
  sender.len = 5;

  button.active = 0;  // button not active -> button wait
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  act_tick();
  assert(saw(drain_all(), PGR_GIFT_EVENT_WAIT, PGR_GIFT_WAIT_BUTTON));
  button.active = 1;

  // Everything ready -> exactly one automated open invoke + ISSUED.
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  g_open_invokes = 0;
  act_tick();
  s = take_sample();
  assert(g_open_invokes == 1 && s.issued_serial == 1 && s.attempted == 1 && s.automated == 1);
  assert(saw(drain_all(), PGR_GIFT_EVENT_ISSUED, 0));

  // 4. No duplicate: a second acting tick does not re-invoke (attempted latched).
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  act_tick();
  assert(g_open_invokes == 1);

  // 5. Sequence skip (automated): state 0 + success + same GUI -> state 3.
  FakeResponse resp{&cls_open_out, kOpenSuccess};
  FakeSequence seq{&cls_sequence, &gui, &resp, 0};
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  assert(api->observe(PGR_GIFT_OBSERVE_SEQUENCE, &seq, tid) == PGR_GIFT_OK);
  assert(seq.state == PGR_GIFT_STATE_SKIP && take_sample().skip_serial == 1);
  assert(saw(drain_all(), PGR_GIFT_EVENT_SKIP, 0));

  // Identity / response / state guards: none of these skip.
  FakeGui other_gui = make_ready_gui(&sender, &details, &button);
  seq = FakeSequence{&cls_sequence, &other_gui, &resp, 0};  // different GUI
  assert(api->observe(PGR_GIFT_OBSERVE_SEQUENCE, &seq, tid) == PGR_GIFT_OK);
  assert(seq.state == 0);
  seq = FakeSequence{&cls_sequence, &gui, &resp, 0}; resp.result = kOpenSuccess + 1;  // not success
  assert(api->observe(PGR_GIFT_OBSERVE_SEQUENCE, &seq, tid) == PGR_GIFT_OK);
  assert(seq.state == 0);
  resp.result = kOpenSuccess;
  seq = FakeSequence{&cls_sequence, &gui, &resp, 1};  // state already advanced
  assert(api->observe(PGR_GIFT_OBSERVE_SEQUENCE, &seq, tid) == PGR_GIFT_OK);
  assert(seq.state == 1);
  FakeResponse wrong{&cls_other, kOpenSuccess};
  seq = FakeSequence{&cls_sequence, &gui, &wrong, 0};  // wrong response class
  assert(api->observe(PGR_GIFT_OBSERVE_SEQUENCE, &seq, tid) == PGR_GIFT_OK);
  assert(seq.state == 0);
  drain_all();

  // 6. Manual open does NOT skip the animation. Close + fresh READY, press Open manually.
  assert(api->observe(PGR_GIFT_OBSERVE_CLOSE, &gui, tid) == PGR_GIFT_OK);
  assert(take_sample().owned == 0);
  FakeGui gui2 = make_ready_gui(&sender, &details, &button);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui2, tid) == PGR_GIFT_OK);
  assert(api->observe(PGR_GIFT_OBSERVE_OPEN, &gui2, tid) == PGR_GIFT_OK);  // USER pressed Open
  s = take_sample();
  assert(s.owned == 1 && s.attempted == 1 && s.automated == 0);  // manual: not automated
  FakeSequence mseq{&cls_sequence, &gui2, &resp, 0};
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  assert(api->observe(PGR_GIFT_OBSERVE_SEQUENCE, &mseq, tid) == PGR_GIFT_OK);
  assert(mseq.state == 0);  // manual open: animation NOT skipped
  // A tick does not auto-invoke a manually-opened dialog.
  g_open_invokes = 0;
  act_tick();
  assert(g_open_invokes == 0);
  drain_all();

  // 7. Same-GUI READY dedup: a repeated READY on the live GUI must NOT reset attempted/automated
  //    and must NOT bump dialog_serial.
  uint64_t dialog_before = take_sample().dialog_serial;
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui2, tid) == PGR_GIFT_OK);  // same live GUI
  s = take_sample();
  assert(s.attempted == 1 && s.dialog_serial == dialog_before);

  // 8. CLOSE + new READY permits a new gift (owner-thread cleanup + re-arm).
  assert(api->observe(PGR_GIFT_OBSERVE_CLOSE, &gui2, tid) == PGR_GIFT_OK);
  assert(take_sample().owned == 0);
  FakeGui gui3 = make_ready_gui(&sender, &details, &button);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui3, tid) == PGR_GIFT_OK);
  assert(take_sample().attempted == 0);
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  g_open_invokes = 0;
  act_tick();
  assert(g_open_invokes == 1 && take_sample().issued_serial == 2);
  drain_all();

  // 9. Reentrant disable DURING a getter revokes the attempt (no invoke, attempt not latched) and
  //    a later re-enable retries. Close + fresh READY first.
  assert(api->observe(PGR_GIFT_OBSERVE_CLOSE, &gui3, tid) == PGR_GIFT_OK);
  FakeGui gui4 = make_ready_gui(&sender, &details, &button);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui4, tid) == PGR_GIFT_OK);
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  g_open_invokes = 0;
  g_active_disables = true;  // the m_active getter disables mid-tick
  act_tick();
  s = take_sample();
  assert(g_open_invokes == 0 && s.attempted == 0);  // revoked before the invoke, retry allowed
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  act_tick();
  assert(g_open_invokes == 1 && take_sample().attempted == 1);
  drain_all();

  // 10. A failing open invoke latches the attempt (no auto-retry) and surfaces an ERROR.
  assert(api->observe(PGR_GIFT_OBSERVE_CLOSE, &gui4, tid) == PGR_GIFT_OK);
  FakeGui gui5 = make_ready_gui(&sender, &details, &button);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui5, tid) == PGR_GIFT_OK);
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  g_open_invokes = 0;
  g_open_throws = true;
  act_tick();
  assert(g_open_invokes == 1 && take_sample().attempted == 1);
  assert(saw(drain_all(), PGR_GIFT_EVENT_ERROR, PGR_GIFT_ERR_REASON_INVOKE));
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  act_tick();
  assert(g_open_invokes == 1);  // uncertain invoke: never retried
  g_open_throws = false;

  // 11. Wrong thread cannot steal the owner nor drive the controller.
  std::thread([&]() {
    FakeGui fg = make_ready_gui(&sender, &details, &button);
    assert(g_api->observe(PGR_GIFT_OBSERVE_READY, &fg, this_tid()) == PGR_GIFT_OK);  // not owner
    assert(g_api->tick(this_tid(), 0) == PGR_GIFT_OK);  // no-op off the owner thread
    assert(g_api->is_open(this_tid()) == 0);
  }).join();
  // A proven_tid that does not match the real caller is also ignored.
  assert(api->observe(PGR_GIFT_OBSERVE_OPEN, &gui5, tid + 1) == PGR_GIFT_OK);

  // 12. A reentrant stopping tick during a getter must REVOKE IMMEDIATELY and DEFER the teardown to
  //     the outermost frame: the outer pass issues no Open, and once it unwinds the slot is cleared
  //     and every root is gone (owned 0, root_count 0, all handles freed).
  assert(api->observe(PGR_GIFT_OBSERVE_CLOSE, &gui5, tid) == PGR_GIFT_OK);
  FakeGui gui6 = make_ready_gui(&sender, &details, &button);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui6, tid) == PGR_GIFT_OK);
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  g_open_invokes = 0;
  g_alive_nested_tick = true;  // the is_alive getter re-enters tick(stopping) once
  act_tick();
  s = take_sample();
  assert(g_open_invokes == 0);               // no Open issued after the nested stop
  assert(s.owned == 0 && s.root_count == 0); // deferred teardown ran at the outermost frame
  assert(g_live.empty());                    // every GC handle freed exactly once
  drain_all();

  // 13. A reentrant re-arm nested inside the durable-root free (a READY that fires while
  //     gchandle_free runs) must NOT be erased by the outer clear_slot: the freshly armed slot
  //     survives with its own root, and the handles stay balanced.
  FakeString s13a; FakeDetails d13a; FakeSelectable b13a;
  FakeGui gui13a = make_ready_gui(&s13a, &d13a, &b13a);
  FakeGui gui13b = make_ready_gui(&s13a, &d13a, &b13a);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui13a, tid) == PGR_GIFT_OK);
  const uint64_t dialog13 = take_sample().dialog_serial;
  g_rearm_gui = &gui13b;
  g_rearm_on_free = true;  // the CLOSE's root free re-arms gui13b from inside gchandle_free
  assert(api->observe(PGR_GIFT_OBSERVE_CLOSE, &gui13a, tid) == PGR_GIFT_OK);
  s = take_sample();
  assert(s.owned == 1 && s.root_count == 1);        // the re-armed slot survived the outer clear
  assert(s.dialog_serial == dialog13 + 1);          // gui13b armed a fresh dialog
  assert(g_live.size() == 1);                       // old handle freed, new handle held
  assert(s.attempted == 0);                         // the re-armed slot is fresh (not latched)
  // The surviving slot is usable: it can still auto-open.
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  g_open_invokes = 0;
  act_tick();
  assert(g_open_invokes == 1);
  assert(api->observe(PGR_GIFT_OBSERVE_CLOSE, &gui13b, tid) == PGR_GIFT_OK);
  s = take_sample();
  assert(s.owned == 0 && s.root_count == 0);
  g_rearm_gui = nullptr;
  drain_all();

  // 14. A reentrant CLOSE during a button getter invalidates the open pass: after the FINAL gc_get
  //     refresh the slot is gone, so the Open is never invoked and no ISSUED is emitted.
  FakeString s14; FakeDetails d14; FakeSelectable b14;
  FakeGui gui14 = make_ready_gui(&s14, &d14, &b14);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui14, tid) == PGR_GIFT_OK);
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  g_open_invokes = 0;
  const uint64_t issued14 = take_sample().issued_serial;
  g_cur_gui = &gui14;
  g_close_on_active = true;  // the m_active getter closes the owned GUI mid-tick
  act_tick();
  s = take_sample();
  assert(g_open_invokes == 0 && s.owned == 0);   // pass abandoned, slot closed under us
  assert(s.issued_serial == issued14);           // no ISSUED without an actual invoke
  assert(!saw(drain_all(), PGR_GIFT_EVENT_ISSUED, 0));
  assert(g_live.empty());
  g_cur_gui = nullptr;

  // 15. Moving GC: the owned GUI object relocates during a managed getter. Because every managed
  //     reference is re-resolved from its durable root after each call, the controller follows the
  //     object to its new address and issues exactly one open (it never reuses the poisoned
  //     pre-move pointer).
  FakeString s15; FakeDetails d15; FakeSelectable b15;
  FakeGui gui15 = make_ready_gui(&s15, &d15, &b15);
  FakeGui reloc15{};
  g_reloc_storage = &reloc15;
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui15, tid) == PGR_GIFT_OK);
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  g_open_invokes = 0;
  g_cur_gui = &gui15;
  g_relocate_on_alive = true;  // op_Implicit relocates the GUI object once
  act_tick();
  assert(g_open_invokes == 1);  // followed the relocation via the durable root
  assert(api->observe(PGR_GIFT_OBSERVE_CLOSE, g_cur_gui, tid) == PGR_GIFT_OK);
  assert(take_sample().owned == 0);
  g_cur_gui = nullptr;
  g_reloc_storage = nullptr;
  drain_all();

  // 16. READY during the gc_new of a READY: while an outer READY roots its incoming GUI, a reentrant
  //     READY arms a NEWER slot. The outer (now stale) arm must DROP its own fresh root and let the
  //     newer slot win -- no leak, no clobber, dedup intact.
  FakeString s16; FakeDetails d16; FakeSelectable b16;
  FakeGui gui16old = make_ready_gui(&s16, &d16, &b16);
  FakeGui gui16a = make_ready_gui(&s16, &d16, &b16);
  FakeGui gui16b = make_ready_gui(&s16, &d16, &b16);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui16old, tid) == PGR_GIFT_OK);
  const uint64_t dialog16 = take_sample().dialog_serial;
  g_rearm_new_target = &gui16a;  // the outer READY's durable gc_new
  g_rearm_gui = &gui16b;         // the reentrant newer arm
  g_rearm_on_new = true;
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui16a, tid) == PGR_GIFT_OK);
  s = take_sample();
  assert(s.owned == 1 && s.root_count == 1);     // exactly the newer slot is held
  assert(s.dialog_serial == dialog16 + 1);       // gui16b armed one fresh dialog; gui16a dropped
  assert(g_live.size() == 1);                    // old + the stale outer root freed; newer held
  assert(api->is_open(tid) == 1);                // the surviving slot is the live gui16b
  assert(api->observe(PGR_GIFT_OBSERVE_CLOSE, &gui16b, tid) == PGR_GIFT_OK);
  s = take_sample();
  assert(s.owned == 0 && s.root_count == 0 && g_live.empty());
  g_rearm_new_target = nullptr;
  g_rearm_gui = nullptr;
  drain_all();

  // 17. STOP + READY during the free: a nested stopping tick defers the teardown, and a reentrant
  //     READY fired from inside the deferred gchandle_free MUST be rejected (the stop latch is held
  //     across the free). No re-arm survives the stop; every root is gone.
  FakeString s17; FakeDetails d17; FakeSelectable b17;
  FakeGui gui17 = make_ready_gui(&s17, &d17, &b17);
  FakeGui gui17r = make_ready_gui(&s17, &d17, &b17);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui17, tid) == PGR_GIFT_OK);
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  g_open_invokes = 0;
  g_alive_nested_tick = true;   // the is_alive getter re-enters tick(stopping) once
  g_rearm_gui = &gui17r;
  g_rearm_free_target = &gui17; // fire on the durable-root (teardown) free, not a boxed-bool free
  g_rearm_on_free = true;       // the deferred teardown's free tries to re-arm gui17r
  act_tick();
  s = take_sample();
  assert(g_open_invokes == 0);                   // no Open after the nested stop
  assert(s.owned == 0 && s.root_count == 0);     // the re-arm was rejected; slot torn down
  assert(g_live.empty());                        // gui17 freed; gui17r never rooted
  g_rearm_gui = nullptr;
  g_rearm_on_free = false;
  g_rearm_free_target = nullptr;
  drain_all();

  // 18. CLOSE + new READY during the INITIAL alive getter in tick(): the acting pass pins the epoch
  //     BEFORE the first getter, so when the reentrant close frees the old slot and a fresh READY
  //     arms a newer one, the pass abandons WITHOUT clearing the newly-armed slot.
  FakeString s18; FakeDetails d18; FakeSelectable b18;
  FakeGui gui18 = make_ready_gui(&s18, &d18, &b18);
  FakeGui gui18n = make_ready_gui(&s18, &d18, &b18);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui18, tid) == PGR_GIFT_OK);
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  g_open_invokes = 0;
  g_cur_gui = &gui18;
  g_rearm_gui = &gui18n;
  g_close_ready_on_alive = true;  // the first is_alive closes gui18 and arms gui18n
  act_tick();
  s = take_sample();
  assert(g_open_invokes == 0);                   // stale pass abandoned, no Open
  assert(s.owned == 1 && s.root_count == 1);     // the freshly-armed slot survived
  assert(g_live.size() == 1);                    // gui18 freed; gui18n held
  assert(s.attempted == 0);                      // the new slot is fresh
  assert(api->observe(PGR_GIFT_OBSERVE_CLOSE, &gui18n, tid) == PGR_GIFT_OK);
  s = take_sample();
  assert(s.owned == 0 && s.root_count == 0 && g_live.empty());
  g_cur_gui = nullptr;
  g_rearm_gui = nullptr;
  drain_all();

  // 19. api_is_open with a moving GC: the owned object relocates during is_alive. is_open runs in an
  //     active frame and re-resolves the GUI from its root AFTER the getter, so it reports the open
  //     state of the relocated object (never the poisoned pre-move pointer).
  FakeString s19; FakeDetails d19; FakeSelectable b19;
  FakeGui gui19 = make_ready_gui(&s19, &d19, &b19);
  FakeGui reloc19{};
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui19, tid) == PGR_GIFT_OK);
  g_cur_gui = &gui19;
  g_reloc_storage = &reloc19;
  g_relocate_on_alive = true;
  assert(api->is_open(tid) == 1);                // followed the relocation via the durable root
  assert(api->observe(PGR_GIFT_OBSERVE_CLOSE, g_cur_gui, tid) == PGR_GIFT_OK);
  assert(take_sample().owned == 0 && g_live.empty());
  g_cur_gui = nullptr;
  g_reloc_storage = nullptr;

  // 20. api_is_open with a nested stopping tick during is_alive: is_open returns 0 and the deferred
  //     teardown runs at its own frame (the latch is held across the free), leaving no slot/roots.
  FakeString s20; FakeDetails d20; FakeSelectable b20;
  FakeGui gui20 = make_ready_gui(&s20, &d20, &b20);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui20, tid) == PGR_GIFT_OK);
  g_alive_nested_tick = true;    // is_alive re-enters tick(stopping) once
  assert(api->is_open(tid) == 0);                // abandoned under the latched stop
  s = take_sample();
  assert(s.owned == 0 && s.root_count == 0 && g_live.empty());  // deferred teardown ran
  g_alive_nested_tick = false;
  drain_all();

  // 21. Sequence disable+reenable DURING the allocation: because the revocation generation is pinned
  //     BEFORE the gc_new of the sequence, a disable+reenable straddling that allocation revokes the
  //     pinned apply -- the skip is NOT written even though auto-open is on again by the time we act.
  FakeString s21; FakeDetails d21; FakeSelectable b21;
  FakeGui gui21 = make_ready_gui(&s21, &d21, &b21);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui21, tid) == PGR_GIFT_OK);
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  g_open_invokes = 0;
  act_tick();                                    // latch automated via one real issue
  assert(take_sample().automated == 1);
  drain_all();
  FakeResponse resp21{&cls_open_out, kOpenSuccess};
  FakeSequence seq21{&cls_sequence, &gui21, &resp21, 0};
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  const uint64_t skip21 = take_sample().skip_serial;
  g_disable_on_seq_new = true;   // disable+reenable inside the sequence gc_new
  assert(api->observe(PGR_GIFT_OBSERVE_SEQUENCE, &seq21, tid) == PGR_GIFT_OK);
  assert(seq21.state == 0);                      // revoked by the straddling gen bump: no skip
  assert(take_sample().skip_serial == skip21);
  g_disable_on_seq_new = false;

  // 22. Sequence CLOSE+rearm mid-lookup: a reentrant close+ready replaces the owned slot while the
  //     sequence state is read. The final epoch/automated recheck before the field_set aborts the
  //     write -- a replacement sequence is never mutated on stale ownership.
  FakeString s22; FakeDetails d22; FakeSelectable b22;
  FakeGui gui22n = make_ready_gui(&s22, &d22, &b22);
  FakeResponse resp22{&cls_open_out, kOpenSuccess};
  FakeSequence seq22{&cls_sequence, &gui21, &resp22, 0};
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  const uint64_t skip22 = take_sample().skip_serial;
  g_cur_gui = &gui21;            // the sequence's current owned GUI
  g_rearm_gui = &gui22n;         // the replacement armed mid-lookup
  g_close_ready_on_seq_state = true;
  assert(api->observe(PGR_GIFT_OBSERVE_SEQUENCE, &seq22, tid) == PGR_GIFT_OK);
  assert(seq22.state == 0);                      // aborted: never skipped on stale ownership
  assert(take_sample().skip_serial == skip22);
  s = take_sample();
  assert(s.owned == 1);                          // the replacement slot (gui22n) is owned
  assert(api->observe(PGR_GIFT_OBSERVE_CLOSE, &gui22n, tid) == PGR_GIFT_OK);
  s = take_sample();
  assert(s.owned == 0 && s.root_count == 0 && g_live.empty());
  g_cur_gui = nullptr;
  g_rearm_gui = nullptr;
  g_close_ready_on_seq_state = false;
  drain_all();

  // 23. FIFO overflow fails closed: more than CAP skip records latch the fault and surface an
  //     OVERFLOW error; no further issue happens while faulted.
  FakeGui gui7 = make_ready_gui(&sender, &details, &button);
  assert(api->observe(PGR_GIFT_OBSERVE_READY, &gui7, tid) == PGR_GIFT_OK);
  assert(api->update(1, 2000) == PGR_GIFT_OK);
  // Latch automated via one real issue.
  g_open_invokes = 0;
  act_tick();
  assert(take_sample().automated == 1);
  drain_all();
  FakeResponse r2{&cls_open_out, kOpenSuccess};
  for (int i = 0; i < PGR_GIFT_EVENT_CAP + 8; ++i) {
    FakeSequence sq{&cls_sequence, &gui7, &r2, 0};
    assert(api->update(1, 2000) == PGR_GIFT_OK);
    api->observe(PGR_GIFT_OBSERVE_SEQUENCE, &sq, tid);
  }
  s = take_sample();
  assert(s.overflow == 1 && s.fault == 1 && s.pending_events == PGR_GIFT_EVENT_CAP);
  auto records = drain_all();
  assert(records.size() == PGR_GIFT_EVENT_CAP);
  assert(records.back().kind == PGR_GIFT_EVENT_ERROR &&
         records.back().reason == PGR_GIFT_ERR_REASON_OVERFLOW);

  std::printf("incoming_gifts_test passed: ABI sizes, validation/idempotence, ready/wait reasons, "
              "on-off+foreground, single automated open, no duplicate, sequence skip + identity "
              "guards, manual-open no skip, same-GUI ready dedup, close/new ready, revoke during "
              "getter, failing invoke no retry, wrong thread, reentrant stop deferred teardown "
              "(no open, roots 0), reentrant re-arm during free not erased, reentrant close "
              "invalidates open pass (no ISSUED without invoke), moving-GC relocation followed via "
              "root, READY during gc_new of READY (stale outer arm dropped), STOP+READY during free "
              "(re-arm rejected), CLOSE+new READY during initial alive (new slot kept), is_open "
              "moving/stop, sequence disable+reenable during allocation and close/rearm mid-lookup "
              "(no stale skip), FIFO overflow fail-closed\n");
  return 0;
}

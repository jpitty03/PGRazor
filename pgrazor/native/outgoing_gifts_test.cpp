#include "outgoing_gifts.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <set>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// Real-API fake-IL2CPP harness for feature 22 (outgoing bundled-gift controller). It resolves the
// table through the exported pgrazor_native_api v2 registry (query_feature(22,1)) and drives the
// controller against a fake managed heap (classes/methods/fields, boxed bool/int returns, virtual
// enumerator dispatch, System.String content, synchronous RPC callbacks). The module installs NO
// hook. The quarantine case waits 31 seconds against the production clock. The coordinator
// builds and runs this against libpgrazor.so after registering query_feature case 22.

namespace {

using GetApiFn = const void *(*)(uint32_t);


// ----- class / method / field tokens (distinct addresses) -------------------------
int cls_panel, cls_friend, cls_player, cls_service, cls_rpc, cls_sticker_svc, cls_gift_box,
    cls_sticker, cls_sticker_list, cls_send_out, cls_callback, cls_bool, cls_int32, cls_string,
    cls_profile, cls_selectable, cls_iterable, cls_enum, cls_nia, cls_promise;
int m_panel, m_disabled, m_active, m_interactable, m_sendable, m_get_enum, m_move, m_current,
    m_dispose, m_nia_id, m_inventory, m_scount, m_sitem, m_quantity, m_reserve, m_return, m_send,
    m_alive;
int f_data, f_panel_friend, f_player, f_player_id, f_choose_button, f_check_pending, f_check_result,
    f_panel_gifting, f_panel_rpc, f_nia_util, f_rpc_stickers, f_gift_id, f_sticker_id, f_send_result,
    f_callback_service, f_callback_id;

constexpr int32_t kCheckSuccess = 7;
constexpr int32_t kSendSuccess = 1;

// ----- fake managed objects --------------------------------------------------------
struct FakeService_fwd;  // gifting service (full definition below, after FakePanel)
struct FakePromise_t;     // SendGift promise (unused value; stays null)
struct FakeString { void *klass; int len; uint16_t chars[256]; };
void set_str(FakeString *s, const char *ascii) {
  s->klass = &cls_string;
  int n = static_cast<int>(std::strlen(ascii));
  s->len = n;
  for (int i = 0; i < n; ++i) s->chars[i] = static_cast<uint16_t>(ascii[i]);
}
struct FakeSelectable { void *klass; uint8_t active; uint8_t interactable; };
struct FakePlayer { void *klass; FakeString *playerId; };
struct FakeFriend { void *klass; FakePlayer *player; };
struct FakeData { FakeFriend *friend_; };
struct FakeGift { void *klass; uint64_t giftId; };
struct FakeEnum { void *klass; FakeGift **gifts; int count; int idx; int disposes; };
struct FakeIterable { void *klass; FakeEnum *enumerator; };
struct FakeSticker { void *klass; FakeString *stickerId; int qty; };
struct FakeList { void *klass; FakeSticker **items; int count; };
struct FakeStickerSvc { void *klass; FakeList *inventory; int reserves; int returns; };
struct FakeRpc { void *klass; FakeStickerSvc *stickers; };
struct FakePanel {
  void *klass; FakeData *data; FakeSelectable *button; uint8_t checkPending; int32_t checkResult;
  FakeService_fwd *gifting; FakeRpc *rpc; void *niaUtil;
};
// forward: gifting service
struct FakeService_fwd { void *klass; FakeIterable *sendable; };
struct FakeProfile { void *klass; FakePanel *panel; uint8_t disabled; };
struct FakeSendOut { void *klass; int32_t result; };
struct FakeCallback { void *klass; void *service; FakeString *playerId; };

struct FakeBoxB { void *klass; uint8_t v; };
struct FakeBoxI { void *klass; int32_t v; };
FakeBoxB g_bb[32]; int g_bbi = 0;
FakeBoxI g_bi[32]; int g_bii = 0;
FakeBoxB *box_bool(bool v) { FakeBoxB *b = &g_bb[g_bbi++ & 31]; b->klass = &cls_bool; b->v = v ? 1 : 0; return b; }
FakeBoxI *box_int(int32_t v) { FakeBoxI *b = &g_bi[g_bii++ & 31]; b->klass = &cls_int32; b->v = v; return b; }

// ----- runtime shims ----------------------------------------------------------------
const PgrOutgoingGiftsApi *g_api = nullptr;

uint64_t g_owner = 0;

std::set<void *> g_destroyed;  // op_Implicit returns false
bool g_fail_root = false;
uintptr_t g_next_handle = 0x5000;
std::unordered_map<uintptr_t, void *> g_live;

// SendGift capture + synchronous-callback injection.
int g_send_calls = 0;
bool g_last_send_had_sticker = false;
uint64_t g_last_send_gift_id = 0;
bool g_fire_result = false, g_fire_finally = false, g_stop_on_send = false;
FakeCallback *g_cb = nullptr;
FakeSendOut *g_sendout = nullptr;
FakePromise_t *g_promise = nullptr;

struct FakePromise_t { void *klass; };

// Generic one-shot reentrancy injection: when `g_oneshot_method` is invoked, fire `g_oneshot`
// exactly once (cleared first) so a reentrant queue/observe/tick runs mid managed call. A separate
// one-shot `g_throw_on` makes a chosen managed method report a thrown exception.
const void *g_oneshot_method = nullptr;
std::function<void()> g_oneshot;
const void *g_throw_on = nullptr;
int g_reentrant_queue_result = 0;
bool live_contains(void *obj) {
  for (const auto &kv : g_live) if (kv.second == obj) return true;
  return false;
}

uintptr_t fake_gc_new(void *obj, int) {
  if (obj == nullptr || g_fail_root) return 0;
  uintptr_t id = g_next_handle++;
  g_live[id] = obj;
  return id;
}
void *fake_gc_get(uintptr_t h) { auto it = g_live.find(h); return it != g_live.end() ? it->second : nullptr; }
void fake_gc_free(uintptr_t h) {
  auto it = g_live.find(h);
  assert(it != g_live.end() && "double or invalid gchandle_free");
  g_live.erase(it);
}
void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }
void *fake_unbox(void *obj) { return static_cast<char *>(obj) + sizeof(void *); }  // data after klass
void *fake_get_virtual(void *, const void *method) { return const_cast<void *>(method); }
int fake_string_len(void *s) { return static_cast<FakeString *>(s)->len; }
uint16_t *fake_string_chars(void *s) { return static_cast<FakeString *>(s)->chars; }

void fake_field_get(void *obj, void *field, void *out) {
  if (field == &f_data) { *static_cast<void **>(out) = static_cast<FakePanel *>(obj)->data; return; }
  if (field == &f_panel_friend) { *static_cast<void **>(out) = static_cast<FakeData *>(obj)->friend_; return; }
  if (field == &f_player) { *static_cast<void **>(out) = static_cast<FakeFriend *>(obj)->player; return; }
  if (field == &f_player_id) { *static_cast<void **>(out) = static_cast<FakePlayer *>(obj)->playerId; return; }
  if (field == &f_choose_button) { *static_cast<void **>(out) = static_cast<FakePanel *>(obj)->button; return; }
  if (field == &f_check_pending) { *static_cast<uint8_t *>(out) = static_cast<FakePanel *>(obj)->checkPending; return; }
  if (field == &f_check_result) { *static_cast<int32_t *>(out) = static_cast<FakePanel *>(obj)->checkResult; return; }
  if (field == &f_panel_gifting) { *static_cast<void **>(out) = static_cast<FakePanel *>(obj)->gifting; return; }
  if (field == &f_panel_rpc) { *static_cast<void **>(out) = static_cast<FakePanel *>(obj)->rpc; return; }
  if (field == &f_nia_util) { *static_cast<void **>(out) = static_cast<FakePanel *>(obj)->niaUtil; return; }
  if (field == &f_rpc_stickers) { *static_cast<void **>(out) = static_cast<FakeRpc *>(obj)->stickers; return; }
  if (field == &f_gift_id) { *static_cast<uint64_t *>(out) = static_cast<FakeGift *>(obj)->giftId; return; }
  if (field == &f_sticker_id) { *static_cast<void **>(out) = static_cast<FakeSticker *>(obj)->stickerId; return; }
  if (field == &f_send_result) { *static_cast<int32_t *>(out) = static_cast<FakeSendOut *>(obj)->result; return; }
  if (field == &f_callback_service) { *static_cast<void **>(out) = static_cast<FakeCallback *>(obj)->service; return; }
  if (field == &f_callback_id) { *static_cast<void **>(out) = static_cast<FakeCallback *>(obj)->playerId; return; }
  assert(false && "unknown field token");
}

FakeString g_nia_str;  // returned by m_nia_id

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
  if (method == &m_panel) return static_cast<FakeProfile *>(obj)->panel;
  if (method == &m_disabled) return box_bool(static_cast<FakeProfile *>(obj)->disabled != 0);
  if (method == &m_active) return box_bool(static_cast<FakeSelectable *>(obj)->active != 0);
  if (method == &m_interactable) return box_bool(static_cast<FakeSelectable *>(obj)->interactable != 0);
  if (method == &m_alive) { void *o = params[0]; return box_bool(o != nullptr && g_destroyed.find(o) == g_destroyed.end()); }
  if (method == &m_sendable) return static_cast<FakeService_fwd *>(obj)->sendable;
  if (method == &m_get_enum) { FakeEnum *e = static_cast<FakeIterable *>(obj)->enumerator; e->idx = -1; return e; }
  if (method == &m_move) { FakeEnum *e = static_cast<FakeEnum *>(obj); e->idx++; return box_bool(e->idx < e->count); }
  if (method == &m_current) { FakeEnum *e = static_cast<FakeEnum *>(obj); return e->gifts[e->idx]; }
  if (method == &m_dispose) { static_cast<FakeEnum *>(obj)->disposes++; return nullptr; }
  if (method == &m_nia_id) return &g_nia_str;
  if (method == &m_inventory) return static_cast<FakeStickerSvc *>(obj)->inventory;
  if (method == &m_scount) return box_int(static_cast<FakeList *>(obj)->count);
  if (method == &m_sitem) { int i = *static_cast<int32_t *>(params[0]); return static_cast<FakeList *>(obj)->items[i]; }
  if (method == &m_quantity) { FakeSticker *s = static_cast<FakeSticker *>(params[0]); return box_int(s->qty); }
  if (method == &m_reserve) { static_cast<FakeStickerSvc *>(obj)->reserves++; return nullptr; }
  if (method == &m_return) { static_cast<FakeStickerSvc *>(obj)->returns++; return nullptr; }
  if (method == &m_send) {
    ++g_send_calls;
    g_last_send_had_sticker = params[3] != nullptr;
    g_last_send_gift_id = *static_cast<uint64_t *>(params[0]);
    if (g_stop_on_send) { g_stop_on_send = false; g_api->tick(g_owner, 1); }
    if (g_fire_result && g_cb && g_sendout) g_api->observe(PGR_OG_OBSERVE_RPC_RESULT, g_cb, g_sendout, g_owner);
    if (g_fire_finally && g_cb) g_api->observe(PGR_OG_OBSERVE_RPC_FINALLY, g_cb, nullptr, g_owner);
    return g_promise;
  }
  assert(false && "unknown method token");
  return nullptr;
}

PgrOutgoingGiftsConfig make_config() {
  PgrOutgoingGiftsConfig c{};
  c.version = 1;
  c.size = sizeof(PgrOutgoingGiftsConfig);
  c.gchandle_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gchandle_get_target = reinterpret_cast<void *>(&fake_gc_get);
  c.gchandle_free = reinterpret_cast<void *>(&fake_gc_free);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.object_unbox = reinterpret_cast<void *>(&fake_unbox);
  c.object_get_virtual_method = reinterpret_cast<void *>(&fake_get_virtual);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.field_set_value = reinterpret_cast<void *>(&fake_field_get);  // unused by this feature
  c.string_length = reinterpret_cast<void *>(&fake_string_len);
  c.string_chars = reinterpret_cast<void *>(&fake_string_chars);
  c.m_panel = &m_panel; c.m_disabled = &m_disabled; c.m_active = &m_active;
  c.m_interactable = &m_interactable; c.m_sendable = &m_sendable; c.m_get_enumerator = &m_get_enum;
  c.m_move_gift = &m_move; c.m_current_gift = &m_current; c.m_dispose_gift = &m_dispose;
  c.m_nia_id = &m_nia_id; c.m_inventory = &m_inventory; c.m_sticker_count = &m_scount;
  c.m_sticker_item = &m_sitem; c.m_quantity = &m_quantity; c.m_reserve = &m_reserve;
  c.m_return_sticker = &m_return; c.m_rpc_send = &m_send; c.m_alive = &m_alive;
  c.class_panel = &cls_panel; c.class_friend = &cls_friend; c.class_player = &cls_player;
  c.class_service = &cls_service; c.class_rpc = &cls_rpc; c.class_sticker_svc = &cls_sticker_svc;
  c.class_gift_box = &cls_gift_box; c.class_sticker = &cls_sticker;
  c.class_sticker_list = &cls_sticker_list; c.class_send_out = &cls_send_out;
  c.class_callback = &cls_callback; c.class_bool = &cls_bool; c.class_int32 = &cls_int32;
  c.class_string = &cls_string;
  c.f_data = &f_data; c.f_panel_friend = &f_panel_friend; c.f_player = &f_player;
  c.f_player_id = &f_player_id; c.f_choose_button = &f_choose_button;
  c.f_check_pending = &f_check_pending; c.f_check_result = &f_check_result;
  c.f_panel_gifting = &f_panel_gifting; c.f_panel_rpc = &f_panel_rpc; c.f_nia_util = &f_nia_util;
  c.f_rpc_stickers = &f_rpc_stickers; c.f_gift_id = &f_gift_id; c.f_sticker_id = &f_sticker_id;
  c.f_send_result = &f_send_result; c.f_callback_service = &f_callback_service;
  c.f_callback_id = &f_callback_id;
  c.check_success = kCheckSuccess;
  c.send_success = kSendSuccess;
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

PgrOutgoingGiftsSample take_sample() {
  PgrOutgoingGiftsSample s{};
  assert(g_api->sample(&s) == PGR_OG_OK);
  assert(s.version == 1 && s.size == sizeof(PgrOutgoingGiftsSample));
  return s;
}
std::vector<PgrOutgoingGiftsEvent> drain_all() {
  std::vector<PgrOutgoingGiftsEvent> out;
  PgrOutgoingGiftsEvent e{};
  int r;
  while ((r = g_api->next_event(&e)) == 1) out.push_back(e);
  assert(r == 0);
  return out;
}
bool saw(const std::vector<PgrOutgoingGiftsEvent> &v, uint32_t kind, int32_t id) {
  for (const auto &e : v) if (e.kind == kind && e.id == id) return true;
  return false;
}

// ----- scene wiring -----------------------------------------------------------------
struct Scene {
  FakeString rcpt, nia, stk;
  FakeSelectable button{&cls_selectable, 1, 1};
  FakePlayer player{&cls_player, nullptr};
  FakeFriend friend_{&cls_friend, nullptr};
  FakeData data{nullptr};
  FakeGift gift{&cls_gift_box, 0xABCDEF};
  FakeGift *gifts[1];
  FakeEnum enumr{&cls_enum, nullptr, 0, -1, 0};
  FakeIterable iterable{&cls_iterable, nullptr};
  FakeService_fwd service{&cls_service, nullptr};
  FakeSticker sticker{&cls_sticker, nullptr, 0};
  FakeSticker *items[2];
  FakeList list{&cls_sticker_list, nullptr, 0};
  FakeStickerSvc svc{&cls_sticker_svc, nullptr, 0, 0};
  FakeRpc rpc{&cls_rpc, nullptr};
  FakePanel panel{};
  FakeProfile profile{&cls_profile, nullptr, 0};
  FakeCallback cb{&cls_callback, nullptr, nullptr};
  FakeSendOut sendout{&cls_send_out, kSendSuccess};
};

void wire(Scene &s, int gift_count) {
  set_str(&g_nia_str, "NIA12345");
  set_str(&s.rcpt, "RCPT1");
  set_str(&s.nia, "NIA12345");
  set_str(&s.stk, "STK1");
  s.player.playerId = &s.rcpt;
  s.friend_.player = &s.player;
  s.data.friend_ = &s.friend_;
  s.gifts[0] = &s.gift;
  s.enumr.gifts = s.gifts;
  s.enumr.count = gift_count;
  s.iterable.enumerator = &s.enumr;
  s.service.sendable = &s.iterable;
  s.sticker.stickerId = &s.stk;
  s.svc.inventory = &s.list;
  s.rpc.stickers = &s.svc;
  s.panel.klass = &cls_panel;
  s.panel.data = &s.data;
  s.panel.button = &s.button;
  s.panel.checkPending = 0;
  s.panel.checkResult = kCheckSuccess;
  s.panel.gifting = &s.service;
  s.panel.rpc = &s.rpc;
  s.panel.niaUtil = &s.svc;  // any non-null object (class not checked)
  s.profile.panel = &s.panel;
  s.cb.service = &s.rpc;
  s.cb.playerId = &s.rcpt;  // matches recipient
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
  assert(core->query_feature(22, 99) == nullptr);
  const auto *api = static_cast<const PgrOutgoingGiftsApi *>(core->query_feature(22, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrOutgoingGiftsApi));
  g_api = api;


  std::printf("ABI sizes: config=%zu sample=%zu event=%zu table=%zu\n",
              sizeof(PgrOutgoingGiftsConfig), sizeof(PgrOutgoingGiftsSample),
              sizeof(PgrOutgoingGiftsEvent), sizeof(PgrOutgoingGiftsApi));
  assert(sizeof(PgrOutgoingGiftsConfig) == 488 && sizeof(PgrOutgoingGiftsSample) == 88 &&
         sizeof(PgrOutgoingGiftsEvent) == 16 && sizeof(PgrOutgoingGiftsApi) == 64);

  const uint64_t tid = this_tid();
  g_owner = tid;
  PgrOutgoingGiftsConfig cfg = make_config();

  // 1. Validation + not-installed.
  assert(api->install(nullptr) == PGR_OG_ERR_INVALID_ARG);
  PgrOutgoingGiftsConfig bad = cfg; bad.version = 2;
  assert(api->install(&bad) == PGR_OG_ERR_BAD_VERSION);
  bad = cfg; bad.size = sizeof(PgrOutgoingGiftsConfig) - 1;
  assert(api->install(&bad) == PGR_OG_ERR_BAD_SIZE);
  bad = cfg; bad.f_callback_id = nullptr;
  assert(api->install(&bad) == PGR_OG_ERR_MISSING_PTR);
  assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_ERR_NOT_INSTALLED);
  assert(api->tick(tid, 0) == PGR_OG_ERR_NOT_INSTALLED);
  assert(api->queue(&cfg, &cfg, tid) == PGR_OG_QUEUE_ORIGINAL);  // not installed -> original

  assert(api->install(&cfg) == PGR_OG_OK);
  assert(api->install(&cfg) == PGR_OG_OK);  // idempotent
  bad = cfg; bad.send_success = 9;
  assert(api->install(&bad) == PGR_OG_ERR_RECONFIG);

  // 2. queue happy path (no sticker) -> issue, one SendGift, REQUEST aux=0, SENDING.
  { Scene s; wire(s, 1);
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    g_send_calls = 0;
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    PgrOutgoingGiftsSample sm = take_sample();
    assert(sm.active == 1 && sm.issued == 0 && sm.roots >= 2);
    assert(saw(drain_all(), PGR_OG_EVENT_STATUS, PGR_OG_MSG_PREPARING));
    assert(api->tick(tid, 0) == PGR_OG_OK);
    sm = take_sample();
    assert(g_send_calls == 1 && g_last_send_had_sticker == false && g_last_send_gift_id == 0xABCDEF);
    assert(sm.issued == 1 && sm.active == 1);
    auto ev = drain_all();
    assert(saw(ev, PGR_OG_EVENT_STATUS, PGR_OG_MSG_SENDING) && saw(ev, PGR_OG_EVENT_REQUEST, 0));
    // Complete via matched callbacks, then a tick reports SENT + clears.
    s.sendout.result = kSendSuccess;
    assert(api->observe(PGR_OG_OBSERVE_RPC_RESULT, &s.cb, &s.sendout, tid) == PGR_OG_OK);
    assert(api->observe(PGR_OG_OBSERVE_RPC_FINALLY, &s.cb, nullptr, tid) == PGR_OG_OK);
    assert(take_sample().result_known == 1);
    assert(api->tick(tid, 0) == PGR_OG_OK);
    sm = take_sample();
    assert(sm.active == 0 && sm.roots == 0 && g_live.empty());
    ev = drain_all();
    assert(saw(ev, PGR_OG_EVENT_STATUS, PGR_OG_MSG_SENT) && saw(ev, PGR_OG_EVENT_RESULT, kSendSuccess));
  }

  // 3. "Already pending" when a transaction exists.
  { Scene s; wire(s, 1);
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    Scene s2; wire(s2, 1);
    assert(api->queue(&s2.panel, &s2.profile, tid) == PGR_OG_QUEUE_HANDLED);  // still handled
    assert(saw(drain_all(), PGR_OG_EVENT_STATUS, PGR_OG_MSG_ALREADY));
    assert(api->tick(tid, 1) == PGR_OG_OK);  // stop clears
    assert(take_sample().active == 0 && g_live.empty());
    drain_all();
  }

  // 4. With sticker: first candidate qty 0 (skipped), second qty>0 reserved once; SendGift sticker
  //    non-null, REQUEST aux=1; clear returns the reservation exactly once; Dispose once.
  { Scene s; wire(s, 1);
    FakeSticker s0{&cls_sticker, &s.stk, 0};
    FakeSticker s1{&cls_sticker, &s.stk, 3};
    s.items[0] = &s0; s.items[1] = &s1; s.list.items = s.items; s.list.count = 2;
    assert(api->update(PGR_OG_MASK_SEND | PGR_OG_MASK_STICKER, 2000, tid) == PGR_OG_OK);
    g_send_calls = 0;
    g_cb = &s.cb; g_sendout = &s.sendout; g_fire_result = true; g_fire_finally = true;
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->tick(tid, 0) == PGR_OG_OK);  // issues; synchronous callbacks fire inside SendGift
    assert(g_send_calls == 1 && g_last_send_had_sticker == true);
    assert(s.svc.reserves == 1);
    assert(s.enumr.disposes == 1);  // Dispose exactly once
    { auto ev = drain_all();  // the REQUEST record carries aux = sticker(1)
      bool req_sticker = false;
      for (const auto &e : ev) if (e.kind == PGR_OG_EVENT_REQUEST && e.aux == 1) req_sticker = true;
      assert(req_sticker); }
    PgrOutgoingGiftsSample sm = take_sample();
    assert(sm.result_known == 1);  // callback recorded synchronously
    assert(api->tick(tid, 0) == PGR_OG_OK);  // completes -> clear returns reservation
    assert(s.svc.returns == 1);     // reservation returned exactly once
    assert(take_sample().active == 0 && g_live.empty());
    g_fire_result = g_fire_finally = false; g_cb = nullptr; g_sendout = nullptr;
    drain_all();
  }

  // 5. No gifts available: MoveNext false -> NO_GIFTS, Dispose once, no send, clear.
  { Scene s; wire(s, 0);  // enumerator yields nothing
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    g_send_calls = 0;
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->tick(tid, 0) == PGR_OG_OK);
    assert(g_send_calls == 0 && s.enumr.disposes == 1);
    assert(saw(drain_all(), PGR_OG_EVENT_STATUS, PGR_OG_MSG_NO_GIFTS));
    assert(take_sample().active == 0 && g_live.empty());
  }

  // 6. Sticker inventory over the scan bound -> error, clear, no send.
  { Scene s; wire(s, 1);
    s.list.items = s.items; s.list.count = PGR_OG_STICKER_SCAN_MAX + 1;
    assert(api->update(PGR_OG_MASK_SEND | PGR_OG_MASK_STICKER, 2000, tid) == PGR_OG_OK);
    g_send_calls = 0;
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->tick(tid, 0) == PGR_OG_OK);
    assert(g_send_calls == 0);
    assert(saw(drain_all(), PGR_OG_EVENT_ERROR, PGR_OG_ERR_STICKER_SVC));
    assert(take_sample().active == 0 && g_live.empty());
  }

  // 7. Unmatched callback (wrong recipient id): result NOT recorded.
  { Scene s; wire(s, 1);
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->tick(tid, 0) == PGR_OG_OK);  // issued
    FakeString wrong; set_str(&wrong, "OTHER");
    FakeCallback bad_cb{&cls_callback, &s.rpc, &wrong};
    assert(api->observe(PGR_OG_OBSERVE_RPC_RESULT, &bad_cb, &s.sendout, tid) == PGR_OG_OK);
    assert(api->observe(PGR_OG_OBSERVE_RPC_FINALLY, &bad_cb, nullptr, tid) == PGR_OG_OK);
    PgrOutgoingGiftsSample sm = take_sample();
    assert(sm.result_known == 0 && sm.issued == 1 && sm.active == 1);  // unmatched: ignored
    // Wrong service also unmatched.
    FakeRpc other_rpc{&cls_rpc, &s.svc};
    FakeCallback bad_cb2{&cls_callback, &other_rpc, &s.rcpt};
    assert(api->observe(PGR_OG_OBSERVE_RPC_RESULT, &bad_cb2, &s.sendout, tid) == PGR_OG_OK);
    assert(take_sample().result_known == 0);
    // Matched completes it.
    assert(api->observe(PGR_OG_OBSERVE_RPC_RESULT, &s.cb, &s.sendout, tid) == PGR_OG_OK);
    assert(api->observe(PGR_OG_OBSERVE_RPC_FINALLY, &s.cb, nullptr, tid) == PGR_OG_OK);
    assert(api->tick(tid, 0) == PGR_OG_OK);
    assert(take_sample().active == 0 && g_live.empty());
    drain_all();
  }

  // 8. Issued-timeout quarantine: advance the test clock past 30 s -> timed_out, PENDING, retained;
  //    a later finally completes it.
  { Scene s; wire(s, 1);
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->tick(tid, 0) == PGR_OG_OK);  // issued
    drain_all();
    usleep((PGR_OG_TXN_TIMEOUT_MS + 1000) * 1000);
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);  // keep the gate live
    assert(api->tick(tid, 0) == PGR_OG_OK);
    PgrOutgoingGiftsSample sm = take_sample();
    assert(sm.timed_out == 1 && sm.active == 1 && sm.issued == 1);  // retained, not cleared
    assert(saw(drain_all(), PGR_OG_EVENT_STATUS, PGR_OG_MSG_PENDING));
    s.sendout.result = kSendSuccess;
    assert(api->observe(PGR_OG_OBSERVE_RPC_RESULT, &s.cb, &s.sendout, tid) == PGR_OG_OK);
    assert(api->observe(PGR_OG_OBSERVE_RPC_FINALLY, &s.cb, nullptr, tid) == PGR_OG_OK);
    assert(api->tick(tid, 0) == PGR_OG_OK);
    assert(take_sample().active == 0 && g_live.empty());
    drain_all();
  }

  // 9. Off BEFORE issue cancels; off AFTER issue survives.
  { Scene s; wire(s, 1);
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->update(0, 0, tid) == PGR_OG_OK);  // off before issue
    assert(api->tick(tid, 0) == PGR_OG_OK);
    assert(take_sample().active == 0 && g_live.empty());  // unissued cancelled
    drain_all();

    Scene s2; wire(s2, 1);
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    assert(api->queue(&s2.panel, &s2.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->tick(tid, 0) == PGR_OG_OK);  // issued
    assert(api->update(0, 0, tid) == PGR_OG_OK);  // off after issue
    assert(api->tick(tid, 0) == PGR_OG_OK);
    assert(take_sample().active == 1);  // issued survives off
    // Result bookkeeping still works while off.
    assert(api->observe(PGR_OG_OBSERVE_RPC_RESULT, &s2.cb, &s2.sendout, tid) == PGR_OG_OK);
    assert(api->observe(PGR_OG_OBSERVE_RPC_FINALLY, &s2.cb, nullptr, tid) == PGR_OG_OK);
    assert(take_sample().result_known == 1);
    assert(api->tick(tid, 1) == PGR_OG_OK);  // stop clears
    assert(take_sample().active == 0 && g_live.empty());
    drain_all();
  }

  // 10. Profile close: unissued cancels; issued survives.
  { Scene s; wire(s, 1);
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->observe(PGR_OG_OBSERVE_PROFILE_CLOSE, &s.profile, nullptr, tid) == PGR_OG_OK);
    assert(take_sample().active == 0 && g_live.empty());  // unissued cancelled

    Scene s2; wire(s2, 1);
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    assert(api->queue(&s2.panel, &s2.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->tick(tid, 0) == PGR_OG_OK);  // issued
    assert(api->observe(PGR_OG_OBSERVE_PROFILE_CLOSE, &s2.profile, nullptr, tid) == PGR_OG_OK);
    assert(take_sample().active == 1);  // issued survives profile close
    assert(api->tick(tid, 1) == PGR_OG_OK);
    assert(take_sample().active == 0 && g_live.empty());
    drain_all();
  }

  // 10b. Blocked (disabled profile / unready button): HANDLED, but nothing queued. The original is
  //      still suppressed, matching JS line 283 `return;`.
  { Scene s; wire(s, 1);
    s.profile.disabled = 1;
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(take_sample().active == 0 && g_live.empty());  // nothing queued
  }
  { Scene s; wire(s, 1);
    s.button.interactable = 0;  // choose button not ready
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(take_sample().active == 0 && g_live.empty());
  }
  // 10c. Mask off -> NOT eligible -> ORIGINAL runs (host sends normally).
  { Scene s; wire(s, 1);
    assert(api->update(0, 0, tid) == PGR_OG_OK);
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_ORIGINAL);
    assert(take_sample().active == 0 && g_live.empty());
  }

  // 11. Reentrant STOP during the SendGift invoke (with a reserved sticker): revoke now, deferred
  //     teardown at the outermost frame returns the reservation once and frees every root.
  { Scene s; wire(s, 1);
    FakeSticker s1{&cls_sticker, &s.stk, 5};
    s.items[0] = &s1; s.list.items = s.items; s.list.count = 1;
    assert(api->update(PGR_OG_MASK_SEND | PGR_OG_MASK_STICKER, 2000, tid) == PGR_OG_OK);
    g_send_calls = 0;
    g_stop_on_send = true;  // reentrant tick(stopping) inside SendGift
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->tick(tid, 0) == PGR_OG_OK);
    assert(g_send_calls == 1);        // the send was issued before the stop latched
    assert(s.svc.reserves == 1 && s.svc.returns == 1);  // reserved then returned exactly once
    PgrOutgoingGiftsSample sm = take_sample();
    assert(sm.active == 0 && sm.roots == 0 && g_live.empty());  // deferred teardown ran, no leaks
    drain_all();
  }

  // 12. Wrong thread cannot drive the controller nor steal ownership.
  std::thread([&]() {
    Scene s; wire(s, 1);
    assert(g_api->queue(&s.panel, &s.profile, this_tid()) == PGR_OG_QUEUE_ORIGINAL);  // not owner
    assert(g_api->tick(this_tid(), 0) == PGR_OG_OK);  // no-op off the owner
    assert(g_api->observe(PGR_OG_OBSERVE_PROFILE_CLOSE, &s.profile, nullptr, this_tid()) == PGR_OG_OK);
  }).join();
  { Scene s; wire(s, 1);
    assert(api->queue(&s.panel, &s.profile, tid + 1) == PGR_OG_QUEUE_ORIGINAL);  // mismatched proven_tid
  }
  assert(g_live.empty());

  // ---- corrected-implementation reentry fixtures (13-20) ----------------------------

  // 13. A STOP latched by a reentrant tick DURING a queue() getter blocks the arm: gate_live sees
  //     g_pending_stop, so nothing is published (no PREPARING, active stays 0) and roots are freed.
  { Scene s; wire(s, 1);
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    g_oneshot_method = &m_disabled;
    g_oneshot = [&]() { g_api->tick(g_owner, 1); };  // reentrant stop mid queue getter
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    PgrOutgoingGiftsSample sm = take_sample();
    assert(sm.active == 0 && sm.roots == 0 && g_live.empty());  // never armed/published
    assert(!saw(drain_all(), PGR_OG_EVENT_STATUS, PGR_OG_MSG_PREPARING));
  }

  // 14. An unrelated profile's close is ignored: the rooted profile mismatches, so an unissued
  //     transaction survives it. A later stop clears it.
  { Scene s; wire(s, 1);
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    FakeProfile other{&cls_profile, &s.panel, 0};
    assert(api->observe(PGR_OG_OBSERVE_PROFILE_CLOSE, &other, nullptr, tid) == PGR_OG_OK);
    assert(take_sample().active == 1);  // unrelated close ignored
    assert(api->tick(tid, 1) == PGR_OG_OK);
    assert(take_sample().active == 0 && g_live.empty());
    drain_all();
  }

  // 15. A matching profile close fired DURING the tick inventory scan cancels the (unissued) txn
  //     without freeing inside the reentrant invoke (deferred to the outer frame): no reserve, no
  //     send; everything freed by the pass end.
  { Scene s; wire(s, 1);
    FakeSticker s1{&cls_sticker, &s.stk, 3};
    s.items[0] = &s1; s.list.items = s.items; s.list.count = 1;
    assert(api->update(PGR_OG_MASK_SEND | PGR_OG_MASK_STICKER, 2000, tid) == PGR_OG_OK);
    g_send_calls = 0;
    g_oneshot_method = &m_scount;  // during the sticker inventory count getter
    g_oneshot = [&]() { g_api->observe(PGR_OG_OBSERVE_PROFILE_CLOSE, &s.profile, nullptr, g_owner); };
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->tick(tid, 0) == PGR_OG_OK);
    assert(g_send_calls == 0 && s.svc.reserves == 0 && s.svc.returns == 0);  // abandoned pre-reserve
    assert(take_sample().active == 0 && g_live.empty());
    drain_all();
  }

  // 16. ReserveStickers throws: no send is issued and the latched local reservation is released
  //     exactly once via cleanup (reserved-before-invoke latch -> one ReturnReservedStickers).
  { Scene s; wire(s, 1);
    FakeSticker s1{&cls_sticker, &s.stk, 4};
    s.items[0] = &s1; s.list.items = s.items; s.list.count = 1;
    assert(api->update(PGR_OG_MASK_SEND | PGR_OG_MASK_STICKER, 2000, tid) == PGR_OG_OK);
    g_send_calls = 0;
    g_throw_on = &m_reserve;
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->tick(tid, 0) == PGR_OG_OK);
    assert(g_send_calls == 0);
    assert(s.svc.returns == 1);  // released exactly once
    assert(saw(drain_all(), PGR_OG_EVENT_ERROR, PGR_OG_ERR_INVOKE));
    assert(take_sample().active == 0 && g_live.empty());
    g_throw_on = nullptr;
  }

  // 17. A STOP latched by a reentrant tick DURING ReserveStickers: the reserve completed, but the
  //     send gate then sees g_pending_stop, so no send is issued and the reservation is returned
  //     exactly once.
  { Scene s; wire(s, 1);
    FakeSticker s1{&cls_sticker, &s.stk, 4};
    s.items[0] = &s1; s.list.items = s.items; s.list.count = 1;
    assert(api->update(PGR_OG_MASK_SEND | PGR_OG_MASK_STICKER, 2000, tid) == PGR_OG_OK);
    g_send_calls = 0;
    g_oneshot_method = &m_reserve;
    g_oneshot = [&]() { g_api->tick(g_owner, 1); };  // reentrant stop just after reserve
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->tick(tid, 0) == PGR_OG_OK);
    assert(g_send_calls == 0);
    assert(s.svc.reserves == 1 && s.svc.returns == 1);  // reserved then returned once, no send
    assert(take_sample().active == 0 && g_live.empty());
    drain_all();
  }

  // 18. A foreign thread's STOP latches globally (revoke-any-thread) but defers teardown to the
  //     owner's next tick; a foreign thread's update cannot rearm (owner mismatch -> mask zeroed).
  { Scene s; wire(s, 1);
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);  // unissued txn active
    std::thread([&]() {
      const uint64_t ftid = this_tid();
      assert(g_api->tick(ftid, 1) == PGR_OG_OK);                               // latch pending stop
      assert(g_api->update(PGR_OG_MASK_SEND, 2000, ftid) == PGR_OG_OK);        // cannot rearm
    }).join();
    assert(take_sample().active == 1);   // teardown deferred to the owner
    assert(api->tick(tid, 0) == PGR_OG_OK);  // owner next tick clears
    assert(take_sample().active == 0 && g_live.empty());
    drain_all();
  }

  // 19. A reentrant queue() DURING cleanup (ReturnReservedStickers invoke) returns HANDLED and does
  //     NOT rearm: the clean-in-progress gate blocks it, so the rearm cannot survive the free.
  { Scene s; wire(s, 1);
    FakeSticker s1{&cls_sticker, &s.stk, 4};
    s.items[0] = &s1; s.list.items = s.items; s.list.count = 1;
    assert(api->update(PGR_OG_MASK_SEND | PGR_OG_MASK_STICKER, 2000, tid) == PGR_OG_OK);
    g_send_calls = 0;
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->tick(tid, 0) == PGR_OG_OK);  // issued, reserved
    assert(s.svc.reserves == 1);
    assert(api->observe(PGR_OG_OBSERVE_RPC_RESULT, &s.cb, &s.sendout, tid) == PGR_OG_OK);
    assert(api->observe(PGR_OG_OBSERVE_RPC_FINALLY, &s.cb, nullptr, tid) == PGR_OG_OK);
    g_reentrant_queue_result = 999;
    g_oneshot_method = &m_return;  // fires inside clear_txn's ReturnReservedStickers
    g_oneshot = [&]() { g_reentrant_queue_result = g_api->queue(&s.panel, &s.profile, g_owner); };
    assert(api->tick(tid, 0) == PGR_OG_OK);  // completes -> clear returns reservation
    assert(g_reentrant_queue_result == PGR_OG_QUEUE_HANDLED);  // blocked during cleanup
    assert(s.svc.returns == 1);
    assert(take_sample().active == 0 && g_live.empty());  // rearm did not survive
    drain_all();
  }

  // 20. The SendGift promise stays rooted after issue (and across a bookkeeping tick) until a
  //     matching finally completes it; cleanup then frees the promise root.
  { Scene s; wire(s, 1);
    FakePromise_t prom{&cls_promise};
    g_promise = &prom;
    assert(api->update(PGR_OG_MASK_SEND, 2000, tid) == PGR_OG_OK);
    assert(api->queue(&s.panel, &s.profile, tid) == PGR_OG_QUEUE_HANDLED);
    assert(api->tick(tid, 0) == PGR_OG_OK);  // issued; promise rooted
    assert(take_sample().issued == 1);
    assert(live_contains(&prom));            // rooted after issue
    assert(api->tick(tid, 0) == PGR_OG_OK);  // bookkeeping-only pass
    assert(live_contains(&prom));            // still rooted
    assert(api->observe(PGR_OG_OBSERVE_RPC_RESULT, &s.cb, &s.sendout, tid) == PGR_OG_OK);
    assert(api->observe(PGR_OG_OBSERVE_RPC_FINALLY, &s.cb, nullptr, tid) == PGR_OG_OK);
    assert(api->tick(tid, 0) == PGR_OG_OK);  // completes -> promise root freed
    assert(!live_contains(&prom) && take_sample().active == 0 && g_live.empty());
    g_promise = nullptr;
    drain_all();
  }
  assert(g_live.empty());

  std::printf("outgoing_gifts_test passed: ABI/validation/idempotence, queue+issue (no sticker), "
              "matched sync result/finally+SENT, already-pending, sticker reserve/return once + "
              "Dispose once, no-gifts, sticker scan bound, unmatched callback (service/recipient), "
              "issued-timeout quarantine (real clock), off before/after issue, profile close both, "
              "reentrant STOP during send (deferred teardown, reservation returned once, no leaks), "
              "wrong thread; reentry fixtures: STOP during queue getter (no publish), unrelated "
              "profile close ignored, matching profile close during inventory scan (cancel, no free "
              "under invoke, no send), reserve throws (released once, no send), STOP during reserve "
              "(no send, returned once), foreign-thread STOP defers to owner + foreign update cannot "
              "rearm, reentrant queue during cleanup returns HANDLED (no rearm survives), promise "
              "rooted until finally\n");
  return 0;
}

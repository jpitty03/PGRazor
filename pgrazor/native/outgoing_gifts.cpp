#include "outgoing_gifts.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 22 outgoing bundled-gift controller. See outgoing_gifts.h for the ABI and the division of
// labour with runtime/native-outgoing-gifts.js. Built -fno-exceptions / -fno-rtti like every
// sibling: the JS "throw" paths become fail-closed error records; every GC root is freed exactly
// once on the owner thread; a reserved sticker is returned exactly once. No mutex is ever held
// across a managed call, a GC-handle call or the SendGift invoke. This module installs NO hook.
//
// Reentrancy model (same conservatism as incoming_gifts.cpp -- a gc_get()/field read/invoke may
// re-enter the owner thread through il2cpp instrumentation or a relocating collector):
//   * Every managed reference held across a managed call is strongly rooted and re-resolved from its
//     root afterwards; no raw pointer is reused across a potential safepoint.
//   * The single pending transaction is owner-thread-only. "issued" is latched BEFORE the SendGift
//     invoke, so an uncertain send is never retried. The result/finally observers only SET flags
//     (never free), so a callback that completes synchronously inside the SendGift invoke cannot
//     free the locals the issuing frame is still using.
//   * A transaction epoch bumps on queue (new txn) and on clear. Every acting pass pins the epoch
//     before its first managed lookup and abandons if it changed. clear_txn() detaches the whole
//     transaction (active=false) BEFORE the ReturnReservedStickers invoke and
//     the root frees, so a reentrant clear/stop is a no-op and nothing is double-freed/returned.
//   * STOP revokes immediately (any thread disarms via the gate); the owner-thread teardown is
//     deferred to the outermost frame (leave_frame) so a nested stop during the SendGift invoke
//     never frees the issuing frame's locals. queue() and re-arm are blocked while a stop is latched.
//   * sample() reads atomic mirrors published on the owner thread, never the owner-thread Txn.

static_assert(sizeof(PgrOutgoingGiftsConfig) == 488, "config must be 488 bytes");
static_assert(sizeof(PgrOutgoingGiftsSample) == 88, "sample must be 88 bytes");
static_assert(sizeof(PgrOutgoingGiftsEvent) == 16, "event must be 16 bytes");
static_assert(sizeof(PgrOutgoingGiftsApi) == 64, "table must be 64 bytes");
static_assert(offsetof(PgrOutgoingGiftsConfig, gchandle_new) == 8, "layout");
static_assert(offsetof(PgrOutgoingGiftsConfig, m_panel) == 96, "layout");
static_assert(offsetof(PgrOutgoingGiftsConfig, m_alive) == 232, "layout");
static_assert(offsetof(PgrOutgoingGiftsConfig, class_panel) == 240, "layout");
static_assert(offsetof(PgrOutgoingGiftsConfig, class_string) == 344, "layout");
static_assert(offsetof(PgrOutgoingGiftsConfig, f_data) == 352, "layout");
static_assert(offsetof(PgrOutgoingGiftsConfig, f_callback_id) == 472, "layout");
static_assert(offsetof(PgrOutgoingGiftsConfig, check_success) == 480, "layout");
static_assert(offsetof(PgrOutgoingGiftsConfig, send_success) == 484, "layout");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_OG_MAX_LEASE_MS;

// --- il2cpp runtime ABI typedefs ---------------------------------------------------
using GcNew = uintptr_t (*)(void *obj, int pinned);
using GcGet = void *(*)(uintptr_t handle);
using GcFree = void (*)(uintptr_t handle);
using ObjClass = void *(*)(void *obj);
using Unbox = void *(*)(void *obj);
using GetVirtual = void *(*)(void *obj, const void *method);
using Invoke = void *(*)(const void *method, void *obj, void **params, void **exc);
using FieldGet = void (*)(void *obj, void *field, void *out);
using StringLen = int (*)(void *str);
using StringChars = uint16_t *(*)(void *str);

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrOutgoingGiftsConfig g_cfg{};
std::atomic<bool> g_installed{false};
bool g_install_attempted = false;
int g_install_error = PGR_OG_ERR_NOT_INSTALLED;

// Permanent Unity owner, latched once from a genuine on-thread queue()/observe()/tick().
std::atomic<uint64_t> g_owner_tid{0};

// Finish/root accounting (atomic so sample is lock-free for these).
std::atomic<uint32_t> g_roots{0};         // durable GC roots currently held
std::atomic<uint32_t> g_active_depth{0};  // owner-thread reentrancy depth

// Cross-thread sample mirrors (published on the owner thread; read lock-free by sample()).
std::atomic<uint32_t> g_m_active{0};
std::atomic<uint32_t> g_m_issued{0};
std::atomic<uint32_t> g_m_result_known{0};
std::atomic<int32_t> g_m_result{0};
std::atomic<uint32_t> g_m_timed_out{0};

// --- gate / telemetry (guarded by g_state_mutex) -----------------------------------
uint32_t g_mask = 0;
uint64_t g_lease_until_ms = 0;
uint64_t g_gen = 1;  // revocation generation; bumped on disable / dead-lease refresh
bool g_fault = false;
bool g_overflow = false;
uint64_t g_request_serial = 0, g_result_serial = 0, g_error_serial = 0, g_status_serial = 0,
         g_last_tid = 0;

PgrOutgoingGiftsEvent g_fifo[PGR_OG_EVENT_CAP];
uint32_t g_fifo_head = 0, g_fifo_count = 0;

// --- owner-thread reentrancy guards ------------------------------------------------
std::atomic<bool> g_pending_stop{false};
bool g_cancel_pending=false;
std::atomic<bool> g_cleaning{false};  // a reentrant stopping tick requested a deferred teardown
uint64_t g_txn_epoch = 0;     // bumps on each queue (new txn) and each clear

// --- single pending transaction (owner-thread only) --------------------------------
struct Txn {
  bool active = false;
  bool issued = false;
  bool finished = false;
  bool has_result = false;
  int32_t result = 0;
  bool reserved = false;   // a sticker was reserved (must be returned once)
  bool timed_out = false;
  uint64_t deadline_ms = 0;
  uintptr_t panel_root = 0;
  uintptr_t profile_root = 0;
  uintptr_t friend_root = 0;
  uintptr_t promise_root=0, request_sticker_root=0, request_player_id_root=0, request_nia_id_root=0;
  uintptr_t rpc_root = 0;          // the rpc service (for the ownedCallback identity match)
  uintptr_t sticker_svc_root = 0;  // the sticker service (for ReturnReservedStickers)
  uintptr_t sticker_id_root = 0;   // the reserved sticker's StickerId string
  uint16_t id_chars[PGR_OG_ID_MAX];  // bounded recipient id content (captured at queue)
  int id_len = 0;
};
Txn g_txn{};

// --- clock / tid -------------------------------------------------------------------
uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
            static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

// Typed accessors onto the immutable published config.
GcNew gc_new() { return reinterpret_cast<GcNew>(g_cfg.gchandle_new); }
GcGet gc_get() { return reinterpret_cast<GcGet>(g_cfg.gchandle_get_target); }
GcFree gc_free_raw() { return reinterpret_cast<GcFree>(g_cfg.gchandle_free); }
ObjClass obj_class() { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }
Unbox unbox() { return reinterpret_cast<Unbox>(g_cfg.object_unbox); }
GetVirtual get_virtual() { return reinterpret_cast<GetVirtual>(g_cfg.object_get_virtual_method); }
void *managed_invoke(const void *method,void *obj,void **params,void **exc){
  *exc=nullptr;return reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(method,obj,params,exc);
}
Invoke invoke_fn(){return &managed_invoke;}
FieldGet field_get() { return reinterpret_cast<FieldGet>(g_cfg.field_get_value); }

bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }

// --- root bookkeeping --------------------------------------------------------------
uintptr_t hold(void *obj) {
  if (obj == nullptr) return 0;
  uintptr_t h = gc_new()(obj, 0);
  if (h != 0) g_roots.fetch_add(1, std::memory_order_relaxed);
  return h;
}
void drop(uintptr_t *h) {
  if (*h == 0) return;
  uintptr_t v = *h;
  *h = 0;  // detach before the (possibly reentrant) free
  gc_free_raw()(v);
  g_roots.fetch_sub(1, std::memory_order_relaxed);
}

void publish_txn() {
  g_m_active.store(g_txn.active ? 1u : 0u, std::memory_order_relaxed);
  g_m_issued.store(g_txn.issued ? 1u : 0u, std::memory_order_relaxed);
  g_m_result_known.store(g_txn.has_result ? 1u : 0u, std::memory_order_relaxed);
  g_m_result.store(g_txn.result, std::memory_order_relaxed);
  g_m_timed_out.store(g_txn.timed_out ? 1u : 0u, std::memory_order_relaxed);
}

// --- field readers (object must be rooted by the caller) ---------------------------
void *field_ptr(void *obj, void *field) {
  void *out = nullptr;
  if (obj != nullptr) field_get()(obj, field, &out);
  return out;
}
int32_t field_i32(void *obj, void *field) {
  int32_t v = 0;
  if (obj != nullptr) field_get()(obj, field, &v);
  return v;
}
uint64_t field_u64(void *obj, void *field) {
  uint64_t v = 0;
  if (obj != nullptr) field_get()(obj, field, &v);
  return v;
}

// --- managed invoke helpers (no mutex held) ----------------------------------------
// Instance bool getter: root the boxed return, verify System.Boolean, unbox the byte.
bool invoke_bool(void *method, void *obj, bool *ok) {
  void *exc = nullptr;
  void *r = invoke_fn()(method, obj, nullptr, &exc);
  if (exc != nullptr || r == nullptr) { *ok = false; return false; }
  uintptr_t root = hold(r);
  if (root == 0) { *ok = false; return false; }
  void *bt = gc_get()(root);
  bool value = false;
  bool good = bt != nullptr && obj_class()(bt) == g_cfg.class_bool;
  if (good) {
    uint8_t *d = static_cast<uint8_t *>(unbox()(bt));
    good = d != nullptr && *d <= 1;
    value = good && *d == 1;
  }
  drop(&root);
  *ok = good;
  return value;
}

// Instance int getter with one arg (or none). Returns the unboxed Int32, or `fallback` on failure.
int32_t invoke_int(void *method, void *obj, void **params, bool *ok, int32_t fallback) {
  void *exc = nullptr;
  void *r = invoke_fn()(method, obj, params, &exc);
  if (exc != nullptr || r == nullptr) { *ok = false; return fallback; }
  uintptr_t root = hold(r);
  if (root == 0) { *ok = false; return fallback; }
  void *bt = gc_get()(root);
  int32_t value = fallback;
  bool good = bt != nullptr && obj_class()(bt) == g_cfg.class_int32;
  if (good) {
    int32_t *d = static_cast<int32_t *>(unbox()(bt));
    good = d != nullptr;
    if (good) value = *d;
  }
  drop(&root);
  *ok = good;
  return value;
}

// Static op_Implicit(UnityEngine.Object) -> bool. The reference arg is the object pointer itself.
bool is_alive(void *obj) {
  if (obj == nullptr) return false;
  void *params[1] = {obj};
  void *exc = nullptr;
  void *r = invoke_fn()(g_cfg.m_alive, nullptr, params, &exc);
  if (exc != nullptr || r == nullptr) return false;
  uintptr_t root = hold(r);
  if (root == 0) return false;
  void *bt = gc_get()(root);
  bool value = false;
  if (bt != nullptr && obj_class()(bt) == g_cfg.class_bool) {
    uint8_t *d = static_cast<uint8_t *>(unbox()(bt));
    value = d != nullptr && *d == 1;
  }
  drop(&root);
  return value;
}

// Resolve a virtual method against the rooted object, then invoke it. *threw reports a managed
// exception; a null concrete method is treated as a throw. Returns the raw (unrooted) result.
void *invoke_virtual(void *method, uintptr_t root, void **params, bool *threw,
                     uintptr_t argument_root = 0) {
  void *obj=gc_get()(root);
  *threw = true;
  if (obj == nullptr) return nullptr;
  void *concrete = get_virtual()(obj, method);
  if (concrete == nullptr) return nullptr;
  void *exc = nullptr;
  obj=gc_get()(root);if(!obj)return nullptr;
  if (argument_root) {
    params[0] = gc_get()(argument_root);
    if (!params[0]) return nullptr;
  }
  void *r = invoke_fn()(concrete, obj, params, &exc);
  *threw = exc != nullptr;
  return r;
}

// giftButton: present, alive, active-and-enabled and interactable (JS giftButton). Rooted internally.
bool gift_button(void *gui, void *field) {
  void *button = field_ptr(gui, field);
  if (button == nullptr) return false;
  uintptr_t root = hold(button);
  if (root == 0) return false;
  bool result = false;
  void *b = gc_get()(root);
  if (b != nullptr && is_alive(b)) {
    bool ok = false;
    b = gc_get()(root);
    bool active = b != nullptr && invoke_bool(g_cfg.m_active, b, &ok) && ok;
    if (active) {
      b = gc_get()(root);
      result = b != nullptr && invoke_bool(g_cfg.m_interactable, b, &ok) && ok;
    }
  }
  drop(&root);
  return result;
}

// Read up to PGR_OG_ID_MAX UTF-16 units of an exact System.String; false if absent/out of window.
bool read_id(void *str,uint16_t *out,int *out_len){
  uintptr_t root=hold(str);if(!root)return false;
  bool ok=false;void *current=gc_get()(root);
  if(is_exact(current,g_cfg.class_string)){
    int n=reinterpret_cast<StringLen>(g_cfg.string_length)(gc_get()(root));
    if(n>=1&&n<=PGR_OG_ID_MAX){
      auto src=reinterpret_cast<StringChars>(g_cfg.string_chars)(gc_get()(root));
      if(src){std::memcpy(out,src,size_t(n)*sizeof(uint16_t));*out_len=n;ok=true;}
    }
  }
  drop(&root);return ok;
}

// Compare a managed string's content against the captured recipient id (bounded exact match).
bool id_matches(void *str) {
  uint16_t buf[PGR_OG_ID_MAX];
  int len = 0;
  if (!read_id(str, buf, &len)) return false;
  return len == g_txn.id_len &&
         std::memcmp(buf, g_txn.id_chars, static_cast<size_t>(len) * sizeof(uint16_t)) == 0;
}

// --- gate / telemetry --------------------------------------------------------------
uint32_t root_total() {
  return g_roots.load(std::memory_order_relaxed) + g_active_depth.load(std::memory_order_relaxed);
}
uint64_t pin_gen() {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  return g_gen;
}
// Fresh-clock gate: need_bits (e.g. SEND, or SEND|STICKER) must be set, lease live, gen unchanged,
// owner thread, not faulted. Fails closed.
bool gate_live(uint64_t pinned_gen, uint64_t tid, uint32_t need_bits) {
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return false;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_fault || g_gen != pinned_gen || g_pending_stop.load() || g_cleaning.load() || g_cancel_pending) return false;
  if ((g_mask & need_bits) != need_bits) return false;
  if (!(g_lease_until_ms != 0 && now < g_lease_until_ms)) return false;
  return current_tid()==tid && g_owner_tid.load(std::memory_order_acquire) == tid;
}

void push_event(uint32_t kind, int32_t id, int32_t aux, uint64_t tid) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_last_tid = tid;
  if (kind == PGR_OG_EVENT_REQUEST) ++g_request_serial;
  else if (kind == PGR_OG_EVENT_RESULT) ++g_result_serial;
  else if (kind == PGR_OG_EVENT_ERROR) ++g_error_serial;
  else if (kind == PGR_OG_EVENT_STATUS) ++g_status_serial;
  if (g_fifo_count >= PGR_OG_EVENT_CAP) {
    g_overflow = true;
    g_fault = true;  // fail closed: no further issue while faulted
    uint32_t last = (g_fifo_head + g_fifo_count - 1) % PGR_OG_EVENT_CAP;
    g_fifo[last] = PgrOutgoingGiftsEvent{PGR_OG_EVENT_ERROR, PGR_OG_ERR_OVERFLOW, 0, 0};
    return;
  }
  uint32_t idx = (g_fifo_head + g_fifo_count) % PGR_OG_EVENT_CAP;
  g_fifo[idx] = PgrOutgoingGiftsEvent{kind, id, aux, 0};
  ++g_fifo_count;
}
void emit_status(int32_t msg, int32_t aux, uint64_t tid) { push_event(PGR_OG_EVENT_STATUS, msg, aux, tid); }
void emit_error(int32_t reason, uint64_t tid) { push_event(PGR_OG_EVENT_ERROR, reason, 0, tid); }

// --- owner latching ----------------------------------------------------------------
bool owner_ready(uint64_t tid) {
  if (tid == 0) return false;
  uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (!(owner == 0 || owner == tid)) return false;
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
  return current_tid()==tid && g_owner_tid.load(std::memory_order_acquire) == tid;
}

// --- transaction teardown (owner thread) -------------------------------------------
// Detaches the whole transaction and returns any reserved sticker EXACTLY ONCE. active=false is
// set BEFORE the ReturnReservedStickers invoke and the root frees, so a reentrant
// clear/finally/stop fired from inside those calls is a no-op (no double return, no double free).
void clear_txn(uint64_t tid) {
  if (!g_txn.active) return;
  g_cleaning.store(true);
  // Snapshot the reservation before detaching.
  const bool do_return = g_txn.reserved && g_txn.sticker_svc_root != 0 &&
                         g_txn.sticker_id_root != 0;
  uintptr_t svc = g_txn.sticker_svc_root, sid = g_txn.sticker_id_root;
  uintptr_t panel = g_txn.panel_root, profile = g_txn.profile_root, friendr = g_txn.friend_root,
            rpc = g_txn.rpc_root;
  uintptr_t promise=g_txn.promise_root,sticker=g_txn.request_sticker_root,pid=g_txn.request_player_id_root,nid=g_txn.request_nia_id_root;
  // Detach: a reentrant clear now returns immediately; bump the epoch so acting passes abandon.
  g_txn = Txn{};
  ++g_txn_epoch;
  publish_txn();
  if (do_return) {
    // Re-resolve the service/id from their roots across the invoke; ignore a throw (JS catch).
    void *stock = gc_get()(svc);
    void *stickerId = gc_get()(sid);
    if (stock != nullptr && stickerId != nullptr) {
      int32_t one = PGR_OG_RESERVE_COUNT;
      void *params[2] = {stickerId, &one};
      void *exc = nullptr;
      invoke_fn()(g_cfg.m_return_sticker, stock, params, &exc);
      if (exc != nullptr) emit_error(PGR_OG_ERR_STICKER_RELEASE, tid);
    }
  }
  drop(&svc);
  drop(&sid);
  drop(&panel);
  drop(&profile);
  drop(&friendr);
  drop(&rpc);drop(&promise);drop(&sticker);drop(&pid);drop(&nid);
  publish_txn();g_cleaning.store(false);
}

// Run the deferred teardown requested by a reentrant stopping tick, then drop the active depth. Only
// the outermost frame performs the free; the stop latch stays set across it so no re-arm/queue
// survives, and the free happens BEFORE the depth decrement so root_total stays nonzero until done.
void leave_frame(uint64_t tid) {
  if (g_active_depth.load(std::memory_order_relaxed) == 1 && (g_pending_stop.load()||g_cancel_pending)) {
    clear_txn(tid);
    g_pending_stop = false;g_cancel_pending=false;
  }
  g_active_depth.fetch_sub(1, std::memory_order_relaxed);
}

// --- queue (sendReplacement + bundledQueue) ----------------------------------------
// Returns PGR_OG_QUEUE_HANDLED / PGR_OG_QUEUE_ORIGINAL. Must run inside an active frame.
int do_queue(void *panel, void *profile, uint64_t tid) {
  const uint64_t epoch=g_txn_epoch,gen=pin_gen();
  if (!is_exact(panel, g_cfg.class_panel) || profile == nullptr) return PGR_OG_QUEUE_ORIGINAL;

  uintptr_t panel_root = hold(panel);
  uintptr_t profile_root = hold(profile);
  if (panel_root == 0 || profile_root == 0) {
    drop(&panel_root);
    drop(&profile_root);
    return PGR_OG_QUEUE_ORIGINAL;
  }
  // Past the panel-class / profile-presence gate the host original is ALWAYS suppressed (JS line
  // 283 `return;`): a dead profile, identity mismatch, disabled button or unready button is HANDLED
  // but queues nothing. Only the two top checks above fall through to the original.
  int result = PGR_OG_QUEUE_HANDLED;

  // Profile-panel identity + disabled + choose button (JS line 283).
  void *pf = gc_get()(profile_root);
  bool identity_ok = false;
  if (pf != nullptr && is_alive(pf)) {
    void *exc = nullptr;
    pf = gc_get()(profile_root);
    void *cur_panel = pf != nullptr ? invoke_fn()(g_cfg.m_panel, pf, nullptr, &exc) : nullptr;
    void *live_panel = gc_get()(panel_root);
    identity_ok = exc == nullptr && cur_panel != nullptr && cur_panel == live_panel;
  }
  if (identity_ok) {
    bool ok = false;
    pf = gc_get()(profile_root);
    bool disabled=pf?invoke_bool(g_cfg.m_disabled,pf,&ok):true;
    void *lp = gc_get()(panel_root);
    bool button_ok = ok && !disabled && lp != nullptr && gift_button(lp, g_cfg.f_choose_button);
    if (!disabled && button_ok) {
      // Eligible: capture the recipient id and arm the transaction (bundledQueue).
      void *lpanel = gc_get()(panel_root);
      void *data = field_ptr(lpanel, g_cfg.f_data);
      void *frnd = field_ptr(data, g_cfg.f_panel_friend);
      uint16_t id_buf[PGR_OG_ID_MAX];
      int id_len = 0;
      bool id_ok = false;
      if (is_exact(frnd, g_cfg.class_friend)) {
        uintptr_t froot = hold(frnd);
        if (froot != 0) {
          void *fr = gc_get()(froot);
          void *player = field_ptr(fr, g_cfg.f_player);
          if (is_exact(player, g_cfg.class_player)) {
            uintptr_t proot = hold(player);
            if (proot != 0) {
              void *pl = gc_get()(proot);
              id_ok = read_id(field_ptr(pl, g_cfg.f_player_id), id_buf, &id_len);
              drop(&proot);
            }
          }
          // Keep the friend rooted in the transaction on success; else drop it.
          if (id_ok && epoch==g_txn_epoch && gate_live(gen,tid,PGR_OG_MASK_SEND)) {
            g_txn = Txn{};
            g_txn.active = true;
            g_txn.panel_root = panel_root;
            g_txn.profile_root = profile_root;
            g_txn.friend_root = froot;
            g_txn.id_len = id_len;
            std::memcpy(g_txn.id_chars, id_buf, static_cast<size_t>(id_len) * sizeof(uint16_t));
            g_txn.deadline_ms = 0;
            ++g_txn_epoch;
            publish_txn();
            emit_status(PGR_OG_MSG_PREPARING, 0, tid);
            return PGR_OG_QUEUE_HANDLED;  // roots now owned by the transaction
          }
          drop(&froot);
        }
      }
      // gift-recipient-missing (JS bundledQueue throw -> sendReplacement catch -> gift-error).
      emit_error(PGR_OG_ERR_RECIPIENT, tid);
    }
    // else: disabled / button not ready -> suppress original, queue nothing (result already HANDLED).
  }
  drop(&panel_root);
  drop(&profile_root);
  return result;
}

// --- bundledTick issue path --------------------------------------------------------
// Builds + issues the pending transaction. Returns via the transaction state; errors push events
// and either timeout-quarantine (if somehow issued) or clear.
void tick_issue(uint64_t tid, uint64_t pinned, uint64_t pass_epoch) {
  void *panel = gc_get()(g_txn.panel_root);
  void *profile=nullptr;
  // Recipient unchanged + panel/profile identity (JS line 117).
  if (panel == nullptr || !is_alive(panel)) { clear_txn(tid); return; }
  void *exc = nullptr;
  profile = gc_get()(g_txn.profile_root);
  void *cur_panel = profile != nullptr && is_alive(profile)
                        ? invoke_fn()(g_cfg.m_panel, gc_get()(g_txn.profile_root), nullptr, &exc)
                        : nullptr;
  panel = gc_get()(g_txn.panel_root);
  if (exc != nullptr || cur_panel == nullptr || cur_panel != panel) { clear_txn(tid); return; }
  panel = gc_get()(g_txn.panel_root);
  void *friend_now = field_ptr(field_ptr(panel, g_cfg.f_data), g_cfg.f_panel_friend);
  bool recipient_ok = false;
  if (is_exact(friend_now, g_cfg.class_friend)) {
    uintptr_t fr = hold(friend_now);
    if (fr != 0) {
      void *f = gc_get()(fr);
      void *player = field_ptr(f, g_cfg.f_player);
      recipient_ok = is_exact(player, g_cfg.class_player) && id_matches(field_ptr(player, g_cfg.f_player_id));
      drop(&fr);
    }
  }
  if (!recipient_ok) { clear_txn(tid); return; }

  // Check availability (JS line 119).
  panel = gc_get()(g_txn.panel_root);
  bool pending_flag = false;
  {
    uint8_t v = 0;
    field_get()(panel, g_cfg.f_check_pending, &v);
    pending_flag = v != 0;
  }
  panel = gc_get()(g_txn.panel_root);
  if (!gift_button(panel, g_cfg.f_choose_button) || pending_flag ||
      field_i32(gc_get()(g_txn.panel_root), g_cfg.f_check_result) != g_cfg.check_success) {
    emit_status(PGR_OG_MSG_NOT_AVAILABLE, 0, tid);
    clear_txn(tid);
    return;
  }

  // Services (JS line 121).
  panel = gc_get()(g_txn.panel_root);
  void *service = field_ptr(panel, g_cfg.f_panel_gifting);
  uintptr_t service_root=hold(service);
  void *rpc = field_ptr(gc_get()(g_txn.panel_root), g_cfg.f_panel_rpc);
  if (!service_root || !is_exact(gc_get()(service_root), g_cfg.class_service) || !is_exact(rpc, g_cfg.class_rpc)) {
    drop(&service_root);
    emit_error(PGR_OG_ERR_SERVICE_TYPE, tid);
    clear_txn(tid);
    return;
  }
  g_txn.rpc_root = hold(rpc);
  if (service_root == 0 || g_txn.rpc_root == 0) {
    emit_error(PGR_OG_ERR_GCHANDLE, tid);
    drop(&service_root);
    clear_txn(tid);
    return;
  }

  // First sendable gift via the virtual enumerator, with a guaranteed Dispose (JS 123-129).
  bool threw = false;
  void *svc = gc_get()(service_root);
  void *iterable = svc != nullptr ? invoke_fn()(g_cfg.m_sendable, svc, nullptr, &exc) : nullptr;
  if (exc != nullptr || iterable == nullptr) { emit_error(PGR_OG_ERR_INVENTORY, tid); drop(&service_root); clear_txn(tid); return; }
  uintptr_t iter_root = hold(iterable);
  uintptr_t enum_root = 0, gift_root = 0;
  uint32_t enum_err = PGR_OG_ERR_NONE;
  if (iter_root != 0) {
    void *enumerator = invoke_virtual(g_cfg.m_get_enumerator, iter_root, nullptr, &threw);
    if (!threw && enumerator != nullptr) {
      enum_root = hold(enumerator);
      if (enum_root != 0) {
        void *moved = invoke_virtual(g_cfg.m_move_gift, enum_root, nullptr, &threw);
        bool has = false;
        if (!threw && moved != nullptr) {
          uintptr_t mroot = hold(moved);
          if (mroot != 0) {
            void *mb = gc_get()(mroot);
            if (mb != nullptr && obj_class()(mb) == g_cfg.class_bool) {
              uint8_t *d = static_cast<uint8_t *>(unbox()(gc_get()(mroot)));
              if (d && *d <= 1) has = *d == 1;
              else enum_err = PGR_OG_ERR_INVENTORY;
            } else enum_err = PGR_OG_ERR_INVENTORY;
            drop(&mroot);
          } else enum_err = PGR_OG_ERR_GCHANDLE;
        } else {
          enum_err = PGR_OG_ERR_INVENTORY;
        }
        if (has) {
          void *gift = invoke_virtual(g_cfg.m_current_gift, enum_root, nullptr, &threw);
          if (!threw && gift != nullptr){gift_root=hold(gift);if(!gift_root)enum_err=PGR_OG_ERR_GCHANDLE;}else enum_err=PGR_OG_ERR_INVENTORY;
        }
        // Guaranteed Dispose of the enumerator (always, even on the throw paths above).
        bool dthrew = false;
        invoke_virtual(g_cfg.m_dispose_gift, enum_root, nullptr, &dthrew);if(dthrew)enum_err=PGR_OG_ERR_INVENTORY;
      } else {
        enum_err = PGR_OG_ERR_GCHANDLE;
      }
    } else {
      enum_err = PGR_OG_ERR_INVENTORY;
    }
  } else {
    enum_err = PGR_OG_ERR_GCHANDLE;
  }
  drop(&iter_root);
  drop(&enum_root);
  drop(&service_root);

  if (enum_err != PGR_OG_ERR_NONE) { emit_error(enum_err, tid); drop(&gift_root); clear_txn(tid); return; }
  if (gift_root == 0) { emit_status(PGR_OG_MSG_NO_GIFTS, 0, tid); clear_txn(tid); return; }
  void *gift = gc_get()(gift_root);
  if (!is_exact(gift, g_cfg.class_gift_box)) { emit_error(PGR_OG_ERR_INVENTORY, tid); drop(&gift_root); clear_txn(tid); return; }
  uint64_t gift_id = field_u64(gc_get()(gift_root), g_cfg.f_gift_id);
  if (gift_id == 0) { emit_error(PGR_OG_ERR_GIFT_ID_ZERO, tid); drop(&gift_root); clear_txn(tid); return; }

  // Recipient player / playerId / niaId (JS 133-135).
  void *frnd = gc_get()(g_txn.friend_root);
  void *player = field_ptr(frnd, g_cfg.f_player);
  uintptr_t player_root = hold(player);
  void *playerId = player_root != 0 ? field_ptr(gc_get()(player_root), g_cfg.f_player_id) : nullptr;
  uintptr_t playerId_root = hold(playerId);
  void *niaUtil = field_ptr(gc_get()(g_txn.panel_root), g_cfg.f_nia_util);
  if (player_root == 0 || playerId_root == 0 || niaUtil == nullptr) {
    emit_error(PGR_OG_ERR_NIA, tid);
    drop(&player_root); drop(&playerId_root); drop(&gift_root);
    clear_txn(tid);
    return;
  }
  uintptr_t nia_root = hold(niaUtil);
  void *nia = nia_root != 0 ? gc_get()(nia_root) : nullptr;
  void *pl = gc_get()(player_root);
  void *nia_params[1] = {pl};
  void *niaId = nia != nullptr ? invoke_virtual(g_cfg.m_nia_id, nia_root, nia_params, &threw, player_root) : nullptr;
  uintptr_t niaId_root = (!threw && niaId != nullptr) ? hold(niaId) : 0;
  bool nia_ok = niaId_root != 0;
  {
    uint16_t tmp[PGR_OG_ID_MAX]; int tl = 0;
    nia_ok = nia_ok && read_id(gc_get()(niaId_root), tmp, &tl);
  }
  drop(&nia_root);
  if (!nia_ok) {
    emit_error(PGR_OG_ERR_NIA, tid);
    drop(&player_root); drop(&playerId_root); drop(&niaId_root); drop(&gift_root);
    clear_txn(tid);
    return;
  }

  // Optional sticker (JS 137-145). Reserve exactly one with quantity > 0. The sticker bit is read
  // through the locked gate (never an unsynchronized g_mask read); it is re-checked before reserve.
  uintptr_t sticker_root = 0;
  bool want_sticker = gate_live(pinned, tid, PGR_OG_MASK_SEND | PGR_OG_MASK_STICKER);
  if (want_sticker) {
    void *rpc_obj = gc_get()(g_txn.rpc_root);
    void *stock = field_ptr(rpc_obj, g_cfg.f_rpc_stickers);
    if (!is_exact(stock, g_cfg.class_sticker_svc)) {
      emit_error(PGR_OG_ERR_STICKER_SVC, tid);
      drop(&player_root); drop(&playerId_root); drop(&niaId_root); drop(&gift_root);
      clear_txn(tid);
      return;
    }
    g_txn.sticker_svc_root = hold(stock);
    if (!g_txn.sticker_svc_root) {
      emit_error(PGR_OG_ERR_GCHANDLE, tid);
      drop(&player_root); drop(&playerId_root); drop(&niaId_root); drop(&gift_root);
      clear_txn(tid);
      return;
    }
    uint8_t zero = 0;
    void *inv_params[1] = {&zero};
    void *stk = gc_get()(g_txn.sticker_svc_root);
    void *inventory = stk != nullptr ? invoke_fn()(g_cfg.m_inventory, stk, inv_params, &exc) : nullptr;
    if (exc != nullptr || !is_exact(inventory, g_cfg.class_sticker_list)) {
      emit_error(PGR_OG_ERR_STICKER_SVC, tid);
      drop(&player_root); drop(&playerId_root); drop(&niaId_root); drop(&gift_root);
      clear_txn(tid);
      return;
    }
    uintptr_t inv_root = hold(inventory);
    bool ok = false;
    int32_t count = invoke_int(g_cfg.m_sticker_count, gc_get()(inv_root), nullptr, &ok, -1);
    if (!ok || count < 0 || count > PGR_OG_STICKER_SCAN_MAX) {
      emit_error(PGR_OG_ERR_STICKER_SVC, tid);
      drop(&inv_root); drop(&player_root); drop(&playerId_root); drop(&niaId_root); drop(&gift_root);
      clear_txn(tid);
      return;
    }
    for (int32_t i = 0; i < count; ++i) {
      int32_t idx = i;
      void *item_params[1] = {&idx};
      void *inv = gc_get()(inv_root);
      void *cand = inv != nullptr ? invoke_fn()(g_cfg.m_sticker_item, inv, item_params, &exc) : nullptr;
      if (exc != nullptr || !is_exact(cand, g_cfg.class_sticker)) continue;
      uintptr_t cand_root = hold(cand);
      if (cand_root == 0) continue;
      void *c = gc_get()(cand_root);
      void *qparams[1] = {c};
      void *stk2 = gc_get()(g_txn.sticker_svc_root);
      int32_t qty = stk2 != nullptr ? invoke_int(g_cfg.m_quantity, stk2, qparams, &ok, 0) : 0;
      if (!ok || qty <= 0) { drop(&cand_root); continue; }
      // Found a usable sticker: capture its id and reserve one AFTER a fresh sticker-gate re-check.
      void *cobj = gc_get()(cand_root);
      void *sid = field_ptr(cobj, g_cfg.f_sticker_id);
      uintptr_t sid_root = hold(sid);
      uint16_t tmp[PGR_OG_ID_MAX]; int tl = 0;
      if (sid_root == 0 || !read_id(gc_get()(sid_root), tmp, &tl)) {
        emit_error(PGR_OG_ERR_STICKER_ID, tid);
        drop(&sid_root); drop(&cand_root);
        drop(&inv_root); drop(&player_root); drop(&playerId_root); drop(&niaId_root); drop(&gift_root);
        clear_txn(tid);
        return;
      }
      void *res_sid=gc_get()(sid_root);
      void *res_stk=gc_get()(g_txn.sticker_svc_root);
      // No managed lookup after this gate and before ReserveStickers.
      if (!res_sid || !res_stk || !gate_live(pinned, tid, PGR_OG_MASK_SEND | PGR_OG_MASK_STICKER) || g_txn_epoch != pass_epoch ||
          g_pending_stop) {
        drop(&sid_root); drop(&cand_root); drop(&inv_root);
        drop(&player_root); drop(&playerId_root); drop(&niaId_root); drop(&gift_root);
        clear_txn(tid);
        return;
      }
      g_txn.sticker_id_root = sid_root;
      g_txn.reserved = true;  // latch BEFORE the invoke so cleanup returns exactly once
      int32_t one = PGR_OG_RESERVE_COUNT;

      void *res_params[2] = {res_sid, &one};
      invoke_fn()(g_cfg.m_reserve, res_stk, res_params, &exc);
      if(exc){emit_error(PGR_OG_ERR_INVOKE,tid);drop(&cand_root);drop(&inv_root);drop(&player_root);drop(&playerId_root);drop(&niaId_root);drop(&gift_root);clear_txn(tid);return;}
      sticker_root = cand_root;  // keep the sticker rooted for SendGift
      break;
    }
    drop(&inv_root);
  }

  // Snapshot rooted arguments first. The authorization check is the last operation
  // before publication/invoke; no GC lookup can reenter between the gate and send.
  uint64_t gid=gift_id;
  void *send_playerId=gc_get()(playerId_root),*send_niaId=gc_get()(niaId_root);
  void *send_sticker=sticker_root?gc_get()(sticker_root):nullptr;
  void *rpc_obj=gc_get()(g_txn.rpc_root);
  uint64_t issue_now=0;
  if(!rpc_obj||!send_playerId||!send_niaId||(sticker_root&&!send_sticker)||
     !g_txn.active||g_txn_epoch!=pass_epoch||g_pending_stop.load()||g_cancel_pending||
     !read_boottime_ms(&issue_now)||!gate_live(pinned,tid,PGR_OG_MASK_SEND)){
    drop(&sticker_root);drop(&player_root);drop(&playerId_root);drop(&niaId_root);drop(&gift_root);clear_txn(tid);return;
  }
  g_txn.issued=true;g_txn.deadline_ms=issue_now+PGR_OG_TXN_TIMEOUT_MS;
  g_txn.request_sticker_root=sticker_root;sticker_root=0;
  g_txn.request_player_id_root=playerId_root;playerId_root=0;
  g_txn.request_nia_id_root=niaId_root;niaId_root=0;
  publish_txn();
  uint8_t fls=0;void *send_params[5]={&gid,send_playerId,send_niaId,send_sticker,&fls};
  void *promise=invoke_fn()(g_cfg.m_rpc_send,rpc_obj,send_params,&exc);
  emit_status(PGR_OG_MSG_SENDING,0,tid);
  push_event(PGR_OG_EVENT_REQUEST,0,send_sticker?1:0,tid);
  if (exc != nullptr) {
    // The send threw. A callback may already have fired synchronously (finished/result latched);
    // keep the issued transaction quarantined (no retry), matching JS r.timedOut path.
    emit_error(PGR_OG_ERR_INVOKE, tid);
    g_txn.timed_out = true;
    publish_txn();
    emit_status(PGR_OG_MSG_UNCERTAIN, 0, tid);
  } else if (promise != nullptr) {
    // Keep the pending promise alive until matching completion or shutdown.
    g_txn.promise_root=hold(promise);
    if(!g_txn.promise_root)emit_error(PGR_OG_ERR_GCHANDLE,tid);
  }
  (void)gift_id;
  drop(&sticker_root);
  drop(&player_root);
  drop(&playerId_root);
  drop(&niaId_root);
  drop(&gift_root);
}

// --- observe halves ----------------------------------------------------------------
// ownedCallback: issued txn, exact callbackClass, rpc identity AND bounded exact recipient string.
bool owned_callback(void *o) {
  if (!g_txn.active || !g_txn.issued || g_txn.rpc_root == 0) return false;
  if (!is_exact(o, g_cfg.class_callback)) return false;
  uintptr_t root = hold(o);
  if (root == 0) return false;
  void *co = gc_get()(root);
  void *svc = field_ptr(co, g_cfg.f_callback_service);
  void *rpc = gc_get()(g_txn.rpc_root);
  bool match = svc != nullptr && svc == rpc;
  if (match) {
    co = gc_get()(root);
    match = id_matches(field_ptr(co, g_cfg.f_callback_id));
  }
  drop(&root);
  return match;
}

void observe_result(void *callback,void *response,uint64_t){
  const uint64_t epoch=g_txn_epoch;
  uintptr_t root=hold(response);if(!root)return;
  if(owned_callback(callback)&&epoch==g_txn_epoch&&!g_pending_stop.load()){
    void *r=gc_get()(root);
    if(is_exact(r,g_cfg.class_send_out)){
      int32_t result=field_i32(gc_get()(root),g_cfg.f_send_result);
      if(epoch==g_txn_epoch&&g_txn.active&&g_txn.issued&&!g_pending_stop.load()){
        g_txn.result=result;g_txn.has_result=true;publish_txn();
      }
    }
  }
  drop(&root);
}
void observe_finally(void *callback){
  const uint64_t epoch=g_txn_epoch;
  if(owned_callback(callback)&&epoch==g_txn_epoch&&g_txn.active&&!g_pending_stop.load()){
    g_txn.finished=true;publish_txn();
  }
}
void observe_profile_close(void *profile){
  if(g_txn.active){
    if(g_txn.issued)return;
    const uint64_t epoch=g_txn_epoch;
    void *current=gc_get()(g_txn.profile_root);
    if(epoch!=g_txn_epoch||current!=profile)return;
    ++g_txn_epoch;g_cancel_pending=true;
  }else if(g_active_depth.load()>1){++g_txn_epoch;g_cancel_pending=true;}
}

// --- public API --------------------------------------------------------------------
bool validate_config(const PgrOutgoingGiftsConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_OG_ERR_INVALID_ARG; return false; }
  if (c->version != PGR_OG_CONFIG_VERSION) { *rc = PGR_OG_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrOutgoingGiftsConfig)) { *rc = PGR_OG_ERR_BAD_SIZE; return false; }
  void *const *ptrs = reinterpret_cast<void *const *>(&c->gchandle_new);
  for (int i = 0; i < 59; ++i) {
    if (ptrs[i] == nullptr) { *rc = PGR_OG_ERR_MISSING_PTR; return false; }
  }
  *rc = PGR_OG_OK;
  return true;
}

int api_install(const PgrOutgoingGiftsConfig *config) {
  int rc = PGR_OG_OK;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_OG_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_cfg, config, sizeof(*config)) != 0) return PGR_OG_ERR_RECONFIG;
    return PGR_OG_OK;
  }
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;
  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;
    g_mask = 0;
    g_lease_until_ms = 0;
    g_fault = false;
  }
  g_pending_stop = false;
  g_txn_epoch = 0;
  g_txn = Txn{};
  publish_txn();
  g_installed.store(true, std::memory_order_release);
  return g_install_error = PGR_OG_OK;
}

int api_update(uint32_t mask,uint64_t remaining,uint64_t tid){
  if(!g_installed.load())return PGR_OG_ERR_NOT_INSTALLED;
  uint64_t now=0;bool clock_ok=read_boottime_ms(&now);
  std::lock_guard<std::mutex> lock(g_state_mutex);
  uint64_t owner=g_owner_tid.load();
  if(!clock_ok||!tid||current_tid()!=tid||(owner&&owner!=tid)||g_pending_stop.load()||g_cleaning.load()){
    g_mask=0;g_lease_until_ms=0;++g_gen;return clock_ok?PGR_OG_OK:PGR_OG_ERR_CLOCK_FAILURE;
  }
  mask&=PGR_OG_MASK_SEND|PGR_OG_MASK_STICKER;
  remaining=remaining>kMaxLeaseMs?kMaxLeaseMs:remaining;
  if(mask!=g_mask||!remaining||!g_lease_until_ms||now>=g_lease_until_ms)++g_gen;
  g_mask=mask;g_lease_until_ms=remaining?now+remaining:0;return PGR_OG_OK;
}

int api_queue(void *panel, void *profile, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_OG_QUEUE_ORIGINAL;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || panel == nullptr) return PGR_OG_QUEUE_ORIGINAL;
  if (!owner_ready(tid)) return PGR_OG_QUEUE_ORIGINAL;
  if (g_pending_stop.load()||g_cleaning.load()) return PGR_OG_QUEUE_HANDLED;
  if (g_active_depth.load(std::memory_order_relaxed) != 0) return PGR_OG_QUEUE_HANDLED;

  // Already pending (JS line 277): handled, status, no original.
  if (g_txn.active) {
    emit_status(PGR_OG_MSG_ALREADY, 0, tid);
    return PGR_OG_QUEUE_HANDLED;
  }
  // mask/lease gate (JS line 279): not eligible -> original.
  if (!gate_live(pin_gen(), tid, PGR_OG_MASK_SEND)) return PGR_OG_QUEUE_ORIGINAL;

  g_active_depth.fetch_add(1, std::memory_order_relaxed);
  int result = do_queue(panel, profile, tid);
  leave_frame(tid);
  return result;
}

int api_observe(uint32_t kind, void *obj, void *arg, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_OG_ERR_NOT_INSTALLED;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || obj == nullptr) return PGR_OG_OK;
  if (!owner_ready(tid)) return PGR_OG_OK;

  g_active_depth.fetch_add(1, std::memory_order_relaxed);
  switch (kind) {
    case PGR_OG_OBSERVE_PROFILE_CLOSE:
      observe_profile_close(obj);
      break;
    case PGR_OG_OBSERVE_RPC_RESULT:
      observe_result(obj, arg, tid);
      break;
    case PGR_OG_OBSERVE_RPC_FINALLY:
      observe_finally(obj);
      break;
    default:
      break;
  }
  leave_frame(tid);
  return PGR_OG_OK;
}

int api_tick(uint64_t proven_tid, int stopping) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_OG_ERR_NOT_INSTALLED;
  if(stopping){
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_pending_stop.store(true);g_mask=0;g_lease_until_ms=0;++g_gen;
  }
  const uint64_t tid = current_tid();
  if(!proven_tid||tid!=proven_tid||!owner_ready(tid))return PGR_OG_OK;
  if (g_active_depth.load(std::memory_order_relaxed) != 0) {
    if (stopping != 0) g_pending_stop = true;  // revoke now; defer teardown to the outermost frame
    return PGR_OG_OK;
  }

  g_active_depth.fetch_add(1, std::memory_order_relaxed);
  const bool stop = stopping != 0 || g_pending_stop.load();
  if (stop) g_pending_stop = true;

  if (!g_txn.active) { leave_frame(tid); return PGR_OG_OK; }
  if (stop) { leave_frame(tid); return PGR_OG_OK; }  // deferred teardown clears the txn

  uint64_t now = 0;
  const bool clock_ok = read_boottime_ms(&now);

  if (g_txn.issued) {
    // Completion / quarantine. Issued survives off/lease expiry; result bookkeeping still runs.
    if (g_txn.finished) {
      const bool success = g_txn.has_result && g_txn.result == g_cfg.send_success;
      if (success) emit_status(PGR_OG_MSG_SENT, 0, tid);
      else if (!g_txn.has_result) emit_status(PGR_OG_MSG_FAILED_NULL, 0, tid);
      else emit_status(PGR_OG_MSG_FAILED_RESULT, g_txn.result, tid);
      push_event(PGR_OG_EVENT_RESULT, g_txn.has_result ? g_txn.result : 0, (success?1:0)|(g_txn.has_result?2:0), tid);
      clear_txn(tid);
    } else if (clock_ok && now > g_txn.deadline_ms && !g_txn.timed_out) {
      g_txn.timed_out = true;  // telemetry/quarantine only; retain until finally or stop
      publish_txn();
      emit_status(PGR_OG_MSG_PENDING, 0, tid);
    }
    leave_frame(tid);
    return PGR_OG_OK;
  }

  // Unissued: off / lease expiry cancels (JS line 113).
  const uint64_t pinned = pin_gen();
  if (!clock_ok || !gate_live(pinned, tid, PGR_OG_MASK_SEND)) { clear_txn(tid); leave_frame(tid); return PGR_OG_OK; }

  const uint64_t pass_epoch = g_txn_epoch;
  tick_issue(tid, pinned, pass_epoch);
  leave_frame(tid);
  return PGR_OG_OK;
}

int api_sample(PgrOutgoingGiftsSample *out) {
  if (out == nullptr) return PGR_OG_ERR_INVALID_ARG;
  const uint32_t roots = root_total();
  const uint32_t active = g_m_active.load(std::memory_order_relaxed);
  const uint32_t issued = g_m_issued.load(std::memory_order_relaxed);
  const uint32_t result_known = g_m_result_known.load(std::memory_order_relaxed);
  const int32_t result = g_m_result.load(std::memory_order_relaxed);
  const uint32_t timed_out = g_m_timed_out.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_state_mutex);
  out->version = PGR_OG_SAMPLE_VERSION;
  out->size = sizeof(PgrOutgoingGiftsSample);
  out->request_serial = g_request_serial;
  out->result_serial = g_result_serial;
  out->error_serial = g_error_serial;
  out->status_serial = g_status_serial;
  out->last_tid = g_last_tid;
  out->roots = roots;
  out->active = active;
  out->issued = issued;
  out->result_known = result_known;
  out->result = result;
  out->timed_out = timed_out;
  out->fault = g_fault ? 1u : 0u;
  out->overflow = g_overflow ? 1u : 0u;
  out->pending_events = g_fifo_count;
  out->reserved = 0;
  return PGR_OG_OK;
}

int api_next_event(PgrOutgoingGiftsEvent *out) {
  if (out == nullptr) return PGR_OG_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_fifo_count == 0) return 0;
  *out = g_fifo[g_fifo_head];
  g_fifo_head = (g_fifo_head + 1) % PGR_OG_EVENT_CAP;
  --g_fifo_count;
  return 1;
}

const PgrOutgoingGiftsApi kApiTable = {
    PGR_OG_API_VERSION, static_cast<uint32_t>(sizeof(PgrOutgoingGiftsApi)),
    &api_install,       &api_update,
    &api_queue,         &api_observe,
    &api_tick,          &api_sample,
    &api_next_event,
};

}  // namespace

extern "C" const void *pgrazor_outgoing_gifts_api(uint32_t version) {
  if (version != PGR_OG_API_VERSION) return nullptr;
  return &kApiTable;
}

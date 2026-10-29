#include "incoming_gifts.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 17 incoming gift-open controller. See incoming_gifts.h for the ABI and the division of
// labour with runtime/native-incoming-gifts.js. Built -fno-exceptions / -fno-rtti like every
// sibling: the JS "throw" paths become fail-closed error records, and the single durable GUI root
// is freed exactly once on the owner thread. No mutex is held across a managed call, a GC-handle
// call, or the Open invoke. This module installs NO hook.
//
// Reentrancy model (conservative -- a gc_get(), a field access or a runtime_invoke() may re-enter
// the owner thread through il2cpp instrumentation or a relocating collector, so "there is no game
// code between these calls" is NOT assumed):
//   * Every managed reference held across any gc_get()/invoke() is strongly rooted and re-resolved
//     from its root after the call; raw pointers are never reused across a potential safepoint.
//   * A reentrant stopping tick() revokes immediately (latches g_pending_stop so the outer pass
//     issues nothing more) but DEFERS the root free to the outermost frame, after every managed
//     call has returned. A top-level stopping tick() latches g_pending_stop BEFORE its first managed
//     call. The latch stays SET across the deferred free (leave_frame) and is cleared only after it,
//     so observe READY/OPEN/SEQUENCE reject every re-arm until the teardown is done -- no re-arm
//     survives a stop. The active-depth decrement happens AFTER the free so root_count never reads 0
//     while a frame is still live.
//   * A slot epoch (g_slot_epoch) bumps on every arm AND on every clear, before the handle is freed.
//     observe_ready() pins the epoch before its allocation and after the teardown, dropping its own
//     fresh root (never clobbering/leaking the slot) if a reentrant event won a newer arm. clear_slot()
//     detaches the whole slot before the free, so a reentrant READY that arms during the free is left
//     intact. Every acting pass (tick/is_open/sequence) pins the epoch BEFORE its first getter and
//     abandons -- without clearing a possibly-newer slot -- if the epoch changed, the refreshed GUI is
//     null, or a stop latched. ISSUED/skip is emitted only when the mutation actually runs.
//   * The cross-thread sample() reads atomic owned/attempted/automated mirrors (published on the
//     owner thread) rather than the owner-thread Slot fields, so there is no data race.

static_assert(sizeof(PgrIncomingGiftsConfig) == 240, "config must be 240 bytes");
static_assert(sizeof(PgrIncomingGiftsSample) == 88, "sample must be 88 bytes");
static_assert(sizeof(PgrIncomingGiftsEvent) == 16, "event must be 16 bytes");
static_assert(sizeof(PgrIncomingGiftsApi) == 64, "table must be 64 bytes");
static_assert(offsetof(PgrIncomingGiftsConfig, gchandle_new) == 8, "layout");
static_assert(offsetof(PgrIncomingGiftsConfig, m_open) == 8 + 10 * 8, "layout");
static_assert(offsetof(PgrIncomingGiftsConfig, class_open_gui) == 8 + 14 * 8, "layout");
static_assert(offsetof(PgrIncomingGiftsConfig, f_current_state) == 8 + 20 * 8, "layout");
static_assert(offsetof(PgrIncomingGiftsConfig, state_open) == 8 + 28 * 8, "layout");
static_assert(offsetof(PgrIncomingGiftsConfig, open_success) == 8 + 28 * 8 + 4, "layout");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_GIFT_MAX_LEASE_MS;

// --- il2cpp runtime ABI typedefs ---------------------------------------------------
using GcNew = uintptr_t (*)(void *obj, int pinned);
using GcGet = void *(*)(uintptr_t handle);
using GcFree = void (*)(uintptr_t handle);
using ObjClass = void *(*)(void *obj);
using Unbox = void *(*)(void *obj);
using Invoke = void *(*)(const void *method, void *obj, void **params, void **exc);
using FieldGet = void (*)(void *obj, void *field, void *out);
using FieldSet = void (*)(void *obj, void *field, void *value);
using StringLen = int (*)(void *str);
using StringChars = uint16_t *(*)(void *str);

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrIncomingGiftsConfig g_cfg{};
std::atomic<bool> g_installed{false};
bool g_install_attempted = false;
int g_install_error = PGR_GIFT_ERR_NOT_INSTALLED;

// Permanent Unity owner, latched once from a genuine on-thread observe()/tick().
std::atomic<uint64_t> g_owner_tid{0};

// Conservative finish/root accounting (atomic so sample is lock-free for these).
std::atomic<uint32_t> g_slot_roots{0};    // 0 or 1 durable GUI root
std::atomic<uint32_t> g_active_depth{0};   // owner-thread observe/tick reentrancy depth

// Cross-thread sample mirrors (published on the owner thread; read lock-free by sample()). Reading
// the owner-thread Slot directly from another thread would be a C++ data race (UB), so sample()
// never touches g_slot.
std::atomic<uint32_t> g_m_owned{0};
std::atomic<uint32_t> g_m_attempted{0};
std::atomic<uint32_t> g_m_automated{0};

// --- gate / telemetry (guarded by g_state_mutex) -----------------------------------
uint32_t g_mask = 0;          // raw mask (bit 1 == auto-open); echoed in the wait event
uint64_t g_lease_until_ms = 0;
// Revocation generation. Bumped whenever the gate goes off / is refreshed from a dead lease, so a
// disable (or disable+reenable) that straddles a getter revokes the pinned apply.
uint64_t g_gen = 1;
bool g_fault = false;         // latched on FIFO overflow (fail-closed: no further issue)

uint64_t g_issued_serial = 0, g_wait_serial = 0, g_error_serial = 0, g_skip_serial = 0,
         g_dialog_serial = 0, g_last_tid = 0;
uint32_t g_last_wait_reason = PGR_GIFT_WAIT_NONE;
bool g_overflow = false;

// Bounded FIFO of event records (reason codes only). Guarded by g_state_mutex.
PgrIncomingGiftsEvent g_fifo[PGR_GIFT_EVENT_CAP];
uint32_t g_fifo_head = 0, g_fifo_count = 0;

// --- owner-thread-only slot --------------------------------------------------------
struct Slot {
  uintptr_t gui_root = 0;  // durable GC root to the owned open GUI (0 == none)
  bool attempted = false;  // open press observed OR we issued (latches one attempt per lifecycle)
  bool automated = false;  // WE issued the open (enables the sequence skip)
  uint64_t deadline_ms = 0;
  uint64_t next_ms = 0;    // attempt throttle
};
Slot g_slot{};

// Owner-thread-only reentrancy guards (never touched off the owner thread).
bool g_pending_stop = false;  // a reentrant stopping tick requested a deferred teardown
uint64_t g_slot_epoch = 0;    // bumps on each arm; detects a reentrant re-arm during a free

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
GcFree gc_free() { return reinterpret_cast<GcFree>(g_cfg.gchandle_free); }
ObjClass obj_class() { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }
Unbox unbox() { return reinterpret_cast<Unbox>(g_cfg.object_unbox); }
Invoke invoke_fn() { return reinterpret_cast<Invoke>(g_cfg.runtime_invoke); }
FieldGet field_get() { return reinterpret_cast<FieldGet>(g_cfg.field_get_value); }
FieldSet field_set() { return reinterpret_cast<FieldSet>(g_cfg.field_set_value); }

bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }

// Publish the owner-thread slot state into the cross-thread atomic mirrors read by sample().
void publish_slot() {
  g_m_owned.store(g_slot.gui_root != 0 ? 1u : 0u, std::memory_order_relaxed);
  g_m_attempted.store(g_slot.attempted ? 1u : 0u, std::memory_order_relaxed);
  g_m_automated.store(g_slot.automated ? 1u : 0u, std::memory_order_relaxed);
}

// --- durable GUI root (owner thread) -----------------------------------------------
uintptr_t hold_durable(void *obj) {
  if (obj == nullptr) return 0;
  uintptr_t h = gc_new()(obj, 0);
  if (h != 0) g_slot_roots.fetch_add(1, std::memory_order_relaxed);
  return h;
}
// Clear the owned slot. The ENTIRE slot (root field + latch fields) is detached and the epoch is
// bumped BEFORE the handle is freed, so (a) a reentrant close/stop cannot see or re-free this handle
// (the field is already 0), and (b) any acting pass that snapshotted the epoch is invalidated even
// if a reentrant READY re-arms a fresh slot during the free -- a CLOSE invalidates the pass whether
// or not another READY follows. gc_free() itself may re-enter the owner thread; a reentrant READY
// that arms a fresh slot during that free bumps the epoch again, and we leave that freshly-armed
// slot (its root/deadline/next) intact instead of erasing it. (During a stop teardown observe_ready
// rejects every re-arm while g_pending_stop is latched, so nothing is armed until the free is done.)
void clear_slot() {
  uintptr_t h = g_slot.gui_root;
  if (h == 0) {
    // No durable root; just clear the latch fields (no epoch bump, no free).
    g_slot.attempted = false;
    g_slot.automated = false;
    g_slot.deadline_ms = 0;
    g_slot.next_ms = 0;
    publish_slot();
    return;
  }
  g_slot.gui_root = 0;
  g_slot.attempted = false;
  g_slot.automated = false;
  g_slot.deadline_ms = 0;
  g_slot.next_ms = 0;
  ++g_slot_epoch;  // invalidate any acting pass BEFORE the (possibly reentrant) free
  publish_slot();
  gc_free()(h);
  g_slot_roots.fetch_sub(1, std::memory_order_relaxed);
  // The fields were already detached above; a reentrant READY that armed a fresh slot during the
  // free set its own fields/root, which we leave intact. Re-publish the resulting state.
  publish_slot();
}

// --- field readers (object must be rooted by the caller) ---------------------------
void *field_ptr(void *obj, void *field) {
  void *out = nullptr;
  field_get()(obj, field, &out);
  return out;
}
int32_t field_i32(void *obj, void *field) {
  int32_t v = 0;
  field_get()(obj, field, &v);
  return v;
}

// --- managed invoke helpers (no mutex held) ----------------------------------------
// Returns true with *out_threw set if the managed invoke raised. obj must be rooted.
bool invoke_void(void *method, void *obj, void **params, bool *out_threw) {
  void *exc = nullptr;
  invoke_fn()(method, obj, params, &exc);
  *out_threw = exc != nullptr;
  return !*out_threw;
}

// Invoke an instance bool getter: root the boxed return, verify System.Boolean, unbox the byte.
// Any failure yields false (a managed exception makes `*ok` false; the caller treats that as a
// not-ready button, matching the JS rocketBool throw being swallowed into the gift-error path).
bool invoke_bool(void *method, void *obj, bool *ok) {
  void *exc = nullptr;
  void *r = invoke_fn()(method, obj, nullptr, &exc);
  if (exc != nullptr || r == nullptr) {
    *ok = false;
    return false;
  }
  uintptr_t root = gc_new()(r, 0);
  if (root == 0) {
    *ok = false;
    return false;
  }
  void *bt = gc_get()(root);
  bool value = false;
  bool good = bt != nullptr && obj_class()(bt) == g_cfg.class_bool;
  if (good) {
    uint8_t *data = static_cast<uint8_t *>(unbox()(bt));
    good = data != nullptr && *data <= 1;
    value = good && *data == 1;
  }
  gc_free()(root);
  *ok = good;
  return value;
}

// Static op_Implicit(UnityEngine.Object) -> bool. The Object arg is a reference type, so the
// params slot carries the object pointer itself (matches iv_display is_alive / JS alive()). The
// boxed result is rooted before it is unboxed.
bool is_alive(void *obj) {
  void *params[1] = {obj};
  void *exc = nullptr;
  void *r = invoke_fn()(g_cfg.m_alive, nullptr, params, &exc);
  if (exc != nullptr || r == nullptr) return false;
  uintptr_t root = gc_new()(r, 0);
  if (root == 0) return false;
  void *bt = gc_get()(root);
  bool value = false;
  if (bt != nullptr && obj_class()(bt) == g_cfg.class_bool) {
    // unbox() can return null for a malformed boxed value; never dereference it unchecked.
    uint8_t *data = static_cast<uint8_t *>(unbox()(bt));
    value = data != nullptr && *data == 1;
  }
  gc_free()(root);
  return value;
}

// giftText sender validation: exactly System.String, length within 1..256 (JS giftText window).
// The bytes themselves are never read here -- only presence is proven. The string must be rooted
// by the caller for the duration of this call.
bool valid_sender(void *str) {
  if (!is_exact(str, g_cfg.class_string)) return false;
  int n = reinterpret_cast<StringLen>(g_cfg.string_length)(str);
  return n >= PGR_GIFT_SENDER_MIN && n <= PGR_GIFT_SENDER_MAX;
}

// giftButton: the Selectable must be present, alive, active-and-enabled and interactable. Root it
// before the three managed calls (any may move it) and re-resolve between them.
bool gift_button(void *gui) {
  void *button = field_ptr(gui, g_cfg.f_open_button);
  if (button == nullptr) return false;
  uintptr_t root = gc_new()(button, 0);
  if (root == 0) return false;
  bool result = false;
  void *b = gc_get()(root);
  if (b != nullptr && is_alive(b)) {
    bool ok = false;
    b = gc_get()(root);
    bool active = b != nullptr && invoke_bool(g_cfg.m_active, b, &ok) && ok;
    if (active) {
      b = gc_get()(root);
      bool inter = b != nullptr && invoke_bool(g_cfg.m_interactable, b, &ok) && ok;
      result = inter;
    }
  }
  gc_free()(root);
  return result;
}

// currentState == STATE_OPEN (JS giftOpen). The GUI must be rooted by the caller.
bool gift_open(void *gui) {
  return gui != nullptr && field_i32(gui, g_cfg.f_current_state) == g_cfg.state_open;
}

// --- gate / telemetry (brief lock) -------------------------------------------------
uint32_t root_count() {
  return g_slot_roots.load(std::memory_order_relaxed) +
         g_active_depth.load(std::memory_order_relaxed);
}

uint64_t pin_gen() {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  return g_gen;
}

// Fresh-clock gate check for a mutation (the Open invoke and the sequence skip). Fails closed and
// reports the exact wait reason in *wait: AUTO_OFF takes precedence over FOREGROUND, mirroring the
// JS order (mask checked before lease). A fault, a generation mismatch (revoked) or a non-owner
// also fails, reported as FOREGROUND (the neutral "not ready" reason).
bool gate_open(uint64_t pinned_gen, uint64_t tid, uint32_t *wait) {
  *wait = PGR_GIFT_WAIT_FOREGROUND;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return false;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_fault) return false;
  if (!(g_mask & 1u)) {
    *wait = PGR_GIFT_WAIT_AUTO_OFF;
    return false;
  }
  if (g_gen != pinned_gen) return false;
  if (!(g_lease_until_ms != 0 && now < g_lease_until_ms)) return false;
  if (g_owner_tid.load(std::memory_order_acquire) != tid) return false;
  return true;
}

// Push one record onto the bounded FIFO. On overflow, latch the fault and (best-effort) replace the
// record with an OVERFLOW error so the adapter observes the fail-closed transition.
void push_event(uint32_t kind, uint32_t reason, uint64_t tid) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_last_tid = tid;
  if (kind == PGR_GIFT_EVENT_WAIT) {
    ++g_wait_serial;
    g_last_wait_reason = reason;
  } else if (kind == PGR_GIFT_EVENT_ISSUED) {
    ++g_issued_serial;
  } else if (kind == PGR_GIFT_EVENT_ERROR) {
    ++g_error_serial;
  } else if (kind == PGR_GIFT_EVENT_SKIP) {
    ++g_skip_serial;
  }
  if (g_fifo_count >= PGR_GIFT_EVENT_CAP) {
    // Fail closed: drop the newcomer, latch the fault, and overwrite the newest slot with an
    // OVERFLOW error so a drain surfaces it. No further issue happens while g_fault is set.
    g_overflow = true;
    g_fault = true;
    uint32_t last = (g_fifo_head + g_fifo_count - 1) % PGR_GIFT_EVENT_CAP;
    g_fifo[last] = PgrIncomingGiftsEvent{PGR_GIFT_EVENT_ERROR, PGR_GIFT_ERR_REASON_OVERFLOW, tid};
    return;
  }
  uint32_t idx = (g_fifo_head + g_fifo_count) % PGR_GIFT_EVENT_CAP;
  g_fifo[idx] = PgrIncomingGiftsEvent{kind, reason, tid};
  ++g_fifo_count;
}

// De-duplicated wait: only enqueue (and bump the serial) when the reason actually changed, exactly
// like incomingWait (which emits only when incomingGift.wait !== reason).
void emit_wait(uint32_t reason, uint64_t tid) {
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_last_wait_reason == reason) return;
  }
  push_event(PGR_GIFT_EVENT_WAIT, reason, tid);
}

// --- owner latching ----------------------------------------------------------------
bool owner_ready(uint64_t tid) {
  if (tid == 0) return false;
  uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (!(owner == 0 || owner == tid)) return false;
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
  return g_owner_tid.load(std::memory_order_acquire) == tid;
}

// Run the deferred teardown requested by a reentrant stopping tick, then drop the active-depth. Only
// the outermost frame (the one about to take the depth back to 0) performs the free so a nested
// frame never tears down a slot the outer frame is still using; the free happens BEFORE the depth
// decrement so root_count stays non-zero until the durable root is gone.
void leave_frame() {
  if (g_active_depth.load(std::memory_order_relaxed) == 1 && g_pending_stop) {
    // Keep the stop latch SET across the free so a reentrant READY/OPEN/SEQUENCE fired from inside
    // gc_free() is rejected (no re-arm survives a stop); clear the latch only once the teardown is
    // complete.
    clear_slot();
    g_pending_stop = false;
  }
  g_active_depth.fetch_sub(1, std::memory_order_relaxed);
}

// --- observe halves ----------------------------------------------------------------
// READY: the incoming dialog finished opening. A repeated READY on the SAME still-live GUI is a
// no-op (it must NOT reset `attempted`); any other GUI re-arms a fresh slot. Mirrors openReady but
// with the identity guard the native controller adds. Bumps the epoch on a genuine new arm so a
// concurrent clear_slot() in an outer frame will not erase this slot.
void observe_ready(void *gui, uint64_t tid) {
  if (g_pending_stop) return;  // a stop teardown is in progress: reject every re-arm until it is done
  if (!is_exact(gui, g_cfg.class_open_gui)) return;
  if (g_slot.gui_root != 0 && gc_get()(g_slot.gui_root) == gui) return;  // same live GUI: keep state

  // Pin the epoch BEFORE the allocation. hold_durable()'s gc_new() may re-enter the owner thread, so
  // a reentrant READY can win a NEWER arm while we are allocating; the raw `gui` must also be rooted
  // BEFORE any teardown so it is never reused across clear_slot()'s (reentrant/relocating) free.
  const uint64_t epoch_before = g_slot_epoch;
  const bool had_slot = g_slot.gui_root != 0;
  uintptr_t new_root = hold_durable(gui);
  if (new_root == 0) {
    push_event(PGR_GIFT_EVENT_ERROR, PGR_GIFT_ERR_REASON_GCHANDLE, tid);
    return;
  }
  // A reentrant event advanced the epoch during the allocation itself: a newer arm (or close) won.
  // Drop our freshly-made root rather than clobbering/leaking the newer slot; this outer arm is now
  // stale.
  if (g_slot_epoch != epoch_before) {
    gc_free()(new_root);
    g_slot_roots.fetch_sub(1, std::memory_order_relaxed);
    publish_slot();
    return;
  }
  clear_slot();  // detaches + bumps the epoch + frees the old root BEFORE we install ours
  // After the teardown, reject this arm if a reentrant READY re-armed a slot during the free
  // (gui_root set) or if any nested arm/clear advanced the epoch past our own clear's single bump
  // (a newer event won, even one that netted empty). A fresh reentrant rearm is thus preserved.
  if (g_slot.gui_root != 0 || g_slot_epoch != epoch_before + (had_slot ? 1u : 0u)) {
    gc_free()(new_root);
    g_slot_roots.fetch_sub(1, std::memory_order_relaxed);
    publish_slot();
    return;
  }
  g_slot.gui_root = new_root;
  ++g_slot_epoch;  // a fresh arm: protects this slot from an outer clear's field reset
  g_slot.attempted = false;
  g_slot.automated = false;
  uint64_t now = 0;
  if (read_boottime_ms(&now)) {
    g_slot.deadline_ms = now + PGR_GIFT_DEADLINE_MS;
    g_slot.next_ms = now + PGR_GIFT_THROTTLE_MS;
  } else {
    g_slot.deadline_ms = 0;
    g_slot.next_ms = 0;
  }
  publish_slot();
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    ++g_dialog_serial;
    g_last_tid = tid;
    g_last_wait_reason = PGR_GIFT_WAIT_NONE;  // a fresh dialog clears the last wait
  }
}

// OPEN: the Open button was pressed. Only marks `attempted` (so our automation will not also
// press); it never sets `automated`, preserving the manual-vs-automation distinction.
void observe_open(void *gui) {
  if (g_pending_stop) return;  // a stop teardown is in progress: reject
  if (g_slot.gui_root != 0 && gc_get()(g_slot.gui_root) == gui) {
    g_slot.attempted = true;
    publish_slot();
  }
}

// CLOSE: the owned dialog dismissed. A close for a GUI we do not own is ignored.
void observe_close(void *gui) {
  if (g_slot.gui_root != 0 && gc_get()(g_slot.gui_root) == gui) clear_slot();
}

// SEQUENCE: the gift-open presentation iterator's MoveNext. Writes the state-3 skip ONLY for an
// owned + automated open with a live gate and a successful response (JS sequence observer). The
// sequence object is strongly rooted for the whole inspection and re-resolved from its root after
// every gc_get(); the GUI identity is compared against a freshly resolved owned root. The gate is
// re-checked immediately before the field_set, after the FINAL refresh, and a pending stop aborts
// the write.
void observe_sequence(void *obj, uint64_t tid) {
  if (g_pending_stop) return;  // a stop teardown is in progress: reject
  if (g_slot.gui_root == 0 || !g_slot.automated) return;
  if (!is_exact(obj, g_cfg.class_sequence)) return;

  // Pin the revocation generation AND the slot epoch BEFORE the allocation. A disable+reenable (or a
  // reentrant close+ready that replaces the slot) straddling the lookups below changes the gen/epoch
  // and revokes the write -- never mutating a replacement sequence on stale ownership.
  const uint64_t pinned = pin_gen();
  const uint64_t pass_epoch = g_slot_epoch;

  uintptr_t seq_root = gc_new()(obj, 0);
  if (seq_root == 0) {
    push_event(PGR_GIFT_EVENT_ERROR, PGR_GIFT_ERR_REASON_GCHANDLE, tid);
    return;
  }
  uint32_t wait;
  bool skipped = false;
  if (gate_open(pinned, tid, &wait)) {  // auto-open off / lease dead: no skip
    // Identity: root the sequence's GUI field, then compare it against the owned GUI with both
    // freshly resolved. A stale raw compare across getters can only yield a false NEGATIVE (a missed
    // skip, which is fail-closed), never a false positive. The sequence self is re-resolved from its
    // root before every field read -- no raw sequence pointer is retained across a managed call.
    void *o = gc_get()(seq_root);
    void *seq_gui_raw = o != nullptr ? field_ptr(o, g_cfg.f_seq_gui) : nullptr;
    uintptr_t seqgui_root = seq_gui_raw != nullptr ? gc_new()(seq_gui_raw, 0) : 0;
    bool same_gui = false;
    if (seqgui_root != 0) {
      void *owned = g_slot.gui_root != 0 ? gc_get()(g_slot.gui_root) : nullptr;
      void *sg = gc_get()(seqgui_root);
      same_gui = owned != nullptr && sg != nullptr && sg == owned;
      gc_free()(seqgui_root);
    }
    if (same_gui) {
      o = gc_get()(seq_root);
      void *response_raw = o != nullptr ? field_ptr(o, g_cfg.f_seq_response) : nullptr;
      bool resp_ok = false;
      if (is_exact(response_raw, g_cfg.class_open_out)) {
        // Root the response before reading its result across a managed call.
        uintptr_t resp_root = gc_new()(response_raw, 0);
        if (resp_root != 0) {
          void *r = gc_get()(resp_root);
          resp_ok = is_exact(r, g_cfg.class_open_out) &&
                    field_i32(r, g_cfg.f_open_result) == g_cfg.open_success;
          gc_free()(resp_root);
        }
      }
      o = gc_get()(seq_root);
      bool state_zero = resp_ok && o != nullptr && field_i32(o, g_cfg.f_seq_state) == 0;
      if (state_zero) {
        // Final refresh of the sequence self, then recheck epoch + automated + pending-stop + gate
        // immediately before the write. A reentrant close+ready (new slot) or a disable aborts here.
        o = gc_get()(seq_root);
        if (o != nullptr && !g_pending_stop && g_slot_epoch == pass_epoch && g_slot.gui_root != 0 &&
            g_slot.automated && gate_open(pinned, tid, &wait)) {
          int32_t three = PGR_GIFT_STATE_SKIP;
          field_set()(o, g_cfg.f_seq_state, &three);
          skipped = true;
        }
      }
    }
  }
  gc_free()(seq_root);
  if (skipped) push_event(PGR_GIFT_EVENT_SKIP, PGR_GIFT_ERR_REASON_NONE, tid);
}

// --- public API --------------------------------------------------------------------
bool validate_config(const PgrIncomingGiftsConfig *c, int *rc) {
  if (c == nullptr) {
    *rc = PGR_GIFT_ERR_INVALID_ARG;
    return false;
  }
  if (c->version != PGR_GIFT_CONFIG_VERSION) {
    *rc = PGR_GIFT_ERR_BAD_VERSION;
    return false;
  }
  if (c->size != sizeof(PgrIncomingGiftsConfig)) {
    *rc = PGR_GIFT_ERR_BAD_SIZE;
    return false;
  }
  void *const *ptrs = reinterpret_cast<void *const *>(&c->gchandle_new);
  for (int i = 0; i < 28; ++i) {
    if (ptrs[i] == nullptr) {
      *rc = PGR_GIFT_ERR_MISSING_PTR;
      return false;
    }
  }
  *rc = PGR_GIFT_OK;
  return true;
}

int api_install(const PgrIncomingGiftsConfig *config) {
  int rc = PGR_GIFT_OK;
  if (!validate_config(config, &rc)) return rc;

  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_GIFT_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_cfg, config, sizeof(*config)) != 0) return PGR_GIFT_ERR_RECONFIG_MISMATCH;
    return PGR_GIFT_OK;
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
  g_slot_epoch = 0;
  publish_slot();
  // No hook is installed; the host's existing Gum observation forwards events via observe().
  g_installed.store(true, std::memory_order_release);
  return g_install_error = PGR_GIFT_OK;
}

int api_update(uint32_t mask, uint64_t remaining_lease_ms) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_GIFT_ERR_NOT_INSTALLED;
  uint64_t now = 0;
  const bool clock_ok = read_boottime_ms(&now);
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (!clock_ok) {
    g_mask = 0;
    g_lease_until_ms = 0;
    ++g_gen;  // forced disable revokes any in-flight apply
    return PGR_GIFT_ERR_CLOCK_FAILURE;
  }
  const uint64_t clamped = remaining_lease_ms > kMaxLeaseMs ? kMaxLeaseMs : remaining_lease_ms;
  const bool allow = clamped > 0;
  const bool was_live = g_lease_until_ms != 0 && now < g_lease_until_ms;
  const bool cur_on = (g_mask & 1u) && was_live;
  const bool next_on = allow && (mask & 1u);
  // Bump the generation unless this is a pure lease refresh of an already-live auto-open gate.
  if (!(cur_on && next_on)) ++g_gen;
  // The raw mask is stored independently of the lease so gate_open can still distinguish AUTO_OFF
  // (mask bit 1 clear) from FOREGROUND (bit set but the lease dead) -- mirroring incomingTick.
  g_mask = mask;
  g_lease_until_ms = allow ? now + clamped : 0;
  return PGR_GIFT_OK;
}

int api_observe(uint32_t kind, void *obj, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_GIFT_ERR_NOT_INSTALLED;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || obj == nullptr) return PGR_GIFT_OK;
  if (!owner_ready(tid)) return PGR_GIFT_OK;  // not the proven owner: ignore

  g_active_depth.fetch_add(1, std::memory_order_relaxed);
  switch (kind) {
    case PGR_GIFT_OBSERVE_READY:
      observe_ready(obj, tid);
      break;
    case PGR_GIFT_OBSERVE_OPEN:
      observe_open(obj);
      break;
    case PGR_GIFT_OBSERVE_CLOSE:
      observe_close(obj);
      break;
    case PGR_GIFT_OBSERVE_SEQUENCE:
      observe_sequence(obj, tid);
      break;
    default:
      break;
  }
  leave_frame();
  return PGR_GIFT_OK;
}

int api_tick(uint64_t proven_tid, int stopping) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_GIFT_ERR_NOT_INSTALLED;
  const uint64_t tid = current_tid();
  const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (owner == 0 || tid != owner || proven_tid == 0 || tid != proven_tid) return PGR_GIFT_OK;
  if (g_active_depth.load(std::memory_order_relaxed) != 0) {
    // Reentrant tick on the owner thread (inside an outer frame's managed call). A STOP revokes
    // immediately -- it latches the teardown so the outer pass issues nothing more -- but defers the
    // root free to the outermost frame. A non-stopping nested tick is a plain no-op.
    if (stopping != 0) g_pending_stop = true;
    return PGR_GIFT_OK;
  }

  g_active_depth.fetch_add(1, std::memory_order_relaxed);
  const bool stop = stopping != 0;
  // Latch the stop BEFORE any managed call so a reentrant READY/OPEN/SEQUENCE during the getters
  // below is rejected and the deferred teardown (at leave_frame) owns the free.
  if (stop) g_pending_stop = true;

  uint64_t now = 0;
  const bool clock_ok = read_boottime_ms(&now);

  if (g_slot.gui_root == 0) {
    leave_frame();  // if pending_stop, this clears the (empty) slot and the latch
    return PGR_GIFT_OK;  // no owned dialog (JS incomingTick early-returns on !r.gui)
  }

  // Pin the revocation generation AND the slot epoch BEFORE the first managed getter. A reentrant
  // READY during the alive check below re-arms a fresh slot and bumps the epoch; we must not act on
  // (or clear) that newer slot from this now-stale pass, and a disable straddling the getters must
  // revoke the pinned apply.
  const uint64_t pinned = pin_gen();
  const uint64_t pass_epoch = g_slot_epoch;

  void *gui = gc_get()(g_slot.gui_root);
  // A stopping tick clears the root even if the lease expired / clock failed; a dead or past-
  // deadline GUI is dropped (JS incomingTick: now>deadline || !alive(gui) => incomingClear).
  const bool dead = gui == nullptr || !is_alive(gui);
  if (g_slot_epoch != pass_epoch || g_pending_stop) {
    // A reentrant arm/close/stop won during the getters: abandon WITHOUT clearing (the slot may be a
    // freshly-armed newer one). A pending stop is torn down by leave_frame's deferred free.
    leave_frame();
    return PGR_GIFT_OK;
  }
  if (stop || !clock_ok || now > g_slot.deadline_ms || dead) {
    // The dead/expired result applies to the slot we snapshotted (epoch unchanged). On a stop, the
    // deferred teardown in leave_frame performs the free with the latch held.
    if (!stop) clear_slot();
    leave_frame();
    return PGR_GIFT_OK;
  }

  if (g_slot.attempted || now < g_slot.next_ms) {
    leave_frame();
    return PGR_GIFT_OK;
  }
  g_slot.next_ms = now + PGR_GIFT_THROTTLE_MS;

  uint32_t wait = PGR_GIFT_WAIT_NONE;
  bool done = false;

  if (!gate_open(pinned, tid, &wait)) {
    emit_wait(wait, tid);  // AUTO_OFF or FOREGROUND
    done = true;
  }
  if (!done) {
    gui = gc_get()(g_slot.gui_root);
    if (gui == nullptr || !gift_open(gui)) {
      emit_wait(PGR_GIFT_WAIT_OPENING, tid);
      done = true;
    }
  }
  if (!done) {
    // Root the details object across the sender read (it is reached through a managed field and
    // inspected across a gc_get()).
    gui = gc_get()(g_slot.gui_root);
    void *details = gui != nullptr ? field_ptr(gui, g_cfg.f_open_details) : nullptr;
    bool details_ok = false;
    if (is_exact(details, g_cfg.class_details)) {
      uintptr_t droot = gc_new()(details, 0);
      if (droot != 0) {
        void *d = gc_get()(droot);
        details_ok = d != nullptr && valid_sender(field_ptr(d, g_cfg.f_sender));
        gc_free()(droot);
      }
    }
    if (!details_ok) {
      emit_wait(PGR_GIFT_WAIT_DETAILS, tid);
      done = true;
    }
  }
  if (!done) {
    gui = gc_get()(g_slot.gui_root);
    if (gui == nullptr || !gift_button(gui)) {
      emit_wait(PGR_GIFT_WAIT_BUTTON, tid);
      done = true;
    }
  }
  if (!done) {
    // FINAL managed-ref refresh FIRST, then the gate re-check AFTER it. A reentrant CLOSE/READY
    // during the earlier managed lookups (or a reentrant stopping tick) invalidates this pass: the
    // refreshed gui is null or the epoch changed, so we abandon it silently. ISSUED is pushed ONLY
    // when the Open invoke actually runs -- never on a dropped pass and never on a throwing invoke.
    gui = gc_get()(g_slot.gui_root);
    if (gui == nullptr || g_slot_epoch != pass_epoch || g_pending_stop) {
      // slot cleared / re-armed / stopping under us: no issue, no latch, no event.
    } else if (!gate_open(pinned, tid, &wait)) {
      emit_wait(wait, tid);  // auto-open off / lease dead / revoked mid-pass
    } else {
      // Latch the attempt BEFORE the invoke so an uncertain invoke is never retried.
      g_slot.attempted = true;
      g_slot.automated = true;
      publish_slot();
      bool threw = false;
      if (invoke_void(g_cfg.m_open, gui, nullptr, &threw)) {
        // The invoke completed: announce the issue (the adapter sets the legacy opened/checked
        // flags + 'Opening gift' status).
        push_event(PGR_GIFT_EVENT_ISSUED, PGR_GIFT_ERR_REASON_NONE, tid);
      } else {
        push_event(PGR_GIFT_EVENT_ERROR, PGR_GIFT_ERR_REASON_INVOKE, tid);
      }
    }
  }

  leave_frame();
  return PGR_GIFT_OK;
}

int api_sample(PgrIncomingGiftsSample *out) {
  if (out == nullptr) return PGR_GIFT_ERR_INVALID_ARG;
  const uint32_t roots = root_count();
  // Owned/attempted/automated come from the atomic mirrors (published on the owner thread), never
  // from g_slot -- reading the owner-thread Slot from another thread would be a data race.
  const uint32_t owned = g_m_owned.load(std::memory_order_relaxed);
  const uint32_t attempted = g_m_attempted.load(std::memory_order_relaxed);
  const uint32_t automated = g_m_automated.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_state_mutex);
  out->version = PGR_GIFT_SAMPLE_VERSION;
  out->size = sizeof(PgrIncomingGiftsSample);
  out->issued_serial = g_issued_serial;
  out->wait_serial = g_wait_serial;
  out->error_serial = g_error_serial;
  out->skip_serial = g_skip_serial;
  out->dialog_serial = g_dialog_serial;
  out->last_tid = g_last_tid;
  out->root_count = roots;
  out->owned = owned;
  out->attempted = attempted;
  out->automated = automated;
  out->fault = g_fault ? 1u : 0u;
  out->overflow = g_overflow ? 1u : 0u;
  out->last_wait_reason = g_last_wait_reason;
  out->pending_events = g_fifo_count;
  return PGR_GIFT_OK;
}

int api_drain(PgrIncomingGiftsEvent *out) {
  if (out == nullptr) return PGR_GIFT_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_fifo_count == 0) return 0;
  *out = g_fifo[g_fifo_head];
  g_fifo_head = (g_fifo_head + 1) % PGR_GIFT_EVENT_CAP;
  --g_fifo_count;
  return 1;
}

int api_is_open(uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return 0;
  const uint64_t tid = current_tid();
  const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (owner == 0 || tid != owner || proven_tid == 0 || tid != proven_tid) return 0;
  if (g_active_depth.load(std::memory_order_relaxed) != 0) return 0;  // no reentrant managed work
  if (g_slot.gui_root == 0) return 0;

  // is_alive() may relocate the owned object or let a reentrant CLOSE/stop free the slot, so this
  // must run inside an active frame (a reentrant stopping tick latches instead of recursing), pin
  // the epoch, re-resolve the GUI from its durable root AFTER the getter, and leave_frame on every
  // return (the deferred teardown owns any latched stop).
  g_active_depth.fetch_add(1, std::memory_order_relaxed);
  const uint64_t pass_epoch = g_slot_epoch;
  int result = 0;
  void *gui = gc_get()(g_slot.gui_root);
  if (gui != nullptr && is_alive(gui)) {
    gui = (g_slot_epoch == pass_epoch && !g_pending_stop && g_slot.gui_root != 0)
              ? gc_get()(g_slot.gui_root)
              : nullptr;
    if (gui != nullptr) result = gift_open(gui) ? 1 : 0;
  }
  leave_frame();
  return result;
}

const PgrIncomingGiftsApi kApiTable = {
    PGR_GIFT_API_VERSION, static_cast<uint32_t>(sizeof(PgrIncomingGiftsApi)),
    &api_install,         &api_update,
    &api_observe,         &api_tick,
    &api_sample,          &api_drain,
    &api_is_open,
};

}  // namespace

extern "C" const void *pgrazor_incoming_gifts_api(uint32_t version) {
  if (version != PGR_GIFT_API_VERSION) return nullptr;
  return &kApiTable;
}

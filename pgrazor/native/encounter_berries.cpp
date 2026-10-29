// PGRazor native migration: encounter berry priority / request / lifetime controller
// (feature id 18). See encounter_berries.h for the ABI, the "installs no hooks" rationale and
// the begin/finish_resume protocol. Built -fno-exceptions / -fno-rtti like every sibling: the
// JS "throw" paths become fail-closed error flags and RAII destructors free each managed root
// exactly once on scope exit. No mutex is ever held across a managed call or a GC-handle call.
//
// Threading: submit / peek_held / observe_enter / observe_exit / result_begin / result_end /
// tick / begin_resume / finish_resume are the MANAGED entry points. tick latches the permanent
// Unity identity once; every managed entry point then requires that SAME latched owner AND the
// caller's self-reported proven_tid to equal both the owner and the live OS tid (a thread merely
// reporting its own gettid() is NOT enough -- it must be the bound owner). Before the owner is
// latched (no tick yet) the managed entry points are inert no-ops, so a self-reported tid can
// never drive the state machine. api_update is the one policy call allowed off-thread (it only
// ever DISARMS on a foreign thread), guarded by its own brief lock.
//
// The state machine is three disjoint slots, never a single shared "pending":
//   * g_attempt  -- the live held throw being decided/fed (HELD -> FEEDING). Suppresses peek and
//                   dedups submit. Cancelled WITHOUT a submit by enter/exit/dead-scope.
//   * g_queue    -- a resume tick requested but JS has not claimed yet (unclaimed). Detached:
//                   no longer suppresses peek / dedups submit. Cancelled WITHOUT a submit by
//                   enter/exit (exit or a replaced encounter cancels an UNCLAIMED throw).
//   * g_claimed  -- begin_resume handed this frame + ticket to JS; it SURVIVES enter/exit/epoch
//                   until finish_resume frees it in the JS `finally`. Because the attempt is
//                   reset the moment a resume is queued, submit returns PASS for the resumed ball
//                   (the resume's original-submit is never re-swallowed as a duplicate).

#include "encounter_berries.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

static_assert(sizeof(pgr_berry_config_v1) == 256, "config must be 256 bytes");
static_assert(sizeof(pgr_berry_sample_v1) == 64, "sample must be 64 bytes");
static_assert(sizeof(pgr_berry_resume_v1) == 40, "resume POD must be 40 bytes");
static_assert(sizeof(pgr_berry_event_v1) == 16, "event must be 16 bytes");
static_assert(sizeof(pgr_berry_api_v1) == 112, "table must be 112 bytes");
static_assert(offsetof(pgr_berry_config_v1, gchandle_new) == 8, "layout");
static_assert(offsetof(pgr_berry_config_v1, m_ball_type) == 88, "layout");
static_assert(offsetof(pgr_berry_config_v1, class_pokeball) == 144, "layout");
static_assert(offsetof(pgr_berry_config_v1, f_scope_bag) == 192, "layout");
static_assert(offsetof(pgr_berry_config_v1, ball_type_master) == 232, "layout");
static_assert(offsetof(pgr_berry_config_v1, reserved1) == 244, "layout");
static_assert(offsetof(pgr_berry_sample_v1, owned_roots) == 56, "sample owned_roots @56");
static_assert(offsetof(pgr_berry_resume_v1, cause) == 32, "resume cause @32");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_BERRY_MAX_LEASE_MS;
constexpr uint64_t kDeadlineMs = PGR_BERRY_DEADLINE_MS;
constexpr uint64_t kPulseMs    = PGR_BERRY_PULSE_MS;
constexpr int      kMaxPri     = PGR_BERRY_MAX_PRIORITY;

// --- lifetime / publication --------------------------------------------------------
std::mutex           g_install_mutex;
pgr_berry_config_v1  g_cfg{};
std::atomic<bool>    g_installed{false};
bool                 g_install_attempted = false;
int                  g_install_error = PGR_BERRY_ERR_NOT_INSTALLED;

// --- thread identity ----------------------------------------------------------------
std::atomic<uint64_t> g_unity_tid{0};  // permanent Unity identity, latched once from tick

// --- arming (any thread; brief lock) ------------------------------------------------
std::mutex g_arm_mutex;
int32_t    g_ids[kMaxPri] = {0, 0, 0};
uint32_t   g_id_count = 0;
uint64_t   g_lease_until_ms = 0;
uint64_t g_policy_generation = 1;

// --- telemetry (atomic; pull-only; any thread; never frees) -------------------------
std::atomic<uint32_t> g_overlay_active{0};
std::atomic<uint32_t> g_overlay_busy{0};
std::atomic<uint32_t> g_ui_open{0};
std::atomic<uint32_t> g_failed{0};
std::atomic<uint64_t> g_impact_serial{0};
std::atomic<uint64_t> g_request_serial{0};
std::atomic<uint64_t> g_resume_serial{0};
std::atomic<uint32_t> g_fault_serial{0};
std::atomic<uint32_t> g_dropped_events{0};
std::atomic<uint32_t> g_durable_roots{0};  // scope + attempt + queue + claimed GC handles
std::atomic<uint32_t> g_active_frames{0};  // owner-thread reentrancy depth (conservative guard)

// Fail-closed telemetry latch: set when the bounded FIFO drops an event (telemetry integrity
// lost), cleared once the FIFO drains empty again. While set it BLOCKS starting a NEW hold and
// BLOCKS issuing a new feed (preflight releases the held throw instead), but it never consults
// the begin/finish_resume path -- an already-held/queued/claimed throw is still released.
std::atomic<bool> g_feed_locked{false};

// Stop request. Set (and the arming disarmed) immediately when any tick(stopping!=0) arrives,
// even a nested one; drained on the OUTERMOST tick frame. NOT a terminal latch -- a later
// tick(stopping==0) resumes normal operation once JS re-arms via update().
std::atomic<bool> g_stop_requested{false};

// --- bounded telemetry FIFO (owner thread writes; owner thread drains) --------------
std::mutex          g_fifo_mutex;
pgr_berry_event_v1  g_fifo[PGR_BERRY_EVENT_CAP]{};
uint32_t            g_fifo_head = 0, g_fifo_tail = 0, g_fifo_size = 0;

// --- owner-thread-only state --------------------------------------------------------
uint64_t g_epoch = 0;          // bumped on every enter/exit; cancels straddling close/new-enter
bool     g_in_tick = false;    // non-reentrant tick guard (result_begin/end are NOT gated)
void    *g_scope_handle = nullptr;
uint64_t g_pulse_next_ms = 0;
uint64_t g_resume_ticket = 0;  // monotonic resume-claim ticket source
uint64_t g_next_generation = 0;

enum AttemptState { ATT_IDLE = 0, ATT_HELD = 1, ATT_FEEDING = 2 };

struct Attempt {
  AttemptState state = ATT_IDLE;
  void    *ball_handle = nullptr;
  void    *callback_handle = nullptr;
  uint64_t first = 0, second = 0;
  void    *method = nullptr;
  int32_t  priorities[kMaxPri] = {0, 0, 0};
  uint32_t priority_count = 0;
  int32_t  selected = 0;
  uint64_t deadline_ms = 0;
  uint64_t epoch = 0;          // scope epoch captured at submit
  uint64_t generation = 0;     // bumped per queued impact (result-pass token)
  uint64_t policy_generation = 0;
  bool     done = false;       // the server responded (result/error latched)
  bool     result_active = false;  // result_begin issued a token for this generation
} g_attempt;

struct Queue {  // a resume tick requested; JS has NOT claimed it yet (unclaimed, detached)
  bool     pending = false;
  void    *ball_handle = nullptr;
  void    *callback_handle = nullptr;
  uint64_t first = 0, second = 0;
  void    *method = nullptr;
  uint32_t cause = PGR_BERRY_RESUME_MISC;
  uint64_t epoch = 0;          // the attempt epoch; re-checked at begin_resume
  uint64_t generation = 0;
} g_queue;

struct Claimed {  // begin_resume handed this to JS; survives enter/exit until finish_resume
  bool     active = false;
  uint64_t ticket = 0;
  void    *ball_handle = nullptr;
  void    *callback_handle = nullptr;
  uint32_t cause = PGR_BERRY_RESUME_MISC;
} g_claimed;

inline bool any_busy() {
  return g_attempt.state != ATT_IDLE || g_queue.pending || g_claimed.active;
}

// --- clock / tid --------------------------------------------------------------------
uint64_t boottime_ms() {
  struct timespec ts;
#if defined(CLOCK_BOOTTIME)
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return 0;
#elif defined(CLOCK_MONOTONIC)
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
#else
  if (timespec_get(&ts, TIME_UTC) == 0) return 0;
#endif
  if (ts.tv_sec < 0 || ts.tv_nsec < 0) return 0;
  return static_cast<uint64_t>(ts.tv_sec) * 1000ull + static_cast<uint64_t>(ts.tv_nsec) / 1000000ull;
}
inline uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

// A managed entry point is on the owner thread IFF the owner has been latched AND the caller's
// self-reported proven_tid equals BOTH the latched owner AND the live OS tid. A thread cannot
// drive the machine merely by reporting its own gettid() -- it must be the bound owner.
inline bool on_unity(uint64_t proven_tid) {
  const uint64_t owner = g_unity_tid.load(std::memory_order_acquire);
  return owner != 0 && proven_tid == owner && current_tid() == proven_tid;
}

// --- il2cpp primitives over the validated config ------------------------------------
inline void *gc_new(void *o)    { return o ? reinterpret_cast<void *(*)(void *, int)>(g_cfg.gchandle_new)(o, 0) : nullptr; }
inline void *gc_target(void *h) { return h ? reinterpret_cast<void *(*)(void *)>(g_cfg.gchandle_get_target)(h) : nullptr; }
inline void  gc_free(void *h)   { if (h) reinterpret_cast<void (*)(void *)>(g_cfg.gchandle_free)(h); }
inline void *obj_class(void *o) { return o ? reinterpret_cast<void *(*)(void *)>(g_cfg.object_get_class)(o) : nullptr; }
inline void *obj_unbox(void *o) { return reinterpret_cast<void *(*)(void *)>(g_cfg.object_unbox)(o); }
inline bool  exact(void *o, void *k) { return o != nullptr && obj_class(o) == k; }
inline void  field_get(void *o, void *f, void *out) {
  reinterpret_cast<void (*)(void *, void *, void *)>(g_cfg.field_get_value)(o, f, out);
}
inline void *read_ref(void *o, void *f) { void *v = nullptr; field_get(o, f, &v); return v; }
inline int32_t read_s32(void *o, void *f) { int32_t v = 0; field_get(o, f, &v); return v; }

// Invoke a managed method; on a managed exception set *err and return null.
inline void *invoke(void *method, void *obj, void **params, bool *err) {
  void *exc = nullptr;
  void *r = reinterpret_cast<void *(*)(void *, void *, void **, void **)>(g_cfg.runtime_invoke)(
      method, obj, params, &exc);
  if (exc != nullptr) { *err = true; return nullptr; }
  return r;
}

// Boxed bool read: root the box across the unbox + exact-class check, fail on a value > 1.
bool invoke_bool(void *method, void *obj, void **params, bool *out, bool *err) {
  void *box = invoke(method, obj, params, err);
  if (*err) return false;
  void *h = gc_new(box);
  if (box != nullptr && h == nullptr) { *err = true; return false; }
  void *b = gc_target(h);
  bool ok = exact(b, g_cfg.class_bool);
  uint8_t v = 0;
  if (ok) { void *raw = obj_unbox(b); if (raw) v = *reinterpret_cast<uint8_t *>(raw); else ok = false; }
  gc_free(h);
  if (!ok || v > 1) { *err = true; return false; }
  *out = v != 0;
  return true;
}
bool invoke_int(void *method, void *obj, void **params, int32_t *out, bool *err) {
  void *box = invoke(method, obj, params, err);
  if (*err) return false;
  void *h = gc_new(box);
  if (box != nullptr && h == nullptr) { *err = true; return false; }
  void *b = gc_target(h);
  bool ok = exact(b, method == g_cfg.m_ball_type ? g_cfg.class_ball_type : g_cfg.class_int);
  int32_t v = 0;
  if (ok) { void *raw = obj_unbox(b); if (raw) v = *reinterpret_cast<int32_t *>(raw); else ok = false; }
  gc_free(h);
  if (!ok) { *err = true; return false; }
  *out = v;
  return true;
}

// --- RAII transient root ------------------------------------------------------------
// reset() detaches (nulls the member) BEFORE freeing, so a reentrant free observing the struct
// can never double-free. release() hands the handle to a durable slot without freeing.
struct Root {
  void *h = nullptr;
  Root() = default;
  Root(const Root &) = delete;
  Root &operator=(const Root &) = delete;
  void acquire(void *obj, bool *err) {
    reset();
    if (obj == nullptr) { *err = true; return; }
    h = gc_new(obj);
    if (h == nullptr) *err = true;
  }
  void *target() const { return h ? gc_target(h) : nullptr; }
  void reset() { void *x = h; h = nullptr; if (x) gc_free(x); }
  void *release() { void *x = h; h = nullptr; return x; }
  ~Root() { reset(); }
};

// --- durable root accounting (conservative: handles + reentrancy depth) -------------
void refresh_root_count() {
  uint32_t n = 0;
  if (g_scope_handle) ++n;
  if (g_attempt.ball_handle) ++n;
  if (g_attempt.callback_handle) ++n;
  if (g_queue.ball_handle) ++n;
  if (g_queue.callback_handle) ++n;
  if (g_claimed.ball_handle) ++n;
  if (g_claimed.callback_handle) ++n;
  g_durable_roots.store(n, std::memory_order_release);
}

// --- bounded FIFO (owner thread) ----------------------------------------------------
void push_event(uint32_t code, int32_t id, int32_t aux) {
  std::lock_guard<std::mutex> lock(g_fifo_mutex);
  if (g_fifo_size >= PGR_BERRY_EVENT_CAP) {
    g_dropped_events.fetch_add(1, std::memory_order_relaxed);
    g_feed_locked.store(true, std::memory_order_release);  // fail-closed: no new holds/feeds
    return;
  }
  g_fifo[g_fifo_tail] = pgr_berry_event_v1{code, id, aux, 0};
  g_fifo_tail = (g_fifo_tail + 1) % PGR_BERRY_EVENT_CAP;
  ++g_fifo_size;
}

void emit_overlay() {
  const uint32_t active = g_scope_handle ? 1u : 0u;
  const uint32_t busy = any_busy() ? 1u : 0u;
  const uint32_t ui_open = g_ui_open.load(std::memory_order_acquire) ? 1u : 0u;
  g_overlay_active.store(active, std::memory_order_release);
  g_overlay_busy.store(busy, std::memory_order_release);
  // aux bit0 active, bit1 busy, bit2 ui_open -- JS gates the overlay on the open feed UI, exactly
  // like the legacy overlay's EncounterUI.currentState check (not scope existence alone).
  push_event(PGR_BERRY_EVT_ENCOUNTER, 0,
             static_cast<int32_t>(active | (busy << 1) | (ui_open << 2)));
}

void note_error(int32_t id, int32_t reason) {
  g_fault_serial.fetch_add(1, std::memory_order_release);
  push_event(PGR_BERRY_EVT_ERROR, id, reason);
}

// --- slot lifetime (owner thread) ---------------------------------------------------
void free_attempt() {
  void *b = g_attempt.ball_handle, *c = g_attempt.callback_handle;
  g_attempt = Attempt{};
  gc_free(b);
  gc_free(c);
}
void free_queue() {
  void *b = g_queue.ball_handle, *c = g_queue.callback_handle;
  g_queue = Queue{};
  gc_free(b);
  gc_free(c);
}
void free_claimed() {
  void *b = g_claimed.ball_handle, *c = g_claimed.callback_handle;
  g_claimed = Claimed{};
  gc_free(b);
  gc_free(c);
}

// Move the held attempt into the unclaimed resume queue (detach): the ball + callback roots are
// transferred (NOT freed), the attempt is reset so submit/peek stop treating that ball as held.
void move_attempt_to_queue(uint32_t cause) {
  if (g_attempt.state == ATT_IDLE) return;
  g_queue = Queue{};
  g_queue.pending = true;
  g_queue.ball_handle = g_attempt.ball_handle;
  g_queue.callback_handle = g_attempt.callback_handle;
  g_queue.first = g_attempt.first;
  g_queue.second = g_attempt.second;
  g_queue.method = g_attempt.method;
  g_queue.cause = cause;
  g_queue.epoch = g_attempt.epoch;
  g_queue.generation = g_attempt.generation;
  g_attempt = Attempt{};  // ownership of the ball/callback roots moved to the queue; do not free
  refresh_root_count();
  emit_overlay();
}

void move_queue_to_claimed(uint64_t ticket) {
  g_claimed = Claimed{};
  g_claimed.active = true;
  g_claimed.ticket = ticket;
  g_claimed.ball_handle = g_queue.ball_handle;
  g_claimed.callback_handle = g_queue.callback_handle;
  g_claimed.cause = g_queue.cause;
  g_queue = Queue{};
}

// Cancel the UNCLAIMED work (attempt + queue) and drop the scope WITHOUT a submit. A claimed
// resume frame is left for finish_resume (deferred, JS owns that ticket).
void cancel_and_drop_scope() {
  // Detach the entire old state before the first free, which may arm a new scope.
  const Attempt old_attempt = g_attempt;
  const Queue old_queue = g_queue;
  void *old_scope = g_scope_handle;
  g_attempt = Attempt{}; g_queue = Queue{}; g_scope_handle = nullptr;
  g_pulse_next_ms = 0; g_ui_open.store(0, std::memory_order_release);
  gc_free(old_attempt.ball_handle); gc_free(old_attempt.callback_handle);
  gc_free(old_queue.ball_handle); gc_free(old_queue.callback_handle); gc_free(old_scope);
  refresh_root_count();
}

// --- preflight + feed request (owner thread, inside tick) ---------------------------
// Mirrors berryTick lines 39-51. *resume_now receives true when a bypass/fault means the held
// throw must be released. The scope is held in a caller-owned Root and RE-RESOLVED before every
// managed call; after every call the attempt generation + epoch are re-checked, so a reentrant
// enter/exit (which would reset the attempt and free the scope) can never make us feed a closed
// scope or overwrite freshly reset state. A false return with *resume_now==false means the
// reentrant handler already tore the attempt down -- the caller must not touch it.
bool feed_authorized(uint64_t generation) {
  if (g_stop_requested.load(std::memory_order_acquire) || g_feed_locked.load(std::memory_order_acquire)) return false;
  const uint64_t now = boottime_ms();
  std::lock_guard<std::mutex> lock(g_arm_mutex);
  return now && g_id_count && now < g_lease_until_ms && generation == g_policy_generation;
}

void preflight(Root &scope_r, bool *resume_now) {
  *resume_now = false;
  bool err = false;
  const uint64_t local_gen = g_attempt.generation;
  const uint64_t local_epoch = g_attempt.epoch;
  const uint64_t policy_generation = g_attempt.policy_generation;
  auto gen_ok = [&]() {
    return g_attempt.state == ATT_HELD && g_attempt.generation == local_gen &&
           g_epoch == local_epoch;
  };

  // Fail-closed: if telemetry integrity is lost, do not START a feed -- release the held throw.
  if (!feed_authorized(policy_generation)) { *resume_now = true; return; }

  // Stage "ball checks": root the ball, verify it is alive, not already bread-held.
  Root ball_r; ball_r.acquire(gc_target(g_attempt.ball_handle), &err);
  if (err) { *resume_now = true; return; }
  bool ball_alive = false;
  { void *ap[1] = {ball_r.target()}; invoke_bool(g_cfg.m_alive, nullptr, ap, &ball_alive, &err); }
  if (err || !ball_alive) { *resume_now = true; return; }
  if (!gen_ok()) return;
  bool bread = false;
  invoke_bool(g_cfg.m_bread, ball_r.target(), nullptr, &bread, &err);
  if (err || bread) { *resume_now = true; return; }
  if (!gen_ok()) return;

  // Stage "encounter checks": CanUseBerry and not in photo mode.
  bool can = false, photo = false;
  invoke_bool(g_cfg.m_can, scope_r.target(), nullptr, &can, &err);
  if (err) { *resume_now = true; return; }
  if (!gen_ok()) return;
  invoke_bool(g_cfg.m_photo, scope_r.target(), nullptr, &photo, &err);
  if (err || !can || photo) { *resume_now = true; return; }
  if (!gen_ok()) return;

  // Stage "ball type": MasterBall (== ball_type_master) bypasses.
  int32_t ball_type = 0;
  invoke_int(g_cfg.m_ball_type, ball_r.target(), nullptr, &ball_type, &err);
  if (err) { note_error(0, PGR_BERRY_REASON_BALLTYPE_FAULT); *resume_now = true; return; }
  if (!gen_ok()) return;
  if (ball_type == g_cfg.ball_type_master) { *resume_now = true; return; }

  // Stage "inventory": bag of the exact class, first stocked priority id (snapshot taken at
  // submit; a concurrent disarm/re-arm cannot alter this in-flight attempt's priority list).
  void *bag = read_ref(scope_r.target(), g_cfg.f_scope_bag);
  if (!exact(bag, g_cfg.class_bag)) { *resume_now = true; return; }
  Root bag_r; bag_r.acquire(bag, &err);
  if (err) { note_error(0, PGR_BERRY_REASON_GCHANDLE); *resume_now = true; return; }
  int32_t selected = 0;
  for (uint32_t i = 0; i < g_attempt.priority_count; ++i) {
    int32_t id = g_attempt.priorities[i], count = 0;
    void *qp[1] = {&id};
    invoke_int(g_cfg.m_quantity, bag_r.target(), qp, &count, &err);
    if (err) { note_error(0, PGR_BERRY_REASON_INVENTORY); *resume_now = true; return; }
    if (!gen_ok()) return;
    if (count > 0) { selected = id; break; }
  }
  if (selected == 0) { *resume_now = true; return; }  // no stock -> submit without feeding

  // Stage "create callback": allocate + root, write + read back scope and berry id.
  void *cb = reinterpret_cast<void *(*)(void *)>(g_cfg.object_new)(g_cfg.class_callback);
  Root cb_r; cb_r.acquire(cb, &err);
  if (err) { note_error(selected, PGR_BERRY_REASON_GCHANDLE); *resume_now = true; return; }
  void *scope = scope_r.target();
  cb = cb_r.target();
  reinterpret_cast<void (*)(void *, void *, void *)>(g_cfg.field_set_value_object)(
      cb, g_cfg.f_callback_scope, scope);
  int32_t berry = selected;
  cb = cb_r.target();
  reinterpret_cast<void (*)(void *, void *, void *)>(g_cfg.field_set_value)(
      cb, g_cfg.f_callback_berry, &berry);
  cb = cb_r.target();
  if (read_ref(cb, g_cfg.f_callback_scope) != scope_r.target() ||
      read_s32(cb_r.target(), g_cfg.f_callback_berry) != selected) {
    g_failed.store(1, std::memory_order_release);
    note_error(selected, PGR_BERRY_REASON_CALLBACK_READBACK);
    *resume_now = true;
    return;
  }
  // If a reentrant enter/exit reset the attempt while we were building the callback, drop the
  // transient callback root (RAII) and bail WITHOUT mutating the freshly reset state.
  if (!gen_ok()) return;
  if (!feed_authorized(policy_generation)) { *resume_now = true; return; }

  // Commit: transfer the callback root into the attempt and transition to FEEDING. result_begin
  // only mints a token once the attempt is FEEDING, so a result that fires synchronously inside
  // the feed request below matches this exact generation.
  g_attempt.callback_handle = cb_r.release();
  g_attempt.selected = selected;
  g_attempt.state = ATT_FEEDING;
  g_attempt.done = false;
  g_attempt.result_active = false;
  refresh_root_count();

  // Stage "request": issue the single feed request. A managed throw latches failed and resumes
  // (the server outcome is unknown -> no retry).
  cb = gc_target(g_attempt.callback_handle);
  if (g_attempt.generation != local_gen || g_epoch != local_epoch) return;
  if (!cb) { g_attempt.state = ATT_HELD; *resume_now = true; return; }
  if (!feed_authorized(policy_generation)) {
    g_attempt.state = ATT_HELD;
    *resume_now = true;
    return;
  }
  g_request_serial.fetch_add(1, std::memory_order_release);
  int32_t one = 1;
  void *dp[1] = {&one};
  invoke(g_cfg.m_direct, cb, dp, &err);
  push_event(PGR_BERRY_EVT_FEED_REQUESTED, selected, 0);
  if (g_attempt.generation != local_gen || g_epoch != local_epoch) return;
  if (err) {
    g_failed.store(1, std::memory_order_release);
    note_error(selected, PGR_BERRY_REASON_REQUEST_FAULT);
    *resume_now = true;
    return;
  }
}

// --- frame guard --------------------------------------------------------------------
void run_stop();
struct FrameGuard {
  FrameGuard() { g_active_frames.fetch_add(1, std::memory_order_relaxed); }
  ~FrameGuard() {
    if (g_active_frames.load(std::memory_order_relaxed) == 1 && g_stop_requested.load(std::memory_order_acquire)) {
      run_stop(); // keep STOP latched while cleanup calls can re-enter
      g_stop_requested.store(false, std::memory_order_release);
    }
    g_active_frames.fetch_sub(1, std::memory_order_relaxed);
  }
};

// --- arming disarm (any thread; brief lock) -----------------------------------------
void disarm_locked() {
  std::lock_guard<std::mutex> lock(g_arm_mutex);
  g_id_count = 0;
  g_ids[0] = g_ids[1] = g_ids[2] = 0;
  g_lease_until_ms = 0;
  ++g_policy_generation;
}

// --- table functions ----------------------------------------------------------------
bool config_ok(const pgr_berry_config_v1 *c) {
  const void *const ptrs[] = {
      c->gchandle_new, c->gchandle_get_target, c->gchandle_free, c->object_get_class,
      c->object_unbox, c->object_new, c->runtime_invoke, c->field_get_value, c->field_set_value,
      c->field_set_value_object, c->m_ball_type, c->m_bread, c->m_can, c->m_photo, c->m_quantity,
      c->m_direct, c->m_alive, c->class_pokeball, c->class_scope, c->class_bag, c->class_bool,
      c->class_int, c->class_callback, c->f_scope_bag, c->f_scope_ui, c->f_ui_state,
      c->f_callback_scope, c->f_callback_berry};
  for (const void *p : ptrs) if (p == nullptr) return false;
  if (c->reserved0 != 0 || c->reserved1 != 0 || !c->class_ball_type) return false;
  return true;
}

int api_install(const pgr_berry_config_v1 *config) {
  if (config == nullptr) return PGR_BERRY_ERR_NULL_ARG;
  std::lock_guard<std::mutex> lock(g_install_mutex);
  if (config->version != PGR_BERRY_API_VERSION ||
      config->size != sizeof(pgr_berry_config_v1) || !config_ok(config))
    return PGR_BERRY_ERR_BAD_CONFIG;

  if (g_installed.load(std::memory_order_acquire))
    return (std::memcmp(&g_cfg, config, sizeof(g_cfg)) == 0) ? PGR_BERRY_OK
                                                             : PGR_BERRY_ERR_ALREADY_BOUND;
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;
  if (boottime_ms() == 0) return g_install_error = PGR_BERRY_ERR_NO_CLOCK;

  g_cfg = *config;
  g_installed.store(true, std::memory_order_release);
  push_event(PGR_BERRY_EVT_READY, 0, 1);
  return g_install_error = PGR_BERRY_OK;
}

int api_update(const int32_t *ids, uint32_t count, uint64_t remaining_lease_ms,
               uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_BERRY_ERR_NOT_INSTALLED;
  // Policy update is allowed off-thread (it only ever DISARMS on a foreign thread); it does not
  // require the latched Unity owner the way the managed state-machine entry points do.
  const uint64_t owner = g_unity_tid.load(std::memory_order_acquire);
  const bool wrong_thread = owner != 0 && proven_tid != 0 && proven_tid != owner;

  std::lock_guard<std::mutex> lock(g_arm_mutex);
  if (g_stop_requested.load(std::memory_order_acquire) || wrong_thread || proven_tid == 0 || count == 0 || count > static_cast<uint32_t>(kMaxPri) ||
      ids == nullptr) {
    g_id_count = 0;
    g_ids[0] = g_ids[1] = g_ids[2] = 0;
    g_lease_until_ms = 0;  // disarmed
    ++g_policy_generation;
    return wrong_thread ? PGR_BERRY_ERR_NULL_ARG : PGR_BERRY_OK;
  }
  const uint64_t now = boottime_ms();
  if (!g_id_count || !now || now >= g_lease_until_ms || remaining_lease_ms == 0)
    ++g_policy_generation;
  for (uint32_t i = 0; i < count; ++i) g_ids[i] = ids[i];
  for (uint32_t i = count; i < static_cast<uint32_t>(kMaxPri); ++i) g_ids[i] = 0;
  g_id_count = count;
  const uint64_t clamped = remaining_lease_ms > kMaxLeaseMs ? kMaxLeaseMs : remaining_lease_ms;
  g_lease_until_ms = (clamped > 0 && now != 0) ? now + clamped : 0;
  return PGR_BERRY_OK;
}

// Arming snapshot read used by submit().
bool armed_snapshot(int32_t out_ids[kMaxPri], uint32_t *out_count, uint64_t now, uint64_t *generation) {
  std::lock_guard<std::mutex> lock(g_arm_mutex);
  if (g_id_count == 0 || g_lease_until_ms == 0 || now == 0 || now >= g_lease_until_ms) return false;
  for (int i = 0; i < kMaxPri; ++i) out_ids[i] = g_ids[i];
  *out_count = g_id_count;
  *generation = g_policy_generation;
  return true;
}

int api_submit(void *ball, uint64_t first, uint64_t second, void *method, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire) || !on_unity(proven_tid)) return PGR_BERRY_PASS;
  FrameGuard guard;
  const bool is_drop = ((static_cast<uint32_t>(second) >> 16) & 0xffu) != 0;

  // cooldown-action telemetry: once per fresh throw observation (nothing busy, armed site).
  if (!any_busy()) push_event(PGR_BERRY_EVT_ACTION, 0, is_drop ? 1 : 0);

  const uint64_t now = boottime_ms();
  int32_t ids[kMaxPri] = {0, 0, 0};
  uint32_t count = 0;
  uint64_t policy_generation = 0;
  const bool armed = armed_snapshot(ids, &count, now, &policy_generation);
  if (!g_scope_handle || g_failed.load(std::memory_order_acquire) || !armed)
    return PGR_BERRY_PASS;

  // An already-held throw (HELD or FEEDING): the same ball is dropped (HOLD, do not resubmit);
  // any other ball passes through so the player can still throw a different ball.
  if (g_attempt.state != ATT_IDLE) {
    void *held = gc_target(g_attempt.ball_handle);
    return (held != nullptr && held == ball) ? PGR_BERRY_HOLD : PGR_BERRY_PASS;
  }
  // A queued/claimed resume is DETACHED: the resumed ball's original-submit must pass straight
  // through (never re-swallowed as a duplicate), and no fresh frame is queued mid-resume.
  if (g_queue.pending || g_claimed.active) return PGR_BERRY_PASS;

  // Fail-closed: while telemetry integrity is lost, do not start a NEW hold.
  if (g_feed_locked.load(std::memory_order_acquire)) return PGR_BERRY_PASS;

  // Eligibility: exact Pokeball, not a ball drop, not the special first==4 (MasterBall) word.
  if (!exact(ball, g_cfg.class_pokeball) || is_drop || static_cast<uint32_t>(first) == 4u)
    return PGR_BERRY_PASS;

  // Generation exhaustion pre-gate: never hold a ball whose resume token we could not mint.
  if (g_next_generation >= UINT64_MAX - 1) return PGR_BERRY_PASS;

  const uint64_t epoch = g_epoch, generation = g_next_generation;
  void *ball_handle = gc_new(ball);
  if (ball_handle == nullptr) { note_error(0, PGR_BERRY_REASON_GCHANDLE); return PGR_BERRY_PASS; }
  if (g_epoch != epoch || g_next_generation != generation || any_busy() ||
      !g_scope_handle || !feed_authorized(policy_generation)) {
    gc_free(ball_handle); return PGR_BERRY_PASS;
  }
  g_attempt = Attempt{};
  g_attempt.policy_generation = policy_generation;
  g_attempt.state = ATT_HELD;
  g_attempt.ball_handle = ball_handle;
  g_attempt.first = first;
  g_attempt.second = second;
  g_attempt.method = method;
  g_attempt.priority_count = count;
  for (int i = 0; i < kMaxPri; ++i) g_attempt.priorities[i] = ids[i];
  g_attempt.selected = ids[0];
  g_attempt.deadline_ms = (now != 0 ? now : boottime_ms()) + kDeadlineMs;
  g_attempt.epoch = g_epoch;
  g_attempt.generation = ++g_next_generation;
  refresh_root_count();
  g_impact_serial.fetch_add(1, std::memory_order_release);
  push_event(PGR_BERRY_EVT_IMPACT_QUEUED, ids[0], 0);
  emit_overlay();
  return PGR_BERRY_HOLD;
}

int api_peek_held(void *ball, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire) || !on_unity(proven_tid)) return PGR_BERRY_PASS;
  FrameGuard guard;
  if (g_attempt.state != ATT_IDLE) {  // a detached (queued/claimed) resume no longer suppresses
    void *held = gc_target(g_attempt.ball_handle);
    if (held != nullptr && held == ball) return PGR_BERRY_HOLD;  // suppress a stale promise
  }
  return PGR_BERRY_PASS;
}

int api_observe_enter(void *scope, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire) || !on_unity(proven_tid) ||
      g_stop_requested.load(std::memory_order_acquire)) return PGR_BERRY_OK;
  FrameGuard guard;
  const uint64_t before = g_epoch;
  bool err = false;
  Root incoming;
  if (exact(scope, g_cfg.class_scope)) incoming.acquire(scope, &err);
  if (g_epoch != before || g_stop_requested.load(std::memory_order_acquire)) return PGR_BERRY_OK;
  const uint64_t epoch = ++g_epoch;
  cancel_and_drop_scope();
  if (g_epoch != epoch || g_stop_requested.load(std::memory_order_acquire)) return PGR_BERRY_OK;
  g_scope_handle = err ? nullptr : incoming.release();
  g_failed.store(0, std::memory_order_release);
  refresh_root_count(); emit_overlay(); return PGR_BERRY_OK;
}

int api_observe_exit(void *scope, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire) || !on_unity(proven_tid)) return PGR_BERRY_OK;
  FrameGuard guard;
  const uint64_t epoch = g_epoch;
  void *cur = g_scope_handle ? gc_target(g_scope_handle) : nullptr;
  if (g_epoch != epoch || cur == nullptr || cur != scope) return PGR_BERRY_OK;  // only the live scope exits
  ++g_epoch;
  // Exit (or a replaced encounter) cancels an UNCLAIMED attempt/queue WITHOUT a submit; a claimed
  // resume frame is left to finish_resume (deferred cleanup).
  cancel_and_drop_scope();
  push_event(PGR_BERRY_EVT_COOLDOWN_ENC, 0, 0);
  emit_overlay();
  return PGR_BERRY_OK;
}

uint64_t api_result_begin(void *callback, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire) || !on_unity(proven_tid)) return 0;
  FrameGuard guard;
  const uint64_t generation = g_attempt.generation;
  // Only the CURRENT feeding attempt's callback mints a token, and only once (nested/re-entered
  // result_begin for the same attempt is rejected). The token IS the attempt generation, so a
  // late onLeave carrying a stale token cannot match a newer attempt.
  if (g_attempt.state != ATT_FEEDING || g_attempt.callback_handle == nullptr) return 0;
  if (g_attempt.result_active) return 0;
  void *cb = gc_target(g_attempt.callback_handle);
  if (cb == nullptr || cb != callback || g_attempt.generation != generation) return 0;
  g_attempt.result_active = true;
  return g_attempt.generation;  // generation starts at 1, so this is always nonzero
}

int api_result_end(uint64_t token, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_BERRY_ERR_NOT_INSTALLED;
  if (!on_unity(proven_tid) || token == 0) return PGR_BERRY_ERR_BAD_TOKEN;
  // Latch done only for the exact feeding attempt whose generation minted this token, consuming
  // the claim once. A replayed, stale (superseded generation), or cross-encounter token fails.
  if (g_attempt.state == ATT_FEEDING && g_attempt.result_active &&
      token == g_attempt.generation && g_attempt.epoch == g_epoch) {
    g_attempt.result_active = false;  // consume once
    g_attempt.done = true;
    return PGR_BERRY_OK;
  }
  return PGR_BERRY_ERR_BAD_TOKEN;
}

// The normal scope-driven pass (not a stop).
void run_tick_body(uint64_t now) {
  if (!g_scope_handle) return;
  const uint64_t epoch = g_epoch;
  bool err = false;
  // Root the scope for the whole body and RE-RESOLVE it before each managed call.
  Root scope_r; scope_r.acquire(gc_target(g_scope_handle), &err);
  if (g_epoch != epoch) return;
  if (err) { ++g_epoch; cancel_and_drop_scope(); emit_overlay(); return; }
  bool alive = false;
  { void *ap[1] = {scope_r.target()}; invoke_bool(g_cfg.m_alive, nullptr, ap, &alive, &err); }
  if (g_epoch != epoch || g_stop_requested.load(std::memory_order_acquire)) return;
  if (err || !alive) { ++g_epoch; cancel_and_drop_scope(); emit_overlay(); return; }

  // 500 ms overlay pulse (EncounterUI.currentState == state_open).
  if (now >= g_pulse_next_ms) {
    g_pulse_next_ms = now + kPulseMs;
    void *ui = read_ref(scope_r.target(), g_cfg.f_scope_ui);
    Root ui_r; bool ue = false;
    if (ui != nullptr) ui_r.acquire(ui, &ue);
    bool ui_alive = false;
    if (ui != nullptr && !ue) {
      void *ap[1] = {ui_r.target()}; bool e2 = false;
      invoke_bool(g_cfg.m_alive, nullptr, ap, &ui_alive, &e2);
      if (e2) ui_alive = false;
    }
    const bool open = ui != nullptr && !ue && ui_alive &&
                      read_s32(ui_r.target(), g_cfg.f_ui_state) == g_cfg.state_open;
    g_ui_open.store(open ? 1u : 0u, std::memory_order_release);
    emit_overlay();
  }

  if (g_epoch != epoch || g_stop_requested.load(std::memory_order_acquire)) return;
  // No held attempt (a queued/claimed resume is driven by begin/finish_resume) -> nothing to do.
  if (g_attempt.state == ATT_IDLE) return;

  // Completed feed: queue the resume (JS will zero getItemState().next on DONE).
  if (g_attempt.done) { move_attempt_to_queue(PGR_BERRY_RESUME_DONE); return; }

  // 8 s timeout: latch failed (no retry this encounter) and resume once.
  if (now > g_attempt.deadline_ms) {
    g_failed.store(1, std::memory_order_release);
    note_error(g_attempt.selected, PGR_BERRY_REASON_TIMEOUT);
    move_attempt_to_queue(PGR_BERRY_RESUME_TIMEOUT);
    return;
  }

  // Already requested: wait for done / deadline.
  if (g_attempt.state == ATT_FEEDING) return;

  // Lease expired before the feed started: resume without feeding.
  {
    uint64_t lease;
    { std::lock_guard<std::mutex> lk(g_arm_mutex); lease = g_lease_until_ms; }
    if (lease == 0 || now == 0 || now >= lease) {
      move_attempt_to_queue(PGR_BERRY_RESUME_LEASE);
      return;
    }
  }

  // Preflight + single feed request. A bypass/fault queues a resume; reentrancy that already tore
  // the attempt down leaves resume_now false and move_attempt_to_queue no-ops on an idle attempt.
  bool resume_now = false;
  const uint64_t generation = g_attempt.generation;
  preflight(scope_r, &resume_now);
  if (resume_now && g_epoch == epoch && g_attempt.generation == generation) move_attempt_to_queue(PGR_BERRY_RESUME_MISC);
}

// Outermost-frame stop drain. Disarm already happened the instant stop was requested; here we
// convert a held throw into a resume (released once if still valid), or drop a scope with nothing
// pending. Never a terminal latch -- a later non-stopping tick resumes normal operation.
void run_stop() {
  if (g_attempt.state != ATT_IDLE) {
    move_attempt_to_queue(PGR_BERRY_RESUME_STOP);
  } else if (!g_queue.pending && !g_claimed.active) {
    void *scope = g_scope_handle;
    g_scope_handle = nullptr;
    gc_free(scope);
    g_pulse_next_ms = 0;
    g_ui_open.store(0, std::memory_order_release);
    refresh_root_count();
    emit_overlay();
  }
}

int api_tick(uint64_t proven_tid, int stopping) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_BERRY_ERR_NOT_INSTALLED;
  // Basic self-consistency before latching the owner (reject a 0 or spoofed-vs-live tid).
  if (proven_tid == 0 || current_tid() != proven_tid) return PGR_BERRY_OK;
  uint64_t expected = 0;
  g_unity_tid.compare_exchange_strong(expected, proven_tid, std::memory_order_acq_rel);
  if (g_unity_tid.load(std::memory_order_acquire) != proven_tid) return PGR_BERRY_OK;

  // Record the stop request and disarm IMMEDIATELY (even on a nested tick) so no new hold can be
  // started between now and the outermost drain. A nested tick returns here; its request is drained
  // by the frame already running.
  if (stopping) { g_stop_requested.store(true, std::memory_order_release); disarm_locked(); }
  if (g_in_tick || g_active_frames.load(std::memory_order_relaxed)) return PGR_BERRY_OK;  // defer nested work

  g_in_tick = true;
  FrameGuard guard;
  const uint64_t now = boottime_ms();
  if (!g_stop_requested.load(std::memory_order_acquire)) run_tick_body(now);
  g_in_tick = false;
  return PGR_BERRY_OK;
}

uint64_t api_begin_resume(uint64_t proven_tid, pgr_berry_resume_v1 *out) {
  if (!out || !g_installed.load(std::memory_order_acquire) || !on_unity(proven_tid)) return 0;
  FrameGuard guard;
  if (!g_queue.pending || g_claimed.active) return 0;
  const uint64_t generation = g_queue.generation, epoch = g_queue.epoch;
  auto same = [&]() { return g_queue.pending && g_queue.generation == generation &&
                            g_epoch == epoch && !g_claimed.active; };
  bool err = false;
  Root scope, ball;
  scope.acquire(gc_target(g_scope_handle), &err);
  if (!same()) return 0;
  ball.acquire(gc_target(g_queue.ball_handle), &err);
  if (!same()) return 0;
  bool scope_alive = false, ball_alive = false;
  if (!err) { void *args[] = {scope.target()}; invoke_bool(g_cfg.m_alive, nullptr, args, &scope_alive, &err); }
  if (!same()) return 0;
  if (!err) { void *args[] = {ball.target()}; invoke_bool(g_cfg.m_alive, nullptr, args, &ball_alive, &err); }
  if (!same()) return 0;
  void *resolved = ball.target();
  if (!same()) return 0;
  if (err || !scope_alive || !ball_alive || !resolved || g_resume_ticket == UINT64_MAX) {
    free_queue(); refresh_root_count(); emit_overlay(); return 0;
  }
  out->first=g_queue.first; out->second=g_queue.second;
  out->method=g_queue.method; out->cause=g_queue.cause; out->reserved=0;
  const uint64_t ticket=++g_resume_ticket;
  move_queue_to_claimed(ticket); refresh_root_count(); emit_overlay();
  // Free temporary roots before exporting the final target. Their cleanup can
  // trigger callbacks/relocation; the claimed root owns the ball across that work.
  scope.reset(); ball.reset();
  out->ball=gc_target(g_claimed.ball_handle);
  if (!out->ball) { free_claimed(); refresh_root_count(); return 0; }
  return ticket;
}

int api_finish_resume(uint64_t ticket, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_BERRY_ERR_NOT_INSTALLED;
  if (!on_unity(proven_tid) || ticket == 0 || !g_claimed.active || ticket != g_claimed.ticket)
    return PGR_BERRY_ERR_BAD_TOKEN;  // never replay or free a newer/foreign record
  FrameGuard guard;
  free_claimed();  // detach-then-free the claimed ball + callback roots
  refresh_root_count();
  g_resume_serial.fetch_add(1, std::memory_order_release);
  emit_overlay();
  return PGR_BERRY_OK;
}

int api_sample(pgr_berry_sample_v1 *out) {
  if (out == nullptr) return PGR_BERRY_ERR_NULL_ARG;
  out->version = PGR_BERRY_API_VERSION;
  out->size = sizeof(pgr_berry_sample_v1);
  out->overlay_active = g_overlay_active.load(std::memory_order_acquire);
  out->overlay_busy = g_overlay_busy.load(std::memory_order_acquire);
  out->ui_open = g_ui_open.load(std::memory_order_acquire);
  out->failed = g_failed.load(std::memory_order_acquire);
  out->impact_serial = g_impact_serial.load(std::memory_order_acquire);
  out->request_serial = g_request_serial.load(std::memory_order_acquire);
  out->resume_serial = g_resume_serial.load(std::memory_order_acquire);
  out->fault_serial = g_fault_serial.load(std::memory_order_acquire);
  out->dropped_events = g_dropped_events.load(std::memory_order_acquire);
  // Conservative finish guard: durable GC handles + owner-thread reentrancy depth. It is NOT an
  // exact GC-allocation metric -- it is deliberately an upper bound the coordinator gates on.
  out->owned_roots = g_durable_roots.load(std::memory_order_acquire) +
                     g_active_frames.load(std::memory_order_acquire);
  out->reserved = 0;
  return PGR_BERRY_OK;
}

int api_next_event(pgr_berry_event_v1 *out) {
  if (out == nullptr) return 0;
  std::lock_guard<std::mutex> lock(g_fifo_mutex);
  if (g_fifo_size == 0) {
    g_feed_locked.store(false, std::memory_order_release);  // telemetry caught up -> re-enable
    return 0;
  }
  *out = g_fifo[g_fifo_head];
  g_fifo_head = (g_fifo_head + 1) % PGR_BERRY_EVENT_CAP;
  --g_fifo_size;
  if (g_fifo_size == 0) g_feed_locked.store(false, std::memory_order_release);
  return 1;
}

const pgr_berry_api_v1 g_api = {
    PGR_BERRY_API_VERSION,
    static_cast<uint32_t>(sizeof(pgr_berry_api_v1)),
    &api_install,
    &api_update,
    &api_submit,
    &api_peek_held,
    &api_observe_enter,
    &api_observe_exit,
    &api_result_begin,
    &api_result_end,
    &api_tick,
    &api_begin_resume,
    &api_finish_resume,
    &api_sample,
    &api_next_event,
};

}  // namespace

const void *pgrazor_encounter_berries_api(uint32_t version) {
  if (version != PGR_BERRY_API_VERSION) return nullptr;
  return &g_api;
}

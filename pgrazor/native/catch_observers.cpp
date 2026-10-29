#include "catch_observers.h"
#include "hook_engine.h"
#include "interaction_observer.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

// Feature 49 native catch observers. Exception cleanup preserves original native
// unwinding while managed failures remain fail-closed latched faults. The three
// Dobby hooks are process-lifetime; the single owned outcome ring is touched only on the
// proven Unity thread (every hook body gates on gettid()==g_tid). See catch_observers.h and
// the contract for the ABI and division of labour with feature8 (the speed engine).

static_assert(sizeof(PgrObsConfig) == 168, "config must be 168 bytes");
static_assert(sizeof(PgrObsSample) == 616, "sample must be 616 bytes");
static_assert(sizeof(PgrObsApi)    == 48,  "table must be 48 bytes");
static_assert(offsetof(PgrObsConfig, object_get_class)     == 8,   "layout: object_get_class");
static_assert(offsetof(PgrObsConfig, field_get_value)      == 16,  "layout: field_get_value");
static_assert(offsetof(PgrObsConfig, gchandle_new)         == 24,  "layout: gchandle_new");
static_assert(offsetof(PgrObsConfig, gchandle_get_target)  == 32,  "layout: gchandle_get_target");
static_assert(offsetof(PgrObsConfig, gchandle_free)        == 40,  "layout: gchandle_free");
static_assert(offsetof(PgrObsConfig, code_btxu)            == 48,  "layout: code_btxu");
static_assert(offsetof(PgrObsConfig, code_caught)          == 56,  "layout: code_caught");
static_assert(offsetof(PgrObsConfig, code_escaped)         == 64,  "layout: code_escaped");
static_assert(offsetof(PgrObsConfig, class_ball)           == 72,  "layout: class_ball");
static_assert(offsetof(PgrObsConfig, class_response)       == 80,  "layout: class_response");
static_assert(offsetof(PgrObsConfig, class_animator)       == 88,  "layout: class_animator");
static_assert(offsetof(PgrObsConfig, field_bread)          == 96,  "layout: field_bread");
static_assert(offsetof(PgrObsConfig, field_animator)       == 104, "layout: field_animator");
static_assert(offsetof(PgrObsConfig, field_status)         == 112, "layout: field_status");
static_assert(offsetof(PgrObsConfig, speed_api)            == 120, "layout: speed_api");
static_assert(offsetof(PgrObsConfig, status_success)       == 128, "layout: status_success");
static_assert(offsetof(PgrObsConfig, status_item_replacement) == 148, "layout: status_item_replacement");
static_assert(offsetof(PgrObsConfig, thread_current)       == 160, "layout: thread_current");
static_assert(offsetof(PgrObsSample, last_caller)          == 88,  "layout: last_caller");
static_assert(offsetof(PgrObsSample, event_head)           == 96,  "layout: event_head");
static_assert(offsetof(PgrObsSample, events)               == 104, "layout: events");

namespace {

constexpr uint64_t kMaxLeaseMs      = PGR_OBS_MAX_LEASE_MS; // 2500
constexpr uint64_t kOutcomeMs       = 8000u;               // LIMITS.outcomeMs
constexpr int      kOutcomeSeen     = 4;                    // LIMITS.outcomeSeen
constexpr uint64_t kOutcomesPerSec  = 4u;                   // LIMITS.outcomesPerSecond
constexpr uint64_t kHitsPerSec      = 1000u;               // LIMITS.hitsPerSecond

// --- game ABI typedefs -------------------------------------------------------------
typedef void *(*BtxuFn)(void *self, void *method);
typedef void  (*CompletionFn)(void *self, void *response, void *method);

// --- lifetime / publication --------------------------------------------------------
std::mutex        g_install_mutex;
PgrObsConfig      g_config{};
std::atomic<bool> g_installed{false};
bool              g_install_failed = false;

std::atomic<BtxuFn>       g_orig_btxu{nullptr};
std::atomic<CompletionFn> g_orig_caught{nullptr};
std::atomic<CompletionFn> g_orig_escaped{nullptr};
std::atomic<PgrObsBerryFilter> g_berry_filter{nullptr};
uint64_t g_berry_owner=0; // immutable before callback release-publication

// --- policy window (armed by update) -----------------------------------------------
std::atomic<int>      g_enabled{0};
std::atomic<uint64_t> g_lease_deadline{0}; // CLOCK_BOOTTIME ms; 0 == closed
std::atomic<uint64_t> g_tid{0};            // proven Unity thread id
std::atomic<uint64_t> g_cleanup_tid{0};    // retained after the window closes, for prune
std::atomic<int>      g_stopping{0};       // soft stop (update enabled==0 / stop())
std::atomic<int>      g_fault{0};          // latched fault
std::atomic<void *>   g_caller{nullptr};   // expected btxu return address (nullptr == any)
std::atomic<uint64_t> g_gen{1};            // revocation generation (bumped on stop)
std::atomic<void *>   g_last_caller{nullptr};

// --- telemetry (pull-only) ---------------------------------------------------------
std::atomic<uint64_t> g_confirmed{0}, g_suppressed{0}, g_dropped{0};
std::atomic<uint64_t> g_applied{0}, g_completed{0}, g_faults{0}, g_last_tid{0};
std::atomic<uint32_t> g_action{PGR_OBS_A_NONE}, g_reason{PGR_OBS_R_NONE};
std::atomic<int32_t>  g_status{0};

// Bounded event ring: emit() appends here (one writer at a time — every emit() runs on the
// proven Unity thread under g_busy) and sample() copies it out. Each slot stores its own
// global seq last, so a reader can tell a live slot from a stale/torn one and detect a gap.
struct EventSlot {
    std::atomic<uint64_t> seq;  // 0 == empty; otherwise the global sequence written to this slot
    std::atomic<uint64_t> tid;
    std::atomic<uint32_t> action, reason;
    std::atomic<int32_t>  status;
};
EventSlot             g_events[PGR_OBS_EVENT_RING];
std::atomic<uint64_t> g_event_head{0}; // total events ever appended (monotonic cursor horizon)

// --- proven-thread-only outcome state ----------------------------------------------
// The outcome seen ring. Touched ONLY on the proven Unity thread, so it needs no lock.
struct Seen { void *handle; uint64_t expires; };
Seen     g_seen[kOutcomeSeen];
std::atomic<int> g_seen_count{0};
// Sampling runs on the host polling thread. Keep the event payload/cursor one
// coherent snapshot; this lock never spans any managed operation or callback.
std::mutex g_event_mutex;
std::atomic<void *> g_deferred_self{nullptr}; // one bounded root, drained after a return-root failure
uint64_t g_hit_window = 0;  uint64_t g_hits = 0;      // btxu per-second rate window
uint64_t g_oc_window = 0;   uint64_t g_oc_hits = 0;   // outcome per-second rate window

// Reentry flag (not a mutex; never held across a managed call).
std::atomic<int> g_busy{0};

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

void emit(uint32_t action, uint32_t reason, int32_t status) {
    std::lock_guard<std::mutex> event_lock(g_event_mutex);
    uint64_t tid = static_cast<uint64_t>(gettid());
    // Keep the single-slot "last event" fields for debugging/compat.
    g_action.store(action, std::memory_order_relaxed);
    g_reason.store(reason, std::memory_order_relaxed);
    g_status.store(status, std::memory_order_relaxed);
    g_last_tid.store(tid, std::memory_order_relaxed);

    // Append to the ring. One writer at a time (proven thread under g_busy), so a plain
    // head increment is sufficient; the per-slot seq is stored LAST (release) so a concurrent
    // reader either sees the fully-written slot for this seq or an older one it will reject.
    uint64_t seq = g_event_head.load(std::memory_order_relaxed) + 1;
    EventSlot &slot = g_events[seq % PGR_OBS_EVENT_RING];
    slot.seq.store(0, std::memory_order_release);          // mark slot in-progress / invalid
    slot.tid.store(tid, std::memory_order_relaxed);
    slot.action.store(action, std::memory_order_relaxed);
    slot.reason.store(reason, std::memory_order_relaxed);
    slot.status.store(status, std::memory_order_relaxed);
    slot.seq.store(seq, std::memory_order_release);        // publish payload under this seq
    g_event_head.store(seq, std::memory_order_release);    // publish the new horizon last
}

void gate_close() {
    g_gen.fetch_add(1, std::memory_order_acq_rel);
    g_enabled.store(0, std::memory_order_relaxed);
    g_lease_deadline.store(0, std::memory_order_relaxed);
    g_tid.store(0, std::memory_order_relaxed);
}

void latch_fault() {
    if (g_fault.exchange(1, std::memory_order_relaxed) == 0) g_faults.fetch_add(1, std::memory_order_relaxed);
    g_stopping.store(1, std::memory_order_relaxed);
    gate_close();
}

// --- il2cpp helpers over validated config pointers --------------------------------
inline void *obj_class(void *o) { return reinterpret_cast<void *(*)(void *)>(g_config.object_get_class)(o); }
inline void  field_get(void *o, void *f, void *out) {
    reinterpret_cast<void (*)(void *, void *, void *)>(g_config.field_get_value)(o, f, out);
}
inline uint8_t read_u8(void *o, void *f)  { uint8_t v = 1;  field_get(o, f, &v); return v; }
inline int32_t read_i32(void *o, void *f) { int32_t v = 0;  field_get(o, f, &v); return v; }
inline void   *read_ptr(void *o, void *f) { void *v = nullptr; field_get(o, f, &v); return v; }
inline void *gc_new(void *obj)  { return reinterpret_cast<void *(*)(void *, int)>(g_config.gchandle_new)(obj, 0); }
inline void *gc_target(void *h) { return reinterpret_cast<void *(*)(void *)>(g_config.gchandle_get_target)(h); }
inline void  gc_free(void *h)   { reinterpret_cast<void (*)(void *)>(g_config.gchandle_free)(h); }
inline bool  exact(void *o, void *k) { return o != nullptr && obj_class(o) == k; }

// References allow the return-root-failure path to transfer its receiver to
// deferred cleanup. All other exits, including original exceptions, balance
// temporary roots and the reentry guard.
struct CatchFrame {
    void *&self;void *&response;bool &busy;
    ~CatchFrame(){if(self)gc_free(self);if(response)gc_free(response);if(busy)g_busy.store(0,std::memory_order_release);}
};

inline const PgrSpeedApi *speed() { return reinterpret_cast<const PgrSpeedApi *>(g_config.speed_api); }

bool on_proven_thread() {
    uint64_t tid = g_tid.load(std::memory_order_relaxed);
    return tid != 0 && static_cast<uint64_t>(gettid()) == tid;
}

// The thread that actually armed/acquired the window. Retained after the lease closes so the
// completion signal and the cleanup tick stay bound to one thread even past lease expiry.
bool on_cleanup_thread() {
    uint64_t tid = g_cleanup_tid.load(std::memory_order_relaxed);
    return tid != 0 && static_cast<uint64_t>(gettid()) == tid;
}

// Independent proof of il2cpp attachment (defence in depth; the hooks are only ever entered on
// the attached Unity thread, but the module verifies it itself rather than trusting gettid()).
// thread_current is validated non-null at install, so a null here means a genuinely unattached
// caller and every managed path fails closed.
bool attached() {
    void *fn = g_config.thread_current;
    if (fn == nullptr) return false; // never happens post-install (config_ok requires it)
    return reinterpret_cast<void *(*)()>(fn)() != nullptr;
}

// enabled() gate from quick-ball.js: armed, on the proven Unity thread, before the BOOTTIME
// lease deadline, not faulted/stopping.
bool gate_open() {
    if (g_fault.load(std::memory_order_relaxed) || g_stopping.load(std::memory_order_relaxed)) return false;
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    if (!on_proven_thread()) return false;
    if (!attached()) return false; // fail closed: never arm on an unattached thread
    uint64_t deadline = g_lease_deadline.load(std::memory_order_relaxed);
    if (deadline == 0) return false;
    uint64_t now = boottime_ms();
    return now != 0 && now <= deadline;
}

// --- original publication ----------------------------------------------------------
template <typename Fn>
Fn original_for(std::atomic<Fn> &slot) {
    Fn orig = slot.load(std::memory_order_acquire);
    if (!orig) {
        std::lock_guard<std::mutex> lock(g_install_mutex);
        orig = slot.load(std::memory_order_acquire);
    }
    if (!orig) std::abort(); // a live hook must have a published original; never fabricate
    return orig;
}

// --- outcome dedup + correlation (proven thread only) ------------------------------
void seen_free_index(int i) { gc_free(g_seen[i].handle); for (int j = i; j + 1 < g_seen_count; ++j) g_seen[j] = g_seen[j + 1]; --g_seen_count; }

// pruneSeen(): free expired handles; drain ALL on stop/fault/off. Proven thread only.
void prune_seen() {
    bool drain_all = g_stopping.load(std::memory_order_relaxed) ||
                     g_fault.load(std::memory_order_relaxed) ||
                     !g_enabled.load(std::memory_order_relaxed);
    uint64_t now = boottime_ms();
    if (now == 0) drain_all = true;
    for (int i = 0; i < g_seen_count;) {
        if (!drain_all && now < g_seen[i].expires) { ++i; continue; }
        seen_free_index(i);
    }
}

// outcome(): mirrors quick-ball.js outcome(). response is the RE-RESOLVED current pointer.
void outcome(void *response, int32_t status) {
    // Repeated poll of a response already reported.
    for (int i = 0; i < g_seen_count; ++i) if (gc_target(g_seen[i].handle) == response) return;

    uint64_t now = boottime_ms();
    if (now == 0) { latch_fault(); emit(PGR_OBS_A_OUTCOME_UNAVAILABLE, PGR_OBS_R_NONE, status); return; }
    if (now - g_oc_window >= 1000) { g_oc_window = now; g_oc_hits = 0; }
    if (++g_oc_hits > kOutcomesPerSec) { g_dropped.fetch_add(1, std::memory_order_relaxed); return; }

    if (g_seen_count >= kOutcomeSeen) seen_free_index(0); // free the oldest
    void *h = gc_new(response);
    if (h == nullptr) { g_dropped.fetch_add(1, std::memory_order_relaxed);
                        emit(PGR_OBS_A_OUTCOME_UNAVAILABLE, PGR_OBS_R_NONE, status); return; }
    g_seen[g_seen_count].handle = h;
    g_seen[g_seen_count].expires = now + kOutcomeMs;
    ++g_seen_count;

    if (status == g_config.status_success)      { g_confirmed.fetch_add(1, std::memory_order_relaxed);  emit(PGR_OBS_A_OUTCOME_CONFIRMED, PGR_OBS_R_CAUGHT, status); }
    else if (status == g_config.status_escape)  { g_confirmed.fetch_add(1, std::memory_order_relaxed);  emit(PGR_OBS_A_OUTCOME_CONFIRMED, PGR_OBS_R_BROKE_FREE, status); }
    else if (status == g_config.status_flee)    { g_confirmed.fetch_add(1, std::memory_order_relaxed);  emit(PGR_OBS_A_OUTCOME_CONFIRMED, PGR_OBS_R_FLED, status); }
    else {
        uint32_t reason = PGR_OBS_R_UNKNOWN;
        if (status == g_config.status_missed) reason = PGR_OBS_R_AMBIGUOUS;
        else if (status == g_config.status_error || status == g_config.status_item_replacement) reason = PGR_OBS_R_NOT_CONFIRMED;
        g_suppressed.fetch_add(1, std::memory_order_relaxed);
        emit(PGR_OBS_A_OUTCOME_SUPPRESSED, reason, status);
    }
}

// --- btxu replacement --------------------------------------------------------------
void *btxu_replacement(void *self, void *method) {
    void *caller = __builtin_return_address(0);
    g_last_caller.store(caller, std::memory_order_relaxed);
    BtxuFn orig = original_for(g_orig_btxu);
    pgr_interaction_owner::Phase observer(19);
    PgrObsBerryFilter filter=g_berry_filter.load(std::memory_order_acquire);
    if(filter&&(static_cast<uint64_t>(gettid())!=g_berry_owner||!attached()))filter=nullptr;
    // Preserve the single deferred receiver bound after a result-root failure.
    // Until the owner drains it, no new optional observer can acquire another.
    if(g_deferred_self.load(std::memory_order_acquire)){observer.api=nullptr;filter=nullptr;}

    void *expected = g_caller.load(std::memory_order_relaxed);
    bool caller_ok = (expected == nullptr) || (caller == expected);
    bool armed = gate_open() && caller_ok && exact(self, g_config.class_ball);

    // Acquire the reentry guard BEFORE the original so a nested btxu (or any reentrant
    // managed callback) cannot drive a second apply. Released at the very end.
    bool held_busy = false;
    if (armed||filter) {
        if (g_busy.exchange(1, std::memory_order_acquire)) {armed = false;filter=nullptr;} // already in flight
        else held_busy = true;
    }

    // Entry rate limit (hitsPerSecond). Exceeding latches a fault, as in quick-ball.js.
    if (armed) {
        uint64_t now = boottime_ms();
        if (now == 0) { latch_fault(); armed = false; }
        else {
            if (now - g_hit_window >= 1000) { g_hit_window = now; g_hits = 0; }
            if (++g_hits > kHitsPerSec) { latch_fault(); armed = false; }
        }
    }

    // Capture the revocation generation at arm time. A stop() on any thread bumps g_gen; it is
    // re-read immediately before the delegated speed mutation so a concurrent revoke aborts the
    // apply (stop/rearm safe) rather than mutating state the policy no longer authorizes.
    uint64_t gen0 = g_gen.load(std::memory_order_acquire);

    // Root `self` BEFORE the original: btxu may GC-move it; the leave path re-resolves it.
    void *self_h = nullptr;
    void *resp_h = nullptr;
    CatchFrame cleanup{self_h,resp_h,held_busy};
    pgr_interaction_owner::AbortBeforeRoots unwind{observer};
    if (armed || observer.wanted() || filter) {
        self_h = gc_new(self);
        if (self && self_h == nullptr) { latch_fault(); armed = false; }
    }

    if(!self||self_h)observer.enter(self_h?gc_target(self_h):self);
    if(filter&&self_h&&filter(gc_target(self_h),g_berry_owner)==1){observer.leave(nullptr);return nullptr;}
    void *response = orig(self_h ? gc_target(self_h) : self, method);

    // If self was rooted we are on a managed path and WILL make further managed frees below.
    // Root the response NOW so no safepoint inside those frees can move or reclaim the value we
    // return, on the armed path AND the armed-then-disarmed bail path alike (M1). When self was
    // never rooted (never armed) nothing managed runs before the return, so the raw result is
    // handed back untouched.
    void *ret = response; // re-resolved below; never a stale pre-op pointer
    if ((self_h != nullptr || observer.token) && response != nullptr) resp_h = gc_new(response);
    if(self_h && response && !resp_h){
        latch_fault();g_deferred_self=self_h;self_h=nullptr;armed=false;
    }
    // Never pass a borrowed managed result across the observer's result reader.
    if(!response||resp_h)observer.leave(resp_h?gc_target(resp_h):nullptr);
    else observer.abort();

    if (armed) do {
        if (!gate_open() || response == nullptr) break; // thread/lease moved, or pending (null) response
        if (resp_h == nullptr) {
            // A borrowed return must not cross a managed free without a root.
            // Fault disables further arms, so at most one deferred receiver exists.
            latch_fault();g_deferred_self=self_h;self_h=nullptr;break;
        }
        void *resp = gc_target(resp_h);
        ret = resp;
        if (!exact(resp, g_config.class_response)) { latch_fault(); break; }
        int32_t status = read_i32(resp, g_config.field_status);
        outcome(resp, status);

        if (status != g_config.status_success && status != g_config.status_escape) break;

        // Concurrent owned speed attempt -> skip (no re-apply).
        PgrSpeedSample ss{};
        if (speed()->sample(&ss) == 0 && ss.owned) { emit(PGR_OBS_A_SKIP, PGR_OBS_R_CONCURRENT, status); break; }

        void *ball = gc_target(self_h);
        uint8_t bread = read_u8(ball, g_config.field_bread);
        if (bread > 1) { latch_fault(); break; }
        if (bread == 1) { emit(PGR_OBS_A_SKIP, PGR_OBS_R_SPECIAL, status); break; }

        void *animator = read_ptr(ball, g_config.field_animator);
        if (!exact(animator, g_config.class_animator)) { latch_fault(); break; }

        // Consult the revocation generation + re-check the gate immediately before the delegated
        // mutation: a stop() since arm time (or a lost thread/lease) must suppress the apply.
        if (g_gen.load(std::memory_order_acquire) != gen0 || !gate_open()) {
            emit(PGR_OBS_A_SKIP, PGR_OBS_R_REVOKED, status); break;
        }

        // Micro-refresh feature8's lease (== JS s.nativeSpeed.update()), then delegate the apply.
        uint64_t now = boottime_ms();
        uint64_t deadline = g_lease_deadline.load(std::memory_order_relaxed);
        uint64_t remaining = (now != 0 && deadline > now) ? (deadline - now) : 0;
        if (remaining > kMaxLeaseMs) remaining = kMaxLeaseMs;
        if (remaining == 0) break;
        speed()->update(1, remaining, g_tid.load(std::memory_order_relaxed));
        if (g_gen.load(std::memory_order_acquire) != gen0 || !gate_open()) break;
        speed()->apply(animator, gc_target(resp_h));
        g_applied.fetch_add(1, std::memory_order_relaxed);
        emit(PGR_OBS_A_APPLIED, PGR_OBS_R_NONE, status);
    } while (false);

    // Free every OTHER managed handle FIRST: a safepoint inside those frees could relocate the
    // response, so the response root must still pin it. THEN re-resolve the response through its
    // live root and free it LAST, with ret captured immediately before that final free, so no
    // unrooted value ever crosses a managed free and the pointer returned to the game is current
    // (M1). The return root's own free is non-relocating (see header invariants).
    if (self_h != nullptr) {gc_free(self_h);self_h=nullptr;}
    if (resp_h != nullptr) { ret = gc_target(resp_h); gc_free(resp_h);resp_h=nullptr; }
    return ret;
}

// --- completion replacements -------------------------------------------------------
// reason: feature8's complete() treats 6 as broke-free, anything else as caught.
//
// Completion is a CLEANUP signal: the catch animation (and thus btdd/btdf) routinely fires
// AFTER the <=2.5 s observer lease has expired, so this must NOT gate on gate_open()/the lease
// (H1). It gates instead on the module being installed, il2cpp attachment, and the cleanup-owner
// thread (g_cleanup_tid, retained past lease expiry) -- exactly the binding feature8's own
// complete() uses for its identity compare. The response is pinned across the delegated managed
// cleanup and across the game's own completion body; the re-resolved argument is forwarded to the
// original EXACTLY ONCE and the roots are freed only after that last managed call (M2).
void completion_body(void *self, void *response, void *method, CompletionFn orig, int32_t reason) {
    if (!g_installed.load(std::memory_order_acquire)) { orig(self, response, method); return; }

    void *self_h = nullptr, *resp_h = nullptr;
    bool held_busy=false;
    pgr_interaction_owner::Phase observer(reason==5?10:11);
    CatchFrame cleanup{self_h,resp_h,held_busy};
    pgr_interaction_owner::AbortBeforeRoots unwind{observer};
    if (response != nullptr && on_cleanup_thread() && attached() &&
        !g_busy.exchange(1, std::memory_order_acquire)) {
        held_busy=true;
    }
    if(held_busy||observer.wanted()){
        // Pin both across the delegated complete() (a feature8 managed cleanup that may safepoint).
        if (self != nullptr) self_h = gc_new(self);
        if(response&&(!self || self_h))resp_h = gc_new(response);
        if (held_busy&&resp_h != nullptr && (!self || self_h)) {
            speed()->complete(gc_target(resp_h), reason); // feature8 does its own re-resolve + compare
            g_completed.fetch_add(1, std::memory_order_relaxed);
            emit(PGR_OBS_A_COMPLETED, reason == 6 ? PGR_OBS_R_BROKE_FREE : PGR_OBS_R_CAUGHT, 0);
        } else if((self&&!self_h)||(response&&!resp_h)) {
            latch_fault(); // root failure: fail closed, still forward the game's own body below
        }
    }

    // Forward to the game's own completion body EXACTLY ONCE, with pointers re-resolved through
    // the live roots (the delegated cleanup above may have moved them). When nothing was pinned
    // (not the cleanup thread / detached / null response) no managed call ran, so the raw args
    // are still current.
    if((!self||self_h)&&(!response||resp_h))observer.enter(self_h?gc_target(self_h):self,resp_h?gc_target(resp_h):response);
    void *fwd_self     = self_h != nullptr ? gc_target(self_h) : self;
    void *fwd_response = resp_h != nullptr ? gc_target(resp_h) : response;
    orig(fwd_self, fwd_response, method);
    observer.leave();

    // Free the non-return roots first, then the response root last (the forward above is the final
    // managed use; gchandle_free is non-relocating), keeping both pinned until all cleanup is done.
}
void caught_replacement(void *self, void *response, void *method) {
    completion_body(self, response, method, original_for(g_orig_caught), 5 /* caught */);
}
void escaped_replacement(void *self, void *response, void *method) {
    completion_body(self, response, method, original_for(g_orig_escaped), 6 /* broke-free */);
}

// --- config validation -------------------------------------------------------------
bool config_ok(const PgrObsConfig *c) {
    if(c->reserved||c->reserved2)return false;
    const void *const *ptrs = reinterpret_cast<const void *const *>(&c->object_get_class);
    for (int i = 0; i < 15; ++i) if (ptrs[i] == nullptr) return false;
    if (c->thread_current == nullptr) return false; // appended 16th pointer (attachment probe)
    const void *codes[] = { c->code_btxu, c->code_caught, c->code_escaped };
    for (int i = 0; i < 3; ++i) for (int j = i + 1; j < 3; ++j) if (codes[i] == codes[j]) return false;
    const void *ks[] = { c->class_ball, c->class_response, c->class_animator };
    for (int i = 0; i < 3; ++i) for (int j = i + 1; j < 3; ++j) if (ks[i] == ks[j]) return false;
    const void *fs[] = { c->field_bread, c->field_animator, c->field_status };
    for (int i = 0; i < 3; ++i) for (int j = i + 1; j < 3; ++j) if (fs[i] == fs[j]) return false;
    const int32_t lits[] = { c->status_success, c->status_escape, c->status_flee,
                             c->status_missed, c->status_error, c->status_item_replacement };
    for (int i = 0; i < 6; ++i) for (int j = i + 1; j < 6; ++j) if (lits[i] == lits[j]) return false;
    // feature8 speed table must be the v1 56-byte table.
    const PgrSpeedApi *sp = reinterpret_cast<const PgrSpeedApi *>(c->speed_api);
    if (sp->version != 1 || sp->size != sizeof(PgrSpeedApi) || sp->update == nullptr ||
        sp->apply == nullptr || sp->complete == nullptr || sp->sample == nullptr) return false;
    return true;
}

// --- C API table functions ---------------------------------------------------------
int api_install(const PgrObsConfig *config) {
    if (config == nullptr) return PGR_OBS_ERR_NULL;
    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (g_install_failed) return PGR_OBS_ERR_HOOK;
    if (config->version != 1 || config->size != sizeof(PgrObsConfig) || !config_ok(config))
        return PGR_OBS_ERR_BAD_CONFIG;
    if (g_installed.load(std::memory_order_acquire)) {
        return std::memcmp(&g_config, config, sizeof(g_config)) == 0 ? PGR_OBS_OK : PGR_OBS_ERR_HOOK;
    }
    g_config = *config; // publish before any callback can read it (still under lock)

    BtxuFn b = nullptr; CompletionFn c = nullptr, e = nullptr;
    int rc_b = pgrazor_hook(g_config.code_btxu,    reinterpret_cast<void *>(&btxu_replacement),    reinterpret_cast<void **>(&b));
    int rc_c = pgrazor_hook(g_config.code_caught,  reinterpret_cast<void *>(&caught_replacement),  reinterpret_cast<void **>(&c));
    int rc_e = pgrazor_hook(g_config.code_escaped, reinterpret_cast<void *>(&escaped_replacement), reinterpret_cast<void **>(&e));
    // A reported success with a NULL trampoline is the one state that MUST be undone: the
    // replacement is physically installed but has no original to chain to, so any later call
    // would run a null original. Unhooking ONLY that broken target removes our patch so the
    // game's own method runs; targets that hooked cleanly (valid trampoline) are never unhooked
    // (no unsafe unhook of a live sibling) -- they stay installed and simply pass through because
    // the policy can never arm once this install is marked failed.
    if (rc_b == 0 && b == nullptr) { pgrazor_unhook(g_config.code_btxu);    rc_b = PGR_OBS_ERR_HOOK; }
    if (rc_c == 0 && c == nullptr) { pgrazor_unhook(g_config.code_caught);  rc_c = PGR_OBS_ERR_HOOK; }
    if (rc_e == 0 && e == nullptr) { pgrazor_unhook(g_config.code_escaped); rc_e = PGR_OBS_ERR_HOOK; }

    if (rc_b != 0 || rc_c != 0 || rc_e != 0) {
        // Permanent inert partial patch: publish EVERY original we obtained so each still-live
        // chain is complete (a passthrough can always reach the game's method); policy can never
        // arm because g_installed stays false. Nothing already-published is dropped.
        g_orig_btxu.store(b, std::memory_order_release);
        g_orig_caught.store(c, std::memory_order_release);
        g_orig_escaped.store(e, std::memory_order_release);
        g_install_failed = true;
        return PGR_OBS_ERR_HOOK;
    }
    g_orig_btxu.store(b, std::memory_order_release);
    g_orig_caught.store(c, std::memory_order_release);
    g_orig_escaped.store(e, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    return PGR_OBS_OK;
}

int api_update(int enabled, uint64_t lease_ms, uint64_t unity_tid, void *expected_caller) {
    if (!g_installed.load(std::memory_order_acquire)) return PGR_OBS_ERR_NOT_INSTALLED;
    if (lease_ms > kMaxLeaseMs) { gate_close(); return PGR_OBS_ERR_LEASE; }
    if (!enabled) { gate_close(); g_stopping.store(1, std::memory_order_relaxed); return PGR_OBS_OK; }
    if (unity_tid == 0 || lease_ms == 0) { gate_close(); return PGR_OBS_ERR_BAD_ARG; }
    uint64_t now = boottime_ms();
    if (now == 0) { gate_close(); return PGR_OBS_ERR_LEASE; }
    g_caller.store(expected_caller, std::memory_order_relaxed);
    g_tid.store(unity_tid, std::memory_order_relaxed);
    g_cleanup_tid.store(unity_tid, std::memory_order_relaxed);
    g_lease_deadline.store(now + lease_ms, std::memory_order_relaxed);
    g_stopping.store(0, std::memory_order_relaxed);
    g_enabled.store(1, std::memory_order_relaxed);
    return PGR_OBS_OK;
}

int api_tick(uint64_t proven_tid) {
    if (!g_installed.load(std::memory_order_acquire)) return PGR_OBS_ERR_NOT_INSTALLED;
    if (proven_tid == 0 || static_cast<uint64_t>(gettid()) != proven_tid) return PGR_OBS_OK;
    if (proven_tid != g_cleanup_tid.load(std::memory_order_relaxed)) return PGR_OBS_OK;
    // Prune frees GC handles (a managed call); only run it on an il2cpp-attached thread. If the
    // thread is detached the bounded (<=4) seen ring is simply held until a later attached tick.
    if (!attached()) return PGR_OBS_OK;
    if (g_busy.exchange(1, std::memory_order_acquire)) return PGR_OBS_OK;
    if(void *h=g_deferred_self.exchange(nullptr,std::memory_order_acq_rel))gc_free(h);
    prune_seen();
    g_busy.store(0, std::memory_order_release);
    return PGR_OBS_OK;
}

int api_stop(uint64_t /*proven_tid*/) {
    if (!g_installed.load(std::memory_order_acquire)) return PGR_OBS_ERR_NOT_INSTALLED;
    // Immediate revoke from any thread. Frees nothing: the proven tick drains the seen ring.
    g_stopping.store(1, std::memory_order_relaxed);
    g_enabled.store(0, std::memory_order_relaxed);
    g_lease_deadline.store(0, std::memory_order_relaxed);
    g_gen.fetch_add(1, std::memory_order_relaxed);
    return PGR_OBS_OK;
}

int api_sample(PgrObsSample *out) {
    if (out == nullptr) return PGR_OBS_ERR_NULL;
    std::lock_guard<std::mutex> event_lock(g_event_mutex);
    out->version    = PGR_OBS_SAMPLE_VERSION;
    out->size       = sizeof(PgrObsSample);
    out->confirmed  = g_confirmed.load(std::memory_order_relaxed);
    out->suppressed = g_suppressed.load(std::memory_order_relaxed);
    out->dropped    = g_dropped.load(std::memory_order_relaxed);
    out->applied    = g_applied.load(std::memory_order_relaxed);
    out->completed  = g_completed.load(std::memory_order_relaxed);
    out->faults     = g_faults.load(std::memory_order_relaxed);
    out->last_tid   = g_last_tid.load(std::memory_order_relaxed);
    out->action     = g_action.load(std::memory_order_relaxed);
    out->reason     = g_reason.load(std::memory_order_relaxed);
    out->status     = g_status.load(std::memory_order_relaxed);
    out->seen_count = static_cast<uint32_t>(g_seen_count)+(g_deferred_self.load(std::memory_order_acquire)?1u:0u);
    out->fault      = static_cast<uint32_t>(g_fault.load(std::memory_order_relaxed));
    out->last_caller = reinterpret_cast<uint64_t>(g_last_caller.load(std::memory_order_relaxed));

    // Copy the bounded event ring. event_head is read (acquire) AFTER the slots so a slot the
    // reader then inspects for seq s <= head has already been published; each slot carries its
    // own seq so the JS adapter rejects a torn/overwritten slot (seq != expected) and detects a
    // gap (a seq skipped because the ring wrapped between polls -> 'outcome-dropped').
    for (uint32_t i = 0; i < PGR_OBS_EVENT_RING; ++i) {
        PgrObsEvent &dst = out->events[i];
        const EventSlot &src = g_events[i];
        uint64_t s1 = src.seq.load(std::memory_order_acquire);
        dst.tid    = src.tid.load(std::memory_order_relaxed);
        dst.action = src.action.load(std::memory_order_relaxed);
        dst.reason = src.reason.load(std::memory_order_relaxed);
        dst.status = src.status.load(std::memory_order_relaxed);
        dst.pad    = 0;
        uint64_t s2 = src.seq.load(std::memory_order_acquire);
        dst.seq = (s1 == s2) ? s1 : 0; // torn mid-write -> publish as empty, reader skips it
    }
    out->event_head = g_event_head.load(std::memory_order_acquire);
    out->serial     = static_cast<uint32_t>(out->event_head);
    return PGR_OBS_OK;
}

const PgrObsApi g_api = {
    1, sizeof(PgrObsApi),
    &api_install,
    &api_update,
    &api_tick,
    &api_stop,
    &api_sample,
};

int register_berry_filter(PgrObsBerryFilter filter,uint64_t tid){
    if(!g_installed.load(std::memory_order_acquire))return PGR_OBS_ERR_NOT_INSTALLED;
    if(!filter||!tid||tid!=static_cast<uint64_t>(gettid())||!attached())return PGR_OBS_ERR_BAD_ARG;
    std::lock_guard<std::mutex> lock(g_install_mutex);
    auto existing=g_berry_filter.load(std::memory_order_acquire);
    if(existing)return existing==filter&&g_berry_owner==tid?PGR_OBS_OK:PGR_OBS_ERR_BAD_CONFIG;
    uint64_t owner=g_cleanup_tid.load(std::memory_order_relaxed);
    if(owner&&owner!=tid)return PGR_OBS_ERR_BAD_ARG;
    g_cleanup_tid.store(tid,std::memory_order_relaxed);g_berry_owner=tid;
    g_berry_filter.store(filter,std::memory_order_release);return PGR_OBS_OK;
}
int berry_filter_ready(){return g_berry_filter.load(std::memory_order_acquire)?1:0;}
const PgrObsApi2 g_api2={2,sizeof(PgrObsApi2),register_berry_filter,berry_filter_ready};

} // namespace

const void *pgrazor_catch_observers_api(uint32_t version) {
    if(version==2)return &g_api2;
    if (version != 1) return nullptr;
    return &g_api;
}

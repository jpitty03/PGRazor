#include "catch_animation.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

// Built -fno-exceptions on the ARM64 target ABI and the host x86 test ABI. Managed
// invokes report failure through the runtime_invoke exception out-param and a bool
// out-param, never a C++ throw.

static_assert(sizeof(PgrSpeedConfig) == 104, "PgrSpeedConfig must be 104 bytes");
static_assert(sizeof(PgrSpeedSample) == 32,  "PgrSpeedSample must be 32 bytes");
static_assert(sizeof(PgrSpeedApi)    == 56,  "PgrSpeedApi must be 56 bytes");
static_assert(offsetof(PgrSpeedConfig, runtime_invoke)      == 8,  "layout: runtime_invoke");
static_assert(offsetof(PgrSpeedConfig, gchandle_new)        == 16, "layout: gchandle_new");
static_assert(offsetof(PgrSpeedConfig, gchandle_get_target) == 24, "layout: gchandle_get_target");
static_assert(offsetof(PgrSpeedConfig, gchandle_free)       == 32, "layout: gchandle_free");
static_assert(offsetof(PgrSpeedConfig, object_get_class)    == 40, "layout: object_get_class");
static_assert(offsetof(PgrSpeedConfig, object_unbox)        == 48, "layout: object_unbox");
static_assert(offsetof(PgrSpeedConfig, method_get_speed)    == 56, "layout: method_get_speed");
static_assert(offsetof(PgrSpeedConfig, method_set_speed)    == 64, "layout: method_set_speed");
static_assert(offsetof(PgrSpeedConfig, method_alive)        == 72, "layout: method_alive");
static_assert(offsetof(PgrSpeedConfig, class_single)        == 80, "layout: class_single");
static_assert(offsetof(PgrSpeedConfig, class_bool)          == 88, "layout: class_bool");
static_assert(offsetof(PgrSpeedConfig, class_animator)      == 96, "layout: class_animator");

namespace {

// Status codes (0 == success); aligned with the other native modules.
enum { OK = 0, ERR_NULL = 1, ERR_BAD_CONFIG = 2, ERR_LEASE = 4, ERR_NOT_INSTALLED = 6, ERR_BAD_ARG = 9 };

// Event (action) codes surfaced through sample().action.
enum { EV_NONE = 0, EV_APPLIED = 1, EV_RESTORED = 2, EV_EXTERNAL = 3, EV_DESTROYED = 4,
       EV_CLEANUP_FAILED = 5, EV_SKIP = 6 };

// Reason codes surfaced through sample().reason.
enum { R_NONE = 0, R_STOP = 1, R_OFF = 2, R_LEASE = 3, R_TIMEOUT = 4, R_CAUGHT = 5,
       R_BROKE_FREE = 6, R_CLOCK = 7, R_APPLY_READBACK = 8, R_SKIP_DESTROYED = 9,
       R_SKIP_NONSTANDARD = 10, R_SKIP_CONCURRENT = 11, R_ROOT = 12 };

constexpr uint64_t kMaxLeaseMs = 2500u; // LIMITS.leaseMs
constexpr uint64_t kTimeoutMs  = 8000u; // LIMITS.timeoutMs
constexpr float    kBallSpeed  = 20.0f; // BALL_SPEED
constexpr float    kTol        = 1e-5f; // float comparison tolerance from quick-ball.js

// --- lifetime / publication -------------------------------------------------------
std::mutex        g_install_mutex;      // serialises install() + config publication
PgrSpeedConfig    g_config{};           // validated copy, published under install mutex
std::atomic<bool> g_installed{false};

// --- policy window (armed by update) ----------------------------------------------
std::atomic<int>      g_allowed{0};
std::atomic<uint64_t> g_lease_deadline{0}; // CLOCK_BOOTTIME ms; 0 == closed
std::atomic<uint64_t> g_tid{0};            // proven Unity thread id
std::atomic<uint64_t> g_cleanup_tid{0};    // keep ownership thread after policy closes
std::atomic<int>      g_stopping{0};       // hard stop / fault latched -> restore path
std::atomic<int>      g_fault{0};          // latched restore/apply/root fault

// --- telemetry (pull-only) --------------------------------------------------------
std::atomic<uint32_t> g_owned{0};
std::atomic<uint32_t> g_action{EV_NONE};
std::atomic<uint32_t> g_reason{R_NONE};
std::atomic<uint32_t> g_serial{0};
std::atomic<uint32_t> g_original_bits{0};
std::atomic<uint32_t> g_readback_bits{0};

// --- game-thread-only state -------------------------------------------------------
// The single owned attempt. Touched ONLY on the proven Unity thread (apply/complete/
// tick all gate on gettid()==g_tid), so it needs no lock.
struct Attempt {
    void    *animator = nullptr; // pointer-sized GC handle
    void    *response = nullptr; // pointer-sized GC handle
    float    original = 0.0f;    // speed snapshot captured before the setter
    uint64_t deadline = 0;       // BOOTTIME ms: now + kTimeoutMs
    uint64_t retry_at = 0;       // BOOTTIME ms before which a soft restore is skipped
    bool     reported = false;   // cleanup-failed emitted once
};
Attempt  g_attempt_storage;
Attempt *g_attempt = nullptr;
// Reentry guard: a runtime_invoke (set_speed) could synchronously re-enter. Not a
// mutex (never held across the managed call); a flag.
std::atomic<int> g_busy{0};

uint32_t bits_of(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
float    float_of(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

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

void emit(uint32_t action, uint32_t reason, float original, float readback) {
    g_action.store(action, std::memory_order_relaxed);
    g_reason.store(reason, std::memory_order_relaxed);
    g_original_bits.store(bits_of(original), std::memory_order_relaxed);
    g_readback_bits.store(bits_of(readback), std::memory_order_relaxed);
    g_serial.fetch_add(1, std::memory_order_release); // publish last
}

// Clear the policy window (off / stop). Does NOT free the attempt: the restore must
// run on the proven thread; the next tick observes the closed window and restores.
void gate_close() {
    g_allowed.store(0, std::memory_order_relaxed);
    g_lease_deadline.store(0, std::memory_order_relaxed);
    g_tid.store(0, std::memory_order_relaxed);
}

// Latch a fault: clear the policy and set stopping, but preserve the attempt so the
// restore keeps retrying until it succeeds.
void latch_fault() {
    g_fault.store(1, std::memory_order_relaxed);
    g_stopping.store(1, std::memory_order_relaxed);
    gate_close();
}

// --- il2cpp helpers over validated config pointers --------------------------------
inline void *obj_class(void *o) {
    return reinterpret_cast<void *(*)(void *)>(g_config.object_get_class)(o);
}
inline void *unbox(void *o) {
    return reinterpret_cast<void *(*)(void *)>(g_config.object_unbox)(o);
}
inline void *gc_new(void *obj)  { return reinterpret_cast<void *(*)(void *, int)>(g_config.gchandle_new)(obj, 0); }
inline void *gc_target(void *h) { return reinterpret_cast<void *(*)(void *)>(g_config.gchandle_get_target)(h); }
inline void  gc_free(void *h)   { reinterpret_cast<void (*)(void *)>(g_config.gchandle_free)(h); }
inline bool  exact(void *o, void *k) { return o != nullptr && obj_class(o) == k; }

// runtime_invoke with the exception out-param. ok=false on a managed exception.
void *invoke(void *method, void *obj, void **params, bool *ok) {
    void *exc = nullptr;
    void *r = reinterpret_cast<void *(*)(void *, void *, void **, void **)>(g_config.runtime_invoke)(
        method, obj, params, &exc);
    if (exc != nullptr) { *ok = false; return nullptr; }
    return r;
}

// Animator.get_speed -> boxed System.Single, unboxed exactly. Mirrors speed() in JS.
bool read_speed(void *animator_obj, float *out) {
    if (!exact(animator_obj, g_config.class_animator)) return false;
    bool ok = true;
    void *boxed = invoke(g_config.method_get_speed, animator_obj, nullptr, &ok);
    if (!ok || !boxed) return false;
    void *h = gc_new(boxed);
    if (h == nullptr) return false;
    bool good = false;
    void *o = gc_target(h);
    if (exact(o, g_config.class_single)) {
        void *v = unbox(o);
        if (v != nullptr) { std::memcpy(out, v, 4); good = true; }
    }
    gc_free(h);
    return good;
}

// Animator.set_speed(float). The param array holds a pointer to the raw float.
bool write_speed(void *animator_obj, float value) {
    if (!exact(animator_obj, g_config.class_animator)) return false;
    float f = value;
    void *params[1] = { &f };
    bool ok = true;
    invoke(g_config.method_set_speed, animator_obj, params, &ok);
    return ok;
}

// Object.op_Implicit(Object) -> boxed System.Boolean (static); arg[0] is the object.
bool read_alive(void *animator_obj, bool *alive_out) {
    if (!exact(animator_obj, g_config.class_animator)) return false;
    void *params[1] = { animator_obj };
    bool ok = true;
    void *boxed = invoke(g_config.method_alive, nullptr, params, &ok);
    if (!ok || !boxed) return false;
    void *h = gc_new(boxed);
    if (h == nullptr) return false;
    bool good = false;
    void *o = gc_target(h);
    if (exact(o, g_config.class_bool)) {
        void *v = unbox(o);
        if (v != nullptr) {
            uint8_t b = *reinterpret_cast<uint8_t *>(v);
            if (b <= 1) { *alive_out = (b == 1); good = true; }
        }
    }
    gc_free(h);
    return good;
}

inline bool near_eq(float a, float b) { return std::isfinite(a) && std::fabs(a - b) <= kTol; }

bool on_proven_thread() {
    uint64_t tid = g_tid.load(std::memory_order_relaxed);
    return tid != 0 && static_cast<uint64_t>(gettid()) == tid;
}

// enabled() gate from quick-ball.js: armed, on the proven Unity thread, before the
// BOOTTIME lease deadline, not faulted/stopping.
bool gate_open() {
    if (g_fault.load(std::memory_order_relaxed) || g_stopping.load(std::memory_order_relaxed)) return false;
    if (!g_allowed.load(std::memory_order_relaxed)) return false;
    if (!on_proven_thread()) return false;
    uint64_t deadline = g_lease_deadline.load(std::memory_order_relaxed);
    if (deadline == 0) return false;
    uint64_t now = boottime_ms();
    return now != 0 && now <= deadline;
}

void release_attempt() {
    if (g_attempt == nullptr) return;
    if (g_attempt->response) gc_free(g_attempt->response);
    if (g_attempt->animator) gc_free(g_attempt->animator);
    *g_attempt = Attempt{};
    g_attempt = nullptr;
    g_owned.store(0, std::memory_order_release);
}

// restore(): the Unity-thread cleanup from quick-ball.js. force bypasses the 1 s retry
// gate (used for completion and clock failure). Proven-thread only.
void restore(uint32_t reason, bool force) {
    Attempt *a = g_attempt;
    if (a == nullptr) return;
    uint64_t now = boottime_ms();
    if (!force) {
        if (now != 0 && now < a->retry_at) return;
    }

    bool alive = false;
    if (!read_alive(gc_target(a->animator), &alive)) goto fail;
    if (!alive) { emit(EV_DESTROYED, reason, a->original, 0.0f); release_attempt(); return; }

    float current;
    if (!read_speed(gc_target(a->animator), &current)) goto fail;
    if (!std::isfinite(current)) goto fail;
    // An external change away from our 20x releases ownership WITHOUT overwriting.
    if (!near_eq(current, kBallSpeed)) {
        emit(EV_EXTERNAL, reason, a->original, current);
        release_attempt();
        return;
    }
    if (!write_speed(gc_target(a->animator), a->original)) goto fail;
    float readback;
    if (!read_speed(gc_target(a->animator), &readback)) goto fail;
    if (!near_eq(readback, a->original)) goto fail;

    emit(EV_RESTORED, reason, a->original, readback);
    release_attempt();
    return;

fail:
    // Latch the fault, keep the attempt, retry in 1 s. Report cleanup-failed once.
    a->retry_at = (now == 0) ? 0 : now + 1000;
    latch_fault();
    if (!a->reported) { a->reported = true; emit(EV_CLEANUP_FAILED, reason, a->original, 0.0f); }
}

// --- config validation ------------------------------------------------------------
bool config_ok(const PgrSpeedConfig *c) {
    const void *const *ptrs = reinterpret_cast<const void *const *>(&c->runtime_invoke);
    for (int i = 0; i < 12; ++i) if (ptrs[i] == nullptr) return false;
    // Three distinct methods and three distinct classes.
    const void *ms[] = { c->method_get_speed, c->method_set_speed, c->method_alive };
    for (int i = 0; i < 3; ++i) for (int j = i + 1; j < 3; ++j) if (ms[i] == ms[j]) return false;
    const void *ks[] = { c->class_single, c->class_bool, c->class_animator };
    for (int i = 0; i < 3; ++i) for (int j = i + 1; j < 3; ++j) if (ks[i] == ks[j]) return false;
    return true;
}

// --- C API table functions --------------------------------------------------------
int api_install(const PgrSpeedConfig *config) {
    if (config == nullptr) return ERR_NULL;
    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (config->version != 1 || config->size != sizeof(PgrSpeedConfig) || !config_ok(config))
        return ERR_BAD_CONFIG; // invalid config never writes
    if (g_installed.load(std::memory_order_acquire)) {
        // Process-lifetime config: an identical repeat is a no-op success; a different
        // one is refused (no second table is ever adopted).
        return std::memcmp(&g_config, config, sizeof(g_config)) == 0 ? OK : ERR_BAD_CONFIG;
    }
    g_config = *config;
    g_installed.store(true, std::memory_order_release);
    return OK;
}

int api_update(int allowed, uint64_t lease_ms, uint64_t unity_tid) {
    if (!g_installed.load(std::memory_order_acquire)) return ERR_NOT_INSTALLED;
    if (lease_ms > kMaxLeaseMs) { gate_close(); return ERR_LEASE; } // out-of-policy lease
    if (!allowed) { gate_close(); return OK; } // off: window cleared; tick restores
    if (unity_tid == 0 || lease_ms == 0) { gate_close(); return ERR_BAD_ARG; }
    if (g_owned.load(std::memory_order_acquire) && unity_tid != g_cleanup_tid.load()) {
        gate_close(); return ERR_BAD_ARG;
    }
    uint64_t now = boottime_ms();
    if (now == 0) { gate_close(); return ERR_LEASE; }
    g_tid.store(unity_tid, std::memory_order_relaxed);
    g_lease_deadline.store(now + lease_ms, std::memory_order_relaxed);
    g_allowed.store(1, std::memory_order_relaxed);
    return OK;
}

// apply(): called from the JS btxu-leave observer on a confirmed success. The observer
// has already proved the ball/caller/thread/bread/animator class; here we own the
// snapshot + 20x write + readback.
int api_apply(void *animator, void *response) {
    if (!g_installed.load(std::memory_order_acquire)) return ERR_NOT_INSTALLED;
    if (animator == nullptr || response == nullptr) return ERR_NULL;
    if (g_busy.exchange(1, std::memory_order_acquire)) return OK; // reentry guard
    int rc = OK;
    if (!gate_open()) goto done;
    if (g_attempt != nullptr) { emit(EV_SKIP, R_SKIP_CONCURRENT, 0.0f, 0.0f); goto done; } // single attempt
    if (!exact(animator, g_config.class_animator)) goto done;

    {
        // Keep both arguments alive across every managed getter, not only the
        // setter. A getter may allocate or synchronously enter other game code.
        struct Roots {
            void *animator = nullptr;
            void *response = nullptr;
            ~Roots() { if (response) gc_free(response); if (animator) gc_free(animator); }
        } roots;
        roots.animator = gc_new(animator);
        if (roots.animator) roots.response = gc_new(response);
        if (!roots.animator || !roots.response) {
            latch_fault(); emit(EV_CLEANUP_FAILED, R_ROOT, 0.0f, 0.0f); goto done;
        }
        bool alive = false;
        if (!read_alive(gc_target(roots.animator), &alive)) { latch_fault(); goto done; }
        if (!alive) { emit(EV_SKIP, R_SKIP_DESTROYED, 0.0f, 0.0f); goto done; }

        float original;
        if (!read_speed(gc_target(roots.animator), &original)) { latch_fault(); goto done; }
        if (!near_eq(original, 1.0f)) { emit(EV_SKIP, R_SKIP_NONSTANDARD, original, 0.0f); goto done; }

        uint64_t now = boottime_ms();
        if (now == 0) { latch_fault(); goto done; }

        // Ownership is recorded BEFORE the setter: even a throwing set_speed may have
        // mutated native state, so cleanup must be able to inspect and restore it.
        g_attempt_storage = Attempt{ roots.animator, roots.response, original, now + kTimeoutMs, 0, false };
        roots.animator = roots.response = nullptr;
        g_attempt = &g_attempt_storage;
        // A later policy update must not move an owned animator's cleanup to a
        // different thread. Bind cleanup to the thread that actually acquired it.
        g_cleanup_tid.store(static_cast<uint64_t>(gettid()), std::memory_order_release);
        g_owned.store(1, std::memory_order_release);

        if (!write_speed(gc_target(g_attempt->animator), kBallSpeed)) { latch_fault(); goto done; } // attempt kept for restore
        float readback;
        if (!read_speed(gc_target(g_attempt->animator), &readback) || !near_eq(readback, kBallSpeed)) { latch_fault(); goto done; }
        emit(EV_APPLIED, R_NONE, original, readback);
    }

done:
    g_busy.store(0, std::memory_order_release);
    return rc;
}

// complete(): called from the JS completion observers (caught / broke-free). Force a
// restore when the rooted response matches.
int api_complete(void *response, int32_t reason) {
    if (!g_installed.load(std::memory_order_acquire)) return ERR_NOT_INSTALLED;
    if (response == nullptr) return ERR_NULL;
    if (g_busy.exchange(1, std::memory_order_acquire)) return OK;
    if (g_cleanup_tid.load()==static_cast<uint64_t>(gettid()) && g_attempt != nullptr && gc_target(g_attempt->response) == response) {
        uint32_t r = (reason == R_BROKE_FREE) ? R_BROKE_FREE : R_CAUGHT;
        restore(r, true);
    }
    g_busy.store(0, std::memory_order_release);
    return OK;
}

// tick(): the Unity-thread cleanup pass. Only acts on the proven thread.
int api_tick(uint64_t proven_tid) {
    if (!g_installed.load(std::memory_order_acquire)) return ERR_NOT_INSTALLED;
    if (proven_tid == 0 || static_cast<uint64_t>(gettid()) != proven_tid) return OK;
    if (proven_tid != g_cleanup_tid.load()) return OK;
    if (g_busy.exchange(1, std::memory_order_acquire)) return OK;
    if (g_attempt != nullptr) {
        if (g_fault.load(std::memory_order_relaxed) || g_stopping.load(std::memory_order_relaxed)) {
            restore(R_STOP, false);
        } else if (!g_allowed.load(std::memory_order_relaxed)) {
            restore(R_OFF, false);
        } else {
            uint64_t now = boottime_ms();
            if (now == 0) restore(R_CLOCK, true);
            else if (now >= g_lease_deadline.load(std::memory_order_relaxed)) restore(R_LEASE, false);
            else if (now >= g_attempt->deadline) restore(R_TIMEOUT, false);
        }
    }
    g_busy.store(0, std::memory_order_release);
    return OK;
}

int api_sample(PgrSpeedSample *out) {
    if (out == nullptr) return ERR_NULL;
    out->serial   = g_serial.load(std::memory_order_acquire);
    out->owned    = g_owned.load(std::memory_order_relaxed);
    out->fault    = static_cast<uint32_t>(g_fault.load(std::memory_order_relaxed));
    out->action   = g_action.load(std::memory_order_relaxed);
    out->reason   = g_reason.load(std::memory_order_relaxed);
    out->pad      = 0;
    out->original = float_of(g_original_bits.load(std::memory_order_relaxed));
    out->readback = float_of(g_readback_bits.load(std::memory_order_relaxed));
    return OK;
}

const PgrSpeedApi g_api = {
    1, sizeof(PgrSpeedApi),
    &api_install,
    &api_update,
    &api_apply,
    &api_complete,
    &api_tick,
    &api_sample,
};

} // namespace

const void *pgrazor_catch_animation_api(uint32_t version) {
    if (version != 1) return nullptr;
    return &g_api;
}

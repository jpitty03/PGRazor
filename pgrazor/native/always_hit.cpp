#include "always_hit.h"
#include "hook_engine.h"
#include "dobby.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

// Built -fno-exceptions on both the ARM64 target ABI and the host x86 test ABI. The
// register-context field read by the launch/release instruments (general.regs.x0 /
// general.regs.rdi) is present in the pinned Dobby register-context header for both
// ABIs; any other ABI fails loudly here rather than reading the wrong register.
#if defined(__aarch64__)
#define PGRAZOR_ALWAYSHIT_CTX_SELF(ctx) \
    (reinterpret_cast<void *>((ctx)->general.regs.x0))
#elif defined(__x86_64__)
#define PGRAZOR_ALWAYSHIT_CTX_SELF(ctx) \
    (reinterpret_cast<void *>((ctx)->general.regs.rdi))
#else
#error "Always Hit native instrument supports arm64 and x86_64 register contexts only."
#endif

static_assert(sizeof(PgrAlwaysHitValue)  == 12,  "PokeballThrow by-value must be 12 bytes");
static_assert(sizeof(PgrAlwaysHitConfig) == 192, "PgrAlwaysHitConfig must be 192 bytes");
static_assert(sizeof(PgrAlwaysHitSample) == 24,  "PgrAlwaysHitSample must be 24 bytes");
static_assert(sizeof(PgrAlwaysHitApi)    == 40,  "PgrAlwaysHitApi must be 40 bytes");
static_assert(offsetof(PgrAlwaysHitConfig, code_launch)        == 8,   "layout: code_launch");
static_assert(offsetof(PgrAlwaysHitConfig, code_release)       == 16,  "layout: code_release");
static_assert(offsetof(PgrAlwaysHitConfig, code_collision)     == 24,  "layout: code_collision");
static_assert(offsetof(PgrAlwaysHitConfig, code_trigger)       == 32,  "layout: code_trigger");
static_assert(offsetof(PgrAlwaysHitConfig, code_make_throw)    == 40,  "layout: code_make_throw");
static_assert(offsetof(PgrAlwaysHitConfig, ball_class)         == 48,  "layout: ball_class");
static_assert(offsetof(PgrAlwaysHitConfig, machine_class)      == 56,  "layout: machine_class");
static_assert(offsetof(PgrAlwaysHitConfig, action_class)       == 64,  "layout: action_class");
static_assert(offsetof(PgrAlwaysHitConfig, object_get_class)   == 72,  "layout: object_get_class");
static_assert(offsetof(PgrAlwaysHitConfig, field_get_value)    == 80,  "layout: field_get_value");
static_assert(offsetof(PgrAlwaysHitConfig, runtime_invoke)     == 88,  "layout: runtime_invoke");
static_assert(offsetof(PgrAlwaysHitConfig, invoke_method)      == 96,  "layout: invoke_method");
static_assert(offsetof(PgrAlwaysHitConfig, gchandle_new)       == 104, "layout: gchandle_new");
static_assert(offsetof(PgrAlwaysHitConfig, gchandle_get_target)== 112, "layout: gchandle_get_target");
static_assert(offsetof(PgrAlwaysHitConfig, gchandle_free)      == 120, "layout: gchandle_free");
static_assert(offsetof(PgrAlwaysHitConfig, field_bread)        == 128, "layout: field_bread");
static_assert(offsetof(PgrAlwaysHitConfig, field_item)         == 136, "layout: field_item");
static_assert(offsetof(PgrAlwaysHitConfig, field_state)        == 144, "layout: field_state");
static_assert(offsetof(PgrAlwaysHitConfig, field_current)      == 152, "layout: field_current");
static_assert(offsetof(PgrAlwaysHitConfig, field_flying)       == 160, "layout: field_flying");
static_assert(offsetof(PgrAlwaysHitConfig, field_falling)      == 168, "layout: field_falling");
static_assert(offsetof(PgrAlwaysHitConfig, field_hit)          == 176, "layout: field_hit");
static_assert(offsetof(PgrAlwaysHitConfig, field_pokemon)      == 184, "layout: field_pokemon");

namespace {

// Status codes (0 == success); aligned with the other native modules' conventions.
enum {
    OK = 0, ERR_NULL = 1, ERR_BAD_CONFIG = 2, ERR_HOOK = 3, ERR_LEASE = 4,
    ERR_NOT_INSTALLED = 6, ERR_BAD_ARG = 9
};

// Lease cap: matches s.throwUntil derivation / LIMITS.leaseMs (2500 ms) in quick-ball.js.
constexpr uint64_t kMaxLeaseMs = 2500u;

// Verified game ABIs. The collision/trigger replacements forward every argument verbatim;
// makeThrow returns the 12-byte PokeballThrow by value (see the header's ABI note on the
// System.Boolean parameter the original task brief omitted).
typedef void (*CollisionFn)(void *self, void *arg, void *method);
typedef PgrAlwaysHitValue (*MakeThrowFn)(void *self, uint8_t missed_arg, void *method);

// --- lifetime / publication -------------------------------------------------------
std::mutex        g_install_mutex;        // serialises install() + original publication
PgrAlwaysHitConfig g_config{};            // validated copy, published under install mutex
std::atomic<bool> g_installed{false};     // true only after a fully successful install
bool              g_install_failed = false; // sticky; protected by install mutex

std::atomic<CollisionFn> g_orig_collision{nullptr};
std::atomic<CollisionFn> g_orig_trigger{nullptr};
std::atomic<MakeThrowFn> g_orig_make_throw{nullptr};

// --- policy window (armed by update) ----------------------------------------------
std::atomic<int>      g_enabled{0};
std::atomic<int>      g_allowed{0};
std::atomic<uint64_t> g_lease_deadline{0}; // CLOCK_BOOTTIME ms; 0 == closed
std::atomic<uint64_t> g_tid{0};            // proven Unity thread id
std::atomic<uint64_t> g_cleanup_tid{0};    // retained after policy closes, for root cleanup
std::atomic<int>      g_fault{0};          // latched after a managed-exception / root failure

// --- telemetry --------------------------------------------------------------------
std::atomic<uint64_t> g_applied{0};
std::atomic<uint64_t> g_normalized{0};
std::atomic<uint64_t> g_errors{0};

// --- game-thread-only state -------------------------------------------------------
// The single handled ball. Touched ONLY on the Unity thread (launch/release instruments
// and the collision/trigger replacements all gate on the proven tid), so it needs no
// lock. It is a GC root (not a raw pointer) so the identity stays valid for the dedupe
// across the launch->collision window even if the collector runs. g_clear_handled lets a
// stop (update) request the free without touching managed memory off the game thread.
void                 *g_handled = nullptr; // pointer-sized GC handle, or nullptr
std::atomic<int>      g_clear_handled{0};
// Reentrancy guard: our runtime_invoke of HitPokemon can synchronously re-enter the
// collision/trigger hook. Not a mutex (never held across the managed call); a flag.
std::atomic<int>      g_busy{0};

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

void gate_close() {
    g_enabled.store(0, std::memory_order_relaxed);
    g_allowed.store(0, std::memory_order_relaxed);
    g_lease_deadline.store(0, std::memory_order_relaxed);
    g_tid.store(0, std::memory_order_relaxed);
    g_clear_handled.store(1, std::memory_order_relaxed); // free the root on the game thread
}

// The enabled() gate from quick-ball.js: armed, permitted, on the proven Unity thread,
// before the BOOTTIME lease deadline, and not faulted.
bool gate_open() {
    if (g_fault.load(std::memory_order_relaxed)) return false;
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    if (!g_allowed.load(std::memory_order_relaxed)) return false;
    uint64_t tid = g_tid.load(std::memory_order_relaxed);
    if (tid == 0 || static_cast<uint64_t>(gettid()) != tid) return false;
    uint64_t deadline = g_lease_deadline.load(std::memory_order_relaxed);
    if (deadline == 0) return false;
    uint64_t now = boottime_ms();
    return now != 0 && now <= deadline;
}

// --- il2cpp helpers over validated config pointers --------------------------------
inline void *obj_class(void *o) {
    return reinterpret_cast<void *(*)(void *)>(g_config.object_get_class)(o);
}
inline void field_get(void *o, void *f, void *out) {
    reinterpret_cast<void (*)(void *, void *, void *)>(g_config.field_get_value)(o, f, out);
}
inline uint8_t read_bool(void *o, void *f) { uint8_t v = 1;  field_get(o, f, &v); return v; }
inline int32_t read_int(void *o, void *f)  { int32_t v = 0;  field_get(o, f, &v); return v; }
inline void   *read_ptr(void *o, void *f)  { void *v = nullptr; field_get(o, f, &v); return v; }
inline void *gc_new(void *obj)    { return reinterpret_cast<void *(*)(void *, int)>(g_config.gchandle_new)(obj, 0); }
inline void *gc_target(void *h)   { return reinterpret_cast<void *(*)(void *)>(g_config.gchandle_get_target)(h); }
inline void  gc_free(void *h)     { reinterpret_cast<void (*)(void *)>(g_config.gchandle_free)(h); }

// exact(o,k): nonnull AND object's class is exactly k (object_get_class().equals(k)).
inline bool exact(void *o, void *k) { return o != nullptr && obj_class(o) == k; }

// valid(ball): exact ball class, bread guard clear, eligible item. Mirrors the JS valid().
bool valid_ball(void *ball) {
    if (!exact(ball, g_config.ball_class)) return false;
    if (read_bool(ball, g_config.field_bread) != 0) return false;
    int32_t item = read_int(ball, g_config.field_item);
    return item == 1 || item == 2 || item == 3 || item == 5;
}

// Free the handled root when a stop requested it. Game-thread only.
void maybe_clear_handled() {
    if(g_cleanup_tid.load()!=static_cast<uint64_t>(gettid()) || g_busy.load())return;
    if(!gate_open())g_clear_handled.store(1);
    if (g_clear_handled.exchange(0, std::memory_order_relaxed)) {
        if (g_handled) { gc_free(g_handled); g_handled = nullptr; }
    }
}

// --- original publication ---------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Shared collision/trigger replacement body. One handled hit per launch/release; the
// original site is suppressed once we invoke the ball's own HitPokemon (and for every
// later event on the same handled ball), preserving the old suppression semantics.
// ---------------------------------------------------------------------------
void collision_body(void *ball, void *arg, void *method, CollisionFn orig) {
    maybe_clear_handled();
    if (!gate_open() || g_busy.load(std::memory_order_relaxed)) { orig(ball, arg, method); return; }
    if (!valid_ball(ball)) { orig(ball, arg, method); return; }

    void *machine = read_ptr(ball, g_config.field_state);
    if (!exact(machine, g_config.machine_class)) { orig(ball, arg, method); return; }

    void *current = read_ptr(machine, g_config.field_current);
    void *flying  = read_ptr(ball, g_config.field_flying);
    void *falling = read_ptr(ball, g_config.field_falling);
    if (current == nullptr || (current != flying && current != falling)) { orig(ball, arg, method); return; }

    // Already handled this ball: suppress the original (no re-apply), as in JS.
    if (g_handled != nullptr && gc_target(g_handled) == ball) return;

    void *action  = read_ptr(ball, g_config.field_hit);
    void *pokemon = read_ptr(ball, g_config.field_pokemon);
    if (!exact(action, g_config.action_class) || pokemon == nullptr) { orig(ball, arg, method); return; }

    // Qualified. Root the ball as the handled identity BEFORE invoking (the JS set
    // `handled` before invoke). Rooting is the native equivalent of the JS pointer string;
    // a root failure is a latched fault that chains the original rather than risk a double
    // apply with no dedupe, and never fabricates a root.
    void *handle = gc_new(ball);
    if (handle == nullptr) {
        g_fault.store(1, std::memory_order_relaxed);
        g_errors.fetch_add(1, std::memory_order_relaxed);
        orig(ball, arg, method);
        return;
    }
    if (g_handled) gc_free(g_handled);
    g_handled = handle;

    // Invoke the game's own wired HitPokemon action via runtime_invoke, handling any
    // managed exception through the out-parameter. No lock is held across this call; the
    // busy flag blocks reentrancy. Either outcome suppresses the original collision.
    g_busy.store(1, std::memory_order_relaxed);
    void *exc = nullptr;
    reinterpret_cast<void *(*)(void *, void *, void **, void **)>(g_config.runtime_invoke)(
        g_config.invoke_method, action, nullptr, &exc);
    g_busy.store(0, std::memory_order_relaxed);

    if (exc != nullptr) {
        g_fault.store(1, std::memory_order_relaxed);   // latch; gate closes on next read
        g_errors.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_applied.fetch_add(1, std::memory_order_relaxed);
    }
    // Suppress the original site either way (preserves old suppression semantics).
}

void collision_replacement(void *self, void *arg, void *method) {
    collision_body(self, arg, method, original_for(g_orig_collision));
}
void trigger_replacement(void *self, void *arg, void *method) {
    collision_body(self, arg, method, original_for(g_orig_trigger));
}

// ---------------------------------------------------------------------------
// btyb (makeThrow): forward to the original, then normalise a spuriously-set "missed"
// byte (1 -> 0) only when permitted. Permission is the enabled()+valid() pair evaluated
// BEFORE the call, matching the JS onEnter force snapshot. Radius/item/pad are untouched.
// ---------------------------------------------------------------------------
PgrAlwaysHitValue make_throw_replacement(void *self, uint8_t missed_arg, void *method) {
    maybe_clear_handled();
    MakeThrowFn orig = original_for(g_orig_make_throw);
    const bool permit = gate_open() && valid_ball(self);
    PgrAlwaysHitValue out = orig(self, missed_arg, method);
    if (permit && out.missed == 1) {
        out.missed = 0;
        g_normalized.fetch_add(1, std::memory_order_relaxed);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Launch/EndDrag entry instruments: read the `this` ball only and, when enabled+valid,
// reset the handled root so the coming throw gets exactly one applied hit. They never
// alter the call and read no undetermined argument register.
// ---------------------------------------------------------------------------
void reset_handled(void *ball) {
    if (!gate_open() || ball == nullptr || !valid_ball(ball)) return;
    if (g_handled) { gc_free(g_handled); g_handled = nullptr; } // == JS `handled=null`
}
void instrument_launch(void *, DobbyRegisterContext *ctx) {
    maybe_clear_handled();
    reset_handled(PGRAZOR_ALWAYSHIT_CTX_SELF(ctx));
}
void instrument_release(void *, DobbyRegisterContext *ctx) {
    maybe_clear_handled();
    reset_handled(PGRAZOR_ALWAYSHIT_CTX_SELF(ctx));
}

// ---------------------------------------------------------------------------
// Config validation: every pointer present, headers exact, code targets and class
// identities distinct pre-patch.
// ---------------------------------------------------------------------------
bool config_ok(const PgrAlwaysHitConfig *c) {
    const void *const *ptrs = reinterpret_cast<const void *const *>(&c->code_launch);
    for (int i = 0; i < 23; ++i) if (ptrs[i] == nullptr) return false;

    const void *codes[] = {c->code_launch, c->code_release, c->code_collision,
                           c->code_trigger, c->code_make_throw};
    for (int i = 0; i < 5; ++i)
        for (int j = i + 1; j < 5; ++j)
            if (codes[i] == codes[j]) return false; // five distinct patch sites

    if (c->ball_class == c->machine_class || c->ball_class == c->action_class ||
        c->machine_class == c->action_class) return false; // distinct class identities
    return true;
}

// ---------------------------------------------------------------------------
// C API table functions.
// ---------------------------------------------------------------------------
int api_install(const PgrAlwaysHitConfig *config) {
    if (config == nullptr) return ERR_NULL;

    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (g_install_failed) return ERR_HOOK;

    if (config->version != 1 || config->size != sizeof(PgrAlwaysHitConfig) || !config_ok(config))
        return ERR_BAD_CONFIG;

    if (g_installed.load(std::memory_order_acquire)) {
        // Hooks live for the process lifetime: an identical repeat install is a no-op
        // success, a different config is refused. No second engine is ever stacked.
        return std::memcmp(&g_config, config, sizeof(g_config)) == 0 ? OK : ERR_HOOK;
    }

    g_config = *config; // publish before any callback can read it (still under lock)

    CollisionFn col = nullptr, trg = nullptr;
    MakeThrowFn mk = nullptr;
    int rc_col = pgrazor_hook(g_config.code_collision,
                              reinterpret_cast<void *>(&collision_replacement),
                              reinterpret_cast<void **>(&col));
    int rc_trg = pgrazor_hook(g_config.code_trigger,
                              reinterpret_cast<void *>(&trigger_replacement),
                              reinterpret_cast<void **>(&trg));
    int rc_mk  = pgrazor_hook(g_config.code_make_throw,
                              reinterpret_cast<void *>(&make_throw_replacement),
                              reinterpret_cast<void **>(&mk));
    // Entry instruments: signature-agnostic, read the `this` ball only.
    int rc_il = pgrazor_instrument(g_config.code_launch, &instrument_launch);
    int rc_ir = pgrazor_instrument(g_config.code_release, &instrument_release);

    // A reported hook success with a null trampoline would make our replacement swallow
    // the game's own method. Undo exactly that target and treat the install as failed, so
    // the original runs unmodified.
    if (rc_col == 0 && col == nullptr) { pgrazor_unhook(g_config.code_collision);  rc_col = ERR_HOOK; }
    if (rc_trg == 0 && trg == nullptr) { pgrazor_unhook(g_config.code_trigger);    rc_trg = ERR_HOOK; }
    if (rc_mk  == 0 && mk  == nullptr) { pgrazor_unhook(g_config.code_make_throw); rc_mk  = ERR_HOOK; }

    if (rc_col != 0 || rc_trg != 0 || rc_mk != 0 || rc_il != 0 || rc_ir != 0) {
        // A failed partial patch is permanent and inert: targets that patched cleanly keep
        // their valid trampoline and merely chain the original (policy can never arm
        // because g_installed stays false and update() refuses); targets we unhooked above
        // run unmodified. Publish whatever originals exist so every chain is complete.
        g_orig_collision.store(col, std::memory_order_release);
        g_orig_trigger.store(trg, std::memory_order_release);
        g_orig_make_throw.store(mk, std::memory_order_release);
        g_install_failed = true;
        return ERR_HOOK;
    }

    g_orig_collision.store(col, std::memory_order_release);
    g_orig_trigger.store(trg, std::memory_order_release);
    g_orig_make_throw.store(mk, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    return OK;
}

int api_update(int enabled, int allowed, uint64_t lease_ms, uint64_t unity_tid) {
    if (!g_installed.load(std::memory_order_acquire)) return ERR_NOT_INSTALLED;

    // An out-of-policy lease is an invalid update: disable and report.
    if (lease_ms > kMaxLeaseMs) { gate_close(); return ERR_LEASE; }

    if (!enabled) { gate_close(); return OK; } // stop: clear window, request root free

    if (!allowed) {                            // armed but not currently permitted
        g_enabled.store(1, std::memory_order_relaxed);
        g_allowed.store(0, std::memory_order_relaxed);
        g_lease_deadline.store(0, std::memory_order_relaxed);
        g_tid.store(0, std::memory_order_relaxed);
        return OK;
    }
    if (unity_tid == 0 || lease_ms == 0) { gate_close(); return ERR_BAD_ARG; }

    uint64_t now = boottime_ms();
    if (now == 0) { gate_close(); return ERR_LEASE; }

    g_tid.store(unity_tid, std::memory_order_relaxed);
    g_cleanup_tid.store(unity_tid, std::memory_order_relaxed);
    g_lease_deadline.store(now + lease_ms, std::memory_order_relaxed);
    g_allowed.store(1, std::memory_order_relaxed);
    g_enabled.store(1, std::memory_order_relaxed);
    return OK;
}

int api_tick(uint64_t proven_tid) {
    if(proven_tid==static_cast<uint64_t>(gettid()))maybe_clear_handled();
    return OK;
}

int api_sample(PgrAlwaysHitSample *out) {
    if (out == nullptr) return ERR_NULL;
    out->applied    = g_applied.load(std::memory_order_relaxed);
    out->normalized = g_normalized.load(std::memory_order_relaxed);
    out->errors     = g_errors.load(std::memory_order_relaxed);
    return OK;
}

const PgrAlwaysHitApi g_api = {
    1, sizeof(PgrAlwaysHitApi),
    &api_install,
    &api_update,
    &api_sample,
    &api_tick,
};

} // namespace

const void *pgrazor_always_hit_api(uint32_t version) {
    if (version != 1) return nullptr;
    return &g_api;
}

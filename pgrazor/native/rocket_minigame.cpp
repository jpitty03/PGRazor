#include "rocket_minigame.h"
#include "hook_engine.h"

#include <atomic>
#include <mutex>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <unistd.h>
#include <sys/syscall.h>

// Native state is Unity-thread-only. The only cross-thread entrypoints are the table
// functions (update/tick/restore) driven from the JS pilot; those touch atomics and a
// pending flag, never the rooted handles. A busy guard prevents reentrancy because
// SetActive can re-enter the lifecycle hooks; the install mutex is never held across a
// runtime_invoke.

namespace {

typedef void (*UpdateFn)(void *, int64_t, void *);
typedef void (*LifeFn)(void *, void *);

std::mutex g_install_mutex;                    // serialises install() and publishes originals
PgrRocketMiniConfig g_cfg{};                   // validated copy, published under the mutex
bool g_install_failed = false;                 // sticky; guarded by g_install_mutex

std::atomic<int>     g_active{0};          // fully installed and live
std::atomic<int>     g_failed{0};          // latched managed-exception failure (stop collecting)
std::atomic<int>     g_policy_on{0};       // adapter says collection permitted
std::atomic<int64_t> g_deadline{0};        // BOOTTIME ms past which a cached allowed is dead
std::atomic<int>     g_restore_pending{0}; // off-thread restore request
std::atomic<int64_t> g_unity_tid{0};       // latched OS tid of the Unity thread (tick only)
std::atomic<int>     g_busy{0};            // reentrancy guard for Unity-thread state mutation

std::atomic<UpdateFn> g_orig_update{nullptr};
std::atomic<LifeFn>   g_orig_exit{nullptr};
std::atomic<LifeFn>   g_orig_hide{nullptr};
std::atomic<LifeFn>   g_orig_destroy{nullptr};

// Rooted snapshot, written and read only on the Unity thread under the busy guard.
struct MiniState {
    void *challenge;   // gchandle
    void *transform;   // gchandle
    void *go;          // gchandle
    float scale[3];
    int   active;
};
MiniState g_mini{};
bool g_have_mini = false;

int64_t boottime_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int64_t native_tid() { return (int64_t)syscall(SYS_gettid); }

bool on_unity_thread() {
    int64_t u = g_unity_tid.load(std::memory_order_acquire);
    return u != 0 && native_tid() == u;
}

bool native_allowed() {
    return g_active.load() && !g_failed.load() && g_policy_on.load() &&
           boottime_ms() < g_deadline.load();
}

// runtime_invoke wrapper. A thrown managed exception latches the module into a failed
// state (collection stops; restore is preserved) and yields a null result, mirroring the
// legacy JS path where invoke() throws and rocket.js sets rocketState.failed = true.
void *inv(void *method, void *obj, void **params) {
    void *exc = nullptr;
    void *r = g_cfg.runtime_invoke(method, obj, params, &exc);
    if (exc) { g_failed.store(1); return nullptr; }
    return r;
}

bool exact(void *obj, void *klass) {
    return obj && g_cfg.object_class(obj) == klass;
}

// UnityEngine.Object.op_Implicit(object) -> bool. Static; the reference arg is passed
// directly as the object pointer.
bool obj_alive(void *o) {
    if (!o) return false;
    void *params[1] = { o };
    void *boxed = inv(g_cfg.alive_method, nullptr, params);
    if (!exact(boxed, g_cfg.bool_class)) return false;
    uint8_t *b = (uint8_t *)g_cfg.object_unbox(boxed);
    return b && *b == 1;
}

// GameObject.activeSelf -> boxed bool. Tri-state: 1/0 for a valid boxed bool, or -1 when
// the result is missing / not a boxed System.Boolean / not a 0-or-1 byte. Returning -1
// (never defaulting to 0) prevents snapshotting a bogus "inactive" original and later
// overwriting the game's real state with SetActive(false).
int obj_active(void *go) {
    void *boxed = inv(g_cfg.active_method, go, nullptr);
    if (!boxed || !exact(boxed, g_cfg.bool_class)) return -1;
    uint8_t *b = (uint8_t *)g_cfg.object_unbox(boxed);
    if (!b) return -1;
    if (*b == 0) return 0;
    if (*b == 1) return 1;
    return -1;
}

void set_scale(void *transform, const float v[3]) {
    void *params[1] = { (void *)v }; // Vector3 value-type (size 12) passed by pointer
    inv(g_cfg.setScale_method, transform, params);
}
void set_active(void *go, int on) {
    uint8_t b = on ? 1 : 0;
    void *params[1] = { &b };
    inv(g_cfg.setActive_method, go, params);
}

// Snapshot is detached before any restoring call so a SetActive-driven re-entry cannot
// see a half-restored state (it sees g_have_mini == false, intentionally). Handles are
// freed only after the restore calls complete. Restore runs regardless of the failed
// latch so an owned target is always put back. No mutex is held across runtime_invoke.
void do_restore() {
    if (g_busy.exchange(1)) { g_restore_pending.store(1); return; }
    // Nothing owned: a pending request is already satisfied, so clear it (a reentrant
    // SetActive during a prior restore can have re-raised it) rather than letting a stale
    // flag undo the next fresh collection.
    if (!g_have_mini) { g_restore_pending.store(0); g_busy.store(0); return; }
    MiniState snap = g_mini;
    g_have_mini = false;              // detach first
    g_restore_pending.store(0);
    void *transform = g_cfg.target(snap.transform);
    void *go = g_cfg.target(snap.go);
    if (transform && obj_alive(transform)) set_scale(transform, snap.scale);
    if (go && obj_alive(go)) set_active(go, snap.active);
    g_cfg.free_root(snap.challenge);
    g_cfg.free_root(snap.transform);
    g_cfg.free_root(snap.go);
    g_busy.store(0);
}

// Snapshot original scale/active on first collect, then scale each component to
// max(abs(original),1)*1000 and force-activate. Only runs when running + alive/exact and
// external collection is allowed. Every managed reference used across an invoke is rooted
// first; the snapshot is fully validated before g_mini/g_have_mini are published, so a
// failure never leaks a root and never introduces a false "successful" collection.
void do_collect(void *challenge) {
    if (!native_allowed()) return;
    if (!exact(challenge, g_cfg.mini_class) || !obj_alive(challenge)) return;
    // running: read like present(challenge, running_field, 1) -> non-zero byte.
    uint8_t running = 0;
    g_cfg.field_get_value(challenge, g_cfg.running_field, &running);
    if (running == 0) return;

    // A different challenge means the previous one is stale: restore it first.
    if (g_have_mini && g_cfg.target(g_mini.challenge) != challenge) do_restore();

    if (g_busy.exchange(1)) return;

    if (!g_have_mini) {
        // Root the challenge BEFORE any invoke, so the collider it owns stays reachable
        // across the transform/gameObject calls (no unrooted managed refs across invokes).
        void *ch_h = g_cfg.root(challenge, 0);
        if (!ch_h) { g_busy.store(0); return; }

        void *collider = nullptr;
        g_cfg.field_get_value(challenge, g_cfg.collider_field, &collider);
        if (!collider || !obj_alive(collider)) { g_cfg.free_root(ch_h); g_busy.store(0); return; }

        void *transform = inv(g_cfg.transform_method, collider, nullptr);
        if (!transform) { g_cfg.free_root(ch_h); g_busy.store(0); return; }
        // Root the transform BEFORE invoking gameObject (the next invoke could GC it).
        void *tr_h = g_cfg.root(transform, 0);
        if (!tr_h) { g_cfg.free_root(ch_h); g_busy.store(0); return; }

        void *go = inv(g_cfg.gameObject_method, collider, nullptr);
        if (!go) { g_cfg.free_root(ch_h); g_cfg.free_root(tr_h); g_busy.store(0); return; }
        void *go_h = g_cfg.root(go, 0);
        if (!go_h) { g_cfg.free_root(ch_h); g_cfg.free_root(tr_h); g_busy.store(0); return; }

        // localScale -> boxed Vector3. Copy the three floats immediately, before the next
        // invoke (obj_active) could GC the box.
        void *boxed_scale = inv(g_cfg.scale_method, g_cfg.target(tr_h), nullptr);
        float *sv = boxed_scale ? (float *)g_cfg.object_unbox(boxed_scale) : nullptr;
        float sc[3];
        bool ok = sv != nullptr;
        for (int i = 0; i < 3 && ok; i++) {
            sc[i] = sv[i];
            if (!std::isfinite(sc[i]) || std::fabs(sc[i]) >= 10000.0f) ok = false;
        }
        // activeSelf: must be a valid boxed bool (tri-state). An unknown/invalid value
        // aborts the whole snapshot rather than guessing false.
        int act = ok ? obj_active(g_cfg.target(go_h)) : -1;
        if (!ok || act < 0) {
            g_cfg.free_root(ch_h);
            g_cfg.free_root(tr_h);
            g_cfg.free_root(go_h);
            g_busy.store(0);
            return;
        }

        // Snapshot fully valid -> publish (g_have_mini last).
        g_mini.challenge = ch_h;
        g_mini.transform = tr_h;
        g_mini.go = go_h;
        std::memcpy(g_mini.scale, sc, sizeof(sc));
        g_mini.active = act;
        g_have_mini = true;
    }

    float scaled[3];
    for (int i = 0; i < 3; i++) {
        float a = std::fabs(g_mini.scale[i]);
        if (a < 1.0f) a = 1.0f;
        scaled[i] = a * 1000.0f;
    }
    set_scale(g_cfg.target(g_mini.transform), scaled);
    set_active(g_cfg.target(g_mini.go), 1);
    g_busy.store(0);
}

void maybe_restore_on_thread() {
    if (g_have_mini && (g_restore_pending.load() || !native_allowed() ||
        !obj_alive(g_cfg.target(g_mini.challenge)))) do_restore();
}

// --- original publication ---------------------------------------------------------
// A replacement that runs before install has published its original would otherwise
// chain through a null pointer and silently swallow the game's own method. If the slot
// is not yet visible, synchronise on the install mutex (held by install across the whole
// patch+publish sequence, and never across a runtime_invoke). After the mutex a live hook
// with a null original is impossible, so abort rather than skip the original.
template <class Fn>
Fn original_for(std::atomic<Fn> &slot) {
    Fn o = slot.load(std::memory_order_acquire);
    if (!o) {
        std::lock_guard<std::mutex> lk(g_install_mutex);
        o = slot.load(std::memory_order_acquire);
    }
    if (!o) std::abort();
    return o;
}

// --- hook replacements (run on the Unity thread) ---------------------------------

void repl_update(void *self, int64_t a1, void *method) {
    // The hook NEVER learns or overwrites the Unity tid; it only gates on the tid latched
    // by tick(). Restore/collect touch rooted state and so require the Unity thread.
    if (g_active.load() && on_unity_thread()) {
        maybe_restore_on_thread(); // clear stale/no-longer-allowed state first
    }
    // Chain the original THEN collect (original published before the hook went live).
    original_for(g_orig_update)(self, a1, method);
    if (g_active.load() && on_unity_thread() && native_allowed()) do_collect(self);
}

void restore_if_owned(void *self) {
    if (!g_active.load()) return;
    if (!on_unity_thread()) return; // only touch rooted state on the proven Unity thread
    if (g_have_mini && g_cfg.target(g_mini.challenge) == self) {
        if (g_busy.load()) g_restore_pending.store(1);
        else do_restore();
    }
    // A minigame lifecycle event ends this challenge instance; clear the transient
    // managed-exception latch so the next challenge gets a fresh attempt, matching the
    // legacy per-encounter reset. Restoration above is unaffected by the latch.
    g_failed.store(0);
}

// Lifecycle restore happens BEFORE the original runs.
void repl_exit(void *self, void *method)    { restore_if_owned(self); original_for(g_orig_exit)(self, method); }
void repl_hide(void *self, void *method)    { restore_if_owned(self); original_for(g_orig_hide)(self, method); }
void repl_destroy(void *self, void *method) { restore_if_owned(self); original_for(g_orig_destroy)(self, method); }

// --- config validation -------------------------------------------------------------

bool config_pointers_ok(const PgrRocketMiniConfig *c) {
    const void *const ptrs[] = {
        c->update_addr, c->exit_addr, c->hide_addr, c->destroy_addr,
        c->transform_method, c->gameObject_method, c->scale_method, c->active_method,
        c->setScale_method, c->setActive_method, c->alive_method,
        c->collider_field, c->running_field, c->mini_class, c->bool_class,
        (const void *)c->root, (const void *)c->free_root, (const void *)c->target,
        (const void *)c->object_class, (const void *)c->object_unbox,
        (const void *)c->runtime_invoke, (const void *)c->field_get_value};
    for (const void *p : ptrs)
        if (p == nullptr) return false;
    return true;
}

// --- table functions -------------------------------------------------------------

// Hook one site. On a reported success with a null trampoline, undo that exact site so
// the game's original runs unmodified, and report failure.
int hook_site(void *addr, void *repl, void **orig) {
    *orig = nullptr;
    int rc = pgrazor_hook(addr, repl, orig);
    if (rc == 0 && *orig == nullptr) { pgrazor_unhook(addr); return PGR_RMINI_ERR_HOOK_FAILED; }
    return rc == 0 ? PGR_RMINI_OK : PGR_RMINI_ERR_HOOK_FAILED;
}

int api_install(const PgrRocketMiniConfig *cfg) {
    if (!cfg) return PGR_RMINI_ERR_NULL_ARG;

    std::lock_guard<std::mutex> lk(g_install_mutex);
    if (g_install_failed) return PGR_RMINI_ERR_HOOK_FAILED; // never re-patch after a partial failure

    if (cfg->version != 1 || cfg->size != sizeof(PgrRocketMiniConfig) || !config_pointers_ok(cfg))
        return PGR_RMINI_ERR_BAD_CONFIG;

    if (g_active.load(std::memory_order_acquire)) {
        // Hooks live for the process lifetime: an identical repeat is a no-op success, a
        // different config is refused.
        return std::memcmp(&g_cfg, cfg, sizeof(g_cfg)) == 0
                   ? PGR_RMINI_OK
                   : PGR_RMINI_ERR_ALREADY_BOUND;
    }

    g_cfg = *cfg; // publish before any callback can read it (still under the mutex)
    g_have_mini = false;
    g_failed.store(0);
    g_policy_on.store(0);
    g_deadline.store(0);
    g_restore_pending.store(0);
    g_busy.store(0);

    void *u = nullptr, *e = nullptr, *h = nullptr, *d = nullptr;
    int rc_u = hook_site(cfg->update_addr,  (void *)repl_update,  &u);
    int rc_e = hook_site(cfg->exit_addr,    (void *)repl_exit,    &e);
    int rc_h = hook_site(cfg->hide_addr,    (void *)repl_hide,    &h);
    int rc_d = hook_site(cfg->destroy_addr, (void *)repl_destroy, &d);

    if (rc_u != PGR_RMINI_OK || rc_e != PGR_RMINI_OK ||
        rc_h != PGR_RMINI_OK || rc_d != PGR_RMINI_OK) {
        // Partial failure is permanent and inert: cleanly-patched sites keep their valid
        // trampoline and merely chain the original (g_active stays 0, so collection can
        // never arm); sites that failed/were undone run the game's original unmodified. No
        // second engine is ever stacked and no later install repatches.
        g_orig_update.store((UpdateFn)u, std::memory_order_release);
        g_orig_exit.store((LifeFn)e, std::memory_order_release);
        g_orig_hide.store((LifeFn)h, std::memory_order_release);
        g_orig_destroy.store((LifeFn)d, std::memory_order_release);
        g_install_failed = true;
        return PGR_RMINI_ERR_HOOK_FAILED;
    }

    g_orig_update.store((UpdateFn)u, std::memory_order_release);
    g_orig_exit.store((LifeFn)e, std::memory_order_release);
    g_orig_hide.store((LifeFn)h, std::memory_order_release);
    g_orig_destroy.store((LifeFn)d, std::memory_order_release);
    g_active.store(1, std::memory_order_release); // publish last
    return PGR_RMINI_OK;
}

int api_update(int allowed, int64_t lease_remaining_ms) {
    if (allowed) {
        int64_t lease = lease_remaining_ms;
        if (lease < 0) lease = 0;
        if (lease > 2500) lease = 2500;
        g_policy_on.store(1);
        g_deadline.store(boottime_ms() + lease);
    } else {
        g_policy_on.store(0); // policy only; restoration is left to tick/lifecycle hooks
    }
    return g_active.load();
}

int api_tick(int proven_unity_tick) {
    if (!g_active.load()) return 0;
    if (proven_unity_tick) {
        // tick() is invoked by the host's verified Unity loop: the running thread IS the
        // Unity thread, so latch its tid here (the sole authority) and process a pending
        // restore. A non-proven tick touches no rooted state.
        g_unity_tid.store(native_tid(), std::memory_order_release);
        maybe_restore_on_thread();
    }
    return 1;
}

int api_restore() {
    if (!g_active.load()) return 0;
    g_policy_on.store(0); // immediate stop of collection
    if (on_unity_thread() && !g_busy.load()) do_restore();
    else g_restore_pending.store(1); // off-thread: next on-thread hook/tick restores
    return 1;
}

} // namespace

extern "C" const void *pgrazor_rocket_minigame_api(uint32_t version) {
    static const PgrRocketMiniApi api = {
        1, (uint32_t)sizeof(PgrRocketMiniApi),
        api_install, api_update, api_tick, api_restore
    };
    return version == 1 ? &api : nullptr;
}

// Standalone host/Android harness for feature 8 (catch animation speed ownership).
// Never linked into or packaged with the app; the coordinator cross-compiles it and
// runs it against a real libpgrazor-core.so with `./catch_animation_test <path-to-.so>`.
//
// catch_animation installs NO Dobby hook in this stage: the JS adapter owns the Gum
// observers and CALLS apply()/complete() on a confirmed success/finish. So, unlike
// rocket_minigame_test.cpp, there are no padded hook targets -- the whole fake managed
// runtime (Animator.get_speed/set_speed, Object.op_Implicit liveness, the boxed
// Single/Boolean readers, the il2cpp gchandle/object exports) is modelled in-process so
// a single thread drives the apply/restore state machine deterministically, exactly like
// the fakes in rocket_minigame_test.cpp.
//
// The API is pulled through the core getter registry (dlsym only exposes
// pgrazor_native_api; pgrazor_catch_animation_api is hidden by the version script and
// answered under feature id 8).
//
// A restore/apply/root fault LATCHES for the process lifetime (g_fault is sticky), so
// every fault scenario runs in its own fork()ed child off a pristine post-install state;
// each child drives one deterministic case and _exit()s its own status. The clean
// (non-latching) cases run in the parent. The 8 s timeout path is intentionally not
// driven here -- it cannot be reached without advancing CLOCK_BOOTTIME 8 s, and it shares
// restore() with the lease-expiry case which IS driven.
#include "catch_animation.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <limits>

// Event (action) and reason codes, mirrored from catch_animation.cpp's anonymous
// namespace (not exported); the JS adapter hardcodes the same mapping.
enum { EV_APPLIED = 1, EV_RESTORED = 2, EV_EXTERNAL = 3, EV_DESTROYED = 4,
       EV_CLEANUP_FAILED = 5, EV_SKIP = 6 };
enum { R_OFF = 2, R_LEASE = 3, R_SKIP_NONSTANDARD = 10, R_ROOT = 12 };

constexpr float kBallSpeed = 20.0f;

// --- fake managed heap ------------------------------------------------------------
// Objects start with {klass, alive} so object_get_class and the op_Implicit liveness
// probe are served uniformly; the Animator adds its live speed.
struct Animator { void *klass; int alive; float speed; };
struct Response { void *klass; int alive; };
struct Boxed    { void *klass; uint8_t payload[8]; };

// Class identities (address markers) and MethodInfo* markers.
int animator_class, single_class, bool_class, response_class;
int get_speed_m, set_speed_m, alive_m, exc_marker;
void *const ANIM = &animator_class;

// Shared boxed scratch; each reader copies out immediately after the invoke.
Boxed g_box_single{}, g_box_bool{};

// The managed objects the scenarios drive.
Animator g_anim{};
Response g_resp{}, g_resp2{};

// GC-root bookkeeping with an injectable failure point (as in rocket_minigame_test.cpp).
struct Handle { void *obj; bool used; };
Handle g_handles[32];
int g_roots = 0, g_root_calls = 0, g_root_fail_on = 0;
bool g_missing_animator = false;

// set_speed injection knobs.
int g_set_calls = 0;          // every set_speed invoke, success or fail
int g_set_fail = 0;           // set_speed throws WITHOUT writing
int g_set_throw_after_write = 0; // one-shot: write the value THEN throw

// Reentrancy: when armed, the set_speed invoke re-enters apply() once.
int   g_reenter = 0;
void *g_reenter_anim = nullptr;
void *g_reenter_resp = nullptr;

const PgrSpeedApi *g_api = nullptr; // resolved once; used by the reentry injection

// --- fake IL2CPP exports ----------------------------------------------------------
void *fake_root(void *o, int) {
    ++g_root_calls;
    if (g_root_fail_on && g_root_calls == g_root_fail_on) return nullptr;
    for (auto &h : g_handles) if (!h.used) { h.used = true; h.obj = o; ++g_roots; return &h; }
    return nullptr;
}
void *fake_target(void *h) {
    void *o = h ? static_cast<Handle *>(h)->obj : nullptr;
    return g_missing_animator && o == &g_anim ? nullptr : o;
}
void  fake_free(void *h)   { if (h && static_cast<Handle *>(h)->used) { static_cast<Handle *>(h)->used = false; --g_roots; } }
void *fake_class(void *o)  { return static_cast<Animator *>(o)->klass; } // klass is each struct's first field
void *fake_unbox(void *o)  { return static_cast<Boxed *>(o)->payload; }

void *fake_invoke(void *method, void *obj, void **params, void **exc) {
    if (exc) *exc = nullptr;
    if (method == &get_speed_m) {
        std::memcpy(g_box_single.payload, &static_cast<Animator *>(obj)->speed, sizeof(float));
        g_box_single.klass = &single_class; return &g_box_single;
    }
    if (method == &set_speed_m) {
        ++g_set_calls;
        if (g_set_fail) { if (exc) *exc = &exc_marker; return nullptr; } // threw, nothing written
        std::memcpy(&static_cast<Animator *>(obj)->speed, params[0], sizeof(float));
        if (g_set_throw_after_write) { g_set_throw_after_write = 0; if (exc) *exc = &exc_marker; return nullptr; }
        if (g_reenter) { g_reenter = 0; g_api->apply(g_reenter_anim, g_reenter_resp); } // re-entry during the setter
        return nullptr;
    }
    if (method == &alive_m) { // static op_Implicit(Object): arg[0] is the object
        g_box_bool.payload[0] = static_cast<Animator *>(params[0])->alive ? 1 : 0;
        g_box_bool.klass = &bool_class; return &g_box_bool;
    }
    return nullptr;
}

// --- test helpers -----------------------------------------------------------------
PgrSpeedSample snap() { PgrSpeedSample s{}; g_api->sample(&s); return s; }

void reset_fakes() {
    for (auto &h : g_handles) { h.used = false; h.obj = nullptr; }
    g_roots = g_root_calls = g_root_fail_on = 0;
    g_missing_animator = false;
    g_set_calls = g_set_fail = g_set_throw_after_write = 0;
    g_reenter = 0; g_reenter_anim = g_reenter_resp = nullptr;
    g_anim = Animator{&animator_class, 1, 1.0f};
    g_resp = Response{&response_class, 1};
    g_resp2 = Response{&response_class, 1};
}

void sleep_ms(long ms) { timespec ts{ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&ts, nullptr); }

// =================================================================================
// Latched-fault cases: each runs in its own fork()ed child (sticky g_fault). They
// drive one deterministic scenario and return 0 on pass, a distinct code otherwise.
// =================================================================================

// Managed set_speed that throws AFTER mutating native state: apply must KEEP ownership
// (the snapshot + 20x write happened) and a later tick must restore the original.
int child_setter_throws_after_write() {
    reset_fakes();
    g_set_throw_after_write = 1; // one-shot on the apply write
    g_api->update(1, 2500, gettid());
    g_api->apply(&g_anim, &g_resp);
    if (snap().owned != 1 || snap().fault != 1 || g_roots != 2) return 1;
    if (g_anim.speed != kBallSpeed) return 2; // native state WAS mutated before the throw
    g_api->tick(gettid());                    // fault -> restore(R_STOP); the set now succeeds
    if (snap().action != EV_RESTORED || snap().owned != 0 || g_roots != 0) return 3;
    if (g_anim.speed != 1.0f) return 4;
    return 0;
}

// A restore failure latches the fault, keeps the roots, and schedules a 1 s retry; a
// soft tick inside that window must NOT re-attempt the set; a forced restore (complete)
// bypasses the window and succeeds without ever dropping the roots.
int child_restore_failure_retries() {
    reset_fakes();
    g_api->update(1, 2500, gettid());
    g_api->apply(&g_anim, &g_resp);
    if (snap().owned != 1 || g_roots != 2) return 10;
    g_set_fail = 1;                 // the restore write will throw
    g_api->update(0, 0, 0);         // off -> next tick restores
    int before = g_set_calls;
    g_api->tick(gettid());          // restore(R_OFF) write fails -> fault, retry_at=now+1s, roots kept
    if (snap().fault != 1 || snap().owned != 1 || g_roots != 2) return 11;
    if (snap().action != EV_CLEANUP_FAILED) return 12;
    if (g_set_calls != before + 1) return 13; // exactly one failed attempt
    int after = g_set_calls;
    g_api->tick(gettid());          // inside the 1 s window: no retry, no set, roots preserved
    if (g_set_calls != after || g_roots != 2 || snap().owned != 1) return 14;
    g_set_fail = 0;                 // the game recovers
    g_api->complete(&g_resp, 5);    // forced restore bypasses the retry window
    if (snap().action != EV_RESTORED || snap().owned != 0 || g_roots != 0) return 15;
    if (g_anim.speed != 1.0f) return 16;
    return 0;
}

// Root failure on the animator handle: partial state unwinds, nothing is written, the
// fault latches and no root leaks.
int child_root_failure_animator() {
    reset_fakes();
    g_api->update(1, 2500, gettid());
    g_root_calls = 0; g_root_fail_on = 1; // animator must be rooted before any managed getter
    g_api->apply(&g_anim, &g_resp);
    if (snap().owned != 0 || g_roots != 0 || snap().fault != 1) return 20;
    if (snap().action != EV_CLEANUP_FAILED || snap().reason != R_ROOT) return 21;
    if (g_anim.speed != 1.0f) return 22; // never written
    return 0;
}

// Root failure on the response handle: the already-taken animator handle is freed before
// the fault latches -> no leak.
int child_root_failure_response() {
    reset_fakes();
    g_api->update(1, 2500, gettid());
    g_root_calls = 0; g_root_fail_on = 2; // animator ok, response fails before managed getters
    g_api->apply(&g_anim, &g_resp);
    if (snap().owned != 0 || g_roots != 0 || snap().fault != 1) return 23;
    if (snap().action != EV_CLEANUP_FAILED || snap().reason != R_ROOT) return 24;
    if (g_anim.speed != 1.0f) return 25;
    return 0;
}

// A current speed read back as NaN during restore is a fault/retry, NOT an external
// change: ownership and the fault are kept, nothing is released.
int child_nan_current_keeps_ownership() {
    reset_fakes();
    g_api->update(1, 2500, gettid());
    g_api->apply(&g_anim, &g_resp);
    if (snap().owned != 1 || g_roots != 2) return 30;
    g_anim.speed = std::numeric_limits<float>::quiet_NaN();
    g_api->update(0, 0, 0);
    g_api->tick(gettid());          // restore reads NaN -> !finite -> fault, retry
    if (snap().action == EV_EXTERNAL) return 31; // MUST NOT be treated as an external change
    if (snap().action != EV_CLEANUP_FAILED || snap().fault != 1) return 32;
    if (snap().owned != 1 || g_roots != 2) return 33; // ownership retained for the retry
    return 0;
}

// A managed set_speed that synchronously re-enters apply() must not deadlock (the reentry
// guard is a flag, never a mutex held across the invoke): the nested apply is dropped and
// the outer apply still completes.
int child_reentrancy_no_deadlock() {
    reset_fakes();
    g_reenter = 1; g_reenter_anim = &g_anim; g_reenter_resp = &g_resp;
    g_api->update(1, 2500, gettid());
    g_api->apply(&g_anim, &g_resp); // reaching the next line at all proves no deadlock
    if (g_reenter != 0) return 40;  // the reentry fired from inside the setter
    if (snap().action != EV_APPLIED || snap().owned != 1) return 41;
    if (g_anim.speed != kBallSpeed || g_roots != 2) return 42;
    return 0;
}

int run_child(int (*body)()) {
    pid_t pid = fork();
    if (pid == 0) _exit(body());
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 126;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 125;
}

int child_missing_target_keeps_ownership() {
    reset_fakes();
    g_api->update(1, 2500, gettid());
    g_api->apply(&g_anim, &g_resp);
    g_missing_animator = true;
    g_api->update(0, 0, 0);
    g_api->tick(gettid());
    if (!snap().fault || !snap().owned || g_roots != 2 || g_anim.speed != kBallSpeed) return 1;
    g_missing_animator = false;
    g_api->complete(&g_resp, 5);
    return snap().owned || g_roots || g_anim.speed != 1.0f ? 2 : 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::puts(dlerror()); return 2; }
    auto get = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib, "pgrazor_native_api"));
    auto core = get ? static_cast<const pgrazor_native_api_v2 *>(get(2)) : nullptr;
    if (!core || core->size != 48 || core->query_feature(8, 99) || core->query_feature(99, 1)) return 3;
    g_api = static_cast<const PgrSpeedApi *>(core->query_feature(8, 1));
    if (!g_api || g_api->version != 1 || g_api->size != 56) return 4;

    PgrSpeedConfig cfg{};
    cfg.version = 1; cfg.size = sizeof(cfg);
    cfg.runtime_invoke = reinterpret_cast<void *>(&fake_invoke);
    cfg.gchandle_new = reinterpret_cast<void *>(&fake_root);
    cfg.gchandle_get_target = reinterpret_cast<void *>(&fake_target);
    cfg.gchandle_free = reinterpret_cast<void *>(&fake_free);
    cfg.object_get_class = reinterpret_cast<void *>(&fake_class);
    cfg.object_unbox = reinterpret_cast<void *>(&fake_unbox);
    cfg.method_get_speed = &get_speed_m;
    cfg.method_set_speed = &set_speed_m;
    cfg.method_alive = &alive_m;
    cfg.class_single = &single_class;
    cfg.class_bool = &bool_class;
    cfg.class_animator = &animator_class;

    reset_fakes();
    // install() is process-lifetime and idempotent for an identical config; a config
    // whose version/size/pointers fail validation never writes.
    if (g_api->install(&cfg) != 0 || g_api->install(&cfg) != 0) return 5;
    PgrSpeedConfig bad = cfg; bad.class_bool = bad.class_single; // duplicate class -> refused
    if (g_api->install(&bad) == 0) return 6;

    // --- latched-fault cases, each off the pristine post-install state ----------------
    if (run_child(&child_setter_throws_after_write)) return 10;
    if (run_child(&child_restore_failure_retries))   return 11;
    if (run_child(&child_root_failure_animator))     return 12;
    if (run_child(&child_root_failure_response))     return 13;
    if (run_child(&child_nan_current_keeps_ownership)) return 14;
    if (run_child(&child_reentrancy_no_deadlock))    return 15;
    if (run_child(&child_missing_target_keeps_ownership)) return 16;

    // =============================================================================
    // Clean (non-latching) cases run in the parent; each ends with no owned attempt,
    // no leaked root and no latched fault.
    // =============================================================================

    // (1) Confirmed success with the original ~1.0: snapshot + 20x write + readback.
    reset_fakes();
    g_api->update(1, 2500, gettid());
    g_api->apply(&g_anim, &g_resp);
    if (snap().action != EV_APPLIED || snap().owned != 1) return 50;
    if (g_anim.speed != kBallSpeed || g_roots != 2) return 51;
    // off then a proven tick restores the original and frees both roots.
    g_api->update(0, 0, 0);
    g_api->tick(gettid());
    if (snap().action != EV_RESTORED || snap().reason != R_OFF) return 52;
    if (g_anim.speed != 1.0f || g_roots != 0 || snap().owned != 0 || snap().fault != 0) return 53;

    // (2) A pre-existing non-standard speed is left untouched: apply skips, roots nothing.
    reset_fakes();
    g_anim.speed = 1.5f;
    g_api->update(1, 2500, gettid());
    g_api->apply(&g_anim, &g_resp);
    if (snap().action != EV_SKIP || snap().reason != R_SKIP_NONSTANDARD) return 54;
    if (g_anim.speed != 1.5f || g_roots != 0 || snap().owned != 0) return 55;
    g_api->update(0, 0, 0);

    // (3) complete() only restores the attempt whose rooted response matches.
    reset_fakes();
    g_api->update(1, 2500, gettid());
    g_api->apply(&g_anim, &g_resp);
    if (snap().owned != 1) return 56;
    g_api->complete(&g_resp2, 5); // a different response: ignored
    if (snap().owned != 1 || g_anim.speed != kBallSpeed) return 57;
    g_api->complete(&g_resp, 5);  // the matching response: forced restore
    if (snap().action != EV_RESTORED || snap().owned != 0 || g_anim.speed != 1.0f || g_roots != 0) return 58;
    g_api->update(0, 0, 0);

    // (4) Lease expiry restores (shares restore() with the omitted 8 s timeout path).
    reset_fakes();
    g_api->update(1, 2500, gettid());
    g_api->apply(&g_anim, &g_resp);
    g_api->update(1, 1, gettid());
    sleep_ms(10);
    g_api->tick(gettid());
    if (snap().action != EV_RESTORED || snap().reason != R_LEASE) return 59;
    if (g_anim.speed != 1.0f || g_roots != 0) return 60;
    g_api->update(0, 0, 0);

    // (5) An external change away from 20x releases ownership WITHOUT overwriting; the
    //     externally-chosen value is retained and no fault latches.
    reset_fakes();
    g_api->update(1, 2500, gettid());
    g_api->apply(&g_anim, &g_resp);
    g_anim.speed = 5.0f; // some other system set it
    g_api->update(0, 0, 0);
    g_api->tick(gettid());
    if (snap().action != EV_EXTERNAL) return 61;
    if (g_anim.speed != 5.0f || g_roots != 0 || snap().owned != 0 || snap().fault != 0) return 62;

    // (6) A destroyed animator releases the roots cleanly (no write, no fault).
    reset_fakes();
    g_api->update(1, 2500, gettid());
    g_api->apply(&g_anim, &g_resp);
    g_anim.alive = 0;
    g_api->update(0, 0, 0);
    g_api->tick(gettid());
    if (snap().action != EV_DESTROYED || g_roots != 0 || snap().owned != 0 || snap().fault != 0) return 63;

    // (7) Off the proven Unity thread, apply() does nothing: no snapshot, no root, no write.
    reset_fakes();
    g_api->update(1, 2500, gettid());
    g_api->apply(&g_anim, &g_resp);
    if (g_api->update(1, 2500, gettid() + 1) == 0) return 65;
    g_api->tick(gettid()); // rejected policy change cannot steal the cleanup thread
    if (snap().owned || g_roots || g_anim.speed != 1.0f) return 66;

    reset_fakes();
    g_api->update(1, 2500, gettid() + 1); // prove a thread that is not this one
    g_api->apply(&g_anim, &g_resp);
    if (snap().owned != 0 || g_anim.speed != 1.0f || g_roots != 0) return 64;

    std::puts("PASS native catch animation: dlopen/getter-registry ABI, config v1 (104B) validation, "
              "apply 20x only when original ~1 (non-standard preserved), complete only on the matching "
              "response, off/lease restore (8 s timeout omitted), external-change retention, destroyed "
              "release, wrong-thread reject, setter-throws-after-write ownership+tick restore, restore "
              "retry without dropping roots, partial-root-failure containment, NaN-current fault-retry "
              "(not external), reentrant setter no deadlock");
    return 0;
}

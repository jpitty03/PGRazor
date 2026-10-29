// Standalone Android harness for feature 6 (rocket minigame collector). Never linked
// into or packaged with the app; the coordinator cross-compiles it and runs it against a
// real libpgrazor-core.so with `./rocket_minigame_test <path-to-.so>`.
//
// The four lifecycle/update sites are REAL Dobby hooks (padded no-inline targets reached
// through volatile call pointers, exactly like ar_throw_test.cpp). Everything the native
// collector touches through the config -- the managed challenge/collider/transform/
// gameObject, the boxed Vector3/bool results, the IL2CPP runtime_invoke/object/field/
// gchandle exports -- is modelled by in-process fakes so a single thread can drive the
// whole collect/restore state machine deterministically.
#include "rocket_minigame.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>

// --- fake managed heap ------------------------------------------------------------
// Every managed object starts with {klass, alive} so object_get_class and the
// op_Implicit liveness probe can be served uniformly.
struct Header    { void *klass; int alive; };
struct Transform { void *klass; int alive; float scale[3]; };
struct GameObject{ void *klass; int alive; int active; };
struct Collider  { void *klass; int alive; Transform *tr; GameObject *go; };
struct Challenge { void *klass; int alive; uint8_t running; Collider *collider; };
struct Boxed     { void *klass; uint8_t payload[16]; };

// Class identities (address markers) and MethodInfo*/FieldInfo* markers.
int mini_tag, bool_tag, vec_tag;
int transform_m, gameObject_m, scale_m, active_m, setScale_m, setActive_m, alive_m;
int collider_f, running_f;
void *const MINI = &mini_tag, *const BOOL = &bool_tag, *const VEC = &vec_tag;

// Shared boxed scratch; every reader copies out immediately after the invoke.
Boxed g_box_bool{}, g_box_vec{};

// GC-root bookkeeping with an injectable failure point.
struct Handle { void *obj; bool used; };
Handle g_handles[16];
int g_roots = 0, g_root_calls = 0, g_root_fail_on = 0;

// SetActive-driven reentrancy: when armed, the setActive invoke re-enters OnDestroy once.
int   g_reenter = 0;
void *g_reenter_challenge = nullptr;
void *g_reenter_method = nullptr;

// Original-chaining observation for the real hooks.
int g_update_orig = 0, g_exit_orig = 0, g_hide_orig = 0, g_destroy_orig = 0;
int64_t g_last_arg = 0;
Transform *g_watch = nullptr;   // transform observed by the OnExit original
float g_exit_observed = -1.0f;  // its scale[0] AT THE MOMENT the original runs

// --- real hook targets (padded, reached via volatile pointers) --------------------
__attribute__((noinline)) void t_update(void *self, int64_t a1, void *m) {
    asm volatile("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
    ++g_update_orig; g_last_arg = a1; (void)self; (void)m;
}
__attribute__((noinline)) void t_exit(void *self, void *m) {
    asm volatile("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
    ++g_exit_orig; if (g_watch) g_exit_observed = g_watch->scale[0]; (void)self; (void)m;
}
__attribute__((noinline)) void t_hide(void *self, void *m) {
    asm volatile("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
    ++g_hide_orig; (void)self; (void)m;
}
__attribute__((noinline)) void t_destroy(void *self, void *m) {
    asm volatile("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
    ++g_destroy_orig; (void)self; (void)m;
}
typedef void (*UpdFn)(void *, int64_t, void *);
typedef void (*LifeFn)(void *, void *);
UpdFn  volatile call_update  = &t_update;
LifeFn volatile call_exit    = &t_exit;
LifeFn volatile call_hide    = &t_hide;
LifeFn volatile call_destroy = &t_destroy;

// --- fake IL2CPP exports ----------------------------------------------------------
void *fake_root(void *o, int) {
    ++g_root_calls;
    if (g_root_fail_on && g_root_calls == g_root_fail_on) return nullptr;
    for (auto &h : g_handles) if (!h.used) { h.used = true; h.obj = o; ++g_roots; return &h; }
    return nullptr;
}
void *fake_target(void *h) { return h ? static_cast<Handle *>(h)->obj : nullptr; }
void  fake_free(void *h)   { if (h && static_cast<Handle *>(h)->used) { static_cast<Handle *>(h)->used = false; --g_roots; } }
void *fake_class(void *o)  { return static_cast<Header *>(o)->klass; }
void *fake_unbox(void *o)  { return static_cast<Boxed *>(o)->payload; }

void fake_field_get(void *obj, void *field, void *out) {
    Challenge *c = static_cast<Challenge *>(obj);
    if (field == &running_f)       *static_cast<uint8_t *>(out) = c->running;
    else if (field == &collider_f) *static_cast<void **>(out) = c->collider;
}

void *fake_invoke(void *method, void *obj, void **params, void **exc) {
    if (exc) *exc = nullptr; // no managed exception is simulated in this harness
    if (method == &transform_m)  return static_cast<Collider *>(obj)->tr;
    if (method == &gameObject_m) return static_cast<Collider *>(obj)->go;
    if (method == &scale_m) {
        std::memcpy(g_box_vec.payload, static_cast<Transform *>(obj)->scale, sizeof(float) * 3);
        g_box_vec.klass = VEC; return &g_box_vec;
    }
    if (method == &active_m) {
        g_box_bool.payload[0] = (uint8_t)static_cast<GameObject *>(obj)->active;
        g_box_bool.klass = BOOL; return &g_box_bool;
    }
    if (method == &alive_m) {
        Header *h = static_cast<Header *>(params[0]);
        g_box_bool.payload[0] = (h && h->alive) ? 1 : 0;
        g_box_bool.klass = BOOL; return &g_box_bool;
    }
    if (method == &setScale_m) {
        std::memcpy(static_cast<Transform *>(obj)->scale, params[0], sizeof(float) * 3);
        return nullptr;
    }
    if (method == &setActive_m) {
        static_cast<GameObject *>(obj)->active = *static_cast<uint8_t *>(params[0]);
        if (g_reenter) { g_reenter = 0; call_destroy(g_reenter_challenge, g_reenter_method); }
        return nullptr;
    }
    return nullptr;
}

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::puts(dlerror()); return 2; }
    auto get = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib, "pgrazor_native_api"));
    auto core = get ? static_cast<const pgrazor_native_api_v2 *>(get(2)) : nullptr;
    if (!core || core->size != 48 || core->query_feature(6, 99) || core->query_feature(99, 1)) return 3;
    auto api = static_cast<const PgrRocketMiniApi *>(core->query_feature(6, 1));
    if (!api || api->version != 1 || api->size != 40) return 4;

    PgrRocketMiniConfig cfg{};
    cfg.version = 1; cfg.size = sizeof(cfg);
    cfg.update_addr = (void *)&t_update; cfg.exit_addr = (void *)&t_exit;
    cfg.hide_addr = (void *)&t_hide; cfg.destroy_addr = (void *)&t_destroy;
    cfg.transform_method = &transform_m; cfg.gameObject_method = &gameObject_m;
    cfg.scale_method = &scale_m; cfg.active_method = &active_m;
    cfg.setScale_method = &setScale_m; cfg.setActive_method = &setActive_m;
    cfg.alive_method = &alive_m;
    cfg.collider_field = &collider_f; cfg.running_field = &running_f;
    cfg.mini_class = MINI; cfg.bool_class = BOOL;
    cfg.root = &fake_root; cfg.free_root = &fake_free; cfg.target = &fake_target;
    cfg.object_class = &fake_class; cfg.object_unbox = &fake_unbox;
    cfg.runtime_invoke = &fake_invoke; cfg.field_get_value = &fake_field_get;

    // Install is process-lifetime and idempotent for an identical config.
    if (api->install(&cfg) != PGR_RMINI_OK || api->install(&cfg) != PGR_RMINI_OK) return 5;

    void *method = &transform_m; // any non-null MethodInfo* stand-in for the hook arg
    GameObject goA{VEC, 1, 0};  Transform trA{VEC, 1, {2.f, -3.f, 0.5f}};
    Collider colA{VEC, 1, &trA, &goA};  Challenge chA{MINI, 1, 1, &colA};

    // 1) Original chaining + exact int64 arg, and NO collection before a proven tick:
    //    policy is live but the Unity tid is unlatched, so the rooted path never runs.
    api->update(1, 2500);
    g_roots = g_root_calls = 0;
    call_update(&chA, 111, method);
    if (g_update_orig != 1 || g_last_arg != 111) return 6;
    if (g_roots != 0 || trA.scale[0] != 2.f || goA.active != 0) return 7;

    // Prove the Unity thread, then a first collect snapshots the original and scales it.
    api->tick(1);
    g_root_calls = 0;
    call_update(&chA, 222, method);
    if (g_last_arg != 222) return 8;
    if (g_roots != 3) return 9; // challenge + transform + gameObject rooted
    if (trA.scale[0] != 2000.f || trA.scale[1] != 3000.f || trA.scale[2] != 1000.f) return 10;
    if (goA.active != 1) return 11;

    // 2) Repeated collection of the SAME challenge never re-snapshots (no new roots).
    int roots_calls = g_root_calls;
    call_update(&chA, 333, method);
    if (g_roots != 3 || g_root_calls != roots_calls) return 12;
    if (trA.scale[0] != 2000.f) return 13; // reapplied from the preserved original

    // 3) Disable (policy off) then a proven tick restores the original and frees roots.
    api->update(0, 0);
    api->tick(1);
    if (g_roots != 0) return 14;
    if (trA.scale[0] != 2.f || trA.scale[1] != -3.f || trA.scale[2] != 0.5f || goA.active != 0) return 15;

    // 4) Lease expiry restores: collect under a 1ms lease, let it lapse, tick restores.
    trA.scale[0] = 2.f; trA.scale[1] = -3.f; trA.scale[2] = 0.5f; goA.active = 0;
    api->update(1, 2500); api->tick(1);
    call_update(&chA, 1, method);
    if (g_roots != 3) return 16;
    api->update(1, 1);
    timespec pause{0, 10000000}; nanosleep(&pause, nullptr);
    api->tick(1);
    if (g_roots != 0 || trA.scale[0] != 2.f || goA.active != 0) return 17;

    // 5) A different challenge restores the previous one before snapshotting the new one.
    trA.scale[0] = 2.f; trA.scale[1] = -3.f; trA.scale[2] = 0.5f; goA.active = 0;
    GameObject goB{VEC, 1, 0}; Transform trB{VEC, 1, {4.f, 5.f, 6.f}};
    Collider colB{VEC, 1, &trB, &goB}; Challenge chB{MINI, 1, 1, &colB};
    api->update(1, 2500); api->tick(1);
    call_update(&chA, 1, method);
    if (g_roots != 3 || trA.scale[0] != 2000.f) return 18;
    call_update(&chB, 1, method);
    if (trA.scale[0] != 2.f || goA.active != 0) return 19;  // previous restored
    if (g_roots != 3) return 20;                            // only the new one is owned
    if (trB.scale[0] != 4000.f || trB.scale[1] != 5000.f || trB.scale[2] != 6000.f || goB.active != 1) return 21;

    // 6) Lifecycle restore happens BEFORE the original: the OnExit original observes the
    //    transform already restored to its original scale.
    g_watch = &trB; g_exit_observed = -1.f;
    call_exit(&chB, method);
    if (g_exit_orig != 1 || g_exit_observed != 4.f) return 22;
    if (g_roots != 0 || trB.scale[0] != 4.f || goB.active != 0) return 23;
    g_watch = nullptr;

    // 7) An invalid (out-of-range) scale aborts the snapshot and leaks no roots.
    trA.scale[0] = 20000.f; trA.scale[1] = 1.f; trA.scale[2] = 1.f; goA.active = 0;
    api->update(1, 2500); api->tick(1);
    call_update(&chA, 1, method);
    if (g_roots != 0 || trA.scale[0] != 20000.f) return 24; // untouched, nothing owned

    // 8) An invalid boxed activeSelf (neither 0 nor 1) likewise aborts without a leak.
    trA.scale[0] = 2.f; trA.scale[1] = -3.f; trA.scale[2] = 0.5f; goA.active = 2;
    call_update(&chA, 1, method);
    if (g_roots != 0 || trA.scale[0] != 2.f) return 25;
    goA.active = 0;

    // 9) Root failures at the first and a later acquisition both unwind cleanly.
    g_root_fail_on = 1; g_root_calls = 0;
    call_update(&chA, 1, method);
    if (g_roots != 0 || trA.scale[0] != 2.f) return 26;
    g_root_fail_on = 2; g_root_calls = 0;
    call_update(&chA, 1, method);
    if (g_roots != 0 || trA.scale[0] != 2.f) return 27;
    g_root_fail_on = 0;

    // 10) A managed SetActive that re-enters OnDestroy must not deadlock: the reentrant
    //     lifecycle hook defers (busy) and the restore is honoured on the next tick.
    trA.scale[0] = 2.f; trA.scale[1] = -3.f; trA.scale[2] = 0.5f; goA.active = 0;
    g_root_calls = 0; g_reenter = 1; g_reenter_challenge = &chA; g_reenter_method = method;
    call_update(&chA, 1, method);
    if (g_roots != 3 || trA.scale[0] != 2000.f || goA.active != 1) return 28; // collected
    if (g_destroy_orig < 1) return 29;                                        // reentry ran
    api->tick(1);
    if (g_roots != 0 || trA.scale[0] != 2.f || goA.active != 0) return 30;    // deferred restore

    std::puts("PASS native rocket minigame: real hooks/ABI, proven-tick gating, snapshot "
              "scale/active preserve+1000, no re-snapshot, disable/expiry/change restore, "
              "lifecycle-before-original, root/scale/bool failure containment, reentrant no-deadlock");
    return 0;
}

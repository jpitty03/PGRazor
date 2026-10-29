// Standalone Android (x86_64 host) test of the real native incubation module. Modelled on
// always_hit_test.cpp / adventure_sync_test.cpp: it dlopen()s the built core, pulls the
// pgrazor_native_api v2 table and asks query_feature(9, 1) for the incubation API (it does
// NOT invent any getter), then drives a compact fake IL2CPP through the install/tick/clear/
// sample ABI. The fake models the game's own FindObjectOfType / DiContainer.Resolve,
// Player.get_Incubators / IItemBag.get_AllPokemon, the GetEnumerator/MoveNext/get_Current/
// Dispose protocol (MoveNext returns a boxed System.Boolean), and PutEggInIncubator.
//
// Each scenario runs in a fresh fork so the module's process-lifetime g_st/install/telemetry
// start clean; only the expired-lease case sleeps (a few ms) to cross a real CLOCK_BOOTTIME
// deadline mid-enumeration. This harness is never packaged into the app.
//
// Coverage: shortest/longest stable-tie choice, exclusions (assigned/special/invalid
// distance/non-egg/non-unlimited), inventory confirmation, no resubmit of a pending request
// across off/on and stop/restart, managed-throw -> uncertain (request counted once, roots
// kept), an expired lease during enumeration (no Put), wrong-thread no-op, a reentrant clear
// that revokes submission and defers the root free, a partial root failure that still Disposes
// the enumerator, and GC-root balance throughout.
#include "incubation.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <pthread.h>

// ---- symbolic il2cpp identities (compared by address only) -----------------------
static int c_map, c_player_contract, c_player, c_bag_contract, c_bag, c_inc, c_pk,
           c_service_contract, c_service, c_boolean, c_string, c_enum, exc_obj;
static int f_inc_item, f_inc_id, f_inc_pokemon, f_inc_expiry,
           f_egg_is, f_egg_type, f_egg_assignment, f_egg_id, f_egg_target, f_egg_start;
static int m_find, m_container, m_resolve, m_incubators, m_eggs, m_put,
           m_enumerate, m_move, m_current, m_dispose;

struct Str  { void *klass; const uint16_t *chars; int32_t len; };
struct Inc  { void *klass; int32_t item; Str *id; uint64_t pokemon; uint64_t expiry; };
struct Egg  { void *klass; uint8_t is_egg; int32_t type; Str *assignment; uint64_t id; double target; double start; };
struct Coll { void *klass; void **rows; int count; };
struct Enum { void *klass; void **rows; int count; int idx; };
struct Box  { void *klass; uint8_t val; };

// ---- scenario knobs / observations (per-process; fork resets them) ---------------
static bool  g_no_map = false;
static void *g_throw_method = nullptr;
static bool  g_sleep_eggs = false;
static bool  g_reenter_clear = false;
static bool  g_reenter_disable = false;
static bool  g_expire_pending_root = false;
static void *g_fail_root_obj = nullptr;
static int   g_put_calls = 0;
static uint64_t g_put_egg = 0;
static void *g_put_inc = nullptr;
static int   g_roots = 0;        // live GC handles (must balance to 0 outside pending)
static int   g_disposed = 0;     // Dispose invocations observed
static const pgrazor_incubation_api_v1 *g_api = nullptr;

static Inc  *g_inc_rows[8];
static Egg  *g_egg_rows[16];
static Coll  g_inc_coll{&c_enum, nullptr, 0};
static Coll  g_egg_coll{&c_enum, nullptr, 0};
static int   g_map_obj = 0, g_container_obj = 0;
static Inc   g_player{&c_player, 0, nullptr, 0, 0};
static Inc   g_bag{&c_bag, 0, nullptr, 0, 0};
static Inc   g_service{&c_service, 0, nullptr, 0, 0};
static Box   g_promise{&c_pk, 1};

// ---- fake il2cpp primitives ------------------------------------------------------
static void *obj_class(void *o) { return *static_cast<void **>(o); }
static void *gc_new(void *o, int) {
    if (!o || o == g_fail_root_obj) return nullptr;
    if (g_expire_pending_root && o == &g_service) {
        static int service_roots = 0;
        if (++service_roots == 2) usleep(10000);
    }
    ++g_roots; return o;
}
static void *gc_target(void *h) { return h; }
static void  gc_free(void *h) { if (h) --g_roots; }
static void *object_unbox(void *o) { return &static_cast<Box *>(o)->val; }
static int32_t  string_length(void *o) { return static_cast<Str *>(o)->len; }
static uint16_t *string_chars(void *o) { return const_cast<uint16_t *>(static_cast<Str *>(o)->chars); }
static void *class_get_type(void *k) { return k; }   // identity: Type object == class pointer
static void *type_get_object(void *t) { return t; }
static void *object_get_virtual_method(void *o, void *m) { (void)o; return m; } // vtable slot == method

static void field_get(void *o, void *f, void *out) {
    if      (f == &f_inc_item)       *static_cast<int32_t *>(out)  = static_cast<Inc *>(o)->item;
    else if (f == &f_inc_id)         *static_cast<void **>(out)    = static_cast<Inc *>(o)->id;
    else if (f == &f_inc_pokemon)    *static_cast<uint64_t *>(out) = static_cast<Inc *>(o)->pokemon;
    else if (f == &f_inc_expiry)     *static_cast<uint64_t *>(out) = static_cast<Inc *>(o)->expiry;
    else if (f == &f_egg_is)         *static_cast<uint8_t *>(out)  = static_cast<Egg *>(o)->is_egg;
    else if (f == &f_egg_type)       *static_cast<int32_t *>(out)  = static_cast<Egg *>(o)->type;
    else if (f == &f_egg_assignment) *static_cast<void **>(out)    = static_cast<Egg *>(o)->assignment;
    else if (f == &f_egg_id)         *static_cast<uint64_t *>(out) = static_cast<Egg *>(o)->id;
    else if (f == &f_egg_target)     *static_cast<double *>(out)   = static_cast<Egg *>(o)->target;
    else if (f == &f_egg_start)      *static_cast<double *>(out)   = static_cast<Egg *>(o)->start;
}

static void *runtime_invoke(void *method, void *obj, void **params, void **exc) {
    asm volatile("nop; nop; nop; nop" ::: "memory");
    *exc = (method == g_throw_method) ? &exc_obj : nullptr;
    if (method == &m_find)       return (*exc || g_no_map) ? nullptr : &g_map_obj;
    if (method == &m_container)  return *exc ? nullptr : &g_container_obj;
    if (method == &m_resolve) {
        if (*exc) return nullptr;
        void *t = params ? params[0] : nullptr;
        if (t == &c_player_contract)  return &g_player;
        if (t == &c_bag_contract)     return &g_bag;
        if (t == &c_service_contract) return &g_service;
        return nullptr;
    }
    if (method == &m_incubators) return *exc ? nullptr : &g_inc_coll;
    if (method == &m_eggs)       return *exc ? nullptr : &g_egg_coll;
    if (method == &m_put) {
        ++g_put_calls;
        g_put_egg = params ? *static_cast<uint64_t *>(params[0]) : 0;
        g_put_inc = params ? params[1] : nullptr;
        return *exc ? nullptr : &g_promise;
    }
    if (method == &m_enumerate) {
        static Enum e; Coll *c = static_cast<Coll *>(obj);
        e.klass = &c_enum; e.rows = c->rows; e.count = c->count; e.idx = -1;
        return &e;
    }
    if (method == &m_move) {
        static Box b; Enum *e = static_cast<Enum *>(obj);
        ++e->idx; b.klass = &c_boolean; b.val = (e->idx < e->count) ? 1 : 0;
        return &b;
    }
    if (method == &m_current) {
        Enum *e = static_cast<Enum *>(obj);
        void *row = e->rows[e->idx];
        if (obj_class(row) == &c_pk) {
            if (g_sleep_eggs) { timespec t{0, 12000000}; nanosleep(&t, nullptr); g_sleep_eggs = false; }
            static bool reentered = false;
            if (g_reenter_clear && !reentered) { reentered = true; g_api->clear(static_cast<uint64_t>(gettid())); }
            if (g_reenter_disable && !reentered) { reentered = true; g_api->tick(0, 0, 2500, gettid()); }
        }
        return row;
    }
    if (method == &m_dispose) { ++g_disposed; return nullptr; }
    return nullptr;
}

// ---- config + helpers ------------------------------------------------------------
static int install(const pgrazor_incubation_api_v1 *api) {
    pgrazor_incubation_config_v1 c{};
    c.version = PGRAZOR_INCUBATION_API_VERSION;
    c.size = sizeof(c);
    c.runtime_invoke = (void *)&runtime_invoke;
    c.gchandle_new = (void *)&gc_new;
    c.gchandle_get_target = (void *)&gc_target;
    c.gchandle_free = (void *)&gc_free;
    c.object_get_class = (void *)&obj_class;
    c.object_unbox = (void *)&object_unbox;
    c.string_length = (void *)&string_length;
    c.string_chars = (void *)&string_chars;
    c.object_get_virtual_method = (void *)&object_get_virtual_method;
    c.class_get_type = (void *)&class_get_type;
    c.type_get_object = (void *)&type_get_object;
    c.field_get_value = (void *)&field_get;
    c.m_find = &m_find; c.m_container = &m_container; c.m_resolve = &m_resolve;
    c.m_incubators = &m_incubators; c.m_eggs = &m_eggs; c.m_put = &m_put;
    c.m_enumerate = &m_enumerate; c.m_move = &m_move; c.m_current = &m_current; c.m_dispose = &m_dispose;
    c.class_map = &c_map; c.class_player_contract = &c_player_contract; c.class_player = &c_player;
    c.class_bag_contract = &c_bag_contract; c.class_bag = &c_bag; c.class_inc = &c_inc; c.class_pk = &c_pk;
    c.class_service_contract = &c_service_contract; c.class_service = &c_service;
    c.f_inc_item = &f_inc_item; c.f_inc_id = &f_inc_id; c.f_inc_pokemon = &f_inc_pokemon; c.f_inc_expiry = &f_inc_expiry;
    c.f_egg_is = &f_egg_is; c.f_egg_type = &f_egg_type; c.f_egg_assignment = &f_egg_assignment;
    c.f_egg_id = &f_egg_id; c.f_egg_target = &f_egg_target; c.f_egg_start = &f_egg_start;
    c.unlimited = 901; c.special = 2;
    c.class_boolean = &c_boolean; c.class_string = &c_string;
    return api->install(&c);
}

static Str g_empty{&c_string, nullptr, 0}; // len 0: chars never dereferenced
static Str *mkstr(const uint16_t *lit, int len) {
    static Str pool[8]; static int n = 0;
    Str *s = &pool[n++ % 8]; s->klass = &c_string; s->chars = lit; s->len = len; return s;
}
static uint32_t sample_status(const pgrazor_incubation_api_v1 *api) {
    pgrazor_incubation_sample_v1 s{}; api->sample(&s); return s.status;
}

// Drive one fresh orchestration pass: toggle off then on so next_ms resets to 0.
static void repoll(uint64_t tid) { g_api->tick(0, 0, 2500, tid); }

// ---- scenarios -------------------------------------------------------------------
// Returns 0 on success; a small nonzero code pinpoints the failing check.
static int scenario(int which) {
    const uint64_t tid = static_cast<uint64_t>(gettid());
    g_inc_coll.rows = reinterpret_cast<void **>(g_inc_rows);
    g_egg_coll.rows = reinterpret_cast<void **>(g_egg_rows);
    if (install(g_api) != PGRAZOR_INC_OK) return 90;

    static const uint16_t U[] = {'U'};
    Inc inc_unl{&c_inc, 901, mkstr(U, 1), 0, 0};        // free unlimited incubator
    Inc inc_lim{&c_inc, 902, mkstr(U, 1), 0, 0};        // non-unlimited (never chosen as free)
    g_inc_rows[0] = &inc_unl; g_inc_coll.count = 1;

    switch (which) {
    case 0: { // shortest + stable tie + Put once + STARTING + owned roots held
        Egg a{&c_pk, 1, 0, &g_empty, 11, 2, 0};         // 2 km (first)
        Egg b{&c_pk, 1, 0, &g_empty, 12, 5, 0};         // 5 km
        Egg c{&c_pk, 1, 0, &g_empty, 13, 2, 0};         // 2 km (tie; must NOT win)
        g_egg_rows[0] = &a; g_egg_rows[1] = &b; g_egg_rows[2] = &c; g_egg_coll.count = 3;
        g_api->tick(1, 0, 2500, tid);
        if (g_put_calls != 1 || g_put_egg != 11) return 1;      // shortest, first of the tie
        if (sample_status(g_api) != PGRAZOR_INC_STATUS_STARTING) return 2;
        pgrazor_incubation_sample_v1 s{}; g_api->sample(&s);
        if (s.request_serial != 1 || s.owned_roots == 0 || s.distance != 2) return 3;
        if (g_roots != s.owned_roots) return 4;                 // only durable roots remain
        return 0;
    }
    case 1: { // longest
        Egg a{&c_pk, 1, 0, &g_empty, 11, 2, 0};
        Egg b{&c_pk, 1, 0, &g_empty, 12, 5, 0};
        g_egg_rows[0] = &a; g_egg_rows[1] = &b; g_egg_coll.count = 2;
        g_api->tick(1, 1, 2500, tid);
        if (g_put_calls != 1 || g_put_egg != 12) return 1;
        return 0;
    }
    case 2: { // exclusions: assigned / special / invalid-distance / non-egg leave one eligible
        static const uint16_t M[] = {'m'};
        Egg assigned{&c_pk, 1, 0, mkstr(M, 1), 21, 3, 0};       // already assigned
        Egg special {&c_pk, 1, 2, &g_empty, 22, 3, 0};          // special egg type (excluded)
        Egg badkm   {&c_pk, 1, 0, &g_empty, 23, 0, 0};          // 0 km (invalid)
        Egg notegg  {&c_pk, 0, 0, &g_empty, 24, 4, 0};          // not an egg
        Egg ok      {&c_pk, 1, 0, &g_empty, 25, 7, 0};          // only eligible
        g_egg_rows[0] = &assigned; g_egg_rows[1] = &special; g_egg_rows[2] = &badkm;
        g_egg_rows[3] = &notegg;   g_egg_rows[4] = &ok;          g_egg_coll.count = 5;
        g_inc_rows[0] = &inc_lim; g_inc_rows[1] = &inc_unl; g_inc_coll.count = 2; // non-unl ignored
        g_api->tick(1, 0, 2500, tid);
        if (g_put_calls != 1 || g_put_egg != 25) return 1;
        return 0;
    }
    case 3: { // inventory confirmation: no resubmit, EGG_INCUBATING, roots released
        Egg a{&c_pk, 1, 0, &g_empty, 11, 2, 0};
        g_egg_rows[0] = &a; g_egg_coll.count = 1;
        g_api->tick(1, 0, 2500, tid);
        if (g_put_calls != 1) return 1;
        inc_unl.pokemon = 11;                                   // inventory now shows our egg
        repoll(tid); g_api->tick(1, 0, 2500, tid);
        if (g_put_calls != 1) return 2;                         // never resubmit
        if (sample_status(g_api) != PGRAZOR_INC_STATUS_EGG_INCUBATING) return 3;
        pgrazor_incubation_sample_v1 s{}; g_api->sample(&s);
        if (s.owned_roots != 0 || g_roots != 0) return 4;       // confirmed -> roots dropped
        return 0;
    }
    case 4: { // pending not yet confirmed: off/on must not resubmit
        Egg a{&c_pk, 1, 0, &g_empty, 11, 2, 0};
        g_egg_rows[0] = &a; g_egg_coll.count = 1;
        g_api->tick(1, 0, 2500, tid);
        if (g_put_calls != 1) return 1;
        repoll(tid); g_api->tick(1, 0, 2500, tid);              // still pokemon==0
        if (g_put_calls != 1) return 2;
        if (sample_status(g_api) != PGRAZOR_INC_STATUS_CONFIRMING) return 3;
        return 0;
    }
    case 5: { // stop/restart: clear frees roots but keeps the uncertain pending IDs
        Egg a{&c_pk, 1, 0, &g_empty, 11, 2, 0};
        g_egg_rows[0] = &a; g_egg_coll.count = 1;
        g_api->tick(1, 0, 2500, tid);
        if (g_put_calls != 1) return 1;
        g_api->clear(tid);                                      // stop
        pgrazor_incubation_sample_v1 s{}; g_api->sample(&s);
        if (s.owned_roots != 0 || g_roots != 0) return 2;       // roots released on stop
        repoll(tid); g_api->tick(1, 0, 2500, tid);              // restart
        if (g_put_calls != 1) return 3;                         // pending preserved -> no resubmit
        if (sample_status(g_api) != PGRAZOR_INC_STATUS_CONFIRMING) return 4;
        return 0;
    }
    case 6: { // managed throw on Put -> uncertain, request counted once, roots kept
        Egg a{&c_pk, 1, 0, &g_empty, 11, 2, 0};
        g_egg_rows[0] = &a; g_egg_coll.count = 1;
        g_throw_method = &m_put;
        g_api->tick(1, 0, 2500, tid);
        pgrazor_incubation_sample_v1 s{}; g_api->sample(&s);
        if (s.request_serial != 1) return 1;                    // issued once, even though it threw
        if (s.status != PGRAZOR_INC_STATUS_UNCONFIRMED) return 2;
        if (s.fault_serial == 0 || s.owned_roots == 0) return 3; // fault noted, roots retained
        repoll(tid); g_api->tick(1, 0, 2500, tid);
        g_api->sample(&s);
        if (s.request_serial != 1) return 4;                    // never auto-duplicates
        return 0;
    }
    case 7: { // lease expires during enumeration -> no Put
        Egg a{&c_pk, 1, 0, &g_empty, 11, 2, 0};
        g_egg_rows[0] = &a; g_egg_coll.count = 1;
        g_sleep_eggs = true;
        g_api->tick(1, 0, 1, tid);                              // 1 ms residual lease
        if (g_put_calls != 0) return 1;
        if (g_roots != 0) return 2;                             // all transient roots balanced
        return 0;
    }
    case 8: { // wrong thread: complete no-op
        Egg a{&c_pk, 1, 0, &g_empty, 11, 2, 0};
        g_egg_rows[0] = &a; g_egg_coll.count = 1;
        g_api->tick(1, 0, 2500, tid + 1);
        if (g_put_calls != 0 || g_roots != 0) return 1;
        if (sample_status(g_api) != PGRAZOR_INC_STATUS_NONE) return 2;
        return 0;
    }
    case 9: { // reentrant clear during the pass revokes submission, defers the free
        Egg a{&c_pk, 1, 0, &g_empty, 11, 2, 0};
        g_egg_rows[0] = &a; g_egg_coll.count = 1;
        g_reenter_clear = true;
        g_api->tick(1, 0, 2500, tid);
        if (g_put_calls != 0) return 1;                         // submission revoked
        pgrazor_incubation_sample_v1 s{}; g_api->sample(&s);
        if (s.owned_roots != 0 || g_roots != 0) return 2;       // deferred free completed
        return 0;
    }
    case 10: { // partial root failure during enumeration still Disposes, roots balance
        Egg a{&c_pk, 1, 0, &g_empty, 11, 2, 0};
        g_egg_rows[0] = &a; g_egg_coll.count = 1;
        g_fail_root_obj = &a;                                   // rooting this egg element fails
        g_api->tick(1, 0, 2500, tid);
        if (g_put_calls != 0) return 1;                         // fail closed before any Put
        if (g_disposed < 2) return 2;                           // both enumerators Disposed
        if (g_roots != 0) return 3;                             // no leaked handle
        return 0;
    }
    case 11: { // no map yet: soft no-op, not an error
        g_no_map = true;
        g_api->tick(1, 0, 2500, tid);
        if (g_put_calls != 0 || g_roots != 0) return 1;
        if (sample_status(g_api) != PGRAZOR_INC_STATUS_NONE) return 2;
        return 0;
    }
    case 12: { // an idle owner cannot be replaced by a caller providing its own real tid
        Egg a{&c_pk, 1, 0, &g_empty, 11, 2, 0};
        g_egg_rows[0] = &a; g_egg_coll.count = 1;
        g_api->tick(1, 0, 2500, tid);
        const int roots = g_roots;
        pthread_t thread;
        if (pthread_create(&thread, nullptr, [](void *) -> void * {
            g_api->clear(gettid()); g_api->tick(0, 0, 2500, gettid()); return nullptr;
        }, nullptr) != 0) return 1;
        pthread_join(thread, nullptr);
        if (roots == 0 || g_roots != roots) return 2;
        g_api->clear(tid);
        return g_roots == 0 ? 0 : 3;
    }
    case 13: { // disable arriving inside enumeration must prevent submission
        Egg a{&c_pk, 1, 0, &g_empty, 11, 2, 0};
        g_egg_rows[0] = &a; g_egg_coll.count = 1;
        g_reenter_disable = true;
        g_api->tick(1, 0, 2500, tid);
        return g_put_calls == 0 && g_roots == 0 ? 0 : 1;
    }
    case 14: { // authorization expires while acquiring durable request roots
        Egg a{&c_pk, 1, 0, &g_empty, 11, 2, 0};
        g_egg_rows[0] = &a; g_egg_coll.count = 1;
        g_expire_pending_root = true;
        g_api->tick(1, 0, 5, tid);
        return g_put_calls == 0 && g_roots == 0 ? 0 : 1;
    }
    default: return 0;
    }
}

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::puts(dlerror()); return 2; }
    auto getter = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib, "pgrazor_native_api"));
    auto core = getter ? static_cast<const pgrazor_native_api_v2 *>(getter(2)) : nullptr;
    g_api = core ? static_cast<const pgrazor_incubation_api_v1 *>(core->query_feature(9, 1)) : nullptr;
    if (!g_api || g_api->size != 40) return 3;   // feature 9 must be wired into query_feature

    const int cases = 15;
    for (int i = 0; i < cases; ++i) {
        pid_t pid = fork();
        if (pid == 0) _exit(scenario(i));         // fresh process -> clean module state
        int status = 0; waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::printf("FAIL incubation scenario %d (code %d)\n", i,
                        WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            return 4;
        }
    }
    std::puts("PASS native incubation: shortest/longest stable tie, exclusions, inventory "
              "confirmation, no resubmit on off/on or stop/restart, managed-throw uncertain, "
              "expired lease mid-enumeration (no Put), wrong-thread no-op, reentrant-clear "
              "revoke+deferred free, partial root failure + Dispose, root balance");
    return 0;
}

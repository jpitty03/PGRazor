// Standalone Android (x86_64 host) test of the real native buddy-feeding module. Modelled
// on quick_spin_test.cpp (real Dobby hooks against NOP-padded targets) and incubation_test.cpp
// (fork-per-scenario for clean process-lifetime state). It dlopen()s the built core, pulls the
// pgrazor_native_api v2 table and asks query_feature(12, 1) for the buddy API (it invents no
// getter), installs against real feed/result targets, and drives a compact fake IL2CPP that
// models FindObjectOfType / DiContainer.Resolve, the BuddyStatsService -> rpc/pokemon fields,
// get_HasBuddy / get_BuddyPokemon / get_BuddyStats, the static hunger getters, GetItemCount,
// and Feed. The fake Feed re-enters the hooked feed target (so our own feed is suppressed as
// non-manual) and can synchronously deliver a result through the hooked result target.
//
// Coverage (the task's minimum set): hunger re-arm across a fed/hungry cycle, stock 0 -> no
// feed, a manual feed marking a pending request ambiguous, a synchronous success result, a
// non-success result (no retry), an in-flight request that is never duplicated, a lease that
// expires inside a getter -> no feed, an off-thread clear that frees nothing, and a stop/clear
// that balances the durable roots while preserving the uncertain pending by value. The single
// long-lived service root is intentionally retained until clear(), so a "balanced" idle state
// here is owned_roots == 1 (service) outside a pending request, and 0 only after clear().
// This harness is never packaged into the app.
#include "buddy_feeding.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <ctime>
#include <cstdio>
#include <cstdint>
#include <cstring>

// ---- symbolic il2cpp identities (compared by address only) -----------------------
static int c_map, c_ctx, c_contract, c_stats, c_pokesvc, c_rpc, c_proto, c_observed,
           c_bag, c_bool, c_int, c_single, c_callback, c_outproto;
static int f_rpc, f_pokemon, f_bag, f_cbrpc, f_result, f_buddyid;
static int m_find, m_container, m_resolve, m_has, m_pokemon, m_stats, m_remaining,
           m_fullness, m_cooldown, m_quantity, m_feed, m_alive, m_result_cb;

struct Obj      { void *klass; };
struct Rpc      { void *klass; void *bag; };
struct Stats    { void *klass; void *rpc; void *pokemon; };
struct Proto    { void *klass; uint64_t buddy_id; };
struct Callback { void *klass; void *rpc; };
struct OutProto { void *klass; int32_t result; };
struct Box      { void *klass; union { uint8_t b; int32_t i; float f; } v; };

// ---- scenario knobs / observations (per-process; fork resets them) ---------------
static bool    g_has = true, g_cooldown = false, g_sync_result = false, g_slow_quantity = false;
static bool    g_missing_stats = false, g_stats_exception = false;
static int g_quantity_action=0, g_rpc_roots=0;
static bool g_clear_rpc_root=false, g_fail_promise=false, g_bad_free=false;
static int32_t g_count = 5, g_result_val = 1;
static float   g_remaining = 0.0f, g_fullness = 0.1f;
static int     g_roots = 0, g_feed_target_calls = 0, g_result_target_calls = 0;
static const pgrazor_buddy_api_v1 *g_api = nullptr;

static Obj      g_map{&c_map}, g_container{&c_ctx}, g_pokesvc{&c_pokesvc}, g_bag{&c_bag};
static Rpc      g_rpc{&c_rpc, &g_bag};
static Stats    g_stats{&c_stats, &g_rpc, &g_pokesvc};
static Proto    g_proto{&c_proto, 0x1234};
static Obj      g_observed{&c_observed};
static Callback g_callback{&c_callback, &g_rpc};
static OutProto g_outproto{&c_outproto, 1};
static Box      g_promise{&c_outproto, {1}};
static Box      g_box;  // reused scratch box per invoke (single-threaded fake)

// ---- fake il2cpp primitives ------------------------------------------------------
static void *obj_class(void *o) { return o ? *static_cast<void **>(o) : nullptr; }
static void *gc_new(void *o, int) {
    if(!o || (g_fail_promise && o==&g_promise)) return nullptr;
    ++g_roots;
    if(o==&g_rpc && ++g_rpc_roots==2 && g_clear_rpc_root) g_api->clear(gettid());
    return o;
}
static void *gc_target(void *h) { return h; }
static void gc_free(void *h) {
    if(h) {pgrazor_buddy_sample_v1 v{}; g_api->sample(&v); if(!v.owned_roots) g_bad_free=true; --g_roots;}
}
static void *object_unbox(void *o) { return &static_cast<Box *>(o)->v; }
static void *class_get_type(void *k) { return k; }
static void *type_get_object(void *t) { return t; }

static void field_get(void *o, void *f, void *out) {
    if      (f == &f_rpc)     *static_cast<void **>(out)    = static_cast<Stats *>(o)->rpc;
    else if (f == &f_pokemon) *static_cast<void **>(out)    = static_cast<Stats *>(o)->pokemon;
    else if (f == &f_bag)     *static_cast<void **>(out)    = static_cast<Rpc *>(o)->bag;
    else if (f == &f_cbrpc)   *static_cast<void **>(out)    = static_cast<Callback *>(o)->rpc;
    else if (f == &f_result)  *static_cast<int32_t *>(out)  = static_cast<OutProto *>(o)->result;
    else if (f == &f_buddyid) *static_cast<uint64_t *>(out) = static_cast<Proto *>(o)->buddy_id;
}

// Real hooked targets (NOP-padded so Dobby has room). call_* go through the trampoline.
#define PGR_NOP8() asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;")
__attribute__((noinline)) void *target_feed(void *self, int32_t item, int32_t count, void *method) {
    PGR_NOP8(); ++g_feed_target_calls; (void)self; (void)item; (void)count; (void)method; return &g_promise;
}
__attribute__((noinline)) void target_result(void *self, void *proto, void *method) {
    PGR_NOP8(); ++g_result_target_calls; (void)self; (void)proto; (void)method;
}
void *(*volatile call_feed)(void *, int32_t, int32_t, void *) = &target_feed;
void  (*volatile call_result)(void *, void *, void *) = &target_result;

static void *runtime_invoke(void *method, void *obj, void **params, void **exc) {
    asm volatile("nop; nop; nop; nop" ::: "memory");
    *exc = nullptr;
    const bool stat=method==&m_find || method==&m_alive || method==&m_remaining || method==&m_fullness || method==&m_cooldown;
    if((stat && obj) || (!stat && !obj)) _exit(80);
    if(method==&m_quantity && g_quantity_action){
        int action=g_quantity_action; g_quantity_action=0;
        if(action==1) g_api->clear(gettid()); else g_api->tick(0,0,gettid());
    }
    if (method == &m_find)      return &g_map;
    if (method == &m_container) return &g_container;
    if (method == &m_resolve) {
        void *t = params ? params[0] : nullptr;
        return (t == &c_contract) ? static_cast<void *>(&g_stats) : nullptr;
    }
    if (method == &m_has)      { g_box.klass = &c_bool;   g_box.v.b = g_has ? 1 : 0;      return &g_box; }
    if (method == &m_cooldown) { g_box.klass = &c_bool;   g_box.v.b = g_cooldown ? 1 : 0; return &g_box; }
    if (method == &m_alive)    { g_box.klass = &c_bool;   g_box.v.b = 1;                  return &g_box; }
    if (method == &m_remaining){ g_box.klass = &c_single; g_box.v.f = g_remaining;        return &g_box; }
    if (method == &m_fullness) { g_box.klass = &c_single; g_box.v.f = g_fullness;         return &g_box; }
    if (method == &m_quantity) { if (g_slow_quantity) { timespec t{0, 10000000}; nanosleep(&t, nullptr); }
                                 g_box.klass = &c_int; g_box.v.i = g_count;               return &g_box; }
    if (method == &m_pokemon)  return &g_proto;
    if (method == &m_stats) {
        if (g_stats_exception) { *exc = &g_observed; return nullptr; }
        return g_missing_stats ? nullptr : &g_observed;
    }
    if (method == &m_feed) {
        int32_t item = params ? *static_cast<int32_t *>(params[0]) : 0;
        int32_t count = params ? *static_cast<int32_t *>(params[1]) : 0;
        void *pr = call_feed(obj, item, count, &m_feed);       // re-enter the hooked feed target
        if (g_sync_result) { g_outproto.result = g_result_val; call_result(&g_callback, &g_outproto, &m_result_cb); }
        return pr;
    }
    return nullptr;
}

// ---- config + helpers ------------------------------------------------------------
static pgrazor_buddy_config_v1 make_config() {
    pgrazor_buddy_config_v1 c{};
    c.version = PGRAZOR_BUDDY_API_VERSION;
    c.size = sizeof(c);
    c.runtime_invoke = (void *)&runtime_invoke;
    c.gchandle_new = (void *)&gc_new;
    c.gchandle_get_target = (void *)&gc_target;
    c.gchandle_free = (void *)&gc_free;
    c.object_get_class = (void *)&obj_class;
    c.object_unbox = (void *)&object_unbox;
    c.class_get_type = (void *)&class_get_type;
    c.type_get_object = (void *)&type_get_object;
    c.field_get_value = (void *)&field_get;
    c.m_find = &m_find; c.m_container = &m_container; c.m_resolve = &m_resolve;
    c.m_has = &m_has; c.m_pokemon = &m_pokemon; c.m_stats = &m_stats;
    c.m_remaining = &m_remaining; c.m_fullness = &m_fullness; c.m_cooldown = &m_cooldown;
    c.m_quantity = &m_quantity; c.m_feed = &m_feed; c.m_alive = &m_alive;
    c.feed_address = (void *)&target_feed; c.result_address = (void *)&target_result;
    c.class_map = &c_map; c.class_contract = &c_contract; c.class_stats_service = &c_stats;
    c.class_pokemon_service = &c_pokesvc; c.class_rpc = &c_rpc; c.class_buddy_proto = &c_proto;
    c.class_observed = &c_observed; c.class_bag = &c_bag; c.class_bool = &c_bool;
    c.class_int = &c_int; c.class_single = &c_single; c.class_callback = &c_callback;
    c.class_out_proto = &c_outproto;
    c.f_rpc = &f_rpc; c.f_pokemon = &f_pokemon; c.f_bag = &f_bag;
    c.f_callback_rpc = &f_cbrpc; c.f_result = &f_result; c.f_buddy_id = &f_buddyid;
    c.success = 1; c.reserved = 0;
    return c;
}

static pgrazor_buddy_sample_v1 snap() {
    pgrazor_buddy_sample_v1 s{}; g_api->sample(&s); return s;
}

// Force a fresh poll cycle when no request is pending: toggling the berry off/on clears
// next_ms (and, as in JS, the per-cycle blocked/feeds/progress). A pending request
// short-circuits before this gate, so it is unaffected.
static void repoll(uint64_t tid) { g_api->tick(0, 2500, tid); }

// ---- scenarios -------------------------------------------------------------------
static int scenario(int which) {
    const uint64_t tid = static_cast<uint64_t>(gettid());
    pgrazor_buddy_config_v1 cfg = make_config();
    if (g_api->install(&cfg) != PGR_BUDDY_OK) return 90;

    switch (which) {
    case 0: { // happy sync result -> BERRY_FED, result consumed once, roots balanced
        g_remaining = 0; g_fullness = 0.1f; g_count = 5; g_sync_result = true; g_result_val = 1;
        g_api->tick(701, 2500, tid);                       // issue + synchronous capture
        pgrazor_buddy_sample_v1 s = snap();
        if (s.feed_request_serial != 1 || g_feed_target_calls != 1) return 1;
        g_api->tick(701, 2500, tid);                       // section-1 consume (no repoll needed)
        s = snap();
        if (s.result_serial != 1) return 2;                // consumed exactly once
        if (s.status != PGR_BUDDY_STATUS_BERRY_FED) return 3;
        if (s.owned_roots != 1 || g_roots != 1) return 4;  // pending dropped; only the service root
        return 0;
    }
    case 1: { // hunger re-arm: fed (remaining>0) resets, then hungry feeds again
        g_remaining = 0; g_sync_result = true; g_result_val = 1;
        g_api->tick(701, 2500, tid);                       // issue + capture
        g_api->tick(701, 2500, tid);                       // consume -> BERRY_FED, feeds=1
        g_remaining = 300;                                 // buddy now fed
        repoll(tid); g_api->tick(701, 2500, tid);          // re-poll -> FED, re-armed
        if (snap().status != PGR_BUDDY_STATUS_FED) return 1;
        g_remaining = 0;                                   // hungry again
        repoll(tid); g_api->tick(701, 2500, tid);          // issue a second feed + capture
        if (snap().feed_request_serial != 2) return 2;     // re-armed and fed again
        return 0;
    }
    case 2: { // stock 0 -> no feed, OUT_OF_STOCK, only the durable service root
        g_remaining = 0; g_count = 0;
        g_api->tick(701, 2500, tid);
        pgrazor_buddy_sample_v1 s = snap();
        if (s.feed_request_serial != 0 || g_feed_target_calls != 0) return 1;
        if (s.status != PGR_BUDDY_STATUS_OUT_OF_STOCK) return 2;
        if (s.owned_roots != 1 || g_roots != 1) return 3;
        return 0;
    }
    case 3: { // manual feed while pending -> ambiguous: a later result is NOT attributed
        g_remaining = 0; g_count = 5; g_sync_result = false;
        g_api->tick(701, 2500, tid);                       // issue our feed (async)
        if (snap().feed_request_serial != 1) return 1;
        call_feed(&g_rpc, 701, 1, &m_feed);                // the player's own manual feed
        g_outproto.result = 1; call_result(&g_callback, &g_outproto, &m_result_cb); // result for our rpc
        g_api->tick(701, 2500, tid);
        if (snap().result_serial != 0) return 2;           // ambiguous -> never consumed as ours
        if (snap().feed_request_serial != 1) return 3;     // and never resubmitted
        return 0;
    }
    case 4: { // non-success result -> FEED_FAILED + blocked, no immediate retry
        g_remaining = 0; g_count = 5; g_sync_result = true; g_result_val = 7; // not success(1)
        g_api->tick(701, 2500, tid);
        g_api->tick(701, 2500, tid);                       // consume the failure
        pgrazor_buddy_sample_v1 s = snap();
        if (s.status != PGR_BUDDY_STATUS_FEED_FAILED || s.detail != 7) return 1;
        const uint64_t issued = s.feed_request_serial;
        g_api->tick(701, 2500, tid);                       // poll-gated / blocked -> no retry
        if (snap().feed_request_serial != issued) return 2;
        if (snap().owned_roots != 1 || g_roots != 1) return 3;
        return 0;
    }
    case 5: { // lease expires inside a getter -> no feed, roots balance to the service only
        g_remaining = 0; g_count = 5; g_slow_quantity = true;
        g_api->tick(701, 1, tid);                          // 1 ms residual lease
        pgrazor_buddy_sample_v1 s = snap();
        if (s.feed_request_serial != 0 || g_feed_target_calls != 0) return 1;
        if (s.owned_roots != 1 || g_roots != 1) return 2;  // pending dropped before the Feed
        return 0;
    }
    case 6: { // in-flight request (async, no result) is never resubmitted while pending
        g_remaining = 0; g_count = 5; g_sync_result = false;
        g_api->tick(701, 2500, tid);
        if (snap().feed_request_serial != 1) return 1;
        g_api->tick(701, 2500, tid);                       // still pending, no result
        if (snap().feed_request_serial != 1) return 2;     // no automatic duplicate
        return 0;
    }
    case 7: { // off-thread clear frees nothing; owner clear balances + preserves pending
        g_remaining = 0; g_count = 5; g_sync_result = false;
        g_api->tick(701, 2500, tid);
        const int before = g_roots;                        // service + pending rpc (+ promise)
        if (before == 0) return 1;
        pthread_t th;
        if (pthread_create(&th, nullptr, [](void *) -> void * {
            g_api->clear(static_cast<uint64_t>(gettid())); return nullptr; }, nullptr) != 0) return 2;
        pthread_join(th, nullptr);
        if (g_roots != before) return 3;                   // off-thread clear freed nothing
        g_api->clear(tid);                                 // owner clear
        if (g_roots != 0 || snap().owned_roots != 0) return 4; // durable roots balanced
        g_api->tick(701, 2500, tid);
        if (snap().feed_request_serial != 1) return 5;     // pending preserved by value -> no resubmit
        return 0;
    }
    case 8: { // wrong thread: complete no-op (no latch, no resolve, no feed)
        g_remaining = 0; g_count = 5;
        g_api->tick(701, 2500, tid + 1);
        if (snap().feed_request_serial != 0 || g_roots != 0) return 1;
        if (snap().status != PGR_BUDDY_STATUS_NONE) return 2;
        return 0;
    }
    case 9: case 10: case 11: {
        g_quantity_action=which==9?1:which==10?2:0;
        g_clear_rpc_root=which==11;
        g_api->tick(701,2500,tid);
        if(snap().feed_request_serial || g_feed_target_calls || g_bad_free) return 1;
        g_api->clear(tid); if(g_roots || snap().owned_roots) return 2;
        return 0;
    }
    case 12: {
        g_remaining=120; g_api->tick(701,2500,tid);
        pthread_t th;
        pthread_create(&th,nullptr,[](void*)->void* {g_api->tick(701,2500,gettid());g_api->clear(gettid());return nullptr;},nullptr);
        pthread_join(th,nullptr);
        if(snap().feed_request_serial || g_roots!=1) return 1;
        auto changed=make_config(); changed.success=7;
        if(g_api->install(&changed)!=PGR_BUDDY_ERR_ALREADY_BOUND) return 2;
        g_api->clear(tid); return g_roots||g_bad_free?3:0;
    }
    case 13: {
        g_fail_promise=true; g_api->tick(701,2500,tid);
        if(snap().feed_request_serial!=1 || !snap().fault_serial) return 1;
        g_api->tick(701,2500,tid); if(snap().feed_request_serial!=1) return 2;
        g_api->clear(tid); return g_roots||g_bad_free?3:0;
    }
    case 14: { // Buddy identity arrives before hunger; recover without toggling Off.
        g_missing_stats = true;
        g_api->tick(701,2500,tid);
        auto waiting = snap();
        if (waiting.status != PGR_BUDDY_STATUS_WAIT_HUNGER || waiting.fault_serial) return 1;
        if (waiting.feed_request_serial || g_feed_target_calls || g_roots != 1) return 2;
        g_missing_stats = false;
        g_sync_result = true;
        // Use the real production poll clock and preserve the same buddy ID and
        // berry setting. An OFF/ON repoll would hide the blocked-state regression.
        timespec delay{5,100000000};
        if (nanosleep(&delay,nullptr) != 0) return 3;
        g_api->tick(701,2500,tid);
        if (snap().feed_request_serial != 1 || g_feed_target_calls != 1) return 4;
        g_api->tick(701,2500,tid);
        if (snap().status != PGR_BUDDY_STATUS_BERRY_FED || snap().result_serial != 1) return 5;
        g_api->clear(tid);
        return g_roots || snap().owned_roots || g_bad_free ? 6 : 0;
    }
    case 15: { // Managed getter exceptions remain faults, not missing-data waits.
        g_stats_exception = true;
        g_api->tick(701,2500,tid);
        if (snap().status != PGR_BUDDY_STATUS_ERROR || snap().fault_serial != 1) return 1;
        if (snap().feed_request_serial || g_feed_target_calls) return 2;
        g_api->clear(tid);
        return g_roots || snap().owned_roots || g_bad_free ? 3 : 0;
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
    if (!core || core->query_feature(12, 99) != nullptr) { std::puts("FAIL exact-version"); return 3; }
    g_api = static_cast<const pgrazor_buddy_api_v1 *>(core->query_feature(12, 1));
    if (!g_api || g_api->size != 40) { std::puts("FAIL feature 12 not wired"); return 3; }

    // Config validation (parent process; these all fail before any hook is attempted).
    pgrazor_buddy_config_v1 c = make_config();
    pgrazor_buddy_config_v1 bad;
    if (g_api->install(nullptr) != PGR_BUDDY_ERR_NULL_ARG) { std::puts("FAIL null"); return 5; }
    bad = c; bad.version = 2;
    if (g_api->install(&bad) != PGR_BUDDY_ERR_BAD_CONFIG) { std::puts("FAIL version"); return 5; }
    bad = c; bad.size = sizeof(c) - 1;
    if (g_api->install(&bad) != PGR_BUDDY_ERR_BAD_CONFIG) { std::puts("FAIL size"); return 5; }
    bad = c; bad.m_feed = nullptr;
    if (g_api->install(&bad) != PGR_BUDDY_ERR_BAD_CONFIG) { std::puts("FAIL missing"); return 5; }
    bad = c; bad.result_address = bad.feed_address;
    if (g_api->install(&bad) != PGR_BUDDY_ERR_DUP_HOOK) { std::puts("FAIL dup"); return 5; }

    const int cases = 16;
    for (int i = 0; i < cases; ++i) {
        pid_t pid = fork();
        if (pid == 0) _exit(scenario(i));          // fresh process -> clean module/hook state
        int status = 0; waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::printf("FAIL buddy scenario %d (code %d)\n", i,
                        WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            return 4;
        }
    }
    std::puts("PASS native buddy feeding: sync result, hunger re-arm, stock 0 no feed, manual-feed "
              "ambiguity, result failure no retry, in-flight no duplicate, lease expiry mid-getter "
              "no feed, off-thread clear no free + stop/root balance with preserved pending, "
              "wrong-thread no-op, config validation, delayed hunger snapshot recovery without "
              "setting changes, managed hunger exception remains fail-closed");
    return 0;
}

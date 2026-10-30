// PGRazor native migration: buddy auto-feeding (feature id 12).
//
// Full native port of runtime/features/buddy.js. See buddy_feeding.h for the ABI and the
// division of labour with runtime/native-buddy-feeding.js. Built -fno-exceptions / -fno-rtti
// like every sibling: JS "throw" paths become fail-closed error flags and RAII destructors
// free each managed root exactly once on scope exit.
//
// Threading model (the invariants this file enforces):
//   * tick() and both hooks run on the game's Unity thread. tick latches a permanent Unity
//     identity (g_unity_tid) once, separate from the non-blocking per-pass owner guard
//     (g_owner_tid) so a nested tick is suppressed while the Feed/result callbacks that fire
//     *inside* our own Feed invoke are preserved. A foreign thread never steals ownership.
//   * No mutex is ever held across a managed call (runtime_invoke) or across an original
//     trampoline. all mutable state is owner-thread-only and guarded against reentrant cleanup;
//     telemetry is atomic so
//     sample() is lock-free and never frees.
//   * Only the Unity thread creates or frees GC handles. clear() off the proven thread is a
//     no-op; the durable roots are freed by the owner, the uncertain pending kept by value.

#include "buddy_feeding.h"
#include "hook_engine.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

static_assert(sizeof(pgrazor_buddy_config_v1) == 352, "config must be 352 bytes");
static_assert(sizeof(pgrazor_buddy_sample_v1) == 56,  "sample must be 56 bytes");
static_assert(sizeof(pgrazor_buddy_api_v1)    == 40,  "table must be 40 bytes");
static_assert(offsetof(pgrazor_buddy_config_v1, runtime_invoke)  == 8,   "layout");
static_assert(offsetof(pgrazor_buddy_config_v1, field_get_value) == 72,  "layout");
static_assert(offsetof(pgrazor_buddy_config_v1, m_find)          == 80,  "layout");
static_assert(offsetof(pgrazor_buddy_config_v1, m_alive)         == 168, "layout");
static_assert(offsetof(pgrazor_buddy_config_v1, feed_address)    == 176, "layout");
static_assert(offsetof(pgrazor_buddy_config_v1, result_address)  == 184, "layout");
static_assert(offsetof(pgrazor_buddy_config_v1, class_map)       == 192, "layout");
static_assert(offsetof(pgrazor_buddy_config_v1, class_out_proto) == 288, "layout");
static_assert(offsetof(pgrazor_buddy_config_v1, f_rpc)           == 296, "layout");
static_assert(offsetof(pgrazor_buddy_config_v1, f_buddy_id)      == 336, "layout");
static_assert(offsetof(pgrazor_buddy_config_v1, success)         == 344, "layout");
static_assert(offsetof(pgrazor_buddy_sample_v1, feed_request_serial) == 24, "layout");
static_assert(offsetof(pgrazor_buddy_sample_v1, owned_roots)     == 52,  "layout");

namespace {

constexpr uint64_t kPollMs     = PGRAZOR_BUDDY_POLL_MS;
constexpr uint64_t kFeedMs     = PGRAZOR_BUDDY_FEED_MS;
constexpr uint64_t kProgressMs = PGRAZOR_BUDDY_PROGRESS_MS;
constexpr uint64_t kManualMs   = PGRAZOR_BUDDY_MANUAL_MS;
constexpr uint64_t kMaxLeaseMs = PGRAZOR_BUDDY_MAX_LEASE_MS;
constexpr int      kMaxFeeds   = PGRAZOR_BUDDY_MAX_FEEDS;

// Hook signatures (exact game ABI; enum ItemId and int arrive in integer registers).
using FeedFn   = void *(*)(void *self, int32_t item, int32_t count, void *method);
using ResultFn = void  (*)(void *self, void *proto, void *method);

// --- lifetime / publication -------------------------------------------------------
std::mutex                g_install_mutex;
pgrazor_buddy_config_v1   g_cfg{};
std::atomic<bool>         g_installed{false};
bool                      g_install_attempted = false;   // install mutex; failure latches
int                       g_install_error = PGR_BUDDY_ERR_NOT_INSTALLED;
std::atomic<FeedFn>       g_orig_feed{nullptr};
std::atomic<ResultFn>     g_orig_result{nullptr};

// --- telemetry (pull-only; any thread; never frees) -------------------------------
std::atomic<uint32_t> g_status{PGR_BUDDY_STATUS_NONE};
std::atomic<uint32_t> g_status_serial{0};
std::atomic<int32_t>  g_detail{0};
std::atomic<uint64_t> g_feed_serial{0};
std::atomic<uint64_t> g_result_serial{0};
std::atomic<uint64_t> g_service_serial{0};
std::atomic<uint32_t> g_fault_serial{0};
std::atomic<uint32_t> g_root_count{0};

// --- thread identity / reentrancy -------------------------------------------------
// g_unity_tid is the permanent Unity identity, latched once. g_owner_tid is the
// non-blocking busy guard for a single tick pass (nested tick loses and no-ops). The
// hooks never touch g_owner_tid; they act on the latched Unity thread only.
std::atomic<uint64_t> g_unity_tid{0};
std::atomic<uint64_t> g_owner_tid{0};

// --- owner-thread state ----------------------------------------------------------
uint32_t g_depth = 0; // owner-thread only, includes original callback frames
std::atomic<uint32_t> g_active_frames{0};
bool g_revoke = false, g_deferred_clear = false;
struct Pending {
    bool     active = false;
    void    *rpc_handle = nullptr;      // durable GC root on the issuing BuddyRpcService
    void    *promise_handle = nullptr;  // durable GC root on the returned promise (may be null)
    bool     have_result = false;
    int32_t  result = 0;
    bool     ambiguous = false;         // a manual feed interfered: never attribute a result
    double   fullness = 0.0;            // fullness at feed time (progress baseline)
    uint64_t deadline_ms = 0;
    uint64_t generation = 0;            // bumped each new request (result-hook onLeave match)
};
struct State {
    void    *service_handle = nullptr;  // durable GC root on BuddyStatsService
    Pending  pending;
    uint64_t buddy_id = 0;
    bool     have_id = false;
    int32_t  config_berry = 0;
    uint64_t next_ms = 0;
    bool     blocked = false;
    int      feeds = 0;
    bool     have_progress = false;
    double   progress_before = 0.0;
    uint64_t progress_deadline = 0;
    bool     invoking = false;          // true while WE invoke Feed (suppress manual path)
    uint64_t next_generation = 0;
} g_st;

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
inline bool on_unity_thread(uint64_t proven_tid) {
    return proven_tid != 0 && current_tid() == proven_tid;
}

// Publish (or wait for install to publish) an original trampoline. Never skips the original.
template <typename Fn>
Fn load_orig(std::atomic<Fn> &slot) {
    Fn fn = slot.load(std::memory_order_acquire);
    if (fn != nullptr) return fn;
    std::lock_guard<std::mutex> lock(g_install_mutex);
    return slot.load(std::memory_order_acquire);
}

// --- il2cpp primitives over the validated config ----------------------------------
inline void *gc_new(void *o)      { return reinterpret_cast<void *(*)(void *, int)>(g_cfg.gchandle_new)(o, 0); }
inline void *gc_target(void *h)   { return h ? reinterpret_cast<void *(*)(void *)>(g_cfg.gchandle_get_target)(h) : nullptr; }
inline void  gc_free(void *h)     { if (h) reinterpret_cast<void (*)(void *)>(g_cfg.gchandle_free)(h); }
inline void *obj_class(void *o)   { return reinterpret_cast<void *(*)(void *)>(g_cfg.object_get_class)(o); }
inline void *obj_unbox(void *o)   { return reinterpret_cast<void *(*)(void *)>(g_cfg.object_unbox)(o); }
inline void *cls_type(void *k)    { return reinterpret_cast<void *(*)(void *)>(g_cfg.class_get_type)(k); }
inline void *type_obj(void *t)    { return reinterpret_cast<void *(*)(void *)>(g_cfg.type_get_object)(t); }
inline void  field_get(void *o, void *f, void *out) {
    reinterpret_cast<void (*)(void *, void *, void *)>(g_cfg.field_get_value)(o, f, out);
}
inline bool exact(void *o, void *k) { return o != nullptr && obj_class(o) == k; }
inline void *read_ref(void *o, void *f) { void *v = nullptr; field_get(o, f, &v); return v; }
inline int32_t read_s32(void *o, void *f) { int32_t v = 0; field_get(o, f, &v); return v; }
inline uint64_t read_u64(void *o, void *f) { uint64_t v = 0; field_get(o, f, &v); return v; }

// Invoke a managed method; on a managed exception set *err and return null (JS threw).
inline void *invoke(void *method, void *obj, void **params, bool *err) {
    if (method != g_cfg.m_find && method != g_cfg.m_alive && method != g_cfg.m_remaining &&
        method != g_cfg.m_fullness && method != g_cfg.m_cooldown && obj == nullptr) { *err = true; return nullptr; }
    void *exc = nullptr;
    void *r = reinterpret_cast<void *(*)(void *, void *, void **, void **)>(
        g_cfg.runtime_invoke)(method, obj, params, &exc);
    if (exc != nullptr) { *err = true; return nullptr; }
    return r;
}

// --- RAII rooting -----------------------------------------------------------------
// Single owned GC root, freed exactly once on scope exit. Noncopyable (a copy would
// double-free). set() frees the previous handle first. Never fabricates a root.
struct Root {
    void *h = nullptr;
    Root() = default;
    Root(const Root &) = delete;
    Root &operator=(const Root &) = delete;
    void set(void *obj, bool *err) {
        if (h) { gc_free(h); h = nullptr; }
        if (obj == nullptr) { *err = true; return; }
        h = gc_new(obj);
        if (h == nullptr) *err = true;
    }
    void *target() const { return h ? gc_target(h) : nullptr; }
    ~Root() { if (h) gc_free(h); }
};

// Boxed value reads: root the box across the unbox, exact-class check, then read. Fail
// closed on a null box, a wrong box class, or (for bool) a value outside {0,1}.
bool invoke_bool(void *method, void *obj, void **params, bool *out, bool *err) {
    void *box = invoke(method, obj, params, err);
    if (*err) return false;
    Root r; r.set(box, err); if (*err) return false;
    void *b = r.target();
    if (!exact(b, g_cfg.class_bool)) { *err = true; return false; }
    void *raw = obj_unbox(b);
    if (raw == nullptr) { *err = true; return false; }
    uint8_t v = *reinterpret_cast<uint8_t *>(raw);
    if (v > 1) { *err = true; return false; }
    *out = v != 0;
    return true;
}
bool invoke_int(void *method, void *obj, void **params, int32_t *out, bool *err) {
    void *box = invoke(method, obj, params, err);
    if (*err) return false;
    Root r; r.set(box, err); if (*err) return false;
    void *b = r.target();
    if (!exact(b, g_cfg.class_int)) { *err = true; return false; }
    void *raw = obj_unbox(b);
    if (raw == nullptr) { *err = true; return false; }
    *out = *reinterpret_cast<int32_t *>(raw);
    return true;
}
bool invoke_float(void *method, void *obj, void **params, float *out, bool *err) {
    void *box = invoke(method, obj, params, err);
    if (*err) return false;
    Root r; r.set(box, err); if (*err) return false;
    void *b = r.target();
    if (!exact(b, g_cfg.class_single)) { *err = true; return false; } // float box check (hardens the JS)
    void *raw = obj_unbox(b);
    if (raw == nullptr) { *err = true; return false; }
    *out = *reinterpret_cast<float *>(raw);
    return true;
}

// --- telemetry writers (Unity thread) ---------------------------------------------
void set_status(uint32_t code) {
    if (g_status.exchange(code) != code)
        g_status_serial.fetch_add(1, std::memory_order_release);
}
void set_status_detail(uint32_t code, int32_t detail) {
    const bool changed = g_detail.exchange(detail) != detail;
    const bool status_changed = g_status.exchange(code) != code;
    if (changed || status_changed) g_status_serial.fetch_add(1, std::memory_order_release);
}
void note_fault() { g_fault_serial.fetch_add(1, std::memory_order_release); }

// --- durable root accounting (Unity thread) ---------------------------------------
void refresh_root_count() {
    uint32_t n = 0;
    if (g_st.service_handle) ++n;
    if (g_st.pending.active) {
        if (g_st.pending.rpc_handle) ++n;
        if (g_st.pending.promise_handle) ++n;
    }
    g_root_count.store(n, std::memory_order_release);
}
// Release the pending request's durable roots but keep the uncertain request BY VALUE so a
// restart cannot resubmit it (JS confirmation was by live hunger / result hook; we never
// auto-duplicate). The rpc root being gone means a *new* result can no longer be matched;
// an already-captured result survives and is consumed on the next tick.
void release_pending_roots() {
    void *rpc = g_st.pending.rpc_handle, *promise = g_st.pending.promise_handle;
    g_st.pending.rpc_handle = nullptr; g_st.pending.promise_handle = nullptr;
    gc_free(rpc); gc_free(promise);
}
// Drop the pending request entirely (confirmed result consumed, or superseded).
void drop_pending() {
    Pending old = g_st.pending;
    g_st.pending = Pending{};
    gc_free(old.rpc_handle); gc_free(old.promise_handle);
}

void release_roots() {
    void *service = g_st.service_handle; g_st.service_handle = nullptr;
    release_pending_roots(); gc_free(service); refresh_root_count();
}
struct OwnerScope {
    bool active;
    explicit OwnerScope(bool enabled=true) : active(enabled) {
        if(active) { ++g_depth; g_active_frames.store(g_depth); }
    }
    ~OwnerScope() {
        if(!active) return;
        if(g_depth==1 && g_deferred_clear) {
            // Stay busy while GC functions run; reentrant cleanup can only defer.
            g_deferred_clear=false; release_roots(); g_deferred_clear=false;
        }
        --g_depth; g_active_frames.store(g_depth);
    }
};

// --- service resolution (Unity thread) --------------------------------------------
// Resolve a contract through the DI container and verify the concrete identity, exactly as
// incubation.cpp does: the System.Type argument is rooted across the invoke.
void *resolve(Root &container_root, void *contract_cls, void *concrete_cls, Root *root, bool *err) {
    if (container_root.target() == nullptr) { *err = true; return nullptr; }
    void *type = type_obj(cls_type(contract_cls));
    if (type == nullptr) { *err = true; return nullptr; }
    Root type_root; type_root.set(type, err);
    if (*err) return nullptr;
    void *params[1] = { type_root.target() };
    if (!params[0]) { *err = true; return nullptr; }
    void *o = invoke(g_cfg.m_resolve, container_root.target(), params, err);
    if (*err) return nullptr;
    root->set(o, err);
    if (*err) return nullptr;
    if (!exact(root->target(), concrete_cls)) { *err = true; return nullptr; }
    return root->target();
}

// --- the orchestration pass (Unity thread, no exceptions) -------------------------
// deadline is an absolute CLOCK_BOOTTIME ms watermark for the single Feed.
void orchestrate(int32_t berry, uint64_t now, uint64_t deadline) {
    State &r = g_st;

    // 1. A pending request outstanding: consume a captured result, else wait / time out.
    if (r.pending.active) {
        bool have_result; int32_t result; double fullness;
        {

            have_result = r.pending.have_result;
            result = r.pending.result;
            fullness = r.pending.fullness;
        }
        if (have_result) {
            bool ok = (result == g_cfg.success);
            double feed_fullness = fullness;
            {

                drop_pending();
            }
            refresh_root_count();
            r.next_ms = now + kPollMs;
            g_result_serial.fetch_add(1, std::memory_order_release); // adapter zeroes getItemState().next
            if (!ok) { r.blocked = true; set_status_detail(PGR_BUDDY_STATUS_FEED_FAILED, result); }
            else {
                ++r.feeds;
                r.have_progress = true;
                r.progress_before = feed_fullness;
                r.progress_deadline = now + kProgressMs;
                set_status(PGR_BUDDY_STATUS_BERRY_FED);
            }
            return;
        }
        if (now > r.pending.deadline_ms && !r.blocked) {
            r.blocked = true;
            set_status(PGR_BUDDY_STATUS_TIMEOUT);
        }
        return;
    }

    // 2. Config (berry) change resets the per-cycle state (keeps no pending here).
    if (r.config_berry != berry) {
        r.config_berry = berry;
        r.blocked = false;
        r.feeds = 0;
        r.have_progress = false;
        r.next_ms = 0;
    }

    // 3. Off / map-lease / poll gates (mirror s.buddyBerry / s.buddyLease / r.next).
    if (berry == 0) { set_status(PGR_BUDDY_STATUS_OFF); return; }
    if (now == 0 || now >= deadline) { set_status(PGR_BUDDY_STATUS_WAIT_MAP); return; }
    if (now < r.next_ms) return;
    r.next_ms = now + kPollMs;

    // 4. Managed orchestration. Transient roots are RAII; durable roots live in g_st.
    bool err = false;
    Root map_type_h, map_h, container_h, service_tmp, proto_h, stats_h, pokemon_h, rpc_h, bag_h;

    // Resolve (or re-resolve) the BuddyStatsService and confirm it is alive.
    bool service_alive = false;
    if (g_st.service_handle) {
        void *svc = gc_target(g_st.service_handle);
        if (svc != nullptr) {
            void *ap[1] = { svc };
            invoke_bool(g_cfg.m_alive, nullptr, ap, &service_alive, &err);
            if (err) goto fail;
        }
    }
    if (!g_st.service_handle || !service_alive) {
        if (g_st.service_handle) { gc_free(g_st.service_handle); g_st.service_handle = nullptr; refresh_root_count(); }

        void *map_type = type_obj(cls_type(g_cfg.class_map));
        if (map_type == nullptr) { err = true; goto fail; }
        map_type_h.set(map_type, &err); if (err) goto fail;
        void *mp[1] = { map_type_h.target() };
        if (!mp[0]) { err = true; goto fail; }
        void *map = invoke(g_cfg.m_find, nullptr, mp, &err);
        if (err) goto fail;
        if (map == nullptr) return;                       // no map yet (soft, not an error)
        map_h.set(map, &err); if (err) goto fail;

        void *container = invoke(g_cfg.m_container, map_h.target(), nullptr, &err);
        if (err) goto fail;
        container_h.set(container, &err); if (err) goto fail;

        void *service = resolve(container_h, g_cfg.class_contract,
                                g_cfg.class_stats_service, &service_tmp, &err);
        if (err) goto fail;
        g_st.service_handle = gc_new(service);
        if (g_st.service_handle == nullptr) { err = true; goto fail; }
        refresh_root_count();
        g_service_serial.fetch_add(1, std::memory_order_release);
    }

    {
        void *service = gc_target(g_st.service_handle);
        if (service == nullptr) { err = true; goto fail; }
        pokemon_h.set(read_ref(service, g_cfg.f_pokemon), &err); if(err) goto fail;
        service = gc_target(g_st.service_handle); if(!service) {err=true; goto fail;}
        rpc_h.set(read_ref(service, g_cfg.f_rpc), &err); if(err) goto fail;
        void *pokemon = pokemon_h.target();
        void *rpc = rpc_h.target();
        if (!exact(pokemon, g_cfg.class_pokemon_service) || !exact(rpc, g_cfg.class_rpc)) {
            err = true; goto fail;                        // buddy-service-type
        }

        bool has = false;
        invoke_bool(g_cfg.m_has, pokemon_h.target(), nullptr, &has, &err);
        if (err) goto fail;
        if (!has) { set_status(PGR_BUDDY_STATUS_CHOOSE); return; }

        void *proto = invoke(g_cfg.m_pokemon, pokemon_h.target(), nullptr, &err);
        if (err) goto fail;
        proto_h.set(proto, &err); if (err) goto fail;
        if (!exact(proto_h.target(), g_cfg.class_buddy_proto)) return; // not ready (soft)
        uint64_t id = read_u64(proto_h.target(), g_cfg.f_buddy_id);
        if (id == 0) return;                              // no buddy id yet (soft)
        if (!g_st.have_id || id != g_st.buddy_id) {
            g_st.buddy_id = id; g_st.have_id = true;
            g_st.feeds = 0; g_st.have_progress = false; g_st.blocked = false;
        }

        void *stats = invoke(g_cfg.m_stats, gc_target(g_st.service_handle), nullptr, &err);
        if (err) goto fail;
        // Hunger data can arrive after the buddy identity during map startup.
        // A null snapshot is not a failed feed or managed exception. Latching
        // blocked here would permanently suppress this already-known hungry
        // buddy even when a later poll obtains valid stats.
        if (stats == nullptr) {
            set_status(PGR_BUDDY_STATUS_WAIT_HUNGER); return;
        }
        stats_h.set(stats, &err); if (err) goto fail;
        if (!exact(stats_h.target(), g_cfg.class_observed)) {
            set_status(PGR_BUDDY_STATUS_WAIT_HUNGER); return;
        }

        float remaining = 0.0f, fullness = 0.0f;
        void *sp[1] = { stats_h.target() };
        invoke_float(g_cfg.m_remaining, nullptr, sp, &remaining, &err); if (err) goto fail;
        sp[0] = stats_h.target();
        invoke_float(g_cfg.m_fullness, nullptr, sp, &fullness, &err); if (err) goto fail;
        if (!std::isfinite(remaining) || !std::isfinite(fullness)) { err = true; goto fail; }

        if (remaining > 0.0f) {                           // fed -> re-arm for the next cycle
            g_st.feeds = 0; g_st.have_progress = false; g_st.blocked = false;
            set_status_detail(PGR_BUDDY_STATUS_FED,
                              static_cast<int32_t>(std::fmin(2147483647.0, std::ceil(static_cast<double>(remaining) / 60.0))));
            return;
        }
        if (g_st.blocked) return;                         // paused; only a fed state re-arms (above)
        if (g_st.have_progress) {
            if (fullness > g_st.progress_before + 0.001) g_st.have_progress = false;
            else if (now < g_st.progress_deadline) return;
            else { g_st.blocked = true; set_status(PGR_BUDDY_STATUS_HUNGER_STALE); return; }
        }

        bool cooldown = false;
        sp[0] = stats_h.target();
        invoke_bool(g_cfg.m_cooldown, nullptr, sp, &cooldown, &err); if (err) goto fail;
        if (cooldown || fullness >= 0.999f) { set_status(PGR_BUDDY_STATUS_COOLDOWN); return; }

        if (g_st.feeds >= kMaxFeeds) { g_st.blocked = true; set_status(PGR_BUDDY_STATUS_PAUSED_SIX); return; }

        rpc = rpc_h.target(); if(!rpc) {err=true; goto fail;}
        bag_h.set(read_ref(rpc, g_cfg.f_bag), &err); if(err) goto fail;
        void *bag = bag_h.target();
        if (!exact(bag, g_cfg.class_bag)) { err = true; goto fail; } // buddy-item-bag

        int32_t berry_arg = berry;
        void *qp[1] = { &berry_arg };
        int32_t count = 0;
        invoke_int(g_cfg.m_quantity, bag_h.target(), qp, &count, &err); if (err) goto fail;
        if (count < 1) { set_status(PGR_BUDDY_STATUS_OUT_OF_STOCK); return; }

        // Establish the pending request (durable rpc root) BEFORE the Feed so a synchronous
        // result callback is captured, and so a managed throw leaves us uncertain (no retry).
        rpc = rpc_h.target(); if(!rpc) {err=true; goto fail;}
        void *rpc_handle = gc_new(rpc);
        if (rpc_handle == nullptr) { err = true; goto fail; }
        uint64_t gen;
        {

            g_st.pending = Pending{};
            g_st.pending.active = true;
            g_st.pending.rpc_handle = rpc_handle;
            g_st.pending.fullness = fullness;
            g_st.pending.deadline_ms = now + kFeedMs;
            g_st.pending.generation = gen = ++g_st.next_generation;
            g_st.invoking = true;
        }
        refresh_root_count();

        // Lease re-check immediately before the single Feed, AFTER the root allocation (which
        // can re-enter the host / consume the window). No request issued yet -> safe to abort.
        rpc = gc_target(rpc_handle);
        uint64_t t = boottime_ms();
        if (g_revoke || !rpc || t == 0 || t >= deadline) {

            drop_pending();
            g_st.invoking = false;
            refresh_root_count();
            return;
        }

        set_status(PGR_BUDDY_STATUS_FEEDING);
        g_feed_serial.fetch_add(1, std::memory_order_release); // count issuance before the call

        int32_t one = 1;
        void *fp[2] = { &berry_arg, &one };
        void *promise = invoke(g_cfg.m_feed, rpc, fp, &err); // feed/result hooks fire here
        {

            g_st.invoking = false;
        }
        if (err) { r.blocked = true; set_status(PGR_BUDDY_STATUS_ERROR); note_fault(); return; } // pending kept
        if (promise != nullptr) {
            void *ph = gc_new(promise);
            if(!ph) {r.blocked=true; set_status(PGR_BUDDY_STATUS_ERROR); note_fault();}

            if (g_st.pending.active && g_st.pending.generation == gen) g_st.pending.promise_handle = ph;
            else gc_free(ph);                              // a manual feed superseded us meanwhile
        }
        refresh_root_count();
        (void)map_h; (void)container_h; (void)service_tmp;
        return;
    }

fail:
    // Managed exception / fail-closed error. Block and surface the paused status. Durable
    // pending roots (if a request is in flight) are kept; transient roots free via RAII.
    g_st.blocked = true;
    set_status(PGR_BUDDY_STATUS_ERROR);
    note_fault();
}

// --- hooks ------------------------------------------------------------------------
// A manual Feed (not one of ours, on the Unity thread): mark any pending request ambiguous,
// block, and back off so we never duplicate the player's berry. Always chains the original.
void *hook_feed(void *self, int32_t item, int32_t count, void *method) {
    FeedFn orig = load_orig(g_orig_feed);
    uint64_t unity = g_unity_tid.load(std::memory_order_acquire);
    OwnerScope frame(unity != 0 && current_tid() == unity);
    if (unity != 0 && current_tid() == unity) {
        bool act;
        {  act = !g_st.invoking; }
        if (act) {
            uint64_t now = boottime_ms();

            if (g_st.pending.active) { g_st.pending.ambiguous = true; g_st.blocked = true; }
            g_st.next_ms = now + kManualMs;
            g_st.feeds = 0;
            g_st.have_progress = false;
        }
    }
    return orig(self, item, count, method);
}

// The generated feed-completion callback. Capture OUR pending result (matched by callback
// class + its captured rpc + the out-proto class), committed only after the original runs.
void hook_result(void *self, void *proto, void *method) {
    ResultFn orig = load_orig(g_orig_result);
    uint64_t unity = g_unity_tid.load(std::memory_order_acquire);
    OwnerScope frame(unity != 0 && current_tid() == unity);

    bool capture = false;
    int32_t captured = 0;
    uint64_t gen = 0;
    if (unity != 0 && current_tid() == unity) {

        void *rpc = g_st.pending.active ? gc_target(g_st.pending.rpc_handle) : nullptr;
        if (g_st.pending.active && !g_st.pending.ambiguous && rpc != nullptr &&
            exact(self, g_cfg.class_callback) &&
            read_ref(self, g_cfg.f_callback_rpc) == rpc &&
            exact(proto, g_cfg.class_out_proto)) {
            captured = read_s32(proto, g_cfg.f_result);
            gen = g_st.pending.generation;
            capture = true;
        }
    }

    orig(self, proto, method);

    if (capture) {

        if (g_st.pending.active && g_st.pending.generation == gen && !g_st.pending.ambiguous) {
            g_st.pending.have_result = true;
            g_st.pending.result = captured;
        }
    }
}

// --- C API table functions --------------------------------------------------------
bool config_ok(const pgrazor_buddy_config_v1 *c) {
    const void *const ptrs[] = {
        c->runtime_invoke, c->gchandle_new, c->gchandle_get_target, c->gchandle_free,
        c->object_get_class, c->object_unbox, c->class_get_type, c->type_get_object,
        c->field_get_value, c->m_find, c->m_container, c->m_resolve, c->m_has, c->m_pokemon,
        c->m_stats, c->m_remaining, c->m_fullness, c->m_cooldown, c->m_quantity, c->m_feed,
        c->m_alive, c->feed_address, c->result_address, c->class_map, c->class_contract,
        c->class_stats_service, c->class_pokemon_service, c->class_rpc, c->class_buddy_proto,
        c->class_observed, c->class_bag, c->class_bool, c->class_int, c->class_single,
        c->class_callback, c->class_out_proto, c->f_rpc, c->f_pokemon, c->f_bag,
        c->f_callback_rpc, c->f_result, c->f_buddy_id};
    for (const void *p : ptrs) if (p == nullptr) return false;
    return true;
}

int api_install(const pgrazor_buddy_config_v1 *config) {
    if (config == nullptr) return PGR_BUDDY_ERR_NULL_ARG;
    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (config->version != PGRAZOR_BUDDY_API_VERSION ||
        config->size != sizeof(pgrazor_buddy_config_v1) || !config_ok(config))
        return PGR_BUDDY_ERR_BAD_CONFIG;
    if (config->feed_address == config->result_address) return PGR_BUDDY_ERR_DUP_HOOK;

    if (g_installed.load(std::memory_order_acquire))
        return (std::memcmp(&g_cfg, config, sizeof(g_cfg)) == 0)
                   ? PGR_BUDDY_OK : PGR_BUDDY_ERR_ALREADY_BOUND;
    if (g_install_attempted) return g_install_error;     // a prior failure latches
    g_install_attempted = true;

    if (boottime_ms() == 0) return g_install_error = PGR_BUDDY_ERR_NO_CLOCK;
    g_cfg = *config;

    // Install the two hooks, publishing each original before the next. On a partial failure
    // retain any published trampoline (another thread may already use it) and latch.
    void *orig_feed = nullptr;
    if (pgrazor_hook(g_cfg.feed_address, reinterpret_cast<void *>(&hook_feed), &orig_feed) != 0 ||
        orig_feed == nullptr)
        return g_install_error = PGR_BUDDY_ERR_HOOK_FEED;
    g_orig_feed.store(reinterpret_cast<FeedFn>(orig_feed), std::memory_order_release);

    void *orig_result = nullptr;
    if (pgrazor_hook(g_cfg.result_address, reinterpret_cast<void *>(&hook_result), &orig_result) != 0 ||
        orig_result == nullptr)
        return g_install_error = PGR_BUDDY_ERR_HOOK_RESULT;
    g_orig_result.store(reinterpret_cast<ResultFn>(orig_result), std::memory_order_release);

    g_installed.store(true, std::memory_order_release);
    return g_install_error = PGR_BUDDY_OK;
}

int api_tick(int32_t berry, uint64_t remaining_lease_ms, uint64_t proven_tid) {
    if (!g_installed.load(std::memory_order_acquire)) return PGR_BUDDY_ERR_NOT_INSTALLED;
    if (!on_unity_thread(proven_tid)) return PGR_BUDDY_OK;

    // Latch the permanent Unity identity once; a foreign tid never rebinds it.
    uint64_t expected_unity = 0;
    g_unity_tid.compare_exchange_strong(expected_unity, proven_tid, std::memory_order_acq_rel);
    if (g_unity_tid.load(std::memory_order_acquire) != proven_tid) return PGR_BUDDY_OK;

    if(g_depth != 0) { if(berry==0 || remaining_lease_ms==0) g_revoke=true; return PGR_BUDDY_OK; }
    OwnerScope frame; g_revoke=false;

    // Non-blocking owner guard: a nested tick loses and no-ops (the Feed/result callbacks
    // that fire inside our own invoke are hooks, not ticks, so they are never suppressed).
    uint64_t expected = 0;
    if (!g_owner_tid.compare_exchange_strong(expected, proven_tid, std::memory_order_acq_rel))
        return PGR_BUDDY_OK;

    uint64_t lease = remaining_lease_ms > kMaxLeaseMs ? kMaxLeaseMs : remaining_lease_ms;
    uint64_t now = boottime_ms();
    orchestrate(berry, now, now + lease);

    g_owner_tid.store(0, std::memory_order_release);
    return PGR_BUDDY_OK;
}

int api_clear(uint64_t proven_tid) {
    if (!g_installed.load(std::memory_order_acquire)) return PGR_BUDDY_ERR_NOT_INSTALLED;
    if (!on_unity_thread(proven_tid) ||
        g_unity_tid.load(std::memory_order_acquire) != proven_tid)
        return PGR_BUDDY_OK;                              // never free off the Unity thread

    g_revoke=true;
    if(g_depth != 0) { g_deferred_clear=true; return PGR_BUDDY_OK; }
    OwnerScope frame;
    release_roots();

    return PGR_BUDDY_OK;
}

int api_sample(pgrazor_buddy_sample_v1 *out) {
    if (out == nullptr) return PGR_BUDDY_ERR_NULL_ARG;
    out->version = PGRAZOR_BUDDY_API_VERSION;
    out->size = sizeof(pgrazor_buddy_sample_v1);
    out->status = g_status.load(std::memory_order_acquire);
    out->status_serial = g_status_serial.load(std::memory_order_acquire);
    out->detail = g_detail.load(std::memory_order_acquire);
    out->reserved = 0;
    out->feed_request_serial = g_feed_serial.load(std::memory_order_acquire);
    out->result_serial = g_result_serial.load(std::memory_order_acquire);
    out->service_serial = g_service_serial.load(std::memory_order_acquire);
    out->fault_serial = g_fault_serial.load(std::memory_order_acquire);
    out->owned_roots = g_root_count.load(std::memory_order_acquire) + g_active_frames.load(std::memory_order_acquire);
    return PGR_BUDDY_OK;
}

const pgrazor_buddy_api_v1 g_api = {
    PGRAZOR_BUDDY_API_VERSION,
    static_cast<uint32_t>(sizeof(pgrazor_buddy_api_v1)),
    &api_install,
    &api_tick,
    &api_clear,
    &api_sample,
};

} // namespace

const void *pgrazor_buddy_feeding_api(uint32_t version) {
    if (version != PGRAZOR_BUDDY_API_VERSION) return nullptr;
    return &g_api;
}

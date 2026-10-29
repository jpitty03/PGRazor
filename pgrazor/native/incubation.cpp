// PGRazor native migration: unlimited-incubator auto-incubation (feature id 9).
//
// Full native port of runtime/features/incubation.js. See incubation.h for the ABI and
// the division of labour with runtime/native-incubation.js. Built -fno-exceptions /
// -fno-rtti like every sibling module: the JS "throw" paths become fail-closed error
// flags, and RAII destructors (which run on ordinary scope exit, no unwinding needed)
// free every managed root exactly once. The orchestration is Unity-thread-only; small
// telemetry fields are atomics so sample() is safe from any thread.

#include "incubation.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

static_assert(sizeof(pgrazor_incubation_config_v1) == 360, "config must be 360 bytes");
static_assert(sizeof(pgrazor_incubation_sample_v1) == 48,  "sample must be 48 bytes");
static_assert(sizeof(pgrazor_incubation_api_v1)    == 40,  "table must be 40 bytes");
static_assert(offsetof(pgrazor_incubation_config_v1, runtime_invoke) == 8,  "layout");
static_assert(offsetof(pgrazor_incubation_config_v1, field_get_value) == 96, "layout");
static_assert(offsetof(pgrazor_incubation_config_v1, m_find)    == 104, "layout");
static_assert(offsetof(pgrazor_incubation_config_v1, m_dispose) == 176, "layout");
static_assert(offsetof(pgrazor_incubation_config_v1, class_map) == 184, "layout");
static_assert(offsetof(pgrazor_incubation_config_v1, class_service) == 248, "layout");
static_assert(offsetof(pgrazor_incubation_config_v1, f_inc_item) == 256, "layout");
static_assert(offsetof(pgrazor_incubation_config_v1, f_egg_start) == 328, "layout");
static_assert(offsetof(pgrazor_incubation_config_v1, unlimited) == 336, "layout");
static_assert(offsetof(pgrazor_incubation_config_v1, special)   == 340, "layout");
static_assert(offsetof(pgrazor_incubation_config_v1, class_boolean) == 344, "layout");
static_assert(offsetof(pgrazor_incubation_config_v1, class_string)  == 352, "layout");
static_assert(offsetof(pgrazor_incubation_sample_v1, owned_roots) == 40, "layout");

namespace {

constexpr unsigned kMaxInc    = PGRAZOR_INCUBATION_MAX_INCUBATORS;
constexpr unsigned kMaxEggs   = PGRAZOR_INCUBATION_MAX_EGGS;
constexpr unsigned kMaxStr    = PGRAZOR_INCUBATION_MAX_STRLEN;
constexpr uint64_t kPollMs    = PGRAZOR_INCUBATION_POLL_MS;
constexpr uint64_t kNoEggsMs  = PGRAZOR_INCUBATION_NO_EGGS_MS;
constexpr uint64_t kDeadlineMs= PGRAZOR_INCUBATION_DEADLINE_MS;
constexpr uint64_t kMaxLeaseMs= PGRAZOR_INCUBATION_MAX_LEASE_MS;


// --- lifetime / publication -------------------------------------------------------
std::mutex                       g_install_mutex;
pgrazor_incubation_config_v1     g_config{};
std::atomic<bool>                g_installed{false};

// --- telemetry (pull-only; any thread) --------------------------------------------
std::atomic<uint32_t> g_status{PGRAZOR_INC_STATUS_NONE};
std::atomic<uint32_t> g_status_serial{0};
std::atomic<uint64_t> g_request_serial{0};
std::atomic<uint64_t> g_distance_bits{0};   // bit-cast double
std::atomic<uint32_t> g_longest{0};
std::atomic<uint32_t> g_fault_serial{0};
std::atomic<uint32_t> g_root_count{0};      // durable pending roots currently held

// --- ownership / reentrancy -------------------------------------------------------
// A non-blocking owner guard: whichever proven Unity thread wins the compare-exchange
// owns the current tick/clear. A nested call on the same thread, or a concurrent call
// from a different thread claiming the Unity tid, loses the exchange and no-ops instead
// of stealing or racing the owner's state. A clear() that arrives from inside the
// owner's own pass cannot free roots underneath it; it defers to the pass epilogue.
std::atomic<uint64_t> g_owner_tid{0};
std::atomic<uint64_t> g_unity_tid{0}; // process-lifetime identity, separate from busy owner
bool g_revoke          = false;   // owner-thread only: a reentrant clear fired this pass
bool g_teardown_defer  = false;   // owner-thread only: free owned roots at pass end

// --- Unity-thread-only state ------------------------------------------------------
// Touched only by the current owner (after the owner guard), so no lock is needed.
struct IncId { uint16_t data[PGRAZOR_INCUBATION_MAX_STRLEN]; int len = 0; };
inline bool incid_eq(const IncId &a, const IncId &b) {
    return a.len == b.len && std::memcmp(a.data, b.data, a.len * 2) == 0;
}
struct State {
    bool     enabled = false;
    bool     blocked = false;
    uint64_t next_ms = 0;        // next eligible poll (CLOCK_BOOTTIME ms)
    bool     pending = false;
    IncId    pending_inc;        // incubator ItemId we are awaiting confirmation on (by value)
    uint64_t pending_egg = 0;    // egg id we asked to incubate (by value)
    uint64_t pending_deadline = 0;
    void    *handles[4] = {nullptr, nullptr, nullptr, nullptr}; // pending GC roots
    int      handle_count = 0;
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

inline bool on_unity_thread(uint64_t proven_tid) {
    return proven_tid != 0 && static_cast<uint64_t>(gettid()) == proven_tid;
}

bool bind_unity_thread(uint64_t tid) {
    if (!on_unity_thread(tid)) return false;
    uint64_t expected = 0;
    g_unity_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
    return g_unity_tid.load(std::memory_order_acquire) == tid;
}

// --- il2cpp primitives over the validated config ----------------------------------
inline void *gc_new(void *o)      { return reinterpret_cast<void *(*)(void *, int)>(g_config.gchandle_new)(o, 0); }
inline void *gc_target(void *h)   { return reinterpret_cast<void *(*)(void *)>(g_config.gchandle_get_target)(h); }
inline void  gc_free(void *h)     { reinterpret_cast<void (*)(void *)>(g_config.gchandle_free)(h); }
inline void *obj_class(void *o)   { return reinterpret_cast<void *(*)(void *)>(g_config.object_get_class)(o); }
inline void *obj_unbox(void *o)   { return reinterpret_cast<void *(*)(void *)>(g_config.object_unbox)(o); }
inline int32_t  str_len(void *o)  { return reinterpret_cast<int32_t (*)(void *)>(g_config.string_length)(o); }
inline uint16_t *str_chars(void *o){ return reinterpret_cast<uint16_t *(*)(void *)>(g_config.string_chars)(o); }
inline void *cls_type(void *k)    { return reinterpret_cast<void *(*)(void *)>(g_config.class_get_type)(k); }
inline void *type_obj(void *t)    { return reinterpret_cast<void *(*)(void *)>(g_config.type_get_object)(t); }
inline void *virt(void *m, void *o){ return reinterpret_cast<void *(*)(void *, void *)>(g_config.object_get_virtual_method)(o, m); }
inline void  field_get(void *o, void *f, void *out) {
    reinterpret_cast<void (*)(void *, void *, void *)>(g_config.field_get_value)(o, f, out);
}

// Invoke a managed method; on a managed exception set *err and return null (JS threw).
inline void *invoke(void *method, void *obj, void **params, bool *err) {
    if (method != g_config.m_find && obj == nullptr) { *err = true; return nullptr; }
    void *exc = nullptr;
    void *r = reinterpret_cast<void *(*)(void *, void *, void **, void **)>(
        g_config.runtime_invoke)(method, obj, params, &exc);
    if (exc != nullptr) { *err = true; return nullptr; }
    return r;
}
// Interface dispatch then invoke (0-arg). *err on a null vtable slot or managed exception.
inline void *invoke_virtual(void *method, void *obj, bool *err) {
    if (obj == nullptr) { *err = true; return nullptr; }
    void *v = virt(method, obj);
    if (v == nullptr) { *err = true; return nullptr; }
    return invoke(v, obj, nullptr, err);
}
inline bool exact(void *o, void *k) { return o != nullptr && obj_class(o) == k; }

// Scalar field reads (8-byte zeroed scratch, as in incubationScalar/readS32/readPtr).
inline uint64_t read_u64(void *o, void *f) { uint64_t v = 0; field_get(o, f, &v); return v; }
inline int32_t  read_s32(void *o, void *f) { int32_t v = 0; field_get(o, f, &v); return v; }
inline double   read_dbl(void *o, void *f) { double v = 0; field_get(o, f, &v); return v; }
inline void    *read_ref(void *o, void *f) { void *v = nullptr; field_get(o, f, &v); return v; }
// Bool value-type field; fails closed on any value that is not exactly 0 or 1.
inline bool read_bool(void *o, void *f, bool *err) {
    uint64_t v = 0; field_get(o, f, &v);
    uint64_t b = v & 0xff;
    if (b > 1) { *err = true; return false; }
    return b != 0;
}

// Read a managed string field into a bounded IncId. On an exact-class / size-bound /
// null-chars violation set *err. A null reference is an empty id (len 0).
bool read_incid(void *o, void *f, IncId *out, bool *err) {
    out->len = 0;
    void *s = read_ref(o, f);
    if (s == nullptr) return true;
    if (!exact(s, g_config.class_string)) { *err = true; return false; } // must be System.String
    int32_t n = str_len(s);
    if (n < 0 || static_cast<uint32_t>(n) > kMaxStr) { *err = true; return false; }
    if (n > 0) {
        uint16_t *chars = str_chars(s);
        if (chars == nullptr) { *err = true; return false; }              // chars non-null if len>0
        std::memcpy(out->data, chars, static_cast<size_t>(n) * 2);
    }
    out->len = n;
    return true;
}
// Presence-only check for an assignment string (non-empty), same exact-class / bounds.
inline bool ref_string_present(void *o, void *f, bool *err) {
    void *s = read_ref(o, f);
    if (s == nullptr) return false;
    if (!exact(s, g_config.class_string)) { *err = true; return false; }
    int32_t n = str_len(s);
    if (n < 0 || static_cast<uint32_t>(n) > kMaxStr) { *err = true; return false; }
    if (n > 0 && str_chars(s) == nullptr) { *err = true; return false; }
    return n > 0;
}

// --- RAII rooting -----------------------------------------------------------------
// Single owned GC root; freed exactly once on scope exit. reset() frees the previous
// handle before taking a new one (the egg-choice churn), so a tick never grows roots.
// Noncopyable: a copy would double-free the handle on the second destructor.
struct Root {
    void *h = nullptr;
    Root() = default;
    Root(const Root &) = delete;
    Root &operator=(const Root &) = delete;
    void set(void *obj, bool *err) {
        if (h) { gc_free(h); h = nullptr; }
        if (obj == nullptr) { *err = true; return; }
        h = gc_new(obj);
        if (h == nullptr) *err = true;      // never fabricate a root
    }
    void *target() const { return h ? gc_target(h) : nullptr; }
    ~Root() { if (h) gc_free(h); }
};

// --- status / telemetry writers (Unity thread) ------------------------------------
void set_status(uint32_t code) {
    if (g_status.exchange(code) != code)
        g_status_serial.fetch_add(1, std::memory_order_release);
}
void set_status_distance(uint32_t code, double km) {
    uint64_t bits; std::memcpy(&bits, &km, 8);
    g_distance_bits.store(bits, std::memory_order_release);
    set_status(code);
}
void note_fault() { g_fault_serial.fetch_add(1, std::memory_order_release); }

// --- pending teardown (Unity thread) ----------------------------------------------
// Free only the durable GC roots; leave the uncertain pending IDs intact.
void release_roots() {
    for (int i = 0; i < g_st.handle_count; ++i)
        if (g_st.handles[i]) { gc_free(g_st.handles[i]); g_st.handles[i] = nullptr; }
    g_st.handle_count = 0;
    g_root_count.store(0, std::memory_order_release);
}
// Full reset: drop the roots AND the pending assignment (confirmed or freshly superseded).
void pending_reset() {
    release_roots();
    g_st.pending = false;
    g_st.pending_egg = 0;
    g_st.pending_deadline = 0;
    g_st.pending_inc.len = 0;
}
// Append one durable pending root; returns false (fail closed) if a handle can't be made.
bool pending_hold(void *obj) {
    if (obj == nullptr || g_st.handle_count >= 4) return false;
    void *h = gc_new(obj);
    if (h == nullptr) return false;
    g_st.handles[g_st.handle_count++] = h;
    g_root_count.store(static_cast<uint32_t>(g_st.handle_count), std::memory_order_release);
    return true;
}

// --- bounded enumerator -----------------------------------------------------------
// Walks collection via GetEnumerator/MoveNext/get_Current, rooting each element across
// the visit and freeing that handle immediately (so 25000 eggs never accumulate roots).
// Dispose runs on completion AND on error, including when the enumerator could not be
// rooted (best-effort immediate invoke on the still-valid returned object first). visit
// returns false to stop early. *err is set on any managed exception, null/collection
// box, bad-boolean box, size bound (limit) or root failure.
template <typename Visit>
void enumerate(void *collection, unsigned limit, bool *err, Visit &&visit) {
    if (collection == nullptr) { *err = true; return; }   // collection non-null before enumerator
    void *ch = gc_new(collection);
    if (ch == nullptr) { *err = true; return; }

    void *enumer = invoke_virtual(g_config.m_enumerate, collection, err);
    if (*err || enumer == nullptr) { if (!*err) *err = true; gc_free(ch); return; }
    void *eh = gc_new(enumer);
    if (eh == nullptr) {
        bool disp = false;                                // Dispose the still-valid enumerator
        invoke_virtual(g_config.m_dispose, enumer, &disp);// (best effort, before any other call)
        *err = true; gc_free(ch); return;
    }

    for (unsigned i = 0;; ++i) {
        void *box = invoke_virtual(g_config.m_move, gc_target(eh), err);
        if (*err) break;
        if (box == nullptr) { *err = true; break; }        // MoveNext returned no box
        if (!exact(box, g_config.class_boolean)) { *err = true; break; } // must be System.Boolean
        void *bh = gc_new(box);                            // root the box across the unbox
        if (bh == nullptr) { *err = true; break; }
        void *boxed = gc_target(bh);
        void *raw = boxed ? obj_unbox(boxed) : nullptr;
        uint8_t bv = (raw != nullptr) ? (*reinterpret_cast<uint8_t *>(raw) & 0xff) : 0xff;
        gc_free(bh);
        if (raw == nullptr || bv > 1) { *err = true; break; } // null box / non-0/1 bool fail closed
        if (bv == 0) break;                                // MoveNext false -> done
        if (i == limit) { *err = true; break; }            // bound exceeded (fail closed)
        void *value = invoke_virtual(g_config.m_current, gc_target(eh), err);
        if (*err) break;
        void *vh = (value != nullptr) ? gc_new(value) : nullptr;
        if (vh == nullptr) { *err = true; break; }
        bool keep_going = visit(gc_target(vh));
        gc_free(vh);
        if (*err || !keep_going) break;
    }

    bool disp_err = false;                                 // Dispose must run regardless
    invoke_virtual(g_config.m_dispose, gc_target(eh), &disp_err);
    if (disp_err) *err = true;
    gc_free(eh);
    gc_free(ch);
}

// Resolve a contract through the DI container and verify the concrete identity. The
// System.Type argument is rooted across the invoke; the resolved object is kept in the
// caller's Root and re-checked exact before any use.
void *resolve(void *container, void *contract_cls, void *concrete_cls, Root *root, bool *err) {
    if (container == nullptr) { *err = true; return nullptr; }
    void *type = type_obj(cls_type(contract_cls));
    if (type == nullptr) { *err = true; return nullptr; }
    Root type_root; type_root.set(type, err);             // keep the Type object alive over invoke
    if (*err) return nullptr;
    void *params[1] = { type_root.target() };
    if (!params[0]) { *err = true; return nullptr; }
    void *o = invoke(g_config.m_resolve, container, params, err);
    if (*err) return nullptr;
    root->set(o, err);                                     // keep across later invokes
    if (*err) return nullptr;
    if (!exact(root->target(), concrete_cls)) { *err = true; return nullptr; } // service-type mismatch
    return root->target();
}

// --- the orchestration pass (Unity thread, no exceptions) -------------------------
// deadline is an absolute CLOCK_BOOTTIME ms watermark for the single managed request.
void orchestrate(uint64_t now, uint64_t deadline, int longest) {
    bool err = false;
    Root map_type_h, map_h, container_h, player_h, bag_h, service_h, free_h, choice_h;
    IncId free_id;
    uint64_t choice_id = 0;
    double choice_km = 0;
    bool have_free = false, have_choice = false, matched = false;

    // FindObjectOfType(typeof(MapSceneContext)) -> soft return when the map is absent.
    void *map_type = type_obj(cls_type(g_config.class_map));
    if (map_type == nullptr) { note_fault(); g_st.blocked = true;
        set_status(PGRAZOR_INC_STATUS_ERROR_RETRY); return; }
    map_type_h.set(map_type, &err); if (err) goto fail;    // root the System.Type over the invoke
    {
        void *map_params[1] = { map_type_h.target() };
        if (!map_params[0]) { err = true; goto fail; }
        void *map = invoke(g_config.m_find, nullptr, map_params, &err);
        if (err) goto fail;
        if (map == nullptr) return;                        // no map yet (not an error)
        map_h.set(map, &err); if (err) goto fail;

        void *container = invoke(g_config.m_container, map_h.target(), nullptr, &err);
        if (err) goto fail;
        container_h.set(container, &err); if (err) goto fail;

        void *player = resolve(container_h.target(), g_config.class_player_contract,
                               g_config.class_player, &player_h, &err);
        if (err) goto fail;

        // --- incubators: confirm any pending assignment and find the free unlimited one.
        void *incubators = invoke(g_config.m_incubators, player, nullptr, &err);
        if (err) goto fail;
        if (incubators == nullptr) { err = true; goto fail; }
        enumerate(incubators, kMaxInc, &err, [&](void *o) -> bool {
            if (!exact(o, g_config.class_inc)) { err = true; return false; } // proto
            int32_t item = read_s32(o, g_config.f_inc_item);
            IncId id; if (!read_incid(o, g_config.f_inc_id, &id, &err)) return false;
            uint64_t assigned = read_u64(o, g_config.f_inc_pokemon);
            if (g_st.pending && incid_eq(id, g_st.pending_inc)) {
                if (assigned != 0) {
                    matched = true;
                    bool ours = (assigned == g_st.pending_egg);
                    pending_reset();                       // inventory confirmed -> drop IDs + roots
                    g_st.blocked = false;
                    set_status(ours ? PGRAZOR_INC_STATUS_EGG_INCUBATING
                                     : PGRAZOR_INC_STATUS_ALREADY_IN_USE);
                }
            }
            if (!have_free && item == g_config.unlimited && id.len > 0 && assigned == 0 &&
                read_u64(o, g_config.f_inc_expiry) == 0) {
                free_h.set(o, &err); if (err) return false;
                free_id = id; have_free = true;
            }
            return true;
        });
        if (err) goto fail;
        if (matched) return;                               // inventory confirmed -> done

        if (g_st.pending) {
            // Confirmation still outstanding. Never retry / duplicate an uncertain request;
            // the next poll re-enumerates and can confirm (IDs persist process lifetime).
            if (now >= g_st.pending_deadline) { g_st.blocked = true;
                set_status(PGRAZOR_INC_STATUS_UNCONFIRMED); }
            else set_status(PGRAZOR_INC_STATUS_CONFIRMING);
            return;
        }
        if (g_st.blocked) { set_status(PGRAZOR_INC_STATUS_STOPPED_RETRY); return; }
        if (!have_free) { set_status(PGRAZOR_INC_STATUS_NO_FREE); return; }

        // --- eggs: choose shortest/longest eligible (first stable tie).
        void *bag = resolve(container_h.target(), g_config.class_bag_contract,
                            g_config.class_bag, &bag_h, &err);
        if (err) goto fail;
        void *eggs = invoke(g_config.m_eggs, bag, nullptr, &err);
        if (err) goto fail;
        if (eggs == nullptr) { err = true; goto fail; }
        enumerate(eggs, kMaxEggs, &err, [&](void *o) -> bool {
            if (!exact(o, g_config.class_pk)) { err = true; return false; } // egg-proto
            if (!read_bool(o, g_config.f_egg_is, &err)) return !err;         // not an egg (or bad bool)
            if (read_s32(o, g_config.f_egg_type) == g_config.special) return true; // excluded
            bool present = ref_string_present(o, g_config.f_egg_assignment, &err);
            if (err) return false;
            if (present) return true;                      // already assigned
            uint64_t id = read_u64(o, g_config.f_egg_id);
            double km = read_dbl(o, g_config.f_egg_target) - read_dbl(o, g_config.f_egg_start);
            if (id == 0 || !std::isfinite(km) || km <= 0 || km > 100) return true;
            if (!have_choice || (longest ? km > choice_km : km < choice_km)) {
                choice_h.set(o, &err); if (err) return false;
                choice_id = id; choice_km = km; have_choice = true;
            }
            return true;
        });
        if (err) goto fail;
        if (!have_choice) { set_status(PGRAZOR_INC_STATUS_NO_EGGS);
            g_st.next_ms = now + kNoEggsMs; return; }

        // Recheck both assignment fields immediately before resolving the service.
        if (!g_st.enabled || g_revoke) return;             // settings flipped / reentrant stop
        {
            void *free_obj = free_h.target();
            void *choice_obj = choice_h.target();
            if (free_obj == nullptr || choice_obj == nullptr) { err = true; goto fail; }
            if (read_u64(free_obj, g_config.f_inc_pokemon) != 0) return;
            bool present = ref_string_present(choice_obj, g_config.f_egg_assignment, &err);
            if (err) goto fail;                            // invalid assignment string -> fail closed
            if (present) return;
        }

        void *service = resolve(container_h.target(), g_config.class_service_contract,
                               g_config.class_service, &service_h, &err);
        if (err) goto fail;

        // Resolving ran managed code; recheck AFTER it too, on refreshed targets.
        void *free_obj = free_h.target();
        void *choice_obj = choice_h.target();
        if (free_obj == nullptr || choice_obj == nullptr) { err = true; goto fail; }
        if (read_u64(free_obj, g_config.f_inc_pokemon) != 0) return;
        {
            bool present = ref_string_present(choice_obj, g_config.f_egg_assignment, &err);
            if (err) goto fail;
            if (present) return;
        }
        if (!g_st.enabled || g_revoke) return;             // stop may have arrived during resolve

        // Lease deadline re-check immediately before the single request (fix: an expired
        // residual lease aborts safely with no managed call, even if it elapsed mid-pass).
        { uint64_t t = boottime_ms(); if (t == 0 || t >= deadline) return; }

        // Record the pending assignment (durable roots) BEFORE the request, so a managed
        // exception from the call keeps us in the uncertain/confirming state.
        pending_reset();
        if (!pending_hold(choice_obj) || !pending_hold(free_obj) || !pending_hold(service)) {
            pending_reset(); err = true; goto fail;        // partial root failure -> fail closed
        }
        // Root allocation can reenter the host or consume the remaining lease.
        // No request has been issued yet, so cancellation here can drop all pending roots.
        const uint64_t submit_now = boottime_ms();
        service = service_h.target();
        free_obj = free_h.target();
        if (!g_st.enabled || g_revoke || submit_now == 0 || submit_now >= deadline) {
            pending_reset(); return;
        }
        if (!service || !free_obj) { pending_reset(); err = true; goto fail; }
        g_st.pending = true;
        g_st.pending_inc = free_id;
        g_st.pending_egg = choice_id;
        g_st.pending_deadline = now + kDeadlineMs;
        set_status_distance(PGRAZOR_INC_STATUS_STARTING, choice_km);
        g_longest.store(longest ? 1u : 0u, std::memory_order_release);

        // Count the issued invocation exactly once, BEFORE the call, because the server may
        // observe the request even if the managed call then throws. Confirmation of success
        // only ever comes from live inventory, never from this counter or the returned task.
        g_request_serial.fetch_add(1, std::memory_order_release);

        uint64_t id_arg = choice_id;
        void *put_params[2] = { &id_arg, free_obj };
        void *promise = invoke(g_config.m_put, service, put_params, &err);
        if (err) goto fail;                                // pending roots deliberately kept
        if (promise != nullptr && !pending_hold(promise)) {
            // Could not root the returned task: report uncertain (not silent) and keep the
            // pending assignment; a later inventory poll still decides the outcome.
            note_fault();
            set_status(PGRAZOR_INC_STATUS_UNCONFIRMED);
        }
        return;
    }

fail:
    // Managed exception / fail-closed error. Block and surface the uncertain state if a
    // request may already be in flight; keep the pending roots in that case.
    g_st.blocked = true;
    set_status(g_st.pending ? PGRAZOR_INC_STATUS_UNCONFIRMED : PGRAZOR_INC_STATUS_ERROR_RETRY);
    note_fault();
}

// --- C API table functions --------------------------------------------------------
bool config_ok(const pgrazor_incubation_config_v1 *c) {
    const void *const ptrs[] = {
        c->runtime_invoke, c->gchandle_new, c->gchandle_get_target, c->gchandle_free,
        c->object_get_class, c->object_unbox, c->string_length, c->string_chars,
        c->object_get_virtual_method, c->class_get_type, c->type_get_object, c->field_get_value,
        c->m_find, c->m_container, c->m_resolve, c->m_incubators, c->m_eggs, c->m_put,
        c->m_enumerate, c->m_move, c->m_current, c->m_dispose,
        c->class_map, c->class_player_contract, c->class_player, c->class_bag_contract,
        c->class_bag, c->class_inc, c->class_pk, c->class_service_contract, c->class_service,
        c->f_inc_item, c->f_inc_id, c->f_inc_pokemon, c->f_inc_expiry, c->f_egg_is,
        c->f_egg_type, c->f_egg_assignment, c->f_egg_id, c->f_egg_target, c->f_egg_start,
        c->class_boolean, c->class_string};
    for (const void *p : ptrs) if (p == nullptr) return false;
    return true;
}

int api_install(const pgrazor_incubation_config_v1 *config) {
    if (config == nullptr) return PGRAZOR_INC_ERR_NULL_ARG;
    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (config->version != PGRAZOR_INCUBATION_API_VERSION ||
        config->size != sizeof(pgrazor_incubation_config_v1) || !config_ok(config))
        return PGRAZOR_INC_ERR_BAD_CONFIG;
    if (g_installed.load(std::memory_order_acquire))
        return (std::memcmp(&g_config, config, sizeof(g_config)) == 0)
                   ? PGRAZOR_INC_OK : PGRAZOR_INC_ERR_ALREADY_BOUND;
    g_config = *config;
    g_installed.store(true, std::memory_order_release);
    return PGRAZOR_INC_OK;
}

int api_tick(int enabled, int longest, uint64_t remaining_lease_ms, uint64_t proven_tid) {
    if (!g_installed.load(std::memory_order_acquire)) return PGRAZOR_INC_ERR_NOT_INSTALLED;
    if (!bind_unity_thread(proven_tid)) return PGRAZOR_INC_OK;

    // Claim ownership. A nested call on this thread, or any concurrent caller supplying
    // its own tid, loses the exchange and must not run / steal the owner's state.
    uint64_t expected = 0;
    if (!g_owner_tid.compare_exchange_strong(expected, proven_tid,
                                             std::memory_order_acq_rel)) {
        if (expected == proven_tid && (!enabled || remaining_lease_ms == 0)) {
            g_revoke = true;
            if (!enabled) g_st.enabled = false;
        }
        return PGRAZOR_INC_OK;
    }
    g_revoke = false;
    g_teardown_defer = false;

    if (!enabled) {
        g_st.enabled = false;
        set_status(PGRAZOR_INC_STATUS_OFF);                // keep pending for later confirmation
    } else {
        if (!g_st.enabled) { g_st.enabled = true; if (!g_st.pending) g_st.blocked = false; g_st.next_ms = 0; }

        uint64_t lease = remaining_lease_ms;
        if (lease > kMaxLeaseMs) lease = kMaxLeaseMs;      // clamp the residual window
        if (lease == 0) {
            set_status(PGRAZOR_INC_STATUS_WAIT_MAP);
        } else {
            uint64_t now = boottime_ms();
            if (now == 0) {
                set_status(PGRAZOR_INC_STATUS_WAIT_MAP);
            } else if (now >= g_st.next_ms) {
                g_st.next_ms = now + kPollMs;
                orchestrate(now, now + lease, longest);    // absolute CLOCK_BOOTTIME deadline
            }
        }
    }

    // A reentrant clear that arrived during this pass deferred its free to here.
    if (g_teardown_defer) { release_roots(); g_st.enabled = false; }
    g_revoke = false;
    g_teardown_defer = false;
    g_owner_tid.store(0, std::memory_order_release);
    return PGRAZOR_INC_OK;
}

int api_clear(uint64_t proven_tid) {
    if (!g_installed.load(std::memory_order_acquire)) return PGRAZOR_INC_ERR_NOT_INSTALLED;
    if (!on_unity_thread(proven_tid) || g_unity_tid.load(std::memory_order_acquire) != proven_tid)
        return PGRAZOR_INC_OK; // never bind/steal ownership during cleanup

    uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
    if (owner != 0) {
        if (owner == proven_tid) {
            // Reentrant clear from inside our own pass: revoke any further submission and
            // defer the root free to the pass epilogue (do not free underneath it).
            g_revoke = true;
            g_teardown_defer = true;
            g_st.enabled = false;
        }
        // owner != proven_tid: a different thread owns the pass; do not steal or free.
        return PGRAZOR_INC_OK;
    }

    // No pass in progress: become the transient owner and release roots now. The uncertain
    // pending IDs are PRESERVED (by value) for post-restart inventory confirmation.
    uint64_t expected = 0;
    if (!g_owner_tid.compare_exchange_strong(expected, proven_tid,
                                             std::memory_order_acq_rel)) return PGRAZOR_INC_OK;
    release_roots();
    g_st.enabled = false;
    g_owner_tid.store(0, std::memory_order_release);
    return PGRAZOR_INC_OK;
}

int api_sample(pgrazor_incubation_sample_v1 *out) {
    if (out == nullptr) return PGRAZOR_INC_ERR_NULL_ARG;
    out->version = PGRAZOR_INCUBATION_API_VERSION;
    out->size = sizeof(pgrazor_incubation_sample_v1);
    out->status = g_status.load(std::memory_order_acquire);
    out->status_serial = g_status_serial.load(std::memory_order_acquire);
    out->request_serial = g_request_serial.load(std::memory_order_acquire);
    uint64_t bits = g_distance_bits.load(std::memory_order_acquire);
    std::memcpy(&out->distance, &bits, 8);
    out->longest = g_longest.load(std::memory_order_acquire);
    out->fault_serial = g_fault_serial.load(std::memory_order_acquire);
    out->owned_roots = g_root_count.load(std::memory_order_acquire);
    out->reserved = 0;
    return PGRAZOR_INC_OK;
}

const pgrazor_incubation_api_v1 g_api = {
    PGRAZOR_INCUBATION_API_VERSION,
    static_cast<uint32_t>(sizeof(pgrazor_incubation_api_v1)),
    &api_install,
    &api_tick,
    &api_clear,
    &api_sample,
};

} // namespace

const void *pgrazor_incubation_api(uint32_t version) {
    if (version != PGRAZOR_INCUBATION_API_VERSION) return nullptr;
    return &g_api;
}

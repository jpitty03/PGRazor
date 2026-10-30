// PGRazor native migration: inventory item catalog + auto-recycle (feature id 14).
//
// Full native port of runtime/features/items.js. See items.h for the ABI and the division of
// labour with runtime/native-items.js. Built -fno-exceptions / -fno-rtti like every sibling:
// JS "throw" paths become fail-closed error flags and RAII destructors free each managed root
// exactly once on scope exit.
//
// Threading model (the invariants this file enforces):
//   * tick() and all five hooks run on the game's Unity thread. tick latches a permanent Unity
//     identity (g_unity_tid) once, separate from the non-blocking per-pass owner guard
//     (g_owner_tid) and the frame depth (g_depth) which includes the result/error/recycle
//     callbacks that fire INSIDE our own RecycleItem invoke. A foreign thread never steals
//     ownership and a nested tick is a no-op.
//   * No mutex is ever held across a managed call (runtime_invoke) or across an original
//     trampoline. The install mutex only guards hook publication; the catalog mutex only guards
//     a plain memcpy. All mutable orchestration state is owner-thread-only; telemetry is atomic
//     so sample()/catalog()/service_handle() are lock-free and never free a handle.
//   * Only the Unity thread creates or frees GC handles. clear() off the proven thread is a
//     no-op; durable roots are freed by the owner, the uncertain pending kept by value. A
//     reentrant clear from inside the owner's pass defers the free to the pass epilogue.

#include "items.h"
#include "hook_engine.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

static_assert(sizeof(void *) == 8, "items native ABI requires LP64");
static_assert(sizeof(pgrazor_items_config_v1) == 432, "config must be 432 bytes");
static_assert(sizeof(pgrazor_items_sample_v1) == 56,  "sample must be 56 bytes");
static_assert(sizeof(pgrazor_items_api_v1)    == 72,  "table must be 72 bytes");
static_assert(sizeof(pgrazor_item_row)        == 8,   "row must be 8 bytes");
static_assert(sizeof(pgrazor_item_cap)        == 8,   "cap must be 8 bytes");
static_assert(sizeof(pgrazor_item_event)      == 16,  "event must be 16 bytes");
static_assert(sizeof(pgrazor_items_rules_v1)  == 32 + PGRAZOR_ITEMS_MAX_RULES * 8,
              "rules snapshot must be 4128 bytes");
static_assert(offsetof(pgrazor_items_config_v1, has_lookup)     == 8,   "layout");
static_assert(offsetof(pgrazor_items_config_v1, runtime_invoke) == 16,  "layout (first core ptr)");
static_assert(offsetof(pgrazor_items_config_v1, f_status)       == 272, "layout (last core ptr)");
static_assert(offsetof(pgrazor_items_config_v1, class_get_type) == 280, "layout (first lookup ptr)");
static_assert(offsetof(pgrazor_items_config_v1, func_type)      == 416, "layout (last lookup ptr)");
static_assert(offsetof(pgrazor_items_config_v1, success)        == 424, "layout");
static_assert(offsetof(pgrazor_items_rules_v1, generation)      == 16,  "layout");
static_assert(offsetof(pgrazor_items_rules_v1, entries)         == 32,  "layout");
static_assert(offsetof(pgrazor_items_sample_v1, bag_ready_serial) == 8,  "layout");
static_assert(offsetof(pgrazor_items_sample_v1, events_lost)      == 32, "layout");
static_assert(offsetof(pgrazor_items_sample_v1, has_candidate)    == 40, "layout");
static_assert(offsetof(pgrazor_items_sample_v1, owned_roots)      == 44, "layout");
static_assert(offsetof(pgrazor_item_event, aux)                   == 12, "layout");

namespace {

constexpr uint32_t kMaxList    = PGRAZOR_ITEMS_MAX_LIST;
constexpr uint32_t kMaxRows    = PGRAZOR_ITEMS_MAX_ROWS;
constexpr uint32_t kMaxRules   = PGRAZOR_ITEMS_MAX_RULES;
constexpr int      kMaxBlocked = static_cast<int>(PGRAZOR_ITEMS_MAX_BLOCKED);
constexpr int      kMaxQuar    = static_cast<int>(PGRAZOR_ITEMS_MAX_QUAR);
constexpr uint32_t kMaxEvents  = PGRAZOR_ITEMS_MAX_EVENTS;
constexpr int32_t  kMaxQty     = PGRAZOR_ITEMS_MAX_QTY;
constexpr uint64_t kPollMs     = PGRAZOR_ITEMS_POLL_MS;
constexpr uint64_t kBackoffMs  = PGRAZOR_ITEMS_BACKOFF_MS;
constexpr uint64_t kConfirmMs  = PGRAZOR_ITEMS_CONFIRM_MS;
constexpr uint64_t kSettleMs   = PGRAZOR_ITEMS_SETTLE_MS;
constexpr uint64_t kMaxLeaseMs = PGRAZOR_ITEMS_MAX_LEASE_MS;

// Hook signatures (exact game ABI; the trailing void* is the il2cpp MethodInfo*). The bool
// filter arg to the list method arrives in an integer register as int32.
using InitFn    = void  (*)(void *self, void *method);
using ListFn    = void *(*)(void *self, void *p0, int32_t p1, void *p2, void *p3, void *p4, void *method);
using RecycleFn = void *(*)(void *self, void *data, int32_t count, void *expiry, void *method);
using ResultFn  = void  (*)(void *self, void *response, void *method);
using ErrorFn   = void  (*)(void *self, void *str, void *method);

// --- lifetime / publication -------------------------------------------------------
std::mutex                 g_install_mutex;
pgrazor_items_config_v1    g_cfg{};
std::atomic<bool>          g_installed{false};
bool                       g_install_attempted = false;  // install-mutex guarded; failure latches
int                        g_install_error = PGR_ITEMS_ERR_NOT_INSTALLED;
std::atomic<InitFn>        g_orig_init{nullptr};
std::atomic<ListFn>        g_orig_list{nullptr};
std::atomic<RecycleFn>     g_orig_recycle{nullptr};
std::atomic<ResultFn>      g_orig_result{nullptr};
std::atomic<ErrorFn>       g_orig_error{nullptr};

// --- telemetry (pull-only; any thread; never frees) -------------------------------
std::atomic<uint64_t> g_bag_ready_serial{0};
std::atomic<uint64_t> g_catalog_serial{0};
std::atomic<uint64_t> g_cleanup_serial{0};
std::atomic<uint64_t> g_fault_serial{0};
std::atomic<uint64_t> g_events_lost{0};
std::atomic<uint32_t> g_has_candidate{0};
std::atomic<uint32_t> g_root_count{0};
std::atomic<uint32_t> g_active_frames{0};
std::atomic<void *>   g_service_atomic{nullptr};

// Bounded request/confirm/error event queue. Producer is the Unity thread (emit_*); consumer is
// the adapter via drain() on any thread. A tiny mutex guards only the ring-buffer slot copy --
// never a managed call, never a GC free -- so it honours the "no mutex across managed/original"
// invariant. Overflow fails closed: the newest event is dropped, events_lost and fault both bump.
std::mutex          g_event_mutex;
pgrazor_item_event  g_events[kMaxEvents];
uint32_t            g_ev_head = 0;   // event-mutex guarded
uint32_t            g_ev_tail = 0;   // event-mutex guarded
uint32_t            g_ev_size = 0;   // event-mutex guarded

// --- thread identity / reentrancy -------------------------------------------------
std::atomic<uint64_t> g_unity_tid{0};
std::atomic<uint64_t> g_owner_tid{0};
std::atomic<bool>     g_stopping{false};
uint32_t              g_depth = 0;          // owner-thread only, includes callback frames
bool                  g_revoke = false;     // owner-thread only: stop/disable arrived this pass
bool                  g_deferred_clear = false; // owner-thread only: free roots at depth-1 epilogue
uint64_t              g_pending_gen = 0;    // owner-thread only: monotonic request identity source

// --- owner-thread state -----------------------------------------------------------
struct Pending {
    bool     active = false;
    uint64_t gen = 0;              // immutable request identity, matched before AND after original
    int32_t  id = 0;
    int32_t  amount = 0;
    int32_t  cap = 0;
    void    *bag_handle = nullptr; // DURABLE GC root on the bag object (held until resolution);
                                   // matched by target so a reused address can't confirm it
    uint64_t deadline_ms = 0;
    uint64_t epoch = 0;            // recorded for parity; not used for matching
};
struct State {
    void    *service_handle = nullptr;          // durable GC root on the service
    void    *filter_handles[3] = {nullptr, nullptr, nullptr}; // durable roots [0],[1] (([2] null)
    bool     have_filters = false;
    Pending  pending;
    void *scan_bag_handle = nullptr;
    void *scan_handle = nullptr; // large-list snapshot, owner-thread durable root
    int32_t scan_count = 0, scan_cursor = 0;
    uint64_t scan_generation = 0, scan_epoch = 0;
    void *probe_bag = nullptr;
    uint32_t probe_cursor = 0;
    uint64_t probe_generation = 0, probe_epoch = 0;
    uint64_t next_ms = 0;
    bool     have_generation = false;
    uint64_t rules_generation = 0;
    bool     invoking = false;                   // true while WE invoke list/recycle
    // Definite game rejections: paused until a settings/generation change (JS parity).
    int32_t  blocked[kMaxBlocked];
    int      blocked_count = 0;
    // UNCERTAIN outcomes (timeout / manual interference / managed exception / clear-with-pending):
    // quarantined for the process lifetime, NEVER cleared by a generation change.
    int32_t  quar[kMaxQuar];
    int      quar_count = 0;
    bool     fail_closed = false;                // sticky: a quarantine set overflowed -> no recycle
} g_st;

// Persistent catalog (owner-thread). Mirrors the JS rows Map: counts are zeroed then updated
// each poll (depleted items persist at 0), published to a mutex-guarded copy for catalog().
pgrazor_item_row g_rows[kMaxRows];
uint32_t         g_rows_n = 0;
std::mutex       g_catalog_mutex;
pgrazor_item_row g_pub_rows[kMaxRows];
uint32_t         g_pub_n = 0;

// Rules snapshot copied once per pass (owner-thread). The immutable install config is separate.
pgrazor_items_rules_v1 g_snapshot{};

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
inline void *gc_new(void *o)      { return o ? reinterpret_cast<void *(*)(void *, int)>(g_cfg.gchandle_new)(o, 0) : nullptr; }
inline void *gc_target(void *h)   { return h ? reinterpret_cast<void *(*)(void *)>(g_cfg.gchandle_get_target)(h) : nullptr; }
inline void  gc_free(void *h)     { if (h) reinterpret_cast<void (*)(void *)>(g_cfg.gchandle_free)(h); }
inline void *obj_class(void *o)   { return reinterpret_cast<void *(*)(void *)>(g_cfg.object_get_class)(o); }
inline void *obj_unbox(void *o)   { return reinterpret_cast<void *(*)(void *)>(g_cfg.object_unbox)(o); }
inline void  field_get(void *o, void *f, void *out) {
    reinterpret_cast<void (*)(void *, void *, void *)>(g_cfg.field_get_value)(o, f, out);
}
inline void  field_static_get(void *f, void *out) {
    reinterpret_cast<void (*)(void *, void *)>(g_cfg.field_static_get_value)(f, out);
}
inline void *cls_type(void *k)    { return reinterpret_cast<void *(*)(void *)>(g_cfg.class_get_type)(k); }
inline void *type_obj(void *t)    { return reinterpret_cast<void *(*)(void *)>(g_cfg.type_get_object)(t); }
inline void  class_init(void *k)  { reinterpret_cast<void (*)(void *)>(g_cfg.runtime_class_init)(k); }
inline void *method_object(void *m, void *k) {
    return reinterpret_cast<void *(*)(void *, void *)>(g_cfg.method_get_object)(m, k);
}
inline bool exact(void *o, void *k) { return o != nullptr && obj_class(o) == k; }
inline void *read_ref(void *o, void *f) { void *v = nullptr; field_get(o, f, &v); return v; }
inline int32_t read_s32(void *o, void *f) { int32_t v = 0; field_get(o, f, &v); return v; }
inline uint8_t read_u8(void *o, void *f) { uint64_t v = 0; field_get(o, f, &v); return static_cast<uint8_t>(v & 0xff); }

// Invoke a managed method; on a managed exception set *err and return null (JS threw). The two
// static methods accept a null obj; every other method requires one.
inline void *invoke(void *method, void *obj, void **params, bool *err) {
    if (method != g_cfg.m_find && method != g_cfg.m_create_delegate && obj == nullptr) {
        *err = true; return nullptr;
    }
    void *exc = nullptr;
    void *r = reinterpret_cast<void *(*)(void *, void *, void **, void **)>(
        g_cfg.runtime_invoke)(method, obj, params, &exc);
    if (exc != nullptr) { *err = true; return nullptr; }
    return r;
}

// --- RAII rooting -----------------------------------------------------------------
struct Root {
    void *h = nullptr;
    Root() = default;
    Root(const Root &) = delete;
    Root &operator=(const Root &) = delete;
    void set(void *obj, bool *err) {
        if (h) { void *old = h; h = nullptr; gc_free(old); }
        if (obj == nullptr) { *err = true; return; }
        h = gc_new(obj);
        if (h == nullptr) *err = true;      // never fabricate a root
    }
    void *target() const { return h ? gc_target(h) : nullptr; }
    ~Root() { void *old = h; h = nullptr; gc_free(old); }
};

// Boxed System.Int32 read: root the box across the unbox, exact-class check, then read.
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

void note_fault() { g_fault_serial.fetch_add(1, std::memory_order_release); }

// Enqueue one request/confirm/error event (Unity thread). The mutex guards only the slot copy.
// On overflow we fail closed: drop the newest event and raise events_lost + fault so the adapter
// never silently believes a stale "last" value belongs to a later serial.
void push_event(uint32_t kind, int32_t id, int32_t amount, int32_t aux) {
    std::lock_guard<std::mutex> lock(g_event_mutex);
    if (g_ev_size >= kMaxEvents) {
        g_st.fail_closed = true;
        g_revoke = true;
        g_events_lost.fetch_add(1, std::memory_order_release);
        g_fault_serial.fetch_add(1, std::memory_order_release);
        return;
    }
    g_events[g_ev_tail] = pgrazor_item_event{kind, id, amount, aux};
    g_ev_tail = (g_ev_tail + 1) % kMaxEvents;
    ++g_ev_size;
}

// --- telemetry writers (Unity thread) ---------------------------------------------
void emit_request(int32_t id, int32_t amount, int32_t keep) { push_event(PGR_ITEMS_EV_REQUEST, id, amount, keep); }
void emit_confirm(int32_t id, int32_t amount)               { push_event(PGR_ITEMS_EV_CONFIRM, id, amount, 0); }
void emit_error(int32_t reason, int32_t id, int32_t amount) { push_event(PGR_ITEMS_EV_ERROR, id, amount, reason); }
void emit_bag_ready() { g_bag_ready_serial.fetch_add(1, std::memory_order_release); }
void set_has_candidate(bool v) { g_has_candidate.store(v ? 1u : 0u, std::memory_order_release); }

// --- quarantine sets (Unity thread) -----------------------------------------------
// blocked[]: definite game rejections, cleared on a config generation change (JS parity).
// quar[]:    uncertain outcomes, NEVER cleared by a generation change (fail-safe quarantine).
// Either set overflowing latches g_st.fail_closed so no further recycle is ever issued -- a full
// quarantine set must never silently permit the next request.
bool blocked_contains(int32_t id) {
    for (int i = 0; i < g_st.blocked_count; ++i) if (g_st.blocked[i] == id) return true;
    return false;
}
void blocked_add(int32_t id) {
    if (id <= 0 || blocked_contains(id)) return;
    if (g_st.blocked_count >= kMaxBlocked) { g_st.fail_closed = true; note_fault(); return; }
    g_st.blocked[g_st.blocked_count++] = id;
}
void blocked_clear() { g_st.blocked_count = 0; }

bool quar_contains(int32_t id) {
    for (int i = 0; i < g_st.quar_count; ++i) if (g_st.quar[i] == id) return true;
    return false;
}
void quar_add(int32_t id) {
    if (id <= 0 || quar_contains(id)) return;
    if (g_st.quar_count >= kMaxQuar) { g_st.fail_closed = true; note_fault(); return; }
    g_st.quar[g_st.quar_count++] = id;
}
bool item_paused(int32_t id) { return blocked_contains(id) || quar_contains(id); }

// --- durable root accounting (Unity thread) ---------------------------------------
void refresh_root_count() {
    uint32_t n = 0;
    if (g_st.service_handle) ++n;
    for (int i = 0; i < 3; ++i) if (g_st.filter_handles[i]) ++n;
    if (g_st.pending.bag_handle) ++n;
    if (g_st.scan_handle) ++n;
    if (g_st.scan_bag_handle) ++n;
    if (g_st.probe_bag) ++n;
    g_root_count.store(n, std::memory_order_release);
    g_service_atomic.store(g_st.service_handle, std::memory_order_release);
}
// Detach every slot BEFORE freeing: a GC reentry triggered by gc_free must never observe a slot
// that still points at a handle we are in the middle of freeing (double-free / stale borrow).
void release_filters() {
    for (int i = 0; i < 3; ++i) { void *h = g_st.filter_handles[i]; g_st.filter_handles[i] = nullptr; gc_free(h); }
    g_st.have_filters = false;
}
// Resolve the pending request: detach then free its bag root. quarantine=true permanently pins the
// id (uncertain outcome) so a restart can never resubmit it; a reused bag address cannot confirm a
// different RPC because the id is also quarantined and the old root is gone.
void clear_pending(bool quarantine) {
    if (quarantine && g_st.pending.active) quar_add(g_st.pending.id);
    void *h = g_st.pending.bag_handle;
    g_st.pending.bag_handle = nullptr;
    g_st.pending.active = false;
    gc_free(h);
}
void release_probe() {
    void *bag = g_st.probe_bag; g_st.probe_bag = nullptr;
    g_st.probe_cursor = 0; gc_free(bag); refresh_root_count();
}
void release_scan() {
    release_probe();
    void *h = g_st.scan_handle; g_st.scan_handle = nullptr;
    void *bag = g_st.scan_bag_handle; g_st.scan_bag_handle = nullptr;
    g_st.scan_count = g_st.scan_cursor = 0;
    gc_free(h); gc_free(bag);
    refresh_root_count();
}
void release_roots() {
    release_scan();
    void *service = g_st.service_handle; g_st.service_handle = nullptr;
    release_filters();
    gc_free(service);
    clear_pending(true);                 // in-flight recycle at teardown -> drop attribution + pin
    refresh_root_count();
}

struct OwnerScope {
    bool active;
    explicit OwnerScope(bool enabled = true) : active(enabled) {
        if (active) { ++g_depth; g_active_frames.store(g_depth, std::memory_order_release); }
    }
    ~OwnerScope() {
        if (!active) return;
        if (g_depth == 1 && g_deferred_clear) {
            // Stay "busy" while GC frees run; a reentrant clear can only defer, never free here.
            g_deferred_clear = false;
            release_roots();
            g_deferred_clear = false;
        }
        --g_depth;
        g_active_frames.store(g_depth, std::memory_order_release);
    }
};

// --- catalog publication (Unity thread -> mutex-guarded copy; no managed calls) ----
void publish_catalog() {
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    uint32_t n = g_rows_n; if (n > kMaxRows) n = kMaxRows;
    if (g_catalog_serial.load(std::memory_order_relaxed) != 0 && g_pub_n == n &&
        std::memcmp(g_pub_rows, g_rows, static_cast<size_t>(n) * sizeof(pgrazor_item_row)) == 0) return;
    std::memcpy(g_pub_rows, g_rows, static_cast<size_t>(n) * sizeof(pgrazor_item_row));
    g_pub_n = n;
    g_catalog_serial.fetch_add(1, std::memory_order_release);
}

// Find or add a row id, update its count (JS rows.set semantics). Bounded; fails closed never.
void row_update(int32_t id, int32_t count) {
    for (uint32_t i = 0; i < g_rows_n; ++i) if (g_rows[i].id == id) { g_rows[i].count = count; return; }
    if (g_rows_n < kMaxRows) { g_rows[g_rows_n].id = id; g_rows[g_rows_n].count = count; ++g_rows_n; }
}

bool rule_cap(int32_t id, int32_t *cap) {
    uint32_t n = g_snapshot.count; if (n > kMaxRules) n = kMaxRules;
    for (uint32_t i = 0; i < n; ++i) if (g_snapshot.entries[i].id == id) { *cap = g_snapshot.entries[i].cap; return true; }
    return false;
}

// --- service / filter lookup (optional; Unity thread) -----------------------------
// Mirrors runtime/features/items.js lines 15-23: FindObjectOfType(map) -> get_Container ->
// Resolve(IItemInventoryService) and, the first time, build the two filter delegates from the
// gui service and its singleton. A soft return (no map / no container / not-our-service) leaves
// state untouched; any managed/identity failure emits a lookup error and backs off.
void do_lookup(uint64_t now) {
    bool err = false;
    Root map_type, map_h, container_h, gui_type, gui_h, singleton_h, fn_type_h;
    void *new_filters[2] = {nullptr, nullptr};

    void *mt = type_obj(cls_type(g_cfg.class_map));
    if (mt == nullptr) { err = true; goto lerr; }
    map_type.set(mt, &err); if (err) goto lerr;
    {
        void *mp[1] = { map_type.target() };
        if (!mp[0]) { err = true; goto lerr; }
        void *map = invoke(g_cfg.m_find, nullptr, mp, &err);
        if (err) goto lerr;
        if (map == nullptr) return;                        // no map yet (soft)
        map_h.set(map, &err); if (err) goto lerr;
    }

    void *container;
    container = invoke(g_cfg.m_container, map_h.target(), nullptr, &err);
    if (err) goto lerr;
    if (container == nullptr) return;                      // no container yet (soft)
    container_h.set(container, &err); if (err) goto lerr;

    {
        void *st = type_obj(cls_type(g_cfg.class_service_contract));
        if (st == nullptr) { err = true; goto lerr; }
        Root st_r; st_r.set(st, &err); if (err) goto lerr;
        void *sp[1] = { st_r.target() };
        if (!sp[0]) { err = true; goto lerr; }
        void *service = invoke(g_cfg.m_resolve, container_h.target(), sp, &err);
        if (err) goto lerr;
        if (!exact(service, g_cfg.class_service)) return;  // not our service (soft, JS falls through)
        // Replacing the service object invalidates the filter delegates built from the old
        // container; detach-then-free the old root and drop the stale filters so they rebuild.
        if (g_st.service_handle) {
            bool same = gc_target(g_st.service_handle) == service;
            void *old = g_st.service_handle; g_st.service_handle = nullptr; gc_free(old);
            if (!same) { release_filters(); release_scan(); }
        }
        g_st.service_handle = gc_new(service);
        if (g_st.service_handle == nullptr) { err = true; goto lerr; }
    }

    if (!g_st.have_filters) {
        void *gt = type_obj(cls_type(g_cfg.class_gui_contract));
        if (gt == nullptr) { err = true; goto lerr; }
        gui_type.set(gt, &err); if (err) goto lerr;
        void *gp[1] = { gui_type.target() };
        if (!gp[0]) { err = true; goto lerr; }
        void *gui = invoke(g_cfg.m_resolve, container_h.target(), gp, &err);
        if (err) goto lerr;
        if (!exact(gui, g_cfg.class_gui)) { err = true; goto lerr; } // item-gui-target
        gui_h.set(gui, &err); if (err) goto lerr;

        class_init(g_cfg.class_singleton);
        void *singleton = nullptr;
        field_static_get(g_cfg.f_singleton_instance, &singleton);
        if (!exact(singleton, g_cfg.class_singleton)) { err = true; goto lerr; } // item-filter-target
        singleton_h.set(singleton, &err); if (err) goto lerr;

        void *ft = type_obj(g_cfg.func_type);
        if (ft == nullptr) { err = true; goto lerr; }
        fn_type_h.set(ft, &err); if (err) goto lerr;

        void *methods[2]  = { g_cfg.m_usable, g_cfg.m_show };
        void *klasses[2]  = { g_cfg.class_gui, g_cfg.class_singleton };
        Root *target_roots[2] = { &gui_h, &singleton_h };
        for (int i = 0; i < 2; ++i) {
            void *reflection = method_object(methods[i], klasses[i]);
            Root refl; refl.set(reflection, &err);
            if (err) { for (int j = 0; j < i; ++j) gc_free(new_filters[j]); goto lerr; }
            // Re-target the delegate's bound instance NOW: method_object() above may have moved it.
            void *dp[3] = { fn_type_h.target(), target_roots[i]->target(), refl.target() };
            if (!dp[0] || !dp[1] || !dp[2]) {
                err = true; for (int j = 0; j < i; ++j) gc_free(new_filters[j]); goto lerr;
            }
            void *fn = invoke(g_cfg.m_create_delegate, nullptr, dp, &err);
            if (err || fn == nullptr) {
                err = true; for (int j = 0; j < i; ++j) gc_free(new_filters[j]); goto lerr;
            }
            new_filters[i] = gc_new(fn);
            if (new_filters[i] == nullptr) {
                err = true; for (int j = 0; j < i; ++j) gc_free(new_filters[j]); goto lerr;
            }
        }
        release_filters();
        g_st.filter_handles[0] = new_filters[0];
        g_st.filter_handles[1] = new_filters[1];
        g_st.filter_handles[2] = nullptr;
        g_st.have_filters = true;
    }

    emit_bag_ready();
    refresh_root_count();
    return;

lerr:
    emit_error(PGR_ITEMS_REASON_LOOKUP, 0, 0);
    note_fault();
    g_st.next_ms = now + kBackoffMs;
    refresh_root_count();
}

// --- catalog enumeration + the single recycle (Unity thread, no exceptions) --------
// deadline is an absolute CLOCK_BOOTTIME ms watermark for the single RecycleItem.
void enumerate_and_recycle(uint64_t now, uint64_t deadline) {
    bool err = false;
    Root list_h, candidate_h, expiry_h, row_h, bag_h;
    int32_t cand_id = 0, cand_cap = 0;
    bool have_candidate = false;
    bool partial = false;
    int32_t scan_start = 0;

    // The service lives behind a durable root, so re-target it (never cache the raw pointer) before
    // every instance call. The bag is read off the service and gets its OWN temporary root so it
    // survives the list/count/get/quantity/expiring/recycle GCs; re-target it at each call too.
    void *service = gc_target(g_st.service_handle);
    if (service == nullptr) { note_fault(); return; }
    void *bag0 = read_ref(service, g_cfg.f_bag);
    if (!exact(bag0, g_cfg.class_bag)) return;             // bag not ready (soft return, JS parity)
    bag_h.set(bag0, &err); if (err) goto fail;
    if (g_st.scan_handle && gc_target(g_st.scan_bag_handle) != bag_h.target())
        release_scan();

    int32_t count;
    if (g_st.scan_handle) {
        list_h.set(gc_target(g_st.scan_handle), &err); if (err) goto fail;
        if (!invoke_int(g_cfg.m_count, list_h.target(), nullptr, &count, &err)) goto fail;
        if (count != g_st.scan_count) { // mutable view changed; never continue with shifted indexes
            release_scan(); g_st.next_ms = 0; return;
        }
        scan_start = g_st.scan_cursor;
    } else {
        void *f0 = g_st.filter_handles[0] ? gc_target(g_st.filter_handles[0]) : nullptr;
        void *f1 = g_st.filter_handles[1] ? gc_target(g_st.filter_handles[1]) : nullptr;
        void *f2 = g_st.filter_handles[2] ? gc_target(g_st.filter_handles[2]) : nullptr;
        int32_t zero = 0;
        void *lp[5] = {f0, &zero, f1, f2, nullptr};
        g_st.invoking = true;
        void *list = invoke(g_cfg.m_list, gc_target(g_st.service_handle), lp, &err);
        g_st.invoking = false;
        if (err) goto fail;
        list_h.set(list, &err); if (err) goto fail;
        if (!invoke_int(g_cfg.m_count, list_h.target(), nullptr, &count, &err)) goto fail;
        if (count < 0 || static_cast<uint32_t>(count) > kMaxList) { err = true; goto fail; }
        for (uint32_t i = 0; i < g_rows_n; ++i) g_rows[i].count = 0;
        if (count > 64) {
            g_st.scan_handle = gc_new(list_h.target());
            g_st.scan_bag_handle = gc_new(bag_h.target());
            if (!g_st.scan_handle || !g_st.scan_bag_handle) { err = true; goto fail; }
            g_st.scan_count = count; g_st.scan_cursor = 0;
            g_st.scan_generation = g_snapshot.generation; g_st.scan_epoch = g_snapshot.epoch;
            refresh_root_count();
        }
    }

    for (int32_t i = scan_start; i < count; ++i) {
        if (g_st.scan_handle) {
            const uint64_t t = boottime_ms();
            if (i - scan_start >= 64 || g_revoke || !t || t >= deadline ||
                (i > scan_start && t - now >= 4)) break;
            g_st.scan_cursor = i + 1;
        }
        int32_t idx = i;
        void *gp[1] = { &idx };
        void *data = invoke(g_cfg.m_get, list_h.target(), gp, &err);
        if (err) goto fail;
        row_h.set(data, &err); if (err) goto fail;
        void *d = row_h.target();
        if (!exact(d, g_cfg.class_data)) { err = true; goto fail; }
        int32_t id = read_s32(d, g_cfg.f_id);
        int32_t qty = read_s32(d, g_cfg.f_count);
        uint8_t rec = read_u8(d, g_cfg.f_recyclable);
        if (rec != 1 || id <= 0 || qty < 0 || qty > kMaxQty) continue;
        row_update(id, qty);
        // A latched fail-closed quarantine overflow suppresses ALL recycle selection; the catalog
        // still enumerates. A paused id (definite rejection OR uncertain quarantine) is skipped.
        if (!have_candidate && g_snapshot.enabled && !g_st.fail_closed) {
            int32_t cap;
            if (rule_cap(id, &cap) && qty > cap && !item_paused(id)) {
                candidate_h.set(d, &err); if (err) goto fail;
                cand_id = id; cand_cap = cap; have_candidate = true;
                if (g_st.scan_handle) { g_st.scan_cursor = i; break; }
            }
        }
    }

    partial = g_st.scan_handle && g_st.scan_cursor < g_st.scan_count;
    if (!partial) { release_scan(); publish_catalog(); }
    else g_st.next_ms = 0; // continue next frame, with no repeated managed list enumeration
    refresh_root_count();

    if (!have_candidate || !g_snapshot.enabled) {
        set_has_candidate(false);
        if (!partial) g_cleanup_serial.fetch_add(1, std::memory_order_release);
        return;
    }
    set_has_candidate(true);
    if (now >= deadline) return;                           // !itemPermitted (residual lease gone)

    int32_t current;
    {
        int32_t idv = cand_id;
        void *qp[1] = { &idv };
        if (!invoke_int(g_cfg.m_quantity, bag_h.target(), qp, &current, &err)) goto fail;
    }
    if (current <= cand_cap || current > kMaxQty) {
        if (g_st.scan_handle) ++g_st.scan_cursor;
        return; // stale-list guard: trust live quantity
    }

    void *expiry;
    expiry = invoke(g_cfg.m_expiring, gc_target(g_st.service_handle), nullptr, &err);
    if (err) goto fail;
    expiry_h.set(expiry, &err); if (err) goto fail;

    // Refresh the candidate and bag targets AFTER the final allocation (get_ExpiringItemsCopy may
    // have moved them), then recheck the lease/stop with these final targets in hand.
    void *data_obj, *bag_obj;
    data_obj = candidate_h.target();
    bag_obj  = bag_h.target();
    if (data_obj == nullptr || bag_obj == nullptr) { err = true; goto fail; }

    {
        uint64_t t = boottime_ms();
        if (g_revoke || t == 0 || t >= deadline) return;   // no request issued yet -> clean cancel
    }

    int32_t amount;
    amount = current - cand_cap;

    // Record the pending request BEFORE the call so a synchronous result/error callback is
    // attributed, and so a managed throw leaves the id quarantined (never duplicated). The bag is
    // held by its OWN durable root and matched by target; a reused address cannot confirm it. A
    // monotonic gen pins this request's identity across the original/callback boundary.
    clear_pending(false);                                  // drop any stale handle (should be none)
    g_st.pending = Pending{};
    g_st.pending.active = true;
    g_st.pending.gen = ++g_pending_gen;
    g_st.pending.id = cand_id;
    g_st.pending.amount = amount;
    g_st.pending.cap = cand_cap;
    g_st.pending.bag_handle = gc_new(bag_obj);
    g_st.pending.deadline_ms = now + kConfirmMs;
    g_st.pending.epoch = g_snapshot.epoch;
    if (g_st.pending.bag_handle == nullptr) { err = true; goto fail; }
    refresh_root_count();
    // Allocation/target lookup can re-enter. Check again AFTER the final managed
    // roots/targets are resolved; the earlier selection check is not sufficient.
    void *expiry_obj;
    expiry_obj = expiry_h.target();
    data_obj = candidate_h.target();
    bag_obj = bag_h.target();
    {
        const uint64_t t = boottime_ms();
        if (!data_obj || !bag_obj || !expiry_obj || g_revoke ||
            g_stopping.load(std::memory_order_acquire) || g_st.fail_closed ||
            t == 0 || t >= deadline) {
            clear_pending(false); refresh_root_count(); return; // not issued
        }
    }
    g_st.invoking = true;
    emit_request(cand_id, amount, cand_cap);
    if (g_revoke || g_st.fail_closed) { g_st.invoking = false; clear_pending(false); refresh_root_count(); return; }
    {
        if (g_st.scan_handle) ++g_st.scan_cursor; // commit cursor only when actually issuing
        int32_t amt = amount;
        void *rp[3] = { data_obj, &amt, expiry_obj };
        invoke(g_cfg.m_recycle, bag_obj, rp, &err); // recycle/result/error hooks fire here
    }
    g_st.invoking = false;
    if (err) goto fail;

    // A synchronous result may already have consumed pending; FIFO retains request first.
    refresh_root_count();
    return;

fail:
    release_scan();
    g_st.invoking = false;
    {
        int32_t pid = 0, pamt = 0;
        if (g_st.pending.active) { pid = g_st.pending.id; pamt = g_st.pending.amount; }
        clear_pending(true);                               // uncertain -> quarantine + free bag root
        emit_error(PGR_ITEMS_REASON_NATIVE, pid, pamt);
    }
    note_fault();
    g_st.next_ms = now + kBackoffMs;
    refresh_root_count();
}

// Overlay counts do not require the inventory list or its filter delegates.
void refresh_berries(uint64_t now, uint64_t deadline) {
    bool err = false;
    Root bag;
    void *service = gc_target(g_st.service_handle);
    if (!exact(service, g_cfg.class_service)) return;
    void *b = read_ref(service, g_cfg.f_bag);
    if (!exact(b, g_cfg.class_bag)) return;
    bag.set(b, &err);
    int32_t counts[5] = {};
    const int32_t ids[5] = {701,703,705,706,707};
    for (int i = 0; i < 5 && !err; ++i) {
        uint64_t t = boottime_ms();
        if (g_revoke || !t || t >= deadline) return;
        b = bag.target();
        if (!exact(b, g_cfg.class_bag)) return;
        int32_t id = ids[i]; void *args[1] = {&id};
        if (!invoke_int(g_cfg.m_quantity, b, args, &counts[i], &err)) break;
        if (counts[i] < 0 || counts[i] > kMaxQty) err = true;
    }
    if (err) { note_fault(); emit_error(PGR_ITEMS_REASON_NATIVE, 0, 0); g_st.next_ms = now + kBackoffMs; return; }
    uint64_t t = boottime_ms();
    if (g_revoke || !t || t >= deadline) return;
    for (int i = 0; i < 5; ++i) row_update(ids[i], counts[i]);
    publish_catalog();
}

// Automatic no-work detection reads only user-selected quantities. A descriptor is
// still obtained through the existing list path when an actual excess is found.
// 0 complete/no excess, 1 excess, 2 incomplete/unavailable.
int probe_selected(uint64_t now, uint64_t deadline) {
    Root bag; bool err = false;
    void *service = gc_target(g_st.service_handle);
    if (!exact(service, g_cfg.class_service)) return 2;
    void *b = read_ref(service, g_cfg.f_bag);
    if (!exact(b, g_cfg.class_bag)) return 2;
    bag.set(b, &err); if (err) goto fail;
    if (g_st.probe_bag && (gc_target(g_st.probe_bag) != bag.target() ||
        g_st.probe_generation != g_snapshot.generation || g_st.probe_epoch != g_snapshot.epoch))
        release_probe();
    if (!g_st.probe_bag) {
        g_st.probe_bag = gc_new(bag.target()); if (!g_st.probe_bag) goto fail;
        g_st.probe_generation = g_snapshot.generation; g_st.probe_epoch = g_snapshot.epoch;
        refresh_root_count();
    }
    {
        uint32_t processed = 0;
        while (g_st.probe_cursor < g_snapshot.count) {
            uint64_t t = boottime_ms();
            if (g_revoke || !t || t >= deadline || processed >= 64 ||
                (processed && t - now >= 4)) { g_st.next_ms = 0; return 2; }
            const auto entry = g_snapshot.entries[g_st.probe_cursor];
            ++processed;
            if (entry.id <= 0 || item_paused(entry.id)) { ++g_st.probe_cursor; continue; }
            int32_t quantity = 0, id = entry.id; void *args[] = {&id};
            if (!invoke_int(g_cfg.m_quantity, bag.target(), args, &quantity, &err) ||
                quantity < 0 || quantity > kMaxQty) goto fail;
            // A revoked/expired observation cannot finish or advance a cleanup pass.
            t = boottime_ms();
            if (g_revoke || !t || t >= deadline) { g_st.next_ms = 0; return 2; }
            if (read_ref(gc_target(g_st.service_handle), g_cfg.f_bag) != bag.target()) {
                release_probe(); g_st.next_ms = 0; return 2;
            }
            ++g_st.probe_cursor;
            if (quantity > entry.cap) { release_probe(); return 1; }
        }
    }
    release_probe();
    { const uint64_t t = boottime_ms();
      if (g_revoke || !t || t >= deadline) { g_st.next_ms = 0; return 2; } }
    set_has_candidate(false);
    g_cleanup_serial.fetch_add(1, std::memory_order_release);
    return 0;
fail:
    release_probe(); note_fault(); emit_error(PGR_ITEMS_REASON_NATIVE, 0, 0);
    g_st.next_ms = now + kBackoffMs; return 2;
}

// --- orchestration pass (Unity thread) --------------------------------------------
void orchestrate(uint64_t now, uint64_t deadline, uint32_t force_poll) {
    if (now == 0) return;                                  // no clock
    if (force_poll & 1u) g_st.next_ms = 0;
    if (force_poll & 8u) release_scan(); // also resets selected probe for the new cleanup ticket
    if (!g_snapshot.enabled) release_probe();
    if (g_st.scan_handle && ((!g_snapshot.enabled && !(force_poll & 2u)) ||
        g_st.scan_generation != g_snapshot.generation || g_st.scan_epoch != g_snapshot.epoch))
        release_scan();

    // A config generation change clears ONLY the definite-rejection blocked set (JS:
    // itemConfig+':'+itemEnabled change). The uncertain quarantine (quar[]) is deliberately left
    // intact: an unknown recycle outcome must stay paused regardless of any cap/toggle change.
    if (!g_st.have_generation || g_st.rules_generation != g_snapshot.generation) {
        g_st.have_generation = true;
        g_st.rules_generation = g_snapshot.generation;
        blocked_clear();
    }

    if (g_st.pending.active) {
        if (now > g_st.pending.deadline_ms) {
            // Timeout is an UNKNOWN outcome: permanently quarantine (not settings-scoped) and drop
            // attribution (free the bag root) so no late callback and no restart can resubmit it.
            int32_t id = g_st.pending.id, amount = g_st.pending.amount;
            clear_pending(true);
            emit_error(PGR_ITEMS_REASON_TIMEOUT, id, amount);
            refresh_root_count();                          // JS falls through to enumerate
        } else {
            return;                                        // still awaiting confirmation
        }
    }

    if (now >= deadline || now < g_st.next_ms) return;
    g_st.next_ms = now + kPollMs;
    const bool explicit_catalog = (force_poll & 2u) != 0;
    if (explicit_catalog) release_probe();
    // The berry overlay explicitly requests these five IDs independently of cleanup.
    if (force_poll & 4u) {
        if (!g_st.service_handle && g_cfg.has_lookup) do_lookup(now);
        if (g_st.service_handle) refresh_berries(now, deadline);
    }
    if (!explicit_catalog && g_snapshot.enabled && !g_snapshot.count) {
        release_scan();
        const uint64_t t = boottime_ms();
        if (g_revoke || !t || t >= deadline) { g_st.next_ms = 0; return; }
        set_has_candidate(false);
        g_cleanup_serial.fetch_add(1, std::memory_order_release); return;
    }
    const bool full = g_snapshot.enabled || explicit_catalog;
    if (!g_st.service_handle || (full && !g_st.have_filters)) {
        if (g_cfg.has_lookup) do_lookup(now);
        if (!g_st.service_handle || (full && !g_st.have_filters)) return;
    }
    if (!full) {
        set_has_candidate(false);
        return;
    }
    if (!explicit_catalog && !g_st.scan_handle && probe_selected(now, deadline) != 1) return;
    enumerate_and_recycle(now, deadline);
}

// --- service capture (Unity thread) -----------------------------------------------
void capture_service(void *o) {
    if (g_stopping.load(std::memory_order_acquire)) return;
    uint64_t unity = g_unity_tid.load(std::memory_order_acquire);
    if (unity == 0 || current_tid() != unity) return;      // only the proven Unity thread
    if (!exact(o, g_cfg.class_service)) return;
    if (g_st.service_handle && gc_target(g_st.service_handle) == o) return; // already captured
    // A different service replaces the old one: detach-then-free its root and drop the filter
    // delegates built from the old container so they rebuild against the new service.
    if (g_st.service_handle) {
        void *old = g_st.service_handle; g_st.service_handle = nullptr; gc_free(old);
        release_filters();
        release_scan();
    }
    g_st.service_handle = gc_new(o);
    g_st.next_ms = 0;                                      // force an immediate re-poll
    emit_bag_ready();
    refresh_root_count();
}

void capture_filters(void *p0, void *p2, void *p3) {
    if (g_st.invoking || g_stopping.load(std::memory_order_acquire)) return;
    uint64_t unity = g_unity_tid.load(std::memory_order_acquire);
    if (unity == 0 || current_tid() != unity) return;
    release_scan();
    release_filters();
    g_st.filter_handles[0] = p0 ? gc_new(p0) : nullptr;
    g_st.filter_handles[1] = p2 ? gc_new(p2) : nullptr;
    g_st.filter_handles[2] = p3 ? gc_new(p3) : nullptr;
    g_st.have_filters = true;
    refresh_root_count();
}

// --- result matching (Unity thread) -----------------------------------------------
// Mirrors JS matched(): pending active, exact callback class, its captured bag field equals the
// pending bag (compared against the rooted bag TARGET so a reused address can't collide), a
// non-null prediction whose ItemProto id equals the pending id.
bool matched(void *callback) {
    if (!g_st.pending.active || !g_st.pending.bag_handle) return false;
    const uint64_t gen = g_st.pending.gen;
    const int32_t id = g_st.pending.id;
    bool err = false;
    Root cb, prediction, proto;
    cb.set(callback, &err);
    if (err || !exact(cb.target(), g_cfg.class_callback)) return false;
    if (read_ref(cb.target(), g_cfg.f_callback_bag) != gc_target(g_st.pending.bag_handle)) return false;
    prediction.set(read_ref(cb.target(), g_cfg.f_prediction), &err);
    if (err) return false;
    proto.set(invoke(g_cfg.m_proto, prediction.target(), nullptr, &err), &err);
    if (err || !proto.target()) return false;
    const bool same_item = read_s32(proto.target(), g_cfg.f_proto_id) == id;
    return same_item && g_st.pending.active && g_st.pending.gen == gen &&
           !g_stopping.load(std::memory_order_acquire) && !g_revoke;
}

// --- hooks ------------------------------------------------------------------------
void hook_init(void *self, void *method) {
    InitFn orig = load_orig(g_orig_init);
    uint64_t unity = g_unity_tid.load(std::memory_order_acquire);
    OwnerScope frame(unity != 0 && current_tid() == unity);
    orig(self, method);                                    // onLeave parity: capture after original
    if (unity != 0 && current_tid() == unity) capture_service(self);
}

void *hook_list(void *self, void *p0, int32_t p1, void *p2, void *p3, void *p4, void *method) {
    ListFn orig = load_orig(g_orig_list);
    uint64_t unity = g_unity_tid.load(std::memory_order_acquire);
    OwnerScope frame(unity != 0 && current_tid() == unity);
    if (unity != 0 && current_tid() == unity) {            // onEnter: capture service + filters
        capture_service(self);
        capture_filters(p0, p2, p3);
    }
    return orig(self, p0, p1, p2, p3, p4, method);
}

void *hook_recycle(void *self, void *data, int32_t count, void *expiry, void *method) {
    RecycleFn orig = load_orig(g_orig_recycle);
    uint64_t unity = g_unity_tid.load(std::memory_order_acquire);
    OwnerScope frame(unity != 0 && current_tid() == unity);
    if (unity != 0 && current_tid() == unity && !g_st.invoking &&
        !g_stopping.load(std::memory_order_acquire)) {
        // Manual recycle by the player while a request is in flight: the outcome of OUR request is
        // now UNKNOWN. Permanently quarantine the id and drop attribution (free the bag root) so no
        // later callback and no restart can attribute or resubmit it, then back off.
        if (g_st.pending.active) { clear_pending(true); refresh_root_count(); }
        g_st.next_ms = boottime_ms() + kBackoffMs;
    }
    return orig(self, data, count, expiry, method);
}

void hook_result(void *self, void *response, void *method) {
    ResultFn orig = load_orig(g_orig_result);
    uint64_t unity = g_unity_tid.load(std::memory_order_acquire);
    OwnerScope frame(unity != 0 && current_tid() == unity);

    bool consume = false, ok = false;
    uint64_t gen = 0;
    if (unity != 0 && current_tid() == unity && !g_stopping.load(std::memory_order_acquire)) {
        if (matched(self)) {
            consume = true;
            gen = g_st.pending.gen;                        // pin the identity across the original
            ok = exact(response, g_cfg.class_response) &&
                 read_s32(response, g_cfg.f_status) == g_cfg.success;
        }
    }

    orig(self, response, method);

    // Only confirm the SAME request: a stop (clear) or a fresh request during the original
    // (stopping set / gen changed / pending cleared) must never let this callback confirm a
    // different RPC -- and a deferred clear keeps pending.active set, so re-check g_stopping too.
    if (consume && !g_stopping.load(std::memory_order_acquire) &&
        g_st.pending.active && g_st.pending.gen == gen) {
        int32_t id = g_st.pending.id, amount = g_st.pending.amount;
        g_st.next_ms = g_st.scan_handle ? 0 : boottime_ms() + kSettleMs;
        if (!ok) { clear_pending(false); blocked_add(id); emit_error(PGR_ITEMS_REASON_REJECTED, id, amount); }
        else { clear_pending(false); emit_confirm(id, amount); }
        refresh_root_count();
    }
}

void hook_error(void *self, void *str, void *method) {
    ErrorFn orig = load_orig(g_orig_error);
    uint64_t unity = g_unity_tid.load(std::memory_order_acquire);
    OwnerScope frame(unity != 0 && current_tid() == unity);

    bool consume = false;
    uint64_t gen = 0;
    if (unity != 0 && current_tid() == unity && !g_stopping.load(std::memory_order_acquire)) {
        if (matched(self)) { consume = true; gen = g_st.pending.gen; }
    }

    orig(self, str, method);

    if (consume && !g_stopping.load(std::memory_order_acquire) &&
        g_st.pending.active && g_st.pending.gen == gen) {
        int32_t id = g_st.pending.id, amount = g_st.pending.amount;
        g_st.next_ms = g_st.scan_handle ? 0 : boottime_ms() + kSettleMs;
        clear_pending(false);                              // definite game error -> settings-scoped
        blocked_add(id);
        emit_error(PGR_ITEMS_REASON_REJECTED, id, amount);
        refresh_root_count();
    }
}

// --- C API table functions --------------------------------------------------------
bool core_ok(const pgrazor_items_config_v1 *c) {
    const void *const ptrs[] = {
        c->runtime_invoke, c->gchandle_new, c->gchandle_get_target, c->gchandle_free,
        c->object_get_class, c->object_unbox, c->field_get_value, c->m_list, c->m_count,
        c->m_get, c->m_quantity, c->m_expiring, c->m_recycle, c->m_proto, c->init_address,
        c->list_address, c->recycle_address, c->result_address, c->error_address,
        c->class_service, c->class_bag, c->class_data, c->class_callback, c->class_response,
        c->class_int, c->f_bag, c->f_id, c->f_count, c->f_recyclable, c->f_callback_bag,
        c->f_prediction, c->f_proto_id, c->f_status};
    for (const void *p : ptrs) if (p == nullptr) return false;
    return true;
}
bool lookup_ok(const pgrazor_items_config_v1 *c) {
    const void *const ptrs[] = {
        c->class_get_type, c->type_get_object, c->field_static_get_value, c->runtime_class_init,
        c->method_get_object, c->m_find, c->m_container, c->m_resolve, c->m_create_delegate,
        c->m_usable, c->m_show, c->class_map, c->class_service_contract, c->class_gui_contract,
        c->class_gui, c->class_singleton, c->f_singleton_instance, c->func_type};
    for (const void *p : ptrs) if (p == nullptr) return false;
    return true;
}

bool hooks_distinct(const pgrazor_items_config_v1 *c) {
    const void *const a[] = { c->init_address, c->list_address, c->recycle_address,
                              c->result_address, c->error_address };
    for (int i = 0; i < 5; ++i) for (int j = i + 1; j < 5; ++j) if (a[i] == a[j]) return false;
    return true;
}

int api_install(const pgrazor_items_config_v1 *config) {
    if (config == nullptr) return PGR_ITEMS_ERR_NULL_ARG;
    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (config->version != PGRAZOR_ITEMS_API_VERSION ||
        config->size != sizeof(pgrazor_items_config_v1) || !core_ok(config))
        return PGR_ITEMS_ERR_BAD_CONFIG;
    if (config->has_lookup && !lookup_ok(config)) return PGR_ITEMS_ERR_BAD_CONFIG;
    if (!hooks_distinct(config)) return PGR_ITEMS_ERR_DUP_HOOK;

    if (g_installed.load(std::memory_order_acquire))
        return (std::memcmp(&g_cfg, config, sizeof(g_cfg)) == 0)
                   ? PGR_ITEMS_OK : PGR_ITEMS_ERR_ALREADY_BOUND;
    if (g_install_attempted) return g_install_error;       // a prior failure latches
    g_install_attempted = true;

    if (boottime_ms() == 0) return g_install_error = PGR_ITEMS_ERR_NO_CLOCK;
    g_cfg = *config;

    // Install the five hooks, publishing each original before the next. On a partial failure
    // retain any published trampoline (another thread may already use it) and latch.
    void *o = nullptr;
    if (pgrazor_hook(g_cfg.init_address, reinterpret_cast<void *>(&hook_init), &o) != 0 || !o)
        return g_install_error = PGR_ITEMS_ERR_HOOK;
    g_orig_init.store(reinterpret_cast<InitFn>(o), std::memory_order_release);
    o = nullptr;
    if (pgrazor_hook(g_cfg.list_address, reinterpret_cast<void *>(&hook_list), &o) != 0 || !o)
        return g_install_error = PGR_ITEMS_ERR_HOOK;
    g_orig_list.store(reinterpret_cast<ListFn>(o), std::memory_order_release);
    o = nullptr;
    if (pgrazor_hook(g_cfg.recycle_address, reinterpret_cast<void *>(&hook_recycle), &o) != 0 || !o)
        return g_install_error = PGR_ITEMS_ERR_HOOK;
    g_orig_recycle.store(reinterpret_cast<RecycleFn>(o), std::memory_order_release);
    o = nullptr;
    if (pgrazor_hook(g_cfg.result_address, reinterpret_cast<void *>(&hook_result), &o) != 0 || !o)
        return g_install_error = PGR_ITEMS_ERR_HOOK;
    g_orig_result.store(reinterpret_cast<ResultFn>(o), std::memory_order_release);
    o = nullptr;
    if (pgrazor_hook(g_cfg.error_address, reinterpret_cast<void *>(&hook_error), &o) != 0 || !o)
        return g_install_error = PGR_ITEMS_ERR_HOOK;
    g_orig_error.store(reinterpret_cast<ErrorFn>(o), std::memory_order_release);

    g_installed.store(true, std::memory_order_release);
    return g_install_error = PGR_ITEMS_OK;
}

int api_tick(const pgrazor_items_rules_v1 *rules, uint32_t force_poll,
             uint64_t remaining_lease_ms, uint64_t proven_tid) {
    if (!g_installed.load(std::memory_order_acquire)) return PGR_ITEMS_ERR_NOT_INSTALLED;

    // Validate + snapshot the rules before any thread gate (a bad snapshot is a hard error).
    pgrazor_items_rules_v1 snap;
    if (rules != nullptr) {
        if (rules->version != PGRAZOR_ITEMS_API_VERSION ||
            rules->size != sizeof(pgrazor_items_rules_v1) || rules->count > kMaxRules)
            return PGR_ITEMS_ERR_BAD_RULES;
        snap = *rules;
    } else {
        std::memset(&snap, 0, sizeof(snap));
        snap.version = PGRAZOR_ITEMS_API_VERSION;
        snap.size = sizeof(snap);
    }

    if (!on_unity_thread(proven_tid)) return PGR_ITEMS_OK;
    uint64_t expected_unity = 0;
    g_unity_tid.compare_exchange_strong(expected_unity, proven_tid, std::memory_order_acq_rel);
    if (g_unity_tid.load(std::memory_order_acquire) != proven_tid) return PGR_ITEMS_OK;

    if (g_depth != 0) {
        // Nested tick (re-entry inside our own managed call): a NO-OP that must NOT reset a stop
        // set by a reentrant clear, and must NOT clear a revoke. It revokes the in-flight selection
        // when auto-recycle turned off, the lease is spent, OR the rules changed -- a new cap /
        // epoch / generation must never be satisfied by the original pass's cap selection.
        if (snap.enabled == 0 || remaining_lease_ms == 0 ||
            snap.generation != g_snapshot.generation || snap.epoch != g_snapshot.epoch)
            g_revoke = true;
        return PGR_ITEMS_OK;
    }
    g_stopping.store(false, std::memory_order_release);    // top-level only: resume after a prior stop
    OwnerScope frame; g_revoke = false;

    uint64_t expected = 0;
    if (!g_owner_tid.compare_exchange_strong(expected, proven_tid, std::memory_order_acq_rel))
        return PGR_ITEMS_OK;

    g_snapshot = snap;                                     // owner-thread snapshot of the rules
    uint64_t lease = remaining_lease_ms > kMaxLeaseMs ? kMaxLeaseMs : remaining_lease_ms;
    uint64_t now = boottime_ms();
    orchestrate(now, now + lease, force_poll);

    g_owner_tid.store(0, std::memory_order_release);
    return PGR_ITEMS_OK;
}

int api_clear(uint64_t proven_tid) {
    if (!g_installed.load(std::memory_order_acquire)) return PGR_ITEMS_ERR_NOT_INSTALLED;
    if (!on_unity_thread(proven_tid) ||
        g_unity_tid.load(std::memory_order_acquire) != proven_tid)
        return PGR_ITEMS_OK;                               // never free off the Unity thread

    g_stopping.store(true, std::memory_order_release);
    g_revoke = true;
    if (g_depth != 0) { g_deferred_clear = true; return PGR_ITEMS_OK; } // defer under a live pass
    OwnerScope frame;
    release_roots();            // frees service + filters + any in-flight bag root (pins its id)
    g_rows_n = 0;               // display state; pending attribution dropped + id quarantined
    return PGR_ITEMS_OK;
}

int api_sample(pgrazor_items_sample_v1 *out) {
    if (out == nullptr) return PGR_ITEMS_ERR_NULL_ARG;
    out->version = PGRAZOR_ITEMS_API_VERSION;
    out->size = sizeof(pgrazor_items_sample_v1);
    out->bag_ready_serial = g_bag_ready_serial.load(std::memory_order_acquire);
    out->catalog_serial = g_catalog_serial.load(std::memory_order_acquire);
    out->fault_serial = g_fault_serial.load(std::memory_order_acquire);
    out->events_lost = g_events_lost.load(std::memory_order_acquire);
    out->has_candidate = g_has_candidate.load(std::memory_order_acquire);
    out->owned_roots = g_root_count.load(std::memory_order_acquire) +
                       g_active_frames.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(g_catalog_mutex);
        out->catalog_count = g_pub_n;
    }
    out->reserved = 0;
    return PGR_ITEMS_OK;
}

int api_catalog(pgrazor_item_row *out, uint32_t max, uint32_t *written) {
    if (!g_installed.load(std::memory_order_acquire)) return PGR_ITEMS_ERR_NOT_INSTALLED;
    if (out == nullptr || written == nullptr) return PGR_ITEMS_ERR_NULL_ARG;
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    uint32_t n = g_pub_n < max ? g_pub_n : max;
    std::memcpy(out, g_pub_rows, static_cast<size_t>(n) * sizeof(pgrazor_item_row));
    *written = n;
    return PGR_ITEMS_OK;
}

int api_drain(pgrazor_item_event *out, uint32_t max, uint32_t *written) {
    if (!g_installed.load(std::memory_order_acquire)) return PGR_ITEMS_ERR_NOT_INSTALLED;
    if (out == nullptr || written == nullptr) return PGR_ITEMS_ERR_NULL_ARG;
    std::lock_guard<std::mutex> lock(g_event_mutex);
    uint32_t n = 0;
    while (n < max && g_ev_size > 0) {
        out[n++] = g_events[g_ev_head];
        g_ev_head = (g_ev_head + 1) % kMaxEvents;
        --g_ev_size;
    }
    *written = n;
    return PGR_ITEMS_OK;
}

void *api_service_handle(void) {
    return g_service_atomic.load(std::memory_order_acquire);   // borrowed; readiness/identity only
}

// Scoped acquire: hand external features their OWN root on the service, independent of our durable
// handle's lifetime. Creates a GC handle, so it is valid only on the proven Unity thread; the
// caller frees the returned handle with il2cpp_gchandle_free.
void *api_service_acquire(void) {
    uint64_t unity = g_unity_tid.load(std::memory_order_acquire);
    if (unity == 0 || current_tid() != unity) return nullptr;  // only the proven Unity thread
    if (g_stopping.load(std::memory_order_acquire)) return nullptr;
    OwnerScope frame;
    if (g_st.service_handle == nullptr) return nullptr;
    void *obj = gc_target(g_st.service_handle);
    if (obj == nullptr) return nullptr;
    void *owned = gc_new(obj);
    if (g_stopping.load(std::memory_order_acquire)) { gc_free(owned); return nullptr; }
    return owned; // caller owns + frees independently
}

const pgrazor_items_api_v1 g_api = {
    PGRAZOR_ITEMS_API_VERSION,
    static_cast<uint32_t>(sizeof(pgrazor_items_api_v1)),
    &api_install,
    &api_tick,
    &api_clear,
    &api_sample,
    &api_catalog,
    &api_service_handle,
    &api_drain,
    &api_service_acquire,
};

} // namespace

const void *pgrazor_items_api(uint32_t version) {
    if (version != PGRAZOR_ITEMS_API_VERSION) return nullptr;
    return &g_api;
}

extern "C" uint64_t pgrazor_items_cleanup_serial() {
    return g_cleanup_serial.load(std::memory_order_acquire);
}

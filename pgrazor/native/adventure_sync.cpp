// PGRazor native migration: Adventure Sync popup suppression (feature id 2).
//
// Mirrors runtime/features/adventureSync.js. On the Adventure Sync DialogController it
// declines the initial and instruction dialogs and acknowledges the result dialog by
// pressing the button the game itself wired, on a proven later Unity-thread tick, only
// while an externally granted dismissal lease is live. It NEVER confirms an
// initial/instruction dialog (that would enable Adventure Sync), never fabricates a
// callback, and never drives UI from the host heartbeat.
//
// Division of labour with the JS adapter (runtime/features/adventureSync.js):
//   * JS resolves and validates all il2cpp metadata (class identity, method
//     address + instruction-prefix checks, field offsets) BEFORE calling install().
//   * This module owns the pending-dialog queue, GC rooting, callback-identity checks,
//     the Update-thread delayed decline/confirm, original chaining, the CLOCK_BOOTTIME
//     lease and the Unity-thread guard.
//
// ABI note on the Initialize hooks. InitializeAsInitial/Instruction/Result take 4/7/5
// parameters whose types are NOT recoverable from the current sources, so a
// full-signature replacement that chained the original would be a guess. They are
// therefore attached with an entry instrument, which is signature-agnostic: it reads
// only the `this` controller (x0 on arm64, rdi on x86_64) to tag the dialog kind, reads
// no undetermined argument register, and does not alter the call. The callback delegate
// is snapshotted later, on the first Update, once the dialog has wired it. The
// 0-parameter Update/OnConfirm/OnDecline methods have a determinable il2cpp ABI,
// void(void* this, void* methodInfo), and are hooked with a full chained replacement.
//
// Mutations go through the central engine wrappers (hook_engine.h): pgrazor_hook /
// pgrazor_unhook for the chained replacements and pgrazor_instrument for the entry
// instruments, so every target in the process shares one serialized trampoline
// registry. The register-context field read by the instrument (general.regs.x0 /
// general.regs.rdi) is present in the pinned Dobby register-context header for both
// ABIs; a build on any other ABI fails loudly below rather than reading the wrong
// register.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <cstdlib>
#include <unistd.h>
#include <android/log.h>

#include "dobby.h"

#include "adventure_sync.h"
#include "hook_engine.h"

namespace {

// --- entry-instrument register access --------------------------------------------
// The first integer argument (the `this` controller) by platform ABI. Centralised so
// the one Dobby-version-dependent line per ABI is easy to confirm/adjust; both fields
// exist in the pinned Dobby register-context header.
#if defined(__aarch64__)
#define PGRAZOR_ADVSYNC_CTX_SELF(ctx) \
    (reinterpret_cast<void *>((ctx)->general.regs.x0))
#elif defined(__x86_64__)
#define PGRAZOR_ADVSYNC_CTX_SELF(ctx) \
    (reinterpret_cast<void *>((ctx)->general.regs.rdi))
#else
#error "Adventure Sync native instrument supports arm64 and x86_64 register contexts only."
#endif

constexpr uint64_t kMaxLeaseMs = PGRAZOR_ADVSYNC_MAX_LEASE_MS;
constexpr uint64_t kDelayNs    = static_cast<uint64_t>(PGRAZOR_ADVSYNC_DELAY_MS) * 1000000ull;
constexpr uint64_t kExpiryNs   = static_cast<uint64_t>(PGRAZOR_ADVSYNC_EXPIRY_MS) * 1000000ull;
constexpr unsigned kQueueMax   = PGRAZOR_ADVSYNC_QUEUE_MAX;

// ---------------------------------------------------------------------------
// Pure policy (shared by the live path and the host-independent self-check).
// ---------------------------------------------------------------------------

enum DialogKind { DLG_INITIAL = 0, DLG_INSTRUCTION = 1, DLG_RESULT = 2 };

// The result dialog's sole button acknowledges the outcome (confirm). The initial and
// instruction dialogs are declined. Confirming an initial/instruction dialog would
// enable Adventure Sync and is never allowed.
inline bool dialog_is_confirm(DialogKind kind) { return kind == DLG_RESULT; }

// A lease must be positive and within the cap.
inline bool lease_ok(uint64_t lease_ms) { return lease_ms != 0 && lease_ms <= kMaxLeaseMs; }

// ---------------------------------------------------------------------------
// Monotonic clock including suspend. 0 == "no valid time" (never satisfies now<deadline).
// ---------------------------------------------------------------------------
uint64_t boottime_ns() {
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return 0;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

inline void logw(const char *msg) {
    __android_log_write(ANDROID_LOG_WARN, "PGRazorAdvSync", msg);
}

// ---------------------------------------------------------------------------
// Validated config + lifetime/policy state.
// ---------------------------------------------------------------------------

std::mutex g_install_mutex;          // serialises install() against itself
pgrazor_advsync_config_v1 g_config{}; // validated copy, published under install mutex
std::atomic<bool> g_installed{false}; // true only after a fully successful install
bool g_install_failed = false;        // sticky; protected by install mutex

std::atomic<bool>     g_allowed{false};
std::atomic<uint64_t> g_deadline_ns{0};
std::atomic<uint64_t> g_unity_tid{0}; // Unity-thread guard, set from the validated tick

// Chained originals for the fully-typed (0-arg) methods.
typedef void (*member_fn)(void *self, void *method_info);
std::atomic<member_fn> g_orig_update{nullptr};
std::atomic<member_fn> g_orig_on_confirm{nullptr};
std::atomic<member_fn> g_orig_on_decline{nullptr};

// Bounded pending queue. All mutation happens on the Unity thread, but the mutex also
// guards against the reentrant OnConfirm/OnDecline hook that our own invoke triggers.
struct Pending {
    bool       in_use = false;
    void      *handle = nullptr;   // pointer-sized GC root (il2cpp_gchandle_new)
    bool       is_confirm = false; // which wired button we may press
    uint64_t   callback = 0;       // delegate snapshot; 0 until first Update sees it
    uint64_t   captured_ns = 0;
};
std::mutex g_queue_mutex;
Pending g_slots[kQueueMax];

// ---------------------------------------------------------------------------
// il2cpp helpers over the validated config pointers.
// ---------------------------------------------------------------------------
inline void *gc_new(void *obj) {
    return reinterpret_cast<void *(*)(void *, int)>(g_config.gchandle_new)(obj, 0);
}
inline void *gc_target(void *handle) {
    return reinterpret_cast<void *(*)(void *)>(g_config.gchandle_get_target)(handle);
}
inline void gc_free(void *handle) {
    reinterpret_cast<void (*)(void *)>(g_config.gchandle_free)(handle);
}
inline void *obj_class(void *obj) {
    return reinterpret_cast<void *(*)(void *)>(g_config.object_get_class)(obj);
}
// Raw read of a validated reference-field offset (delegate pointer), pointer-sized.
inline uint64_t read_field_ptr(void *obj, uint64_t offset) {
    return static_cast<uint64_t>(
        *reinterpret_cast<uintptr_t *>(reinterpret_cast<uintptr_t>(obj) + offset));
}
// Invoke a 0-arg managed instance method via runtime_invoke, catching the managed
// exception through the exception out-parameter (handled, never propagated). The
// returned object and any exception object are not retained, so nothing to release.
inline void invoke_managed(void *method, void *obj) {
    void *exc = nullptr;
    reinterpret_cast<void *(*)(void *, void *, void **, void **)>(g_config.runtime_invoke)(
        method, obj, nullptr, &exc);
    if (exc != nullptr) logw("managed exception from auto-dismiss (handled)");
}

inline bool on_unity_thread() {
    uint64_t guard = g_unity_tid.load(std::memory_order_acquire);
    return guard != 0 && static_cast<uint64_t>(gettid()) == guard;
}

inline bool policy_active_now() {
    if (!g_allowed.load(std::memory_order_acquire)) return false;
    uint64_t now = boottime_ns();
    uint64_t deadline = g_deadline_ns.load(std::memory_order_acquire);
    return now != 0 && now < deadline;
}

inline uint64_t slot_offset(const Pending &p) {
    return p.is_confirm ? g_config.off_on_confirm : g_config.off_on_decline;
}

// Queue helpers. Callers hold g_queue_mutex.
void drop_locked(unsigned i) {
    if (!g_slots[i].in_use) return;
    if (g_slots[i].handle) gc_free(g_slots[i].handle);
    g_slots[i] = Pending{};
}
void remove_for_object_locked(void *self) {
    for (unsigned i = 0; i < kQueueMax; ++i)
        if (g_slots[i].in_use && gc_target(g_slots[i].handle) == self) drop_locked(i);
}

// ---------------------------------------------------------------------------
// Capture: tag a freshly shown dialog with its kind (entry instrument, x0 only).
// ---------------------------------------------------------------------------
void tag_dialog(void *self, DialogKind kind) {
    if (!g_installed.load(std::memory_order_acquire)) return;
    if (self == nullptr) return;
    if (!on_unity_thread()) return;
    if (obj_class(self) != g_config.controller_class) return; // exact identity

    std::lock_guard<std::mutex> lock(g_queue_mutex);
    remove_for_object_locked(self);                   // re-init of the same controller
    if (!policy_active_now()) return;
    for (unsigned i = 0; i < kQueueMax; ++i) {
        if (g_slots[i].in_use) continue;
        void *handle = gc_new(self);
        if (handle == nullptr) return;                // never fabricate a root
        g_slots[i] = Pending{};
        g_slots[i].in_use = true;
        g_slots[i].handle = handle;
        g_slots[i].is_confirm = dialog_is_confirm(kind);
        g_slots[i].callback = 0;
        g_slots[i].captured_ns = boottime_ns();
        return;
    }
    // Queue full: leave the dialog to the game's own flow (bounded, no growth).
}

// Drop our pending entry for a controller whose button was pressed (by the user or by
// our own invoke). Preserves the SDK's own cancellation/completion callbacks: we only
// forget our tracking, we never suppress the original handler.
void clear_for_object(void *self) {
    if (self == nullptr || !on_unity_thread()) return;
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    remove_for_object_locked(self);
}

// ---------------------------------------------------------------------------
// Unity-thread processing of the queue for one controller (called after Update).
// Removes an entry before invoking, because the wired callback can synchronously
// create the result dialog. The lock is never held across runtime_invoke, so the
// reentrant OnConfirm/OnDecline hook can take it without deadlock.
// ---------------------------------------------------------------------------
void process_updates(void *self) {
    if (!on_unity_thread()) return;

    for (;;) {
        void *due_method = nullptr;
        void *due_obj = nullptr;
        void *due_handle = nullptr;
        uint64_t now = boottime_ns();
        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            bool armed = false;
            for (unsigned i = 0; i < kQueueMax; ++i) {
                if (!g_slots[i].in_use) continue;
                void *obj = gc_target(g_slots[i].handle);
                if (obj != self) continue;

                if (!policy_active_now() || now <= g_slots[i].captured_ns ||
                    now - g_slots[i].captured_ns > kExpiryNs) {
                    drop_locked(i);
                    continue;
                }
                uint64_t current_cb = read_field_ptr(obj, slot_offset(g_slots[i]));
                if (g_slots[i].callback == 0) {
                    if (current_cb == 0) { drop_locked(i); continue; } // no wired button
                    g_slots[i].callback = current_cb;                  // snapshot once
                }
                if (now - g_slots[i].captured_ns < kDelayNs) continue;  // settle delay
                if (obj_class(obj) != g_config.controller_class ||
                    current_cb != g_slots[i].callback) {
                    drop_locked(i);                                     // identity changed
                    continue;
                }
                // Due: detach from the queue before invoking.
                due_method = g_slots[i].is_confirm ? g_config.method_on_confirm
                                                   : g_config.method_on_decline;
                due_obj = obj;
                due_handle = g_slots[i].handle;
                g_slots[i] = Pending{};
                armed = true;
                break;
            }
            if (!armed) break;
        }
        invoke_managed(due_method, due_obj);
        gc_free(due_handle);
    }
}

// ---------------------------------------------------------------------------
// Dobby replacements (0-arg il2cpp instance methods: void(self, methodInfo)).
// ---------------------------------------------------------------------------
member_fn original_for(std::atomic<member_fn> &slot) {
    member_fn orig=slot.load(std::memory_order_acquire);
    if(!orig){std::lock_guard<std::mutex> guard(g_install_mutex);orig=slot.load(std::memory_order_acquire);}
    if(!orig)std::abort();
    return orig;
}
void update_replacement(void *self, void *method_info) {
    original_for(g_orig_update)(self, method_info);
    process_updates(self);
}
void on_confirm_replacement(void *self, void *method_info) {
    member_fn orig=original_for(g_orig_on_confirm);
    clear_for_object(self);                       // JS cleared in onEnter
    orig(self, method_info);
}
void on_decline_replacement(void *self, void *method_info) {
    member_fn orig=original_for(g_orig_on_decline);
    clear_for_object(self);
    orig(self, method_info);
}

// Entry instruments for the Initialize overloads (read x0 only; do not alter the call).
void instrument_initial(void *, DobbyRegisterContext *ctx) {
    tag_dialog(PGRAZOR_ADVSYNC_CTX_SELF(ctx), DLG_INITIAL);
}
void instrument_instruction(void *, DobbyRegisterContext *ctx) {
    tag_dialog(PGRAZOR_ADVSYNC_CTX_SELF(ctx), DLG_INSTRUCTION);
}
void instrument_result(void *, DobbyRegisterContext *ctx) {
    tag_dialog(PGRAZOR_ADVSYNC_CTX_SELF(ctx), DLG_RESULT);
}

// ---------------------------------------------------------------------------
// Config validation.
// ---------------------------------------------------------------------------
bool config_pointers_ok(const pgrazor_advsync_config_v1 *c) {
    const void *const ptrs[] = {
        c->runtime_invoke, c->gchandle_new, c->gchandle_get_target, c->gchandle_free,
        c->object_get_class, c->method_on_confirm, c->method_on_decline, c->code_update,
        c->code_on_confirm, c->code_on_decline, c->code_init_initial,
        c->code_init_instruction, c->code_init_result, c->controller_class};
    for (const void *p : ptrs)
        if (p == nullptr) return false;
    if (c->off_on_confirm == 0 || c->off_on_decline == 0) return false;
    if (c->off_on_confirm >= 0x10000 || c->off_on_decline >= 0x10000) return false;
    if (c->off_on_confirm == c->off_on_decline) return false;
    return true;
}

// ---------------------------------------------------------------------------
// C API table functions.
// ---------------------------------------------------------------------------
int api_install(const pgrazor_advsync_config_v1 *config) {
    if (config == nullptr) return PGRAZOR_ADVSYNC_ERR_NULL_ARG;

    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (g_install_failed) return PGRAZOR_ADVSYNC_ERR_HOOK_FAILED;

    if (config->version != PGRAZOR_ADVSYNC_API_VERSION ||
        config->size != sizeof(pgrazor_advsync_config_v1) ||
        !config_pointers_ok(config)) {
        return PGRAZOR_ADVSYNC_ERR_BAD_CONFIG;
    }

    if (g_installed.load(std::memory_order_acquire)) {
        // Hook lives for the process lifetime; a repeat install of the identical config
        // is a no-op success, a different config is refused.
        return (std::memcmp(&g_config, config, sizeof(g_config)) == 0)
                   ? PGRAZOR_ADVSYNC_OK
                   : PGRAZOR_ADVSYNC_ERR_ALREADY_BOUND;
    }

    g_config = *config; // publish before any callback can read it (still under lock)

    member_fn u = nullptr, c = nullptr, d = nullptr;
    int rc_u = pgrazor_hook(g_config.code_update,
                            reinterpret_cast<void *>(&update_replacement),
                            reinterpret_cast<void **>(&u));
    int rc_c = pgrazor_hook(g_config.code_on_confirm,
                            reinterpret_cast<void *>(&on_confirm_replacement),
                            reinterpret_cast<void **>(&c));
    int rc_d = pgrazor_hook(g_config.code_on_decline,
                            reinterpret_cast<void *>(&on_decline_replacement),
                            reinterpret_cast<void **>(&d));
    // Entry instruments: signature-agnostic, read the `this` controller only.
    int rc_i0 = pgrazor_instrument(g_config.code_init_initial, &instrument_initial);
    int rc_i1 = pgrazor_instrument(g_config.code_init_instruction, &instrument_instruction);
    int rc_i2 = pgrazor_instrument(g_config.code_init_result, &instrument_result);

    // A reported success with a null trampoline would make our replacement swallow the
    // game's own method (the `if (orig)` chain becomes a no-op), silently skipping the
    // original Update/OnConfirm/OnDecline. That must never happen: undo exactly that
    // target so the original runs unmodified, and treat the install as failed.
    if (rc_u == 0 && u == nullptr) { pgrazor_unhook(g_config.code_update);     rc_u = PGRAZOR_ADVSYNC_ERR_HOOK_FAILED; }
    if (rc_c == 0 && c == nullptr) { pgrazor_unhook(g_config.code_on_confirm); rc_c = PGRAZOR_ADVSYNC_ERR_HOOK_FAILED; }
    if (rc_d == 0 && d == nullptr) { pgrazor_unhook(g_config.code_on_decline); rc_d = PGRAZOR_ADVSYNC_ERR_HOOK_FAILED; }

    if (rc_u != 0 || rc_c != 0 || rc_d != 0 || rc_i0 != 0 || rc_i1 != 0 || rc_i2 != 0) {
        // A failed partial patch is permanent and inert: targets that patched cleanly
        // keep their valid trampoline and merely chain the original with an empty queue
        // (policy can never arm because g_installed stays false and update() refuses);
        // targets we unhooked above run the game's original unmodified. No second engine
        // is ever stacked on these targets.
        g_orig_update.store(u, std::memory_order_release);
        g_orig_on_confirm.store(c, std::memory_order_release);
        g_orig_on_decline.store(d, std::memory_order_release);
        g_install_failed = true;
        logw("partial hook failure; policy permanently disabled, clean originals chained");
        return PGRAZOR_ADVSYNC_ERR_HOOK_FAILED;
    }

    g_orig_update.store(u, std::memory_order_release);
    g_orig_on_confirm.store(c, std::memory_order_release);
    g_orig_on_decline.store(d, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    return PGRAZOR_ADVSYNC_OK;
}

int api_update(int allowed, uint64_t lease_ms, uint64_t unity_tid) {
    if (!g_installed.load(std::memory_order_acquire)) return PGRAZOR_ADVSYNC_ERR_NOT_INSTALLED;
    if (unity_tid != 0) g_unity_tid.store(unity_tid, std::memory_order_release);

    if (allowed == 0) {
        // Clear the window. Captured dialogs are retired by the next Unity-thread
        // retire()/Update; nothing is driven and no GC handle is freed from here.
        g_allowed.store(false, std::memory_order_release);
        g_deadline_ns.store(0, std::memory_order_release);
        return PGRAZOR_ADVSYNC_OK;
    }

    g_allowed.store(false, std::memory_order_release);
    g_deadline_ns.store(0, std::memory_order_release);
    if (!lease_ok(lease_ms)) return PGRAZOR_ADVSYNC_ERR_BAD_LEASE;
    uint64_t now = boottime_ns();
    if (now == 0) return PGRAZOR_ADVSYNC_ERR_NO_CLOCK;

    g_deadline_ns.store(now + lease_ms * 1000000ull, std::memory_order_release);
    g_allowed.store(true, std::memory_order_release);
    return PGRAZOR_ADVSYNC_OK;
}

int api_retire(uint64_t unity_tid) {
    if (!g_installed.load(std::memory_order_acquire)) return PGRAZOR_ADVSYNC_ERR_NOT_INSTALLED;
    if (unity_tid != 0 && unity_tid == static_cast<uint64_t>(gettid()))
        g_unity_tid.store(unity_tid, std::memory_order_release);
    if (!on_unity_thread()) return PGRAZOR_ADVSYNC_OK; // only drop on the guard thread

    uint64_t now = boottime_ns();
    bool active = policy_active_now();
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    for (unsigned i = 0; i < kQueueMax; ++i) {
        if (!g_slots[i].in_use) continue;
        if (!active || now <= g_slots[i].captured_ns || now - g_slots[i].captured_ns > kExpiryNs)
            drop_locked(i);
    }
    return PGRAZOR_ADVSYNC_OK;
}

int api_selftest() {
    return pgrazor_adventure_sync_selfcheck();
}

// Ordered, read-only table (never reordered; see adventure_sync.h).
const pgrazor_advsync_api_v1 g_api = {
    PGRAZOR_ADVSYNC_API_VERSION,
    static_cast<uint32_t>(sizeof(pgrazor_advsync_api_v1)),
    &api_install,
    &api_update,
    &api_retire,
    &api_selftest,
};

} // namespace

// ---------------------------------------------------------------------------
// Hidden getter. NOT exported by the version script; the coordinator registers it in
// the core getter registry, which answers only on an exact version match.
// ---------------------------------------------------------------------------
const void *pgrazor_adventure_sync_api(uint32_t version) {
    if (version != PGRAZOR_ADVSYNC_API_VERSION) return nullptr;
    return &g_api;
}

// ---------------------------------------------------------------------------
// Host-independent policy self-check (no Dobby, no device, no image mutation).
// ---------------------------------------------------------------------------
int pgrazor_adventure_sync_selfcheck(void) {
    if (dialog_is_confirm(DLG_INITIAL)) return 1;      // initial must decline
    if (dialog_is_confirm(DLG_INSTRUCTION)) return 2;  // instruction must decline
    if (!dialog_is_confirm(DLG_RESULT)) return 3;      // result must confirm

    if (lease_ok(0)) return 4;                         // zero lease rejected
    if (lease_ok(kMaxLeaseMs + 1)) return 5;           // over-cap lease rejected
    if (!lease_ok(kMaxLeaseMs)) return 6;              // boundary accepted
    if (!lease_ok(1)) return 7;                        // minimal lease accepted

    if (!(kDelayNs < kExpiryNs)) return 8;             // settle delay precedes expiry
    return 0;
}

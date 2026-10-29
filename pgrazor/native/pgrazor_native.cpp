// PGRazor native migration pilot: spawn-getter radius floor via a single Dobby hook.
//
// Scope is deliberately narrow (one production hook) so the ARM gameplay runtime can
// keep using the Frida shim and migrate call sites incrementally through the stable
// pgrazor_native_api table. The hook mirrors runtime/features/spawn.js: call the
// original double(void*,void*) getter, then raise a finite positive radius to a floor
// of 80 only while an enabled+allowed policy window with a valid monotonic deadline is
// live. JS still performs its metadata validation before calling install(address).

#include "pgrazor_native_api.h"
#include "pgrazor_internal.h"

#include <atomic>
#include <cmath>
#include <ctime>
#include <mutex>
#include <cstdlib>
#include <android/log.h>

#include "hook_engine.h"

#include "throws.h"
#include "ar_throw.h"
#include "always_hit.h"
#include "rocket_minigame.h"
#include "catch_animation.h"
#include "spin_camera.h"
#include "incubation.h"
#include "quick_spin.h"
#include "buddy_feeding.h"
#include "items.h"
#include "accounts.h"
#include "iv_display.h"
#include "incoming_gifts.h"
#include "encounter_berries.h"
#include "battles.h"
#include "rocket_actions.h"
#include "rocket_combat.h"
#include "outgoing_gifts.h"
#include "map_tap.h"
#include "adventure_sync.h"
#include "catch_presentation.h"
#include "transfer_policy.h"
#include "transfer_execution.h"
#include "catch_completion.h"
#include "summary_completion.h"
#include "details_completion.h"
#include "details_recovery.h"
#include "catch_canvas.h"
#include "catch_discovery.h"
#include "camera_sampler.h"
#include "map_poi.h"
#include "routes_gate.h"
#include "routes_actions.h"
#include "routes_discovery.h"
#include "routes_services.h"
#include "routes_observers.h"
#include "routes_tracking.h"
#include "shared_catch.h"
#include "runtime_transport.h"
#include "runtime_bootstrap.h"
#include "quickball_metadata.h"
#include "shared_metadata.h"
#include "transfer_observers.h"
#include "catch_wait.h"
#include "camera_metadata.h"
#include "routes_host.h"
#include "shared_host.h"
#include "spin_host.h"
#include "inventory_host.h"
#include "runtime_host.h"
#include "interaction_host.h"
#include "encounter_host.h"
#include "spawn_host.h"
#include "combat_host.h"
#include "camera_host.h"
#include "account_host.h"
#include "quickball_host.h"
#include "routes_metadata.h"
#include "catch_observers.h"
#include "runtime_metadata.h"
#include "unity_scheduler.h"
#include "vgp_actions.h"
#include "vgp_policy.h"
#include "vgp_marker.h"
#include "vgp_candidates.h"
#include "vgp_results.h"
#include "vgp_discovery.h"
#include "vgp_maintenance.h"
#include "vgp_aux.h"
#include "vgp_controller.h"
#include "transfer_facts.h"

namespace {

// ---------------------------------------------------------------------------
// Pure policy helpers (shared by the live hook and the deterministic self-check).
// ---------------------------------------------------------------------------

// Raise a radius to the fixed floor. Caller guarantees `radius` is finite and > 0.
inline double apply_floor(double radius) {
    return radius > PGRAZOR_SPAWN_FLOOR ? radius : PGRAZOR_SPAWN_FLOOR;
}

// The floor applies only when the window is enabled, externally allowed, and unexpired.
inline bool policy_decision(bool enabled, bool allowed, bool deadline_valid) {
    return enabled && allowed && deadline_valid;
}

// CLOCK_BOOTTIME in nanoseconds, or 0 if the clock is unavailable. 0 is treated as
// "no valid time" everywhere, so it can never satisfy the `now < deadline` gate.
uint64_t boottime_ns() {
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return 0;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// ---------------------------------------------------------------------------
// Spawn getter state.
// ---------------------------------------------------------------------------

typedef double (*spawn_getter_fn)(void *, void *);

std::mutex g_install_mutex;                     // serialises install() against itself

std::atomic<spawn_getter_fn> g_original{nullptr}; // original getter trampoline, published first
std::atomic<void *> g_bound_address{nullptr};     // target we are bound to, published last
bool g_install_failed=false;                    // protected by install mutex

std::atomic<bool>     g_enabled{false};
std::atomic<bool>     g_allowed{false};
std::atomic<uint64_t> g_deadline_ns{0};

// Pull-only telemetry (no callbacks into JS).
std::atomic<double>   g_last_original{0.0};
std::atomic<double>   g_last_effective{0.0};
std::atomic<uint64_t> g_floor_count{0};

bool policy_active_now() {
    if (!g_enabled.load(std::memory_order_acquire)) return false;
    if (!g_allowed.load(std::memory_order_acquire)) return false;
    uint64_t now = boottime_ns();
    uint64_t deadline = g_deadline_ns.load(std::memory_order_acquire);
    return now != 0 && now < deadline;
}

// Dobby replacement. Chains the original getter, then conditionally floors the result.
double spawn_replacement(void *self, void *info) {
    spawn_getter_fn orig = g_original.load(std::memory_order_acquire);
    if (orig == nullptr) {
        // Dobby activates before returning its trampoline. Serialize this short
        // publication window; never return a fabricated radius from a live hook.
        std::lock_guard<std::mutex> lock(g_install_mutex);
        orig = g_original.load(std::memory_order_acquire);
    }
    if (orig == nullptr) std::abort(); // failed partially installed hook is unrecoverable

    double radius = orig(self, info);

    double effective = radius;
    if (std::isfinite(radius) && radius > 0.0 && policy_active_now()) {
        effective = apply_floor(radius);
        if(effective != radius) g_floor_count.fetch_add(1, std::memory_order_relaxed);
    }

    g_last_original.store(radius, std::memory_order_relaxed);
    g_last_effective.store(effective, std::memory_order_relaxed);
    return effective;
}

// ---------------------------------------------------------------------------
// C API table functions.
// ---------------------------------------------------------------------------

int api_install(void *address) {
    if (address == nullptr) return PGRAZOR_ERR_NULL_ARG;

    std::lock_guard<std::mutex> lock(g_install_mutex);

    if(g_install_failed)return PGRAZOR_ERR_HOOK_FAILED;

    void *bound = g_bound_address.load(std::memory_order_acquire);
    if (bound != nullptr) {
        // Hook lives for the whole process; a repeat install of the same target is a
        // no-op success, but rebinding to a different address is refused.
        return (bound == address) ? PGRAZOR_OK : PGRAZOR_ERR_ALREADY_BOUND;
    }

    spawn_getter_fn original = nullptr;
    int rc = pgrazor_hook(address,
                       reinterpret_cast<void *>(&spawn_replacement),
                       reinterpret_cast<void **>(&original));
    if (rc != 0 || original == nullptr) {
        // In the pinned Dobby, nonzero returns precede patching. A zero result
        // with no trampoline is different: undo that binding before returning.
        if(rc == 0) pgrazor_unhook(address);
        g_install_failed=true;
        return PGRAZOR_ERR_HOOK_FAILED;
    }

    // Publish while holding the install mutex. An early callback waits for this
    // publication; later callbacks read the trampoline without locking.
    g_original.store(original, std::memory_order_release);
    g_bound_address.store(address, std::memory_order_release);
    return PGRAZOR_OK;
}

int api_update(int enabled, int allowed, uint64_t lease_ms) {
    if (enabled == 0) {
        // Disable clears the window; the hook stays installed and passes through.
        g_enabled.store(false, std::memory_order_release);
        g_deadline_ns.store(0, std::memory_order_release);
        return PGRAZOR_OK;
    }

    g_enabled.store(false, std::memory_order_release);
    g_deadline_ns.store(0, std::memory_order_release);
    if (allowed == 0) return PGRAZOR_OK;
    if (lease_ms == 0 || lease_ms > PGRAZOR_MAX_LEASE_MS) return PGRAZOR_ERR_BAD_LEASE;

    uint64_t now = boottime_ns();
    if (now == 0) return PGRAZOR_ERR_NO_CLOCK;

    g_allowed.store(allowed != 0, std::memory_order_release);
    g_deadline_ns.store(now + lease_ms * 1000000ull, std::memory_order_release);
    g_enabled.store(true, std::memory_order_release);
    return PGRAZOR_OK;
}

int api_sample(pgrazor_spawn_snapshot *out) {
    if (out == nullptr) return PGRAZOR_ERR_NULL_ARG;
    out->original = g_last_original.load(std::memory_order_relaxed);
    out->effective = g_last_effective.load(std::memory_order_relaxed);
    out->count = g_floor_count.load(std::memory_order_relaxed);
    return PGRAZOR_OK;
}

// ---------------------------------------------------------------------------
// Dobby own-function self-check (explicit call only; never automatic in production).
// Hooks a private function, verifies original chaining, detaches, and verifies the
// detach left no dangling hook.
// ---------------------------------------------------------------------------

std::atomic<int> g_selftest_calls{0};
int (*g_selftest_original)(int) = nullptr;

__attribute__((noinline)) int selftest_target(int x) {
    asm volatile("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
    return x + 1;
}

int selftest_replacement(int x) {
    g_selftest_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_selftest_original) std::abort();
    int base = g_selftest_original(x);
    return base + 100;
}

int api_selftest() {
    __android_log_write(ANDROID_LOG_INFO,"PGRazorNative","selftest enter");
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    static int cached = -1;
    if(cached != -1) return cached;
    cached = PGRAZOR_ERR_SELFTEST;
    int (*volatile call)(int) = &selftest_target;
    g_selftest_calls.store(0, std::memory_order_relaxed);
    g_selftest_original = nullptr;

    if (call(1) != 2) return PGRAZOR_ERR_SELFTEST; // baseline
    __android_log_write(ANDROID_LOG_INFO,"PGRazorNative","selftest baseline; hook begin");

    int rc = pgrazor_hook(reinterpret_cast<void *>(&selftest_target),
                       reinterpret_cast<void *>(&selftest_replacement),
                       reinterpret_cast<void **>(&g_selftest_original));
    if (rc != 0 || g_selftest_original == nullptr) return PGRAZOR_ERR_SELFTEST;
    __android_log_write(ANDROID_LOG_INFO,"PGRazorNative","selftest hook complete; invoke begin");

    int status = PGRAZOR_OK;
    // Chaining: replacement calls original (1+1) then adds 100 => 102, exactly once.
    if (call(1) != 102) status = PGRAZOR_ERR_SELFTEST;
    if (g_selftest_calls.load(std::memory_order_relaxed) != 1) status = PGRAZOR_ERR_SELFTEST;
    __android_log_write(ANDROID_LOG_INFO,"PGRazorNative","selftest invoke complete; restore begin");

    if (pgrazor_unhook(reinterpret_cast<void *>(&selftest_target)) != 0) status = PGRAZOR_ERR_SELFTEST;

    // Detach must restore original behaviour with no dangling hook.
    if (call(1) != 2) status = PGRAZOR_ERR_SELFTEST;
    __android_log_write(ANDROID_LOG_INFO,"PGRazorNative","selftest restore complete");

    // Retain the trampoline pointer even if detach failed; no dangling callback.
    cached = status;
    return status;
}

// Ordered, read-only table advertised by pgrazor_native_api(). Never reordered.
const pgrazor_native_api_v1 g_api = {
    PGRAZOR_NATIVE_API_VERSION,
    static_cast<uint32_t>(sizeof(pgrazor_native_api_v1)),
    &api_install,
    &api_update,
    &api_sample,
    &api_selftest,
};

const void *query_feature(uint32_t id, uint32_t version) {
    switch (id) {
        case 1: return pgrazor_throws_api(version);
        case 2: return pgrazor_adventure_sync_api(version);
        case 3: return pgrazor_catch_presentation_api(version);
        case 4: return pgrazor_transfer_policy_api(version);
        case 5: return pgrazor_ar_throw_api(version);
        case 6: return pgrazor_rocket_minigame_api(version);
        case 7: return pgrazor_always_hit_api(version);
        case 8: return pgrazor_catch_animation_api(version);
        case 9: return pgrazor_incubation_api(version);
        case 10: return pgrazor_spin_camera_api(version);
        case 11: return pgrazor_quick_spin_api(version);
        case 12: return pgrazor_buddy_feeding_api(version);
        case 13: return pgrazor_map_tap_api(version);
        case 14: return pgrazor_items_api(version);
        case 15: return pgrazor_iv_display_api(version);
        case 16: return pgrazor_accounts_api(version);
        case 17: return pgrazor_incoming_gifts_api(version);
        case 18: return pgrazor_encounter_berries_api(version);
        case 19: return pgrazor_battles_api(version);
        case 20: return pgrazor_rocket_actions_api(version);
        case 21: return pgrazor_rocket_combat_api(version);
        case 22: return pgrazor_outgoing_gifts_api(version);
        case 23: return pgrazor_transfer_execution_api(version);
        case 24: return pgrazor_catch_completion_api(version);
        case 25: return pgrazor_vgp_policy_api(version);
        case 26: return pgrazor_vgp_actions_api(version);
        case 27: return pgrazor_routes_gate_api(version);
        case 28: return pgrazor_vgp_marker_api(version);
        case 29: return pgrazor_vgp_candidates_api(version);
        case 30: return pgrazor_vgp_results_api(version);
        case 31: return pgrazor_vgp_discovery_api(version);
        case 32: return pgrazor_vgp_maintenance_api(version);
        case 33: return pgrazor_vgp_controller_api(version);
        case 34: return pgrazor_vgp_aux_api(version);
        case 35: return pgrazor_transfer_facts_api(version);
        case 36: return pgrazor_summary_completion_api(version);
        case 37: return pgrazor_details_completion_api(version);
        case 38: return pgrazor_details_recovery_api(version);
        case 39: return pgrazor_catch_canvas_api(version);
        case 40: return pgrazor_catch_discovery_api(version);
        case 41: return pgrazor_camera_sampler_api(version);
        case 42: return pgrazor_map_poi_api(version);
        case 43: return pgrazor_routes_actions_api(version);
        case 44: return pgrazor_routes_discovery_api(version);
        case 45: return pgrazor_routes_services_api(version);
        case 46: return pgrazor_routes_observers_api(version);
        case 47: return pgrazor_runtime_metadata_api(version);
        case 48: return pgrazor_unity_scheduler_api(version);
        case 49: return pgrazor_catch_observers_api(version);
        case 50: return pgrazor_routes_tracking_api(version);
        case 51: return pgrazor_shared_catch_api(version);
        case 52: return pgrazor_runtime_transport_api(version);
        case 53: return pgrazor_routes_metadata_api(version);
        case 54: return pgrazor_runtime_bootstrap_api(version);
        case 55: return pgrazor_quickball_metadata_api(version);
        case 56: return pgrazor_shared_metadata_api(version);
        case 57: return pgrazor_transfer_observers_api(version);
        case 58: return pgrazor_catch_wait_api(version);
        case 59: return pgrazor_camera_metadata_api(version);
        case 60: return pgrazor_routes_host_api(version);
        case 61: return pgrazor_shared_host_api(version);
        case 62: return pgrazor_quickball_host_api(version);
        case 65: return pgrazor_inventory_host_api(version);
        case 64: return pgrazor_camera_host_api(version);
        case 66: return pgrazor_combat_host_api(version);
        case 67: return pgrazor_spin_host_api(version);
        case 68: return pgrazor_account_host_api(version);
        case 70: return pgrazor_runtime_host_api(version);
        case 63: return pgrazor_encounter_host_api(version);
        case 69: return pgrazor_interaction_host_api(version);
        case 71: return pgrazor_spawn_host_api(version);
        default: return nullptr;
    }
}

const pgrazor_native_api_v2 g_api_v2 = {
    2, static_cast<uint32_t>(sizeof(pgrazor_native_api_v2)),
    &api_install, &api_update, &api_sample, &api_selftest, &query_feature,
};
static_assert(sizeof(pgrazor_native_api_v1) == 40, "v1 ABI");
static_assert(sizeof(pgrazor_native_api_v2) == 48, "v2 ABI");

} // namespace

// ---------------------------------------------------------------------------
// Exported symbol (see pgrazor.map).
// ---------------------------------------------------------------------------

extern "C" __attribute__((visibility("default")))
const void *pgrazor_native_api(uint32_t version) {
    if (version == 2) return &g_api_v2;
    if (version != PGRAZOR_NATIVE_API_VERSION) return nullptr; // exact match only in the pilot
    return &g_api;
}

// ---------------------------------------------------------------------------
// Deterministic policy self-check for the JNI host surface (no Dobby, no device).
// ---------------------------------------------------------------------------

int pgrazor_policy_selfcheck(void) {
    if (apply_floor(50.0) != PGRAZOR_SPAWN_FLOOR) return 1;    // below floor is raised
    if (apply_floor(90.0) != 90.0) return 2;                  // above floor is preserved
    if (apply_floor(PGRAZOR_SPAWN_FLOOR) != PGRAZOR_SPAWN_FLOOR) return 3; // boundary

    if (!policy_decision(true, true, true)) return 4;         // all gates open -> apply
    if (policy_decision(false, true, true)) return 5;         // disabled -> off
    if (policy_decision(true, false, true)) return 6;         // not allowed -> off
    if (policy_decision(true, true, false)) return 7;         // expired deadline -> off
    return 0;
}

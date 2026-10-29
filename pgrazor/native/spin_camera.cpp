#include "spin_camera.h"
#include "hook_engine.h"
#include "interaction_observer.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

namespace {

constexpr uint64_t kMaxLeaseMs = 2500ULL;
constexpr uint64_t kNsPerMs = 1000000ULL;
constexpr uint64_t kPhaseExpiryNs = 15000ULL * kNsPerMs;
constexpr uintptr_t kIteratorSpeedOffset = 0x50U;
constexpr float kTargetMinSpeed = 25.0f;

enum PhaseId : uint32_t {
    kPhaseNone = 0,
    kPhaseIn = 1,
    kPhaseOut = 2
};

using StateLifecycleFn = void (*)(void *self, void *method_info);
using MoveNextFn = bool (*)(void *self, void *method_info);

struct PhaseSlot {
    void *handle = nullptr;
    uint64_t epoch = 0;
    uint64_t deadline_ns = 0;
};

std::mutex g_install_mutex;
PgrSpinCameraConfig g_cfg{};
std::atomic<int> g_install_state{0}; // 0 = uninstalled, 1 = installed, -1 = faulted

std::atomic<StateLifecycleFn> g_orig_in_enter{nullptr};
std::atomic<StateLifecycleFn> g_orig_in_exit{nullptr};
std::atomic<StateLifecycleFn> g_orig_out_enter{nullptr};
std::atomic<StateLifecycleFn> g_orig_out_exit{nullptr};
std::atomic<MoveNextFn> g_orig_move_next{nullptr};

// Policy atomics updated by JS adapter via update()/tick().
std::atomic<int> g_allowed{0};
std::atomic<uint64_t> g_lease_deadline_ns{0};
std::atomic<uint64_t> g_proven_tid{0};
std::atomic<uint64_t> g_owner_tid{0};
std::atomic<uint64_t> g_epoch{0};
std::atomic<int> g_pending_prune{0};

// Unity-thread-only state (owned exclusively by g_owner_tid).
PhaseSlot g_slots[2]{}; // [0] = in (Approach), [1] = out (Resolve)
int g_callback_depth = 0;

// Telemetry atomics.
std::atomic<uint32_t> g_fast_serial{0};
std::atomic<uint32_t> g_error_serial{0};
std::atomic<uint32_t> g_last_phase{kPhaseNone};
std::atomic<uint32_t> g_last_error{PGR_SCAM_RTERR_NONE};
std::atomic<uint32_t> g_active_roots_mask{0};
std::atomic<uint32_t> g_fault{0};
std::atomic<float> g_last_original_speed{0.0f};
std::atomic<float> g_last_applied_speed{0.0f};
std::atomic<uint64_t> g_fast_count{0};
std::atomic<uint64_t> g_enter_count{0};
std::atomic<uint64_t> g_exit_count{0};
std::atomic<uint64_t> g_prune_count{0};

inline uint64_t current_tid() noexcept {
    return static_cast<uint64_t>(static_cast<pid_t>(::gettid()));
}

inline bool read_boottime_ns(uint64_t *out_ns) noexcept {
    struct timespec ts {};
    if (::clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) {
        return false;
    }
    *out_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
              static_cast<uint64_t>(ts.tv_nsec);
    return true;
}

inline void record_runtime_error(uint32_t code) noexcept {
    g_last_error.store(code, std::memory_order_relaxed);
    g_error_serial.fetch_add(1, std::memory_order_release);
}

inline void refresh_active_mask() noexcept {
    uint32_t mask = 0;
    if (g_slots[0].handle != nullptr) mask |= 1U;
    if (g_slots[1].handle != nullptr) mask |= 2U;
    g_active_roots_mask.store(mask, std::memory_order_relaxed);
}

struct CallbackDepthGuard {
    CallbackDepthGuard() noexcept { ++g_callback_depth; }
    ~CallbackDepthGuard() noexcept { --g_callback_depth; }
};

struct ScopedGcHandle {
    void *handle = nullptr;
    ScopedGcHandle() noexcept = default;
    explicit ScopedGcHandle(void *h) noexcept : handle(h) {}
    ~ScopedGcHandle() noexcept {
        if (handle != nullptr && g_cfg.gchandle_free != nullptr) {
            g_cfg.gchandle_free(handle);
            handle = nullptr;
        }
    }
    ScopedGcHandle(const ScopedGcHandle &) = delete;
    ScopedGcHandle &operator=(const ScopedGcHandle &) = delete;
};

// Must only be called on g_owner_tid.
void free_slot_on_owner(int idx, bool count_prune) noexcept {
    if (g_slots[idx].handle != nullptr) {
        void *h = g_slots[idx].handle;
        g_slots[idx].handle = nullptr;
        g_slots[idx].epoch = 0;
        g_slots[idx].deadline_ns = 0;
        g_cfg.gchandle_free(h);
        if (count_prune) {
            g_prune_count.fetch_add(1, std::memory_order_relaxed);
        }
        refresh_active_mask();
    }
}

inline bool is_spin_permitted(uint64_t now_ns) noexcept {
    if (g_install_state.load(std::memory_order_acquire) != 1) return false;
    if (g_allowed.load(std::memory_order_acquire) == 0) return false;
    const uint64_t deadline = g_lease_deadline_ns.load(std::memory_order_acquire);
    return deadline != 0 && now_ns < deadline;
}

// Must only be called on the proven/owner Unity thread.
void prune_slots_on_owner(uint64_t now_ns, bool permitted) noexcept {
    const uint64_t cur_epoch = g_epoch.load(std::memory_order_acquire);
    const bool force_all = g_pending_prune.exchange(0, std::memory_order_acq_rel) != 0 || !permitted;
    for (int i = 0; i < 2; ++i) {
        if (g_slots[i].handle == nullptr) continue;
        if (force_all || g_slots[i].epoch != cur_epoch || now_ns >= g_slots[i].deadline_ns) {
            free_slot_on_owner(i, true);
        }
    }
}

// Checks whether the calling thread is the verified Unity owner thread.
// Latches g_owner_tid on first on-thread observation matching g_proven_tid.
bool verify_and_latch_owner_thread() noexcept {
    const uint64_t proven = g_proven_tid.load(std::memory_order_acquire);
    if (proven == 0) return false;
    const uint64_t tid = current_tid();
    if (tid != proven) return false;
    uint64_t expected = 0;
    if (!g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
        if (expected != tid) {
            return false;
        }
    }
    return true;
}

template <typename Fn>
Fn original(std::atomic<Fn>& slot) {
    Fn fn = slot.load(std::memory_order_acquire);
    if (!fn) { std::lock_guard<std::mutex> lock(g_install_mutex); fn = slot.load(std::memory_order_acquire); }
    return fn;
}

void handle_enter_state(int slot_idx, void *expected_class, void *self) noexcept {
    if (self == nullptr || !verify_and_latch_owner_thread()) return;
    CallbackDepthGuard depth;
    uint64_t now_ns = 0;
    if (!read_boottime_ns(&now_ns)) {
        record_runtime_error(PGR_SCAM_RTERR_CLOCK);
        return;
    }
    const bool permitted = is_spin_permitted(now_ns);
    if (!permitted) {
        if (g_callback_depth == 1) {
            prune_slots_on_owner(now_ns, false);
        }
        return;
    }
    if (g_cfg.object_get_class(self) != expected_class) return;

    prune_slots_on_owner(now_ns, true);
    // Root self before replacing the slot.
    void *new_handle = g_cfg.gchandle_new(self, 0);
    if (new_handle == nullptr) {
        record_runtime_error(PGR_SCAM_RTERR_GCHANDLE_ENTER);
        return;
    }
    if (g_slots[slot_idx].handle != nullptr) {
        free_slot_on_owner(slot_idx, false);
    }
    g_slots[slot_idx].handle = new_handle;
    g_slots[slot_idx].epoch = g_epoch.load(std::memory_order_acquire);
    g_slots[slot_idx].deadline_ns = now_ns + kPhaseExpiryNs;
    refresh_active_mask();
    g_enter_count.fetch_add(1, std::memory_order_relaxed);
}

void handle_exit_state(int slot_idx, void *self) noexcept {
    if (self == nullptr || !verify_and_latch_owner_thread()) return;
    CallbackDepthGuard depth;
    if (g_slots[slot_idx].handle == nullptr) return;
    void *target = g_cfg.gchandle_get_target(g_slots[slot_idx].handle);
    if (target == nullptr || target == self) {
        free_slot_on_owner(slot_idx, false);
        g_exit_count.fetch_add(1, std::memory_order_relaxed);
    }
}

void hook_in_enter(void *self, void *method_info) {
    StateLifecycleFn orig = original(g_orig_in_enter);
    handle_enter_state(0, g_cfg.in_class, self);
    if (orig != nullptr) {
        orig(self, method_info);
    }
}

void hook_in_exit(void *self, void *method_info) {
    StateLifecycleFn orig = original(g_orig_in_exit);
    handle_exit_state(0, self);
    if (orig != nullptr) {
        orig(self, method_info);
    }
}

void hook_out_enter(void *self, void *method_info) {
    StateLifecycleFn orig = original(g_orig_out_enter);
    pgr_interaction_owner::Phase observer(8);
    pgr_interaction_owner::Pins pins(observer.wanted(),reinterpret_cast<void*>(g_cfg.gchandle_new),reinterpret_cast<void*>(g_cfg.gchandle_get_target),reinterpret_cast<void*>(g_cfg.gchandle_free),self);
    pgr_interaction_owner::AbortBeforeRoots unwind{observer};
    if(pins.valid)handle_enter_state(1, g_cfg.out_class, pins.at(0));
    if (orig != nullptr) {
        if(pins.valid)observer.enter(pins.at(0));
        orig(pins.at(0), method_info);
        observer.leave();
    }
}

void hook_out_exit(void *self, void *method_info) {
    StateLifecycleFn orig = original(g_orig_out_exit);
    pgr_interaction_owner::Phase observer(9);
    pgr_interaction_owner::Pins pins(observer.wanted(),reinterpret_cast<void*>(g_cfg.gchandle_new),reinterpret_cast<void*>(g_cfg.gchandle_get_target),reinterpret_cast<void*>(g_cfg.gchandle_free),self);
    pgr_interaction_owner::AbortBeforeRoots unwind{observer};
    if(pins.valid)handle_exit_state(1, pins.at(0));
    if (orig != nullptr) {
        if(pins.valid)observer.enter(pins.at(0));
        orig(pins.at(0), method_info);
        observer.leave();
    }
}

bool hook_move_next(void *self, void *method_info) {
    MoveNextFn orig = original(g_orig_move_next);
    if (orig == nullptr) {
        return false;
    }

    ScopedGcHandle iter_root;
    uint64_t matched_epoch = 0;
    uint32_t matched_phase = kPhaseNone;

    const bool on_owner = (self != nullptr && verify_and_latch_owner_thread());
    if (on_owner) {
        CallbackDepthGuard depth;
        uint64_t now_ns = 0;
        if (!read_boottime_ns(&now_ns)) {
            record_runtime_error(PGR_SCAM_RTERR_CLOCK);
        } else {
            const bool permitted = is_spin_permitted(now_ns);
            if (!permitted) {
                if (g_callback_depth == 1) {
                    prune_slots_on_owner(now_ns, false);
                }
            } else if (g_cfg.object_get_class(self) == g_cfg.iterator_class) {
                int32_t state_before = -1;
                g_cfg.field_get_value(self, g_cfg.state_field, &state_before);
                if (state_before == 0) {
                    prune_slots_on_owner(now_ns, true);
                    void *camera = nullptr;
                    g_cfg.field_get_value(self, g_cfg.owner_field, &camera);
                    if (camera != nullptr && g_cfg.object_get_class(camera) == g_cfg.spline_class) {
                        void *cam_fields[2] = {g_cfg.in_camera_field, g_cfg.out_camera_field};
                        uint32_t phases[2] = {kPhaseIn, kPhaseOut};
                        for (int i = 0; i < 2; ++i) {
                            if (g_slots[i].handle == nullptr) continue;
                            void *state_obj = g_cfg.gchandle_get_target(g_slots[i].handle);
                            if (state_obj == nullptr) {
                                free_slot_on_owner(i, true);
                                continue;
                            }
                            void *slot_cam = nullptr;
                            g_cfg.field_get_value(state_obj, cam_fields[i], &slot_cam);
                            if (slot_cam == camera) {
                                void *h = g_cfg.gchandle_new(self, 0);
                                if (h == nullptr) {
                                    record_runtime_error(PGR_SCAM_RTERR_GCHANDLE_ITER);
                                } else {
                                    iter_root.handle = h;
                                    matched_epoch = g_epoch.load(std::memory_order_acquire);
                                    matched_phase = phases[i];
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    // Always chain original MoveNext with unmodified arguments.
    const bool ret = orig(self, method_info);

    if (iter_root.handle != nullptr) {
        CallbackDepthGuard depth;
        uint64_t now_ns = 0;
        if (!read_boottime_ns(&now_ns)) {
            record_runtime_error(PGR_SCAM_RTERR_CLOCK);
        } else if (ret && is_spin_permitted(now_ns) &&
                   matched_epoch == g_epoch.load(std::memory_order_acquire)) {
            void *o = g_cfg.gchandle_get_target(iter_root.handle);
            if (o != nullptr) {
                int32_t state_after = -1;
                g_cfg.field_get_value(o, g_cfg.state_field, &state_after);
                if (state_after == 1) {
                    auto *speed_ptr = reinterpret_cast<float *>(
                        reinterpret_cast<uint8_t *>(o) + kIteratorSpeedOffset);
                    const float speed = *speed_ptr;
                    if (std::isfinite(speed) && speed > 0.0f) {
                        const float applied = speed >= kTargetMinSpeed ? speed : kTargetMinSpeed;
                        *speed_ptr = applied;
                        g_last_original_speed.store(speed, std::memory_order_relaxed);
                        g_last_applied_speed.store(applied, std::memory_order_relaxed);
                        g_last_phase.store(matched_phase, std::memory_order_relaxed);
                        g_fast_count.fetch_add(1, std::memory_order_relaxed);
                        g_fast_serial.fetch_add(1, std::memory_order_release);
                    }
                }
            }
        }
    }

    return ret;
}

bool validate_config(const PgrSpinCameraConfig *cfg) noexcept {
    if (cfg == nullptr) return false;
    if (cfg->version != 1 || cfg->size != sizeof(PgrSpinCameraConfig)) return false;
    if (cfg->gchandle_new == nullptr || cfg->gchandle_get_target == nullptr ||
        cfg->gchandle_free == nullptr || cfg->object_get_class == nullptr ||
        cfg->field_get_value == nullptr) {
        return false;
    }
    if (cfg->in_enter_addr == nullptr || cfg->in_exit_addr == nullptr ||
        cfg->out_enter_addr == nullptr || cfg->out_exit_addr == nullptr ||
        cfg->move_next_addr == nullptr) {
        return false;
    }
    if (cfg->in_class == nullptr || cfg->out_class == nullptr ||
        cfg->spline_class == nullptr || cfg->iterator_class == nullptr) {
        return false;
    }
    if (cfg->in_camera_field == nullptr || cfg->out_camera_field == nullptr ||
        cfg->owner_field == nullptr || cfg->state_field == nullptr) {
        return false;
    }
    // Ensure all 5 hook targets are distinct addresses.
    void *addrs[5] = {
        cfg->in_enter_addr, cfg->in_exit_addr,
        cfg->out_enter_addr, cfg->out_exit_addr,
        cfg->move_next_addr
    };
    for (int i = 0; i < 5; ++i) {
        for (int j = i + 1; j < 5; ++j) {
            if (addrs[i] == addrs[j]) return false;
        }
    }
    return true;
}

template <typename Fn>
bool install_one_hook(void *target, void *replacement, std::atomic<Fn> *orig_slot) noexcept {
    void *orig = nullptr;
    const int rc = pgrazor_hook(target, replacement, &orig);
    if (rc != 0) {
        return false;
    }
    if (orig == nullptr) {
        pgrazor_unhook(target);
        return false;
    }
    orig_slot->store(reinterpret_cast<Fn>(orig), std::memory_order_release);
    return true;
}

int api_install(const PgrSpinCameraConfig *config) {
    if (config == nullptr) return PGR_SCAM_ERR_NULL_ARG;
    if (!validate_config(config)) return PGR_SCAM_ERR_BAD_CONFIG;

    std::lock_guard<std::mutex> lock(g_install_mutex);
    const int state = g_install_state.load(std::memory_order_acquire);
    if (state == -1) {
        return PGR_SCAM_ERR_HOOK_FAILED;
    }
    if (state == 1) {
        if (std::memcmp(&g_cfg, config, sizeof(PgrSpinCameraConfig)) == 0) {
            return PGR_SCAM_OK;
        }
        return PGR_SCAM_ERR_ALREADY_BOUND;
    }

    g_cfg = *config;

    // Install hooks under g_install_mutex. Retain valid originals on partial failure
    // so any already-patched site continues chaining cleanly as a pass-through.
    if (!install_one_hook(g_cfg.in_enter_addr, reinterpret_cast<void *>(&hook_in_enter), &g_orig_in_enter) ||
        !install_one_hook(g_cfg.in_exit_addr, reinterpret_cast<void *>(&hook_in_exit), &g_orig_in_exit) ||
        !install_one_hook(g_cfg.out_enter_addr, reinterpret_cast<void *>(&hook_out_enter), &g_orig_out_enter) ||
        !install_one_hook(g_cfg.out_exit_addr, reinterpret_cast<void *>(&hook_out_exit), &g_orig_out_exit) ||
        !install_one_hook(g_cfg.move_next_addr, reinterpret_cast<void *>(&hook_move_next), &g_orig_move_next)) {
        g_allowed.store(0, std::memory_order_release);
        g_lease_deadline_ns.store(0, std::memory_order_release);
        g_fault.store(1, std::memory_order_release);
        g_install_state.store(-1, std::memory_order_release);
        return PGR_SCAM_ERR_HOOK_FAILED;
    }

    g_install_state.store(1, std::memory_order_release);
    return PGR_SCAM_OK;
}

int api_update(int allowed, uint64_t remaining_lease_ms, uint64_t proven_tid, uint64_t epoch) {
    if (g_install_state.load(std::memory_order_acquire) != 1) {
        return PGR_SCAM_ERR_HOOK_FAILED;
    }

    const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
    if (allowed && (remaining_lease_ms > kMaxLeaseMs || (owner && owner != proven_tid))) {
        g_allowed.store(0); g_lease_deadline_ns.store(0); g_pending_prune.store(1);
        return PGR_SCAM_ERR_BAD_CONFIG;
    }
    const uint64_t prev_epoch = g_epoch.exchange(epoch, std::memory_order_acq_rel);
    if (proven_tid != 0) {
        g_proven_tid.store(proven_tid, std::memory_order_release);
    }

    if (allowed == 0 || remaining_lease_ms == 0 || proven_tid == 0) {
        g_allowed.store(0, std::memory_order_release);
        g_lease_deadline_ns.store(0, std::memory_order_release);
        g_pending_prune.store(1, std::memory_order_release);
    } else {
        uint64_t now_ns = 0;
        if (!read_boottime_ns(&now_ns)) {
            g_allowed.store(0, std::memory_order_release);
            g_lease_deadline_ns.store(0, std::memory_order_release);
            g_pending_prune.store(1, std::memory_order_release);
            record_runtime_error(PGR_SCAM_RTERR_CLOCK);
            return PGR_SCAM_ERR_CLOCK_FAILED;
        }
        const uint64_t clamped_ms = remaining_lease_ms > kMaxLeaseMs ? kMaxLeaseMs : remaining_lease_ms;
        g_lease_deadline_ns.store(now_ns + clamped_ms * kNsPerMs, std::memory_order_release);
        g_allowed.store(1, std::memory_order_release);
        if (prev_epoch != epoch) {
            g_pending_prune.store(1, std::memory_order_release);
        }
    }

    // If update() is running directly on the verified Unity owner thread and not inside
    // a hook callback, prune immediately.
    if (verify_and_latch_owner_thread() && g_callback_depth == 0) {
        uint64_t now_ns = 0;
        if (read_boottime_ns(&now_ns)) {
            prune_slots_on_owner(now_ns, is_spin_permitted(now_ns));
        }
    }

    return PGR_SCAM_OK;
}

int api_tick(uint64_t proven_tid) {
    if (g_install_state.load(std::memory_order_acquire) != 1) return PGR_SCAM_ERR_HOOK_FAILED;
    if (!proven_tid || proven_tid != current_tid() || proven_tid != g_owner_tid.load()) return PGR_SCAM_OK;
    if (g_callback_depth == 0) {
        uint64_t now_ns = 0;
        if (read_boottime_ns(&now_ns)) prune_slots_on_owner(now_ns, is_spin_permitted(now_ns));
    }
    return PGR_SCAM_OK;
}

int api_sample(PgrSpinCameraSample *out) {
    if (out == nullptr) return PGR_SCAM_ERR_NULL_ARG;
    out->version = 1;
    out->size = sizeof(PgrSpinCameraSample);
    out->fast_serial = g_fast_serial.load(std::memory_order_acquire);
    out->error_serial = g_error_serial.load(std::memory_order_acquire);
    out->last_phase = g_last_phase.load(std::memory_order_relaxed);
    out->last_error = g_last_error.load(std::memory_order_relaxed);
    out->active_roots_mask = g_active_roots_mask.load(std::memory_order_relaxed);
    out->fault = g_fault.load(std::memory_order_relaxed);
    out->last_original_speed = g_last_original_speed.load(std::memory_order_relaxed);
    out->last_applied_speed = g_last_applied_speed.load(std::memory_order_relaxed);
    out->fast_count = g_fast_count.load(std::memory_order_relaxed);
    out->enter_count = g_enter_count.load(std::memory_order_relaxed);
    out->exit_count = g_exit_count.load(std::memory_order_relaxed);
    out->prune_count = g_prune_count.load(std::memory_order_relaxed);
    return PGR_SCAM_OK;
}

const PgrSpinCameraApi kApiTable = {
    1,
    sizeof(PgrSpinCameraApi),
    &api_install,
    &api_update,
    &api_tick,
    &api_sample
};

} // namespace

extern "C" const void *pgrazor_spin_camera_api(uint32_t version) {
    if (version != 1) return nullptr;
    return &kApiTable;
}

#include "combat_host.h"

#include "battles.h"
#include "hook_engine.h"
#include "rocket_actions.h"
#include "rocket_combat.h"
#include "rocket_minigame.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "shared_host.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <time.h>
#include <unistd.h>

namespace {

using namespace pgr_shared_meta;

template <typename T>
const T *api_table(const void *p) {
    return static_cast<const T *>(p);
}

using Fn2Ptr      = void (*)(void *self, void *method_info);
using Fn3Ptr      = void (*)(void *self, void *a1, void *method_info);
using Fn4Ptr      = void (*)(void *self, void *a1, void *a2, void *method_info);
using FnPtrBoolPtr = void (*)(void *self, uint8_t a1, void *method_info);

using FnPtrF32Ptr = void (*)(void *self, float a1, void *method_info);
using FnRetBoolPtr = uint8_t (*)(void *self, void *method_info);

struct PgrVec3Val {
    float x;
    float y;
    float z;
};
static_assert(sizeof(PgrVec3Val) == 12, "UnityEngine.Vector3 value ABI must be 12 bytes");

using FnLegInitPtr = void (*)(void *self, void *p1, void *p2, PgrVec3Val pos, void *method_info);

enum HookSlot : unsigned {
    // Rocket Combat dedicated hooks (10)
    HK_RK_ENTER        = 0,
    HK_RK_EXIT         = 1,
    HK_RK_UI_INIT      = 2,
    HK_RK_UI_UPDATE    = 3,
    HK_RK_UI_CLOSE     = 4,
    HK_RK_VN_UPDATE    = 5,
    HK_RK_VN_CLOSE     = 6,
    HK_RK_PROMPT       = 7,
    HK_RK_SHIELD       = 8,
    HK_RK_SHIELD_CLOSE = 9,

    // Shared ChargeAttackDisplayVNext.ObserveBattleState hook (1)
    HK_SHARED_VN_OBSERVE = 10,

    // Battle dedicated hooks (31)
    HK_BT_SC0_ENTER    = 11,
    HK_BT_SC0_EXIT     = 12,
    HK_BT_SC1_ENTER    = 13,
    HK_BT_SC1_EXIT     = 14,
    HK_BT_SC2_ENTER    = 15,
    HK_BT_SC2_EXIT     = 16,

    HK_BT_SW0_OPEN     = 17,
    HK_BT_SW0_CL0      = 18,
    HK_BT_SW0_CL1      = 19,
    HK_BT_SW1_OPEN     = 20,
    HK_BT_SW1_CL0      = 21,
    HK_BT_SW1_CL1      = 22,
    HK_BT_SW2_OPEN     = 23,
    HK_BT_SW2_CL0      = 24,
    HK_BT_SW2_CL1      = 25,

    HK_BT_ENABLE       = 26,
    HK_BT_BUTTONS      = 27,
    HK_BT_LEG_ENABLE   = 28,
    HK_BT_LEG_BUTTONS  = 29,
    HK_BT_DISABLE      = 30,
    HK_BT_LEG_DISABLE  = 31,
    HK_BT_DESTROY      = 32,

    HK_BT_LEG_INIT     = 33,
    HK_BT_MAX_REFRESH  = 34,
    HK_BT_MAX_CLEANUP  = 35,

    HK_BT_RAID_WARN    = 36,
    HK_BT_MAX_WARN     = 37,
    HK_BT_GYM_WARN     = 38,

    HK_BT_LEG_FAST     = 39,
    HK_BT_LEG_CHARGE   = 40,

    HK_COUNT           = 41
};

constexpr size_t kEventCap     = 256;
constexpr size_t kEventJsonMax = 16384;

struct QueuedEvent {
    PgrCombatHostEvent desc{};
    std::string json{};
};

// --- Process-lifetime state -------------------------------------------------------
std::mutex g_setup_mutex;
std::mutex g_options_mutex;
std::mutex g_event_mutex;

PgrCombatHostConfig g_cfg{};
PgrCombatHostConfig g_accepted_cfg{};
bool g_has_accepted_cfg = false;
bool g_default_bind_mode = false;
PgrSharedMetadataOutput g_md{};

const PgrRocketMiniApi         *g_rocket_mini    = nullptr;
const pgr_rocket_actions_api_v1 *g_rocket_actions = nullptr;
const pgr_rocket_combat_api_v1 *g_rocket_combat  = nullptr;
const pgr_battle_api_v1        *g_battles        = nullptr;

std::array<std::atomic<void *>, HK_COUNT> g_originals{};
std::array<void *, HK_COUNT> g_hook_targets{};

std::atomic<bool>     g_configured{false};
std::atomic<bool>     g_ready{false};
std::atomic<bool>     g_stopping{false};
std::atomic<bool>     g_failed{false};
std::atomic<bool>     g_leaves_latched{false};
std::atomic<bool>     g_in_tick{false};
std::atomic<int32_t>  g_last_error{PGR_CBH_OK};
std::atomic<uint32_t> g_capabilities{PGR_CBH_CAP_NONE};
std::atomic<uint32_t> g_installed_hooks{0};
std::atomic<uint32_t> g_active_frames{0};
std::atomic<uint32_t> g_mini_active{0};
std::atomic<uint64_t> g_owner_tid{0};
std::atomic<uint64_t> g_ticks{0};
std::atomic<uint64_t> g_gym_cooldown_serial{0};

// Guarded by g_options_mutex
PgrCombatHostOptions g_opts{};
uint64_t g_battle_deadline_ms = 0;
uint64_t g_rocket_deadline_ms = 0;
uint64_t g_policy_epoch       = 0;

// Guarded by g_event_mutex
std::array<QueuedEvent, kEventCap> g_events{};
size_t   g_event_head   = 0;
size_t   g_event_count  = 0;
uint64_t g_event_serial = 0;

// --- Low-level helpers ------------------------------------------------------------
uint64_t self_tid() {
    return static_cast<uint64_t>(static_cast<pid_t>(gettid()));
}

uint64_t now_ms() {
    timespec ts{};
#if defined(CLOCK_BOOTTIME)
    if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0 && ts.tv_sec >= 0 && ts.tv_nsec >= 0) {
        return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
               static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
    }
#endif
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0 && ts.tv_sec >= 0 && ts.tv_nsec >= 0) {
        return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
               static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
    }
    return 0;
}

bool thread_attached() {
    if (!g_cfg.thread_current) return false;
    return reinterpret_cast<void *(*)()>(g_cfg.thread_current)() != nullptr;
}

void *md_h(int i) { return const_cast<void *>(g_md.opt_handles[i]); }
void *md_k(int i) { return const_cast<void *>(g_md.opt_classes[i]); }
void *md_c(int i) { return const_cast<void *>(g_md.classes[i]); }
void *md_f(int i) { return const_cast<void *>(g_md.fields[i]); }
void *md_m(int i) { return const_cast<void *>(g_md.methods[i]); }

struct FrameGuard {
    FrameGuard() { g_active_frames.fetch_add(1, std::memory_order_relaxed); }
    ~FrameGuard() { g_active_frames.fetch_sub(1, std::memory_order_relaxed); }
};

void *load_original(HookSlot slot) {
    void *p = g_originals[slot].load(std::memory_order_acquire);
    if (!p) {
        std::lock_guard<std::mutex> lk(g_setup_mutex);
        p = g_originals[slot].load(std::memory_order_acquire);
    }
    if (!p) std::abort();
    return p;
}

void ensure_leaves_latched_on_owner(uint64_t tid) {
    if (g_leaves_latched.load(std::memory_order_acquire)) return;
    if (tid == 0 || self_tid() != tid) return;
    const uint32_t caps = g_capabilities.load(std::memory_order_acquire);
    if ((caps & PGR_CBH_CAP_ROCKET_COMBAT) && g_rocket_combat) {
        g_rocket_combat->update(0, 0, tid);
    }
    if ((caps & PGR_CBH_CAP_BATTLES) && g_battles && !g_in_tick.load(std::memory_order_relaxed)) {
        g_battles->tick(tid, 0);
    }
    if ((caps & PGR_CBH_CAP_ROCKET_MINIGAME) && g_rocket_mini) {
        g_rocket_mini->tick(1);
    }
    g_leaves_latched.store(true, std::memory_order_release);
}

bool observation_allowed(uint64_t *out_tid) {
    if (!g_ready.load(std::memory_order_acquire) ||
        g_stopping.load(std::memory_order_acquire) ||
        g_failed.load(std::memory_order_acquire)) {
        return false;
    }
    const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
    const uint64_t tid   = self_tid();
    if (owner == 0 || tid != owner || !thread_attached()) return false;

    // Game lifecycle/UI callbacks also run outside ExecuteTasks. Capture them on
    // the proven attached Unity owner while the scheduler generation is live;
    // automated actions still run from the scheduled tick. Requiring an active
    // ExecuteTasks frame here silently loses normal battle entry/UI callbacks.
    {
        const auto *sched3 = api_table<PgrUnitySchedulerApi3>(pgrazor_unity_scheduler_api(3));
        if (sched3 && sched3->version == 3 && sched3->size >= sizeof(PgrUnitySchedulerApi3) &&
            sched3->snapshot && sched3->valid) {
            PgrUnitySchedulerSnapshot snap{};
            if (sched3->snapshot(&snap) != PGR_US_OK) return false;
            if (snap.installed != 0) {
                // Snapshot owner is zero until the scheduler's first completed
                // pulse; the host has already proven its owner during setup.
                if (snap.poisoned || (snap.owner_tid && snap.owner_tid != static_cast<int32_t>(tid)) ||
                    !snap.generation || !sched3->valid(snap.generation)) {
                    return false;
                }
            }
        }
    }

    // Pure gate: never perform leaf ticks/GC before original arguments are pinned.
    if (!g_leaves_latched.load(std::memory_order_acquire)) return false;
    if (out_tid) *out_tid = tid;
    return true;
}

// --- Transport 52 & Event Queue Helpers -------------------------------------------
const PgrRuntimeTransportApi *transport_api() {
    const auto *tr = api_table<PgrRuntimeTransportApi>(pgrazor_runtime_transport_api(1));
    if (!tr || tr->version != 1 || tr->size < sizeof(PgrRuntimeTransportApi)) return nullptr;
    return tr;
}

void transport_text_action(uint32_t op, const char *msg) {
    if (!(g_cfg.flags & PGR_CBH_CFG_USE_TRANSPORT) || !msg) return;
    const auto *tr = transport_api();
    if (!tr || !tr->emit) return;
    PgrRuntimeTransportString s{msg, static_cast<uint32_t>(std::strlen(msg))};
    tr->emit(op, nullptr, 0, &s, 1);
}

void transport_ready_action(uint32_t op, bool ready, const char *msg) {
    if (!(g_cfg.flags & PGR_CBH_CFG_USE_TRANSPORT) || !msg) return;
    const auto *tr = transport_api();
    if (!tr || !tr->emit) return;
    int64_t val = ready ? 1 : 0;
    PgrRuntimeTransportString s{msg, static_cast<uint32_t>(std::strlen(msg))};
    tr->emit(op, &val, 1, &s, 1);
}

const char *event_kind_name(uint32_t kind) {
    switch (kind) {
        case PGR_CBH_EV_HOST_READY:            return "native-combat-host-ready";
        case PGR_CBH_EV_BATTLES_READY:         return "native-battles-ready";
        case PGR_CBH_EV_BATTLE_SCOPE:          return "battle-scope";
        case PGR_CBH_EV_BATTLE_INPUT:          return "battle-input";
        case PGR_CBH_EV_BATTLE_ENDED:          return "battle-ended";
        case PGR_CBH_EV_BATTLE_DODGE_CUE:      return "battle-dodge-cue";
        case PGR_CBH_EV_BATTLE_ACTION:         return "battle-action";
        case PGR_CBH_EV_BATTLE_MANUAL_SWITCH:  return "battle-manual-switch";
        case PGR_CBH_EV_BATTLE_ERROR:          return "battle-error";
        case PGR_CBH_EV_COOLDOWN_ACTION:       return "cooldown-action";
        case PGR_CBH_EV_ROCKET_COMBAT_READY:   return "native-rocket-combat-ready";
        case PGR_CBH_EV_ROCKET_MINIGAME_READY: return "rocket-minigame-native";
        case PGR_CBH_EV_ROCKET_SCOPE:          return "rocket-scope";
        case PGR_CBH_EV_ROCKET_UI:             return "rocket-ui";
        case PGR_CBH_EV_ROCKET_ACTION:         return "rocket-action";
        case PGR_CBH_EV_ROCKET_ATTACK_GATE:    return "rocket-attack-gate";
        case PGR_CBH_EV_ROCKET_ERROR:          return "rocket-error";
        default:                               return "combat-host-event";
    }
}

void queue_event(uint32_t kind, int64_t code0, int64_t code1, uint64_t tid, const std::string &json) {
    if (json.size() > kEventJsonMax) return;
    if (!(g_cfg.flags & PGR_CBH_CFG_USE_TRANSPORT)) {
        std::lock_guard<std::mutex> lk(g_event_mutex);
        if (g_event_count >= kEventCap) {
            // Drop oldest if ring buffer is full so newest error/scope transitions are preserved.
            g_event_head = (g_event_head + 1) % kEventCap;
            --g_event_count;
        }
        const size_t idx = (g_event_head + g_event_count) % kEventCap;
        QueuedEvent &slot = g_events[idx];
        slot.desc.version   = 1;
        slot.desc.size      = sizeof(PgrCombatHostEvent);
        slot.desc.kind      = kind;
        slot.desc.json_len  = static_cast<uint32_t>(json.size());
        slot.desc.code0     = code0;
        slot.desc.code1     = code1;
        slot.desc.serial    = ++g_event_serial;
        slot.desc.tid       = tid;
        slot.desc.reserved0 = 0;
        slot.desc.reserved1 = 0;
        slot.json           = json;
        ++g_event_count;
    }

    if (g_cfg.flags & PGR_CBH_CFG_USE_TRANSPORT) {
        const auto *tr = transport_api();
        if (tr && tr->emit) {
            const char *name = event_kind_name(kind);
            PgrRuntimeTransportString strs[2] = {
                {name, static_cast<uint32_t>(std::strlen(name))},
                {json.c_str(), static_cast<uint32_t>(json.size())}
            };
            tr->emit(19, nullptr, 0, strs, 2);
        }
    }
}

const char *battle_mode_label(int32_t mode) {
    switch (mode) {
        case 0:  return "Raid battle";
        case 1:  return "Gym battle";
        case 2:  return "Max battle";
        default: return "Battle";
    }
}

void drain_leaf_events(uint64_t tid) {
    const uint32_t caps = g_capabilities.load(std::memory_order_acquire);

    if ((caps & PGR_CBH_CAP_ROCKET_COMBAT) && g_rocket_combat) {
        pgr_rocket_combat_event_v1 rev{};
        for (int i = 0; i < 64 && g_rocket_combat->next_event(&rev) == 1; ++i) {
            char buf[256];
            switch (rev.code) {
                case PGR_RC_EVT_READY:
                    queue_event(PGR_CBH_EV_ROCKET_COMBAT_READY, 1, 0, tid, "{\"available\":true}");
                    break;
                case PGR_RC_EVT_SCOPE:
                    std::snprintf(buf, sizeof(buf), "{\"active\":%s}", rev.aux ? "true" : "false");
                    queue_event(PGR_CBH_EV_ROCKET_SCOPE, rev.aux ? 1 : 0, 0, tid, buf);
                    transport_text_action(47, rev.aux ? "Rocket battle active" : "Ready");
                    break;
                case PGR_RC_EVT_UI:
                    std::snprintf(buf, sizeof(buf), "{\"source\":\"%s\"}",
                                  rev.aux == 1 ? "vnext-update" : "live-update");
                    queue_event(PGR_CBH_EV_ROCKET_UI, rev.aux, 0, tid, buf);
                    break;
                case PGR_RC_EVT_ACTION:
                    if (rev.id == PGR_RC_ACT_SHIELD) {
                        queue_event(PGR_CBH_EV_ROCKET_ACTION, rev.id, rev.aux, tid,
                                    "{\"action\":\"shield\"}");
                        transport_text_action(47, "Rocket: shield");
                    } else if (rev.id == PGR_RC_ACT_CHARGED) {
                        if (rev.aux & 2) {
                            queue_event(PGR_CBH_EV_ROCKET_ACTION, rev.id, rev.aux, tid,
                                        "{\"action\":\"charged\",\"path\":\"vnext\"}");
                        } else {
                            queue_event(PGR_CBH_EV_ROCKET_ACTION, rev.id, rev.aux, tid,
                                        "{\"action\":\"charged\"}");
                        }
                        transport_text_action(47, "Rocket: charged");
                    } else {
                        if (rev.aux & 2) {
                            queue_event(PGR_CBH_EV_ROCKET_ACTION, rev.id, rev.aux, tid,
                                        "{\"action\":\"quick\",\"path\":\"vnext\"}");
                        } else {
                            std::snprintf(buf, sizeof(buf),
                                          "{\"action\":\"quick\",\"accepted\":%s}",
                                          (rev.aux & 1) ? "true" : "false");
                            queue_event(PGR_CBH_EV_ROCKET_ACTION, rev.id, rev.aux, tid, buf);
                        }
                        transport_text_action(47, "Rocket: quick");
                    }
                    break;
                case PGR_RC_EVT_GATE: {
                    static const char *const kGateReasons[8] = {
                        "unknown",
                        "waiting-for-battle-ui",
                        "director-unexpected",
                        "battle-not-active",
                        "active",
                        "vnext-input-blocked",
                        "vnext-fast-input-disabled",
                        "vnext-active"
                    };
                    const char *reason = (rev.id >= 1 && rev.id <= 7) ? kGateReasons[rev.id] : "unknown";
                    std::snprintf(buf, sizeof(buf), "{\"reason\":\"%s\"}", reason);
                    queue_event(PGR_CBH_EV_ROCKET_ATTACK_GATE, rev.id, rev.aux, tid, buf);
                    break;
                }
                case PGR_RC_EVT_ERROR:
                    queue_event(PGR_CBH_EV_ROCKET_ERROR, rev.id, rev.aux, tid,
                                "{\"reason\":\"native-managed-invoke\",\"backend\":\"native\"}");
                    transport_text_action(47, "Rocket automation paused: native-managed-invoke");
                    break;
                default:
                    break;
            }
        }
    }

    if ((caps & PGR_CBH_CAP_BATTLES) && g_battles) {
        pgr_battle_event_v1 bev{};
        for (int i = 0; i < 64 && g_battles->next_event(&bev) == 1; ++i) {
            char buf[256];
            switch (bev.code) {
                case PGR_BATTLE_EVT_READY:
                    queue_event(PGR_CBH_EV_BATTLES_READY, 1, 0, tid, "{\"available\":true}");
                    break;
                case PGR_BATTLE_EVT_SCOPE:
                    std::snprintf(buf, sizeof(buf), "{\"mode\":%d,\"active\":%s}",
                                  bev.id, bev.aux ? "true" : "false");
                    queue_event(PGR_CBH_EV_BATTLE_SCOPE, bev.id, bev.aux, tid, buf);
                    if (bev.aux) {
                        std::snprintf(buf, sizeof(buf), "%s active", battle_mode_label(bev.id));
                        transport_text_action(24, buf);
                    } else {
                        transport_text_action(24, "Ready");
                    }
                    break;
                case PGR_BATTLE_EVT_INPUT:
                    if (bev.aux & 2) {
                        std::snprintf(buf, sizeof(buf),
                                      "{\"mode\":%d,\"legacy\":%s,\"source\":\"scope\"}",
                                      bev.id, (bev.aux & 1) ? "true" : "false");
                    } else {
                        std::snprintf(buf, sizeof(buf), "{\"mode\":%d,\"legacy\":%s}",
                                      bev.id, (bev.aux & 1) ? "true" : "false");
                    }
                    queue_event(PGR_CBH_EV_BATTLE_INPUT, bev.id, bev.aux, tid, buf);
                    break;
                case PGR_BATTLE_EVT_ENDED:
                    std::snprintf(buf, sizeof(buf), "{\"mode\":%d,\"status\":%d}",
                                  bev.id, bev.aux);
                    queue_event(PGR_CBH_EV_BATTLE_ENDED, bev.id, bev.aux, tid, buf);
                    transport_text_action(24, "Battle ended");
                    break;
                case PGR_BATTLE_EVT_DODGE_CUE:
                    std::snprintf(buf, sizeof(buf), "{\"mode\":%d}", bev.id);
                    queue_event(PGR_CBH_EV_BATTLE_DODGE_CUE, bev.id, 0, tid, buf);
                    break;
                case PGR_BATTLE_EVT_ACTION: {
                    static const char *const kActions[4] = {
                        "quick", "charged", "dodge", "max-attack"
                    };
                    const char *act = (bev.aux >= 0 && bev.aux < 4) ? kActions[bev.aux] : "unknown";
                    std::snprintf(buf, sizeof(buf), "{\"mode\":%d,\"action\":\"%s\"}",
                                  bev.id, act);
                    queue_event(PGR_CBH_EV_BATTLE_ACTION, bev.id, bev.aux, tid, buf);
                    std::snprintf(buf, sizeof(buf), "%s: %s", battle_mode_label(bev.id), act);
                    transport_text_action(24, buf);
                    break;
                }
                case PGR_BATTLE_EVT_SWITCH:
                    std::snprintf(buf, sizeof(buf), "{\"mode\":%d,\"active\":%s}",
                                  bev.id, bev.aux ? "true" : "false");
                    queue_event(PGR_CBH_EV_BATTLE_MANUAL_SWITCH, bev.id, bev.aux, tid, buf);
                    if (bev.aux) {
                        transport_text_action(24, "Paused: swap menu open");
                    }
                    break;
                case PGR_BATTLE_EVT_ERROR:
                    std::snprintf(buf, sizeof(buf),
                                  "{\"mode\":%d,\"reason\":\"native-managed-invoke\"}",
                                  bev.id);
                    queue_event(PGR_CBH_EV_BATTLE_ERROR, bev.id, bev.aux, tid, buf);
                    transport_text_action(24, "Battle automation paused: native-managed-invoke");
                    break;
                default:
                    break;
            }
        }
    }
}

void emit_gym_cooldown(uint64_t tid) {
    g_gym_cooldown_serial.fetch_add(1, std::memory_order_release);
    queue_event(PGR_CBH_EV_COOLDOWN_ACTION, PGR_BATTLE_MODE_GYM, 0, tid,
                "{\"action\":\"Gym battle\"}");
    transport_text_action(17, "Gym battle");
}

// Recompute and push Rocket Minigame policy (matches rocketMinigame.js:54).
void sync_minigame_policy(uint64_t now) {
    const uint32_t caps = g_capabilities.load(std::memory_order_acquire);
    if (!(caps & PGR_CBH_CAP_ROCKET_MINIGAME) || !g_rocket_mini) {
        g_mini_active.store(0, std::memory_order_release);
        return;
    }

    std::lock_guard<std::mutex> policy_lock(g_options_mutex);
    const uint32_t flags=g_opts.flags,rocket_mask=g_opts.rocket_mask;
    const uint64_t rocket_deadline=g_rocket_deadline_ms;

    const bool active_ok = (flags & PGR_CBH_OPT_ACTIVE) != 0 &&
                           (flags & PGR_CBH_OPT_ROCKET_ALLOWED) != 0 &&
                           (flags & PGR_CBH_OPT_STOPPING) == 0 &&
                           !g_stopping.load(std::memory_order_acquire) &&
                           !g_failed.load(std::memory_order_acquire);
    const uint64_t rem_ms = (active_ok && now != 0 && now < rocket_deadline)
                                ? std::min<uint64_t>(PGR_CBH_MAX_LEASE_MS, rocket_deadline - now)
                                : 0;

    bool scope_active = false;
    bool combat_failed = false;
    if ((caps & PGR_CBH_CAP_ROCKET_COMBAT) && g_rocket_combat) {
        pgr_rocket_combat_sample_v1 rs{};
        if (g_rocket_combat->sample(&rs) == PGR_RC_OK) {
            scope_active  = (rs.scope_active != 0);
            combat_failed = (rs.failed != 0);
        } else {
            combat_failed = true;
        }
    }

    const bool mini_allowed = active_ok && !combat_failed && scope_active &&
                              ((rocket_mask & 2u) != 0) && (rem_ms > 0);
    g_rocket_mini->update(mini_allowed ? 1 : 0,
                          mini_allowed ? static_cast<int64_t>(rem_ms) : 0);
    g_mini_active.store(mini_allowed ? 1u : 0u, std::memory_order_release);
}

// --- Dobby Hook Replacements ------------------------------------------------------

// Hold every managed original argument before observer allocation/invocation.
// IL2CPP pinned handles keep these receiver/reference arguments stable across GC;
// get() still refreshes handles after managed work. MethodInfo/primitives stay raw.
struct PinnedArgs {
    uintptr_t handles[3]{};void *borrowed[3]{};size_t count=0;bool valid=true;
    PinnedArgs(std::initializer_list<void*> args){for(void *p:args){const size_t i=count++;borrowed[i]=p;
        if(p){handles[i]=reinterpret_cast<uintptr_t(*)(void*,int)>(g_cfg.gchandle_new)(p,1);if(!handles[i])valid=false;}}}
    ~PinnedArgs(){for(size_t i=count;i>0;--i)if(handles[i-1])reinterpret_cast<void(*)(uintptr_t)>(g_cfg.gchandle_free)(handles[i-1]);}
    void *get(size_t i)const{return handles[i]?reinterpret_cast<void*(*)(uintptr_t)>(g_cfg.gchandle_get_target)(handles[i]):borrowed[i];}
};
bool rocket_gate(uint64_t *tid){return (g_capabilities.load()&PGR_CBH_CAP_ROCKET_COMBAT)&&g_rocket_combat&&observation_allowed(tid);}
bool battle_gate(uint64_t *tid){return (g_capabilities.load()&PGR_CBH_CAP_BATTLES)&&g_battles&&observation_allowed(tid);}
void hook_rk_enter(void *self,void *a1, void *a2, void *method){
 auto orig=reinterpret_cast<Fn4Ptr>(load_original(HK_RK_ENTER));FrameGuard frame;uint64_t tid=0;
 if(!rocket_gate(&tid)){orig(self,a1,a2,method);return;}PinnedArgs pins{self,a1,a2};
 if(pins.valid&&rocket_gate(&tid)){g_rocket_combat->observe(PGR_RC_OBS_SCOPE_ENTER,pins.get(0),0,tid);
  if(g_rocket_mini)g_rocket_mini->restore();sync_minigame_policy(now_ms());
  drain_leaf_events(tid);}orig(pins.get(0),pins.get(1),pins.get(2),method);
}
void hook_rk_exit(void *self,void *method){
 auto orig=reinterpret_cast<Fn2Ptr>(load_original(HK_RK_EXIT));FrameGuard frame;uint64_t tid=0;
 if(!rocket_gate(&tid)){orig(self,method);return;}PinnedArgs pins{self};
 if(pins.valid&&rocket_gate(&tid)){g_rocket_combat->observe(PGR_RC_OBS_SCOPE_EXIT,pins.get(0),0,tid);
  pgr_rocket_combat_sample_v1 sample{};if(g_rocket_mini&&g_rocket_combat->sample(&sample)==0&&!sample.scope_active){g_rocket_mini->restore();g_mini_active.store(0);}
  drain_leaf_events(tid);}orig(pins.get(0),method);
}
void hook_rk_ui_update(void *self,uint8_t a1, void *method){
 auto orig=reinterpret_cast<FnPtrBoolPtr>(load_original(HK_RK_UI_UPDATE));FrameGuard frame;uint64_t tid=0;
 if(!rocket_gate(&tid)){orig(self,a1,method);return;}PinnedArgs pins{self};
 if(pins.valid&&rocket_gate(&tid)){g_rocket_combat->observe(PGR_RC_OBS_UI_UPDATE,pins.get(0),0,tid);
  drain_leaf_events(tid);}orig(pins.get(0),a1,method);
}
void hook_rk_ui_close(void *self,void *method){
 auto orig=reinterpret_cast<Fn2Ptr>(load_original(HK_RK_UI_CLOSE));FrameGuard frame;uint64_t tid=0;
 if(!rocket_gate(&tid)){orig(self,method);return;}PinnedArgs pins{self};
 if(pins.valid&&rocket_gate(&tid)){g_rocket_combat->observe(PGR_RC_OBS_UI_CLOSE,pins.get(0),0,tid);
  drain_leaf_events(tid);}orig(pins.get(0),method);
}
void hook_rk_vn_update(void *self,void *a1, void *method){
 auto orig=reinterpret_cast<Fn3Ptr>(load_original(HK_RK_VN_UPDATE));FrameGuard frame;uint64_t tid=0;
 if(!rocket_gate(&tid)){orig(self,a1,method);return;}PinnedArgs pins{self,a1};
 if(pins.valid&&rocket_gate(&tid)){g_rocket_combat->observe(PGR_RC_OBS_VNEXT_UPDATE,pins.get(0),0,tid);
  drain_leaf_events(tid);}orig(pins.get(0),pins.get(1),method);
}
void hook_rk_vn_close(void *self,void *method){
 auto orig=reinterpret_cast<Fn2Ptr>(load_original(HK_RK_VN_CLOSE));FrameGuard frame;uint64_t tid=0;
 if(!rocket_gate(&tid)){orig(self,method);return;}PinnedArgs pins{self};
 if(pins.valid&&rocket_gate(&tid)){g_rocket_combat->observe(PGR_RC_OBS_VNEXT_CLOSE,pins.get(0),0,tid);
  drain_leaf_events(tid);}orig(pins.get(0),method);
}
void hook_rk_prompt(void *self,uint8_t show, void *method){
 auto orig=reinterpret_cast<FnPtrBoolPtr>(load_original(HK_RK_PROMPT));FrameGuard frame;uint64_t tid=0;
 if(!rocket_gate(&tid)){orig(self,show,method);return;}PinnedArgs pins{self};
 if(pins.valid&&rocket_gate(&tid)){g_rocket_combat->observe(PGR_RC_OBS_SHIELD_PROMPT,pins.get(0),show?1:0,tid);
  drain_leaf_events(tid);}orig(pins.get(0),show,method);
}
void hook_rk_shield(void *self,void *method){
 auto orig=reinterpret_cast<Fn2Ptr>(load_original(HK_RK_SHIELD));FrameGuard frame;uint64_t tid=0;
 if(!rocket_gate(&tid)){orig(self,method);return;}PinnedArgs pins{self};
 if(pins.valid&&rocket_gate(&tid)){g_rocket_combat->observe(PGR_RC_OBS_SHIELD_MANUAL,pins.get(0),0,tid);
  drain_leaf_events(tid);}orig(pins.get(0),method);
}
void hook_rk_shield_close(void *self,void *method){
 auto orig=reinterpret_cast<Fn2Ptr>(load_original(HK_RK_SHIELD_CLOSE));FrameGuard frame;uint64_t tid=0;
 if(!rocket_gate(&tid)){orig(self,method);return;}PinnedArgs pins{self};
 if(pins.valid&&rocket_gate(&tid)){g_rocket_combat->observe(PGR_RC_OBS_SHIELD_CLOSE,pins.get(0),0,tid);
  drain_leaf_events(tid);}orig(pins.get(0),method);
}
void hook_rk_ui_init(void *self,void *data,void *method){
 auto orig=reinterpret_cast<Fn3Ptr>(load_original(HK_RK_UI_INIT));FrameGuard frame;uint64_t tid=0;
 if(!rocket_gate(&tid)){orig(self,data,method);return;}PinnedArgs pins{self,data};uint64_t token=0;
 if(pins.valid&&rocket_gate(&tid))token=g_rocket_combat->begin_ui(pins.get(0),tid);
 try{orig(pins.get(0),pins.get(1),method);}catch(...){
  if(token){g_rocket_combat->observe(PGR_RC_OBS_UI_CLOSE,pins.get(0),0,tid);g_rocket_combat->end_ui(token,pins.get(0),tid);}throw;
 }
 if(token){g_rocket_combat->end_ui(token,pins.get(0),tid);drain_leaf_events(tid);}
}
void hook_shared_vn_observe(void *self,void *battle_state,void *method){
 auto orig=reinterpret_cast<Fn3Ptr>(load_original(HK_SHARED_VN_OBSERVE));FrameGuard frame;uint64_t tid=0;
 if(!observation_allowed(&tid)){orig(self,battle_state,method);return;}PinnedArgs pins{self,battle_state};
 if(pins.valid&&observation_allowed(&tid)){
  if(rocket_gate(&tid))g_rocket_combat->observe(PGR_RC_OBS_CHARGE,pins.get(0),0,tid);
  if(battle_gate(&tid))g_battles->observe(PGR_BATTLE_OBS_CHARGE,pins.get(0),0,tid);drain_leaf_events(tid);
 }orig(pins.get(0),pins.get(1),method);
}
template<HookSlot SLOT,uint32_t KIND,int DESC> void hook_bt_2arg(void *self,void *method){
 auto orig=reinterpret_cast<Fn2Ptr>(load_original(SLOT));FrameGuard frame;uint64_t tid=0;
 if(!battle_gate(&tid)){orig(self,method);return;}PinnedArgs pins{self};
 if(pins.valid&&battle_gate(&tid)){g_battles->observe(KIND,pins.get(0),DESC,tid);drain_leaf_events(tid);}orig(pins.get(0),method);
}
template<HookSlot SLOT,uint32_t KIND,int DESC> void hook_bt_3arg(void *self,void *a1,void *method){
 auto orig=reinterpret_cast<Fn3Ptr>(load_original(SLOT));FrameGuard frame;uint64_t tid=0;
 if(!battle_gate(&tid)){orig(self,a1,method);return;}PinnedArgs pins{self,a1};
 if(pins.valid&&battle_gate(&tid)){g_battles->observe(KIND,pins.get(0),DESC,tid);drain_leaf_events(tid);}orig(pins.get(0),pins.get(1),method);
}
template<HookSlot SLOT,uint32_t KIND,int DESC> void hook_bt_4arg(void *self,void *a1,void *a2,void *method){
 auto orig=reinterpret_cast<Fn4Ptr>(load_original(SLOT));FrameGuard frame;uint64_t tid=0;
 if(!battle_gate(&tid)){orig(self,a1,a2,method);return;}PinnedArgs pins{self,a1,a2};
 if(pins.valid&&battle_gate(&tid)){g_battles->observe(KIND,pins.get(0),DESC,tid);drain_leaf_events(tid);}orig(pins.get(0),pins.get(1),pins.get(2),method);
}
void hook_bt_leg_init(void *self,void *p1,void *p2,PgrVec3Val pos,void *method){
 auto orig=reinterpret_cast<FnLegInitPtr>(load_original(HK_BT_LEG_INIT));FrameGuard frame;uint64_t tid=0;
 if(!battle_gate(&tid)){orig(self,p1,p2,pos,method);return;}PinnedArgs pins{self,p1,p2};
 if(pins.valid&&battle_gate(&tid)){g_battles->observe(PGR_BATTLE_OBS_CHARGE,pins.get(0),1,tid);drain_leaf_events(tid);}orig(pins.get(0),pins.get(1),pins.get(2),pos,method);
}
void hook_bt_max_warn(void *self,float seconds,void *method){
 auto orig=reinterpret_cast<FnPtrF32Ptr>(load_original(HK_BT_MAX_WARN));FrameGuard frame;uint64_t tid=0;
 if(!battle_gate(&tid)){orig(self,seconds,method);return;}PinnedArgs pins{self};
 if(pins.valid&&battle_gate(&tid)){g_battles->observe(PGR_BATTLE_OBS_WARNING,pins.get(0),PGR_BATTLE_MODE_MAX,tid);drain_leaf_events(tid);}orig(pins.get(0),seconds,method);
}
uint8_t hook_bt_gym_warn(void *self,void *method){
 auto orig=reinterpret_cast<FnRetBoolPtr>(load_original(HK_BT_GYM_WARN));FrameGuard frame;uint64_t tid=0;
 if(!battle_gate(&tid))return orig(self,method);PinnedArgs pins{self};uint64_t token=0;
 if(pins.valid&&battle_gate(&tid))token=g_battles->gym_begin(pins.get(0),tid);
 uint8_t result=0;try{result=orig(pins.get(0),method);}catch(...){if(token)g_battles->gym_end(token,0,tid);throw;}
 if(token){g_battles->gym_end(token,result==0?1:0,tid);drain_leaf_events(tid);}return result;
}
void hook_bt_leg_fast(void *self,void *sender,void *args,void *method){
 auto orig=reinterpret_cast<Fn4Ptr>(load_original(HK_BT_LEG_FAST));FrameGuard frame;uint64_t tid=0;
 if(battle_gate(&tid)){pgr_battle_sample_v1 sample{};if(g_battles->sample(&sample)==0&&sample.scope_active&&sample.mode==PGR_BATTLE_MODE_GYM)emit_gym_cooldown(tid);}
 orig(self,sender,args,method); // telemetry is callback-free; no managed work before original
}
void hook_bt_leg_charge(void *self,void *method){
 auto orig=reinterpret_cast<Fn2Ptr>(load_original(HK_BT_LEG_CHARGE));FrameGuard frame;uint64_t tid=0;
 if(battle_gate(&tid)){pgr_battle_sample_v1 sample{};if(g_battles->sample(&sample)==0&&sample.scope_active&&sample.mode==PGR_BATTLE_MODE_GYM)emit_gym_cooldown(tid);}orig(self,method);
}

// --- Hook Table Installation ------------------------------------------------------

struct HookEntry {
    HookSlot slot;
    void *target;
    void *replacement;
};

bool install_hook_group(const HookEntry *entries, size_t count) {
    // Reject null targets or duplicate code addresses (including against already-installed slots).
    for (size_t i = 0; i < count; ++i) {
        if (!entries[i].target || !entries[i].replacement) return false;
        for (size_t j = 0; j < i; ++j) {
            if (entries[i].target == entries[j].target) return false;
        }
        for (unsigned k = 0; k < HK_COUNT; ++k) {
            if (k != entries[i].slot && g_hook_targets[k] != nullptr &&
                g_hook_targets[k] == entries[i].target) {
                return false;
            }
        }
    }

    for (size_t i=0;i<count;++i) {
        const HookEntry &e=entries[i];
        if(g_originals[e.slot].load(std::memory_order_acquire))continue;
        void *orig=nullptr;const int rc=pgrazor_hook(e.target,e.replacement,&orig);
        // A nonnull trampoline is process-lifetime even when the engine reports
        // failure. Never remove a published target or clear another thread's original.
        if(orig){g_originals[e.slot].store(orig,std::memory_order_release);
            g_hook_targets[e.slot]=e.target;g_installed_hooks.fetch_add(1);}
        if(rc!=0||!orig)return false;
    }
    return true;
}

bool install_shared_vnext_hook() {
    if (g_originals[HK_SHARED_VN_OBSERVE].load(std::memory_order_acquire) != nullptr) {
        return true;
    }
    HookEntry entry{
        HK_SHARED_VN_OBSERVE,
        md_h(H_RK_VN_OBSERVE_CODE),
        reinterpret_cast<void *>(hook_shared_vn_observe)
    };
    if (!install_hook_group(&entry, 1)) return false;
    g_capabilities.fetch_or(PGR_CBH_CAP_SHARED_VNEXT_OBSERVE, std::memory_order_release);
    return true;
}

bool configure_and_install_rocket(uint64_t tid) {
    if (g_md.optional[OPT_ROCKET] != PGR_SHARED_OPT_AVAILABLE) return false;

    g_rocket_actions = api_table<pgr_rocket_actions_api_v1>(pgrazor_rocket_actions_api(1));
    if (g_rocket_actions && g_rocket_actions->version == 1 &&
        g_rocket_actions->size == sizeof(pgr_rocket_actions_api_v1)) {
        g_capabilities.fetch_or(PGR_CBH_CAP_ROCKET_ACTIONS, std::memory_order_release);
    } else {
        return false;
    }

    g_rocket_combat = api_table<pgr_rocket_combat_api_v1>(pgrazor_rocket_combat_api(1));
    if (!g_rocket_combat || g_rocket_combat->version != 1 ||
        g_rocket_combat->size != sizeof(pgr_rocket_combat_api_v1)) {
        return false;
    }

    pgr_rocket_combat_config_v1 rc_cfg{};
    rc_cfg.version          = 1;
    rc_cfg.size             = sizeof(rc_cfg);
    rc_cfg.gchandle_new     = reinterpret_cast<void *>(g_cfg.gchandle_new);
    rc_cfg.gchandle_get_target = reinterpret_cast<void *>(g_cfg.gchandle_get_target);
    rc_cfg.gchandle_free    = reinterpret_cast<void *>(g_cfg.gchandle_free);
    rc_cfg.object_get_class = reinterpret_cast<void *>(g_cfg.object_get_class);
    rc_cfg.object_unbox     = reinterpret_cast<void *>(g_cfg.object_unbox);
    rc_cfg.runtime_invoke   = reinterpret_cast<void *>(g_cfg.runtime_invoke);
    rc_cfg.field_get_value  = reinterpret_cast<void *>(g_cfg.field_get_value);

    rc_cfg.m_director       = md_h(H_RK_DIRECTOR_M);
    rc_cfg.m_active         = md_h(H_RK_ACTIVE_M);
    rc_cfg.m_fast           = md_h(H_RK_FAST_M);
    rc_cfg.m_charge         = md_h(H_RK_CHARGE_M);
    rc_cfg.m_visible        = md_h(H_RK_VISIBLE_M);
    rc_cfg.m_interactable   = md_h(H_RK_INTERACTABLE_M);
    rc_cfg.m_shield         = md_h(H_RK_SHIELD_M);
    rc_cfg.vm_blocked       = md_h(H_RK_VN_BLOCKED_M);
    rc_cfg.vm_charged       = md_h(H_RK_VN_CHARGED_M);
    rc_cfg.vm_visible       = md_h(H_RK_VN_VISIBLE_M);
    rc_cfg.vm_charge        = md_h(H_RK_VN_CHARGE_M);
    rc_cfg.vm_fast          = md_h(H_RK_VN_FAST_M);
    rc_cfg.m_alive          = md_m(5);

    rc_cfg.f_first          = md_h(H_RK_FIRST);
    rc_cfg.f_second         = md_h(H_RK_SECOND);
    rc_cfg.f_charged        = md_h(H_RK_CHARGED);
    rc_cfg.f_input          = md_h(H_RK_INPUT);
    rc_cfg.f_shields        = md_h(H_RK_SHIELDS);
    rc_cfg.f_button         = md_h(H_RK_BUTTON);
    rc_cfg.vf_gesture       = md_h(H_RK_VN_GESTURE);
    rc_cfg.vf_enabled       = md_h(H_RK_VN_ENABLED);
    rc_cfg.vf_tap           = md_h(H_RK_VN_TAP);
    rc_cfg.vf_button        = md_h(H_RK_VN_BUTTON);
    rc_cfg.f_current_state  = md_f(1);

    rc_cfg.class_rocket     = md_k(OC_ROCKET);
    rc_cfg.class_rui        = md_k(OC_RUI);
    rc_cfg.class_rdir       = md_k(OC_RDIR);
    rc_cfg.class_rcharge    = md_k(OC_RCHARGE);
    rc_cfg.class_rshield    = md_k(OC_RSHIELD);
    rc_cfg.class_rvui       = md_k(OC_RVUI);
    rc_cfg.class_rvgest     = md_k(OC_RVGEST);
    rc_cfg.class_rvcharge   = md_k(OC_RVCHARGE);
    rc_cfg.class_bool       = md_c(6);
    rc_cfg.state_open       = PGR_CBH_STATE_OPEN;

    if (g_rocket_combat->install(&rc_cfg) != PGR_RC_OK) {
        return false;
    }

    const HookEntry rk_entries[10] = {
        {HK_RK_ENTER,        md_h(H_RK_ENTER_CODE),        reinterpret_cast<void *>(hook_rk_enter)},
        {HK_RK_EXIT,         md_h(H_RK_EXIT_CODE),         reinterpret_cast<void *>(hook_rk_exit)},
        {HK_RK_UI_INIT,      md_h(H_RK_UI_CODE),           reinterpret_cast<void *>(hook_rk_ui_init)},
        {HK_RK_UI_UPDATE,    md_h(H_RK_UPDATE_CODE),       reinterpret_cast<void *>(hook_rk_ui_update)},
        {HK_RK_UI_CLOSE,     md_h(H_RK_CLOSE_CODE),        reinterpret_cast<void *>(hook_rk_ui_close)},
        {HK_RK_VN_UPDATE,    md_h(H_RK_VN_UPDATE_CODE),    reinterpret_cast<void *>(hook_rk_vn_update)},
        {HK_RK_VN_CLOSE,     md_h(H_RK_VN_CLOSE_CODE),     reinterpret_cast<void *>(hook_rk_vn_close)},
        {HK_RK_PROMPT,       md_h(H_RK_PROMPT_CODE),       reinterpret_cast<void *>(hook_rk_prompt)},
        {HK_RK_SHIELD,       md_h(H_RK_SHIELD_CODE),       reinterpret_cast<void *>(hook_rk_shield)},
        {HK_RK_SHIELD_CLOSE, md_h(H_RK_SHIELD_CLOSE_CODE), reinterpret_cast<void *>(hook_rk_shield_close)},
    };
    if (!install_hook_group(rk_entries, 10)) {
        return false;
    }
    if (!install_shared_vnext_hook()) {
        return false;
    }

    // Configure & install Feature 6 (Rocket Particle Minigame) if available.
    g_rocket_mini = api_table<PgrRocketMiniApi>(pgrazor_rocket_minigame_api(1));
    if (g_rocket_mini && g_rocket_mini->version == 1 &&
        g_rocket_mini->size == sizeof(PgrRocketMiniApi)) {
        // Ensure none of leaf 6's 4 minigame hook addresses collide with host hooks.
        void *mini_codes[4] = {
            md_h(H_RK_MN_UPDATE_CODE),
            md_h(H_RK_MN_EXIT_CODE),
            md_h(H_RK_MN_HIDE_CODE),
            md_h(H_RK_MN_DESTROY_CODE)
        };
        bool mini_collision = false;
        for (int i = 0; i < 4; ++i) {
            if (!mini_codes[i]) mini_collision = true;
            for (int j = 0; j < i; ++j) {
                if (mini_codes[i] == mini_codes[j]) mini_collision = true;
            }
            for (unsigned k = 0; k < HK_COUNT; ++k) {
                if (g_hook_targets[k] != nullptr && g_hook_targets[k] == mini_codes[i]) {
                    mini_collision = true;
                }
            }
        }
        if (!mini_collision) {
            PgrRocketMiniConfig mn_cfg{};
            mn_cfg.version           = 1;
            mn_cfg.size              = sizeof(mn_cfg);
            mn_cfg.update_addr       = md_h(H_RK_MN_UPDATE_CODE);
            mn_cfg.exit_addr         = md_h(H_RK_MN_EXIT_CODE);
            mn_cfg.hide_addr         = md_h(H_RK_MN_HIDE_CODE);
            mn_cfg.destroy_addr      = md_h(H_RK_MN_DESTROY_CODE);
            mn_cfg.transform_method  = md_h(H_RK_MN_TRANSFORM_M);
            mn_cfg.gameObject_method = md_h(H_RK_MN_GAMEOBJ_M);
            mn_cfg.scale_method      = md_h(H_RK_MN_SCALE_M);
            mn_cfg.active_method     = md_h(H_RK_MN_ACTIVE_M);
            mn_cfg.setScale_method   = md_h(H_RK_MN_SETSCALE_M);
            mn_cfg.setActive_method  = md_h(H_RK_MN_SETACTIVE_M);
            mn_cfg.alive_method      = md_m(5);
            mn_cfg.collider_field    = md_h(H_RK_MN_COLLIDER);
            mn_cfg.running_field     = md_h(H_RK_MN_RUNNING);
            mn_cfg.mini_class        = md_k(OC_RMINI);
            mn_cfg.bool_class        = md_c(6);
            mn_cfg.root              = reinterpret_cast<void *(*)(void *, int)>(g_cfg.gchandle_new);
            mn_cfg.free_root         = reinterpret_cast<void (*)(void *)>(g_cfg.gchandle_free);
            mn_cfg.target            = reinterpret_cast<void *(*)(void *)>(g_cfg.gchandle_get_target);
            mn_cfg.object_class      = reinterpret_cast<void *(*)(void *)>(g_cfg.object_get_class);
            mn_cfg.object_unbox      = reinterpret_cast<void *(*)(void *)>(g_cfg.object_unbox);
            mn_cfg.runtime_invoke    = reinterpret_cast<void *(*)(void *, void *, void **, void **)>(g_cfg.runtime_invoke);
            mn_cfg.field_get_value   = reinterpret_cast<void (*)(void *, void *, void *)>(g_cfg.field_get_value);

            if (g_rocket_mini->install(&mn_cfg) == PGR_RMINI_OK) {
                g_capabilities.fetch_or(PGR_CBH_CAP_ROCKET_MINIGAME, std::memory_order_release);
                g_installed_hooks.fetch_add(4, std::memory_order_relaxed);
                queue_event(PGR_CBH_EV_ROCKET_MINIGAME_READY, 1, 0, tid, "{\"available\":true}");
            } else {
                queue_event(PGR_CBH_EV_ROCKET_MINIGAME_READY, 0, 0, tid,
                            "{\"available\":false,\"reason\":\"install-failed\"}");
            }
        }
    }

    g_capabilities.fetch_or(PGR_CBH_CAP_ROCKET_COMBAT, std::memory_order_release);
    transport_ready_action(46, true, "Ready");
    return true;
}

bool configure_and_install_battles() {
    if (g_md.optional[OPT_BATTLES] != PGR_SHARED_OPT_AVAILABLE) return false;

    g_battles = api_table<pgr_battle_api_v1>(pgrazor_battles_api(1));
    if (!g_battles || g_battles->version != 1 ||
        g_battles->size != sizeof(pgr_battle_api_v1)) {
        return false;
    }

    pgr_battle_config_v1 bt_cfg{};
    bt_cfg.version             = 1;
    bt_cfg.size                = sizeof(bt_cfg);
    bt_cfg.gchandle_new        = reinterpret_cast<void *>(g_cfg.gchandle_new);
    bt_cfg.gchandle_get_target = reinterpret_cast<void *>(g_cfg.gchandle_get_target);
    bt_cfg.gchandle_free       = reinterpret_cast<void *>(g_cfg.gchandle_free);
    bt_cfg.object_get_class    = reinterpret_cast<void *>(g_cfg.object_get_class);
    bt_cfg.object_unbox        = reinterpret_cast<void *>(g_cfg.object_unbox);
    bt_cfg.runtime_invoke      = reinterpret_cast<void *>(g_cfg.runtime_invoke);
    bt_cfg.field_get_value     = reinterpret_cast<void *>(g_cfg.field_get_value);

    bt_cfg.m_current_state     = md_h(H_BT_M_CUR_STATE);
    bt_cfg.m_active            = md_h(H_BT_M_ACTIVE);
    bt_cfg.m_press             = md_h(H_BT_M_PRESS);
    bt_cfg.m_interactable      = md_h(H_RK_INTERACTABLE_M);
    bt_cfg.m_alive             = md_m(5);
    bt_cfg.m_dodge             = md_h(H_BT_M_DODGE);
    bt_cfg.m_legacy_dodge      = md_h(H_BT_M_LEG_DODGE);
    bt_cfg.m_raid_fast         = md_h(H_BT_M_RAID_FAST_ACT);
    bt_cfg.m_max_fast          = md_h(H_BT_M_MAX_FAST);
    bt_cfg.m_legacy_fast       = md_h(H_BT_M_LEG_FAST);
    bt_cfg.m_legacy_charge     = md_h(H_BT_M_LEG_CHARGE);
    bt_cfg.m_legacy_charged    = md_h(H_BT_M_LEG_CHARGED);
    bt_cfg.m_vnext_charge      = md_h(H_RK_VN_CHARGE_M);
    bt_cfg.m_vnext_charged     = md_h(H_RK_VN_CHARGED_M);
    bt_cfg.m_vnext_visible     = md_h(H_RK_VN_VISIBLE_M);

    bt_cfg.f_director          = md_h(H_BT_F_DIRECTOR);
    bt_cfg.f_battle_status     = md_h(H_BT_F_BATTLE_STATUS);
    bt_cfg.f_max_gesture       = md_h(H_BT_F_MAX_GESTURE);
    bt_cfg.f_legacy_flick      = md_h(H_BT_F_LEG_FLICK);
    bt_cfg.f_legacy_tap        = md_h(H_BT_F_LEG_TAP);
    bt_cfg.f_legacy_button     = md_h(H_BT_F_LEG_BUTTON);
    bt_cfg.f_max_button        = md_h(H_BT_F_MAX_BUTTON);
    bt_cfg.f_max_type          = md_h(H_BT_F_MAX_TYPE);
    bt_cfg.f_vnext_enabled     = md_h(H_RK_VN_ENABLED);
    bt_cfg.f_vnext_tap         = md_h(H_RK_VN_TAP);
    bt_cfg.f_vnext_button      = md_h(H_RK_VN_BUTTON);
    bt_cfg.f_gym_warning_state = md_h(H_BT_GYM_WARN_STATE);

    bt_cfg.class_battlestate   = md_k(OC_BATTLE_44226);
    bt_cfg.class_legacy_charge = md_k(OC_BATTLE_9308);
    bt_cfg.class_max_gesture   = md_k(OC_BATTLE_31285);
    bt_cfg.class_raid_gesture  = md_k(OC_BATTLE_31499);
    bt_cfg.class_legacy_gesture = md_k(OC_BATTLE_9182);
    bt_cfg.class_max_widget    = md_k(OC_BATTLE_31280);
    bt_cfg.class_rvcharge      = md_k(OC_RVCHARGE);
    bt_cfg.class_raid_warning  = md_k(OC_BATTLE_31623);
    bt_cfg.class_max_warning   = md_k(OC_BATTLE_31263);
    bt_cfg.class_gym_iterator  = md_h(H_BT_GYM_ITER);
    bt_cfg.class_bool          = md_c(6);

    // Scopes: 0 = Raid (31514), 1 = Gym (10591), 2 = Max (31244)
    bt_cfg.scopes[0] = {md_h(H_BT_SC0_CLASS), PGR_BATTLE_MODE_RAID, 0};
    bt_cfg.scopes[1] = {md_h(H_BT_SC1_CLASS), PGR_BATTLE_MODE_GYM,  0};
    bt_cfg.scopes[2] = {md_h(H_BT_SC2_CLASS), PGR_BATTLE_MODE_MAX,  0};

    // Swaps (matches quick-close.js:695: 0=gym 9322 mode 1, 1=raid 31639 mode 0, 2=max 31337 mode 2)
    bt_cfg.swaps[0] = {md_h(H_BT_SW0_CLASS), md_h(H_BT_SW0_SHOW_M), PGR_BATTLE_MODE_GYM,  0};
    bt_cfg.swaps[1] = {md_h(H_BT_SW1_CLASS), md_h(H_BT_SW1_SHOW_M), PGR_BATTLE_MODE_RAID, 0};
    bt_cfg.swaps[2] = {md_h(H_BT_SW2_CLASS), md_h(H_BT_SW2_SHOW_M), PGR_BATTLE_MODE_MAX,  0};

    bt_cfg.status_active = g_md.opt_ints[I_BAT_STATUS_ACTIVE];
    bt_cfg.status_unset  = g_md.opt_ints[I_BAT_STATUS_UNSET];
    bt_cfg.dodge_left    = g_md.opt_ints[I_BAT_DODGE_LEFT];
    bt_cfg.max_attack    = g_md.opt_ints[I_BAT_MAX_ATTACK];
    bt_cfg.reserved0     = 0;
    bt_cfg.reserved1     = 0;

    if (g_battles->install(&bt_cfg) != PGR_BATTLE_OK) {
        return false;
    }

    const HookEntry bt_entries[30] = {
        // Scopes (6)
        {HK_BT_SC0_ENTER, md_h(H_BT_SC0_ENTER_CODE),
         reinterpret_cast<void *>(hook_bt_4arg<HK_BT_SC0_ENTER, PGR_BATTLE_OBS_SCOPE_ENTER, 0>)},
        {HK_BT_SC0_EXIT,  md_h(H_BT_SC0_EXIT_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_SC0_EXIT,  PGR_BATTLE_OBS_SCOPE_EXIT,  0>)},
        {HK_BT_SC1_ENTER, md_h(H_BT_SC1_ENTER_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_SC1_ENTER, PGR_BATTLE_OBS_SCOPE_ENTER, 1>)},
        {HK_BT_SC1_EXIT,  md_h(H_BT_SC1_EXIT_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_SC1_EXIT,  PGR_BATTLE_OBS_SCOPE_EXIT,  1>)},
        {HK_BT_SC2_ENTER, md_h(H_BT_SC2_ENTER_CODE),
         reinterpret_cast<void *>(hook_bt_4arg<HK_BT_SC2_ENTER, PGR_BATTLE_OBS_SCOPE_ENTER, 2>)},
        {HK_BT_SC2_EXIT,  md_h(H_BT_SC2_EXIT_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_SC2_EXIT,  PGR_BATTLE_OBS_SCOPE_EXIT,  2>)},

        // Swaps (9)
        {HK_BT_SW0_OPEN, md_h(H_BT_SW0_OPEN_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_SW0_OPEN, PGR_BATTLE_OBS_SWAP_OPEN,  0>)},
        {HK_BT_SW0_CL0,  md_h(H_BT_SW0_CL0_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_SW0_CL0,  PGR_BATTLE_OBS_SWAP_CLOSE, 0>)},
        {HK_BT_SW0_CL1,  md_h(H_BT_SW0_CL1_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_SW0_CL1,  PGR_BATTLE_OBS_SWAP_CLOSE, 0>)},
        {HK_BT_SW1_OPEN, md_h(H_BT_SW1_OPEN_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_SW1_OPEN, PGR_BATTLE_OBS_SWAP_OPEN,  1>)},
        {HK_BT_SW1_CL0,  md_h(H_BT_SW1_CL0_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_SW1_CL0,  PGR_BATTLE_OBS_SWAP_CLOSE, 1>)},
        {HK_BT_SW1_CL1,  md_h(H_BT_SW1_CL1_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_SW1_CL1,  PGR_BATTLE_OBS_SWAP_CLOSE, 1>)},
        {HK_BT_SW2_OPEN, md_h(H_BT_SW2_OPEN_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_SW2_OPEN, PGR_BATTLE_OBS_SWAP_OPEN,  2>)},
        {HK_BT_SW2_CL0,  md_h(H_BT_SW2_CL0_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_SW2_CL0,  PGR_BATTLE_OBS_SWAP_CLOSE, 2>)},
        {HK_BT_SW2_CL1,  md_h(H_BT_SW2_CL1_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_SW2_CL1,  PGR_BATTLE_OBS_SWAP_CLOSE, 2>)},

        // Gestures (7)
        {HK_BT_ENABLE,      md_h(H_BT_M_ENABLE_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_ENABLE,      PGR_BATTLE_OBS_GESTURE,      0>)},
        {HK_BT_BUTTONS,     md_h(H_BT_M_BUTTONS_CODE),
         reinterpret_cast<void *>(hook_bt_3arg<HK_BT_BUTTONS,     PGR_BATTLE_OBS_GESTURE,      0>)},
        {HK_BT_LEG_ENABLE,  md_h(H_BT_M_LEG_ENABLE_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_LEG_ENABLE,  PGR_BATTLE_OBS_GESTURE,      1>)},
        {HK_BT_LEG_BUTTONS, md_h(H_BT_M_LEG_BUTTONS_CODE),
         reinterpret_cast<void *>(hook_bt_3arg<HK_BT_LEG_BUTTONS, PGR_BATTLE_OBS_GESTURE,      1>)},
        {HK_BT_DISABLE,     md_h(H_BT_M_DISABLE_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_DISABLE,     PGR_BATTLE_OBS_GESTURE_DROP, 0>)},
        {HK_BT_LEG_DISABLE, md_h(H_BT_M_LEG_DISABLE_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_LEG_DISABLE, PGR_BATTLE_OBS_GESTURE_DROP, 0>)},
        {HK_BT_DESTROY,     md_h(H_BT_M_DESTROY_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_DESTROY,     PGR_BATTLE_OBS_GESTURE_DROP, 0>)},

        // Charges & Max Widgets (3)
        {HK_BT_LEG_INIT,    md_h(H_BT_M_LEG_INIT_CODE),    reinterpret_cast<void *>(hook_bt_leg_init)},
        {HK_BT_MAX_REFRESH, md_h(H_BT_M_MAX_REFRESH_CODE),
         reinterpret_cast<void *>(hook_bt_3arg<HK_BT_MAX_REFRESH, PGR_BATTLE_OBS_MAX_REFRESH, 0>)},
        {HK_BT_MAX_CLEANUP, md_h(H_BT_M_MAX_CLEANUP_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_MAX_CLEANUP, PGR_BATTLE_OBS_MAX_CLEANUP, 0>)},

        // Warnings (3)
        {HK_BT_RAID_WARN, md_h(H_BT_M_RAID_WARN_CODE),
         reinterpret_cast<void *>(hook_bt_2arg<HK_BT_RAID_WARN, PGR_BATTLE_OBS_WARNING, PGR_BATTLE_MODE_RAID>)},
        {HK_BT_MAX_WARN,  md_h(H_BT_M_MAX_WARN_CODE),  reinterpret_cast<void *>(hook_bt_max_warn)},
        {HK_BT_GYM_WARN,  md_h(H_BT_M_GYM_WARN_CODE),  reinterpret_cast<void *>(hook_bt_gym_warn)},

        // Gym Cooldown Actions (2)
        {HK_BT_LEG_FAST,   md_h(H_BT_M_LEG_FAST_CODE),   reinterpret_cast<void *>(hook_bt_leg_fast)},
        {HK_BT_LEG_CHARGE, md_h(H_BT_M_LEG_CHARGE_CODE), reinterpret_cast<void *>(hook_bt_leg_charge)},
    };
    if (!install_hook_group(bt_entries, 30)) {
        return false;
    }
    if (!install_shared_vnext_hook()) {
        return false;
    }

    g_capabilities.fetch_or(PGR_CBH_CAP_BATTLES, std::memory_order_release);
    transport_ready_action(25, true, "Ready");
    return true;
}

// --- Metadata & Export Resolution -------------------------------------------------

bool exports_complete(const PgrCombatHostConfig &c) {
    return c.thread_current != 0 &&
           c.gchandle_new != 0 &&
           c.gchandle_get_target != 0 &&
           c.gchandle_free != 0 &&
           c.object_get_class != 0 &&
           c.object_unbox != 0 &&
           c.runtime_invoke != 0 &&
           c.field_get_value != 0;
}

void fill_exports_from_bootstrap(PgrCombatHostConfig &c) {
    const auto *boot = api_table<PgrBootApi>(pgrazor_runtime_bootstrap_api(1));
    if (!boot || boot->version != 1 || boot->size < sizeof(PgrBootApi) || !boot->snapshot) return;
    PgrBootBinding b{};
    if (boot->snapshot(&b) != PGR_BOOT_OK) return;
    if (!c.thread_current)      c.thread_current      = reinterpret_cast<uintptr_t>(b.runtime.thread_current);
    if (!c.gchandle_new)        c.gchandle_new        = reinterpret_cast<uintptr_t>(b.runtime.gchandle_new);
    if (!c.gchandle_get_target) c.gchandle_get_target = reinterpret_cast<uintptr_t>(b.runtime.gchandle_get_target);
    if (!c.gchandle_free)       c.gchandle_free       = reinterpret_cast<uintptr_t>(b.runtime.gchandle_free);
    if (!c.object_get_class)    c.object_get_class    = reinterpret_cast<uintptr_t>(b.runtime.object_get_class);
    if (!c.object_unbox)        c.object_unbox        = reinterpret_cast<uintptr_t>(b.runtime.object_unbox);
    if (!c.runtime_invoke)      c.runtime_invoke      = reinterpret_cast<uintptr_t>(b.runtime.runtime_invoke);
    if (!c.field_get_value)     c.field_get_value     = reinterpret_cast<uintptr_t>(b.runtime.field_get_value);
}

int finish_setup_locked(uint64_t tid) {
    if (g_ready.load(std::memory_order_acquire)) return PGR_CBH_OK;
    if (g_failed.load(std::memory_order_acquire)) return g_last_error.load(std::memory_order_acquire);

    // 1. Resolve exports (explicit or via Feature 54 runtime_bootstrap).
    if (!exports_complete(g_cfg)) {
        fill_exports_from_bootstrap(g_cfg);
    }
    if (!exports_complete(g_cfg)) {
        if (g_default_bind_mode) return PGR_CBH_NOT_READY;
        g_last_error.store(PGR_CBH_ERR_NULL_EXPORT, std::memory_order_release);
        return PGR_CBH_ERR_NULL_EXPORT;
    }

    // 2. Resolve owner_tid if not yet set.
    uint64_t owner = g_cfg.owner_tid;
    if (owner == 0) {
        const auto *sh = api_table<PgrSharedHostApi>(pgrazor_shared_host_api(1));
        if (sh && sh->version == 1 && sh->size >= sizeof(PgrSharedHostApi) && sh->sample) {
            PgrSharedHostSample sh_s{};
            if (sh->sample(&sh_s) == 0 && sh_s.owner_tid != 0) {
                owner = sh_s.owner_tid;
            }
        }
    }
    if (owner == 0 && (g_cfg.flags & PGR_CBH_CFG_HAS_METADATA_CONFIG) != 0) {
        owner = g_cfg.metadata_config.owner_tid;
    }
    if (owner == 0 && tid != 0) {
        owner = tid;
    }
    if (owner == 0) {
        if (g_default_bind_mode) return PGR_CBH_NOT_READY;
        g_last_error.store(PGR_CBH_ERR_THREAD, std::memory_order_release);
        return PGR_CBH_ERR_THREAD;
    }
    g_cfg.owner_tid = owner;

    // 3. Resolve Feature 56 metadata (`cfg->metadata`, `shared_host(61)->metadata()`, or `metadata_config`).
    const PgrSharedMetadataOutput *md_ptr = g_cfg.metadata;
    if (!md_ptr) {
        const auto *sh = api_table<PgrSharedHostApi>(pgrazor_shared_host_api(1));
        if (sh && sh->version == 1 && sh->size >= sizeof(PgrSharedHostApi) && sh->metadata) {
            md_ptr = sh->metadata();
        }
    }
    if (md_ptr) {
        if (md_ptr->version != 2u ||
            md_ptr->size != sizeof(PgrSharedMetadataOutput)) {
            g_failed.store(true, std::memory_order_release);
            g_last_error.store(PGR_CBH_ERR_METADATA, std::memory_order_release);
            return PGR_CBH_ERR_METADATA;
        }
        g_md = *md_ptr;
    } else if ((g_cfg.flags & PGR_CBH_CFG_HAS_METADATA_CONFIG) != 0) {
        const auto *md_api = api_table<PgrSharedMetadataApi>(
            pgrazor_shared_metadata_api(2u));
        if (!md_api || md_api->version != 2u ||
            md_api->size != sizeof(PgrSharedMetadataApi) || !md_api->resolve) {
            g_failed.store(true, std::memory_order_release);
            g_last_error.store(PGR_CBH_ERR_LEAF_UNAVAILABLE, std::memory_order_release);
            return PGR_CBH_ERR_LEAF_UNAVAILABLE;
        }
        PgrSharedMetadataConfig md_cfg = g_cfg.metadata_config;
        if (md_cfg.owner_tid == 0) md_cfg.owner_tid = owner;
        int mrc = md_api->resolve(&md_cfg, &g_md);
        if (mrc == PGR_SHARED_NOT_READY) {
            return PGR_CBH_NOT_READY;
        }
        if (mrc != PGR_SHARED_OK ||
            g_md.version != 2u ||
            g_md.size != sizeof(PgrSharedMetadataOutput)) {
            g_failed.store(true, std::memory_order_release);
            g_last_error.store(PGR_CBH_ERR_METADATA, std::memory_order_release);
            return PGR_CBH_ERR_METADATA;
        }
    } else {
        if (g_default_bind_mode) return PGR_CBH_NOT_READY;
        g_last_error.store(PGR_CBH_ERR_METADATA, std::memory_order_release);
        return PGR_CBH_ERR_METADATA;
    }

    // Core metadata sanity checks (`GUI.currentState`, `UnityEngine.Object.op_Implicit`, `System.Boolean`).
    if (!md_c(0) || !md_f(1) || !md_c(4) || !md_m(5) || !md_c(6)) {
        g_failed.store(true, std::memory_order_release);
        g_last_error.store(PGR_CBH_ERR_METADATA, std::memory_order_release);
        return PGR_CBH_ERR_METADATA;
    }

    g_owner_tid.store(owner, std::memory_order_release);

    // 4. Configure & install Rocket (Features 6, 20, 21 + 11 hooks) and Battles (Feature 19 + 31 hooks).
    const bool want_rocket  = (g_md.optional[OPT_ROCKET] == PGR_SHARED_OPT_AVAILABLE);
    const bool want_battles = (g_md.optional[OPT_BATTLES] == PGR_SHARED_OPT_AVAILABLE);

    if ((g_cfg.flags & PGR_CBH_CFG_REQUIRE_ROCKET) && !want_rocket) {
        g_failed.store(true, std::memory_order_release);
        g_last_error.store(PGR_CBH_ERR_METADATA, std::memory_order_release);
        transport_ready_action(46, false, "Unavailable: rocket-metadata");
        return PGR_CBH_ERR_METADATA;
    }
    if ((g_cfg.flags & PGR_CBH_CFG_REQUIRE_BATTLES) && !want_battles) {
        g_failed.store(true, std::memory_order_release);
        g_last_error.store(PGR_CBH_ERR_METADATA, std::memory_order_release);
        transport_ready_action(25, false, "Unavailable: battle-metadata");
        return PGR_CBH_ERR_METADATA;
    }

    if (want_rocket) {
        if (!configure_and_install_rocket(owner)) {
            transport_ready_action(46, false, "Unavailable: native-rocket-install");
            if (g_cfg.flags & PGR_CBH_CFG_REQUIRE_ROCKET) {
                g_failed.store(true, std::memory_order_release);
                g_last_error.store(PGR_CBH_ERR_HOOK_INSTALL, std::memory_order_release);
                return PGR_CBH_ERR_HOOK_INSTALL;
            }
        }
    } else {
        transport_ready_action(46, false, "Unavailable: rocket-metadata");
    }

    if (want_battles) {
        if (!configure_and_install_battles()) {
            transport_ready_action(25, false, "Unavailable: native-battle-install");
            if (g_cfg.flags & PGR_CBH_CFG_REQUIRE_BATTLES) {
                g_failed.store(true, std::memory_order_release);
                g_last_error.store(PGR_CBH_ERR_HOOK_INSTALL, std::memory_order_release);
                return PGR_CBH_ERR_HOOK_INSTALL;
            }
        }
    } else {
        transport_ready_action(25, false, "Unavailable: battle-metadata");
    }

    const uint32_t caps = g_capabilities.load(std::memory_order_acquire);
    if ((want_rocket || want_battles) &&
        !(caps & (PGR_CBH_CAP_ROCKET_COMBAT | PGR_CBH_CAP_BATTLES))) {
        g_failed.store(true, std::memory_order_release);
        g_last_error.store(PGR_CBH_ERR_LEAF_INIT, std::memory_order_release);
        return PGR_CBH_ERR_LEAF_INIT;
    }

    g_accepted_cfg     = g_cfg;
    g_has_accepted_cfg = true;
    g_configured.store(true, std::memory_order_release);
    g_ready.store(true, std::memory_order_release);

    // If configure() is running directly on the proven Unity owner thread, prime leaves immediately
    // so synchronous hooks firing before the first tick() already have their Unity TID latched.
    if (self_tid() == owner && thread_attached()) {
        ensure_leaves_latched_on_owner(owner);
    }

    drain_leaf_events(owner);

    char ready_json[256];
    std::snprintf(ready_json, sizeof(ready_json),
                  "{\"available\":true,\"capabilities\":%u,\"hooks\":%u,"
                  "\"rocketCombat\":%s,\"rocketMinigame\":%s,\"battles\":%s}",
                  caps,
                  g_installed_hooks.load(std::memory_order_relaxed),
                  (caps & PGR_CBH_CAP_ROCKET_COMBAT) ? "true" : "false",
                  (caps & PGR_CBH_CAP_ROCKET_MINIGAME) ? "true" : "false",
                  (caps & PGR_CBH_CAP_BATTLES) ? "true" : "false");
    queue_event(PGR_CBH_EV_HOST_READY, static_cast<int64_t>(caps),
                static_cast<int64_t>(g_installed_hooks.load(std::memory_order_relaxed)),
                owner, ready_json);
    return PGR_CBH_OK;
}

// --- API Entrypoints --------------------------------------------------------------

int api_configure(const PgrCombatHostConfig *cfg) {
    if (g_stopping.load(std::memory_order_acquire)) return PGR_CBH_ERR_STOPPED;
    if (g_failed.load(std::memory_order_acquire)) return g_last_error.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> lk(g_setup_mutex);
    if (!cfg) {
        // Default bind mode (matches inventory_host.cpp): uses shared_host(61) + runtime_bootstrap(54) + transport(52).
        if (g_ready.load(std::memory_order_acquire)) return PGR_CBH_OK;
        g_cfg               = PgrCombatHostConfig{};
        g_cfg.version       = PGR_COMBAT_HOST_API_VERSION;
        g_cfg.size          = sizeof(PgrCombatHostConfig);
        g_cfg.flags         = PGR_CBH_CFG_USE_TRANSPORT;
        g_default_bind_mode = true;
        g_configured.store(true, std::memory_order_release);
        int rc = finish_setup_locked(0);
        return (rc == PGR_CBH_NOT_READY) ? PGR_CBH_OK : rc;
    }

    if (cfg->version != PGR_COMBAT_HOST_API_VERSION) return PGR_CBH_ERR_VERSION;
    if (cfg->size != sizeof(PgrCombatHostConfig)) return PGR_CBH_ERR_SIZE;
    if (cfg->reserved0 != 0 || cfg->reserved1 != 0) return PGR_CBH_ERR_INVALID_ARG;
    if ((cfg->flags & ~(PGR_CBH_CFG_HAS_METADATA_CONFIG | PGR_CBH_CFG_USE_TRANSPORT |
                        PGR_CBH_CFG_REQUIRE_BATTLES | PGR_CBH_CFG_REQUIRE_ROCKET)) != 0) {
        return PGR_CBH_ERR_INVALID_ARG;
    }

    if (g_ready.load(std::memory_order_acquire)) {
        if (g_has_accepted_cfg && std::memcmp(&g_accepted_cfg, cfg, offsetof(PgrCombatHostConfig, owner_tid)) == 0) {
            return PGR_CBH_OK;
        }
        return PGR_CBH_ERR_INVALID_ARG;
    }

    g_cfg               = *cfg;
    g_default_bind_mode = false;
    return finish_setup_locked(cfg->owner_tid);
}

void refresh_leaf_gates(){
 std::lock_guard<std::mutex> lock(g_options_mutex);const uint64_t now=now_ms(),owner=g_owner_tid.load();const uint32_t caps=g_capabilities.load();
 const bool active=(g_opts.flags&PGR_CBH_OPT_ACTIVE)&&!(g_opts.flags&PGR_CBH_OPT_STOPPING)&&!g_stopping.load()&&!g_failed.load()&&now;
 const uint64_t battle=active&&(g_opts.flags&PGR_CBH_OPT_BATTLES_ALLOWED)&&now<g_battle_deadline_ms?std::min<uint64_t>(2500,g_battle_deadline_ms-now):0;
 const uint64_t rocket=active&&(g_opts.flags&PGR_CBH_OPT_ROCKET_ALLOWED)&&now<g_rocket_deadline_ms?std::min<uint64_t>(2500,g_rocket_deadline_ms-now):0;
 bool rocket_active=false,rocket_failed=false;
 if((caps&PGR_CBH_CAP_ROCKET_COMBAT)&&g_rocket_combat){g_rocket_combat->update(g_opts.rocket_mask,rocket,owner);pgr_rocket_combat_sample_v1 sample{};
  if(g_rocket_combat->sample(&sample)==0){rocket_active=sample.scope_active!=0;rocket_failed=sample.failed!=0;}else rocket_failed=true;}
 if((caps&PGR_CBH_CAP_BATTLES)&&g_battles)g_battles->update(g_opts.battle_mask,battle,rocket_active?1:0,owner);
 if((caps&PGR_CBH_CAP_ROCKET_MINIGAME)&&g_rocket_mini){bool on=active&&rocket&&!rocket_failed&&rocket_active&&(g_opts.rocket_mask&2);g_rocket_mini->update(on?1:0,on?int64_t(rocket):0);g_mini_active.store(on?1:0);}
}
int api_revoke();
int api_update(const PgrCombatHostOptions *opts) {
    if (!g_configured.load(std::memory_order_acquire)) return PGR_CBH_ERR_NOT_CONFIGURED;
    if (!opts) {api_revoke();return PGR_CBH_ERR_INVALID_ARG;}
    if (opts->version != PGR_COMBAT_HOST_API_VERSION) {api_revoke();return PGR_CBH_ERR_VERSION;}
    if (opts->size != sizeof(PgrCombatHostOptions)) {api_revoke();return PGR_CBH_ERR_SIZE;}
    if (opts->reserved0 != 0||opts->battle_mask>PGR_CBH_BATTLE_MASK_ALL||opts->rocket_mask>PGR_CBH_ROCKET_MASK_ALL) {api_revoke();return PGR_CBH_ERR_INVALID_ARG;}
    if ((opts->flags & ~(PGR_CBH_OPT_ACTIVE | PGR_CBH_OPT_BATTLES_ALLOWED |
                         PGR_CBH_OPT_ROCKET_ALLOWED | PGR_CBH_OPT_STOPPING)) != 0) {
        api_revoke();return PGR_CBH_ERR_INVALID_ARG;
    }
    if (g_stopping.load(std::memory_order_acquire)) return PGR_CBH_ERR_STOPPED;

    const uint64_t now = now_ms();
    const bool active  = (opts->flags & PGR_CBH_OPT_ACTIVE) != 0 &&
                         (opts->flags & PGR_CBH_OPT_STOPPING) == 0 &&
                         now != 0;

    const uint32_t b_mask = (opts->battle_mask <= PGR_CBH_BATTLE_MASK_ALL)
                                ? opts->battle_mask
                                : (opts->battle_mask & PGR_CBH_BATTLE_MASK_ALL);
    const uint32_t r_mask = (opts->rocket_mask <= PGR_CBH_ROCKET_MASK_ALL)
                                ? opts->rocket_mask
                                : (opts->rocket_mask & PGR_CBH_ROCKET_MASK_ALL);

    const uint64_t b_lease = (active && (opts->flags & PGR_CBH_OPT_BATTLES_ALLOWED) && b_mask != 0)
                                 ? std::min<uint64_t>(PGR_CBH_MAX_LEASE_MS, opts->battle_lease_ms)
                                 : 0;
    const uint64_t r_lease = (active && (opts->flags & PGR_CBH_OPT_ROCKET_ALLOWED) && r_mask != 0)
                                 ? std::min<uint64_t>(PGR_CBH_MAX_LEASE_MS, opts->rocket_lease_ms)
                                 : 0;

    {
        std::lock_guard<std::mutex> lk(g_options_mutex);
        g_opts               = *opts;
        g_opts.battle_mask   = b_mask;
        g_opts.rocket_mask   = r_mask;
        g_battle_deadline_ms = (b_lease > 0) ? (now + b_lease) : 0;
        g_rocket_deadline_ms = (r_lease > 0) ? (now + r_lease) : 0;
        ++g_policy_epoch; // host revision; external epoch is diagnostic, not a replay proof
    }

    refresh_leaf_gates();
    return PGR_CBH_OK;
}

int api_revoke() {
    if (!g_configured.load(std::memory_order_acquire)) return PGR_CBH_ERR_NOT_CONFIGURED;
    {
        std::lock_guard<std::mutex> lk(g_options_mutex);
        g_opts.flags         = 0;
        g_opts.battle_mask   = 0;
        g_opts.rocket_mask   = 0;
        g_opts.battle_lease_ms = 0;
        g_opts.rocket_lease_ms = 0;
        g_battle_deadline_ms = 0;
        g_rocket_deadline_ms = 0;
        ++g_policy_epoch;
    }
    refresh_leaf_gates();
    // restore may invoke managed methods, so it must run after the policy lock.
    if((g_capabilities.load()&PGR_CBH_CAP_ROCKET_MINIGAME)&&g_rocket_mini)g_rocket_mini->restore();
    return PGR_CBH_OK;
}

void poll_transport_options() {
    if (!(g_cfg.flags & PGR_CBH_CFG_USE_TRANSPORT)) return;
    const auto *tr = transport_api();
    if (!tr || !tr->sample || !tr->get_bool || !tr->get_int) return;

    PgrRuntimeTransportSample a{}, z{};
    if (tr->sample(&a) != 0 || !a.snapshot_fresh) {
        api_revoke();
        return;
    }
    auto rd_bool = [&](uint32_t idx) {
        int v = 0;
        return tr->get_bool(idx, &v) == 0 && v == 1;
    };
    auto rd_int = [&](uint32_t idx) {
        int32_t v = 0;
        return tr->get_int(idx, &v) == 0 ? v : 0;
    };

    const bool active          = rd_bool(3);
    const bool battles_allowed = rd_bool(6);
    const bool rocket_allowed  = rd_bool(26);
    const int32_t b_raw        = rd_int(0);
    const int32_t r_raw        = rd_int(6);

    if (tr->sample(&z) != 0 || !z.snapshot_fresh || a.snapshot_seq != z.snapshot_seq) {
        api_revoke();
        return;
    }

    const uint32_t b_mask = (b_raw >= 0 && b_raw <= PGR_CBH_BATTLE_MASK_ALL)
                                ? static_cast<uint32_t>(b_raw)
                                : 0u;
    const uint32_t r_mask = (r_raw >= 0 && r_raw <= PGR_CBH_ROCKET_MASK_ALL)
                                ? static_cast<uint32_t>(r_raw)
                                : 0u;

    PgrCombatHostOptions opts{};
    opts.version = PGR_COMBAT_HOST_API_VERSION;
    opts.size    = sizeof(opts);
    if (active)          opts.flags |= PGR_CBH_OPT_ACTIVE;
    if (battles_allowed) opts.flags |= PGR_CBH_OPT_BATTLES_ALLOWED;
    if (rocket_allowed)  opts.flags |= PGR_CBH_OPT_ROCKET_ALLOWED;
    opts.battle_mask     = b_mask;
    opts.rocket_mask     = r_mask;
    opts.battle_lease_ms = (active && battles_allowed && b_mask > 0) ? PGR_CBH_MAX_LEASE_MS : 0;
    opts.rocket_lease_ms = (active && rocket_allowed && r_mask > 0) ? PGR_CBH_MAX_LEASE_MS : 0;
    opts.epoch           = a.snapshot_seq;
    api_update(&opts);
}

int api_tick(uint64_t proven_tid) {
    if (!g_configured.load(std::memory_order_acquire)) return PGR_CBH_ERR_NOT_CONFIGURED;
    if (proven_tid == 0 || self_tid() != proven_tid) return PGR_CBH_ERR_THREAD;

    if (!g_ready.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(g_setup_mutex);
        int rc = finish_setup_locked(proven_tid);
        if (rc != PGR_CBH_OK) return rc;
    }

    const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
    if (proven_tid != owner || !thread_attached()) return PGR_CBH_ERR_THREAD;

    bool expected_in_tick = false;
    if (!g_in_tick.compare_exchange_strong(expected_in_tick, true, std::memory_order_acq_rel)) {
        return PGR_CBH_ERR_REENTRANT;
    }
    struct TickGuard {
        ~TickGuard() { g_in_tick.store(false, std::memory_order_release); }
    } tick_guard;
    FrameGuard fg;

    g_ticks.fetch_add(1, std::memory_order_relaxed);
    const bool stopping = g_stopping.load(std::memory_order_acquire) ||
                          g_failed.load(std::memory_order_acquire);

    if (!stopping) {
        poll_transport_options();
    }

    const uint32_t caps=g_capabilities.load();
    auto terminal=[](){return g_stopping.load()||g_failed.load();};
    // Re-read and serialize gate publication after each managed leaf pass. A revoke
    // during Rocket work cannot be overwritten by a stale pre-tick Battle snapshot.
    if((caps&PGR_CBH_CAP_ROCKET_COMBAT)&&g_rocket_combat){refresh_leaf_gates();g_rocket_combat->tick(proven_tid,terminal()?1:0);}
    if((caps&PGR_CBH_CAP_ROCKET_MINIGAME)&&g_rocket_mini){refresh_leaf_gates();if(terminal())g_rocket_mini->restore();g_rocket_mini->tick(1);}
    if((caps&PGR_CBH_CAP_BATTLES)&&g_battles){refresh_leaf_gates();g_battles->tick(proven_tid,terminal()?1:0);}
    g_leaves_latched.store(true, std::memory_order_release);
    drain_leaf_events(proven_tid);
    return PGR_CBH_OK;
}

int api_stop(uint64_t proven_tid) {
    if (!g_configured.load(std::memory_order_acquire)) return PGR_CBH_ERR_NOT_CONFIGURED;
    g_stopping.store(true, std::memory_order_release);
    api_revoke();

    const uint32_t caps  = g_capabilities.load(std::memory_order_acquire);
    const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
    const uint64_t tid   = (proven_tid != 0) ? proven_tid : owner;

    if ((caps & PGR_CBH_CAP_ROCKET_MINIGAME) && g_rocket_mini) {
        g_rocket_mini->update(0, 0);
        g_rocket_mini->restore();
        if (tid != 0 && self_tid() == tid && thread_attached()) {
            g_rocket_mini->tick(1);
        }
    }
    if ((caps & PGR_CBH_CAP_ROCKET_COMBAT) && g_rocket_combat) {
        g_rocket_combat->update(0, 0, tid);
        g_rocket_combat->tick(tid, 1);
    }
    if ((caps & PGR_CBH_CAP_BATTLES) && g_battles) {
        g_battles->update(0, 0, 1u, tid);
        if (tid != 0 && self_tid() == tid) {
            g_battles->tick(tid, 1);
        }
    }
    drain_leaf_events(tid);
    return PGR_CBH_OK;
}

int api_sample(PgrCombatHostSample *out) {
    if (!out) return PGR_CBH_ERR_INVALID_ARG;

    uint32_t b_roots = 0, b_scope = 0, b_swap = 0;
    int32_t  b_mode  = -1;
    uint64_t b_serial = 0;
    bool     b_failed = false;

    uint32_t r_roots = 0, r_scope = 0, r_ui = 0, r_shield = 0;
    int32_t  r_gate  = 0;
    uint64_t r_serial = 0;
    bool     r_failed = false;

    const uint32_t caps = g_capabilities.load(std::memory_order_acquire);
    if ((caps & PGR_CBH_CAP_BATTLES) && g_battles) {
        pgr_battle_sample_v1 bs{};
        if (g_battles->sample(&bs) == PGR_BATTLE_OK) {
            b_roots  = bs.owned_roots;
            b_scope  = bs.scope_active;
            b_mode   = bs.mode;
            b_swap   = bs.swap_blocked;
            b_serial = bs.action_serial;
            b_failed = (bs.failed != 0);
        }
    }
    if ((caps & PGR_CBH_CAP_ROCKET_COMBAT) && g_rocket_combat) {
        pgr_rocket_combat_sample_v1 rs{};
        if (g_rocket_combat->sample(&rs) == PGR_RC_OK) {
            r_roots  = rs.owned_roots;
            r_scope  = rs.scope_active;
            r_ui     = rs.ui_active;
            r_shield = rs.shield;
            r_serial = rs.action_serial;
            r_failed = (rs.failed != 0);
            if (rs.gate >= 1 && rs.gate <= 3)           r_gate = 1; // update-hold
            else if (rs.gate == 5 || rs.gate == 6)      r_gate = 1; // update-hold (vnext)
            else if (rs.gate == 4 || rs.gate == 7)      r_gate = 2; // combat-ready
            else                                        r_gate = 0;
        }
    }

    const uint32_t host_frames = g_active_frames.load(std::memory_order_relaxed);
    const bool failed          = g_failed.load(std::memory_order_acquire) || b_failed || r_failed;

    std::memset(out, 0, sizeof(*out));
    out->version              = PGR_COMBAT_HOST_API_VERSION;
    out->size                 = sizeof(PgrCombatHostSample);
    out->ready                = g_ready.load(std::memory_order_acquire) ? 1u : 0u;
    out->capabilities         = caps;
    out->stopping             = g_stopping.load(std::memory_order_acquire) ? 1u : 0u;
    out->failed               = failed ? 1u : 0u;
    out->last_error           = g_last_error.load(std::memory_order_acquire);
    out->roots                = b_roots + r_roots;
    out->battle_roots         = b_roots;
    out->rocket_roots         = r_roots;
    out->battle_scope_active  = b_scope;
    out->battle_mode          = b_mode;
    out->battle_swap_blocked  = b_swap;
    out->rocket_scope_active  = r_scope;
    out->rocket_ui_active     = r_ui;
    out->rocket_shield_active = r_shield;
    out->rocket_gate          = r_gate;
    out->mini_active          = g_mini_active.load(std::memory_order_acquire);
    out->active_frames        = host_frames;
    out->installed_hooks      = g_installed_hooks.load(std::memory_order_relaxed);
    out->owner_tid            = g_owner_tid.load(std::memory_order_acquire);
    out->ticks                = g_ticks.load(std::memory_order_relaxed);
    out->battle_action_serial = b_serial;
    out->rocket_action_serial = r_serial;
    out->gym_cooldown_serial  = g_gym_cooldown_serial.load(std::memory_order_acquire);
    return PGR_CBH_OK;
}

int api_next_event(PgrCombatHostEvent *out, char *json_buf, uint32_t json_cap) {
    if (!out) return PGR_CBH_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(g_event_mutex);
    if (g_event_count == 0) return 0;

    const QueuedEvent &front = g_events[g_event_head];
    *out = front.desc;
    const uint32_t needed = static_cast<uint32_t>(front.json.size());
    if (needed > 0) {
        if (!json_buf || json_cap < needed + 1) {
            return PGR_CBH_ERR_BUFFER_TOO_SMALL;
        }
        std::memcpy(json_buf, front.json.data(), needed);
        json_buf[needed] = '\0';
    } else if (json_buf && json_cap > 0) {
        json_buf[0] = '\0';
    }

    g_events[g_event_head] = QueuedEvent{};
    g_event_head = (g_event_head + 1) % kEventCap;
    --g_event_count;
    return 1;
}

void api_on_unity_pulse(void * /*ctx*/, const PgrUnityPulse *pulse) {
    if (!pulse || pulse->tid <= 0) {
        return;
    }
    const auto *sched = api_table<PgrUnitySchedulerApi>(pgrazor_unity_scheduler_api(2));
    if (sched && sched->version >= 2 && sched->size >= sizeof(PgrUnitySchedulerApi) &&
        sched->snapshot && sched->valid) {
        PgrUnitySchedulerSnapshot snap{};
        if (sched->snapshot(&snap) == PGR_US_OK && snap.installed != 0) {
            if (sched->valid(pulse->generation) != 1) return;
        }
    }
    api_tick(static_cast<uint64_t>(pulse->tid));
}

const PgrCombatHostApi kApiTable = {
    PGR_COMBAT_HOST_API_VERSION,
    sizeof(PgrCombatHostApi),
    api_configure,
    api_update,
    api_tick,
    api_revoke,
    api_stop,
    api_sample,
    api_next_event,
    api_on_unity_pulse
};

} // namespace

extern "C" const void *pgrazor_combat_host_api(uint32_t version) {
    return version == PGR_COMBAT_HOST_API_VERSION ? &kApiTable : nullptr;
}

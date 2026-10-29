#pragma once

#include <stddef.h>
#include <stdint.h>

#include "shared_metadata.h"
#include "unity_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

// Feature 66: Compiled production Battle + Team GO Rocket + Rocket Minigame
// orchestration host (`combat_host`).
//
// Composes existing compiled leaves in C++:
//   - Feature 6  (`rocket_minigame.h`): Rocket particle minigame collector (4 Dobby hooks)
//   - Feature 19 (`battles.h`):         Raid / Gym / Max-Dynamax / Legendary battle controller
//   - Feature 20 (`rocket_actions.h`):  Stateless Rocket fast / charge / shield action helper
//   - Feature 21 (`rocket_combat.h`):   Rocket scope / UI / vNext / shield / hold-gate controller
//
// Consumes canonical Feature 56 (`PgrSharedMetadataOutput`, 67304B) resolved from
// explicit `cfg->metadata`, `pgrazor_shared_host_api(1)->metadata()`, or
// `pgrazor_shared_metadata_api(2)->prepare(&cfg->metadata_config, ...)`.
//
// Owns all synchronous Dobby observer hooks for Rocket Combat (10 dedicated +
// 1 shared) and Battles (31 dedicated + 1 shared) with a single compiled owner
// hook on `ChargeAttackDisplayVNext.ObserveBattleState` (`H_RK_VN_OBSERVE_CODE`)
// fanning out to both Rocket Combat and Battles. Never hooks `il2cpp_runtime_invoke`
// or `il2cpp_runtime_invoke_convert_args`.
enum {
    PGR_COMBAT_HOST_FEATURE_ID  = 66,
    PGR_COMBAT_HOST_API_VERSION = 1,

    PGR_CBH_MAX_LEASE_MS        = 2500,
    PGR_CBH_BATTLE_MASK_ALL     = 1023,
    PGR_CBH_ROCKET_MASK_ALL     = 7,
    PGR_CBH_STATE_OPEN          = 2
};

enum PgrCombatHostStatus {
    PGR_CBH_OK                   = 0,
    PGR_CBH_NOT_READY            = 1,
    PGR_CBH_ERR_INVALID_ARG      = -1,
    PGR_CBH_ERR_VERSION          = -2,
    PGR_CBH_ERR_SIZE             = -3,
    PGR_CBH_ERR_NULL_EXPORT      = -4,
    PGR_CBH_ERR_METADATA         = -5,
    PGR_CBH_ERR_LEAF_UNAVAILABLE = -6,
    PGR_CBH_ERR_LEAF_INIT        = -7,
    PGR_CBH_ERR_HOOK_INSTALL     = -8,
    PGR_CBH_ERR_THREAD           = -9,
    PGR_CBH_ERR_STOPPED          = -10,
    PGR_CBH_ERR_REENTRANT        = -11,
    PGR_CBH_ERR_NOT_CONFIGURED   = -12,
    PGR_CBH_ERR_BUFFER_TOO_SMALL = -13
};

enum PgrCombatHostCaps : uint32_t {
    PGR_CBH_CAP_NONE                 = 0u,
    PGR_CBH_CAP_ROCKET_MINIGAME      = 1u << 0,
    PGR_CBH_CAP_ROCKET_ACTIONS       = 1u << 1,
    PGR_CBH_CAP_ROCKET_COMBAT        = 1u << 2,
    PGR_CBH_CAP_BATTLES              = 1u << 3,
    PGR_CBH_CAP_SHARED_VNEXT_OBSERVE = 1u << 4
};

enum PgrCombatHostConfigFlags : uint32_t {
    PGR_CBH_CFG_HAS_METADATA_CONFIG = 1u << 0,
    PGR_CBH_CFG_USE_TRANSPORT       = 1u << 1,
    PGR_CBH_CFG_REQUIRE_BATTLES     = 1u << 2,
    PGR_CBH_CFG_REQUIRE_ROCKET      = 1u << 3
};

enum PgrCombatHostOptionFlags : uint32_t {
    PGR_CBH_OPT_ACTIVE          = 1u << 0,
    PGR_CBH_OPT_BATTLES_ALLOWED = 1u << 1,
    PGR_CBH_OPT_ROCKET_ALLOWED  = 1u << 2,
    PGR_CBH_OPT_STOPPING        = 1u << 3
};

enum PgrCombatHostEventKind : uint32_t {
    PGR_CBH_EV_NONE                  = 0,
    PGR_CBH_EV_HOST_READY            = 1,  // native-combat-host-ready
    PGR_CBH_EV_BATTLES_READY         = 2,  // native-battles-ready
    PGR_CBH_EV_BATTLE_SCOPE          = 3,  // battle-scope
    PGR_CBH_EV_BATTLE_INPUT          = 4,  // battle-input
    PGR_CBH_EV_BATTLE_ENDED          = 5,  // battle-ended
    PGR_CBH_EV_BATTLE_DODGE_CUE      = 6,  // battle-dodge-cue
    PGR_CBH_EV_BATTLE_ACTION         = 7,  // battle-action
    PGR_CBH_EV_BATTLE_MANUAL_SWITCH  = 8,  // battle-manual-switch
    PGR_CBH_EV_BATTLE_ERROR          = 9,  // battle-error
    PGR_CBH_EV_COOLDOWN_ACTION       = 10, // cooldown-action {"action":"Gym battle"}
    PGR_CBH_EV_ROCKET_COMBAT_READY   = 11, // native-rocket-combat-ready
    PGR_CBH_EV_ROCKET_MINIGAME_READY = 12, // rocket-minigame-native
    PGR_CBH_EV_ROCKET_SCOPE          = 13, // rocket-scope
    PGR_CBH_EV_ROCKET_UI             = 14, // rocket-ui
    PGR_CBH_EV_ROCKET_ACTION         = 15, // rocket-action
    PGR_CBH_EV_ROCKET_ATTACK_GATE    = 16, // rocket-attack-gate
    PGR_CBH_EV_ROCKET_ERROR          = 17  // rocket-error
};

struct PgrCombatHostConfig {
    uint32_t version;                          // 0: 1
    uint32_t size;                             // 4: sizeof(PgrCombatHostConfig) == 200 on LP64
    uint32_t flags;                            // 8: PgrCombatHostConfigFlags
    uint32_t reserved0;                        // 12: 0

    // Optional pre-prepared Feature 56 output. If NULL, resolved from
    // `pgrazor_shared_host_api(1)->metadata()` or `metadata_config`.
    const PgrSharedMetadataOutput *metadata;   // 16

    // Direct IL2CPP exports required by leaves 6 / 19 / 20 / 21.
    uintptr_t thread_current;                  // 24: il2cpp_thread_current
    uintptr_t gchandle_new;                    // 32: il2cpp_gchandle_new
    uintptr_t gchandle_get_target;             // 40: il2cpp_gchandle_get_target
    uintptr_t gchandle_free;                   // 48: il2cpp_gchandle_free
    uintptr_t object_get_class;                // 56: il2cpp_object_get_class
    uintptr_t object_unbox;                    // 64: il2cpp_object_unbox
    uintptr_t runtime_invoke;                  // 72: il2cpp_runtime_invoke
    uintptr_t field_get_value;                 // 80: il2cpp_field_get_value

    uint64_t owner_tid;                        // 88: proven Unity thread ID
    uint64_t reserved1;                        // 96: 0

    // Optional embedded Feature 56 config when `PGR_CBH_CFG_HAS_METADATA_CONFIG` is set.
    PgrSharedMetadataConfig metadata_config;   // 104: 96 bytes (104..199)
};

struct PgrCombatHostOptions {
    uint32_t version;          // 0: 1
    uint32_t size;             // 4: sizeof(PgrCombatHostOptions) == 48
    uint32_t flags;            // 8: PgrCombatHostOptionFlags
    uint32_t battle_mask;      // 12: 0..1023
    uint32_t rocket_mask;      // 16: 0..7
    uint32_t reserved0;        // 20: 0
    uint64_t battle_lease_ms;  // 24: clamped to <= 2500ms
    uint64_t rocket_lease_ms;  // 32: clamped to <= 2500ms
    uint64_t epoch;            // 40
};

struct PgrCombatHostSample {
    uint32_t version;              // 0: 1
    uint32_t size;                 // 4: sizeof(PgrCombatHostSample) == 120
    uint32_t ready;                // 8: 0 or 1
    uint32_t capabilities;         // 12: PgrCombatHostCaps bitmask
    uint32_t stopping;             // 16: 0 or 1
    uint32_t failed;               // 20: 0 or 1
    int32_t  last_error;           // 24: PgrCombatHostStatus
    uint32_t roots;                // 28: battle_roots + rocket_roots
    uint32_t battle_roots;         // 32: active battle GCHandles (0..64)
    uint32_t rocket_roots;         // 36: active rocket GCHandles (0..32)
    uint32_t battle_scope_active;  // 40: 0 or 1
    int32_t  battle_mode;          // 44: -1, 0 (raid), 1 (gym), 2 (max)
    uint32_t battle_swap_blocked;  // 48: 0 or 1
    uint32_t rocket_scope_active;  // 52: 0 or 1
    uint32_t rocket_ui_active;     // 56: 0 or 1
    uint32_t rocket_shield_active; // 60: 0 or 1
    int32_t  rocket_gate;          // 64: 0 (unknown), 1 (hold), 2 (ready)
    uint32_t mini_active;          // 68: 0 or 1
    uint32_t active_frames;        // 72: in-flight hook/tick frames
    uint32_t installed_hooks;      // 76: total Dobby hooks installed by host + leaf 6
    uint64_t owner_tid;            // 80: proven Unity thread ID
    uint64_t ticks;                // 88: total Unity ticks processed
    uint64_t battle_action_serial; // 96: monotonic battle action counter
    uint64_t rocket_action_serial; // 104: monotonic rocket action counter
    uint64_t gym_cooldown_serial;  // 112: monotonic gym cooldown-action counter
};

struct PgrCombatHostEvent {
    uint32_t version;   // 0: 1
    uint32_t size;      // 4: sizeof(PgrCombatHostEvent) == 64
    uint32_t kind;      // 8: PgrCombatHostEventKind
    uint32_t json_len;  // 12: UTF-8 byte length (excluding NUL)
    int64_t  code0;     // 16: mode / aux0
    int64_t  code1;     // 24: aux1
    uint64_t serial;    // 32: monotonic event serial
    uint64_t tid;       // 40: emitting thread ID
    uint64_t reserved0; // 48
    uint64_t reserved1; // 56
};

struct PgrCombatHostApi {
    uint32_t version; // 0: 1
    uint32_t size;    // 4: sizeof(PgrCombatHostApi) == 72 on LP64

    int (*configure)(const PgrCombatHostConfig *cfg);
    int (*update)(const PgrCombatHostOptions *opts);
    int (*tick)(uint64_t proven_tid);
    int (*revoke)(void);
    int (*stop)(uint64_t proven_tid);
    int (*sample)(PgrCombatHostSample *out);
    int (*next_event)(PgrCombatHostEvent *out, char *json_buf, uint32_t json_cap);

    // Feature 48 API 2 compatible post-original pulse callback.
    void (*on_unity_pulse)(void *ctx, const PgrUnityPulse *pulse);
};

#if defined(__cplusplus) && (__SIZEOF_POINTER__ == 8)
static_assert(sizeof(PgrCombatHostConfig) == 200, "PgrCombatHostConfig LP64 size mismatch");
static_assert(offsetof(PgrCombatHostConfig, metadata) == 16, "metadata offset");
static_assert(offsetof(PgrCombatHostConfig, thread_current) == 24, "thread_current offset");
static_assert(offsetof(PgrCombatHostConfig, field_get_value) == 80, "field_get_value offset");
static_assert(offsetof(PgrCombatHostConfig, owner_tid) == 88, "owner_tid offset");
static_assert(offsetof(PgrCombatHostConfig, metadata_config) == 104, "metadata_config offset");
static_assert(sizeof(PgrCombatHostOptions) == 48, "PgrCombatHostOptions size mismatch");
static_assert(sizeof(PgrCombatHostSample) == 120, "PgrCombatHostSample size mismatch");
static_assert(sizeof(PgrCombatHostEvent) == 64, "PgrCombatHostEvent size mismatch");
static_assert(sizeof(PgrCombatHostApi) == 72, "PgrCombatHostApi LP64 size mismatch");
#endif

const void *pgrazor_combat_host_api(uint32_t version);

#ifdef __cplusplus
}
#endif

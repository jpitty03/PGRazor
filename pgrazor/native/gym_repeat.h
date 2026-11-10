#pragma once

#include <stddef.h>
#include <stdint.h>

struct PgrSharedMetadataOutput;

#ifdef __cplusplus
extern "C" {
#endif

// Gym "Battle until defeated" native leaf.
//
// Owns the gym re-entry lifecycle hooks (GymApproachGuiController.HandleOpen /
// OnBackButtonPressed, GymBattlePrepUIController.HandleOpen / Battle / ExitPressed,
// GymBattleState.ExitBattleResultsState)
// and drives the pure decision machine in gym_repeat_policy.h. It performs at most
// one game-owned action per scheduled tick on the proven Unity owner thread.
//
// It is NOT a separately queryable Feature; the Combat Orchestration Host (feature 66,
// combat_host.cpp) configures it, forwards the GymBattleState scope enter/exit and the
// GymBattleContinueGuiController.LeaveBattle cancellation it already hooks, and drives
// its tick()/stop(). The host never re-hooks the methods this leaf owns, and this leaf
// never hooks il2cpp_runtime_invoke or the scope/leave methods the host already owns.
enum {
    PGR_GYM_REPEAT_API_VERSION = 1
};

enum PgrGymRepeatStatus {
    PGR_GR_OK                = 0,
    PGR_GR_NOT_READY         = 1,
    PGR_GR_ERR_INVALID_ARG   = -1,
    PGR_GR_ERR_VERSION       = -2,
    PGR_GR_ERR_SIZE          = -3,
    PGR_GR_ERR_NULL_EXPORT   = -4,
    PGR_GR_ERR_METADATA      = -5,
    PGR_GR_ERR_BOOTSTRAP     = -6,
    PGR_GR_ERR_BINDING       = -7,
    PGR_GR_ERR_HOOK_INSTALL  = -8,
    PGR_GR_ERR_THREAD        = -9
};

struct PgrGymRepeatConfig {
    uint32_t version;                        // 0: PGR_GYM_REPEAT_API_VERSION
    uint32_t size;                           // 4: sizeof(PgrGymRepeatConfig)
    uint64_t owner_tid;                      // 8: proven Unity owner thread id
    // Canonical Feature 56 output (opt map-gym find/fort/raid handles + core alive
    // method). Runtime exports, metadata class/method resolution, module base and the
    // transport gate are taken from the existing runtime leaves (bootstrap 54,
    // metadata 1, transport 52) exactly like gym_runtime.cpp.
    const PgrSharedMetadataOutput *metadata; // 16
};

struct PgrGymRepeatSample {
    uint32_t version;         // 0: 1
    uint32_t size;            // 4: sizeof(PgrGymRepeatSample)
    uint32_t ready;           // 8: 1 once the required hooks are installed
    uint32_t installed_hooks; // 12: total Dobby hooks installed by this leaf
    int32_t  state;           // 16: pgr_gym_repeat::State
    int32_t  last_end;        // 20: pgr_gym_repeat::EndReason of the last session
    uint32_t rounds;          // 24: victorious rounds in the current/last session
    uint32_t roots;           // 28: live GC handles held by this leaf
    uint64_t owner_tid;       // 32: proven Unity owner thread id
    uint64_t sessions;        // 40: total armed sessions
    uint64_t actions;         // 48: total game-owned actions performed
};

struct PgrGymRepeatApi {
    uint32_t version; // 0: 1
    uint32_t size;    // 4: sizeof(PgrGymRepeatApi)

    // Resolve + validate the gym lifecycle bindings and install the owned hooks.
    // Returns PGR_GR_OK on success; a binding/hook failure leaves combat untouched
    // (no partial hooks race): the leaf stays inert and reports the error.
    int  (*configure)(const PgrGymRepeatConfig *cfg);

    // Forwarded from the host's existing GymBattleState scope hooks. entered != 0 for
    // EnterState, 0 for ExitState. Owner thread only.
    void (*observe_scope)(int entered, uint64_t proven_tid);

    // Forwarded from the host's existing GymBattleContinueGuiController.LeaveBattle
    // hook (a genuine manual cancel unless it was our own back action).
    void (*observe_leave)(uint64_t proven_tid);

    // Scheduled owner-thread tick: run the machine and perform at most one action.
    int  (*tick)(uint64_t proven_tid);

    // End any session and release roots. Owner thread when possible.
    void (*stop)(uint64_t proven_tid);

    int  (*sample)(PgrGymRepeatSample *out);
};

const void *pgrazor_gym_repeat_api(uint32_t version);

#ifdef __cplusplus
}
#endif

#pragma once
#include <stdint.h>

// PGRazor native migration: Team GO Rocket combat controller (feature id 21, API 1).
//
// A native port of runtime/features/rocket.js (rocketState + rocketTick / rocketVNextTick,
// with rocketClear / rocketDrop / rocketBool / rocketGate) PLUS the managed-object rooting
// that the inline Gum watchers currently host in JS. This module OWNS every managed root
// (the Rocket scope, the legacy CombatUI, the vNext gui, up to 4 vNext charge candidates and
// the shield modal), the per-candidate evaluation and the runtime_invoke of each game method.
//
// WHAT THIS MODULE IS NOT (the incremental boundary -- see docs/NATIVE_MIGRATION.md):
//   * It installs NO Dobby and NO Gum hooks. The scope enter/exit, the CombatUI Initialize /
//     UpdateActionsUI / HandleDismiss, the vNext UpdateUiWidgets / HandleDismiss /
//     ObserveBattleState (RVCHARGE -- SHARED with the still-Gum battle controller feature 19),
//     and the shield ShowShieldPrompt / ShieldUiClicked / HandleDismiss watchers ALL stay JS
//     Gum watchers in the coordinator. Each forwards its already-validated il2cpp object into
//     this table via observe() / begin_ui() / end_ui(). The C++ layer never installs a hook
//     and NEVER calls back into JS.
//   * It does NOT own the charged-attack particle minigame (feature 6, rocket_minigame.cpp).
//     That controller keeps its own roots/hooks; the coordinator gates it off this module's
//     sample() facade (scope_active / failed).
//   * It does NOT re-implement the Rocket action authorization/cadence policy. That lives in
//     feature 20 (rocket_actions.cpp). This module reuses it INTERNALLY through
//     pgrazor_rocket_actions_api(1): it forwards scope/prompt/shield-used observations into
//     feature 20, and every game-method call is gated by a feature 20 begin()/claim() ticket.
//     No feature 20 JS controller runs while feature 21 is selected.
//
// Division of labour with runtime/native-rocket-combat.js:
//   * JS validates + packs every il2cpp pointer (exports, MethodInfo*, FieldInfo*, exact
//     Il2CppClass*, the STATE_OPEN literal) into the config and installs it (NO hooks). It keeps
//     the existing Gum watchers, forwards each observation into observe()/begin_ui()/end_ui(),
//     drives update()/tick() from the verified Unity heartbeat, and drains the bounded event
//     FIFO into the exact rocket-* events. It makes NO managed decision.
//   * This module owns all GC rooting (RAII, bounded, detached-before-free, refreshed after every
//     managed/GC call), the scope epoch + generation-safe paired-UI publication, the shield /
//     charged / quick decision under the mask + lease + feature-20 ticket gate, and the bounded
//     telemetry FIFO. CLOCK_BOOTTIME throughout. No mutex is ever held across a managed call.
//
// Threading model (mirrors feature 18/19): tick() latches the permanent Unity identity once;
// every managed entry point (observe / begin_ui / end_ui / tick) then requires that SAME latched
// owner AND the caller's self-reported proven_tid to equal both the owner and the live OS tid.
// Before the owner is latched the managed entry points are inert no-ops. update() is the one
// policy call allowed off-thread (a foreign thread can only disarm). sample()/next_event() are
// pull-only. No lock is held across a managed call.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "Rocket combat native ABI requires LP64");

enum PgrRocketCombatInfo {
  PGR_RC_FEATURE_ID   = 21,
  PGR_RC_API_VERSION  = 1,
  PGR_RC_MAX_CHARGES  = 4,    // vNext charge-candidate cap (exact source: list.length<4)
  PGR_RC_EVENT_CAP    = 64,   // bounded telemetry FIFO depth (fail-closed on overflow)
  PGR_RC_CLOSED_STATE = 3     // GuiController.currentState closed scalar (literal, source: ===3)
};

// observe() kinds. aux carries the shield-prompt bool for SHIELD_PROMPT, else ignored.
enum PgrRocketCombatObserve {
  PGR_RC_OBS_SCOPE_EXIT    = 0,  // obj=scope; rocketClear when it is the live scope
  PGR_RC_OBS_SCOPE_ENTER   = 1,  // obj=scope (InvasionCombatState); rocketClear + set + arm 20
  PGR_RC_OBS_UI_UPDATE     = 2,  // obj=CombatUI (UpdateActionsUI live-update); replace legacy ui
  PGR_RC_OBS_UI_CLOSE      = 3,  // obj=CombatUI (HandleDismiss); drop ui by identity
  PGR_RC_OBS_VNEXT_UPDATE  = 4,  // obj=CombatVNextGuiController; replace vNext gui
  PGR_RC_OBS_VNEXT_CLOSE   = 5,  // obj=vNext gui; drop vui + charges by identity
  PGR_RC_OBS_CHARGE        = 6,  // obj=ChargeAttackDisplayVNext; prune/dedupe/append (<=4)
  PGR_RC_OBS_SHIELD_PROMPT = 7,  // obj=ShieldModalGuiController, aux=prompt bool; open shield
  PGR_RC_OBS_SHIELD_MANUAL = 8,  // obj=shield modal (ShieldUiClicked); mark used + notify 20
  PGR_RC_OBS_SHIELD_CLOSE  = 9   // obj=shield modal (HandleDismiss); drop shield by identity
};

// Table status codes. 0 == success.
enum PgrRocketCombatStatus {
  PGR_RC_OK               = 0,
  PGR_RC_ERR_NULL_ARG     = 1,
  PGR_RC_ERR_BAD_CONFIG   = 2,
  PGR_RC_ERR_ALREADY_BOUND = 3,
  PGR_RC_ERR_NOT_INSTALLED = 4,
  PGR_RC_ERR_NO_CLOCK     = 5,
  PGR_RC_ERR_NO_POLICY    = 6   // feature 20 action table (pgrazor_rocket_actions_api) unavailable
};

// Bounded-FIFO event codes drained by next_event(). Carry only action / source / reason; never
// an account identifier. The adapter reproduces the exact rocket-* event names/reasons verbatim.
enum PgrRocketCombatEventCode {
  PGR_RC_EVT_NONE   = 0,
  PGR_RC_EVT_READY  = 1,  // native-rocket-combat-ready (aux=1)
  PGR_RC_EVT_SCOPE  = 2,  // rocket-scope        (aux=active 1/0)
  PGR_RC_EVT_UI     = 3,  // rocket-ui           (aux=PgrRocketCombatUiSource)
  PGR_RC_EVT_ACTION = 4,  // rocket-action       (id=PgrRocketCombatAction, aux bit0=accepted, bit1=vnext)
  PGR_RC_EVT_GATE   = 5,  // rocket-attack-gate  (id=PgrRocketCombatGate)
  PGR_RC_EVT_ERROR  = 6   // rocket-error        (managed-invoke fault)
};

// rocket-ui "source" values carried in the event aux.
enum PgrRocketCombatUiSource {
  PGR_RC_UI_LIVE  = 0,  // "live-update"
  PGR_RC_UI_VNEXT = 1   // "vnext-update"
};

// rocket-action kinds carried in the event id.
enum PgrRocketCombatAction {
  PGR_RC_ACT_SHIELD  = 0,  // "shield"
  PGR_RC_ACT_CHARGED = 1,  // "charged"
  PGR_RC_ACT_QUICK   = 2   // "quick"
};

// rocket-attack-gate reason codes carried in the event id. The director-type-name detail that the
// JS gate string carried ("director:<typeName>") is NOT reproduced -- see the module limitations.
enum PgrRocketCombatGate {
  PGR_RC_GATE_NONE       = 0,  // cleared (not emitted)
  PGR_RC_GATE_WAIT_UI    = 1,  // "waiting-for-battle-ui"
  PGR_RC_GATE_DIRECTOR   = 2,  // "director-unexpected"  (JS: "director:<typeName>")
  PGR_RC_GATE_INACTIVE   = 3,  // "battle-not-active"
  PGR_RC_GATE_ACTIVE     = 4,  // "active"
  PGR_RC_GATE_VN_BLOCKED = 5,  // "vnext-input-blocked"
  PGR_RC_GATE_VN_NOFAST  = 6,  // "vnext-fast-input-disabled"
  PGR_RC_GATE_VN_ACTIVE  = 7   // "vnext-active"
};

// Install-time configuration (fixed LP64 layout, 344 bytes). Every pointer is an already-validated
// il2cpp export / MethodInfo* / FieldInfo* / Il2CppClass*; the native layer re-checks and rejects
// any null. See the static_asserts in rocket_combat.cpp for the authoritative offsets/sizes.
typedef struct pgr_rocket_combat_config_v1 {
  uint32_t version;  // == PGR_RC_API_VERSION
  uint32_t size;     // == sizeof(pgr_rocket_combat_config_v1)

  // il2cpp runtime exports (pointer-sized ABI).                                   offset
  void *gchandle_new;         // void* (void* obj, int pinned) -> handle              8
  void *gchandle_get_target;  // void* (void* handle)                                16
  void *gchandle_free;        // void  (void* handle)                                24
  void *object_get_class;     // void* (void* obj)                                   32
  void *object_unbox;         // void* (void* boxed)                                 40
  void *runtime_invoke;       // void* (MethodInfo*, void* obj, void** params, void** exc) 48
  void *field_get_value;      // void  (void* obj, FieldInfo*, void* out)            56

  // Legacy CombatUI path MethodInfo* (never hooked).                                offset
  void *m_director;      // CombatUI bpro -> ICombatDirectorV2                         64
  void *m_active;        // CombatDirectorV2 get_IsInActiveState -> bool               72
  void *m_fast;          // CombatDirectorV2 TryFastAttack -> CombatActionProto (ptr)  80
  void *m_charge;        // CombatChargeAttackDisplay OnTap -> void                    88
  void *m_visible;       // CombatChargeAttackDisplay get_IsVisible -> bool            96
  void *m_interactable;  // UnityEngine.UI.Selectable IsInteractable -> bool          104
  void *m_shield;        // ShieldModalGuiController ShieldUiClicked -> void          112

  // vNext path MethodInfo* (never hooked).                                           offset
  void *vm_blocked;      // CombatVNextGuiController get_IsInputBlocked -> bool       120
  void *vm_charged;      // ChargeAttackDisplayVNext get_IsCharged -> bool           128
  void *vm_visible;      // ChargeAttackDisplayVNext get_IsVisible -> bool           136
  void *vm_charge;       // ChargeAttackDisplayVNext ChargeAttackButtonTapped -> void 144
  void *vm_fast;         // CombatVNextGestureHandler cqul(System.Object) -> void     152

  void *m_alive;         // UnityEngine.Object op_Implicit(Object) -> bool [static]   160

  // Legacy CombatUI FieldInfo* read via field_get_value (NEVER a raw offset).        offset
  void *f_first;         // CombatUI dofo -> CombatChargeAttackDisplay (ref)          168
  void *f_second;        // CombatUI dofp -> CombatChargeAttackDisplay (ref)          176
  void *f_charged;       // CombatChargeAttackDisplay <IsCharged> (bool, present==1)  184
  void *f_input;         // CombatChargeAttackDisplay <InputEnabled> (bool, ==1)      192
  void *f_shields;       // ShieldModalGuiController dnve (int32 count, >0)           200
  void *f_button;        // ShieldModalGuiController shieldButton (ref)              208

  // vNext FieldInfo*.                                                                offset
  void *vf_gesture;      // CombatVNextGuiController eovv -> gesture handler (ref)    216
  void *vf_enabled;      // BattleGestureHandlerVNext FastAttackInputEnabled (==1)   224
  void *vf_tap;          // BattleGestureHandlerVNext tapGesture (ref)               232
  void *vf_button;       // ChargeAttackDisplayVNext attackButton (ref)             240

  // Shared GuiController state field (int32 enum read via field_get_value, NOT boxed). offset
  void *f_current_state; // GuiController currentState (int32: STATE_OPEN / closed)   248

  // Exact Il2CppClass* identities.                                                   offset
  void *class_rocket;    // InvasionCombatState (scope)                              256
  void *class_rui;       // CombatUI (legacy gui)                                    264
  void *class_rdir;      // CombatDirectorV2 (director; NOT UnityEngine.Object)      272
  void *class_rcharge;   // CombatChargeAttackDisplay (legacy charge)                280
  void *class_rshield;   // ShieldModalGuiController                                 288
  void *class_rvui;      // CombatVNextGuiController                                 296
  void *class_rvgest;    // CombatVNextGestureHandler                               304
  void *class_rvcharge;  // ChargeAttackDisplayVNext (vNext charge)                 312
  void *class_bool;      // System.Boolean (boxed-bool unbox check)                 320

  // Validated scalar literal + reserved padding.                                     offset
  int32_t state_open;    // GuiController.currentState normal-open scalar (STATE_OPEN) 328
  uint32_t reserved0;    // must be 0                                                332
  uint32_t reserved1;    // must be 0                                                336
  uint32_t reserved2;    // must be 0                                                340
} pgr_rocket_combat_config_v1;  // 344 bytes

// Pull-only telemetry snapshot (64 bytes). Safe from any thread; never frees. The adapter builds
// its rocketState facade (state truthiness + failed, for the battle-19 mutual-exclusion gate and
// the minigame-6 gate) and the owned_roots finish guard from this.
typedef struct pgr_rocket_combat_sample_v1 {
  uint32_t version;       // == PGR_RC_API_VERSION
  uint32_t size;          // == sizeof(pgr_rocket_combat_sample_v1)
  uint32_t scope_active;  // a Rocket scope is held (1) / absent (0)  -> rocketState.state truthy
  uint32_t failed;        // encounter fault latch (cleared on the next scope enter)
  uint32_t ui_active;     // a legacy CombatUI OR vNext gui is held (1/0)
  uint32_t charges;       // live vNext charge-candidate root count
  uint32_t shield;        // a shield modal is held (1/0)
  int32_t  gate;          // last attack-gate reason (PgrRocketCombatGate, -1 none)
  uint64_t action_serial; // bumped on each action issued
  uint64_t scope_serial;  // bumped on each scope enter
  uint32_t fault_serial;  // bumped on each managed-invoke fault
  uint32_t dropped_events;// FIFO overflow count (fail-closed: blocks new actions until drained)
  uint32_t owned_roots;   // durable GC roots + active frames (coordinator finish guard; nonzero in cleanup)
  uint32_t reserved;      // 0
} pgr_rocket_combat_sample_v1;

// Bounded-FIFO event record (16 bytes). Drained in strict order by next_event().
typedef struct pgr_rocket_combat_event_v1 {
  uint32_t code;      // PgrRocketCombatEventCode
  int32_t  id;        // action / gate reason (or 0)
  int32_t  aux;       // active / ui-source / action flags
  int32_t  reserved;  // 0
} pgr_rocket_combat_event_v1;

/* install(config): validate + publish the config once; also resolve the feature-20 action table
 * (pgrazor_rocket_actions_api(1)). A repeat of the identical config is OK; a different one returns
 * ALREADY_BOUND. BAD_CONFIG on header/size/pointer/literal problems; NO_CLOCK if CLOCK_BOOTTIME is
 * unavailable; NO_POLICY if feature 20 is not linked in. Installs NO hooks. */
typedef int (*pgr_rocket_combat_install_fn)(const pgr_rocket_combat_config_v1 *config);

/* update(mask, lease_ms, proven_tid): refresh the per-tick policy gate (the rocketMask bits 1/2/4
 * and a residual lease clamped to feature 20's cap, turned into an absolute CLOCK_BOOTTIME
 * deadline) and forward the same mask/lease into feature 20. A foreign thread (once the owner is
 * latched) can only disarm. Any thread. */
typedef int (*pgr_rocket_combat_update_fn)(uint32_t mask, uint64_t lease_ms, uint64_t proven_tid);

/* observe(kind, obj, aux, proven_tid): forward one Gum-watcher observation (see
 * PgrRocketCombatObserve). Performs the exact-class identity checks itself; a mismatch is a silent
 * no-op. Owner thread only (inert otherwise). */
typedef int (*pgr_rocket_combat_observe_fn)(uint32_t kind, void *obj, int32_t aux,
                                            uint64_t proven_tid);

/* begin_ui(obj, proven_tid): the legacy CombatUI Initialize onEnter. Returns a nonzero token iff a
 * Rocket scope is held and obj is exactly CombatUI; else 0. Roots obj into a pending slot WITHOUT
 * publishing it, binding the current scope epoch. The token is invalidated by any live UI
 * replacement/close or a newer begin_ui so a late onLeave can never overwrite a replacement UI. */
typedef uint64_t (*pgr_rocket_combat_begin_ui_fn)(void *obj, uint64_t proven_tid);

/* end_ui(token, obj, proven_tid): the matching onLeave, called AFTER the original ran. Publishes
 * the pending UI as the live ui iff the token is still the latest pending token, its scope epoch is
 * unchanged, and the token still identifies the rooted pending object. The obj argument is reserved and ignored. A stale / replayed / superseded token is dropped
 * (its pending root freed) without touching the live ui. */
typedef int (*pgr_rocket_combat_end_ui_fn)(uint64_t token, void *obj, uint64_t proven_tid);

/* tick(proven_tid, stopping): one owner-thread rocketTick pass -- shield-first, then the vNext or
 * legacy charged/quick decision, each gated by a fresh feature-20 begin()/claim() ticket and a
 * final clock/lease/epoch/stop re-check immediately before the runtime_invoke (no GC call between
 * claim and invoke). stopping != 0 disarms feature 20 and clears the Rocket state (deferred to the
 * outermost frame if a nested tick/observer arrives inside a managed getter). Off the proven thread
 * it is a no-op. */
typedef int (*pgr_rocket_combat_tick_fn)(uint64_t proven_tid, int stopping);

/* sample(out): copy the telemetry snapshot. Any thread; never frees. */
typedef int (*pgr_rocket_combat_sample_fn)(pgr_rocket_combat_sample_v1 *out);

/* next_event(out): dequeue one FIFO event (returns 1) or report empty (returns 0). Owner thread. */
typedef int (*pgr_rocket_combat_next_event_fn)(pgr_rocket_combat_event_v1 *out);

// Ordered, read-only table (8-byte header + 8 function pointers = 72 bytes).
typedef struct pgr_rocket_combat_api_v1 {
  uint32_t version;  // == PGR_RC_API_VERSION
  uint32_t size;     // == sizeof(pgr_rocket_combat_api_v1)
  pgr_rocket_combat_install_fn    install;
  pgr_rocket_combat_update_fn     update;
  pgr_rocket_combat_observe_fn    observe;
  pgr_rocket_combat_begin_ui_fn   begin_ui;
  pgr_rocket_combat_end_ui_fn     end_ui;
  pgr_rocket_combat_tick_fn       tick;
  pgr_rocket_combat_sample_fn     sample;
  pgr_rocket_combat_next_event_fn next_event;
} pgr_rocket_combat_api_v1;

/* Hidden getter. NOT exported by the version script. The coordinator registers it in query_feature
 * under feature id 21; it answers only on an exact version match. The returned table lives for the
 * process lifetime. */
const void *pgrazor_rocket_combat_api(uint32_t version);

#ifdef __cplusplus
} /* extern "C" */
#endif

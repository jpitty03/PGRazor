#pragma once
#include <stdint.h>

// PGRazor native migration: raid / gym / Dynamax attack & dodge controller (feature id 19).
// A native port of the battle state machine in runtime/features/battles.js (battleState +
// battleTick, with battleClear / battleDropGesture / battleCapture / battleSwapBlocked /
// battleDodgeCue), plus the scope / gesture / warning / swap / charge / max-widget observations
// that the inline Gum watchers currently host.
//
// WHAT THIS MODULE IS NOT (the incremental boundary -- see docs/NATIVE_MIGRATION.md):
//   * It installs NO Dobby and NO Gum hooks. runtime/features/rocket.js ALSO attaches a Gum
//     Interceptor at the RVCHARGE ObserveBattleState entry (shared vNext charge observer); a
//     Dobby inline trampoline on that shared prologue would corrupt the Rocket site, and
//     "installing Dobby and Gum independently at the same entry is not supported". So every
//     battle hook (scope enter/exit, gesture OnEnable/SetChargeAttackButtons + legacy variants,
//     disable/destroy, raid/gym/max warnings, swap open/close, max Refresh/Cleanup, and the
//     shared ObserveBattleState + legacy Init charge captures) STAYS a JS Gum watcher in the
//     coordinator. Each watcher forwards its already-validated il2cpp object into this table via
//     observe()/gym_begin()/gym_end(). The C++ layer owns the battle state, the GC rooting, the
//     per-tick attack/dodge decision and the telemetry; it NEVER installs a hook and NEVER
//     calls JS.
//   * The gym cooldown-action log (legacyFast/legacyCharge tap -> "Gym battle") stays entirely
//     in JS; this module deliberately does NOT re-emit it (no duplicate cooldown event).
//
// Division of labour with runtime/native-battles.js:
//   * JS validates + packs every il2cpp pointer (exports, MethodInfo*, FieldInfo*, exact
//     Il2CppClass* identities, the 3 scope + 3 swap descriptors, the Active/Unset/Left/BreadAttack
//     literals) into the config and installs it (NO hooks installed by either side). It keeps the
//     existing Gum watchers, forwards each observation into observe()/gym_begin()/gym_end(),
//     drives update()/tick() from the verified Unity heartbeat, re-arms the battleMask + residual
//     lease + rocketActive gate every tick, and drains the bounded event FIFO into the exact
//     battle-* events. The gym MoveNext onLeave calls gym_end AFTER the original ran.
//   * This module owns all GC rooting (RAII, bounded, detached-before-free, refreshed after every
//     managed/GC call), the scope/mode/gesture/swap/charge/max lifetime + scope epoch, the dodge
//     and action cadence timers, the live Active-status gate, the Dynamax scope-gesture recovery,
//     the manual-picker 600 ms grace, and the bounded telemetry FIFO. CLOCK_BOOTTIME throughout.
//     No mutex is ever held across a managed call or a GC-handle call.
//
// Threading model (mirrors feature 18): tick() latches the permanent Unity identity once; every
// managed entry point (observe / gym_begin / gym_end / tick) then requires that SAME latched owner
// AND the caller's self-reported proven_tid to equal both the owner and the live OS tid. Before
// the owner is latched the managed entry points are inert no-ops. update() is the one policy call
// allowed off-thread (a foreign thread can only disarm the mask/lease). sample()/next_event() are
// pull-only. No lock is held across a managed call.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "Battles native ABI requires LP64");

enum PgrBattleFeatureInfo {
  PGR_BATTLE_FEATURE_ID  = 19,
  PGR_BATTLE_API_VERSION = 1,
  PGR_BATTLE_MAX_LIST    = 8,     // charges / maxMoves list cap (<= 8, exact source)
  PGR_BATTLE_NUM_SCOPES  = 3,     // raid / gym / Dynamax scope descriptors
  PGR_BATTLE_NUM_SWAPS   = 3,     // gym / raid / Dynamax swap-picker descriptors
  PGR_BATTLE_MAX_LEASE_MS = 2500, // residual authorization-lease cap (finite lease)
  PGR_BATTLE_EVENT_CAP   = 64,    // bounded telemetry FIFO depth (fail-closed on overflow)
  // Exact timing from runtime/features/battles.js.
  PGR_BATTLE_QUICK_MS     = 150,  // quick/fast attack cadence
  PGR_BATTLE_CHARGED_MS   = 600,  // charged attack cadence
  PGR_BATTLE_MAX_MS       = 700,  // Max attack cadence
  PGR_BATTLE_DODGE_CUE_MS = 450,  // dodge window after a warning cue
  PGR_BATTLE_DODGE_NEXT_MS = 700, // minimum gap before the next dodge cue is honoured
  PGR_BATTLE_DODGE_GAP_MS = 500,  // post-dodge action gap (r.next = now + 500)
  PGR_BATTLE_SWAP_GRACE_MS = 600, // manual-switch open/close grace
  PGR_BATTLE_QUICK_LOG_MS = 5000  // quick-action telemetry de-dupe window
};

// Battle modes (exact source: 0 raid, 1 gym, 2 Dynamax/Max).
enum PgrBattleMode { PGR_BATTLE_MODE_RAID = 0, PGR_BATTLE_MODE_GYM = 1, PGR_BATTLE_MODE_MAX = 2 };

// observe() kinds. descriptor carries the scope/swap index or the legacy / warning-mode selector.
enum PgrBattleObserve {
  PGR_BATTLE_OBS_SCOPE_ENTER = 0,  // obj=scope, descriptor=scope index (0..2); battleClear + set
  PGR_BATTLE_OBS_SCOPE_EXIT  = 1,  // obj=scope; battleClear when it is the live scope
  PGR_BATTLE_OBS_GESTURE     = 2,  // obj=gesture, descriptor=legacy(0/1); class/mode-gated capture
  PGR_BATTLE_OBS_GESTURE_DROP = 3, // obj=gesture (OnDisable/OnDestroy); drop if it is the held one
  PGR_BATTLE_OBS_CHARGE      = 4,  // obj=charge candidate, descriptor=legacy(0 RVCHARGE / 1 Init)
  PGR_BATTLE_OBS_MAX_REFRESH = 5,  // obj=max-move widget; capture when mode==Max
  PGR_BATTLE_OBS_MAX_CLEANUP = 6,  // obj=max-move widget; remove from the list
  PGR_BATTLE_OBS_SWAP_OPEN   = 7,  // obj=picker, descriptor=swap index (0..2)
  PGR_BATTLE_OBS_SWAP_CLOSE  = 8,  // obj=picker; close by object identity (descriptor ignored)
  PGR_BATTLE_OBS_WARNING     = 9   // obj=warning arg, descriptor=mode (0 raid / 2 Max)
};

// Table status codes. 0 == success.
enum PgrBattleStatus {
  PGR_BATTLE_OK               = 0,
  PGR_BATTLE_ERR_NULL_ARG     = 1,
  PGR_BATTLE_ERR_BAD_CONFIG   = 2,
  PGR_BATTLE_ERR_ALREADY_BOUND = 3,
  PGR_BATTLE_ERR_NOT_INSTALLED = 4,
  PGR_BATTLE_ERR_NO_CLOCK     = 5,
  PGR_BATTLE_ERR_BAD_TOKEN    = 6
};

// Bounded-FIFO event codes drained by next_event(). Carry only mode / status / action / reason --
// never an account identifier. Event names/reasons are reproduced by the adapter verbatim.
enum PgrBattleEventCode {
  PGR_BATTLE_EVT_NONE       = 0,
  PGR_BATTLE_EVT_READY      = 1,  // armed (not surfaced as a user event; battles.js had none)
  PGR_BATTLE_EVT_SCOPE      = 2,  // battle-scope        (id=mode, aux=active 1/0)
  PGR_BATTLE_EVT_INPUT      = 3,  // battle-input        (id=mode, aux bit0=legacy, bit1=scope-src)
  PGR_BATTLE_EVT_ENDED      = 4,  // battle-ended        (id=mode, aux=status)
  PGR_BATTLE_EVT_DODGE_CUE  = 5,  // battle-dodge-cue    (id=mode)
  PGR_BATTLE_EVT_ACTION     = 6,  // battle-action       (id=mode, aux=PgrBattleAction)
  PGR_BATTLE_EVT_SWITCH     = 7,  // battle-manual-switch(id=mode, aux=active 1/0)
  PGR_BATTLE_EVT_ERROR      = 8   // battle-error        (id=mode, aux=PgrBattleReason)
};

// battle-action kinds carried in PgrBattleEvent.aux.
enum PgrBattleAction {
  PGR_BATTLE_ACT_QUICK   = 0,  // "quick"
  PGR_BATTLE_ACT_CHARGED = 1,  // "charged"
  PGR_BATTLE_ACT_DODGE   = 2,  // "dodge"
  PGR_BATTLE_ACT_MAX     = 3   // "max-attack"
};

// battle-error reason codes carried in PgrBattleEvent.aux.
enum PgrBattleReason {
  PGR_BATTLE_REASON_INVOKE = 1  // a managed call (currentState / active / attack) faulted
};

// Per-descriptor sub-structs (fixed LP64 layout).
typedef struct pgr_battle_scope_desc {  // 16 bytes
  void   *klass;     // exact Il2CppClass* of the scope's EnterState self (a[0])
  int32_t mode;      // PgrBattleMode
  int32_t reserved;  // 0
} pgr_battle_scope_desc;

typedef struct pgr_battle_swap_desc {  // 24 bytes
  void   *klass;     // exact Il2CppClass* of the swap-picker owner (open a[0])
  void   *show;      // MethodInfo* -> bool (get_IsOpen / get_IsShowing) for the real-visibility re-query
  int32_t mode;      // PgrBattleMode this picker belongs to
  int32_t reserved;  // 0
} pgr_battle_swap_desc;

// Install-time configuration (fixed LP64 layout, 512 bytes). Every pointer is an already-validated
// il2cpp export / MethodInfo* / FieldInfo* / Il2CppClass*; the native layer re-checks and rejects
// any null. See the static_asserts in battles.cpp for the authoritative offsets/sizes.
typedef struct pgr_battle_config_v1 {
  uint32_t version;  // == PGR_BATTLE_API_VERSION
  uint32_t size;     // == sizeof(pgr_battle_config_v1)

  // il2cpp runtime exports (pointer-sized ABI).                                   offset
  void *gchandle_new;         // void* (void* obj, int pinned) -> handle              8
  void *gchandle_get_target;  // void* (void* handle)                                16
  void *gchandle_free;        // void  (void* handle)                                24
  void *object_get_class;     // void* (void* obj)                                   32
  void *object_unbox;         // void* (void* boxed)                                 40
  void *runtime_invoke;       // void* (MethodInfo*, void* obj, void** params, void** exc) 48
  void *field_get_value;      // void  (void* obj, FieldInfo*, void* out)            56

  // Managed MethodInfo* invoked via runtime_invoke (never hooked).                  offset
  void *m_current_state;  // combat director get_CurrentState -> BattleState           64
  void *m_active;         // UnityEngine.Behaviour get_isActiveAndEnabled -> bool      72
  void *m_press;          // UnityEngine.UI.Button Press -> void (Max attack)          80
  void *m_interactable;   // UnityEngine.UI.Selectable IsInteractable -> bool          88
  void *m_alive;          // UnityEngine.Object op_Implicit(Object) -> bool [static]   96
  void *m_dodge;          // modern gesture dodge(Direction) -> void                  104
  void *m_legacy_dodge;   // legacy gesture dodge(object, EventArgs) -> void          112
  void *m_raid_fast;      // raid quick attack() -> void (no args)                    120
  void *m_max_fast;       // Max quick attack(object, EventArgs) -> void              128
  void *m_legacy_fast;    // legacy quick attack(object, EventArgs) -> void           136
  void *m_legacy_charge;  // legacy ChargeAttackButtonTapped -> void                  144
  void *m_legacy_charged; // legacy get_IsCharged -> bool                             152
  void *m_vnext_charge;   // vNext ChargeAttackButtonTapped -> void                   160
  void *m_vnext_charged;  // vNext get_IsCharged -> bool                              168
  void *m_vnext_visible;  // vNext get_IsVisible -> bool                              176

  // FieldInfo* read via field_get_value (NEVER a raw offset).                        offset
  void *f_director;       // modern gesture -> ICombatDirector (ref)                  184
  void *f_battle_status;  // BattleState.BattleStatus (int32 enum)                    192
  void *f_max_gesture;    // Dynamax scope -> scope-owned gesture (ref)               200
  void *f_legacy_flick;   // legacy gesture -> horizontalFlickGesture (ref)           208
  void *f_legacy_tap;     // legacy gesture -> tapGesture (ref)                       216
  void *f_legacy_button;  // legacy charge -> attackButton (ref)                      224
  void *f_max_button;     // max-move widget -> button (ref)                          232
  void *f_max_type;       // max-move widget -> ActiveBreadMoveType (int32 enum)      240
  void *f_vnext_enabled;  // vNext base -> FastAttackInputEnabled (bool, present==1)  248
  void *f_vnext_tap;      // vNext base -> tapGesture (ref)                           256
  void *f_vnext_button;   // vNext charge -> attackButton (ref)                       264
  void *f_gym_warning_state; // gym MoveNext iterator -> state field (int32)          272

  // Exact Il2CppClass* identities.                                                   offset
  void *class_battlestate;    // BattleState (currentState result exact)             280
  void *class_legacy_charge;  // legacy charge widget (9308)                         288
  void *class_max_gesture;    // Dynamax gesture (31285)                             296
  void *class_raid_gesture;   // modern raid/gym gesture (31499)                     304
  void *class_legacy_gesture; // legacy gesture (9182)                               312
  void *class_max_widget;     // max-move widget (31280)                             320
  void *class_rvcharge;       // vNext charge (RVCHARGE)                             328
  void *class_raid_warning;   // raid warning arg (31623)                            336
  void *class_max_warning;    // Max warning arg (31263)                             344
  void *class_gym_iterator;   // gym warning MoveNext iterator                       352
  void *class_bool;           // System.Boolean (boxed-bool unbox check)             360

  // Descriptor arrays (bounded; copied verbatim at install).                         offset
  pgr_battle_scope_desc scopes[PGR_BATTLE_NUM_SCOPES];  // 3 * 16 = 48                368
  pgr_battle_swap_desc  swaps[PGR_BATTLE_NUM_SWAPS];    // 3 * 24 = 72                416

  // Validated scalar literals.                                                       offset
  int32_t status_active;  // BattleStatus.Active                                     488
  int32_t status_unset;   // BattleStatus.Unset                                      492
  int32_t dodge_left;     // Direction.Left (modern dodge enum value)                496
  int32_t max_attack;     // BreadMoveType.BreadAttack                               500
  uint32_t reserved0;     // must be 0                                               504
  uint32_t reserved1;     // must be 0                                               508
} pgr_battle_config_v1;    // 512 bytes

// Pull-only telemetry snapshot (64 bytes). Safe from any thread; never frees.
typedef struct pgr_battle_sample_v1 {
  uint32_t version;       // == PGR_BATTLE_API_VERSION
  uint32_t size;          // == sizeof(pgr_battle_sample_v1)
  uint32_t scope_active;  // a battle scope is held (1) / absent (0)
  int32_t  mode;          // current mode (-1 none)
  uint32_t failed;        // encounter fault latch (cleared when the scope changes)
  uint32_t swap_blocked;  // a manual picker is open / in its grace window
  uint32_t charges;       // live charge-candidate root count
  uint32_t max_moves;     // live max-move root count
  uint64_t action_serial; // bumped on each attack/dodge issued
  uint64_t scope_serial;  // bumped on each scope enter
  uint32_t fault_serial;  // bumped on each managed-invoke fault
  uint32_t dropped_events;// FIFO overflow count (fail-closed: blocks new attacks until drained)
  uint32_t owned_roots;   // durable GC roots + active frames (coordinator finish guard)
  uint32_t reserved;      // 0
} pgr_battle_sample_v1;

// Bounded-FIFO event record (16 bytes). Drained in strict order by next_event().
typedef struct pgr_battle_event_v1 {
  uint32_t code;      // PgrBattleEventCode
  int32_t  id;        // mode (or 0)
  int32_t  aux;       // active / legacy+src / status / action / reason
  int32_t  reserved;  // 0
} pgr_battle_event_v1;

/* install(config): validate + publish the config once. A repeat of the identical config is OK; a
 * different one returns ALREADY_BOUND. BAD_CONFIG on header/size/pointer/literal/descriptor
 * problems; NO_CLOCK if CLOCK_BOOTTIME is unavailable. Installs NO hooks. */
typedef int (*pgr_battle_install_fn)(const pgr_battle_config_v1 *config);

/* update(mask, residual_lease_ms, rocket_active, proven_tid): refresh the per-tick policy gate --
 * the battleMask (exact source bits), the residual lease (clamped to PGR_BATTLE_MAX_LEASE_MS and
 * turned into an absolute CLOCK_BOOTTIME deadline) and the Rocket mutual-exclusion flag. A foreign
 * thread (once the owner is latched) can only disarm. Any thread. */
typedef int (*pgr_battle_update_fn)(uint32_t mask, uint64_t residual_lease_ms,
                                    uint32_t rocket_active, uint64_t proven_tid);

/* observe(kind, obj, descriptor, proven_tid): forward one Gum-watcher observation (see
 * PgrBattleObserve). Performs the exact-class identity checks itself; a mismatch is a silent
 * no-op. Owner thread only (inert otherwise). */
typedef int (*pgr_battle_observe_fn)(uint32_t kind, void *obj, int32_t descriptor,
                                     uint64_t proven_tid);

/* gym_begin(obj, proven_tid): the gym-warning MoveNext onEnter. Returns a nonzero token iff the
 * current mode is gym, obj is exactly the gym iterator, and its state field is 0 or 1 (eligible);
 * else 0. The token binds the current scope epoch. */
typedef uint64_t (*pgr_battle_gym_begin_fn)(void *obj, uint64_t proven_tid);

/* gym_end(token, ret_is_zero, proven_tid): the matching onLeave, called AFTER the original ran.
 * Consumes the token once; arms the gym dodge cue iff the token matches, its epoch is unchanged
 * (same scope) and the original returned false (ret_is_zero != 0). A stale / replayed / cross-scope
 * token is rejected. */
typedef int (*pgr_battle_gym_end_fn)(uint64_t token, int ret_is_zero, uint64_t proven_tid);

/* tick(proven_tid, stopping): one owner-thread battleTick pass -- Dynamax gesture recovery, the
 * live Active-status gate, the manual-picker grace, then the dodge / Max / charged / quick decision
 * under the mask + lease + rocket gate. A final clock/lease/epoch/rocket re-check precedes EVERY
 * attack/dodge mutation. stopping != 0 clears the battle (deferred to the outermost frame if a
 * nested tick arrives inside a managed getter). Off the proven thread it is a no-op. */
typedef int (*pgr_battle_tick_fn)(uint64_t proven_tid, int stopping);

/* sample(out): copy the telemetry snapshot. Any thread; never frees. */
typedef int (*pgr_battle_sample_fn)(pgr_battle_sample_v1 *out);

/* next_event(out): dequeue one FIFO event (returns 1) or report empty (returns 0). Owner thread. */
typedef int (*pgr_battle_next_event_fn)(pgr_battle_event_v1 *out);

// Ordered, read-only table (8-byte header + 8 function pointers = 72 bytes).
typedef struct pgr_battle_api_v1 {
  uint32_t version;  // == PGR_BATTLE_API_VERSION
  uint32_t size;     // == sizeof(pgr_battle_api_v1)
  pgr_battle_install_fn    install;
  pgr_battle_update_fn     update;
  pgr_battle_observe_fn    observe;
  pgr_battle_gym_begin_fn  gym_begin;
  pgr_battle_gym_end_fn    gym_end;
  pgr_battle_tick_fn       tick;
  pgr_battle_sample_fn     sample;
  pgr_battle_next_event_fn next_event;
} pgr_battle_api_v1;

/* Hidden getter. NOT exported by the version script. The coordinator registers it in query_feature
 * under feature id 19; it answers only on an exact version match. The returned table lives for the
 * process lifetime. */
const void *pgrazor_battles_api(uint32_t version);

#ifdef __cplusplus
} /* extern "C" */
#endif

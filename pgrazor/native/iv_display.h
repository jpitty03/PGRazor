#pragma once
#include <stdint.h>

// Feature 15. Native in-game ENCOUNTER / INVENTORY IV display: a full C++ port of the
// IV-display controller of diagnostics/quick-ball.js (the encounterLabel() helper at
// ~line 353, the captureIv / renderIv / dropIv / ivTick machinery at ~lines 379-430,
// and the two display hook blocks -- SetPokemonUI at ~622 and PokemonInfoPanel.DoUpdate
// at ~673). It overlays "<CP> · Lv <level> · <color>IV <pct>%</color>" onto the
// game's own CP label, snapshotting the authentic label/style so disabling restores it
// exactly. It performs NO automatic encounter/catch behaviour and never changes which
// Pokemon is shown: it only re-styles and re-texts an existing UI.Text the game populated.
//
// Two methods are hooked (ONLY these two), mirroring the two JS Interceptors:
//   * EncounterNameplate.SetPokemonUI  void(self, IMapPokemon, WeatherCondition, MethodInfo*)
//   * PokemonInfoPanel.DoUpdate        void(self, PokemonProto, MethodInfo*)
// WeatherCondition is a managed enum value type: it arrives by value in an integer
// register and is declared int32_t here, forwarded verbatim. Each original is chained
// exactly once with the unmodified arguments. The game's own CP text is snapshotted in
// the onLeave half (AFTER the original ran) so the native CP prefix is the real label.
//
// GameMaster level resolution (never hooked) is done through runtime_invoke:
//   * GameMasterDataExt_PlayerLevelSettings.GetPokemonLevelOfCpMultiplier(gm, mul) [static]
//   * GameMasterDataExt_PlayerLevelSettings.CpMultiplierAtLevel(gm, level)         [static]
// The CP-multiplier half-level search matches the JS exactly (lower, then lower+0.5 via
// the quadratic-mean comparison with the 0.00002 tolerance). Unknown IV/level renders as
// '--', never zero.
//
// Independent catch / inventory settings: a catch overlay and a bounded set (max 8) of
// inventory-panel overlays are tracked separately, each with its own enable flag and
// fault latch. Owned labels are wrapped in <b></b>; the native CP prefix (the game's own
// label) is kept as the start of every overlay. The optional UnityEngine.UI.Outline
// snapshot/apply/restore is owned-panel only and fails locally (it never faults the rest).
//
// Loader contract (matches the other native features):
//   root.PGRazorNativeFeature(15, 1, 64) -> query_feature(15,1) -> pgrazor_iv_display_api(1).
// A null return means the pilot/metadata is absent (the feature stays dormant, never a
// second Gum/JS fallback). Any install/update error after selection fails closed.
//
// External-observer (shared) mode. The game's EncounterNameplate.SetPokemonUI is ALSO the
// address at which runtime/features/vgp.js attaches its own Gum Interceptor, so a Dobby
// trampoline cannot be placed there without colliding. When config.external_catch_observer
// is 1 the native layer does NOT install its own setup hook (the owned PokemonInfoPanel hook
// is still a Dobby hook); instead the adapter attaches a global Gum Interceptor at the same
// address and drives the catch pass through the begin_catch/end_catch pair. begin_catch runs
// the onEnter half (claim, root the args, drop the previous overlay, announce the encounter)
// and returns a monotonic nonzero token; Gum runs the original; end_catch runs the onLeave
// half (capture from the refreshed roots) for the matching token, then releases the frame.
// external_catch_observer == 0 (the default) preserves the standalone Dobby-hooked behaviour,
// and the Dobby hook_setup is itself refactored through the same begin/end internals so both
// paths exercise identical logic. VGP's own Gum observer at the address stays compatible
// (multiple Gum listeners coexist); the catch observer never calls the original (Gum does).
//
// Ownership / concurrency (enforced in iv_display.cpp; same guarantees as map_tap.cpp
// and buddy_feeding.cpp):
//   * LP64 only; GC handles are pointer-sized.
//   * Immutable config published once under the install mutex; the two original
//     trampolines are atomically published (a racing early call waits for them). A failed
//     attempt is latched and already-published trampolines are retained (no Gum fallback).
//   * The permanent Unity owner is latched once from a genuine on-thread hook observation
//     whose tid matches proven_tid, separate from the per-pass active depth. A foreign
//     thread can chain originals but performs no managed/GC work and never steals the
//     owner; a wrong-thread update can only disable.
//   * No mutex is ever held across a managed call, a GC-handle call, or an original.
//   * Durable overlay records (text root + optional outline root) live across ticks and
//     are created/restored/freed only on the owner thread. disable()/update() only clear
//     the gate -- they never free; reentrant disable during a setter/getter defers the
//     restore/free to the next owner-thread tick. stop() restores every owned record even
//     when the lease has expired. A snapshot/apply/restore error drops only that record
//     (freeing its roots), never forgetting unrelated records or roots.
//   * CLOCK_BOOTTIME residual lease <= 2500 ms, re-checked after the managed getters and
//     root allocations immediately before any UI mutation.
//   * Pull-only telemetry (serials + last label buffers); the native layer never calls JS.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "IvDisplay native ABI requires LP64");

enum PgrIvDisplayFeatureInfo {
  PGR_IV_FEATURE_ID = 15,
  PGR_IV_API_VERSION = 1,
  PGR_IV_CONFIG_VERSION = 1,
  PGR_IV_SAMPLE_VERSION = 1,
  PGR_IV_MAX_LEASE_MS = 2500,
  PGR_IV_MAX_OWNED = 8,     // bounded inventory-panel overlays; oldest evicted first
  PGR_IV_LABEL_CAP = 256,   // UTF-8 label buffer (telemetry + stored snapshot/overlay)
  PGR_IV_REFRESH_MS = 250,  // ivTick throttle, mirrors the JS 250 ms cadence
  PGR_IV_LEVEL_UNKNOWN = -0x7fffffff  // sentinel for '--' level (adapter emits null)
};

// install()/update()/tick()/stop()/sample()/disable() status codes.
enum PgrIvDisplayStatus {
  PGR_IV_OK = 0,
  PGR_IV_ERR_INVALID_ARG = -1,
  PGR_IV_ERR_BAD_VERSION = -2,
  PGR_IV_ERR_BAD_SIZE = -3,
  PGR_IV_ERR_MISSING_PTR = -4,
  PGR_IV_ERR_DUPLICATE_TARGET = -5,
  PGR_IV_ERR_NOT_INSTALLED = -6,
  PGR_IV_ERR_HOOK_SETUP = -7,
  PGR_IV_ERR_HOOK_OWNED = -8,
  PGR_IV_ERR_RECONFIG_MISMATCH = -9,
  PGR_IV_ERR_CLOCK_FAILURE = -10
};

// Per-channel notice kind, surfaced through the sample so the adapter can re-emit the
// exact JS event. LABEL -> iv-label / iv-owned-label; PENDING -> iv-*-pending; UNAVAIL ->
// iv-*-unavailable (reason code gives the detail).
enum PgrIvNotice {
  PGR_IV_NOTICE_NONE = 0,
  PGR_IV_NOTICE_LABEL = 1,
  PGR_IV_NOTICE_PENDING = 2,
  PGR_IV_NOTICE_UNAVAILABLE = 3
};

// Reason codes for PENDING/UNAVAILABLE notices and outline failures.
enum PgrIvReason {
  PGR_IV_REASON_NONE = 0,
  PGR_IV_REASON_ENCOUNTER_ID_MISMATCH = 1,  // iv-pending encounter-id-mismatch
  PGR_IV_REASON_STATS_NOT_POPULATED = 2,    // iv-(owned-)pending stats-not-populated
  PGR_IV_REASON_POKEMON_CLASS = 3,
  PGR_IV_REASON_TEXT_CLASS = 4,
  PGR_IV_REASON_RESPONSE_CLASS = 5,
  PGR_IV_REASON_STRING = 6,
  PGR_IV_REASON_INVOKE_EXCEPTION = 7,
  PGR_IV_REASON_GCHANDLE = 8,
  PGR_IV_REASON_BOX_MISMATCH = 9,
  PGR_IV_REASON_CLOCK = 10,
  PGR_IV_REASON_OWNED_IDENTITY = 11,
  PGR_IV_REASON_OUTLINE_UNAVAILABLE = 12,  // outline snapshot/apply failed (local only)
  PGR_IV_REASON_RENDER = 13
};

// Fault bits in PgrIvDisplaySample.fault.
enum PgrIvFaultBits {
  PGR_IV_FAULT_CATCH = 1,
  PGR_IV_FAULT_OWNED = 2
};

// Fixed LP64 config: 8-byte header + 77 pointers (616) + two int32 enum literals + two
// uint32 mode words (external_catch_observer + reserved) = 640.
// Packed by the JS adapter after its metadata validation. Every pointer is an already
// validated il2cpp export, MethodInfo*, hook address, Il2CppClass* or FieldInfo*. The
// native layer re-checks the header and rejects any null required pointer. The outline
// group (class_get_type, type_get_object, the nine m_outline_* methods and the outline/
// color/vector2 classes) is all-or-nothing: if any is null the outline overlay is simply
// disabled and the rest of the feature works.
struct PgrIvDisplayConfig {
  uint32_t version;  // == PGR_IV_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrIvDisplayConfig)

  // il2cpp runtime exports (indices 0..12 in the adapter's flat array).
  void *gchandle_new;            // void* (void* obj, int pinned)
  void *gchandle_get_target;     // void* (void* handle)
  void *gchandle_free;           // void  (void* handle)
  void *object_get_class;        // void* (void* obj)
  void *object_unbox;            // void* (void* boxed)
  void *runtime_invoke;          // void* (MethodInfo*, void* obj, void** params, void** exc)
  void *field_get_value;         // void  (void* obj, FieldInfo*, void* out)
  void *class_is_assignable_from; // bool (void* klass, void* oklass)
  void *class_get_type;          // void* (void* klass)              [outline only]
  void *type_get_object;         // void* (void* type) -> System.Type [outline only]
  void *string_new;              // void* (const char* utf8)
  void *string_length;           // int   (void* str)
  void *string_chars;            // uint16_t* (void* str)

  // Managed MethodInfo* invoked via runtime_invoke (never hooked).
  void *m_get_level;             // GetPokemonLevelOfCpMultiplier(gm, single) -> int [static]
  void *m_get_multiplier;        // CpMultiplierAtLevel(gm, int) -> single          [static]
  void *m_set_text;              // UI.Text.set_text(String)
  void *m_set_rich;              // UI.Text.set_supportRichText(bool)
  void *m_set_font;              // UI.Text.set_fontSize(int)
  void *m_set_fit;               // UI.Text.set_resizeTextForBestFit(bool)
  void *m_set_overflow;          // UI.Text.set_horizontalOverflow(HorizontalWrapMode)
  void *m_set_alignment;         // UI.Text.set_alignment(TextAnchor)
  void *m_get_font;              // UI.Text.get_fontSize() -> int
  void *m_get_text;              // UI.Text.get_text() -> String
  void *m_get_rich;              // UI.Text.get_supportRichText() -> bool
  void *m_get_fit;               // UI.Text.get_resizeTextForBestFit() -> bool
  void *m_get_overflow;          // UI.Text.get_horizontalOverflow() -> HorizontalWrapMode
  void *m_get_alignment;         // UI.Text.get_alignment() -> TextAnchor
  void *m_alive;                 // UnityEngine.Object.op_Implicit(Object) -> bool [static]
  void *m_outline_gameobject;    // Component.get_gameObject() -> GameObject  [outline]
  void *m_outline_getcomponent;  // GameObject.GetComponent(System.Type)      [outline]
  void *m_outline_addcomponent;  // GameObject.AddComponent(System.Type)      [outline]
  void *m_outline_getenabled;    // Behaviour.get_enabled() -> bool           [outline]
  void *m_outline_setenabled;    // Behaviour.set_enabled(bool)               [outline]
  void *m_outline_colorget;      // Shadow.get_effectColor() -> Color         [outline]
  void *m_outline_colorset;      // Shadow.set_effectColor(Color)             [outline]
  void *m_outline_distanceget;   // Shadow.get_effectDistance() -> Vector2    [outline]
  void *m_outline_distanceset;   // Shadow.set_effectDistance(Vector2)        [outline]

  // Dobby hook targets (ONLY these two are hooked).
  void *setup_address;           // EncounterNameplate.SetPokemonUI
  void *owned_update_address;    // PokemonInfoPanel.DoUpdate

  // Exact / assignable Il2CppClass* identities.
  void *class_plate;             // EncounterNameplate (SetPokemonUI self exact)
  void *class_map_pokemon;       // IMapPokemon (assignable-from the sender)
  void *class_wildmap;           // WildMapPokemon (map exact)
  void *class_pokemon;           // PokemonProto (exact)
  void *class_text;              // UnityEngine.UI.Text (assignable-from the label)
  void *class_encounter_out;     // EncounterOutProto (exact)
  void *class_wild_proto;        // WildPokemonProto (exact)
  void *class_tappable;          // TappableEncounterProto (exact)
  void *class_panel;             // PokemonInfoPanel (DoUpdate self exact)
  void *class_string;            // System.String (get_text result exact)
  void *class_int;               // System.Int32 (box check)
  void *class_bool;              // System.Boolean (box check)
  void *class_single;            // System.Single (box check)
  void *class_hwm;               // UnityEngine.HorizontalWrapMode (box check)
  void *class_ta;                // UnityEngine.TextAnchor (box check)
  void *class_outline;           // UnityEngine.UI.Outline (exact)   [outline]
  void *class_color;             // UnityEngine.Color (box, 16 bytes) [outline]
  void *class_vector2;           // UnityEngine.Vector2 (box, 8 bytes) [outline]

  // Il2CppFieldInfo* read via field_get_value.
  void *f_plate_text;            // EncounterNameplate.cpText -> UI.Text
  void *f_plate_gm;              // EncounterNameplate.dsqf -> IGameMasterData
  void *f_map_wild;              // WildMapPokemon.eluo -> WildPokemonProto
  void *f_map_response;          // WildMapPokemon.elup -> EncounterOutProto
  void *f_map_tappable;          // WildMapPokemon.elus -> TappableEncounterProto
  void *f_wild_lat;              // WildPokemonProto.Latitude (double)
  void *f_wild_lon;              // WildPokemonProto.Longitude (double)
  void *f_wild_pokemon;          // WildPokemonProto.Pokemon -> PokemonProto
  void *f_wild_id;               // WildPokemonProto.EncounterId (uint64)
  void *f_response_wild;         // EncounterOutProto.Pokemon -> WildPokemonProto
  void *f_tappable_pokemon;      // TappableEncounterProto.Pokemon -> PokemonProto
  void *f_cp;                    // PokemonProto.cp_ (int32)
  void *f_atk;                   // PokemonProto.individualAttack_ (int32)
  void *f_def;                   // PokemonProto.individualDefense_ (int32)
  void *f_sta;                   // PokemonProto.individualStamina_ (int32)
  void *f_cpmul;                 // PokemonProto.cpMultiplier_ (single)
  void *f_addcpmul;              // PokemonProto.additionalCpMultiplier_ (single)
  void *f_panel_cptext;          // PokemonInfoPanel.cpValueText -> UI.Text
  void *f_panel_displayed;       // PokemonInfoPanel.<PokemonProto>k__BackingField
  void *f_panel_gm;              // PokemonInfoPanel.egfu -> IGameMasterData

  // Enum literal values (after validation in JS).
  int32_t overflow_value;        // HorizontalWrapMode.Overflow
  int32_t alignment_value;       // TextAnchor.MiddleCenter

  // Mode words (validated in iv_display.cpp). external_catch_observer selects the shared mode
  // described in the file header: 0 (default) installs the Dobby setup hook; 1 skips it and
  // the adapter's Gum observer drives begin_catch/end_catch. reserved must be 0.
  uint32_t external_catch_observer;  // 0 standalone (Dobby setup hook) | 1 external Gum observer
  uint32_t reserved;                 // must be 0
};

static_assert(sizeof(PgrIvDisplayConfig) == 640,
              "PgrIvDisplayConfig must remain 640 bytes on LP64");

// Fixed LP64 telemetry snapshot (656 bytes). Pull-only; the native layer never calls JS.
// The adapter drives events off the serial counters (like buddy_feeding) and reads the
// two label buffers for the iv-label / iv-owned-label event text.
struct PgrIvDisplaySample {
  uint32_t version;            // == PGR_IV_SAMPLE_VERSION
  uint32_t size;               // == sizeof(PgrIvDisplaySample)
  uint64_t catch_serial;       // bumps on each catch notice (label/pending/unavailable)
  uint64_t owned_serial;       // bumps on each owned notice
  uint64_t position_serial;    // bumps on each resolved encounter position
  uint64_t encounter_serial;   // bumps on each eligible SetPokemonUI (cooldown-encounter)
  uint64_t encounter_id;       // last resolved wild EncounterId
  uint64_t last_tid;           // owner tid of the last notice
  double latitude;             // last resolved encounter latitude (|lat| <= 90)
  double longitude;            // last resolved encounter longitude (|lon| <= 180)
  uint32_t owned_root_count;   // durable record roots + active frame depth (finish guard)
  uint32_t catch_active;       // 1 while a catch overlay record is present
  uint32_t owned_count;        // current inventory overlay records (0..8)
  uint32_t fault;              // PgrIvFaultBits
  uint32_t catch_notice;       // PgrIvNotice for the latest catch notice
  int32_t catch_level_half;    // level*2, or PGR_IV_LEVEL_UNKNOWN ('--')
  int32_t catch_attack;        // IV 0..15, or -1 when unknown/invalid
  int32_t catch_defense;
  int32_t catch_stamina;
  int32_t catch_cp;            // CP, or -1 when unknown/invalid
  uint32_t catch_reason;       // PgrIvReason for pending/unavailable
  uint32_t owned_notice;
  int32_t owned_level_half;
  int32_t owned_attack;
  int32_t owned_defense;
  int32_t owned_stamina;
  int32_t owned_cp;
  uint32_t owned_reason;
  char catch_label[PGR_IV_LABEL_CAP];  // full "<CP> · Lv .. · <color>IV ..%" UTF-8
  char owned_label[PGR_IV_LABEL_CAP];
};

static_assert(sizeof(PgrIvDisplaySample) == 656,
              "PgrIvDisplaySample must remain 656 bytes on LP64");

// Fixed LP64 table: 8-byte header + 7 function pointers = 64 bytes.
struct PgrIvDisplayApi {
  uint32_t version;  // == PGR_IV_API_VERSION
  uint32_t size;     // == sizeof(PgrIvDisplayApi)
  int (*install)(const PgrIvDisplayConfig *config);
  // Arm/clear the independent catch/owned gates and the residual lease. Any thread; a
  // wrong-thread update can only disable, never reassign the owner.
  int (*update)(int catch_enabled, int owned_enabled, uint64_t remaining_lease_ms,
                uint64_t proven_tid);
  // One owner-thread refresh pass (ivTick). stopping != 0 restores and frees every record
  // regardless of the lease. Off the owner thread it is a no-op (never frees).
  int (*tick)(uint64_t proven_tid, int stopping);
  int (*sample)(PgrIvDisplaySample *out_sample);
  // Clear both gates immediately (any thread). Never frees a record; the next owner-thread
  // tick performs the deferred restore/free.
  int (*disable)(void);
  // External-observer (shared) catch pass; valid only when external_catch_observer == 1 (else
  // returns 0 / not-installed). begin_catch runs the onEnter half on the proven owner thread:
  // it validates classes/owner/non-reentrancy/fault, roots self+map into the single bounded
  // owner-thread frame, drops the previous overlay, announces the encounter and returns a
  // monotonic nonzero token (0 == not claimed; the caller must NOT call end_catch then). It
  // never calls the original -- the Gum observer runs it. active_depth is held until the
  // matching end_catch. end_catch completes the pass identified by (token, proven_tid) AFTER
  // the original ran: it refreshes the rooted args, captures the overlay, frees the frame roots
  // and only then releases active_depth. A mismatched/duplicate token or a foreign thread is
  // rejected without stealing, clearing or freeing the live frame. No mutex is held across the
  // managed/GC/original work and no JS callback is made from C++.
  // refreshed_args receives [self,map] after managed teardown, for Gum argument forwarding.
  uint64_t (*begin_catch)(void *self, void *map_pokemon, uint64_t proven_tid, void **refreshed_args);
  int (*end_catch)(uint64_t token, uint64_t proven_tid);
};

static_assert(sizeof(PgrIvDisplayApi) == 64,
              "PgrIvDisplayApi must remain a 64-byte version-1 table");

// Standard feature getter. Returns the static const 64-byte table when `version` == 1,
// else NULL. The coordinator registers it in query_feature as case 15.
const void *pgrazor_iv_display_api(uint32_t version);

#ifdef __cplusplus
}
#endif

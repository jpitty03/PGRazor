#pragma once
#include "game_bindings.h"
#include "runtime_metadata.h"
#include <cstdint>

// Feature 55 / API 2: compiled port of diagnostics/quick-ball.js prepare(module, api, domain)
// structural metadata discovery+validation (source lines 109..327). It is built on feature47's
// reusable primitives (pgrazor_runtime_metadata_api(1): the 31 fixed il2cpp metadata exports) PLUS a
// small, explicitly typed set of EXTRA il2cpp primitives this module owns and calls itself
// (field_static_get_value, type_is_byref, class_get_fields, and the GC-handle export addresses the
// pointer-ABI fingerprint inspects). With those extras every metadata DECISION prepare() made is now
// compiled here -- enum-literal static VALUE reads (success/outcomes/iv literals), bare-class
// value-type size/align, type_is_byref, the GetBool/GetInteger(Int32) overload disambiguation, the
// SchedulerStateMachine current-state field scan, pointerHandleAbi, and every optional-export gate.
// The resolver returns stable, opaque, versioned handles; a thin JS adapter maps them back into the
// legacy prepare() `p` shape. No hook, no Gum/Dobby, no GC handle creation, no runtime_invoke, no
// mutation -- only metadata reflection + the read-only code fingerprints the source performed.
//
// STILL OUT OF SCOPE (runtime, not metadata; see the contract, note (e)): clock()/monotonic clock,
// all runtime_invoke / gchandle_new|get_target|free CALLS, the Gum/Dobby hooks and install() logic.
// pointerHandleAbi here only READS the gchandle export code (it never creates a handle), exactly as
// the source's read-only fingerprint did before any handle function was called.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "QuickballMetadata native ABI requires LP64");

enum PgrQuickballMetaInfo {
  PGR_QB_FEATURE_ID   = 55,
  PGR_QB_API_VERSION  = 2,   // bumped: config now carries the typed EXTRA primitives / export gates
  PGR_QB_CFG_VERSION  = 2,
  PGR_QB_OUT_VERSION  = 2,
  PGR_QB_REASON_MAX   = 120, // prepare() part()/outcomes reason .slice(0,120)
  PGR_QB_OUTCOME_MAX  = 6    // LITERALS: CatchError/Success/Escape/Flee/Missed/ItemReplacement
};

// Precise negative classifications beyond the feature47 PgrMetaStatus set (returned by resolve()).
enum PgrQuickballStatus {
  PGR_QB_NOT_READY = 1, // mandatory classes/domain assemblies not ready; output cleared, retry
  PGR_QB_ERR_EXTRA_MISSING = -34,  // extra config null/bad-version OR a mandatory extra primitive null
  PGR_QB_ERR_GC_POINTER    = -35,  // pointerHandleAbi fingerprint mismatch (gc-pointer-abi, fatal)
  PGR_QB_ERR_SUCCESS_LIT   = -36   // CatchSuccess static literal structural/read fault (fatal)
};

// Optional-export presence bits (PgrQuickballExtra.present). The host sets a bit iff it resolved the
// export non-null; these mirror prepare()'s OPTIONAL map + the type_get_object findExportByName gate.
enum PgrQuickballExport {
  PGR_QB_EXP_OBJECT_NEW            = 1u << 0,
  PGR_QB_EXP_FIELD_SET_VALUE       = 1u << 1,
  PGR_QB_EXP_OBJECT_GET_VIRTUAL    = 1u << 2,
  PGR_QB_EXP_STRING_NEW            = 1u << 3,
  PGR_QB_EXP_STRING_LENGTH         = 1u << 4,
  PGR_QB_EXP_STRING_CHARS          = 1u << 5,
  PGR_QB_EXP_TYPE_GET_OBJECT       = 1u << 6,
  PGR_QB_EXP_TYPE_IS_BYREF         = 1u << 7,
  PGR_QB_EXP_CLASS_GET_FIELDS      = 1u << 8
};

#ifdef __cplusplus
}  // extern "C"

namespace pgr_quickball_schema {

// Optional sub-feature groups, in the exact order prepare() returns them (CORE==the mandatory path).
// OUTCOMES maps to the top-level p.outcomes; TRACE_PARAMS to p.trace.params. The nested trace parts
// (identity/name/params) and the iv/ivText/ivOwned dependency chain mirror the source part() tree.
enum Group {
  GROUP_CORE = 0,
  GROUP_OUTCOMES,
  GROUP_TRACE,
  GROUP_TRACE_IDENTITY,
  GROUP_TRACE_NAME,
  GROUP_TRACE_PARAMS,
  GROUP_THROWS,
  GROUP_ALWAYSHIT,
  GROUP_ARTHROWS,
  GROUP_MAPTAP,
  GROUP_IV,
  GROUP_IVTEXT,
  GROUP_IVOWNED,
  GROUP_IVOUTLINE,
  GROUP_COUNT
};
inline constexpr uint32_t kGroupCount = GROUP_COUNT;

// Dependency: a group whose dep is unavailable reports available=0 reason "<dep>-unavailable",
// mirroring prepare()'s ivText->iv, ivOwned->ivText, alwaysHit/arThrows->throws, identity/name/
// params->trace. OUTCOMES depends only on the (always-present when resolve succeeds) CORE enum_class.
inline constexpr int kGroupDep[kGroupCount] = {
  -1,            // CORE
  -1,            // OUTCOMES
  -1,            // TRACE
  GROUP_TRACE,   // TRACE_IDENTITY
  GROUP_TRACE,   // TRACE_NAME
  GROUP_TRACE,   // TRACE_PARAMS
  -1,            // THROWS
  GROUP_THROWS,  // ALWAYSHIT
  GROUP_THROWS,  // ARTHROWS
  -1,            // MAPTAP
  -1,            // IV
  GROUP_IV,      // IVTEXT
  GROUP_IVTEXT,  // IVOWNED
  -1,            // IVOUTLINE
};
inline constexpr const char *kGroupName[kGroupCount] = {
  "core", "outcomes", "trace", "trace.identity", "trace.name", "trace.params", "throws", "alwaysHit",
  "arThrows", "mapTap", "iv", "ivText", "ivOwned", "ivOutline"};

// Class indices (declaration order == output positional order). 0..7 mandatory; 8..41 optional.
enum ClassIndex {
  C_BALL = 0, C_CAPTURE, C_RESPONSE, C_ANIMATOR, C_OBJECT, C_SYNCCTX, C_SINGLE, C_BOOL,
  C_ACTION, C_SCHEDULER, C_ARVALUES, C_MAPPOKEMON, C_WILDMAP, C_WILDPROTO, C_ENCOUNTEROUT, C_TAPPABLE,
  C_NAMEPLATE, C_PK, C_MAPPK, C_LEVELS, C_UITEXT, C_PANEL, C_OUTLINE, C_SHADOW, C_BEHAVIOUR, C_COMPONENT,
  C_GAMEOBJECT, C_COLOR, C_VECTOR2, C_TEXTANCHOR, C_HWRAP, C_GROUND, C_MAPVIEW, C_LATLNG, C_VEC3,
  C_THROW, C_ENCOUNTER, C_INFO, C_INT32, C_STRING, C_RAC, C_OVERRIDE, C_CLASS_END
};
inline constexpr uint32_t kMandatoryClassCount = 8;

struct ClassSpec {const char *name_space, *name, *canonical; uint32_t optional;};
struct FieldSpec {
  uint32_t group, owner; const char *name; uint64_t offset;
  uint32_t kind, value_size, forbidden_mask, required_mask; const char *type_name;
};
// rva==0 && prologue==nullptr => no code pin (metadata-only method). return/param nullptr => skip.
struct MethodSpec {
  uint32_t group, owner; const char *name, *return_type; uint32_t return_match;
  uint32_t param_count; const char *param0, *param1; uint32_t is_static;
  uint64_t rva; const char *prologue;
};
// Bare-class value-type ABI check (class_is_valuetype + class_value_size, no field involved).
struct ValueSizeSpec { uint32_t group, klass, size, align; };

inline constexpr ClassSpec kClasses[] = {
  {"Niantic.Holoholo.Encounter", "Pokeball", "Niantic.Holoholo.Encounter.Pokeball", 0},                        // 0 BALL
  {"Niantic.Holoholo.Encounter", "EncounterCaptureState", "Niantic.Holoholo.Encounter.EncounterCaptureState", 0}, // 1 CAPTURE
  {"Holoholo.Rpc", "CatchPokemonOutProto", "Holoholo.Rpc.CatchPokemonOutProto", 0},                            // 2 RESPONSE
  {"UnityEngine", "Animator", "UnityEngine.Animator", 0},                                                      // 3 ANIMATOR
  {"UnityEngine", "Object", "UnityEngine.Object", 0},                                                          // 4 OBJECT
  {"UnityEngine", "UnitySynchronizationContext", "UnityEngine.UnitySynchronizationContext", 0},                // 5 SYNCCTX
  {"System", "Single", "System.Single", 0},                                                                    // 6 SINGLE
  {"System", "Boolean", "System.Boolean", 0},                                                                  // 7 BOOL
  {"System", "Action", "System.Action", 1},                                                                    // 8 ACTION
  {"Niantic.Holoholo", "SchedulerStateMachine", "Niantic.Holoholo.SchedulerStateMachine", 1},                  // 9 SCHEDULER
  {"Holoholo.Rpc", "ARPlusEncounterValuesProto", "Holoholo.Rpc.ARPlusEncounterValuesProto", 1},                // 10 ARVALUES
  {"Niantic.Holoholo.Map", "MapPokemon", "Niantic.Holoholo.Map.MapPokemon", 1},                                // 11 MAPPOKEMON
  {"Niantic.Holoholo.Map", "WildMapPokemon", "Niantic.Holoholo.Map.WildMapPokemon", 1},                        // 12 WILDMAP
  {"Holoholo.Rpc", "WildPokemonProto", "Holoholo.Rpc.WildPokemonProto", 1},                                    // 13 WILDPROTO
  {"Holoholo.Rpc", "EncounterOutProto", "Holoholo.Rpc.EncounterOutProto", 1},                                  // 14 ENCOUNTEROUT
  {"Holoholo.Rpc", "TappableEncounterProto", "Holoholo.Rpc.TappableEncounterProto", 1},                        // 15 TAPPABLE
  {"Niantic.Holoholo.Encounter", "EncounterNameplate", "Niantic.Holoholo.Encounter.EncounterNameplate", 1},    // 16 NAMEPLATE
  {"Holoholo.Rpc", "PokemonProto", "Holoholo.Rpc.PokemonProto", 1},                                            // 17 PK
  {"Niantic.Holoholo.Map", "IMapPokemon", "Niantic.Holoholo.Map.IMapPokemon", 1},                              // 18 MAPPK
  {"Niantic.Holoholo", "GameMasterDataExt_PlayerLevelSettings", "Niantic.Holoholo.GameMasterDataExt_PlayerLevelSettings", 1}, // 19 LEVELS
  {"UnityEngine.UI", "Text", "UnityEngine.UI.Text", 1},                                                        // 20 UITEXT
  {"Niantic.Holoholo.UI.Pokemon", "PokemonInfoPanel", "Niantic.Holoholo.UI.Pokemon.PokemonInfoPanel", 1},      // 21 PANEL
  {"UnityEngine.UI", "Outline", "UnityEngine.UI.Outline", 1},                                                  // 22 OUTLINE
  {"UnityEngine.UI", "Shadow", "UnityEngine.UI.Shadow", 1},                                                    // 23 SHADOW
  {"UnityEngine", "Behaviour", "UnityEngine.Behaviour", 1},                                                    // 24 BEHAVIOUR
  {"UnityEngine", "Component", "UnityEngine.Component", 1},                                                     // 25 COMPONENT
  {"UnityEngine", "GameObject", "UnityEngine.GameObject", 1},                                                  // 26 GAMEOBJECT
  {"UnityEngine", "Color", "UnityEngine.Color", 1},                                                            // 27 COLOR
  {"UnityEngine", "Vector2", "UnityEngine.Vector2", 1},                                                        // 28 VECTOR2
  {"UnityEngine", "TextAnchor", "UnityEngine.TextAnchor", 1},                                                  // 29 TEXTANCHOR
  {"UnityEngine", "HorizontalWrapMode", "UnityEngine.HorizontalWrapMode", 1},                                  // 30 HWRAP
  {"Niantic.Holoholo.Map", "GroundTapHandler", "Niantic.Holoholo.Map.GroundTapHandler", 1},                    // 31 GROUND
  {"Niantic.Holoholo.Map", "MapViewHandler", "Niantic.Holoholo.Map.MapViewHandler", 1},                        // 32 MAPVIEW
  {"Niantic", "LatLng", "Niantic.LatLng", 1},                                                                  // 33 LATLNG
  {"UnityEngine", "Vector3", "UnityEngine.Vector3", 1},                                                        // 34 VEC3
  {"Niantic.Holoholo.Encounter", "PokeballThrow", "Niantic.Holoholo.Encounter.PokeballThrow", 1},              // 35 THROW
  {"Niantic.Holoholo.Encounter", "EncounterState", "Niantic.Holoholo.Encounter.EncounterState", 1},            // 36 ENCOUNTER
  {"UnityEngine", "AnimatorStateInfo", "UnityEngine.AnimatorStateInfo", 1},                                    // 37 INFO
  {"System", "Int32", "System.Int32", 1},                                                                      // 38 INT32
  {"System", "String", "System.String", 1},                                                                    // 39 STRING
  {"UnityEngine", "RuntimeAnimatorController", "UnityEngine.RuntimeAnimatorController", 1},                     // 40 RAC
  {"UnityEngine", "AnimatorOverrideController", "UnityEngine.AnimatorOverrideController", 1},                   // 41 OVERRIDE
};
inline constexpr uint32_t kClassCount = sizeof(kClasses) / sizeof(kClasses[0]);
static_assert(kClassCount == C_CLASS_END, "class table matches ClassIndex enum");

// Field KINDs / masks use runtime_metadata.h (ANY=0, VALUE=2, ENUM_I32=3). Default forbidden mask is
// 0x50 (check()/field()); the static dsjv field uses forbidden 0x40 + required 0x10 (static,non-literal).
inline constexpr FieldSpec kFields[] = {
  // --- CORE (mandatory) ---
  {GROUP_CORE, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_ANIMATOR_NAME, PGR_GAME_QUICKBALL_METADATA_H_ANIMATOR_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "UnityEngine.Animator"},
  {GROUP_CORE, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_HOLDINGBREADBALL_K__BACKINGFIELD_NAME, PGR_GAME_QUICKBALL_METADATA_H_HOLDINGBREADBALL_K__BACKINGFIELD_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Boolean"},
  {GROUP_CORE, C_RESPONSE, PGR_GAME_QUICKBALL_METADATA_H_STATUS_NAME, PGR_GAME_QUICKBALL_METADATA_H_STATUS_OFFSET, PGR_MD_FIELD_ENUM_I32, 4, 0x50, 0, "Holoholo.Rpc.CatchPokemonOutProto.Types.Status"},
  // --- TRACE: AnimatorStateInfo layout (0x10 + i*4) ---
  {GROUP_TRACE, C_INFO, PGR_GAME_QUICKBALL_METADATA_H_M_NAME_NAME, PGR_GAME_QUICKBALL_METADATA_H_M_NAME_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE, C_INFO, PGR_GAME_QUICKBALL_METADATA_H_M_PATH_NAME, PGR_GAME_QUICKBALL_METADATA_H_M_PATH_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE, C_INFO, PGR_GAME_QUICKBALL_METADATA_H_M_FULLPATH_NAME, PGR_GAME_QUICKBALL_METADATA_H_M_FULLPATH_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE, C_INFO, PGR_GAME_QUICKBALL_METADATA_H_M_NORMALIZEDTIME_NAME, PGR_GAME_QUICKBALL_METADATA_H_M_NORMALIZEDTIME_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Single"},
  {GROUP_TRACE, C_INFO, PGR_GAME_QUICKBALL_METADATA_H_M_LENGTH_NAME, PGR_GAME_QUICKBALL_METADATA_H_M_LENGTH_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Single"},
  {GROUP_TRACE, C_INFO, PGR_GAME_QUICKBALL_METADATA_H_M_SPEED_NAME, PGR_GAME_QUICKBALL_METADATA_H_M_SPEED_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Single"},
  {GROUP_TRACE, C_INFO, PGR_GAME_QUICKBALL_METADATA_H_M_SPEEDMULTIPLIER_NAME, PGR_GAME_QUICKBALL_METADATA_H_M_SPEEDMULTIPLIER_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Single"},
  {GROUP_TRACE, C_INFO, PGR_GAME_QUICKBALL_METADATA_H_M_TAG_NAME, PGR_GAME_QUICKBALL_METADATA_H_M_TAG_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE, C_INFO, PGR_GAME_QUICKBALL_METADATA_H_M_LOOP_NAME, PGR_GAME_QUICKBALL_METADATA_H_M_LOOP_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  // --- TRACE_IDENTITY: BALL_HASHES dsmh..dsmt (0x340 + i*4) + static dsjv ---
  {GROUP_TRACE_IDENTITY, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSQC_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSQC_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE_IDENTITY, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSQD_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSQD_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE_IDENTITY, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSQE_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSQE_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE_IDENTITY, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSQF_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSQF_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE_IDENTITY, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSQG_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSQG_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE_IDENTITY, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSQH_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSQH_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE_IDENTITY, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSQI_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSQI_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE_IDENTITY, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSQJ_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSQJ_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE_IDENTITY, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSQK_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSQK_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE_IDENTITY, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSQL_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSQL_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE_IDENTITY, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSQM_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSQM_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE_IDENTITY, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSQN_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSQN_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE_IDENTITY, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSQO_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSQO_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_TRACE_IDENTITY, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSNQ_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSNQ_OFFSET, PGR_MD_FIELD_ANY, 0, 0x40, 0x10, "System.Int32"},
  // --- THROWS: PokeballThrow layout ---
  {GROUP_THROWS, C_THROW, "BallType", 16, PGR_MD_FIELD_ANY, 0, 0x50, 0, "Holoholo.Rpc.Item"},
  {GROUP_THROWS, C_THROW, "ReticleSize", 20, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Single"},
  {GROUP_THROWS, C_THROW, "HitBullseye", 24, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Boolean"},
  {GROUP_THROWS, C_THROW, "Spinning", 25, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Boolean"},
  {GROUP_THROWS, C_THROW, "Missed", 26, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Boolean"},
  // --- ALWAYSHIT: Pokeball coroutine/state wiring ---
  {GROUP_ALWAYSHIT, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSOY_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSOY_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "Niantic.Holoholo.SchedulerStateMachine"},
  {GROUP_ALWAYSHIT, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSPT_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSPT_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "Niantic.Holoholo.SchedulerStateCoroutine"},
  {GROUP_ALWAYSHIT, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSPS_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSPS_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "Niantic.Holoholo.SchedulerStateCoroutine"},
  {GROUP_ALWAYSHIT, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_DSOX_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSOX_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "Niantic.Holoholo.Encounter.IEncounterPokemon"},
  {GROUP_ALWAYSHIT, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_HITPOKEMON_NAME, PGR_GAME_QUICKBALL_METADATA_H_HITPOKEMON_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Action"},
  {GROUP_ALWAYSHIT, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_BALLTYPE_K__BACKINGFIELD_NAME, PGR_GAME_QUICKBALL_METADATA_H_BALLTYPE_K__BACKINGFIELD_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "Holoholo.Rpc.Item"},
  // --- ARTHROWS: ARPlusEncounterValuesProto ---
  {GROUP_ARTHROWS, C_ARVALUES, PGR_GAME_QUICKBALL_METADATA_H_PROXIMITY_NAME, PGR_GAME_QUICKBALL_METADATA_H_PROXIMITY_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Single"},
  {GROUP_ARTHROWS, C_ARVALUES, PGR_GAME_QUICKBALL_METADATA_H_AWARENESS_NAME, PGR_GAME_QUICKBALL_METADATA_H_AWARENESS_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Single"},
  {GROUP_ARTHROWS, C_ARVALUES, PGR_GAME_QUICKBALL_METADATA_H_POKEMONFRIGHTENED_NAME, PGR_GAME_QUICKBALL_METADATA_H_POKEMONFRIGHTENED_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Boolean"},
  // --- MAPTAP: LatLng + Vector3 layout ---
  {GROUP_MAPTAP, C_LATLNG, "Latitude", 16, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Double"},
  {GROUP_MAPTAP, C_LATLNG, "Longitude", 24, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Double"},
  {GROUP_MAPTAP, C_VEC3, "x", 16, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Single"},
  {GROUP_MAPTAP, C_VEC3, "y", 20, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Single"},
  {GROUP_MAPTAP, C_VEC3, "z", 24, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Single"},
  // --- IV: PokemonProto / nameplate / wild-map chain ---
  {GROUP_IV, C_PK, PGR_GAME_QUICKBALL_METADATA_H_CP__NAME, PGR_GAME_QUICKBALL_METADATA_H_CP__OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_IV, C_PK, PGR_GAME_QUICKBALL_METADATA_H_INDIVIDUALATTACK__NAME, PGR_GAME_QUICKBALL_METADATA_H_INDIVIDUALATTACK__OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_IV, C_PK, PGR_GAME_QUICKBALL_METADATA_H_INDIVIDUALDEFENSE__NAME, PGR_GAME_QUICKBALL_METADATA_H_INDIVIDUALDEFENSE__OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_IV, C_PK, PGR_GAME_QUICKBALL_METADATA_H_INDIVIDUALSTAMINA__NAME, PGR_GAME_QUICKBALL_METADATA_H_INDIVIDUALSTAMINA__OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Int32"},
  {GROUP_IV, C_PK, PGR_GAME_QUICKBALL_METADATA_H_CPMULTIPLIER__NAME, PGR_GAME_QUICKBALL_METADATA_H_CPMULTIPLIER__OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Single"},
  {GROUP_IV, C_PK, PGR_GAME_QUICKBALL_METADATA_H_ADDITIONALCPMULTIPLIER__NAME, PGR_GAME_QUICKBALL_METADATA_H_ADDITIONALCPMULTIPLIER__OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Single"},
  {GROUP_IV, C_NAMEPLATE, PGR_GAME_QUICKBALL_METADATA_H_CPTEXT_NAME, PGR_GAME_QUICKBALL_METADATA_H_CPTEXT_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "UnityEngine.UI.Text"},
  {GROUP_IV, C_NAMEPLATE, PGR_GAME_QUICKBALL_METADATA_H_DSUA_NAME, PGR_GAME_QUICKBALL_METADATA_H_DSUA_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "IGameMasterData"},
  {GROUP_IV, C_WILDMAP, PGR_GAME_QUICKBALL_METADATA_H_EMBR_NAME, PGR_GAME_QUICKBALL_METADATA_H_EMBR_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "Holoholo.Rpc.WildPokemonProto"},
  {GROUP_IV, C_WILDMAP, PGR_GAME_QUICKBALL_METADATA_H_EMBS_NAME, PGR_GAME_QUICKBALL_METADATA_H_EMBS_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "Holoholo.Rpc.EncounterOutProto"},
  {GROUP_IV, C_WILDMAP, PGR_GAME_QUICKBALL_METADATA_H_EMBV_NAME, PGR_GAME_QUICKBALL_METADATA_H_EMBV_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "Holoholo.Rpc.TappableEncounterProto"},
  {GROUP_IV, C_WILDPROTO, PGR_GAME_QUICKBALL_METADATA_H_LATITUDE_NAME, PGR_GAME_QUICKBALL_METADATA_H_LATITUDE_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Double"},
  {GROUP_IV, C_WILDPROTO, PGR_GAME_QUICKBALL_METADATA_H_LONGITUDE_NAME, PGR_GAME_QUICKBALL_METADATA_H_LONGITUDE_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.Double"},
  {GROUP_IV, C_WILDPROTO, PGR_GAME_QUICKBALL_METADATA_H_POKEMON_NAME, PGR_GAME_QUICKBALL_METADATA_H_POKEMON_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "Holoholo.Rpc.PokemonProto"},
  {GROUP_IV, C_WILDPROTO, PGR_GAME_QUICKBALL_METADATA_H_ENCOUNTERID_NAME, PGR_GAME_QUICKBALL_METADATA_H_ENCOUNTERID_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "System.UInt64"},
  {GROUP_IV, C_ENCOUNTEROUT, PGR_GAME_QUICKBALL_METADATA_H_POKEMON_NAME, PGR_GAME_QUICKBALL_METADATA_H_POKEMON_OFFSET_2, PGR_MD_FIELD_ANY, 0, 0x50, 0, "Holoholo.Rpc.WildPokemonProto"},
  {GROUP_IV, C_TAPPABLE, PGR_GAME_QUICKBALL_METADATA_H_POKEMON_NAME, PGR_GAME_QUICKBALL_METADATA_H_POKEMON_OFFSET_3, PGR_MD_FIELD_ANY, 0, 0x50, 0, "Holoholo.Rpc.PokemonProto"},
  // --- IVOWNED: PokemonInfoPanel ---
  {GROUP_IVOWNED, C_PANEL, PGR_GAME_QUICKBALL_METADATA_H_CPVALUETEXT_NAME, PGR_GAME_QUICKBALL_METADATA_H_CPVALUETEXT_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "UnityEngine.UI.Text"},
  {GROUP_IVOWNED, C_PANEL, PGR_GAME_QUICKBALL_METADATA_H_POKEMONPROTO_K__BACKINGFIELD_NAME, PGR_GAME_QUICKBALL_METADATA_H_POKEMONPROTO_K__BACKINGFIELD_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "Holoholo.Rpc.PokemonProto"},
  {GROUP_IVOWNED, C_PANEL, PGR_GAME_QUICKBALL_METADATA_H_EGMN_NAME, PGR_GAME_QUICKBALL_METADATA_H_EGMN_OFFSET, PGR_MD_FIELD_ANY, 0, 0x50, 0, "IGameMasterData"},
};
inline constexpr uint32_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);
static_assert(kFieldCount == 65, "quick-ball resolves exactly 65 pinned fields (source cross-check)");

inline constexpr MethodSpec kMethods[] = {
  // --- CORE (mandatory) ---
  {GROUP_CORE, C_CAPTURE, PGR_GAME_QUICKBALL_METADATA_H_BSWF_NAME, "System.Void", PGR_MD_MATCH_EXACT, 1, "Holoholo.Rpc.CatchPokemonOutProto", nullptr, 0,
   PGR_GAME_QUICKBALL_METADATA_H_CORE_CAPTURE_BSWF_RVA, PGR_GAME_QUICKBALL_METADATA_H_CORE_CAPTURE_BSWF_PROLOGUE},
  {GROUP_CORE, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_BTQU_NAME, "Holoholo.Rpc.CatchPokemonOutProto", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0,
   PGR_GAME_QUICKBALL_METADATA_H_CORE_BALL_BTQU_RVA, PGR_GAME_QUICKBALL_METADATA_H_CORE_BALL_BTQU_PROLOGUE},
  {GROUP_CORE, C_ANIMATOR, "get_speed", "System.Single", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0,
   PGR_GAME_QUICKBALL_METADATA_H_CORE_ANIMATOR_GET_SPEED_RVA, PGR_GAME_QUICKBALL_METADATA_H_CORE_ANIMATOR_GET_SPEED_PROLOGUE},
  {GROUP_CORE, C_ANIMATOR, "set_speed", "System.Void", PGR_MD_MATCH_EXACT, 1, "System.Single", nullptr, 0,
   PGR_GAME_QUICKBALL_METADATA_H_CORE_ANIMATOR_SET_SPEED_RVA, PGR_GAME_QUICKBALL_METADATA_H_CORE_ANIMATOR_SET_SPEED_PROLOGUE},
  {GROUP_CORE, C_OBJECT, "op_Implicit", "System.Boolean", PGR_MD_MATCH_EXACT, 1, "UnityEngine.Object", nullptr, 1,
   PGR_GAME_QUICKBALL_METADATA_H_CORE_OBJECT_OP_IMPLICIT_RVA, PGR_GAME_QUICKBALL_METADATA_H_CORE_OBJECT_OP_IMPLICIT_PROLOGUE},
  {GROUP_CORE, C_SYNCCTX, "ExecuteTasks", "System.Void", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 1, 0, nullptr},
  {GROUP_CORE, C_CAPTURE, PGR_GAME_QUICKBALL_METADATA_H_BSWD_NAME, "System.Void", PGR_MD_MATCH_EXACT, 1, "Holoholo.Rpc.CatchPokemonOutProto", nullptr, 0,
   PGR_GAME_QUICKBALL_METADATA_H_CORE_CAPTURE_BSWD_RVA, PGR_GAME_QUICKBALL_METADATA_H_CORE_CAPTURE_BSWF_PROLOGUE},
  // --- TRACE ---
  {GROUP_TRACE, C_ANIMATOR, "GetCurrentAnimatorStateInfo", "UnityEngine.AnimatorStateInfo", PGR_MD_MATCH_EXACT, 1, "System.Int32", nullptr, 0, 0, nullptr},
  {GROUP_TRACE, C_ANIMATOR, "GetNextAnimatorStateInfo", "UnityEngine.AnimatorStateInfo", PGR_MD_MATCH_EXACT, 1, "System.Int32", nullptr, 0, 0, nullptr},
  {GROUP_TRACE, C_ANIMATOR, "IsInTransition", "System.Boolean", PGR_MD_MATCH_EXACT, 1, "System.Int32", nullptr, 0, 0, nullptr},
  // --- TRACE_IDENTITY ---
  {GROUP_TRACE_IDENTITY, C_ANIMATOR, "get_runtimeAnimatorController", "UnityEngine.RuntimeAnimatorController", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  {GROUP_TRACE_IDENTITY, C_ANIMATOR, "get_layerCount", "System.Int32", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  {GROUP_TRACE_IDENTITY, C_ANIMATOR, "get_parameterCount", "System.Int32", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  // --- TRACE_NAME ---
  {GROUP_TRACE_NAME, C_OBJECT, "get_name", "System.String", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  // --- THROWS ---
  {GROUP_THROWS, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_BTRL_NAME, "System.Void", PGR_MD_MATCH_EXACT, 1, "Niantic.Holoholo.Encounter.PokeballThrow", nullptr, 0,
   PGR_GAME_QUICKBALL_METADATA_H_THROWS_BALL_BTRL_RVA, PGR_GAME_QUICKBALL_METADATA_H_THROWS_BALL_BTRL_PROLOGUE},
  {GROUP_THROWS, C_ENCOUNTER, "NotifyPokeballHit", "System.Void", PGR_MD_MATCH_EXACT, 1, "Niantic.Holoholo.Encounter.PokeballThrow", nullptr, 0,
   PGR_GAME_QUICKBALL_METADATA_H_THROWS_ENCOUNTER_NOTIFYPOKEBALLHIT_RVA, PGR_GAME_QUICKBALL_METADATA_H_THROWS_ENCOUNTER_NOTIFYPOKEBALLHIT_PROLOGUE},
  // --- ALWAYSHIT ---
  {GROUP_ALWAYSHIT, C_ACTION, "Invoke", "System.Void", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  {GROUP_ALWAYSHIT, C_BALL, "OnCollisionEnter", "System.Void", PGR_MD_MATCH_EXACT, 1, "UnityEngine.Collision", nullptr, 0,
   PGR_GAME_QUICKBALL_METADATA_H_ALWAYSHIT_BALL_ONCOLLISIONENTER_RVA, PGR_GAME_QUICKBALL_METADATA_H_ALWAYSHIT_BALL_ONCOLLISIONENTER_PROLOGUE},
  {GROUP_ALWAYSHIT, C_BALL, "OnTriggerEnter", "System.Void", PGR_MD_MATCH_EXACT, 1, "UnityEngine.Collider", nullptr, 0,
   PGR_GAME_QUICKBALL_METADATA_H_ALWAYSHIT_BALL_ONTRIGGERENTER_RVA, PGR_GAME_QUICKBALL_METADATA_H_ALWAYSHIT_BALL_ONTRIGGERENTER_PROLOGUE},
  {GROUP_ALWAYSHIT, C_BALL, "Launch", "System.Void", PGR_MD_MATCH_EXACT, 1, "UnityEngine.Vector2", nullptr, 0,
   PGR_GAME_QUICKBALL_METADATA_H_ALWAYSHIT_BALL_LAUNCH_RVA, PGR_GAME_QUICKBALL_METADATA_H_ALWAYSHIT_BALL_LAUNCH_PROLOGUE},
  {GROUP_ALWAYSHIT, C_BALL, "EndDrag", "System.Void", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0,
   PGR_GAME_QUICKBALL_METADATA_H_ALWAYSHIT_BALL_ENDDRAG_RVA, PGR_GAME_QUICKBALL_METADATA_H_ALWAYSHIT_BALL_ENDDRAG_PROLOGUE},
  {GROUP_ALWAYSHIT, C_BALL, PGR_GAME_QUICKBALL_METADATA_H_BTRB_NAME, "Niantic.Holoholo.Encounter.PokeballThrow", PGR_MD_MATCH_EXACT, 1, "System.Boolean", nullptr, 0,
   PGR_GAME_QUICKBALL_METADATA_H_ALWAYSHIT_BALL_BTRB_RVA, PGR_GAME_QUICKBALL_METADATA_H_ALWAYSHIT_BALL_BTRB_PROLOGUE},
  // --- ARTHROWS: TryCapture return is checked for exact IPromise(`1)?<CatchPokemonOutProto> in resolve().
  {GROUP_ARTHROWS, C_MAPPOKEMON, "TryCapture", "Holoholo.Rpc.CatchPokemonOutProto", PGR_MD_MATCH_CONTAINS, 2,
   "Niantic.Holoholo.Encounter.PokeballThrow", "Holoholo.Rpc.ARPlusEncounterValuesProto", 0,
   PGR_GAME_QUICKBALL_METADATA_H_ARTHROWS_MAPPOKEMON_TRYCAPTURE_RVA, PGR_GAME_QUICKBALL_METADATA_H_ARTHROWS_MAPPOKEMON_TRYCAPTURE_PROLOGUE},
  // --- MAPTAP: GetGroundPosition param1 is checked for Vector3(&) + type_is_byref in resolve().
  {GROUP_MAPTAP, C_GROUND, PGR_GAME_QUICKBALL_METADATA_H_MAP_GROUND_TAP_NAME, "System.Void", PGR_MD_MATCH_EXACT, 2, "System.Object", "System.EventArgs", 0,
   PGR_GAME_QUICKBALL_METADATA_H_MAPTAP_GROUND_MAP_GROUND_TAP_RVA, PGR_GAME_QUICKBALL_METADATA_H_MAPTAP_GROUND_MAP_GROUND_TAP_PROLOGUE},
  {GROUP_MAPTAP, C_MAPVIEW, "GetGroundPosition", "System.Boolean", PGR_MD_MATCH_EXACT, 2, "UnityEngine.Vector2", "UnityEngine.Vector3", 0,
   PGR_GAME_QUICKBALL_METADATA_H_MAPTAP_MAPVIEW_GETGROUNDPOSITION_RVA, PGR_GAME_QUICKBALL_METADATA_H_MAPTAP_MAPVIEW_GETGROUNDPOSITION_PROLOGUE},
  {GROUP_MAPTAP, C_MAPVIEW, "GetWorldLocation", "Niantic.LatLng", PGR_MD_MATCH_EXACT, 1, "UnityEngine.Vector3", nullptr, 0,
   PGR_GAME_QUICKBALL_METADATA_H_MAPTAP_MAPVIEW_GETWORLDLOCATION_RVA, PGR_GAME_QUICKBALL_METADATA_H_MAPTAP_MAPVIEW_GETWORLDLOCATION_PROLOGUE},
  // --- IV ---
  {GROUP_IV, C_NAMEPLATE, "SetPokemonUI", "System.Void", PGR_MD_MATCH_EXACT, 2,
   "Niantic.Holoholo.Map.IMapPokemon", "Niantic.Rpc.GameplayWeatherProto.Types.WeatherCondition", 0,
   PGR_GAME_QUICKBALL_METADATA_H_IV_NAMEPLATE_SETPOKEMONUI_RVA, PGR_GAME_QUICKBALL_METADATA_H_IV_NAMEPLATE_SETPOKEMONUI_PROLOGUE},
  {GROUP_IV, C_LEVELS, "GetPokemonLevelOfCpMultiplier", "System.Int32", PGR_MD_MATCH_EXACT, 2, "IGameMasterData", "System.Single", 1,
   PGR_GAME_QUICKBALL_METADATA_H_IV_LEVELS_GETPOKEMONLEVELOFCPMULTIPLIER_RVA, PGR_GAME_QUICKBALL_METADATA_H_IV_LEVELS_GETPOKEMONLEVELOFCPMULTIPLIER_PROLOGUE},
  {GROUP_IV, C_LEVELS, "CpMultiplierAtLevel", "System.Single", PGR_MD_MATCH_EXACT, 2, "IGameMasterData", "System.Int32", 1,
   PGR_GAME_QUICKBALL_METADATA_H_IV_LEVELS_CPMULTIPLIERATLEVEL_RVA, PGR_GAME_QUICKBALL_METADATA_H_IV_LEVELS_CPMULTIPLIERATLEVEL_PROLOGUE},
  {GROUP_IV, C_UITEXT, "set_text", "System.Void", PGR_MD_MATCH_EXACT, 1, "System.String", nullptr, 0, 0, nullptr},
  {GROUP_IV, C_UITEXT, "set_supportRichText", "System.Void", PGR_MD_MATCH_EXACT, 1, "System.Boolean", nullptr, 0, 0, nullptr},
  {GROUP_IV, C_UITEXT, "get_fontSize", "System.Int32", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  {GROUP_IV, C_UITEXT, "set_fontSize", "System.Void", PGR_MD_MATCH_EXACT, 1, "System.Int32", nullptr, 0, 0, nullptr},
  {GROUP_IV, C_UITEXT, "set_resizeTextForBestFit", "System.Void", PGR_MD_MATCH_EXACT, 1, "System.Boolean", nullptr, 0, 0, nullptr},
  {GROUP_IV, C_UITEXT, "set_horizontalOverflow", "System.Void", PGR_MD_MATCH_EXACT, 1, "UnityEngine.HorizontalWrapMode", nullptr, 0, 0, nullptr},
  {GROUP_IV, C_UITEXT, "set_alignment", "System.Void", PGR_MD_MATCH_EXACT, 1, "UnityEngine.TextAnchor", nullptr, 0, 0, nullptr},
  // --- IVTEXT ---
  {GROUP_IVTEXT, C_UITEXT, "get_text", "System.String", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  {GROUP_IVTEXT, C_UITEXT, "get_supportRichText", "System.Boolean", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  {GROUP_IVTEXT, C_UITEXT, "get_resizeTextForBestFit", "System.Boolean", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  {GROUP_IVTEXT, C_UITEXT, "get_horizontalOverflow", "UnityEngine.HorizontalWrapMode", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  {GROUP_IVTEXT, C_UITEXT, "get_alignment", "UnityEngine.TextAnchor", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  // --- IVOWNED ---
  {GROUP_IVOWNED, C_PANEL, "DoUpdate", "System.Void", PGR_MD_MATCH_EXACT, 1, "Holoholo.Rpc.PokemonProto", nullptr, 0,
   PGR_GAME_QUICKBALL_METADATA_H_IVOWNED_PANEL_DOUPDATE_RVA, PGR_GAME_QUICKBALL_METADATA_H_IVOWNED_PANEL_DOUPDATE_PROLOGUE},
  // --- IVOUTLINE ---
  {GROUP_IVOUTLINE, C_COMPONENT, "get_gameObject", "UnityEngine.GameObject", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  {GROUP_IVOUTLINE, C_GAMEOBJECT, "GetComponent", "UnityEngine.Component", PGR_MD_MATCH_EXACT, 1, "System.Type", nullptr, 0, 0, nullptr},
  {GROUP_IVOUTLINE, C_GAMEOBJECT, "AddComponent", "UnityEngine.Component", PGR_MD_MATCH_EXACT, 1, "System.Type", nullptr, 0, 0, nullptr},
  {GROUP_IVOUTLINE, C_BEHAVIOUR, "get_enabled", "System.Boolean", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  {GROUP_IVOUTLINE, C_BEHAVIOUR, "set_enabled", "System.Void", PGR_MD_MATCH_EXACT, 1, "System.Boolean", nullptr, 0, 0, nullptr},
  {GROUP_IVOUTLINE, C_SHADOW, "get_effectColor", "UnityEngine.Color", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  {GROUP_IVOUTLINE, C_SHADOW, "set_effectColor", "System.Void", PGR_MD_MATCH_EXACT, 1, "UnityEngine.Color", nullptr, 0, 0, nullptr},
  {GROUP_IVOUTLINE, C_SHADOW, "get_effectDistance", "UnityEngine.Vector2", PGR_MD_MATCH_EXACT, 0, nullptr, nullptr, 0, 0, nullptr},
  {GROUP_IVOUTLINE, C_SHADOW, "set_effectDistance", "System.Void", PGR_MD_MATCH_EXACT, 1, "UnityEngine.Vector2", nullptr, 0, 0, nullptr},
};
inline constexpr uint32_t kMethodCount = sizeof(kMethods) / sizeof(kMethods[0]);
static_assert(kMethodCount == 51, "quick-ball has exactly 51 resolved methods (source cross-check)");

// Bare-class value-type ABI checks (quick-close boxed-abi / trace-abi / throw-value-abi / map-value-abi
// / iv-outline-value-abi): class_is_valuetype true, class_value_size == size, align == alignment.
inline constexpr ValueSizeSpec kValueSizes[] = {
  {GROUP_CORE,      C_SINGLE,  4,  4},   // boxed-abi:System.Single   (source line 177)
  {GROUP_CORE,      C_BOOL,    1,  1},   // boxed-abi:System.Boolean  (source line 177)
  {GROUP_TRACE,     C_INT32,   4,  4},   // trace-abi:System.Int32    (source line 206)
  {GROUP_TRACE,     C_INFO,    36, 4},   // trace-abi:AnimatorStateInfo (source line 210)
  {GROUP_THROWS,    C_THROW,   12, 4},   // throw-value-abi           (source line 226)
  {GROUP_MAPTAP,    C_LATLNG,  16, 8},   // map-value-abi latlng      (source line 260)
  {GROUP_MAPTAP,    C_VEC3,    12, 4},   // map-value-abi vector      (source line 260)
  {GROUP_IVOUTLINE, C_COLOR,   16, 4},   // iv-outline-value-abi color (source line 315)
  {GROUP_IVOUTLINE, C_VECTOR2, 8,  4},   // iv-outline-value-abi vec2  (source line 315)
};
inline constexpr uint32_t kValueSizeCount = sizeof(kValueSizes) / sizeof(kValueSizes[0]);

// Standalone caller-code check (prepare line 189) + the returned caller RETURN ADDRESS (line 325,
// p.caller = module.base + 0xace0e4c). The check and the returned address are DISTINCT: the check
// validates 16 bytes at 0xace0e44 with the fixed prologue; the returned caller is +8 past that start
// (0xace0e4c) and is a plain module-relative pointer the adapter re-derives, NOT a validated handle.
inline constexpr uint64_t kCallerCheckRva = PGR_GAME_QUICKBALL_METADATA_H_KCALLERCHECKRVA_RVA;
inline constexpr uint64_t kCallerCheckLen = 16;
inline constexpr const char *kCallerCheckPrologue = PGR_GAME_QUICKBALL_METADATA_H_KCALLERCHECKPROLOGUE_PROLOGUE;
inline constexpr uint64_t kCallerRva = PGR_GAME_QUICKBALL_METADATA_H_KCALLERRVA_RVA;
static_assert(kCallerRva == kCallerCheckRva + 8, "returned caller sits 8 bytes into the checked range");

// pointerHandleAbi (prepare lines 109..140): read-only fingerprint of il2cpp_gchandle_new /
// _get_target / _free. Each export is validated readable+executable and its instruction words
// compared; the first body target is reached through an AArch64 'B imm26' branch. No handle is created.
inline constexpr uint32_t kGcCreateWords[] = PGR_GAME_QUICKBALL_METADATA_H_KGCCREATEWORDS_FINGERPRINT;
inline constexpr uint32_t kGcCreateWordCount = 5;
inline constexpr uint64_t kGcCreateBranchOff = PGR_GAME_QUICKBALL_METADATA_H_KGCCREATEBRANCHOFF;    // branch(create.add(20))
inline constexpr uint64_t kGcCreateBodyOff   = PGR_GAME_QUICKBALL_METADATA_H_KGCCREATEBODYOFF; // csel x0,x9,x21,lo : full-width return
inline constexpr uint32_t kGcCreateBodyWord  = PGR_GAME_QUICKBALL_METADATA_H_KGCCREATEBODYWORD;
inline constexpr uint32_t kGcGetHead  = PGR_GAME_QUICKBALL_METADATA_H_KGCGETHEAD;   // words(getBody,[...])
inline constexpr uint64_t kGcGetTailOff = PGR_GAME_QUICKBALL_METADATA_H_KGCGETTAILOFF;
inline constexpr uint32_t kGcGetTail[] = PGR_GAME_QUICKBALL_METADATA_H_KGCGETTAIL_FINGERPRINT;
inline constexpr uint32_t kGcFreeHead = PGR_GAME_QUICKBALL_METADATA_H_KGCFREEHEAD;   // words(freeBody,[...])
inline constexpr uint64_t kGcFreeTailOff = PGR_GAME_QUICKBALL_METADATA_H_KGCFREETAILOFF;
inline constexpr uint32_t kGcFreeTail[] = PGR_GAME_QUICKBALL_METADATA_H_KGCFREETAIL_FINGERPRINT;

// CatchPokemonOutProto.Types.Status literal names (prepare line 16: LITERALS).
inline constexpr const char *kStatusEnumName = "Holoholo.Rpc.CatchPokemonOutProto.Types.Status";
inline constexpr const char *kOutcomeLiterals[PGR_QB_OUTCOME_MAX] = {
  "CatchError", "CatchSuccess", "CatchEscape", "CatchFlee", "CatchMissed", "CatchItemReplacement"};

// 80-byte il2cpp_debug_get_method_info prologue (prepare lines 56..59, 179..180).
inline constexpr unsigned char kDebugPrologue[] = PGR_GAME_QUICKBALL_METADATA_H_KDEBUGPROLOGUE_FINGERPRINT;
inline constexpr uint32_t kDebugPrologueLen = sizeof(kDebugPrologue);

}  // namespace pgr_quickball_schema

extern "C" {
#endif  // __cplusplus

// Per optional sub-feature group result. reason is empty on available==1.
struct PgrQuickballMetaGroup {
  uint32_t available;
  uint32_t reserved;
  char reason[PGR_QB_REASON_MAX];
};

// Extra typed il2cpp primitives this module OWNS and either calls (field_static_get_value /
// type_is_byref / class_get_fields) or fingerprints the code of (gchandle_* addresses), plus the
// optional-export presence bitmask (PgrQuickballExport). Supplied by the JS adapter. Versioned.
struct PgrQuickballExtra {
  uint32_t version, size;
  void *field_static_get_value;    // void (void* field, void* out) ; MANDATORY (success literal)
  void *type_is_byref;             // bool (void* type)              ; optional (mapTap)
  void *class_get_fields;          // void* (void* klass, void** it) ; optional (alwaysHit)
  const void *gchandle_new;        // export CODE address (read-only); MANDATORY (pointerHandleAbi)
  const void *gchandle_get_target; // export CODE address (read-only); MANDATORY
  const void *gchandle_free;       // export CODE address (read-only); MANDATORY
  uint32_t present;                // PgrQuickballExport bits (host-resolved export presence)
  uint32_t reserved;
};

// Config mirrors feature53's shape/semantics plus the typed extra block (API 2).
struct PgrQuickballMetaConfig {
  uint32_t version, size;
  PgrMetaBindConfig binding;
  void *thread_current;
  uint64_t owner_tid;
  const PgrQuickballExtra *extra;
};

#ifdef __cplusplus
// Fixed positional output (an ABI; never reordered). classes/conflict are kClassCount; fields/methods/
// method_code track the schema tables; the scalar literal values and the two trace.params / one
// alwaysHit discovered handles follow, then the per-group availability array.
struct PgrQuickballMetaOutput {
  uint32_t version, size;
  const void *classes[pgr_quickball_schema::kClassCount];
  uint32_t class_conflict[pgr_quickball_schema::kClassCount];
  const void *enum_class;
  const void *fields[pgr_quickball_schema::kFieldCount];
  const void *methods[pgr_quickball_schema::kMethodCount];
  const void *method_code[pgr_quickball_schema::kMethodCount];  // compiled addr for pinned methods, else null
  const void *trace_params_bool;   // GetBool(Int32) MethodInfo (valid iff groups[TRACE_PARAMS].available)
  const void *trace_params_int;    // GetInteger(Int32) MethodInfo
  const void *alwayshit_current;   // SchedulerStateMachine current-state FieldInfo (iff ALWAYSHIT avail)
  int32_t success;                 // CatchSuccess literal value (mandatory)
  int32_t outcomes_values[PGR_QB_OUTCOME_MAX];  // LITERALS order (valid iff groups[OUTCOMES].available)
  int32_t iv_overflow_value;       // HorizontalWrapMode.Overflow (valid iff groups[IV].available)
  int32_t iv_alignment_value;      // TextAnchor.MiddleCenter
  PgrQuickballMetaGroup groups[pgr_quickball_schema::kGroupCount];
};

struct PgrQuickballMetaApi {
  uint32_t version, size, config_size, output_size;
  int (*resolve)(const PgrQuickballMetaConfig *, PgrQuickballMetaOutput *);
};
static_assert(sizeof(PgrQuickballExtra) == 64, "LP64 extra block");
static_assert(sizeof(PgrQuickballMetaConfig) == 96, "LP64 config = 88 (feature53) + extra ptr");
static_assert(sizeof(PgrQuickballMetaGroup) == 128, "group = 8 + 120 reason");
static_assert(sizeof(PgrQuickballMetaApi) == 24, "LP64 API");
static_assert(sizeof(PgrQuickballMetaOutput) == 3712, "fixed positional output ABI (adapter mirrors)");
#endif

const void *pgrazor_quickball_metadata_api(uint32_t version);

#ifdef __cplusplus
}
#endif

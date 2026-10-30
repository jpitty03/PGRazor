#pragma once
#include <stddef.h>
#include <stdint.h>

// Scoped owner-thread admission check, rechecked after managed reflection.
void pgr_smart_set_admission_guard(bool (*guard)(void *), void *context);

// Smart Background Catcher native helper (FEATURE_PROPOSALS module2). This is an INTERNAL C++ helper
// surface (NOT an exported pgrazor_native_api feature table): the VGP controller (feature 25/26) and the
// existing VGP actions/aux modules link these functions directly and drive them from the ALREADY-PROVEN
// Unity owner thread. The coordinator integrates the controller plumbing and the Java transport; this
// module only:
//   * parses the Smart Catcher settings out of the native transport string (index 7, exact format below),
//   * decides, after a CONFIRMED encounter, which ball/berry to use (shiny / IV / CP / level / candy
//     precedence) subject to the actual ItemBag stock, and
//   * builds + issues the single berry UseItemEncounter RPC (reusing the normal SendRpc generic path),
//     handing OWN GC handles back to the controller which polls the promise through vgp_results.
//
// WHAT THIS MODULE IS / IS NOT (same conservatism as vgp_aux.cpp / vgp_actions.cpp):
//   * It installs NO hook, spawns NO thread, keeps NO automation loop, and NEVER calls back into JS. Every
//     managed call runs on the caller-proven Unity owner thread only. plan() issues NO managed mutation
//     (it is read-only: field reads + optional GameMaster level lookup getters). berry() issues EXACTLY
//     ONE game action (SendRpc) per call, fail-closed, never retried.
//   * It does NOT own the VGP gate. The controller proves the owner tid and supplies a residual foreground
//     lease that must already fold in transport-enabled freshness AND vgpAllowed: a zero/expired lease is a
//     refusal. berry() clamps the lease, turns it into a fresh CLOCK_BOOTTIME deadline and RE-CHECKS it
//     immediately before the invoke (after all reflection), so an op whose reflection outran the lease is
//     refused without issuing. It never mutates inventory directly.
//   * Master Balls are NEVER selected. The ball falls back only among regular balls that are actually in
//     stock; the berry is optional and dropped when absent. "no regular ball in stock" is reported
//     DISTINCTLY from an unavailable/data error.
//
// RUNTIME METADATA SOURCING. The helper caches a once-published, validated binding (bind()). The
// controller assembles it from the pgrazor_runtime_bootstrap_api(54) snapshot (the il2cpp runtime +
// reflection exports) and from pgrazor_runtime_metadata_api(47) validators (resolve_class / validate_field
// / validate_method) for every game class/field/method listed below -- i.e. the field offsets and
// method signatures are validated by feature 47, NOT blindly trusted. This leaf does not self-resolve the
// translated-ARM64 metadata (that would be an unverifiable Houdini claim and is untestable in a fixture);
// it re-checks the published header + rejects any null mandatory pointer and performs the managed work.
// See the coordinator note returned with this task for the exact ns/name/offset/type descriptors.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "SmartCatch native ABI requires LP64");

enum PgrSmartInfo {
  PGR_SMART_BINDING_VERSION = 1,
  PGR_SMART_MAX_SPECIES     = 200,   // candy-species cap (mirrors SmartCatchSettings.java)
  PGR_SMART_CONFIG_MAX      = 4096,  // transport index-7 byte ceiling (fail closed above)
  PGR_SMART_MAX_LEASE_MS    = 2500,  // residual foreground lease ceiling (matches VGP siblings)
  PGR_SMART_SENDRPC_ARITY   = 5,     // SendRpc<Req,Out>(rpcId, request, 0, 10000, 0)
  PGR_SMART_TYPE_ARGS       = 2,     // SendRpc generic arity (request + out)
  PGR_SMART_ARRAY_DATA_OFF  = 32     // Il2CppArray<T> first element byte offset on LP64
};

// Holoholo.Rpc.Item enum values this module uses directly (written into the proto Item enum i32 field and
// queried against ItemBag stock). Balls 1..3 are the only selectable balls; MASTER (4) is never chosen.
enum PgrSmartItem {
  PGR_SMART_BALL_POKE   = 1,
  PGR_SMART_BALL_GREAT  = 2,
  PGR_SMART_BALL_ULTRA  = 3,
  PGR_SMART_BALL_MASTER = 4,  // reference only; never selected
  PGR_SMART_BERRY_RAZZ        = 701,
  PGR_SMART_BERRY_NANAB       = 703,
  PGR_SMART_BERRY_PINAP       = 705,
  PGR_SMART_BERRY_GOLDEN_RAZZ = 706,
  PGR_SMART_BERRY_SILVER_PINAP = 708
};

// plan() / berry() / berry_success() status domain. Positive are distinct outcomes; negatives are
// data/runtime faults the controller surfaces as "smart unavailable" (never as "no balls").
enum PgrSmartStatus {
  PGR_SMART_ISSUED        = 1,   // berry(): the single SendRpc was issued; OWN handles transferred
  PGR_SMART_OK            = 0,   // plan(): decision written (may be .enabled==false); berry(): refused
  PGR_SMART_NO_BALLS      = 2,   // plan(): enabled but NO regular ball in stock (distinct from error)
  PGR_SMART_NOT_SUCCESS   = 3,   // berry_success(): response is UseItemEncounterOutProto but not Success
  PGR_SMART_ERR_NULL_ARG      = -1,
  PGR_SMART_ERR_BAD_VERSION   = -2,
  PGR_SMART_ERR_BAD_SIZE      = -3,
  PGR_SMART_ERR_MISSING_PTR   = -4,
  PGR_SMART_ERR_NOT_BOUND     = -6,
  PGR_SMART_ERR_RECONFIG      = -9,
  PGR_SMART_ERR_CLOCK_FAILURE = -10,
  PGR_SMART_ERR_IDENTITY      = -11, // a class identity check failed or a required object was null
  PGR_SMART_ERR_GCHANDLE      = -12, // a GC root / Type[] allocation failed (fail closed)
  PGR_SMART_ERR_MANAGED       = -13, // a managed invoke threw
  PGR_SMART_ERR_REFLECT       = -14, // SendRpc / MakeGenericMethod resolution/validation failed
  PGR_SMART_ERR_SHAPE         = -15  // a boxed return had the wrong class / unexpected shape
};

// Parsed Smart Catcher settings (transport index 7). Format, exactly as emitted by
// SmartCatchSettings.config():
//   "1;enabled;ball;berry;premium;highCp;highLevel;grade;curve;<comma-separated species ids>"
// Field 0 is the schema tag "1". A malformed string, a wrong tag, an out-of-range field or an over-long
// buffer yields a SAFE DISABLED result (enabled == false): smart is never run on an unparseable config.
struct PgrSmartSettings {
  int32_t  enabled;        // 0/1
  int32_t  ball;           // 1..3 (default 1)
  int32_t  berry;          // 0 or {701,703,705,706,708} (default 0 == Off)
  int32_t  premium;        // 0/1 (shiny or IV>=90% -> Ultra + Golden Razz)
  int32_t  high_cp;        // 0..10000
  int32_t  high_level;     // 0..50
  int32_t  grade;          // 0..3 (throw rating)
  int32_t  curve;          // 0/1
  uint32_t species_count;  // number of valid candy species ids parsed (<= PGR_SMART_MAX_SPECIES)
  int32_t  species[PGR_SMART_MAX_SPECIES];  // ascending, de-duplicated National Dex ids (1..2000)
};

// The decision handed back to the controller after a confirmed encounter. ball is always a regular ball
// (1..3) that is in stock on PGR_SMART_OK with enabled==true; berry is 0 when Off or out of stock.
struct PgrSmartDecision {
  bool     enabled;   // false -> smart disabled; the controller keeps its own default catch strategy
  int      ball;      // selected regular ball id (1..3), in stock; 0 only when enabled==false
  int      berry;     // selected berry id (0 == none / out of stock)
  int      grade;     // throw rating 0..3 (passthrough from settings)
  bool     curve;     // curveball (passthrough from settings)
  uint64_t delay_ms;  // reserved for controller-driven throw pacing (0: no helper-imposed delay)
};

// The once-published, validated binding. The controller fills every mandatory pointer from the feature 54
// bootstrap snapshot (runtime + reflection exports) and feature 47 validators (classes/fields/methods);
// the optional GameMaster group (level lookup) may be left all-null, in which case plan() never claims a
// level and the high-level threshold is skipped. bind() re-checks version/size and rejects any null
// mandatory pointer. Fixed LP64 order; see smart_catch.cpp for the validation loop bounds.
struct PgrSmartBinding {
  uint32_t version;  // == PGR_SMART_BINDING_VERSION
  uint32_t size;     // == sizeof(PgrSmartBinding)

  // --- il2cpp runtime exports (11) --------------------------------------------------
  void *gc_new;                 // uintptr_t (void* obj, int pinned)
  void *gc_get;                 // void*     (uintptr_t handle)
  void *gc_free;                // void      (uintptr_t handle)
  void *object_get_class;       // void*     (void* obj)
  void *object_new;             // void*     (void* klass)
  void *object_unbox;           // void*     (void* boxed)
  void *runtime_invoke;         // void*     (MethodInfo*, void* obj, void** params, void** exc)
  void *field_get_value;        // void      (void* obj, FieldInfo*, void* out)
  void *field_set_value;        // void      (void* obj, FieldInfo*, void* value)        [value field]
  void *field_set_value_object; // void      (void* obj, FieldInfo*, void* value)        [ref field barrier]
  void *array_new;              // void*     (void* elem_class, uintptr_t len)

  // --- il2cpp reflection exports (9) ------------------------------------------------
  void *class_get_method_from_name; // MethodInfo* (klass, name, argc)
  void *method_is_generic;          // bool   (MethodInfo*)
  void *method_get_param_count;     // uint32 (MethodInfo*)
  void *method_get_object;          // void*  (MethodInfo*, klass) -> Reflection.MethodInfo
  void *class_get_type;             // void*  (klass) -> Il2CppType*
  void *type_get_object;            // void*  (Il2CppType*) -> System.Type
  void *gc_wbarrier_set_field;      // void   (void* obj, void** field, void* value)
  void *method_get_from_reflection; // MethodInfo* (void* reflectionMethod)
  void *method_is_inflated;         // bool   (MethodInfo*)

  // --- exact Il2CppClass* identities (12) -------------------------------------------
  void *class_wildmap;        // Niantic.Holoholo.Map.WildMapPokemon (target exact class)
  void *class_pokemon_proto;  // Holoholo.Rpc.PokemonProto (WildMapPokemon.eluq)
  void *class_display_proto;  // Holoholo.Rpc.PokemonDisplayProto (PokemonProto.pokemonDisplay_)
  void *class_item_bag;       // IItemBag concrete (SendRpc owner source + GetItemCount receiver)
  void *class_rpc;            // VGP rpc service (SendRpc owner; item-bag rpc field class)
  void *class_use_item_req;   // Holoholo.Rpc.UseItemEncounterProto (request)
  void *class_use_item_out;   // Holoholo.Rpc.UseItemEncounterOutProto (response)
  void *class_type;           // System.Type (Type[] element class)
  void *class_int;            // System.Int32 (boxed GetItemCount result / level)
  void *class_bool;           // System.Boolean (boxed shiny, if read boxed)
  void *class_ulong;          // System.UInt64 (boxed WildMapPokemon.get_EncounterId)
  void *class_string;         // System.String (WildMapPokemon.get_SpawnPointId)

  // --- validated FieldInfo* (15) ----------------------------------------------------
  void *f_map_pokemon;    // WildMapPokemon.eluq -> PokemonProto (read ONLY after encounter success)
  void *f_proto_id;       // PokemonProto.pokemonId_ (int, species / National Dex id)
  void *f_proto_cp;       // PokemonProto.cp_ (int)
  void *f_proto_atk;      // PokemonProto.individualAttack_ (int)
  void *f_proto_def;      // PokemonProto.individualDefense_ (int)
  void *f_proto_sta;      // PokemonProto.individualStamina_ (int)
  void *f_proto_cpmul;    // PokemonProto.cpMultiplier_ (float)
  void *f_proto_addcpmul; // PokemonProto.additionalCpMultiplier_ (float)
  void *f_proto_display;  // PokemonProto.pokemonDisplay_ -> PokemonDisplayProto
  void *f_display_shiny;  // PokemonDisplayProto.shiny_ (bool)
  void *f_req_item;       // UseItemEncounterProto.Item (enum i32)
  void *f_req_encid;      // UseItemEncounterProto.EncounterId (ulong)
  void *f_req_spawn;      // UseItemEncounterProto.SpawnPointGuid (string, ref write barrier)
  void *f_out_status;     // UseItemEncounterOutProto.Status (enum i32)
  void *f_bag_rpc;        // IItemBag rpc-service field (cynr; class == class_rpc)

  // --- validated MethodInfo* (4) ----------------------------------------------------
  void *m_get_encounter_id; // WildMapPokemon.get_EncounterId -> ulong (boxed)
  void *m_get_spawn_id;     // WildMapPokemon.get_SpawnPointId -> string
  void *m_item_count;       // IItemBag.GetItemCount(Item) -> int (boxed)
  void *m_req_ctor;         // UseItemEncounterProto .ctor()

  // --- OPTIONAL GameMaster level group (5; all-null disables level) -----------------
  void *class_gm;           // GameMaster data class (f_bag_gm field class)
  void *class_single;       // System.Single (boxed cp-multiplier)
  void *f_bag_gm;           // IItemBag GameMaster field (nullable; absent -> no level)
  void *m_gm_level;         // GameMaster.<level-for-multiplier>(float) -> int (boxed)
  void *m_gm_mult;          // GameMaster.<multiplier-for-level>(int) -> float (boxed)

  // --- validated literals -----------------------------------------------------------
  int32_t rpc_id;           // the UseItemEncounter RPC method id passed to SendRpc
  int32_t status_success;   // UseItemEncounterOutProto.Status value meaning Success
};

// ----- PURE helpers (no runtime; safe off-thread, used directly by the fixture) -----

// Parse the transport index-7 string into *out. Returns PGR_SMART_OK (0) with a SAFE result on any input
// (malformed -> out->enabled==0), or PGR_SMART_ERR_NULL_ARG when out is null. len is the byte length
// (the string need not be NUL-terminated); a len above PGR_SMART_CONFIG_MAX yields a disabled result.
int pgr_smart_parse_settings(const char *text, size_t len, PgrSmartSettings *out);

// Pure decision precedence over the extracted encounter stats. iv_sum is attack+defense+stamina (0..45),
// iv_valid says the three IVs were populated (each 0..15). level_half is the half-level (level*2), or a
// negative value when the level is unknown (never fabricated). species_id is the National Dex id. Fills
// the DESIRED ball/berry/grade/curve (stock is applied later by plan()). enabled follows settings.enabled.
void pgr_smart_decide(const PgrSmartSettings *s, bool shiny, int iv_sum, bool iv_valid, int cp,
                      int level_half, int species_id, PgrSmartDecision *out);

// ----- RUNTIME helpers (owner thread only, after bind) ------------------------------

// Publish the validated binding once for the process lifetime. A repeat of the identical binding is OK;
// a different one is PGR_SMART_ERR_RECONFIG. Validates header/size and rejects any null mandatory pointer.
int pgr_smart_bind(const PgrSmartBinding *binding);

// Plan the ball/berry for a CONFIRMED encounter. `target` is the rooted WildMapPokemon, `itemBag` the
// rooted IItemBag; `owner` is the proven Unity owner tid. Reads WildMapPokemon.eluq (PokemonProto) ONLY
// (exact class checked), derives shiny/IV/CP/(optional level)/species, decides, then resolves actual
// stock: the selected ball or a cheaper in-stock regular ball (never Master); the berry is dropped when
// Off or out of stock. Read-only: issues no managed mutation. Returns PGR_SMART_OK (decision written; may
// be disabled), PGR_SMART_NO_BALLS (enabled, no regular ball), PGR_SMART_OK with enabled==false when
// off-thread/not-bound (refused), or a negative data/runtime fault. *out is always initialized.
int pgr_smart_plan(void *target, void *itemBag, uint64_t owner, PgrSmartDecision *out);

// Build + issue the single berry UseItemEncounter RPC. `itemId` must be a valid berry (701/703/705/706/708).
// `leaseMs` is the residual foreground lease (clamped to PGR_SMART_MAX_LEASE_MS, re-checked after
// reflection and immediately before the invoke; 0/expired -> refuse). On success writes the OWN GC handles
// the controller adopts and frees: *promiseHandle roots the returned SendRpc promise (poll it through
// vgp_results ENCOUNTER phase); *reflectionHandle roots the closed SendRpc Reflection.MethodInfo and MUST
// outlive the poll. Returns PGR_SMART_ISSUED (1), PGR_SMART_OK (0 refused: gate/thread/lease/not-bound), or
// a negative error (PGR_SMART_ERR_* -- treated as uncertain: NEVER retried). *promiseHandle and
// *reflectionHandle are always initialized to 0; no inventory is mutated directly.
int pgr_smart_berry(void *target, void *itemBag, int32_t itemId, uint64_t owner, uint64_t leaseMs,
                    uint64_t *promiseHandle, uint64_t *reflectionHandle);

// Validate a resolved SendRpc response: returns PGR_SMART_ISSUED-like 1 when `response` is EXACTLY
// UseItemEncounterOutProto AND its Status == status_success, PGR_SMART_NOT_SUCCESS (3) when it is the
// right class but not Success, PGR_SMART_OK (0) when refused (off-thread/not-bound), or a negative error.
int pgr_smart_berry_success(void *response, uint64_t owner);

#ifdef __cplusplus
}
#endif

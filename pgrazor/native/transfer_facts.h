#pragma once
#include <stdint.h>

// Feature 35 (API 1). TRANSFER FACTS READER: a STATELESS, synchronous C++ port of the per-pokemon
// CATEGORY EXTRACTION that diagnostics/quick-close.js transferCatch performs inline (~quick-close.js
// 1731+). Given a caller-rooted pokemon + GameMaster handle and the proven caught id, it reads the IVs,
// species, size class, rare/shiny/event/background booleans, favorite and egg out of managed memory and
// returns them as a flat facts record. It does NOT decide anything: the feature-4 transfer_policy still
// runs (in JS, on these facts), native23 still owns the ReleasePokemon lifecycle, and the host keeps its
// existing source/identity gates. This reader NEVER issues a release action, even if its own checks fail.
//
// NOT owned here: NO hook, NO RPC, NO callback into JS, NO retained GC root between calls, NO cache, NO
// automation, NO policy change. Each inspect() roots its inputs + every temporary managed box, re-resolves
// every rooted object from its handle after each managed call (relocating collector), frees every root on
// EVERY exit on the calling owner thread (RAII), and transfers nothing. A busy guard refuses a reentrant
// call (e.g. one fired from inside a managed invoke). No mutex is ever held across a managed invoke.
//
// SAFETY: bad metadata NEVER authorizes a deletion. Any malformed class/box/bool/size/count, an IV outside
// 0..15, a species outside 1..2000, a missing object, or a caught-id that is zero or does not match the
// pokemon id BEFORE AND AFTER every managed call -> a NEGATIVE return and NO facts. The caller treats a
// non-success as "keep" (conservative). A closed foreground window / wrong owner thread / reentry -> 0
// (refused, NOT an authorization). Facts are written ONLY on success (1); the output is zeroed on every
// other path. The full 64-bit id is compared without any Number conversion.
//
// Division of labour with runtime/native-transfer-facts.js: the adapter validates + packs the il2cpp
// pointers (8 runtime exports, 7 exact class identities, 4 MethodInfo*, 15 FieldInfo*) and the enum
// sentinels into the 320-byte config and calls install() ONLY; the host (VGP / details / direct callers)
// invokes inspect() on the raw table, feeds the decoded flags into its existing keep/transfer decision,
// and drives native23.submit. No JS fallback when the native reader is selected but unavailable.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "TransferFacts native ABI requires LP64");

enum PgrTransferFactsInfo {
  PGR_TF_FEATURE_ID     = 35,
  PGR_TF_API_VERSION    = 1,
  PGR_TF_CONFIG_VERSION = 1,
  PGR_TF_REQUEST_VERSION = 1,
  PGR_TF_OUT_VERSION    = 1,
  PGR_TF_MAX_LEASE_MS   = 2500,  // residual foreground lease ceiling (JS allowedUntil gate)
  PGR_TF_SIZE_COUNT     = 5,     // Holoholo.Rpc.CharacterSize members {Xxs,Xs,M,Xl,Xxl}
  PGR_TF_PROTECT_SIZE   = 48,    // protect bits (xxs|xxl) that request a PokemonCharacterSize invoke
  PGR_TF_IV_MAX         = 15,    // individual value range 0..15 (reject out of range -> keep)
  PGR_TF_SPECIES_MIN    = 1,     // species 1..2000 (reject out of range -> keep)
  PGR_TF_SPECIES_MAX    = 2000,
  PGR_TF_KNOWN_ALL      = 0xFF   // all 8 category bits known on success
};

// Feature-4 bit order (shared with transfer_policy flags): rare 0, shiny 1, event 2, background 3,
// xxs 4, xxl 5, favorite 6, egg 7. The size bits (xxs/xxl) are left false when no size was requested.
enum PgrTransferFactsFlag {
  PGR_TF_FLAG_RARE       = 1u << 0,
  PGR_TF_FLAG_SHINY      = 1u << 1,
  PGR_TF_FLAG_EVENT      = 1u << 2,
  PGR_TF_FLAG_BACKGROUND = 1u << 3,
  PGR_TF_FLAG_XXS        = 1u << 4,
  PGR_TF_FLAG_XXL        = 1u << 5,
  PGR_TF_FLAG_FAVORITE   = 1u << 6,
  PGR_TF_FLAG_EGG        = 1u << 7
};

// inspect() returns 1 VALID (facts written), 0 REFUSED (window/owner/reentry -- NOT an authorization), or
// a negative error. install() returns PGR_TF_OK (0) or a negative error. A negative return is ALWAYS safe
// to treat as "keep this pokemon".
enum PgrTransferFactsStatus {
  PGR_TF_VALID             = 1,   // inspect(): facts fully written
  PGR_TF_OK                = 0,   // install OK / inspect refused (gate/busy/thread/expired) -- no facts
  PGR_TF_ERR_NULL_ARG      = -1,
  PGR_TF_ERR_BAD_VERSION   = -2,
  PGR_TF_ERR_BAD_SIZE      = -3,
  PGR_TF_ERR_MISSING_PTR   = -4,
  PGR_TF_ERR_BAD_CONFIG    = -5,  // size members not distinct, or xxs/xxl not distinct members of sizes[]
  PGR_TF_ERR_NOT_INSTALLED = -6,
  PGR_TF_ERR_RECONFIG      = -9,
  PGR_TF_ERR_CLOCK_FAILURE = -10,
  PGR_TF_ERR_IDENTITY      = -11,  // class mismatch, null required object, or caught id zero / id mismatch
  PGR_TF_ERR_GCHANDLE      = -12,  // a GC root could not be allocated (fail closed)
  PGR_TF_ERR_MANAGED       = -13,  // a managed invoke threw
  PGR_TF_ERR_MALFORMED     = -14   // non-strict bool, unknown size enum, count<0, IV/species out of range
};

// Fixed LP64 config (320 bytes): 8-byte header + 34 validated il2cpp pointers + 10 int32 sentinels. Every
// pointer is already validated by the JS adapter straight off p.transfer / p.details; the native layer
// re-checks the header, rejects any null, and validates the enum sentinels. No hook address. The field
// order matches runtime/native-transfer-facts.js exactly (indices in the comments).
struct PgrTransferFactsConfig {
  uint32_t version;  // == PGR_TF_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrTransferFactsConfig)

  // --- il2cpp runtime exports (indices 0..7) ----------------------------------------
  void *gc_new;           // 0  uintptr_t (void* obj, int pinned)
  void *gc_get;           // 1  void*     (uintptr_t handle)
  void *gc_free;          // 2  void      (uintptr_t handle)
  void *object_get_class; // 3  void*     (void* obj)
  void *runtime_invoke;   // 4  void*     (MethodInfo*, void* obj, void** params, void** exc)
  void *object_unbox;     // 5  void*     (void* boxed) -> pointer to the unboxed value
  void *field_get_value;  // 6  void      (void* obj, FieldInfo*, void* out)
  void *string_length;    // 7  int32_t   (void* string)

  // --- exact Il2CppClass* identities (indices 8..14) --------------------------------
  void *class_pokemon;    // 8  Pokemon (PK; exact inspected object class)
  void *class_display;    // 9  PokemonDisplayProto (DISPLAY; exact display field class)
  void *class_bool;       // 10 System.Boolean (exact rare/shiny box class)
  void *class_int;        // 11 System.Int32 (exact event-count box class)
  void *class_event;      // 12 origin-events collection class (exact events field class)
  void *class_character_size; // 13 Holoholo.Rpc.CharacterSize (exact size box class)
  void *class_string;     // 14 System.String (exact limited-identifier field class)

  // --- MethodInfo* invoked via runtime_invoke (indices 15..18) ----------------------
  void *m_size;       // 15 Pokemon.PokemonCharacterSize(IGameMasterData) -> CharacterSize (box)
  void *m_rare;       // 16 Pokemon.IsLegendaryOrMythicOrUltraBeast(IGameMasterData) -> Boolean (box)
  void *m_shiny;      // 17 Pokemon.get_IsShinyPokemon() -> Boolean (box)
  void *m_event_count;// 18 origin-events get_Count() -> Int32 (box)

  // --- FieldInfo* read via field_get_value (indices 19..33) -------------------------
  void *f_id;               // 19 Pokemon.id_ (System.UInt64; identity match vs caught id)
  void *f_species;          // 20 Pokemon.pokemonId_ (Int32)
  void *f_display;          // 21 Pokemon.pokemonDisplay_ (reference -> DISPLAY)
  void *f_events;           // 22 Pokemon.originEvents_ (reference -> event collection)
  void *f_limited;          // 23 Pokemon.limitedPokemonIdentifier_ (reference -> String, null/empty ok)
  void *f_attack;           // 24 Pokemon.individualAttack_ (Int32)
  void *f_defense;          // 25 Pokemon.individualDefense_ (Int32)
  void *f_stamina;          // 26 Pokemon.individualStamina_ (Int32)
  void *f_favorite;         // 27 Pokemon.favorite_ (Boolean)
  void *f_egg;              // 28 Pokemon.isEgg_ (Boolean)
  void *f_costume;          // 29 DISPLAY.Costume (Int32 enum)
  void *f_original_costume; // 30 DISPLAY.OriginalCostume (Int32 enum)
  void *f_background;       // 31 DISPLAY.LocationCard (reference; non-null => background)
  void *f_art;              // 32 DISPLAY.NaturalArtType (Int32 enum)
  void *f_art_asset;        // 33 DISPLAY.NaturalArtBackgroundAsset (Int32 enum)

  // --- enum sentinels (int32) -------------------------------------------------------
  int32_t xxs;              // CharacterSize.Xxs (must be a member of sizes[] and != xxl)
  int32_t xxl;              // CharacterSize.Xxl (must be a member of sizes[] and != xxs)
  int32_t costume_none;     // Costume.Unset
  int32_t art_none;         // NaturalArtType.NaturalArtUnset
  int32_t asset_none;       // NaturalArtBackgroundAsset.NaturalBackgroundUnset
  int32_t sizes[PGR_TF_SIZE_COUNT]; // all 5 CharacterSize members, pairwise distinct
};

static_assert(sizeof(PgrTransferFactsConfig) == 320,
              "PgrTransferFactsConfig must remain 320 bytes on LP64");

// Fixed LP64 request (56 bytes). bag/pokemon are caller-rooted managed pointers; native roots both
// immediately on entry. The caller has already resolved the pokemon (feature 34 / its bag lookup) and
// proved the source/encounter in JS; the reader only re-proves identity against the caught id.
struct PgrTransferFactsRequest {
  uint32_t version;    // == PGR_TF_REQUEST_VERSION
  uint32_t size;       // == sizeof(PgrTransferFactsRequest)
  uint64_t owner_tid;  // proven owner tid (must equal the current OS tid, nonzero)
  uint64_t lease_ms;   // residual foreground lease (clamped to 2500; re-checked after managed work)
  uint64_t caught_id;  // proven caught id (nonzero; must equal pokemon id before AND after managed calls)
  void    *pokemon;    // the inspected Pokemon (exact PK)
  void    *gm;         // GameMaster handle (non-null; interface, concrete class NOT asserted -- legacy)
  uint32_t protect;    // feature-4 protect mask; (protect & 48) requests a size invoke
  uint32_t reserved;   // 0
};

static_assert(sizeof(PgrTransferFactsRequest) == 56,
              "PgrTransferFactsRequest must remain 56 bytes on LP64");

// Fixed LP64 output (40 bytes). Zeroed + header-initialised on EVERY path; facts and flags are written
// ONLY on success (code 1). flags use PgrTransferFactsFlag; known is 0xFF on success, 0 otherwise.
struct PgrTransferFactsOut {
  uint32_t version;   // == PGR_TF_OUT_VERSION
  uint32_t size;      // == sizeof(PgrTransferFactsOut)
  int32_t  attack;    // individual attack (0..15 on success)
  int32_t  defense;   // individual defense (0..15 on success)
  int32_t  stamina;   // individual stamina (0..15 on success)
  int32_t  species;   // pokemon id / species (1..2000 on success)
  uint32_t known;     // mask of category bits that are valid (0xFF on success)
  uint32_t flags;     // PgrTransferFactsFlag category bits
  uint64_t caught_id; // echoed proven caught id (== pokemon id)
};

static_assert(sizeof(PgrTransferFactsOut) == 40,
              "PgrTransferFactsOut must remain 40 bytes on LP64");

// Fixed LP64 table: 8-byte header + 2 function pointers = 24 bytes.
struct PgrTransferFactsApi {
  uint32_t version;  // == PGR_TF_API_VERSION
  uint32_t size;     // == sizeof(PgrTransferFactsApi)
  // Validate + publish the immutable config once for the process lifetime. A repeat of the identical
  // config is PGR_TF_OK; a different one is PGR_TF_ERR_RECONFIG. No managed work.
  int (*install)(const PgrTransferFactsConfig *config);
  // Read one pokemon's transfer facts. Requires current tid == owner_tid (nonzero) and a residual lease
  // (clamped to 2500 ms, re-checked after the managed work). Returns PGR_TF_VALID (1, *out written),
  // PGR_TF_OK (0, refused: gate/busy/thread/expired), or a negative error (keep). Owner thread only; a
  // reentrant call is refused. NEVER issues a release action.
  int (*inspect)(const PgrTransferFactsRequest *request, PgrTransferFactsOut *out);
};

static_assert(sizeof(PgrTransferFactsApi) == 24,
              "PgrTransferFactsApi must remain a 24-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 35 and answers only on an exact version match.
const void *pgrazor_transfer_facts_api(uint32_t version);

#ifdef __cplusplus
}
#endif

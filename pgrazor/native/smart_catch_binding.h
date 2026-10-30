#pragma once
#include <stdint.h>

// Smart Background Catcher BINDING surface (FEATURE_PROPOSALS module2, binding half). This is the
// INTERNAL C++ glue that turns the already-proven runtime snapshots into a validated PgrSmartBinding and
// publishes it through pgr_smart_bind() (owned by smart_catch.h / the helper worker). It performs NO game
// mutation: it only resolves exports, resolves+validates classes/fields/methods via the real feature 47
// validators, reads the UseItemEncounter RPC id and the Status.Success value as runtime enum literals
// (never guessed), and hands the assembled binding to the helper.
//
// SOURCING (no guessed addresses):
//   * il2cpp runtime + reflection exports come from the feature 54 bootstrap snapshot
//     (pgrazor_runtime_bootstrap_api). The handful of reflection exports the bootstrap table does not
//     carry are dlsym'd from the SAME dlopen handle the snapshot returns (never a fabricated pointer).
//   * every game Il2CppClass* / FieldInfo* / MethodInfo* is resolved+validated through
//     pgrazor_runtime_metadata_api feature 47 (resolve_class / validate_field / validate_method), which
//     the coordinator binds via the bootstrap probe(). This leaf only snapshots; it never binds feature 47.
//   * rpc_id and status_success are read with il2cpp_field_static_get_value on the validated enum literals
//     (Holoholo.Rpc.Method.UseItemEncounter and the UseItemEncounterOutProto.Status "Success" literal).
//
// The published binding is immutable (pgr_smart_bind rejects a changed config) and every entry point is
// owner-tid guarded: it must run on the proven Unity owner thread. Returns are precise, log-usable codes.
// NO hook, NO thread, NO JS callback, NO inventory mutation.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "SmartCatch binding ABI requires LP64");

enum PgrSmartBindInfo {
  PGR_SMARTBIND_BOOTSTRAP_API = 54,  // pgrazor_runtime_bootstrap_api version queried
  PGR_SMARTBIND_METADATA_API  = 47,  // pgrazor_runtime_metadata_api version queried
  PGR_SMARTBIND_FRESH_MIN_MS  = 5000 // spawn-freshness floor: known despawn < 5s is filtered
};

// ensure_bound()/spawn_fresh() result domain. 0/positive are non-faults; negatives are log-usable faults.
enum PgrSmartBindResult {
  PGR_SMARTBIND_OK            = 0,   // ensure_bound: binding published (or already identical). spawn_fresh: n/a
  PGR_SMARTBIND_NOT_READY     = 1,   // bootstrap library/domain or feature 47 not ready yet; RETRY later
  PGR_SMARTBIND_FRESH_PASS    = 1,   // spawn_fresh: lifetime unknown or >= FRESH_MIN_MS remaining
  PGR_SMARTBIND_FRESH_FILTER  = 0,   // spawn_fresh: known to despawn in < FRESH_MIN_MS
  PGR_SMARTBIND_ERR_OWNER       = -1,  // called off the proven Unity owner thread / owner mismatch
  PGR_SMARTBIND_ERR_NO_BOOTSTRAP= -2,  // feature 54 getter absent / wrong ABI / snapshot faulted
  PGR_SMARTBIND_ERR_NO_METADATA = -3,  // feature 47 getter absent / wrong ABI / not bound
  PGR_SMARTBIND_ERR_EXPORT      = -4,  // a required il2cpp export (snapshot slot or dlsym) was null
  PGR_SMARTBIND_ERR_CLASS       = -5,  // a mandatory class failed resolve_class
  PGR_SMARTBIND_ERR_FIELD       = -6,  // a mandatory field failed validate_field / structural resolve
  PGR_SMARTBIND_ERR_METHOD      = -7,  // a mandatory method failed validate_method
  PGR_SMARTBIND_ERR_LITERAL     = -8,  // the UseItemEncounter or Status.Success enum literal read failed
  PGR_SMARTBIND_ERR_BIND        = -9,  // pgr_smart_bind rejected the assembled binding
  PGR_SMARTBIND_ERR_BUSY        = -10, // concurrent/reentrant ensure_bound; nothing changed
  PGR_SMARTBIND_ERR_IDENTITY    = -11  // spawn_fresh: null/wrong-class target, or not bound
};

// Assemble + publish the Smart Catcher native binding for this process. `owner` is the proven Unity owner
// tid (must equal the live thread, with a non-null il2cpp thread). Idempotent: a second successful call
// re-hands the identical binding (pgr_smart_bind treats that as OK). Returns PGR_SMARTBIND_OK on success,
// PGR_SMARTBIND_NOT_READY while the runtime is still coming up (retry), or a negative fault above. Issues
// NO managed mutation; the only managed reads are the two static enum-literal getters. The optional
// GameMaster level group is bound only when fully validated; otherwise the helper runs without level.
int pgr_smart_ensure_bound(uint64_t owner);

// Spawn-freshness filter for a rooted WildMapPokemon. Validates identity (non-null, exact WildMapPokemon
// class), reads WildMapPokemon's WildPokemonProto, and derives despawn from LastModifiedMs +
// TimeTillHiddenMs (field-validated during ensure_bound). Returns PGR_SMARTBIND_FRESH_PASS (1) when the
// lifetime is unknown (TimeTillHiddenMs <= 0) or at least FRESH_MIN_MS remains, PGR_SMARTBIND_FRESH_FILTER
// (0) when the mon is known to despawn in under FRESH_MIN_MS, or PGR_SMARTBIND_ERR_IDENTITY (-11) on a
// null/wrong-class target, an owner-thread mismatch, or when ensure_bound has not succeeded. Read-only.
int pgr_smart_spawn_fresh(void *rootedWild, uint64_t owner);
// 1 = exact successful encounter with Pokemon data, 0 = declined, negative = malformed.
int pgr_smart_encounter_success(void *target, void *response, uint64_t owner);

#ifdef __cplusplus
}
#endif

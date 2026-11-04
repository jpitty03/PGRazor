#pragma once
#include <atomic>
#include <stdint.h>

// Incense spawns (Niantic.Holoholo.Map.IncenseMapPokemon) for VGP and VGP+. pgr_vgp_incense_bind()
// validates everything on the Unity owner and publishes it once. Until then, and in the module
// fixtures, every VGP module stays wild-only.
struct PgrVgpIncense {
  void *klass;           // IncenseMapPokemon (exact)
  void *base;            // MapPokemon, parent of WildMapPokemon and IncenseMapPokemon (scene query type)
  void *m_id;            // IncenseMapPokemon.get_EncounterId() -> System.UInt64
  void *m_encounter;     // IncenseMapPokemon.SendEncounterRequest() -> IPromise<PokemonEncounterResponse>
  void *m_spawn;         // IncenseMapPokemon.get_SpawnPointId() -> System.String
  void *f_response;      // IncenseMapPokemon.emsg -> IncenseEncounterOutProto (set by the encounter)
  void *class_response;  // Holoholo.Rpc.IncenseEncounterOutProto
  void *f_result;        // IncenseEncounterOutProto.Result (enum Int32)
  void *f_pokemon;       // IncenseEncounterOutProto.Pokemon -> PokemonProto
  int32_t success;       // Result.IncenseEncounterSuccess
};

inline std::atomic<const PgrVgpIncense *> g_pgr_vgp_incense{nullptr};
inline const PgrVgpIncense *pgr_vgp_incense() { return g_pgr_vgp_incense.load(std::memory_order_acquire); }
inline bool pgr_vgp_incense_class(const void *klass) {
  const PgrVgpIncense *i = pgr_vgp_incense();
  return i && klass && klass == i->klass;
}

// Unity owner only. 0 bound, 1 not ready yet (retry later), negative failed (stays wild-only).
int pgr_vgp_incense_bind(uint64_t owner);

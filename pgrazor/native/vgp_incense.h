#pragma once
#include <atomic>
#include <stdint.h>

// Map spawns other than wild ones that VGP and VGP+ catch: incense (Niantic.Holoholo.Map.IncenseMapPokemon)
// and every other MapPokemon kind that shares the wild calls, such as lure (TroyDiskMapPlacePokemon)
// spawns. pgr_vgp_incense_bind() validates each kind on the Unity owner and publishes it once. Until then,
// and in the module fixtures, every VGP module stays wild-only.
struct PgrVgpIncense {
  void *klass;           // the spawn class (exact)
  void *base;            // MapPokemon, parent of every spawn class (scene query type)
  void *m_id;            // get_EncounterId() -> System.UInt64
  void *m_encounter;     // SendEncounterRequest() -> IPromise<PokemonEncounterResponse>
  void *m_spawn;         // get_SpawnPointId() -> System.String; null when the kind has none (no berries)
  void *f_response;      // the stored *EncounterOutProto (set by the encounter)
  void *class_response;  // Holoholo.Rpc.*EncounterOutProto
  void *f_result;        // response Result/Status (enum Int32)
  void *f_pokemon;       // response Pokemon -> PokemonProto
  int32_t success;       // the result's success literal
  void *m_destroy;       // MapPokemon.Destroy() -> System.Void; null leaves a caught spawn standing
};

constexpr int kPgrVgpKinds = 8;
inline std::atomic<const PgrVgpIncense *> g_pgr_vgp_kinds[kPgrVgpKinds]{};
// WildMapPokemon's Destroy(): a caught wild spawn also stays standing for a while unless removed.
inline std::atomic<void *> g_pgr_vgp_wild_destroy{nullptr};
// Set once any kind is bound; its base drives the MapPokemon scene query.
inline std::atomic<const PgrVgpIncense *> g_pgr_vgp_incense{nullptr};
inline const PgrVgpIncense *pgr_vgp_incense() { return g_pgr_vgp_incense.load(std::memory_order_acquire); }
// The bound kind whose class satisfies `is`, or null (wild, a stop, or an unsupported kind).
template <class Is> const PgrVgpIncense *pgr_vgp_kind_where(Is is) {
  for (auto &slot : g_pgr_vgp_kinds) {
    const PgrVgpIncense *k = slot.load(std::memory_order_acquire);
    if (k && is(k->klass)) return k;
  }
  return nullptr;
}
inline const PgrVgpIncense *pgr_vgp_kind(const void *klass) {
  return klass ? pgr_vgp_kind_where([klass](void *k) { return k == klass; }) : nullptr;
}
inline bool pgr_vgp_incense_class(const void *klass) { return pgr_vgp_kind(klass) != nullptr; }

// Unity owner only. 0 bound, 1 not ready yet (retry later), negative failed (stays wild-only).
int pgr_vgp_incense_bind(uint64_t owner);

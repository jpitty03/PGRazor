#pragma once
#include <cstdint>
#include <string>
#include <vector>
struct PgrSharedMetadataOutput;
// The player's Pokemon storage as plain data, for any feature that ranks or filters owned Pokemon
// (Smart battling counters today). Nothing managed is kept: callers that need a game object look the
// Pokemon up again by id. Unity owner thread only.
struct PgrOwnedPokemon {
  uint64_t id = 0;
  int species = 0, form = 0, cp = 0, hp = 0, max_hp = 0;
  int atk = 0, def = 0, sta = 0;  // IVs
  double cpm = 0;                 // cpMultiplier + additionalCpMultiplier
  int move1 = 0, move2 = 0, move3 = 0;
  int alignment = 0;              // 0 normal, 1 shadow, 2 purified
  bool lucky = false, favorite = false, egg = false, deployed = false;  // deployed: in a gym or a station
  std::string nickname;                 // player-given name, when set
  int gender = 0, shiny = 0, costume = 0;
  uint64_t caught_ms = 0;               // UTC milliseconds, when the game exposes it
};
// Called every Unity tick: binds the runtime on the owner thread.
void pgr_storage_tick(uint64_t tid, const PgrSharedMetadataOutput *metadata);
// The current snapshot. Re-read from the game only when the storage size changed or the copy is older than
// max_age_ms. Returns false when the storage cannot be read yet (out is left empty).
bool pgr_storage_snapshot(std::vector<PgrOwnedPokemon> *out, uint64_t max_age_ms = 30000);
// Force the next snapshot to re-read the live Pokemon list. Owner thread only; safe no-op elsewhere.
// Called after a successful catch and before a Rocket counter pick, so catch+transfer bursts do not
// leave stale members (the storage size alone cannot detect a catch paired with a transfer).
void pgr_storage_invalidate();
// Full plain-data snapshot as a JSON array, fetched fresh from the live list. Consumers pick only
// the fields they need (e.g. nickname + CP for duplicate Rocket counters). Owner thread only.
std::string pgr_storage_json();

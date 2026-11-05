#pragma once
#include <cstdint>

// PvP stat-product ranks from the bundled PvPoke snapshot (pvp_data.h). Pure arithmetic: no managed
// calls, no allocation, no cache. One rank costs one pass over the 4096 IV combinations.
namespace pgr_pvp {

constexpr int kLeagues = 3;            // 0 Great (1500), 1 Ultra (2500), 2 Master (no cap)
constexpr int kMaxLevelIndex = 98;     // level 50 (index 0 = level 1, steps of half a level)

struct Settings {
  int rank[kLeagues] = {0, 0, 0};  // keep at or below this rank (1..4096) per league; 0 = league off
  int meta_limit = 0;               // only species in PvPoke's top N for that league (1..2000)
};

struct Keep {
  int league = -1;        // 0..2
  int rank = 0;           // 1..4096
  int species = -1;       // snapshot index of the evolution that earned the rank
  int level_index = 0;    // best level under the cap (0 = level 1)
  int cp = 0;             // CP at that level
};

int species_count();
const char *species_name(int index);
int species_dex(int index);
const char *species_key(int index);
bool species_base(int index);

// CP of a snapshot species with these IVs at a CP multiplier (game formula, minimum 10).
int cp(int index, int attack, int defense, int stamina, double multiplier);

// Stat-product rank of the IVs in one league among all 4096 combinations, each at its highest level
// (up to 50) under the cap. 0 when the IVs cannot fit under the cap at any level.
int rank(int index, int league, int attack, int defense, int stamina, int *level_index = nullptr, int *cp_at = nullptr);

// Best (lowest) rank per league over the Pokemon's own species/form and everything it can evolve into,
// counting only species in that league's top meta_limit, and only evolutions that still fit under the
// cap at the Pokemon's current CP multiplier (0 = unknown, not checked). out[league].league is -1 when
// nothing qualifies in that league.
void best(int index, bool shadow, int attack, int defense, int stamina, double multiplier, int meta_limit,
          Keep out[kLeagues], unsigned leagues = 7);

// PvPoke species rank (position in that league's overall ranking, shadow-aware) of the best-placed
// species the Pokemon is or can evolve into; league -1 when none is ranked. rank holds the position.
Keep species_rank(int index, bool shadow, int league);

// Whether to keep for PvP: some league's best rank is within that league's limit. Returns the
// qualifying league with the lowest rank.
bool keep(int index, bool shadow, int attack, int defense, int stamina, double multiplier, const Settings &settings, Keep *out);

// "P2|great|ultra|master|meta" (ranks, 0 = off); false (and settings off) on anything else.
bool parse(const char *text, unsigned length, Settings *out);

}  // namespace pgr_pvp

#pragma once
#include <stdint.h>

// PvP keep check for auto-transfer (Unity owner only). pokemon is a caller-rooted Holoholo.Rpc.PokemonProto
// whose IVs and species the transfer facts already proved. Settings come from transport string slot 16.
//   0  no PvP keep (or every league is off): the other transfer rules decide
//   1  keep: text gets "PvP GL #12 Medicham"
//   2  keep: PvP keep is on but this Pokemon could not be checked (text "pvp-check-unavailable")
// Declared weak so module fixtures that do not link it skip the check.
extern "C" int pgr_pvp_keep(void *pokemon, int attack, int defense, int stamina, int species, uint64_t owner,
                            char *text, uint32_t capacity) __attribute__((weak));

// Ranks for the IV displays' PvP format (Unity owner only). Returns false at once unless that screen's
// format is PvP (transport slot 14). Results are cached, so a scrolling storage list stays cheap.
// Great and Ultra League: the best IV rank over every species the Pokemon is or can evolve into that
// is in PvPoke's top 150 for that league. Master League: that family's best PvPoke species rank (a
// hundo is always IV #1 there). species is the evolution it is for ("" when it is the Pokemon itself).
struct PgrPvpBest {
  int rank[3];          // 0 Great (IV), 1 Ultra (IV), 2 Master (species); 0 = nothing to show
  char species[3][32];
};
enum PgrPvpScreen { PGR_PVP_STORAGE = 0, PGR_PVP_ENCOUNTER = 1, PGR_PVP_DETAILS = 2 };
extern "C" bool pgr_pvp_best(void *pokemon, int screen, int attack, int defense, int stamina, uint64_t owner,
                             PgrPvpBest *out) __attribute__((weak));

// The Great (0), Ultra (1) or Master (2) League crest as a managed Sprite (iv_panel.cpp), or null.
// Unity owner only.
extern "C" void *pgr_pvp_crest(int league) __attribute__((weak));

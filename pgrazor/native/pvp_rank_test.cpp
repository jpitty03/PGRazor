// Native harness: py -3.12 -B pgrazor/native_harness.py pvp_rank --serial <device>
// Expected ranks come from an independent full-sort reference over the same snapshot.
#include "pvp_rank.cpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <time.h>

namespace {
int find(const char *key) {
  for (int i = 0; i < pgr_pvp::species_count(); ++i)
    if (!std::strcmp(pgr_pvp::species_key(i), key)) return i;
  std::printf("missing species %s\n", key);
  assert(false);
  return -1;
}
void expect(const char *key, int league, int a, int d, int s, int rank, int level, int cp) {
  int l = 0, c = 0;
  const int r = pgr_pvp::rank(find(key), league, a, d, s, &l, &c);
  if (r != rank || l != level || c != cp)
    std::printf("%s league %d %d/%d/%d: rank %d level %d cp %d, expected %d %d %d\n", key, league, a, d, s, r, l, c, rank, level, cp);
  assert(r == rank && l == level && c == cp);
}
pgr_pvp::Settings settings(int great, int ultra, int master, int meta) {
  pgr_pvp::Settings s{};
  s.rank[0] = great;
  s.rank[1] = ultra;
  s.rank[2] = master;
  s.meta_limit = meta;
  return s;
}
}  // namespace

int main() {
  // League 0 Great, 1 Ultra, 2 Master (no cap: everything at level 50).
  expect("azumarill", 0, 0, 15, 15, 1, 89, 1499);
  expect("azumarill", 0, 15, 15, 15, 2550, 70, 1497);
  expect("medicham", 0, 5, 15, 14, 1, 98, 1494);
  expect("medicham", 0, 15, 15, 15, 520, 85, 1495);
  expect("registeel", 1, 0, 14, 15, 1, 98, 2499);
  expect("registeel", 1, 0, 15, 15, 14, 97, 2489);
  expect("umbreon", 1, 15, 15, 15, 1, 98, 2416);  // fits under the cap at level 50
  expect("dragonite", 2, 15, 15, 15, 1, 98, 4287);
  expect("dragonite", 2, 15, 14, 15, 3, 98, 4277);
  expect("dragonite", 2, 13, 15, 15, 5, 98, 4256);
  assert(pgr_pvp::rank(find("azumarill"), 3, 0, 15, 15) == 0 && pgr_pvp::rank(find("azumarill"), 0, 16, 0, 0) == 0);

  pgr_pvp::Settings parsed{};
  assert(pgr_pvp::parse("P2|50|10|0|150", 14, &parsed) && parsed.rank[0] == 50 && parsed.rank[1] == 10 &&
         parsed.rank[2] == 0 && parsed.meta_limit == 150);
  assert(!pgr_pvp::parse("P2|50|10|0|0", 12, &parsed) && parsed.rank[0] == 0);
  assert(!pgr_pvp::parse("P2|50|10|0|150x", 15, &parsed) && !pgr_pvp::parse("P1|6|50|150", 11, &parsed));
  assert(!pgr_pvp::parse("P2|5000|10|0|150", 16, &parsed));

  // Marill keeps for its evolution Azumarill (Great League rank 1); a hundo does not.
  pgr_pvp::Keep why{};
  const int marill = find("marill");
  assert(pgr_pvp::keep(marill, false, 0, 15, 15, 0, settings(50, 10, 0, 150), &why) && why.league == 0 && why.rank == 1 &&
         why.species == find("azumarill"));
  assert(!pgr_pvp::keep(marill, false, 15, 15, 15, 0, settings(50, 10, 0, 150), &why) && why.league == -1);
  // Each league uses its own limit: rank 14 passes a top-25 Ultra League limit but not a top-10 one.
  assert(pgr_pvp::keep(find("registeel"), false, 0, 15, 15, 0, settings(0, 25, 0, 150), &why) && why.league == 1 && why.rank == 14);
  assert(!pgr_pvp::keep(find("registeel"), false, 0, 15, 15, 0, settings(0, 10, 0, 150), &why));
  // Master League only when it is on and the species is in its meta list.
  assert(!pgr_pvp::keep(find("dragonite"), false, 15, 14, 15, 0, settings(0, 0, 0, 150), &why));
  assert(pgr_pvp::keep(find("dragonite"), false, 15, 14, 15, 0, settings(0, 0, 10, 150), &why) && why.league == 2 && why.rank == 3);
  // Already over the Great League cap at its current level: no Great League keep.
  assert(!pgr_pvp::keep(find("azumarill"), false, 0, 15, 15, pgr_pvp_data::cpm[98], settings(50, 0, 0, 150), &why));
  // A species outside the meta list never keeps.
  assert(pgr_pvp_data::species[find("azumarill")].meta[0] > 1);  // Melmetal leads the Great League list
  assert(!pgr_pvp::keep(marill, false, 0, 15, 15, 0, settings(50, 0, 0, 1), &why));
  // best() fills every league independently.
  pgr_pvp::Keep all[pgr_pvp::kLeagues];
  pgr_pvp::best(find("registeel"), false, 0, 14, 15, 0, 2000, all);
  assert(all[0].league == 0 && all[1].league == 1 && all[1].rank == 1 && all[2].league == 2);

  // Cost of one display lookup for a three-stage family checked in all three leagues.
  const int bulbasaur = find("bulbasaur");
  timespec t0{}, t1{};
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int seen = 0;
  for (int i = 0; i < 50; ++i) {
    pgr_pvp::best(bulbasaur, false, i % 16, 15, 15, 0, 2000, all);
    seen += all[0].league >= 0;
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  const double us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3 / 50;
  std::printf("best(): %.0f us per lookup (bulbasaur family, 3 leagues), %d with a Great League rank\n", us, seen);
  std::puts("PASS pvp_rank: reference ranks, Master League, per-league limits, cap fit, evolutions, meta and settings");
  return 0;
}

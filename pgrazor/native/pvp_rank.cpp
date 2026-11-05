#include "pvp_rank.h"
#include "pvp_data.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace pgr_pvp {
namespace {
using namespace pgr_pvp_data;
constexpr int kSpecies = sizeof(species) / sizeof(species[0]);

bool valid_iv(int v) { return v >= 0 && v <= 15; }

int cp_of(const Species &e, int a, int d, int s, double m) {
  const double value = (e.attack + a) * std::sqrt(double(e.defense + d)) * std::sqrt(double(e.stamina + s)) * m * m / 10.0;
  const int result = int(std::floor(value));
  return result < 10 ? 10 : result;
}

// Highest level index whose CP stays under the cap, or -1 when even level 1 is over it.
int best_level(const Species &e, int a, int d, int s, int cap) {
  if (cp_of(e, a, d, s, cpm[0]) > cap) return -1;
  int lo = 0, hi = kMaxLevelIndex;
  while (lo < hi) {
    const int mid = (lo + hi + 1) / 2;
    if (cp_of(e, a, d, s, cpm[mid]) <= cap) lo = mid; else hi = mid - 1;
  }
  return lo;
}

double stat_product(const Species &e, int a, int d, int s, int level) {
  const double m = cpm[level];
  double hp = std::floor((e.stamina + s) * m);
  if (hp < 10) hp = 10;
  return (e.attack + a) * m * (e.defense + d) * m * hp;
}
// The species/form at index and everything it can evolve into (forward edges only).
int family_of(int index, int family[32]) {
  int count = 1;
  family[0] = index;
  for (int at = 0; at < count; ++at) {
    const Species &e = species[family[at]];
    for (int n = 0; n < e.evolutions; ++n) {
      const int next = edges[e.evolution + n];
      bool seen = false;
      for (int j = 0; j < count; ++j) seen = seen || family[j] == next;
      if (!seen && count < 32) family[count++] = next;
    }
  }
  return count;
}
}  // namespace

int species_count() { return kSpecies; }
const char *species_name(int i) { return i >= 0 && i < kSpecies ? species[i].name : ""; }
int species_dex(int i) { return i >= 0 && i < kSpecies ? species[i].dex : 0; }
const char *species_key(int i) { return i >= 0 && i < kSpecies ? species[i].key : ""; }
bool species_base(int i) { return i >= 0 && i < kSpecies && species[i].base; }

int cp(int i, int a, int d, int s, double m) {
  if (i < 0 || i >= kSpecies || !std::isfinite(m) || m <= 0) return 0;
  return cp_of(species[i], a, d, s, m);
}

int rank(int index, int league, int a, int d, int s, int *level_out, int *cp_out) {
  if (level_out) *level_out = 0;
  if (cp_out) *cp_out = 0;
  if (index < 0 || index >= kSpecies || league < 0 || league >= kLeagues || !valid_iv(a) || !valid_iv(d) || !valid_iv(s)) return 0;
  const Species &e = species[index];
  const int cap = league_cap[league];
  const int level = best_level(e, a, d, s, cap);
  if (level < 0) return 0;
  const double mine = stat_product(e, a, d, s, level);
  // Rank = 1 + combinations with a strictly larger stat product (ties share a rank).
  int better = 0;
  for (int sa = 0; sa < 16; ++sa)
    for (int sd = 0; sd < 16; ++sd)
      for (int ss = 0; ss < 16; ++ss) {
        const int l = best_level(e, sa, sd, ss, cap);
        if (l >= 0 && stat_product(e, sa, sd, ss, l) > mine * (1 + 1e-12)) ++better;
      }
  if (level_out) *level_out = level;
  if (cp_out) *cp_out = cp_of(e, a, d, s, cpm[level]);
  return better + 1;
}

void best(int index, bool shadow, int a, int d, int s, double multiplier, int meta_limit, Keep out[kLeagues], unsigned leagues) {
  for (int league = 0; league < kLeagues; ++league) out[league] = Keep{};
  if (meta_limit < 1 || index < 0 || index >= kSpecies || !valid_iv(a) || !valid_iv(d) || !valid_iv(s)) return;
  // The Pokemon's own species and everything it can evolve into (forward edges only).
  int family[32];
  const int count = family_of(index, family);
  const bool known = std::isfinite(multiplier) && multiplier > 0;
  for (int league = 0; league < kLeagues; ++league) {
    if (!(leagues & (1u << league))) continue;
    for (int j = 0; j < count; ++j) {
      const Species &e = species[family[j]];
      const int position = shadow && e.shadow[league] ? e.shadow[league] : e.meta[league];
      if (!position || position > meta_limit) continue;
      // Already over the cap at its current level: it can never be used in this league.
      if (known && cp_of(e, a, d, s, multiplier) > league_cap[league]) continue;
      int level = 0, at_cp = 0;
      const int r = rank(family[j], league, a, d, s, &level, &at_cp);
      if (r && (out[league].league < 0 || r < out[league].rank)) out[league] = Keep{league, r, family[j], level, at_cp};
    }
  }
}

Keep species_rank(int index, bool shadow, int league) {
  Keep best{};
  if (index < 0 || index >= kSpecies || league < 0 || league >= kLeagues) return best;
  int family[32];
  const int count = family_of(index, family);
  for (int j = 0; j < count; ++j) {
    const Species &e = species[family[j]];
    const int position = shadow && e.shadow[league] ? e.shadow[league] : e.meta[league];
    if (position && (best.league < 0 || position < best.rank)) best = Keep{league, position, family[j], 0, 0};
  }
  return best;
}

bool keep(int index, bool shadow, int a, int d, int s, double multiplier, const Settings &settings, Keep *out) {
  if (out) *out = Keep{};
  if (!settings.rank[0] && !settings.rank[1] && !settings.rank[2]) return false;
  Keep leagues[kLeagues];
  best(index, shadow, a, d, s, multiplier, settings.meta_limit, leagues,
       (settings.rank[0] ? 1u : 0u) | (settings.rank[1] ? 2u : 0u) | (settings.rank[2] ? 4u : 0u));
  Keep chosen{};
  for (int league = 0; league < kLeagues; ++league) {
    const Keep &k = leagues[league];
    if (k.league < 0 || !settings.rank[league] || k.rank > settings.rank[league]) continue;
    if (chosen.league < 0 || k.rank < chosen.rank) chosen = k;
  }
  if (out) *out = chosen;
  return chosen.league >= 0;
}

bool parse(const char *text, unsigned length, Settings *out) {
  if (!out) return false;
  *out = Settings{};
  if (!text || length < 11 || length > 40) return false;
  char buffer[41];
  std::memcpy(buffer, text, length);
  buffer[length] = 0;
  int great = 0, ultra = 0, master = 0, meta = 0, used = 0;
  if (std::sscanf(buffer, "P2|%d|%d|%d|%d%n", &great, &ultra, &master, &meta, &used) != 4 || used != int(length)) return false;
  for (int r : {great, ultra, master})
    if (r < 0 || r > 4096) return false;
  if (meta < 1 || meta > 2000) return false;
  out->rank[0] = great;
  out->rank[1] = ultra;
  out->rank[2] = master;
  out->meta_limit = meta;
  return true;
}

}  // namespace pgr_pvp

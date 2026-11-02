#pragma once
// Pure, allocation-free ranking. Caller serializes calls (the IV display Unity owner).
// Snapshot and rules match original 0.1.6 patch 0005, not current PvP metagame ratings.
namespace pgr_pvp {
unsigned cache_capacity();
unsigned long long cache_builds();
unsigned long long cache_hits();
unsigned long long cache_evictions();
struct Fit {int league,species,rank,level,cp;}; // level index: 0 = Lv1, 1 = Lv1.5
constexpr int kMaxFits=192;
int ranks(int dex,int form,int attack,int defense,int stamina,double multiplier,Fit *out,int capacity);
const char *name(int species_index);
double multiplier(int level_index);
// Complete rich-text lines, each prefixed by newline. Never writes a partial line/tag.
int lines(int dex,int form,int attack,int defense,int stamina,double multiplier,bool owned,char *out,int capacity);
int cell_summary(int dex,int form,int attack,int defense,int stamina,double multiplier,char *out,int capacity);
}

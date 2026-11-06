#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
// Smart healing: heals damaged (and optionally fainted) Pokemon with the cheapest mix of potions and
// revives through the game's own IItemBag.UseItemOnPokemon. Settings ride transport string slot 18
// ("H1|enabled|when|threshold|revive"). Unity owner thread only.
void pgr_heal_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata);
void pgr_heal_stop(uint64_t tid);

// The item choice, separate so it can be tested without the game.
namespace pgr_heal {
enum Kind {POTION,SUPER_POTION,HYPER_POTION,MAX_POTION,REVIVE,MAX_REVIVE,KIND_COUNT};
struct Stock {int count[KIND_COUNT]={};};
struct Plan {int count[KIND_COUNT]={};int healed=0;long cost=0;bool any()const{for(int c:count)if(c)return true;return false;}};
// The plan for one Pokemon at hp of max_hp. A fainted Pokemon (hp 0) is planned only when revive is
// true; otherwise the plan is empty. An empty plan means use nothing.
Plan plan(int hp,int max_hp,const Stock &stock,bool revive);
// The item to use first from a plan: a revive before any potion, then the largest potion.
int first(const Plan &p);
}

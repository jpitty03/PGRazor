#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
// Smart healing: heals damaged (and optionally fainted) Pokemon with the cheapest mix of potions and
// revives through the game's own IItemBag.UseItemOnPokemon. Settings ride transport string slot 18
// ("H2|enabled|when|threshold|revive|full"). Unity owner thread only.
void pgr_heal_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata);
void pgr_heal_stop(uint64_t tid);

// Read-only owner-thread probe: non-zero while smart healing still has pending work
// (an item use in flight or Pokemon queued to heal). Touches no IL2CPP/managed state,
// so another owner-thread feature can wait for healing to settle without racing it.
int pgr_heal_busy();

// ---------------------------------------------------------------------------------------------
// Explicit between-round gym healing (off-map). pgr_heal_tick heals ONLY while the transport
// reports the map visible; a Gym Battle-until-defeated loop never shows the map, so these two
// owner-thread calls let the gym-repeat coordinator drive the same after-battle scan + item path
// in the verified gap between rounds WITHOUT globally relaxing that map gate. Both run on the Unity
// owner thread, honour the live Smart healing settings (enabled, when After battle/Both, threshold,
// revive, heal fully, item selection/stock safeguards) and store no GUI pointer or callback globally.
//
// pgr_heal_gym_baseline: call once at a confirmed MANUAL Gym Battle press, before the first round.
// READ ONLY -- records every stored Pokemon's current HP as the after-battle baseline and arms the
// off-map session. Uses no item and queues nothing. A disabled or periodic-only setting records no
// baseline and grants no permission.
void pgr_heal_gym_baseline(uint64_t tid,const PgrSharedMetadataOutput *metadata);

// pgr_heal_gym_tick: call only after a CONFIRMED complete, victorious round with the same hostile
// gym's approach/prep open on top and every repeat gate already passed. round_token is the round's
// unique, nonzero, monotonic timestamp; repeat calls with a finished token never rescan or restart it.
// still_ready is the caller's revalidation hook (repeat still on, same account, gym still open): it is
// checked before issuing any item and again after each potentially reentrant managed lookup, so
// disabling repeat, switching account or leaving the gym can never start another item. Returns 1 while
// still observing or healing (call again), 0 when the round is settled or skipped (stop the round),
// and -1 on error (stop the repeat safely). Honours the post-battle storage-HP delay: it keeps an
// observation window open and scans for the real HP drop before declaring a round done, and follows
// each item use to its async result + HP confirmation; an initially empty queue is never mistaken for
// a finished scan.
int pgr_heal_gym_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata,uint64_t round_token,bool (*still_ready)());

// The item choice, separate so it can be tested without the game.
namespace pgr_heal {
enum Kind {POTION,SUPER_POTION,HYPER_POTION,MAX_POTION,REVIVE,MAX_REVIVE,KIND_COUNT};
struct Stock {int count[KIND_COUNT]={};};
struct Plan {int count[KIND_COUNT]={};int healed=0;long cost=0;bool any()const{for(int c:count)if(c)return true;return false;}};
// The plan for one Pokemon at hp of max_hp. A fainted Pokemon (hp 0) is planned only when revive is
// true; otherwise the plan is empty. An empty plan means use nothing. full: heal all the way whenever the
// items on hand can (the cheapest mix that does), instead of saving big items for big wounds.
Plan plan(int hp,int max_hp,const Stock &stock,bool revive,bool full=false);
// The item to use first from a plan: a revive before any potion, then the largest potion.
int first(const Plan &p);
}

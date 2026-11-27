#pragma once
#include <stdint.h>
#include <string>
struct PgrSharedMetadataOutput;
// Smart battling: picks counters for the Pokemon you are fighting. Before a gym, raid or typed Rocket
// grunt battle the game's recommended party is replaced with the best counters from storage. During
// gym and Team GO Rocket battles the team member best suited to the current opponent is swapped in,
// when a new opponent appears or yours faints. One setting, off by default, on transport string slot
// 21 ("F1|enabled"). Unity owner thread only.
void pgr_smart_battle_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata);
void pgr_smart_battle_stop(uint64_t tid);
bool pgr_smart_battle_enabled();
// Rocket "choose your next Pokemon" prompt: of these SwapButtonVNext buttons (all usable), the index
// of the best counter to the current opponent, or -1 when that cannot be worked out.
int pgr_smart_battle_pick_prompt(void *const *buttons,int count);
// Current Team GO Rocket line-ups (Java RocketLineups): lines "key|slot1|slot2|slot3", each slot
// "type1,type2,dex;...". Any thread. Used by the Rocket party pick; unknown characters keep the old rules.
void pgr_smart_battle_lineups(const char *text);
// The counters picked for the last Rocket battle, for the overlay card: "key\n" then one
// "species,form,cp,shadow" line per Pokemon, best first. Empty when there is nothing new. Any thread.
std::string pgr_smart_battle_counters();

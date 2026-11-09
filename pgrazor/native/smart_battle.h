#pragma once
#include <stdint.h>
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

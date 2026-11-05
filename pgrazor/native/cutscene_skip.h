#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
// Skip cutscenes. Transport string slot 17 carries "K1|<0 off, 1 on>". When on, the game's
// Pokemon cutscenes (evolve, Mega, egg hatch, egg incubate, purify, fusion, unfusion, form change,
// new species / Pokedex) and a Team GO Rocket member's spoken lines are never created: the hooked
// entry points return what the game would get after the cutscene ends, without building its GUI.
// The Rocket screen with the BATTLE button still appears; battles start only when it is pressed.
void pgr_cutscene_skip_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata);
void pgr_cutscene_skip_stop(uint64_t tid);

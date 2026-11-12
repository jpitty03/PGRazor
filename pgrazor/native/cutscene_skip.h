#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
// Slot 17: "K2|<skip cutscenes>|<auto incubate>" (legacy K1 is accepted).
// Auto incubate closes only the Eggs inventory opened by hatch-result dismissal, through
// ClickDismiss after it settles. Manually opened Eggs pages remain accessible.
// When Skip cutscenes is on, the game's
// Pokemon cutscenes (evolve, Mega, egg hatch, egg incubate, purify, fusion, unfusion, form change,
// new species / Pokedex) and a Team GO Rocket member's spoken lines are never created: the hooked
// entry points return what the game would get after the cutscene ends, without building its GUI.
// The Rocket screen with the BATTLE button still appears; battles start only when it is pressed.
void pgr_cutscene_skip_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata);
void pgr_cutscene_skip_stop(uint64_t tid);

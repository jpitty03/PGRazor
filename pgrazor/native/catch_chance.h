#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
// Catch chance and auto ball for manual encounters. Transport string slot 15 carries
// "C1|<show>|<auto>|<target percent>|<picker berry>".
void pgr_catch_chance_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata);
// The encounter state opened / closed (from the EncounterInteractionState hooks; Unity thread).
void pgr_catch_chance_enter(void *state);
void pgr_catch_chance_exit(void *state);
void pgr_catch_chance_stop();

#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
// Catch chance and auto ball for manual encounters. Transport string slot 15 carries
// "C3|<show>|<auto>|<target percent>|<picker berry>|<Mighty Safari>|<auto throw>|<cooldown ms>".
// C1 and C2 remain accepted.
void pgr_catch_chance_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata);
// The encounter state opened / closed (from the EncounterInteractionState hooks; Unity thread).
void pgr_catch_chance_enter(void *state);
void pgr_catch_chance_exit(void *state);
void pgr_catch_chance_stop();
// VGP+ auto ball: base[1..3] = the server's Poke/Great/Ultra capture probability in an
// EncounterOutProto (0 when not offered). Owner thread, after the first tick; false when unreadable.
bool pgr_catch_chance_base(void *encounter_out,double base[4]);
// Background VGP+ gate: the confirmed response must offer this ball with a
// finite positive capture probability. No manual EncounterState is consulted.
bool pgr_catch_chance_offers_ball(void *encounter_out,int item);

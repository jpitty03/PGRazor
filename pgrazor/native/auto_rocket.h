#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
// Auto Rocket: walks to invaded PokéStops, starts each Team GO Rocket battle (grunts, optionally
// leaders), lets the Rocket battle automation fight it, leaves the shadow catch to Auto throw and
// moves on. Java owns the session (transport string slot 12, "T1|...") and the movement; this
// controller owns finding stops and pressing the game's own screens. Unity owner thread only.
// Returns true while a session owns the game, so other map automation waits.
bool pgr_auto_rocket_tick(uint64_t tid, const PgrSharedMetadataOutput* metadata);
void pgr_auto_rocket_stop(uint64_t tid);
// 1 while an Auto Rocket session is set to keep every catch (no transfer rules).
extern "C" int pgr_auto_rocket_keeps_catches();
// True while a session is only looking for or walking to its next stop.
bool pgr_auto_rocket_between_battles();
// True while a walking session is between stops and VGP+ may catch and spin; it closes when the next
// stop is reached and VGP+ is idle, so VGP+ never acts while a stop is opened or a battle runs.
bool pgr_auto_rocket_vgp_window();

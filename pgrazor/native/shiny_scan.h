#pragma once
#include <stdint.h>
#include "shared_metadata.h"
// Shiny Scanner, native half. While Java allows it (transport string slot 23,
// "N2|<session>|<on>|<gap ms>|<max in flight>|<budget>|<count>|<species,...>"), every wild Pokemon on the map within
// 80 m gets one background encounter (WildMapPokemon.SendEncounterRequest, as Shundo Sniper does) and the
// shiny flag is read from the encounter response the game keeps on the map Pokemon. Nothing is thrown, fed
// or opened. A Pokemon the game already has a response for is read without a new request, several requests
// wait for their answers at once, and each encounter id is asked about only once. Results go out as
// "shiny-scan" events; "sent" says whether the check cost a request.
// With <count> set (the scanner itself may be off) every wild Pokemon the game holds an encounter answer for
// is reported once as an "encounter" event, whoever opened it: the player, VGP+, Shundo Sniper or this
// scanner. That is what the encounter counter adds up.
void pgr_shiny_scan_tick(uint64_t tid, const PgrSharedMetadataOutput *metadata);
void pgr_shiny_scan_stop(uint64_t tid);

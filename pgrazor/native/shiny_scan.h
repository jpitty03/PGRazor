#pragma once
#include <stdint.h>
#include "shared_metadata.h"
// Shiny Scanner, native half. While Java allows it (transport string slot 23,
// "N3|<session>|<on>|<gap ms>|<max in flight>|<budget>|<count>|<hold>|<catch>|<species,...>"), every wild Pokemon on the map within
// 80 m gets one background encounter (WildMapPokemon.SendEncounterRequest, as Shundo Sniper does) and the
// shiny flag is read from the encounter response the game keeps on the map Pokemon. Nothing is thrown, fed
// or opened. A Pokemon the game already has a response for is read without a new request, several requests
// wait for their answers at once, and each encounter id is asked about only once. Results go out as
// "shiny-scan" events; "sent" says whether the check cost a request.
// With <count> set (the scanner itself may be off) every wild Pokemon the game holds an encounter answer for
// is reported once as an "encounter" event, whoever opened it: the player, VGP+, Shundo Sniper or this
// scanner. That is what the encounter counter adds up.
// A shiny that was found stays marked: its place on the screen is sent ten times a second (transport
// action 59) until it leaves the map, so the overlay can draw a symbol above it.
// The found shiny's encounter stays open. With <hold> set no further checks are sent until it is caught
// or let go (a new session), as Shundo Sniper does. A new <catch> number acts on it: while it is on the
// map the game's own tap is invoked (MapPokemon.OnTap), so the encounter opens or the player's Auto throw
// catches it exactly as if they had tapped it; once it has left the map it is thrown at directly, a
// Golden Razz (or Razz) Berry and then excellent curveballs with the best ball on hand (never a Master
// Ball), through the game's catch request.
void pgr_shiny_scan_tick(uint64_t tid, const PgrSharedMetadataOutput *metadata);
void pgr_shiny_scan_stop(uint64_t tid);

#pragma once
#include <stdint.h>
#include "shared_metadata.h"
// Shundo Sniper, native half. Java teleports to a 100% IV spawn from the feeds and names its encounter
// id on transport string slot 19 ("S1|<request>|<encounter id>|<go>"). This module waits for that spawn to
// load on the map, opens a background encounter (WildMapPokemon.SendEncounterRequest) and reads the
// shiny flag and IVs from the encounter response. A non-shiny is dropped without a berry or a throw,
// so it adds no cooldown. A shiny is held in its encounter until Java sets go (the cooldown estimate
// where the player waits is zero), then caught with the best ball on hand (Ultra, Great, then Poké; never
// a Master Ball) after a Golden Razz (or Razz) Berry, as excellent curveballs. Results go out as
// "shundo-sniper" events tagged with the request number.
void pgr_shundo_tick(uint64_t tid, const PgrSharedMetadataOutput *metadata);
void pgr_shundo_stop(uint64_t tid);

#pragma once
#include <stdint.h>
#include "shared_metadata.h"
// Battery usage. Transport string slot 20 carries "B2|<low resolution>|<20 fps>|<low quality>|<black
// screen>" (each 0 or 1). While a setting is on, this module keeps the game's Unity graphics at the lower
// value, re-applied every few seconds because the game sets some of them itself:
//   black screen:   PGRazor's black screen covers the game, so a 0.25 render scale (frame rate kept);
//   low resolution: the URP asset's renderScale at most 0.5 (the 3D view is drawn smaller, then scaled up);
//   20 fps:         Application.targetFrameRate 20 (the game's own default is 30 on Android);
//   low quality:    no fog, HDR, opaque-texture copy, MSAA or URP shadows (zero shadow distance),
//                   QualitySettings.lodBias at most 0.5, and three of the game's own render steps skipped
//                   through validated hooks: the angled ground-shadow pass, Vista water reflections and the
//                   encounter's dense ground cover. The hooks call straight through while it is off.
//                   In encounters, the grass tufts, clovers, flowers and props' blob shadows are switched
//                   off too (they are separate objects, not part of the ground cover).
// The game's own value is remembered before the first change (and again whenever the game changes it)
// and put back when the setting is turned off, the config goes stale, or the module stops.
void pgr_battery_tick(uint64_t tid, const PgrSharedMetadataOutput *metadata);
void pgr_battery_stop(uint64_t tid);

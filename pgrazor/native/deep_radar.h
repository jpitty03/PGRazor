#pragma once
#include <cstdint>
#include "map_poi.h"

// Feature72/API1. Passive decoded map response reader. No gameplay RPCs, no
// refresh calls, no retained managed objects. Only scalar snapshots cross52.
// tick/stop must be called on the already-proven Unity owner. Inactive and
// stale leases leave the installed process-lifetime hook as pure passthrough.
struct PgrDeepRadarApi {
 uint32_t version,size;
 void (*tick)(uint64_t owner_tid,uint64_t remaining_ms);
 void (*stop)(uint64_t owner_tid);
};
static_assert(sizeof(PgrDeepRadarApi)==24,"Deep Radar LP64 API1");
void pgr_deep_radar_tick(uint64_t owner_tid,uint64_t remaining_ms);
void pgr_deep_radar_stop(uint64_t owner_tid);
void pgr_deep_radar_seed_forts(uint64_t owner_tid,const PgrMapPoiOutput*);
extern "C" const void *pgrazor_deep_radar_api(uint32_t version);

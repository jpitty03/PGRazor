#pragma once
#include "deep_radar_store.h"
#include "runtime_bootstrap.h"
#include <cstdint>
// Optional local Pokédex enrichment. Owner/lease is supplied by the observer;
// returns -1 when unavailable. Every invocation's temporary roots are RAII-owned.
int pgr_deep_radar_uncaught(const PgrBootBinding&,int32_t species,bool (*allowed)());
unsigned pgr_deep_radar_scene(const PgrBootBinding&,pgr::radar::Store&,uint64_t owner,uint64_t now,uint64_t lease,bool (*allowed)());

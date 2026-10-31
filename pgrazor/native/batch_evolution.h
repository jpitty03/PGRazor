#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
void pgr_evolution_tick(uint64_t,const PgrSharedMetadataOutput*);
void pgr_evolution_stop(uint64_t);

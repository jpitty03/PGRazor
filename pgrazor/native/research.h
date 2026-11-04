#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
void pgr_research_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata);
void pgr_research_stop();

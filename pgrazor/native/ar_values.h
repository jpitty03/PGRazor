#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
// Reads the game master's AR+ thresholds once and hands the AR+ throw hook values inside them.
void pgr_ar_values_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata);

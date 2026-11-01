#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
// Unity owner only. True blocks automatic catches for a manual incense session.
bool pgr_incense_guard_tick(uint64_t owner, const PgrSharedMetadataOutput *metadata);
// Call only after revoking/disabling automatic catches on that same Unity tick.
void pgr_incense_guard_open(uint64_t owner);
void pgr_incense_guard_stop(uint64_t owner);

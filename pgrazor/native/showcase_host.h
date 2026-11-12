#pragma once
#include <cstdint>
struct PgrSharedMetadataOutput;
// Owner-thread entry points. Keep returns true for winners AND uncertainty;
// false means only that Showcase retention does not veto the existing rules.
void pgr_showcase_tick(uint64_t owner,const PgrSharedMetadataOutput*,uint64_t deadline_ms);
void pgr_showcase_stop(uint64_t owner);
void pgr_showcase_caught(uint64_t id,uint64_t owner,bool honor_keep_all=true);
bool pgr_showcase_keep(uint64_t id,uint64_t owner);

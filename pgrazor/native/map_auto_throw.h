#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
// Unity owner only. Prepare before gameplay automation, so map taps have an explicit owner.
void pgr_map_auto_throw_tick(uint64_t tid,const PgrSharedMetadataOutput* md,bool blocked);
bool pgr_map_auto_throw_active();
void pgr_map_auto_throw_stop();

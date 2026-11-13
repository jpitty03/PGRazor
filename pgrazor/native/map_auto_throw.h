#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
// Unity owner only. Prepare before gameplay automation, so map taps have an explicit owner.
void pgr_map_auto_throw_tick(uint64_t tid,const PgrSharedMetadataOutput* md,bool blocked);
// True once a tapped Pokemon is being caught; false for a tap that is still waiting for VGP+.
bool pgr_map_auto_throw_active();
// The Pokemon being caught (rooted by the session, owner thread only), or null.
void* pgr_map_auto_throw_target();
void pgr_map_auto_throw_stop();

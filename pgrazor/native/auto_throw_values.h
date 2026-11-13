#pragma once
#include "throws.h"

inline PgrThrowValue pgr_raid_throw_value(int ball) {
    PgrThrowValue value{};
    value.item=ball;value.radius=0.1f;value.bullseye=1;value.spinning=1;
    return value;
}

// Start with an ordinary straight hit, then use the same policy as a hand throw.
// This includes rating, per-throw variety and curveball preferences. Always Hit
// is intrinsic to Auto Throw; rating/curve defaults are exclusive to Auto-Raid.
inline bool pgr_manual_throw_value(const PgrThrowApi* api,void* ball,int item,PgrThrowValue& value) {
    value={};value.item=item;value.radius=1.0f;
    if(!api||!api->rewrite_submit||api->rewrite_submit(ball,&value)!=0)return false;
    value.missed=0;
    return true;
}

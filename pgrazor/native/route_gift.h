#pragma once
#include <cstdint>
struct PgrSharedMetadataOutput;
// Exchange gifts with Mateo on Routes: presses the game's own buttons through a gift-exchange NPC
// encounter (conversation, "Exchange gift", send, open). Owner-thread tick; config in transport slot 24.
void pgr_route_gift_tick(uint64_t tid, const PgrSharedMetadataOutput *metadata);
void pgr_route_gift_stop();

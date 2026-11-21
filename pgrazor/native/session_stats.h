#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
// Once a minute on the Unity owner, reads the signed-in player's lifetime counters (PlayerStatsProto)
// and reports them as a "session-stats" event. Read-only; no game request is sent.
void pgr_session_stats_tick(uint64_t tid, const PgrSharedMetadataOutput *metadata);

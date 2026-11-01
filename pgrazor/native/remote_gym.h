#pragma once
#include <stdint.h>

// One-shot Remote Gym Feeding (Feature 2 of GALARIAN_BIRD_FEATURE.md). Feeds surplus berries to the
// player's own remotely deployed gym defenders and terminates: it runs once per manual command, never
// on an auto timer. Called ONLY by the existing proven Unity gameplay owner thread; it never starts a
// worker thread and performs no network or threading outside the game's own RPC APIs.
//
// The command, permission snapshot and action events all travel over the existing runtime transport
// (command at string slot 12, event action 19 kind "remote-gym"); the coordinator wires the slot, the
// per-tick call site and the Java/Control side. See remote_gym_policy.h for the testable policy.

struct PgrSharedMetadataOutput;

void pgr_remote_gym_tick(uint64_t tid, const PgrSharedMetadataOutput *metadata);
void pgr_remote_gym_stop(uint64_t tid);

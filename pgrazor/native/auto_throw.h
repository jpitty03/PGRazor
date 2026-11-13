#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
// Unity-thread entry points. Auto-Raid calls the shared capture primitive directly;
// only this manual encounter controller is governed by the Throws preference.
void pgr_auto_throw_tick(uint64_t tid, const PgrSharedMetadataOutput* metadata, bool raid_owns_game);
void pgr_auto_throw_enter(void* interaction);
void pgr_auto_throw_exit(void* interaction);
void pgr_auto_throw_stop();
bool pgr_auto_throw_pending();

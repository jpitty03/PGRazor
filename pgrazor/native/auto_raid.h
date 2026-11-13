#pragma once
#include <stdint.h>

// Native Auto-Raid controller for PGRazor (0.435.2).
// Runs exclusively on the proven Unity main gameplay thread. Discovers nearby active raid bosses matching
// user tier/species filters, requests travel via the runtime transport, opens the local gym, enforces
// Free Daily Pass first and optional Premium Battle Pass (strictly forbidding Remote Raid Passes and
// shop/coin purchases), monitors lobby minimum players and wait timeout, delegates raid combat to the
// existing combat_host raid auto-attack path, advances RaidResolveState rewards into the bonus catch
// encounter, and tracks completed raids, consecutive losses, and shiny catch stop conditions.
//
// Returns true while Auto-Raid owns or reserves gameplay automation so other autonomous map jobs
// (such as incense_guard / shundo) pause until Auto-Raid is idle or stopped.

struct PgrSharedMetadataOutput;

bool pgr_auto_raid_tick(uint64_t tid, const PgrSharedMetadataOutput* metadata);
void pgr_auto_raid_stop(uint64_t tid);

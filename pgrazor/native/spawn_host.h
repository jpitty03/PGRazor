#pragma once
#include "shared_metadata.h"
#include "unity_scheduler.h"
#include <stdint.h>
enum PgrSpawnHostStatus {PGR_SPAH_OK=0,PGR_SPAH_NOT_READY=1,PGR_SPAH_EMPTY=2,PGR_SPAH_BAD_CONFIG=-1,PGR_SPAH_REBIND=-2,PGR_SPAH_OFF_OWNER=-3,PGR_SPAH_BUSY=-4,PGR_SPAH_UNAVAILABLE=-5,PGR_SPAH_STOPPED=-6,PGR_SPAH_TRANSPORT_ERROR=-7};
enum PgrSpawnHostFlags {PGR_SPAH_TRANSPORT=1};
// Explicit metadata is a validated56 output; test fixtures may supply a synthetic
// universe. Production passes null to configure and uses54+61 exclusively.
struct PgrSpawnHostConfig {uint32_t version,size,flags,exclusive_owner;const PgrSharedMetadataOutput *metadata;void *thread_current;uint64_t owner_tid;};
struct PgrSpawnHostOptions {uint32_t version,size,enabled,allowed,lease_ms,reserved;};
struct PgrSpawnHostSample {uint32_t version,size,configured,installed,stopping,ready;int32_t error,leaf_error;uint64_t owner_tid,ticks;double original,effective;uint64_t count;};
struct PgrSpawnHostEvent {uint32_t version,size;char json[384];};
struct PgrSpawnHostApi {
 uint32_t version,size;
 int (*configure)(const PgrSpawnHostConfig*);
 int (*update)(const PgrSpawnHostOptions*);
 int (*tick)(uint64_t tid);
 int (*stop)();
 int (*sample)(PgrSpawnHostSample*);
 int (*next_event)(PgrSpawnHostEvent*);
 PgrUnityWork on_unity_pulse;
};
static_assert(sizeof(PgrSpawnHostConfig)==40,"Spawn host config");
static_assert(sizeof(PgrSpawnHostOptions)==24,"Spawn host options");
static_assert(sizeof(PgrSpawnHostSample)==72,"Spawn host sample");
static_assert(sizeof(PgrSpawnHostApi)==64,"Spawn host API");
extern "C" const void *pgrazor_spawn_host_api(uint32_t version);

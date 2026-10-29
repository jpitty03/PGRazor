#pragma once
#include "quickball_metadata.h"
#include "throws.h"
#include "unity_scheduler.h"
#include <stdint.h>

enum PgrQuickballHostStatus {
 PGR_QBH_OK=0,PGR_QBH_NOT_READY=1,PGR_QBH_EMPTY=2,
 PGR_QBH_BAD_CONFIG=-1,PGR_QBH_RECONFIG=-2,PGR_QBH_OFF_OWNER=-3,
 PGR_QBH_BUSY=-4,PGR_QBH_METADATA=-5,PGR_QBH_LEAF=-6,
 PGR_QBH_STOPPED=-7,PGR_QBH_OVERFLOW=-8,PGR_QBH_CLOCK=-9
};
enum PgrQuickballHostFlags {PGR_QBH_TRANSPORT=1};
enum PgrQuickballHostSites {PGR_QBH_SUBMIT=1,PGR_QBH_NAMEPLATE=2};
enum PgrQuickballHostCapabilities {
 PGR_QBH_SPEED=1,PGR_QBH_OBSERVERS=2,PGR_QBH_THROWS=4,
 PGR_QBH_ALWAYS_HIT=8,PGR_QBH_AR=16,PGR_QBH_MAP=32,PGR_QBH_IV=64
};
struct PgrQuickballHostRuntime {
 void *runtime_invoke,*gchandle_new,*gchandle_get_target,*gchandle_free;
 void *object_get_class,*object_unbox,*field_get_value,*class_is_assignable_from;
 void *object_new,*field_set_value,*string_new,*string_length,*string_chars,*type_get_object;
};
struct PgrQuickballHostConfig {
 uint32_t version,size,flags,reserved;
 const PgrQuickballMetaConfig *metadata;
 const PgrQuickballHostRuntime *runtime;
};
// Independent groups preserve original setters: the lease belongs to the settings
// update, never to the consumer tick. Deadline is measured by CLOCK_BOOTTIME.
struct PgrQuickballHostOptions {
 uint32_t version,size;
 uint32_t quick_allowed,throw_allowed,ar_allowed,map_allowed,iv_allowed;
 uint32_t grade,curve,always_hit,ar_enabled,map_mode,iv_catch,iv_owned;
 uint32_t lease_ms,reserved;
 uint64_t map_epoch;
};
struct PgrQuickballHostSample {
 uint32_t version,size,configured,metadata_ready,ready,stopping,capabilities,shared_sites;
 uint64_t owner_tid,ticks;
 uint32_t owned,queued;
 int32_t error,metadata_error;
};
struct PgrQuickballHostEvent {uint32_t version,size;uint64_t sequence;char kind[48];char json[1536];};
struct PgrQuickballHostApi {
 uint32_t version,size;
 int (*configure)(const PgrQuickballHostConfig *); // null selects bootstrap54+transport52
 int (*set_options)(const PgrQuickballHostOptions *);
 int (*shared_sites)(uint32_t installed_mask); // only0..3; attestation, not hook detection
 int (*tick)(uint64_t proven_tid);
 int (*stop)(); // revoke immediately; owner ticks restore/free before owned reaches0
 int (*sample)(PgrQuickballHostSample *);
 int (*next_event)(PgrQuickballHostEvent *);
 int (*rewrite_submit)(void *,PgrThrowValue *);
 // Output must hold TWO pointers: refreshed [nameplate, mapPokemon].
 uint64_t (*begin_catch)(void *,void *,uint64_t,void **);
 int (*end_catch)(uint64_t,uint64_t);
 PgrUnityWork on_unity_pulse;
};
static_assert(sizeof(PgrQuickballHostRuntime)==112,"Runtime ABI");
static_assert(sizeof(PgrQuickballHostConfig)==32,"Config ABI");
static_assert(sizeof(PgrQuickballHostOptions)==72,"Options ABI");
static_assert(sizeof(PgrQuickballHostSample)==64,"Sample ABI");
static_assert(sizeof(PgrQuickballHostEvent)==1600,"Event ABI");
static_assert(sizeof(PgrQuickballHostApi)==96,"API ABI");
extern "C" const void *pgrazor_quickball_host_api(uint32_t version);

#pragma once
#include "shared_metadata.h"
#include "unity_scheduler.h"
#include <stdint.h>
enum PgrSpinHostStatus {PGR_SPH_OK=0,PGR_SPH_EMPTY=1,PGR_SPH_BAD_CONFIG=-1,PGR_SPH_REBIND=-2,PGR_SPH_OFF_OWNER=-3,PGR_SPH_BUSY=-4,PGR_SPH_STOPPED=-5,PGR_SPH_FAULT=-6};
enum PgrSpinHostFlags {PGR_SPH_TRANSPORT=1};
enum PgrSpinHostCapabilities {PGR_SPH_CAMERA=1,PGR_SPH_QUICK=2};
struct PgrSpinHostConfig {
 uint32_t version,size,flags,exclusive_owner;
 const PgrSharedMetadataOutput *metadata;
 const PgrSharedMetaExtraExports *extra;
 void *object_get_class,*field_get_value,*runtime_invoke,*object_unbox;
 void *(*thread_current)();
 uint64_t owner_tid;
};
struct PgrSpinHostOptions {uint32_t version,size,enabled,lease_ms;uint64_t epoch;};
struct PgrSpinHostSample {
 uint32_t version,size,configured,capabilities,stopping,owned;
 int32_t camera_error,quick_error;
 uint64_t owner_tid,ticks;
};
struct PgrSpinHostEvent {uint32_t version,size;char kind[48],json[640];};
struct PgrSpinHostApi {
 uint32_t version,size;
 int (*configure)(const PgrSpinHostConfig*);
 int (*update)(const PgrSpinHostOptions*);
 int (*tick)(uint64_t tid,uint64_t sequence,uint64_t opener_depth);
 int (*stop)();
 int (*sample)(PgrSpinHostSample*);
 int (*next_event)(PgrSpinHostEvent*);
 PgrUnityWork on_unity_pulse;
};
static_assert(sizeof(PgrSpinHostConfig)==80,"Spin host config");
static_assert(sizeof(PgrSpinHostOptions)==24,"Spin host options");
static_assert(sizeof(PgrSpinHostSample)==48,"Spin host sample");
static_assert(sizeof(PgrSpinHostEvent)==696,"Spin host event");
static_assert(sizeof(PgrSpinHostApi)==64,"Spin host API");
extern "C" const void *pgrazor_spin_host_api(uint32_t version);

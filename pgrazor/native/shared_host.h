#pragma once
#include "shared_metadata.h"
#include "unity_scheduler.h"
#include "transfer_execution.h"
#include <stdint.h>

// Feature61. Native catch host; optional gameplay hosts are separate consumers.
enum PgrSharedHostCapability : uint32_t {
 PGR_SHH_SUMMARY=1, PGR_SHH_DETAILS=2, PGR_SHH_RECOVERY=4,
 PGR_SHH_CANVAS=8,PGR_SHH_DISCOVERY=16,PGR_SHH_TRANSFER=32,PGR_SHH_DIRECT=64
};
enum PgrSharedHostFlags : uint32_t { PGR_SHH_ALLOWED=1,PGR_SHH_DISMISS=2,PGR_SHH_TRANSFER_ENABLED=4,PGR_SHH_KEEP_ALL=8 };
struct PgrSharedHostConfig {
 uint32_t version,size;
 PgrSharedMetadataConfig metadata;
 void *object_get_class,*field_get_value,*runtime_invoke,*object_unbox,*string_length;
 uint32_t exclusive_hooks,transport_flags; // bit0: compiled52 settings + event delivery; no JS heartbeat/drain
};
static_assert(sizeof(PgrSharedHostConfig)==152,"LP64 host config");
struct PgrSharedHostOptions {
 uint32_t version,size,flags,protect;
 uint64_t epoch,remaining_ms;
 const int32_t *keep_ids;
 uint32_t keep_count,reserved;
};
static_assert(sizeof(PgrSharedHostOptions)==48,"LP64 options");
struct PgrSharedHostEvent {
 uint32_t version,size,kind,flags;
 int32_t status,before,after,reason;
 uint64_t serial,slot,tid;
};
static_assert(sizeof(PgrSharedHostEvent)==56,"LP64 event");
struct PgrSharedHostSample {
 uint32_t version,size,ready,capabilities;
 uint32_t stopping,fault,records,roots;
 uint64_t owner_tid,ticks,hook_frames;
};
static_assert(sizeof(PgrSharedHostSample)==56,"LP64 sample");
struct PgrSharedHostApi {
 uint32_t version,size;
 // nullptr: compiled54 snapshot + validated exports; caller must already be
 // attached on Unity owner thread and own exclusive hooks. Returns1 to retry.
 int (*configure)(const PgrSharedHostConfig*);
 int (*update)(const PgrSharedHostOptions*);
 int (*tick)(uint64_t tid,uint64_t sequence);
 int (*stop)();
 int (*sample)(PgrSharedHostSample*);
 int (*next_event)(PgrSharedHostEvent*);
 const PgrSharedMetadataOutput *(*metadata)();
 void (*on_unity_pulse)(void*,const PgrUnityPulse*);
};
static_assert(sizeof(PgrSharedHostApi)==72,"LP64 API");
// One transfer executor, policy epoch and FIFO owner for catch and VGP hosts.
// Owner-thread only. Callers retain their managed arguments across submit.
struct PgrSharedHostTransferApi {
 uint32_t version,size;
 int (*ready)();
 int (*submit)(const PgrTransferExecRequest*,uint64_t*);
 int (*next_event)(PgrTransferExecEvent*);
};
static_assert(sizeof(PgrSharedHostTransferApi)==32,"LP64 transfer broker API2");
extern "C" const void *pgrazor_shared_host_api(uint32_t version);

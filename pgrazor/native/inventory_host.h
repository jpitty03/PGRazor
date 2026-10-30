#pragma once
#include "shared_metadata.h"
#include "runtime_bootstrap.h"
#include "unity_scheduler.h"
#include <stdint.h>
enum PgrInventoryHostCaps : uint32_t {PGR_IH_ITEMS=1,PGR_IH_BUDDY=2,PGR_IH_INCUBATION=4,PGR_IH_INCOMING=8,PGR_IH_OUTGOING=16,PGR_IH_SOCIAL=32};
struct PgrInventoryRuntime {
 PgrBootRuntimeExports boot;
 void *string_length,*object_get_virtual_method,*class_get_type,*type_get_object,*runtime_class_init,*method_get_object;
};
struct PgrInventoryHostConfig {
 uint32_t version,size,flags,reserved; // flags1 native52 transport; null config enables it
 const PgrSharedMetadataOutput *metadata;
 const PgrInventoryRuntime *runtime;
};
struct PgrInventoryHostOptions {
 uint32_t version,size,item_enabled,item_allowed,item_catalog,buddy_allowed,incubator_enabled,incubator_longest,incubator_allowed,gift_mask,gift_allowed,reserved;
 int32_t buddy_berry;uint32_t lease_ms;
 const char *item_rules; // bounded ASCII id:cap; pairs, copied synchronously
};
struct PgrInventoryHostSample {uint32_t version,size,configured,ready,capabilities,stopping,roots,error;uint64_t owner_tid,ticks;};
struct PgrInventoryHostEvent {uint32_t version,size;uint64_t sequence;char json[65536];};
struct PgrInventoryHostApi {
 uint32_t version,size;
 int (*configure)(const PgrInventoryHostConfig*);
 int (*update)(const PgrInventoryHostOptions*);
 int (*tick)(uint64_t);
 int (*stop)();
 int (*sample)(PgrInventoryHostSample*);
 int (*next_event)(PgrInventoryHostEvent*);
 void (*request_item_poll)();
 void *(*acquire_item_service)(); // owned root; caller must release through IL2CPP
 void (*on_unity_pulse)(void*,const PgrUnityPulse*);
 int (*visibility_site)(uint32_t);
 void (*on_visibility)(void*,uint64_t);
};
static_assert(sizeof(PgrInventoryHostConfig)==32,"config");
static_assert(sizeof(PgrInventoryHostOptions)==64,"options");
static_assert(sizeof(PgrInventoryHostSample)==48,"sample");
static_assert(sizeof(PgrInventoryHostApi)==96,"API");
// Separate v2 table preserves the exact v1 layout. Tickets coalesce safely: a
// subsequent successful empty pass satisfies all earlier cleanup requests.
struct PgrInventoryCleanupApi {
 uint32_t version,size;
 uint64_t (*request)(); // Unity owner thread only; zero means unavailable; otherwise an opaque completion ticket
 int (*pending)(uint64_t); // 1 pending, 0 complete or unavailable
};
extern "C" const void *pgrazor_inventory_host_api(uint32_t);

#pragma once
#include "runtime_metadata.h"
#include "routes_schema.h"
#include <cstdint>

// Feature53/API1: compiled Routes class/field/method discovery. No hook or gameplay
// mutation. Caller supplies the real module and native IL2CPP exports once; all
// descriptors and validation decisions are compiled into the binary.
struct PgrRoutesMetadataConfig {
    uint32_t version,size;
    PgrMetaBindConfig binding;
    void *thread_current;
    uint64_t owner_tid;
};
struct PgrRoutesMetadataOutput {
    uint32_t version,size;
    const void *classes[pgr_routes_schema::kClassCount];
    const void *fields[pgr_routes_schema::kFieldCount];
    const void *methods[pgr_routes_schema::kMethodCount];
    const void *list_count,*list_item,*waypoint_count,*waypoint_item,*constructor;
};
struct PgrRoutesMetadataApi {
    uint32_t version,size,config_size,output_size;
    int (*resolve)(const PgrRoutesMetadataConfig *,PgrRoutesMetadataOutput *);
};
static_assert(sizeof(PgrRoutesMetadataConfig)==88,"LP64 config");
static_assert(sizeof(PgrRoutesMetadataApi)==24,"LP64 API");
extern "C" const void *pgrazor_routes_metadata_api(uint32_t version);

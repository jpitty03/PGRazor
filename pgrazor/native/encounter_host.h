#pragma once
#include <stddef.h>
#include <stdint.h>
#include "shared_metadata.h"

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "EncounterHost native ABI requires LP64");

enum PgrEncounterHostFeatureInfo {
  PGR_EH_FEATURE_ID  = 63,
  PGR_EH_API_VERSION = 1
};

enum PgrEncounterHostStatus {
  PGR_EH_OK                = 0,
  PGR_EH_RETRY             = 1,
  PGR_EH_ERR_NULL_ARG      = -1,
  PGR_EH_ERR_BAD_CONFIG    = -2,
  PGR_EH_ERR_ALREADY_BOUND = -3,
  PGR_EH_ERR_NOT_INSTALLED = -4,
  PGR_EH_ERR_HOOK          = -5,
  PGR_EH_ERR_LEAF          = -6,
  PGR_EH_ERR_METADATA      = -7,
  PGR_EH_ERR_NO_CLOCK      = -8
};

enum PgrEncounterHostCapabilityFlags {
  PGR_EH_CAP_BERRIES       = 1u << 0,
  PGR_EH_CAP_VGP           = 1u << 1,
  PGR_EH_CAP_VGP_MARKER    = 1u << 2,
  PGR_EH_CAP_MAP_POI       = 1u << 3,
  PGR_EH_CAP_MAP_POI_EXTRA = 1u << 4,
  PGR_EH_CAP_TRANSFER      = 1u << 5,
  PGR_EH_CAP_ITEMS         = 1u << 6
};

// Optional host callbacks for test/adapter observation (nullable in production when using Feature 52).
typedef void (*pgr_eh_emit_json_fn)(const char *event_type, const char *json_payload);
typedef void (*pgr_eh_set_berry_ready_fn)(int ready);
typedef void (*pgr_eh_set_encounter_berry_fn)(int active, int busy);
typedef void (*pgr_eh_set_cooldown_encounter_fn)(int active);
typedef void (*pgr_eh_cooldown_action_fn)(const char *action);
typedef void (*pgr_eh_set_vgp_ready_fn)(int ready, const char *reason);
typedef void (*pgr_eh_set_map_poi_snapshot_fn)(const char *json_array);

// Fixed LP64 configuration (264 bytes).
// Keeps exact layout constraints:
//   offsetof(PgrEncounterHostConfig, metadata_config) == 160
//   offsetof(PgrEncounterHostConfig, object_get_virtual_method) == 256
//   sizeof(PgrEncounterHostConfig) == 264
struct PgrEncounterHostConfig {
  uint32_t version;           // 0: == PGR_EH_API_VERSION (1)
  uint32_t size;              // 4: == sizeof(PgrEncounterHostConfig) (264)
  uint32_t require_all;       // 8: 0 = modular capability gating, non-zero = require all
  uint32_t capability_mask;   // 12: 0 = all available, else bitmask of PgrEncounterHostCapabilityFlags

  // 18 pointer slots (offsets 16..159 = 144 bytes):
  // Optional pre-resolved Feature 56 metadata and extra exports (slots 0..1).
  const PgrSharedMetadataOutput *resolved_metadata; // 16
  const PgrSharedMetaExtraExports *extra_exports;   // 24

  // IL2CPP runtime exports not in PgrMetaExports / PgrSharedMetaExtraExports (slots 2..10).
  void *runtime_invoke;         // 32: void* (MethodInfo*, void* obj, void** params, void** exc)
  void *object_get_class;       // 40: void* (void* obj)
  void *object_unbox;           // 48: void* (void* boxed)
  void *object_new;             // 56: void* (void* klass)
  void *field_get_value;        // 64: void  (void* obj, FieldInfo*, void* out)
  void *field_set_value;        // 72: void  (void* obj, FieldInfo*, void* val)
  void *field_set_value_object; // 80: void  (void* obj, FieldInfo*, void* val)
  void *string_length;          // 88: int32_t (void* str)
  void *string_chars;           // 96: const uint16_t* (void* str)

  // Optional host callbacks (slots 11..17).
  pgr_eh_emit_json_fn            emit_json;              // 104
  pgr_eh_set_berry_ready_fn      set_berry_ready;        // 112
  pgr_eh_set_encounter_berry_fn  set_encounter_berry;    // 120
  pgr_eh_set_cooldown_encounter_fn set_cooldown_encounter; // 128
  pgr_eh_cooldown_action_fn      cooldown_action;        // 136
  pgr_eh_set_vgp_ready_fn        set_vgp_ready;          // 144
  pgr_eh_set_map_poi_snapshot_fn set_map_poi_snapshot;   // 152

  // Embedded Feature 56 config (96 bytes at offset 160).
  PgrSharedMetadataConfig metadata_config;               // 160..255

  // Virtual method resolver (8 bytes at offset 256).
  void *object_get_virtual_method;                       // 256..263
};

static_assert(offsetof(PgrEncounterHostConfig, metadata_config) == 160,
              "metadata_config must be at offset 160");
static_assert(offsetof(PgrEncounterHostConfig, object_get_virtual_method) == 256,
              "object_get_virtual_method must be at offset 256");
static_assert(sizeof(PgrEncounterHostConfig) == 264,
              "PgrEncounterHostConfig must be 264 bytes on LP64");

// Runtime options snapshot (96 bytes on LP64).
struct PgrEncounterHostOptions {
  uint32_t version;                  // 0: == PGR_EH_API_VERSION (1)
  uint32_t size;                     // 4: == sizeof(PgrEncounterHostOptions) (96)

  uint32_t berries_allowed;          // 8
  uint32_t berry_priority_count;     // 12: 0..3
  int32_t  berry_priority_ids[3];    // 16..27
  uint32_t vgp_allowed;              // 28

  uint32_t vgp_enabled;              // 32
  uint32_t vgp_catch_allowed;        // 36
  uint32_t vgp_spin_allowed;         // 40
  uint32_t vgp_map_visible;          // 44

  uint32_t map_poi_allowed;          // 48
  uint32_t auto_transfer_allowed;    // 52
  uint32_t item_transfer_enabled;    // 56
  uint32_t item_access_allowed;      // 60

  int32_t  transfer_protect;         // 64
  uint32_t transfer_keep_all;        // 68
  uint64_t lease_remaining_ms;       // 72: clamped to 2500ms

  double   lat;                      // 80
  double   lon;                      // 88
};

static_assert(sizeof(PgrEncounterHostOptions) == 96,
              "PgrEncounterHostOptions must be 96 bytes on LP64");

// Pull-only telemetry snapshot (64 bytes on LP64).
struct PgrEncounterHostSample {
  uint32_t version;              // 0: == PGR_EH_API_VERSION
  uint32_t size;                 // 4: == sizeof(PgrEncounterHostSample) (64)
  uint32_t installed;            // 8
  uint32_t active_capabilities;  // 12: bitmask of PgrEncounterHostCapabilityFlags
  uint64_t tick_serial;          // 16
  uint64_t berry_resume_serial;  // 24
  uint64_t vgp_tick_serial;      // 32
  uint64_t map_poi_scan_serial;  // 40
  uint64_t transfer_eval_serial; // 48
  uint32_t last_map_poi_count;   // 56
  int32_t  last_error;           // 60
};

static_assert(sizeof(PgrEncounterHostSample) == 64,
              "PgrEncounterHostSample must be 64 bytes on LP64");

// Feature 63 API table (72 bytes on LP64).
struct PgrEncounterHostApi {
  uint32_t version; // == PGR_EH_API_VERSION
  uint32_t size;    // == sizeof(PgrEncounterHostApi) (72)
  int (*configure)(const PgrEncounterHostConfig *config);
  int (*install)(void);
  int (*update_options)(const PgrEncounterHostOptions *options, uint64_t proven_tid);
  int (*set_keep_species)(const int32_t *species_ids, uint32_t count);
  int (*tick)(uint64_t proven_tid);
  int (*stop)(uint64_t proven_tid);
  int (*sample)(PgrEncounterHostSample *out);
  int (*poll_event)(char *type_buf, uint32_t type_cap, char *json_buf, uint32_t json_cap);
};

static_assert(sizeof(PgrEncounterHostApi) == 72,
              "PgrEncounterHostApi must be 72 bytes on LP64");

const void *pgrazor_encounter_host_api(uint32_t version);

#ifdef __cplusplus
}
#endif

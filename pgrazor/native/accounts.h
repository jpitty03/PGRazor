#ifndef PGR_ACCOUNTS_H_
#define PGR_ACCOUNTS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PGR_FEATURE_ACCOUNTS 16u
#define PGR_ACCOUNTS_API_VERSION 1u
#define PGR_ACCOUNTS_API_TABLE_SIZE 56u
#define PGR_ACCOUNTS_MAX_UTF16 256u
#define PGR_ACCOUNTS_EVENT_QUEUE_CAP 64u
#define PGR_ACCOUNTS_MAX_LEASE_MS 2000u
#define PGR_ACCOUNTS_TICK_INTERVAL_MS 250u
#define PGR_ACCOUNTS_ERROR_BACKOFF_MS 5000u
#define PGR_ACCOUNTS_IDENTITY_HEARTBEAT_MS 5000u
#define PGR_ACCOUNTS_LOGOUT_TIMEOUT_MS 15000u

typedef struct Il2CppClass Il2CppClass;
typedef struct Il2CppObject Il2CppObject;
typedef struct Il2CppType Il2CppType;
typedef struct MethodInfo MethodInfo;
typedef struct FieldInfo FieldInfo;
typedef struct Il2CppString Il2CppString;

typedef enum pgrazor_account_cmd_kind : uint32_t {
  PGR_ACC_CMD_NONE = 0,
  PGR_ACC_CMD_SAVE_SESSION = 1,
  PGR_ACC_CMD_RESET_LOGIN = 2,
  PGR_ACC_CMD_LOGOUT = 3
} pgrazor_account_cmd_kind_t;

typedef enum pgrazor_account_event_kind : uint32_t {
  PGR_ACC_EVT_NONE = 0,
  PGR_ACC_EVT_READY = 1,
  PGR_ACC_EVT_SESSION_ENDED = 2,
  PGR_ACC_EVT_LOGOUT_REQUESTED = 3,
  PGR_ACC_EVT_LOGOUT_COMPLETE = 4,
  PGR_ACC_EVT_LOGOUT_FAILED = 5,
  PGR_ACC_EVT_SESSION_SAVED = 6,
  PGR_ACC_EVT_LOGIN_RESET_COMPLETE = 7,
  PGR_ACC_EVT_IDENTITY = 8,
  PGR_ACC_EVT_READ_ERROR = 9
} pgrazor_account_event_kind_t;

typedef enum pgrazor_account_reason_code : uint32_t {
  PGR_ACC_REASON_NONE = 0,
  PGR_ACC_REASON_CLEARED_AUTH_STATE = 1,
  PGR_ACC_REASON_SESSION_SAVE_UNAVAILABLE = 2,
  PGR_ACC_REASON_LOGIN_RESET_UNAVAILABLE = 3,
  PGR_ACC_REASON_LOGIN_RESET_NOT_SAVED = 4,
  PGR_ACC_REASON_LOGOUT_WRAPPER_INVALID = 5,
  PGR_ACC_REASON_LOGOUT_AUTH_INVALID = 6,
  PGR_ACC_REASON_LOGOUT_FUTURE_INVALID = 7,
  PGR_ACC_REASON_LOGOUT_FAILED = 8,
  PGR_ACC_REASON_LOGOUT_TIMEOUT = 9,
  PGR_ACC_REASON_MISSING_AUTH_STATE = 10,
  PGR_ACC_REASON_INVOCATION_EXCEPTION = 11,
  PGR_ACC_REASON_LEASE_EXPIRED = 12,
  PGR_ACC_REASON_REENTRANT_ABORT = 13,
  PGR_ACC_REASON_QUEUE_OVERFLOW = 14
} pgrazor_account_reason_code_t;

typedef struct pgrazor_accounts_config_v1 {
  Il2CppObject* (*runtime_invoke)(const MethodInfo* method, void* obj, void** params, Il2CppObject** exc);
  uintptr_t (*gchandle_new)(Il2CppObject* obj, bool pinned);
  Il2CppObject* (*gchandle_get_target)(uintptr_t gchandle);
  void (*gchandle_free)(uintptr_t gchandle);
  Il2CppClass* (*object_get_class)(Il2CppObject* obj);
  void* (*object_unbox)(Il2CppObject* obj);
  const Il2CppType* (*class_get_type)(Il2CppClass* klass);
  Il2CppObject* (*type_get_object)(const Il2CppType* type);
  Il2CppString* (*string_new)(const char* str);
  int32_t (*string_length)(Il2CppString* str);
  const uint16_t* (*string_chars)(Il2CppString* str);
  void (*field_get_value)(Il2CppObject* obj, FieldInfo* field, void* value);

  Il2CppClass* boolean_class;
  Il2CppClass* string_class;
  Il2CppClass* future_class;
  Il2CppClass* map_scene_context_class;
  Il2CppClass* di_container_class;
  Il2CppClass* player_class;
  Il2CppClass* player_contract_class;
  Il2CppClass* auth_class;
  Il2CppClass* auth_contract_class;
  Il2CppClass* wrapper_class;
  Il2CppClass* wrapper_contract_class;
  Il2CppClass* prefs_class;

  FieldInfo* auth_active_provider_field;
  FieldInfo* future_status_field;
  FieldInfo* future_disposed_field;

  const MethodInfo* find_object_of_type_method;
  const MethodInfo* map_get_container_method;
  const MethodInfo* container_resolve_method;
  const MethodInfo* player_get_id_method;
  const MethodInfo* player_get_name_method;
  const MethodInfo* auth_get_connected_method;
  const MethodInfo* auth_get_cached_method;
  const MethodInfo* wrapper_logout_method;
  const MethodInfo* auth_logout_method;
  const MethodInfo* prefs_has_key_method;
  const MethodInfo* prefs_delete_key_method;
  const MethodInfo* prefs_save_method;

  void* auth_logout_target_fn;
  int32_t promise_succeeded_value;
  int32_t promise_failed_value;
} pgrazor_accounts_config_v1_t;

typedef struct pgrazor_account_event_v1 {
  uint64_t seq;
  uint64_t timestamp_ms;
  uint32_t kind;
  uint32_t reason;
  uint8_t available;
  uint8_t future_captured;
  uint16_t id_len;
  uint16_t name_len;
  uint16_t reserved;
  uint16_t id[PGR_ACCOUNTS_MAX_UTF16];
  uint16_t name[PGR_ACCOUNTS_MAX_UTF16];
} pgrazor_account_event_v1_t;

static_assert(sizeof(pgrazor_account_event_v1_t) == 1056, "event POD layout mismatch");

typedef struct pgrazor_account_snapshot_v1 {
  uint64_t seq;
  uint64_t last_identity_ms;
  uint64_t last_accepted_token;
  uint64_t last_consumed_token;
  int32_t owner_tid;
  uint32_t active_depth;
  uint32_t active_gc_roots;
  uint8_t configured;
  uint8_t hook_installed;
  uint8_t hook_latched_fail;
  uint8_t logging_out;
  uint8_t queue_overflow_latched;
  uint8_t has_identity;
  uint16_t id_len;
  uint16_t name_len;
  uint16_t reserved;
  uint16_t id[PGR_ACCOUNTS_MAX_UTF16];
  uint16_t name[PGR_ACCOUNTS_MAX_UTF16];
} pgrazor_account_snapshot_v1_t;

static_assert(sizeof(pgrazor_account_snapshot_v1_t) == 1080, "snapshot POD layout mismatch");

typedef struct pgrazor_accounts_api_v1 {
  uint32_t version;
  uint32_t size;
  bool (*api_init)(const pgrazor_accounts_config_v1_t* cfg);
  bool (*api_update)(bool allowed, uint32_t lease_ms, uint32_t cmd_kind, uint64_t cmd_token);
  void (*api_tick)(uint64_t now_ms, int32_t caller_tid);
  bool (*api_clear)(void);
  void (*api_sample)(pgrazor_account_snapshot_v1_t* out_snapshot);
  size_t (*api_drain)(pgrazor_account_event_v1_t* out_events, size_t max_events);
} pgrazor_accounts_api_v1_t;

#if defined(__LP64__) || UINTPTR_MAX == 0xffffffffffffffffULL
static_assert(sizeof(void*) == 8, "LP64 ABI required");
static_assert(sizeof(uintptr_t) == 8, "LP64 uintptr_t required");
static_assert(sizeof(pgrazor_accounts_config_v1_t) == 41 * 8, "LP64 config layout mismatch");
static_assert(sizeof(pgrazor_accounts_api_v1_t) == PGR_ACCOUNTS_API_TABLE_SIZE, "LP64 api table layout mismatch");
#else
static_assert(sizeof(void*) == 4, "ILP32 ABI expected on 32-bit build");
static_assert(sizeof(pgrazor_accounts_config_v1_t) == 42 * 4, "ILP32 config layout mismatch");
static_assert(sizeof(pgrazor_accounts_api_v1_t) == 32, "ILP32 api table layout mismatch");
#endif

const pgrazor_accounts_api_v1_t* pgrazor_accounts_api(uint32_t version);

#ifdef __cplusplus
}
#endif

#endif  // PGR_ACCOUNTS_H_

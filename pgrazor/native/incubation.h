#pragma once
#include <stdint.h>

// PGRazor native migration: unlimited-incubator auto-incubation (feature id 9).
//
// Full port of runtime/features/incubation.js. The JS factory only ever drives the
// single permanent "unlimited" incubator, confirms every assignment through the live
// inventory (never from a bool), excludes special eggs, and chooses the shortest or
// longest eligible egg (first stable tie). This module owns that entire orchestration
// natively: it enumerates the player's incubators and the bag's eggs through the game's
// own il2cpp collections, resolves the incubator service, and issues exactly one
// PutEggInIncubator per confirmed opportunity. There are NO JS callbacks; the adapter
// (runtime/native-incubation.js) only packs the validated metadata into the config,
// drives tick() from the existing verified Unity heartbeat, and pulls telemetry.
//
// Division of labour with the JS adapter:
//   * JS resolves and validates every il2cpp pointer (export addresses, MethodInfo*,
//     class identities, field offsets, the two enum literals) BEFORE calling install().
//   * This module owns the GC rooting (RAII, fixed/bounded, never freed twice), the
//     bounded enumerators, the pending-assignment confirmation state machine, the poll
//     / deadline timers (CLOCK_BOOTTIME), the Unity-thread guard, and pull-only
//     telemetry. It never fabricates a root, never retries an uncertain request, and
//     fails closed on any size bound / unknown class / managed exception.
//
// No Dobby hooks are installed: the existing validated Unity tick calls native tick().

#ifdef __cplusplus
extern "C" {
#endif

// Feature id in the native pilot registry (coordinator wires the getter under this id).
#define PGRAZOR_INCUBATION_FEATURE_ID 9u

// Table + config ABI version. The getter answers an exact match only.
#define PGRAZOR_INCUBATION_API_VERSION 1u

// Bounds (fail closed on overflow). Mirror the JS limits exactly.
#define PGRAZOR_INCUBATION_MAX_INCUBATORS 128u
#define PGRAZOR_INCUBATION_MAX_EGGS       25000u
#define PGRAZOR_INCUBATION_MAX_STRLEN     256u

// Timers, milliseconds (CLOCK_BOOTTIME). 5s poll, 15s back-off with no eggs, 30s
// confirmation deadline; identical to the JS now+5000 / now+15000 / now+30000.
#define PGRAZOR_INCUBATION_POLL_MS     5000u
#define PGRAZOR_INCUBATION_NO_EGGS_MS  15000u
#define PGRAZOR_INCUBATION_DEADLINE_MS 30000u

// Hard cap on the per-tick authorization lease the host may hand us (ms). The host
// passes the RESIDUAL window; we clamp to this and build an absolute CLOCK_BOOTTIME
// deadline that is re-checked immediately before the single PutEggInIncubator.
#define PGRAZOR_INCUBATION_MAX_LEASE_MS 2500u

// Status codes surfaced through the sample. The adapter maps each to the exact JS
// status string; code 10 (STARTING) carries the chosen distance in the sample.
enum {
    PGRAZOR_INC_STATUS_NONE          = 0,  /* initial; no event emitted */
    PGRAZOR_INC_STATUS_OFF           = 1,  /* "Off" */
    PGRAZOR_INC_STATUS_WAIT_MAP      = 2,  /* "Waiting for the map" */
    PGRAZOR_INC_STATUS_EGG_INCUBATING= 3,  /* "Egg incubating" */
    PGRAZOR_INC_STATUS_ALREADY_IN_USE= 4,  /* "Incubator already in use" */
    PGRAZOR_INC_STATUS_UNCONFIRMED   = 5,  /* "Assignment unconfirmed; waiting for inventory" */
    PGRAZOR_INC_STATUS_CONFIRMING    = 6,  /* "Confirming egg assignment" */
    PGRAZOR_INC_STATUS_STOPPED_RETRY = 7,  /* "Incubation stopped; toggle off/on to retry" */
    PGRAZOR_INC_STATUS_NO_FREE       = 8,  /* "Unlimited incubator in use or unavailable" */
    PGRAZOR_INC_STATUS_NO_EGGS       = 9,  /* "No eggs waiting" */
    PGRAZOR_INC_STATUS_STARTING      = 10, /* "Starting <distance> km egg" */
    PGRAZOR_INC_STATUS_ERROR_RETRY   = 11  /* "Incubation unavailable; toggle off/on to retry" */
};

// Status codes returned by every table function. 0 == success.
enum {
    PGRAZOR_INC_OK            = 0,
    PGRAZOR_INC_ERR_NULL_ARG  = 1,  /* required pointer argument was null */
    PGRAZOR_INC_ERR_BAD_CONFIG= 2,  /* config header/size/pointer/literal invalid */
    PGRAZOR_INC_ERR_ALREADY_BOUND = 3, /* install() called again with a different config */
    PGRAZOR_INC_ERR_NOT_INSTALLED = 6  /* operation needs a successful install() */
};

// Install-time configuration, packed by the JS adapter after its metadata validation.
// Fixed LP64 layout (8-byte fields); the adapter writes it byte-for-byte and sets
// size == sizeof(pgrazor_incubation_config_v1). Every pointer is an already-validated
// il2cpp export, managed MethodInfo*, class pointer or FieldInfo*; the native layer
// re-checks the header and rejects any null pointer or invalid literal before use.
typedef struct pgrazor_incubation_config_v1 {
    uint32_t version;              /* == PGRAZOR_INCUBATION_API_VERSION */
    uint32_t size;                 /* == sizeof(pgrazor_incubation_config_v1) */

    /* il2cpp runtime exports (pointer-sized GC-handle ABI; real module exports). */
    void *runtime_invoke;          /* void* (MethodInfo*, void* obj, void** params, void** exc) */
    void *gchandle_new;            /* void* (void* obj, int pinned) -> pointer-sized handle */
    void *gchandle_get_target;     /* void* (void* handle) */
    void *gchandle_free;           /* void  (void* handle) */
    void *object_get_class;        /* void* (void* obj) */
    void *object_unbox;            /* void* (void* boxed) */
    void *string_length;           /* int32_t (void* str) */
    void *string_chars;            /* uint16_t* (void* str) */
    void *object_get_virtual_method; /* void* (void* obj, MethodInfo*) */
    void *class_get_type;          /* void* (void* klass) */
    void *type_get_object;         /* void* (void* type) -> System.Type object */
    void *field_get_value;         /* void  (void* obj, FieldInfo*, void* out) */

    /* Managed MethodInfo* invoked via runtime_invoke (direct) ... */
    void *m_find;                  /* Object.FindObjectOfType(System.Type) [static] */
    void *m_container;             /* SceneContext.get_Container() */
    void *m_resolve;              /* DiContainer.Resolve(System.Type) */
    void *m_incubators;            /* Player.get_Incubators() */
    void *m_eggs;                  /* IItemBag.get_AllPokemon() */
    void *m_put;                   /* IIncubatorService.PutEggInIncubator(Int64, inc) */
    /* ... and via object_get_virtual_method (interface dispatch). */
    void *m_enumerate;             /* IEnumerable.GetEnumerator() */
    void *m_move;                  /* IEnumerator.MoveNext() */
    void *m_current;               /* IEnumerator.get_Current() */
    void *m_dispose;               /* IDisposable.Dispose() */

    /* Class identities (exact object_get_class checks / type lookups). The two
       appended at the tail (class_boolean/class_string) keep every existing field
       offset fixed; see the layout static_asserts in incubation.cpp. */
    void *class_map;               /* MapSceneContext (FindObjectOfType target) */
    void *class_player_contract;   /* IPlayerProfile contract (Resolve type) */
    void *class_player;            /* concrete player (exact check) */
    void *class_bag_contract;      /* IItemBag contract (Resolve type) */
    void *class_bag;               /* concrete bag (exact check) */
    void *class_inc;               /* EggIncubatorProto (exact check) */
    void *class_pk;                /* PokemonProto (exact check) */
    void *class_service_contract;  /* IIncubatorService contract (Resolve type) */
    void *class_service;           /* IncubatorService concrete (exact check) */

    /* FieldInfo* read via field_get_value. */
    void *f_inc_item;              /* EggIncubatorProto.Item (int enum) */
    void *f_inc_id;                /* EggIncubatorProto.ItemId (string) */
    void *f_inc_pokemon;           /* EggIncubatorProto.PokemonId (int64) */
    void *f_inc_expiry;            /* EggIncubatorProto.Unconverted...ExpirationTimeMs (int64) */
    void *f_egg_is;                /* PokemonProto.isEgg_ (bool) */
    void *f_egg_type;              /* PokemonProto.eggType_ (int enum) */
    void *f_egg_assignment;        /* PokemonProto.eggIncubatorId_ (string) */
    void *f_egg_id;                /* PokemonProto.id_ (uint64) */
    void *f_egg_target;            /* PokemonProto.eggKmWalkedTarget_ (double) */
    void *f_egg_start;             /* PokemonProto.eggKmWalkedStart_ (double) */

    /* Enum literals (int32): the only eligible item and the excluded egg type. */
    int32_t unlimited;             /* ItemId == IncubatorBasicUnlimited (offset 336) */
    int32_t special;               /* EggType == EggTypeSpecial (excluded) (offset 340) */

    /* Exact boxing/reference identities, appended after the int32 literals so the
       8 bytes of literals sit at 336/340 and these two pointers at 344/352 (size 360).
       class_boolean gates the MoveNext box unbox; class_string gates every managed
       string field read (ItemId / egg assignment). */
    void *class_boolean;           /* System.Boolean (exact check before unbox) (offset 344) */
    void *class_string;            /* System.String  (exact check before chars)  (offset 352) */
} pgrazor_incubation_config_v1;

// Pull-only telemetry snapshot (48 bytes). status_serial increments on each distinct
// status transition; request_serial increments once per issued PutEggInIncubator (the
// moment the invocation is attempted, even if it throws); fault_serial increments on
// each managed-exception / fail-closed error. distance and longest describe the most
// recent request (and the STARTING status message). owned_roots exposes the number of
// durable pending GC roots currently held so the coordinator can gate a finish/teardown
// on this layer having released them.
typedef struct pgrazor_incubation_sample_v1 {
    uint32_t version;              /* == PGRAZOR_INCUBATION_API_VERSION */
    uint32_t size;                 /* == sizeof(pgrazor_incubation_sample_v1) */
    uint32_t status;               /* one of PGRAZOR_INC_STATUS_* */
    uint32_t status_serial;        /* bumped on each status change */
    uint64_t request_serial;       /* bumped on each issued request */
    double   distance;             /* km of the most recent choice/request */
    uint32_t longest;              /* 1 == longest priority, 0 == shortest */
    uint32_t fault_serial;         /* bumped on each fail-closed error */
    uint32_t owned_roots;          /* durable pending GC roots currently held (0..4) */
    uint32_t reserved;             /* 0; keeps the snapshot 8-byte aligned at 48 bytes */
} pgrazor_incubation_sample_v1;

/*
 * install(config): validate and publish the config once for the process lifetime. A
 * repeat install of the identical config is OK; a different config returns
 * ALREADY_BOUND. On any header/size/pointer/literal problem returns BAD_CONFIG. No
 * process image is mutated and no managed call is made here.
 */
typedef int (*pgrazor_incubation_install_fn)(const pgrazor_incubation_config_v1 *config);

/*
 * tick(enabled, longest, remaining_lease_ms, proven_tid): one orchestration pass. Runs
 * the full unlimited-incubator flow ONLY when proven_tid is non-zero and equals the
 * current OS thread (the host has already proven this is the Unity thread) AND no other
 * caller already owns a pass (a non-blocking owner-tid guard; a nested/foreign call is a
 * no-op). enabled and longest carry the current settings; remaining_lease_ms is the
 * residual (incubatorLease - now) window, clamped here to PGRAZOR_INCUBATION_MAX_LEASE_MS
 * and turned into an absolute CLOCK_BOOTTIME deadline re-checked immediately before the
 * single PutEggInIncubator (0 == expired -> no managed work). Off (enabled==0) reports
 * "Off" and keeps any pending assignment; the host calls clear() for stop. Returns OK or
 * NOT_INSTALLED.
 */
typedef int (*pgrazor_incubation_tick_fn)(int enabled, int longest,
                                          uint64_t remaining_lease_ms, uint64_t proven_tid);

/*
 * clear(proven_tid): Unity-thread-only teardown for stop/detach. RELEASES the durable
 * pending GC roots but PRESERVES the uncertain pending assignment IDs (by value) so that
 * after a JS restart the next pass still requires live-inventory confirmation and never
 * auto-duplicates the request. Called off the proven thread, or by a caller that is not
 * the real owner of an in-flight pass, it is a no-op (never frees managed handles off the
 * Unity thread or out from under a running pass; a reentrant clear from inside the owner's
 * own pass defers the free until that pass finishes). Returns OK or NOT_INSTALLED.
 */
typedef int (*pgrazor_incubation_clear_fn)(uint64_t proven_tid);

/* sample(out): copy the latest telemetry snapshot. Safe from any thread. */
typedef int (*pgrazor_incubation_sample_fn)(pgrazor_incubation_sample_v1 *out);

// Ordered, read-only table advertised by the getter (40 bytes: header + 4 pointers).
typedef struct pgrazor_incubation_api_v1 {
    uint32_t version;              /* == PGRAZOR_INCUBATION_API_VERSION */
    uint32_t size;                 /* == sizeof(pgrazor_incubation_api_v1) */
    pgrazor_incubation_install_fn install;
    pgrazor_incubation_tick_fn    tick;
    pgrazor_incubation_clear_fn   clear;
    pgrazor_incubation_sample_fn  sample;
} pgrazor_incubation_api_v1;

/*
 * Hidden getter (NOT exported by the version script). The coordinator registers it in
 * the core getter registry under feature id 9; the loader hands the adapter the table
 * on an exact version match, else NULL. The table lives for the process lifetime.
 */
const void *pgrazor_incubation_api(uint32_t version);

#ifdef __cplusplus
} /* extern "C" */
#endif

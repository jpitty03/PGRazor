#pragma once
#include <stdint.h>

// PGRazor native migration: buddy auto-feeding (feature id 12).
//
// Full native port of runtime/features/buddy.js. The JS factory resolves the single
// BuddyStatsService through the game's own Map/DiContainer, confirms a chosen buddy and
// its hunger through the live service, and -- when the buddy is hungry, off cooldown, and
// the selected berry is in stock -- issues exactly one BuddyRpcService.Feed per hunger
// cycle. It re-arms when a later poll shows the buddy fed and then hungry again, never
// feeds more than six berries per cycle, and treats an uncertain outcome as un-retryable.
//
// Two methods are hooked (ONLY these two), exactly as the JS Interceptor did:
//   * Feed   -- detect a MANUAL feed (not one of ours): mark any pending request ambiguous,
//               block, and back off so we never race the player's own berry.
//   * result -- the generated BuddyRpcService feed-completion callback: capture the
//               Result of OUR pending request (matched by callback class + its captured
//               rpc field + the out-proto class), committed only after the original runs
//               (onEnter/onLeave parity).
// Both hooks ALWAYS chain their original; the trampoline is published under the install
// mutex and a racing early call waits for it. A partial install failure never unhooks a
// published trampoline and latches the failure for the process lifetime (no Gum fallback).
//
// Division of labour with runtime/native-buddy-feeding.js:
//   * JS validates and packs every il2cpp pointer (exports, MethodInfo*, hook addresses,
//     class identities, field offsets, the Success literal) BEFORE install(), drives tick()
//     from the verified Unity heartbeat, and turns pull-only telemetry into the existing
//     buddy-* events. On a confirmed native result it sets getItemState().next = 0.
//   * This module owns all GC rooting (RAII, bounded, freed once), the service resolution,
//     the hunger / cooldown / stock / feed-limit state machine, the pending-request
//     confirmation (sync or async via the result hook), the CLOCK_BOOTTIME timers, the
//     Unity-thread guard, and the two hooks. It never calls back into JS.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "Buddy feeding native ABI requires LP64");

// Feature id in the native pilot registry (coordinator wires the getter under this id).
#define PGRAZOR_BUDDY_FEATURE_ID 12u

// Table + config + sample ABI version. The getter answers an exact match only.
#define PGRAZOR_BUDDY_API_VERSION 1u

// Timers, milliseconds (CLOCK_BOOTTIME). Mirror the JS now+5000 / now+30000 / now+15000.
#define PGRAZOR_BUDDY_POLL_MS        5000u
#define PGRAZOR_BUDDY_FEED_MS        30000u  // feed-result confirmation deadline
#define PGRAZOR_BUDDY_PROGRESS_MS    15000u  // hunger-update observation deadline
#define PGRAZOR_BUDDY_MANUAL_MS      30000u  // back-off after a manual feed
#define PGRAZOR_BUDDY_MAX_LEASE_MS   2500u   // per-tick authorization lease cap (residual)
#define PGRAZOR_BUDDY_MAX_FEEDS      6       // berries per hunger cycle before pausing

// Status codes surfaced through the sample. The adapter maps each to the exact JS string.
// FED_REMAINING carries whole minutes in `detail`; FEED_FAILED carries the result value.
enum {
    PGR_BUDDY_STATUS_NONE         = 0,  /* initial; no event emitted */
    PGR_BUDDY_STATUS_OFF          = 1,  /* "Off" */
    PGR_BUDDY_STATUS_WAIT_MAP     = 2,  /* "Waiting for the map" */
    PGR_BUDDY_STATUS_CHOOSE       = 3,  /* "Choose a buddy first" */
    PGR_BUDDY_STATUS_WAIT_HUNGER  = 4,  /* "Waiting for buddy hunger data" */
    PGR_BUDDY_STATUS_FED          = 5,  /* "Buddy fed - <detail> min remaining" */
    PGR_BUDDY_STATUS_COOLDOWN     = 6,  /* "Waiting for buddy feeding cooldown" */
    PGR_BUDDY_STATUS_PAUSED_SIX   = 7,  /* "Feeding paused after six berries; check your buddy" */
    PGR_BUDDY_STATUS_OUT_OF_STOCK = 8,  /* "Selected berry is out of stock" */
    PGR_BUDDY_STATUS_FEEDING      = 9,  /* "Feeding buddy" */
    PGR_BUDDY_STATUS_BERRY_FED    = 10, /* "Berry fed; checking buddy hunger" */
    PGR_BUDDY_STATUS_FEED_FAILED  = 11, /* "Buddy feed failed (result <detail>); toggle Off to retry" */
    PGR_BUDDY_STATUS_TIMEOUT      = 12, /* "Buddy feed timed out; no automatic retry" */
    PGR_BUDDY_STATUS_HUNGER_STALE = 13, /* "Hunger did not update; toggle Off to retry" */
    PGR_BUDDY_STATUS_ERROR        = 14  /* "Buddy feeding paused: <reason>" */
};

// Status codes returned by every table function. 0 == success.
enum {
    PGR_BUDDY_OK            = 0,
    PGR_BUDDY_ERR_NULL_ARG  = 1,  /* required pointer argument was null */
    PGR_BUDDY_ERR_BAD_CONFIG= 2,  /* header/size/pointer/literal invalid */
    PGR_BUDDY_ERR_ALREADY_BOUND = 3, /* install() repeated with a different config */
    PGR_BUDDY_ERR_DUP_HOOK  = 4,  /* feed and result share a hook address */
    PGR_BUDDY_ERR_HOOK_FEED = 5,  /* Dobby could not hook/publish Feed */
    PGR_BUDDY_ERR_HOOK_RESULT = 6, /* Dobby could not hook/publish the result callback */
    PGR_BUDDY_ERR_NOT_INSTALLED = 7, /* operation needs a successful install() */
    PGR_BUDDY_ERR_NO_CLOCK  = 8   /* CLOCK_BOOTTIME was unavailable at install */
};

// Install-time configuration, packed by the JS adapter after its metadata validation.
// Fixed LP64 layout (8-byte fields; see the static_asserts in buddy_feeding.cpp). Every
// pointer is an already-validated il2cpp export, MethodInfo*, hook address, class pointer
// or FieldInfo*; the native layer re-checks the header and rejects any null before use.
typedef struct pgrazor_buddy_config_v1 {
    uint32_t version;              /* == PGRAZOR_BUDDY_API_VERSION */
    uint32_t size;                 /* == sizeof(pgrazor_buddy_config_v1) */

    /* il2cpp runtime exports (pointer-sized GC-handle ABI). */
    void *runtime_invoke;          /* void* (MethodInfo*, void* obj, void** params, void** exc) */
    void *gchandle_new;            /* void* (void* obj, int pinned) -> pointer-sized handle */
    void *gchandle_get_target;     /* void* (void* handle) */
    void *gchandle_free;           /* void  (void* handle) */
    void *object_get_class;        /* void* (void* obj) */
    void *object_unbox;            /* void* (void* boxed) */
    void *class_get_type;          /* void* (void* klass) */
    void *type_get_object;         /* void* (void* type) -> System.Type object */
    void *field_get_value;         /* void  (void* obj, FieldInfo*, void* out) */

    /* Managed MethodInfo* invoked via runtime_invoke. */
    void *m_find;                  /* Object.FindObjectOfType(System.Type) [static] */
    void *m_container;             /* SceneContext.get_Container() */
    void *m_resolve;               /* DiContainer.Resolve(System.Type) */
    void *m_has;                   /* BuddyPokemonService.get_HasBuddy() -> bool */
    void *m_pokemon;               /* BuddyPokemonService.get_BuddyPokemon() -> BuddyPokemonProto */
    void *m_stats;                 /* BuddyStatsService.get_BuddyStats() -> BuddyObservedData */
    void *m_remaining;             /* BuddyObservedDataExt.OnMapRemainingSec(stats) -> float [static] */
    void *m_fullness;              /* BuddyObservedDataExt.PreMapFullnessPercentage(stats) -> float [static] */
    void *m_cooldown;              /* BuddyObservedDataExt.InFullnessCooldown(stats) -> bool [static] */
    void *m_quantity;              /* IItemBag.GetItemCount(ItemId) -> int */
    void *m_feed;                  /* BuddyRpcService.Feed(ItemId, int) -> IPromise (also hooked) */
    void *m_alive;                 /* UnityEngine.Object.op_Implicit(Object) -> bool [static] */

    /* Dobby hook targets (ONLY these two are hooked). */
    void *feed_address;            /* BuddyRpcService.Feed code address */
    void *result_address;          /* generated feed-completion callback code address */

    /* Exact Il2CppClass* identities. */
    void *class_map;               /* FindObjectOfType target (the lookup map) */
    void *class_contract;          /* IBuddyStatsService contract (Resolve type) */
    void *class_stats_service;     /* BuddyStatsService concrete (exact check) */
    void *class_pokemon_service;   /* BuddyPokemonService (service.pokemon exact check) */
    void *class_rpc;               /* BuddyRpcService (service.rpc exact check) */
    void *class_buddy_proto;       /* BuddyPokemonProto (get_BuddyPokemon exact check) */
    void *class_observed;          /* BuddyObservedData (get_BuddyStats exact check) */
    void *class_bag;               /* ItemBagImpl (rpc.bag exact check) */
    void *class_bool;              /* System.Boolean (box check before unbox) */
    void *class_int;               /* System.Int32 (box check before unbox) */
    void *class_single;            /* System.Single (box check before unbox) */
    void *class_callback;          /* generated callback class (result hook a[0]) */
    void *class_out_proto;         /* BuddyFeedingOutProto (result hook a[1]) */

    /* Il2CppFieldInfo* read via field_get_value. */
    void *f_rpc;                   /* BuddyStatsService -> BuddyRpcService */
    void *f_pokemon;               /* BuddyStatsService -> BuddyPokemonService */
    void *f_bag;                   /* BuddyRpcService -> IItemBag */
    void *f_callback_rpc;          /* callback.<>4__this -> the issuing BuddyRpcService */
    void *f_result;                /* BuddyFeedingOutProto.Result (enum i32) */
    void *f_buddy_id;              /* BuddyPokemonProto.BuddyPokemonId (uint64) */

    int32_t success;               /* BuddyFeedingOutProto.Result == Success literal */
    int32_t reserved;              /* 0; keeps the config 8-byte aligned (size 352) */
} pgrazor_buddy_config_v1;

// Pull-only telemetry snapshot (56 bytes). status_serial increments on each distinct
// status transition; feed_request_serial once per issued Feed (counted at issuance, even
// if the managed call then throws); result_serial once per consumed feed result (ok OR
// fail) so the adapter can zero getItemState().next; service_serial once per freshly
// resolved service; fault_serial on each managed-exception / fail-closed error. owned_roots
// is the number of durable GC roots held (service + pending rpc + pending promise) so the
// coordinator can gate a finish/teardown on this layer having released them.
typedef struct pgrazor_buddy_sample_v1 {
    uint32_t version;              /* == PGRAZOR_BUDDY_API_VERSION */
    uint32_t size;                 /* == sizeof(pgrazor_buddy_sample_v1) */
    uint32_t status;               /* one of PGR_BUDDY_STATUS_* */
    uint32_t status_serial;        /* bumped on each status change */
    int32_t  detail;               /* minutes (FED) or result value (FEED_FAILED) */
    uint32_t reserved;             /* 0 */
    uint64_t feed_request_serial;  /* bumped on each issued Feed */
    uint64_t result_serial;        /* bumped on each consumed feed result */
    uint64_t service_serial;       /* bumped on each freshly resolved service */
    uint32_t fault_serial;         /* bumped on each fail-closed error */
    uint32_t owned_roots;          /* durable roots plus active owner frames (conservative finish guard) */
} pgrazor_buddy_sample_v1;

/*
 * install(config): validate and publish the config once for the process lifetime, then
 * install the two hooks. A repeat install of the identical config is OK; a different config
 * returns ALREADY_BOUND. On a header/size/pointer/literal problem returns BAD_CONFIG; on a
 * duplicate hook address DUP_HOOK; on a Dobby failure HOOK_FEED/HOOK_RESULT (the failed
 * attempt latches for the process lifetime and never unhooks a published trampoline).
 */
typedef int (*pgrazor_buddy_install_fn)(const pgrazor_buddy_config_v1 *config);

/*
 * tick(berry, remaining_lease_ms, proven_tid): one orchestration pass. Runs the full feed
 * flow ONLY when proven_tid is non-zero and equals the current OS thread (the host proved
 * the Unity thread) AND no other caller owns a pass (a non-blocking owner guard; a nested
 * tick or a foreign-thread call is a no-op). berry is the selected berry ItemId (0 == Off,
 * mirrors s.buddyBerry); a changed berry resets the per-cycle state. remaining_lease_ms is
 * the residual (buddyLease - now) window, clamped here to PGRAZOR_BUDDY_MAX_LEASE_MS and
 * turned into an absolute CLOCK_BOOTTIME deadline re-checked immediately before the single
 * Feed (0/expired -> no managed feed). Returns OK or NOT_INSTALLED.
 */
typedef int (*pgrazor_buddy_tick_fn)(int32_t berry, uint64_t remaining_lease_ms,
                                     uint64_t proven_tid);

/*
 * clear(proven_tid): Unity-thread-only teardown for stop/detach. RELEASES the durable GC
 * roots (service, pending rpc, pending promise) but PRESERVES any uncertain pending request
 * BY VALUE (deadline, fullness, an already-captured result) so a JS handler restart can
 * never resubmit an unknown request and never auto-duplicates it. Called off the proven
 * thread it is a no-op (never frees handles off the Unity thread). Returns OK or
 * NOT_INSTALLED.
 */
typedef int (*pgrazor_buddy_clear_fn)(uint64_t proven_tid);

/* sample(out): copy the latest telemetry snapshot. Safe from any thread; never frees. */
typedef int (*pgrazor_buddy_sample_fn)(pgrazor_buddy_sample_v1 *out);

// Ordered, read-only table advertised by the getter (40 bytes: header + 4 pointers).
typedef struct pgrazor_buddy_api_v1 {
    uint32_t version;              /* == PGRAZOR_BUDDY_API_VERSION */
    uint32_t size;                 /* == sizeof(pgrazor_buddy_api_v1) */
    pgrazor_buddy_install_fn install;
    pgrazor_buddy_tick_fn    tick;
    pgrazor_buddy_clear_fn   clear;
    pgrazor_buddy_sample_fn  sample;
} pgrazor_buddy_api_v1;

/*
 * Hidden getter (NOT exported by the version script). The coordinator registers it in the
 * core getter registry under feature id 12; the loader hands the adapter the table on an
 * exact version match, else NULL. The table lives for the process lifetime.
 */
const void *pgrazor_buddy_feeding_api(uint32_t version);

#ifdef __cplusplus
} /* extern "C" */
#endif

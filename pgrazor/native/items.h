#pragma once
#include <stdint.h>

// PGRazor native migration: inventory item catalog + auto-recycle (feature id 14).
//
// Full native port of runtime/features/items.js. The JS factory resolves the single
// IItemInventoryService through the game's own Map/DiContainer (an optional path), builds
// the two inventory filter delegates, enumerates the item bag's ItemData list on demand to
// publish changed catalog rows, and -- when auto-recycle is on and a recyclable item is over its
// user-configured cap -- issues exactly one IItemBag.RecycleItem per over-cap item. A result
// is confirmed only by matching the generated completion/error callback (by callback class,
// its captured bag field and the predicted item id), never by the returned promise. A manual
// recycle by the player blocks attribution and backs off. Disabled recycling scans only for
// explicit catalog demand; berry overlays read five quantities without listing inventory.
//
// Five methods are hooked (ONLY these five), exactly as the JS Interceptor did:
//   * init    -- IItemInventoryService ctor-ish init: capture the live service (onLeave).
//   * list    -- the service list method: capture the service AND the real filter delegates
//                the game passes (onEnter) so we can drive list ourselves before/after.
//   * recycle -- IItemBag.RecycleItem: detect a MANUAL recycle (not one of ours) -> block the
//                pending id, drop the pending request, back off (onEnter).
//   * result  -- generated recycle-completion callback (success/response path).
//   * error   -- generated recycle-error callback (string path).
// All five ALWAYS chain their original; trampolines are published under the install mutex and
// a racing early call waits for it. A partial install failure never unhooks a published
// trampoline and latches for the process lifetime (no Gum fallback).
//
// Division of labour with runtime/native-items.js:
//   * JS validates and packs every il2cpp pointer (exports, MethodInfo*, hook addresses, class
//     identities, field offsets, the func-param type and the Success literal) BEFORE install().
//     It drives tick() from the verified Unity heartbeat, supplies the user's cap rules + epoch
//     each tick (immutable install config; the rules are a separately-passed snapshot), turns
//     pull-only telemetry into the existing item-* events, maps item ids to names, and exposes a
//     compatible itemState object (.next force-poll, borrowed .service handle for readiness, the
//     scoped .acquireService() owned root for VGP, .vgpCleanupUntil).
//   * This module owns all GC rooting (RAII, bounded, freed once), the optional service/filter
//     resolution, the bounded catalog enumeration, the cap selection, the pending-request
//     confirmation (sync or async via the result/error hooks), the manual-recycle interference,
//     the CLOCK_BOOTTIME timers, the Unity-thread guard, and the five hooks. It never calls
//     back into JS and installs no Gum instrumentation.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "items native ABI requires LP64");

// Feature id in the native pilot registry (coordinator wires the getter under this id).
#define PGRAZOR_ITEMS_FEATURE_ID 14u

// Table + config + rules + sample ABI version. The getter answers an exact match only.
#define PGRAZOR_ITEMS_API_VERSION 1u

// Bounds (fail closed on overflow). Mirror the JS limits exactly where they exist.
#define PGRAZOR_ITEMS_MAX_LIST     2048u   // JS: count<0||count>2048 throws
#define PGRAZOR_ITEMS_MAX_ROWS     1024u   // distinct catalog rows retained (display only)
#define PGRAZOR_ITEMS_MAX_RULES    512u    // user cap entries accepted per snapshot
#define PGRAZOR_ITEMS_MAX_BLOCKED  512u    // ids paused until a settings/generation change (definite rejections)
#define PGRAZOR_ITEMS_MAX_QUAR     512u    // ids permanently quarantined after an UNCERTAIN outcome
#define PGRAZOR_ITEMS_MAX_QTY      100000  // JS: quantity>100000 skipped / current>100000 abort
#define PGRAZOR_ITEMS_MAX_EVENTS   64u     // bounded request/confirm/error queue; overflow fails closed

// Timers, milliseconds (CLOCK_BOOTTIME). Mirror the JS now+3000 / now+15000 / now+1000.
#define PGRAZOR_ITEMS_POLL_MS      3000u   // catalog re-poll cadence
#define PGRAZOR_ITEMS_BACKOFF_MS   15000u  // error / timeout / manual-interference back-off
#define PGRAZOR_ITEMS_CONFIRM_MS   15000u  // recycle confirmation deadline (now+15000)
#define PGRAZOR_ITEMS_SETTLE_MS    1000u   // next poll after a confirmed/failed result (now+1000)
#define PGRAZOR_ITEMS_MAX_LEASE_MS 2500u   // per-tick authorization lease cap (residual)

// Status codes returned by every table function. 0 == success.
enum {
    PGR_ITEMS_OK               = 0,
    PGR_ITEMS_ERR_NULL_ARG     = 1,  /* required pointer argument was null */
    PGR_ITEMS_ERR_BAD_CONFIG   = 2,  /* header/size/pointer/literal invalid */
    PGR_ITEMS_ERR_ALREADY_BOUND= 3,  /* install() repeated with a different config */
    PGR_ITEMS_ERR_DUP_HOOK     = 4,  /* two hook addresses collide */
    PGR_ITEMS_ERR_HOOK         = 5,  /* Dobby could not hook/publish one of the five sites */
    PGR_ITEMS_ERR_NOT_INSTALLED= 6,  /* operation needs a successful install() */
    PGR_ITEMS_ERR_NO_CLOCK     = 7,  /* CLOCK_BOOTTIME was unavailable at install */
    PGR_ITEMS_ERR_BAD_RULES    = 8   /* rules snapshot header/size/count invalid */
};

// Error reason codes surfaced in sample.last_error_code (adapter -> exact JS strings).
enum {
    PGR_ITEMS_REASON_NONE    = 0,
    PGR_ITEMS_REASON_TIMEOUT = 1,  /* "Timed out; item paused until app restart" */
    PGR_ITEMS_REASON_REJECTED= 2,  /* "Game rejected recycling; item paused until settings change" */
    PGR_ITEMS_REASON_NATIVE  = 3,  /* generic orchestration fault (reason:native) */
    PGR_ITEMS_REASON_LOOKUP  = 4   /* service/filter lookup failed (item-lookup-error) */
};

// One published catalog row (POD; read back through catalog()).
typedef struct pgrazor_item_row {
    int32_t id;     /* Holoholo.Rpc.Item enum value (>0) */
    int32_t count;  /* current quantity (0..PGRAZOR_ITEMS_MAX_QTY) */
} pgrazor_item_row;

// Event kinds carried by the bounded request/confirm/error queue (drain()). Each event owns its
// own id/amount so asynchronous records can never be misattributed by a shared "last_*" field.
enum {
    PGR_ITEMS_EV_REQUEST = 1,  /* item-transfer-requested: aux == kept cap */
    PGR_ITEMS_EV_CONFIRM = 2,  /* item-transfer-confirmed: aux == 0 */
    PGR_ITEMS_EV_ERROR   = 3   /* item-transfer-error / item-lookup-error: aux == PGR_ITEMS_REASON_* */
};

// One queued telemetry event (POD; FIFO; drained and consumed by drain()). The adapter maps each
// to the exact JS item-* event in order. A reused id never shares another event's amount.
typedef struct pgrazor_item_event {
    uint32_t kind;    /* PGR_ITEMS_EV_* */
    int32_t  id;      /* item id for this event */
    int32_t  amount;  /* recycled amount for this event */
    int32_t  aux;     /* REQUEST: kept cap; ERROR: reason code; CONFIRM: 0 */
} pgrazor_item_event;

// One user cap rule (POD; packed into the rules snapshot by the adapter).
typedef struct pgrazor_item_cap {
    int32_t id;     /* item id */
    int32_t cap;    /* keep-at-most quantity; auto-recycle the surplus */
} pgrazor_item_cap;

// Per-tick rules snapshot (immutable config stays separate). The adapter rebuilds this from
// s.itemRules / s.itemEnabled / s.itemEpoch and bumps generation whenever the serialized
// config (itemConfig+':'+itemEnabled) changes, so the native layer can clear the blocked set
// exactly when the JS factory did. Snapshotted atomically at the start of each pass.
typedef struct pgrazor_items_rules_v1 {
    uint32_t version;              /* == PGRAZOR_ITEMS_API_VERSION */
    uint32_t size;                 /* == sizeof(pgrazor_items_rules_v1) */
    uint32_t enabled;              /* s.itemEnabled (0/1) */
    uint32_t count;                /* number of valid entries (<= PGRAZOR_ITEMS_MAX_RULES) */
    uint64_t generation;           /* config generation; a change clears blocked */
    uint64_t epoch;                /* s.itemEpoch (recorded in pending; vestigial, kept for parity) */
    pgrazor_item_cap entries[PGRAZOR_ITEMS_MAX_RULES];
} pgrazor_items_rules_v1;

// Install-time configuration, packed by the JS adapter after its metadata validation. Fixed
// LP64 layout (8-byte fields; see the static_asserts in items.cpp). has_lookup gates the
// optional service/filter-construction group: when 0 those pointers are ignored (and must be
// null for config idempotency); when 1 they are all re-validated non-null. Every pointer is an
// already-validated il2cpp export, MethodInfo*, hook address, class pointer, FieldInfo* or
// Il2CppType*; the native layer re-checks the header and rejects any null before use.
typedef struct pgrazor_items_config_v1 {
    uint32_t version;              /* == PGRAZOR_ITEMS_API_VERSION */
    uint32_t size;                 /* == sizeof(pgrazor_items_config_v1) */
    uint32_t has_lookup;           /* 1 == the lookup group is provided and validated */
    uint32_t reserved0;            /* 0 */

    /* ---- CORE group (always required) ---------------------------------------- */
    /* il2cpp runtime exports. */
    void *runtime_invoke;          /* void* (MethodInfo*, void* obj, void** params, void** exc) */
    void *gchandle_new;            /* void* (void* obj, int pinned) */
    void *gchandle_get_target;     /* void* (void* handle) */
    void *gchandle_free;           /* void  (void* handle) */
    void *object_get_class;        /* void* (void* obj) */
    void *object_unbox;            /* void* (void* boxed) */
    void *field_get_value;         /* void  (void* obj, FieldInfo*, void* out) */

    /* Managed MethodInfo* invoked via runtime_invoke. */
    void *m_list;                  /* IItemInventoryService list(Func,bool,Func,Func,Action) -> List<ItemData> */
    void *m_count;                 /* List<ItemData>.get_Count() -> boxed int */
    void *m_get;                   /* List<ItemData>.get_Item(int) -> ItemData */
    void *m_quantity;              /* IItemBag.GetItemCount(Item) -> boxed int */
    void *m_expiring;              /* IItemInventoryService.get_ExpiringItemsCopy() -> ISet<Item> */
    void *m_recycle;               /* IItemBag.RecycleItem(ItemData, int, ISet<Item>) -> IPromise (also hooked) */
    void *m_proto;                 /* ItemProto get_Item() on a prediction proto (result matching) */

    /* Dobby hook targets (ONLY these five are hooked). */
    void *init_address;            /* service init method code address */
    void *list_address;            /* list method code address */
    void *recycle_address;         /* RecycleItem code address */
    void *result_address;          /* generated recycle-completion callback code address */
    void *error_address;           /* generated recycle-error callback code address */

    /* Exact Il2CppClass* identities. */
    void *class_service;           /* IItemInventoryService concrete (exact capture check) */
    void *class_bag;               /* IItemBag concrete (exact service.bag check) */
    void *class_data;              /* ItemData (exact per-row check) */
    void *class_callback;          /* generated completion/error callback class (result/error hook a[0]) */
    void *class_response;          /* IResponse concrete (exact result hook a[1]) */
    void *class_int;               /* System.Int32 (box check before unbox) */

    /* Il2CppFieldInfo* read via field_get_value. */
    void *f_bag;                   /* IItemInventoryService -> IItemBag */
    void *f_id;                    /* ItemData.item (enum i32) */
    void *f_count;                 /* ItemData.count (i32) */
    void *f_recyclable;            /* ItemData.recyclable (bool, offset 0x21) */
    void *f_callback_bag;          /* callback.<>4__this -> IItemBag (match) */
    void *f_prediction;            /* callback.prediction -> HoloInventoryItemProto */
    void *f_proto_id;              /* ItemProto.Item (enum i32) (match) */
    void *f_status;                /* IResponse.Result (enum i32) */

    /* ---- LOOKUP group (optional; validated iff has_lookup != 0) --------------- */
    /* il2cpp exports used only for the service/filter construction path. */
    void *class_get_type;          /* void* (void* klass) */
    void *type_get_object;         /* void* (void* type) -> System.Type */
    void *field_static_get_value;  /* void  (FieldInfo*, void* out) */
    void *runtime_class_init;      /* void  (void* klass) */
    void *method_get_object;       /* void* (MethodInfo*, void* klass) -> System.Reflection.MethodInfo */

    /* Managed MethodInfo* (lookup path). */
    void *m_find;                  /* Object.FindObjectOfType(System.Type) [static] */
    void *m_container;             /* SceneContext.get_Container() */
    void *m_resolve;               /* DiContainer.Resolve(System.Type) */
    void *m_create_delegate;       /* Delegate.CreateDelegate(Type, object, MethodInfo) [static] */
    void *m_usable;                /* gui filter method (delegate target method) */
    void *m_show;                  /* singleton filter method (delegate target method) */

    /* Exact Il2CppClass* identities (lookup path). */
    void *class_map;               /* FindObjectOfType target (the lookup map) */
    void *class_service_contract;  /* IItemInventoryService contract (Resolve type) */
    void *class_gui_contract;      /* IItemInventoryGuiService contract (Resolve type) */
    void *class_gui;               /* ItemInventoryGuiService concrete (exact + usable target) */
    void *class_singleton;         /* gui nested <>c singleton class (exact + show target) */

    /* Il2CppFieldInfo* (lookup path). */
    void *f_singleton_instance;    /* static <>9 field on the singleton class */

    /* Il2CppType* of the list method's first parameter (the Func<> delegate type). */
    void *func_type;               /* fnType source for CreateDelegate */

    /* ---- literals ------------------------------------------------------------ */
    int32_t success;               /* IResponse.Result == Success literal */
    int32_t reserved1;             /* 0; keeps the config 8-byte aligned (size 432) */
} pgrazor_items_config_v1;

// Pull-only telemetry snapshot (56 bytes). The two *_serial fields are edge-triggered (the
// adapter re-emits bag-ready / catalog on a transition). Per-event request/confirm/error data is
// NOT here any more -- it lives in the bounded event queue read by drain() so distinct serials
// can never share one id/amount. events_lost counts queue overflows (fail-closed signal).
// has_candidate tells the adapter whether to reset itemState.vgpCleanupUntil (JS: 0 when no
// candidate). catalog is read separately through catalog(); catalog_count is the current row
// count. owned_roots is durable roots (service + two filters + any pending bag) plus active owner
// frames (conservative finish guard).
typedef struct pgrazor_items_sample_v1 {
    uint32_t version;              /* == PGRAZOR_ITEMS_API_VERSION */
    uint32_t size;                 /* == sizeof(pgrazor_items_sample_v1) */
    uint64_t bag_ready_serial;     /* item-bag-ready (edge-triggered) */
    uint64_t catalog_serial;       /* catalog rows changed (edge-triggered) */
    uint64_t fault_serial;         /* fail-closed orchestration faults */
    uint64_t events_lost;          /* request/confirm/error events dropped on queue overflow */
    uint32_t has_candidate;        /* 1 == an over-cap recyclable item is selected */
    uint32_t owned_roots;          /* durable roots + active owner frames */
    uint32_t catalog_count;        /* current number of catalog rows */
    uint32_t reserved;             /* 0 */
} pgrazor_items_sample_v1;

/*
 * install(config): validate and publish the immutable config once for the process lifetime,
 * then install the five hooks. A repeat install of the identical config is OK; a different one
 * returns ALREADY_BOUND. BAD_CONFIG on a header/size/pointer/literal problem; DUP_HOOK if two
 * hook addresses collide; HOOK on a Dobby failure (the failed attempt latches and never unhooks
 * a published trampoline). NO_CLOCK if CLOCK_BOOTTIME is unavailable. No managed call here.
 */
typedef int (*pgrazor_items_install_fn)(const pgrazor_items_config_v1 *config);

/*
 * tick(rules, force_poll, remaining_lease_ms, proven_tid): one orchestration pass. Runs ONLY
 * when proven_tid is non-zero and equals the current OS thread (the host proved the Unity
 * thread) AND no other caller owns a pass (non-blocking owner guard; a nested/foreign call is a
 * no-op). rules is the per-tick cap snapshot (null treats auto-recycle as off).
 * force_poll bits: 1 resets the poll cadence (getItemState().next = 0), 2 requests a full
 * catalog while recycling is off, 4 requests only five berry counts. Bits 2/4 preserve cadence;
 * enabled recycling always scans fully. Disabled with neither 2 nor 4 never enumerates.
 * remaining_lease_ms is the residual (itemLease - now) window,
 * clamped here to PGRAZOR_ITEMS_MAX_LEASE_MS and turned into an absolute CLOCK_BOOTTIME deadline
 * re-checked immediately before the single RecycleItem (0/expired -> no managed recycle).
 * Returns OK, NOT_INSTALLED or BAD_RULES.
 */
typedef int (*pgrazor_items_tick_fn)(const pgrazor_items_rules_v1 *rules, uint32_t force_poll,
                                     uint64_t remaining_lease_ms, uint64_t proven_tid);

/*
 * clear(proven_tid): Unity-thread-only teardown for stop/detach. RELEASES every durable GC root
 * (service + the two filter delegates + any pending bag root). If a recycle is still in flight it
 * DROPS attribution (frees the bag root so no reused address can confirm it) and PERMANENTLY
 * quarantines the id, so a JS handler restart can never resubmit it and never auto-duplicate.
 * Called off the proven thread it is a no-op; a reentrant clear from inside the owner's own pass
 * defers the free to the pass epilogue. Returns OK or NOT_INSTALLED.
 */
typedef int (*pgrazor_items_clear_fn)(uint64_t proven_tid);

/* sample(out): copy the latest telemetry snapshot. Safe from any thread; never frees. */
typedef int (*pgrazor_items_sample_fn)(pgrazor_items_sample_v1 *out);

/*
 * catalog(out, max, written): copy up to `max` catalog rows into `out`, writing the count into
 * *written. Safe from any thread; never frees. Returns OK, NOT_INSTALLED or NULL_ARG.
 */
typedef int (*pgrazor_items_catalog_fn)(pgrazor_item_row *out, uint32_t max, uint32_t *written);

/*
 * drain(out, max, written): copy up to `max` queued request/confirm/error events into `out` in
 * FIFO order and REMOVE them from the queue, writing the count into *written. Each call drains at
 * most `max`; a short `max` leaves the rest for the next call (order preserved). Safe from any
 * thread (a tiny mutex guards the ring-buffer copy only; never a managed call, never frees).
 * Returns OK, NOT_INSTALLED or NULL_ARG.
 */
typedef int (*pgrazor_items_drain_fn)(pgrazor_item_event *out, uint32_t max, uint32_t *written);

/*
 * service_handle(): the current durable service GC handle (an il2cpp gchandle, pointer-sized) or
 * null when no service is captured. BORROWED -- its lifetime is the module's, not the caller's.
 * Use it only for a readiness / identity check, never to drive managed calls across a possible
 * clear(). Safe from any thread; never creates or frees a handle.
 */
typedef void *(*pgrazor_items_service_handle_fn)(void);

/*
 * service_acquire(): return a NEW, INDEPENDENTLY OWNED il2cpp gchandle rooting the current service
 * object, or null when no service is captured. This is the scoped-access protocol external
 * features (VGP) use: the caller owns the returned handle for the whole operation and MUST free it
 * with il2cpp_gchandle_free, so its validity never depends on the module's own root or on any
 * time-based grace. Creates a GC handle, so it runs ONLY on the proven Unity thread (returns null
 * from any other thread or before the Unity identity latches).
 */
typedef void *(*pgrazor_items_service_acquire_fn)(void);

// Ordered, read-only table advertised by the getter (72 bytes: header + 8 pointers).
typedef struct pgrazor_items_api_v1 {
    uint32_t version;              /* == PGRAZOR_ITEMS_API_VERSION */
    uint32_t size;                 /* == sizeof(pgrazor_items_api_v1) */
    pgrazor_items_install_fn         install;
    pgrazor_items_tick_fn            tick;
    pgrazor_items_clear_fn           clear;
    pgrazor_items_sample_fn          sample;
    pgrazor_items_catalog_fn         catalog;
    pgrazor_items_service_handle_fn  service_handle;
    pgrazor_items_drain_fn           drain;
    pgrazor_items_service_acquire_fn service_acquire;
} pgrazor_items_api_v1;

/*
 * Hidden getter (NOT exported by the version script). The coordinator registers it in the core
 * getter registry under feature id 14; the loader hands the adapter the table on an exact
 * version match, else NULL. The table lives for the process lifetime.
 */
const void *pgrazor_items_api(uint32_t version);

// Internal host seam: advances only after a successful full pass with no recyclable
// candidate and no pending RPC. Unlike catalog_serial, empty unchanged polls count.
uint64_t pgrazor_items_cleanup_serial();
// Internal host force_poll bit 8 invalidates a retained large inventory snapshot.

#ifdef __cplusplus
} /* extern "C" */
#endif

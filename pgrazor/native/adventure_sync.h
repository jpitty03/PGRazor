/*
 * PGRazor native migration: Adventure Sync popup suppression.
 *
 * Feature id 2 of the native pilot. The gameplay runtime resolves this feature's
 * table through the coordinator-owned loader (root.PGRazorNativeFeature(2, version,
 * tableByteSize) in JS, backed by the hidden getter pgrazor_adventure_sync_api()
 * registered in the core getter registry). Nothing here is exported directly; the
 * linker version script (coordinator-owned) keeps the getter local, and the core's
 * registry hands it out only on an exact version+size match.
 *
 * This header is the C ABI contract between runtime/features/adventureSync.js and
 * adventure_sync.cpp. Fields are append-only; a new ABI version bumps
 * PGRAZOR_ADVSYNC_API_VERSION and never reorders or removes an existing field.
 *
 * The module mirrors the behaviour of the pre-migration JS
 * (runtime/features/adventureSync.js): on the Adventure Sync DialogController it
 * declines the initial and instruction dialogs and acknowledges the result dialog,
 * and only ever presses a button the game itself wired. It NEVER confirms an
 * initial/instruction dialog (that would enable Adventure Sync) and never fabricates
 * a callback. JS still performs all il2cpp metadata validation (class identity,
 * method address + instruction-prefix checks, field offsets) BEFORE calling
 * install(); the native layer trusts the validated pointers/offsets it is handed and
 * re-validates the config header and every pointer before it patches anything.
 */
#ifndef PGRAZOR_ADVENTURE_SYNC_H
#define PGRAZOR_ADVENTURE_SYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Feature id in the native pilot registry. */
#define PGRAZOR_ADVSYNC_FEATURE_ID 2u

/* Table + config ABI version. The getter answers an exact match only. */
#define PGRAZOR_ADVSYNC_API_VERSION 1u

/* Maximum accepted dismissal lease, in milliseconds (matches the JS 2500 ms window). */
#define PGRAZOR_ADVSYNC_MAX_LEASE_MS 2500u

/* Delay before an eligible captured dialog may be auto-dismissed (ms). */
#define PGRAZOR_ADVSYNC_DELAY_MS 100u

/* A captured dialog that is never dismissed is dropped after this age (ms). */
#define PGRAZOR_ADVSYNC_EXPIRY_MS 10000u

/* Bound on simultaneously tracked dialogs (matches the JS pending cap). */
#define PGRAZOR_ADVSYNC_QUEUE_MAX 4u

/* Status codes. 0 == success. Shared spirit with pgrazor_native_api.h. */
enum {
    PGRAZOR_ADVSYNC_OK = 0,
    PGRAZOR_ADVSYNC_ERR_NULL_ARG = 1,       /* required pointer argument was null */
    PGRAZOR_ADVSYNC_ERR_BAD_CONFIG = 2,     /* config header, size, pointer or offset invalid */
    PGRAZOR_ADVSYNC_ERR_ALREADY_BOUND = 3,  /* install() called again with a different config */
    PGRAZOR_ADVSYNC_ERR_HOOK_FAILED = 4,    /* Dobby could not install/instrument a target */
    PGRAZOR_ADVSYNC_ERR_BAD_LEASE = 5,      /* lease was 0 or exceeded the max */
    PGRAZOR_ADVSYNC_ERR_NO_CLOCK = 6,       /* CLOCK_BOOTTIME was unavailable */
    PGRAZOR_ADVSYNC_ERR_NOT_INSTALLED = 7,  /* operation needs a successful install() */
    PGRAZOR_ADVSYNC_ERR_SELFTEST = 8        /* a self-check assertion failed */
};

/*
 * Install-time configuration, built by the JS adapter in a scratch buffer after its
 * metadata validation succeeds and copied by install(). Every pointer is an
 * already-validated il2cpp export, managed MethodInfo*, compiled method entry, or
 * class pointer; every offset is the metadata-verified field offset. The native
 * layer re-checks the header and rejects any null pointer or zero offset before it
 * touches the process image. Layout is fixed (LP64, 8-byte fields); JS must write it
 * byte-for-byte and set size == sizeof(pgrazor_advsync_config_v1).
 */
typedef struct pgrazor_advsync_config_v1 {
    uint32_t version;              /* == PGRAZOR_ADVSYNC_API_VERSION */
    uint32_t size;                 /* == sizeof(pgrazor_advsync_config_v1) */

    /* il2cpp runtime function pointers (pointer-sized GC handle ABI; not uint32). */
    void *runtime_invoke;          /* void* (MethodInfo*, void* obj, void** params, void** exc) */
    void *gchandle_new;            /* void* (void* obj, int pinned) -> pointer-sized handle */
    void *gchandle_get_target;     /* void* (void* handle) */
    void *gchandle_free;           /* void  (void* handle) */
    void *object_get_class;        /* void* (void* obj) */

    /* Managed MethodInfo* for the buttons we may press, invoked via runtime_invoke. */
    void *method_on_confirm;
    void *method_on_decline;

    /* Compiled method entry points (Dobby targets). */
    void *code_update;             /* DialogController.Update()        -> hooked, chains original */
    void *code_on_confirm;         /* DialogController.OnConfirm()     -> hooked, chains original */
    void *code_on_decline;         /* DialogController.OnDecline()     -> hooked, chains original */
    void *code_init_initial;       /* InitializeAsInitial(...)         -> entry instrument (this only) */
    void *code_init_instruction;   /* InitializeAsInstruction(...)     -> entry instrument (this only) */
    void *code_init_result;        /* InitializeAsResult(...)          -> entry instrument (this only) */

    /* Identity + field offsets. */
    void *controller_class;        /* DialogController il2cpp class, for object_get_class checks */
    uint64_t off_on_confirm;       /* byte offset of _onConfirm delegate field (0xa8) */
    uint64_t off_on_decline;       /* byte offset of _onDecline delegate field (0xb0) */
} pgrazor_advsync_config_v1;

/*
 * install(config): validate the config and install the module's hooks once for the
 * process lifetime. On success the Update/OnConfirm/OnDecline methods are hooked
 * (original chained) and the three Initialize entries are instrumented (entry-only,
 * reads the `this` controller (x0 on arm64, rdi on x86_64) to tag the dialog kind;
 * does not read undetermined argument registers and does not alter the call).
 * Re-installing with an identical config returns OK; a
 * different config returns ALREADY_BOUND. A partial hook failure disables the policy
 * permanently and returns HOOK_FAILED: the JS adapter must NOT fall back to a second
 * engine on these targets.
 */
typedef int (*pgrazor_advsync_install_fn)(const pgrazor_advsync_config_v1 *config);

/*
 * update(allowed, lease_ms, unity_tid): arm or clear the dismissal window and record
 * the Unity-thread guard.
 *   allowed == 0            -> clear immediately; captured dialogs are left for the
 *                              next Unity-thread retire()/Update to drop. No UI is
 *                              driven and no GC handle is freed off the Unity thread.
 *   allowed != 0            -> require 0 < lease_ms <= PGRAZOR_ADVSYNC_MAX_LEASE_MS and
 *                              a usable CLOCK_BOOTTIME; set a monotonic deadline.
 *   unity_tid               -> the thread id proven to be the Unity thread by the
 *                              host's existing validated tick; stored (when non-zero)
 *                              as the guard the hooks compare gettid() against. Hooks
 *                              and auto-dismissal never run off this thread.
 * This call only mutates atomics; it is safe to call from any thread.
 */
typedef int (*pgrazor_advsync_update_fn)(int allowed, uint64_t lease_ms, uint64_t unity_tid);

/*
 * retire(unity_tid): Unity-thread-only maintenance, called from the host heartbeat
 * tick after it has validated the thread. Refreshes the guard (when unity_tid is
 * non-zero and matches the caller) and drops expired / over-lease captured dialogs,
 * freeing their GC handles. It NEVER drives UI. Called off the guard thread it is a
 * no-op. Returns OK or NOT_INSTALLED.
 */
typedef int (*pgrazor_advsync_retire_fn)(uint64_t unity_tid);

/*
 * selftest(): explicit-only self-check of the pure pending policy (lease bounds, the
 * decline-not-confirm dialog mapping, delay/expiry ordering). No Dobby, no device, no
 * process mutation. NEVER run automatically in production.
 */
typedef int (*pgrazor_advsync_selftest_fn)(void);

/* Ordered, read-only table advertised by the getter. Header first, then pointers. */
typedef struct pgrazor_advsync_api_v1 {
    uint32_t version;                    /* == PGRAZOR_ADVSYNC_API_VERSION */
    uint32_t size;                       /* == sizeof(pgrazor_advsync_api_v1) */
    pgrazor_advsync_install_fn  install;
    pgrazor_advsync_update_fn   update;
    pgrazor_advsync_retire_fn   retire;
    pgrazor_advsync_selftest_fn selftest;
} pgrazor_advsync_api_v1;

/*
 * Hidden getter (NOT exported by the version script). The coordinator registers it in
 * the core getter registry; the loader calls it with the negotiated version and hands
 * the JS adapter the table pointer on an exact version match, else NULL. The returned
 * table lives for the process lifetime and must not be freed or modified.
 */
const void *pgrazor_adventure_sync_api(uint32_t version);

/*
 * Host-independent policy self-check (no Dobby, no device). Returns 0 on success or
 * the id of the first failing case. Exposed for an optional JNI host test the
 * coordinator may wire; safe to call anywhere.
 */
int pgrazor_adventure_sync_selfcheck(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PGRAZOR_ADVENTURE_SYNC_H */

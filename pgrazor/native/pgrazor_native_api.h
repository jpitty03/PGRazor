/*
 * Stable C ABI for the PGRazor native migration pilot.
 *
 * The gameplay runtime resolves the single exported symbol `pgrazor_native_api`,
 * calls it with the ABI version it was built against, and (on an exact match)
 * receives a pointer to a read-only, ordered function table prefixed by a
 * {version, size} header. Fields are never reordered or removed; a new ABI
 * version only appends fields and bumps PGRAZOR_NATIVE_API_VERSION.
 *
 * This header is the contract. Nothing else in the library is exported.
 */
#ifndef PGRAZOR_NATIVE_API_H
#define PGRAZOR_NATIVE_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Current table version. pgrazor_native_api() only answers an exact match in the pilot. */
#define PGRAZOR_NATIVE_API_VERSION 1u

/* Fixed radius floor (metres) applied while the spawn policy is active. */
#define PGRAZOR_SPAWN_FLOOR 80.0

/* Maximum accepted lease for a single policy window, in milliseconds. */
#define PGRAZOR_MAX_LEASE_MS 2500u

/* Status codes returned by every table function. 0 == success. */
enum {
    PGRAZOR_OK = 0,
    PGRAZOR_ERR_NULL_ARG = 1,        /* required pointer argument was null */
    PGRAZOR_ERR_ALREADY_BOUND = 2,   /* install() requested at a different address (rebind refused) */
    PGRAZOR_ERR_HOOK_FAILED = 3,     /* Dobby could not install the hook / publish the original */
    PGRAZOR_ERR_BAD_LEASE = 4,       /* lease was 0 or exceeded PGRAZOR_MAX_LEASE_MS */
    PGRAZOR_ERR_NO_CLOCK = 5,        /* CLOCK_BOOTTIME was unavailable */
    PGRAZOR_ERR_NOT_INSTALLED = 6,   /* reserved: operation needs an installed hook */
    PGRAZOR_ERR_SELFTEST = 7,        /* a self-check assertion failed */
    PGRAZOR_ERR_UNSUPPORTED_VERSION = 8 /* reserved for negotiation callers */
};

/* Telemetry snapshot of the spawn getter. Pull-only: the native layer never calls back into JS. */
typedef struct pgrazor_spawn_snapshot {
    double   original;  /* last radius the original getter returned */
    double   effective; /* last radius this layer returned to the caller */
    uint64_t count;     /* number of times the policy raised the radius to the floor */
} pgrazor_spawn_snapshot;

/*
 * install(address): install the spawn-getter hook at `address` once for the process
 * lifetime. The original pointer is published before the bound address so the
 * replacement never fabricates a radius. Installing the same address again returns
 * PGRAZOR_OK (idempotent); a different address returns PGRAZOR_ERR_ALREADY_BOUND.
 * The hook is never removed; see update(..., enabled=0) to disable its effect.
 */
typedef int (*pgrazor_install_fn)(void *address);

/*
 * update(enabled, allowed, lease_ms): arm or clear the policy window.
 *   enabled == 0            -> clear immediately, hook passes the original radius through.
 *   enabled != 0            -> require 0 < lease_ms <= PGRAZOR_MAX_LEASE_MS and a usable
 *                              CLOCK_BOOTTIME; sets a monotonic deadline of now+lease.
 *   allowed                 -> existing foreground/readiness permission lease.
 * The floor only applies while enabled && allowed && now < deadline.
 */
typedef int (*pgrazor_update_fn)(int enabled, int allowed, uint64_t lease_ms);

/* sample(out): copy the latest telemetry snapshot. Does not clear counters. */
typedef int (*pgrazor_sample_fn)(pgrazor_spawn_snapshot *out);

/*
 * selftest(): run the self-contained Dobby hook self-check against an own function
 * (original chaining + clean detach, leaving no dangling hook). Explicit call only;
 * NEVER run automatically in production. Returns PGRAZOR_OK or PGRAZOR_ERR_SELFTEST.
 */
typedef int (*pgrazor_selftest_fn)(void);

/* Ordered, read-only function table. Header first; function pointers in fixed order. */
typedef struct pgrazor_native_api_v1 {
    uint32_t version;              /* == PGRAZOR_NATIVE_API_VERSION */
    uint32_t size;                 /* == sizeof(pgrazor_native_api_v1) */
    pgrazor_install_fn  install;
    pgrazor_update_fn   update;
    pgrazor_sample_fn   sample;
    pgrazor_selftest_fn selftest;
} pgrazor_native_api_v1;

/* v1 remains available unchanged. v2 appends independent feature discovery. */
typedef const void *(*pgrazor_query_feature_fn)(uint32_t id, uint32_t version);
typedef struct pgrazor_native_api_v2 {
    uint32_t version;
    uint32_t size;
    pgrazor_install_fn install;
    pgrazor_update_fn update;
    pgrazor_sample_fn sample;
    pgrazor_selftest_fn selftest;
    pgrazor_query_feature_fn query_feature;
} pgrazor_native_api_v2;

/*
 * Returns a pointer to a static const pgrazor_native_api_v1 when `version` matches
 * PGRAZOR_NATIVE_API_VERSION, otherwise NULL. The returned table lives for the
 * process lifetime and must not be freed or modified by the caller.
 */
const void *pgrazor_native_api(uint32_t version);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PGRAZOR_NATIVE_API_H */

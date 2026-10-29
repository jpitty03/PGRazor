#pragma once
#include <stdint.h>

// Feature 25 (API 1). Native VGP scheduling / retry POLICY and bounded target-quarantine state
// machine: a focused C++ port of the scheduling state that runtime/features/vgp.js keeps inline in
// vgpState (the `seen` recent-target map, the `next` scan-cadence deadline, the `turn` catch/spin
// alternation counter, the `blocked`-on-uncertain latch and the vgpCatchDelay() / vgpRetry() delay
// table). It owns NOTHING else this increment: candidate enumeration, manual-encounter exclusion,
// the map-POI snapshot, the on-screen marker, and auto-transfer all stay in JS. The actual managed
// encounter/capture/spin invocations are issued by feature 26 (vgp_actions, a sibling worker). This
// module installs NO hook and NEVER calls back into JS -- the host polls it on the proven Unity
// owner thread and reads pull-only telemetry.
//
// WHY A STATE MACHINE AND NOT A WRAPPER: the authoritative scheduling state (which targets are
// quarantined and until when, when the next scan may run, whose turn it is, and whether an uncertain
// response has latched the controller off) moves out of the JS heap into a bounded, fixed-capacity
// native structure with its own monotonic deadlines and its own delay table. The JS adapter becomes a
// thin forwarder; it keeps no parallel Map. The legacy inline JS path remains intact and is used
// verbatim whenever the native pilot is absent (create() returns null).
//
// CLOCK: every policy deadline (seen-key expiry, scan cadence, retry/terminal delay) is anchored to
// the native monotonic CLOCK_BOOTTIME read inside each call. The host passes only RELATIVE durations
// in milliseconds; it never supplies an absolute host timestamp, so there is a single clock and no
// cross-clock drift. The controller does NOT gate on the foreground lease -- that remains the JS
// allowed() check and feature 26's own owner/lease/mask gate; a selected-failure there stops only new
// VGP actions and never falls back to a duplicate legacy throw.
//
// QUARANTINE LIFETIME: the `seen` set survives a VGP mask toggle (off/on) exactly as the JS does --
// clearing the blocked latch does NOT drop the quarantine. Only reset() (owner teardown / detach)
// drops it. Keys are opaque, bounded UTF-8 byte strings (encounter ids are "p"+decimal uint64, stop
// ids are the fort id). The host adapter validates length (<= PGR_VP_MAX_KEY_LEN) and never passes a
// Number; an over-long key is treated as already-seen by the adapter and never enqueued natively.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "VgpPolicy native ABI requires LP64");

enum PgrVgpPolicyInfo {
  PGR_VP_FEATURE_ID     = 25,
  PGR_VP_API_VERSION    = 1,
  PGR_VP_CONFIG_VERSION = 1,
  PGR_VP_SAMPLE_VERSION = 1,
  PGR_VP_MAX_KEYS       = 2048,  // hard capacity ceiling for the quarantine set
  PGR_VP_MAX_KEY_LEN    = 1024    // bounded UTF-8 key length (fort ids / "p"+uint64 fit well under)
};

// Boolean-style results use 1/0; actions use 0 success. Everything else is a negative error.
enum PgrVgpPolicyStatus {
  PGR_VP_YES               = 1,   // due()/sweep()/seen(): affirmative
  PGR_VP_OK                = 0,   // generic success / affirmative negative
  PGR_VP_FULL              = 2,   // touch(): the quarantine is at capacity and the key is new
  PGR_VP_ERR_NULL_ARG      = -1,
  PGR_VP_ERR_BAD_VERSION   = -2,
  PGR_VP_ERR_BAD_SIZE      = -3,
  PGR_VP_ERR_BAD_CONFIG    = -4,  // out-of-range config scalar (e.g. max_keys)
  PGR_VP_ERR_NOT_INSTALLED = -6,
  PGR_VP_ERR_RECONFIG      = -9,
  PGR_VP_ERR_CLOCK_FAILURE = -10,
  PGR_VP_ERR_OFF_OWNER     = -12, // an owner-gated call arrived on a foreign thread
  PGR_VP_ERR_BAD_KEY       = -13  // key pointer null or length 0 / > PGR_VP_MAX_KEY_LEN
};

// Fixed LP64 config (128 bytes). All durations are milliseconds; the native layer re-checks the
// header and the max_keys ceiling. Mirrors the literal policy constants in vgp.js.
struct PgrVgpPolicyConfig {
  uint32_t version;            // == PGR_VP_CONFIG_VERSION
  uint32_t size;               // == sizeof(PgrVgpPolicyConfig)
  uint64_t scan_interval_ms;   // cadence between scans (vgp.js r.next = now + 2500)
  uint64_t catch_seen_ms;      // suppression for a newly selected catch target (30000)
  uint64_t spin_seen_ms;       // suppression for a newly selected spin target (300000)
  uint64_t reject_delay_ms;    // request-rejected retry suppression (vgpRetry 30000)
  uint64_t terminal_delay_ms;  // CatchSuccess / CatchFlee suppression (3600000)
  uint64_t retry_delay_ms;     // CatchEscape / CatchMissed / CatchItemReplacement (5000)
  uint64_t default_delay_ms;   // any other catch status (30000)
  uint32_t max_keys;           // <= PGR_VP_MAX_KEYS
  uint32_t reserved;           // 0
  int32_t  status_terminal[4]; // catch status literals mapped to terminal_delay_ms; -1 = unused
  int32_t  status_retry[4];    // catch status literals mapped to retry_delay_ms; -1 = unused
  uint32_t reserved_tail[6];   // 0
};

static_assert(sizeof(PgrVgpPolicyConfig) == 128, "PgrVgpPolicyConfig must remain 128 bytes on LP64");

// Fixed LP64 telemetry gauge (64 bytes). Pull-only; the native layer never calls JS.
struct PgrVgpPolicySample {
  uint32_t version;       // == PGR_VP_SAMPLE_VERSION
  uint32_t size;          // == sizeof(PgrVgpPolicySample)
  uint64_t scans;         // due() that returned YES (a scan was permitted)
  uint64_t marks;         // touch() inserts/updates
  uint64_t evictions;     // expired keys removed by sweep()
  uint64_t blocks;        // block() calls (uncertain-response latches)
  uint64_t drops;         // reset() calls
  uint32_t seen_size;     // current quarantine size
  uint32_t turn;          // current catch/spin alternation counter
  uint32_t blocked;       // 1 while the uncertain latch is set
  uint32_t full_rejects;  // touch() that hit PGR_VP_FULL
};

static_assert(sizeof(PgrVgpPolicySample) == 64, "PgrVgpPolicySample must remain 64 bytes on LP64");

// Fixed LP64 table: 8-byte header + 13 function pointers = 112 bytes. All owner-gated calls carry the
// caller's proven Unity owner tid; the owner is latched once from a genuine on-thread call and a
// foreign thread is rejected (PGR_VP_ERR_OFF_OWNER). catch_delay() and sample() are pure / pull-only
// and run on any thread.
struct PgrVgpPolicyApi {
  uint32_t version;  // == PGR_VP_API_VERSION
  uint32_t size;     // == sizeof(PgrVgpPolicyApi)
  int (*install)(const PgrVgpPolicyConfig *config);
  // Latch the owner (idempotent) and record the host epoch. Does NOT touch the quarantine or the
  // blocked latch. Owner thread only.
  int (*update)(uint64_t epoch, uint64_t proven_tid);
  // Full teardown: drop the quarantine, the scan cadence, the turn counter and the blocked latch.
  // Owner thread only. (The quarantine is NOT dropped by clear_block / a mask toggle.)
  int (*reset)(uint64_t proven_tid);
  // May a scan run now? YES iff not blocked and the scan cadence has elapsed. No mutation. Owner only.
  int (*due)(uint64_t proven_tid);
  // Current catch/spin order, then advance the alternation: *out_order 0 = catch-first, 1 = spin-first.
  // Owner only.
  int (*order)(uint64_t proven_tid, uint32_t *out_order);
  // Drop every expired key, publish the remaining size in *out_size, and report whether the set is now
  // at capacity (YES -> the host waits for recent targets to expire). Owner only.
  int (*sweep)(uint64_t proven_tid, uint32_t *out_size);
  // Is this key currently quarantined (present and unexpired)? YES / OK. Owner only.
  int (*seen)(uint64_t proven_tid, const char *key, uint32_t key_len);
  // Quarantine the key until now + delay_ms (insert or refresh). PGR_VP_FULL if the set is at capacity
  // and the key is new. Owner only.
  int (*touch)(uint64_t proven_tid, const char *key, uint32_t key_len, uint64_t delay_ms);
  // Set the scan cadence deadline to now + delay_ms. Owner only.
  int (*arm)(uint64_t proven_tid, uint64_t delay_ms);
  // Latch the controller off after an uncertain response. Cleared only by clear_block() / reset().
  // Owner only.
  int (*block)(uint64_t proven_tid);
  // Clear the uncertain latch (the host calls this when VGP is toggled off). Owner only.
  int (*clear_block)(uint64_t proven_tid);
  // Map a catch status to its suppression delay (terminal / retry / default). Pure; any thread.
  int (*catch_delay)(int32_t status, uint64_t *out_ms);
  int (*sample)(PgrVgpPolicySample *out_sample);
};

static_assert(sizeof(PgrVgpPolicyApi) == 112, "PgrVgpPolicyApi must remain a 112-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature
// under feature id 25 and answers only on an exact version match.
const void *pgrazor_vgp_policy_api(uint32_t version);

#ifdef __cplusplus
}
#endif

#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// Pure, dependency-free startup-diagnostic helpers for feature70 (runtime_host)
// and its isolated fixtures. No I/O, no transport, no locks, no managed/il2cpp
// call: the caller owns threading and delivery. These map opaque status codes to
// stable symbolic tokens, build a bounded JSON object, and throttle repeated
// emission so a failed native startup is delivered once -- and, for a LATCHED
// negative fault, re-announced on a slow heartbeat so a late subscriber still
// learns it -- WITHOUT per-poll spam. They never format an account id, token or
// coordinate; only stable status tokens and (caller-appended) code addresses.

// Heartbeat ceiling for a latched negative failure: re-announce at most this
// often. A terminal fault (scheduler hook poisoned) keeps re-emitting at this
// cadence because the overlay otherwise waits on readiness forever.
#define PGR_RTD_HEARTBEAT_MS 15000ull

// Bootstrap (feature54 PgrBootStatus) -> stable token. Keep in sync with
// runtime_bootstrap.h; an unknown code is reported, never silently dropped.
static inline const char *pgr_rtd_boot_reason(int status) {
  switch (status) {
    case 0:   return "ok";
    case 1:   return "not-ready";       // library/domain not up yet (retriable)
    case -1:  return "null-arg";
    case -2:  return "bad-version";
    case -3:  return "bad-size";
    case -4:  return "bad-config";
    case -5:  return "busy";
    case -6:  return "missing-export";  // unsupported il2cpp build
    case -7:  return "module-range";
    case -8:  return "no-metadata";
    case -9:  return "bind-failed";
    case -10: return "resolve-failed";  // ExecuteTasks class/method validation
    case -11: return "thread-attach";
    case -12: return "poisoned";
    case -13: return "export-code";
    default:  return "unknown";
  }
}

// Unity scheduler (feature48 PgrUnitySchedulerStatus) install result -> token.
// "hook-failed" is the status==-3 the user log shows: the Dobby patch on the
// il2cpp runtime_invoke/convert prologue could not be installed, so the single
// post-ExecuteTasks scheduler never starts and every native host stays gray.
static inline const char *pgr_rtd_sched_reason(int status) {
  switch (status) {
    case 0:  return "ok";
    case -1: return "bad-config";
    case -2: return "rebind";
    case -3: return "hook-failed";
    case -4: return "rollback-failed";
    case -5: return "poisoned";
    case -6: return "stale";
    default: return "unknown";
  }
}

// Scheduler hook-install failure stage (PgrUnitySchedulerFailStage) -> token.
static inline const char *pgr_rtd_sched_stage(uint32_t stage) {
  switch (stage) {
    case 0: return "none";
    case 1: return "config";
    case 2: return "verify-metadata";
    case 3: return "resolve-body";   // export veneer did not resolve to code
    case 4: return "invoke-hook";    // Dobby patch on runtime_invoke failed
    case 5: return "convert-hook";   // Dobby patch on the convert entry failed
    default: return "unknown";
  }
}

// Build the base startup-diagnostic JSON object WITHOUT its closing brace into
// buf. Returns the byte length written (excluding NUL), or 0 on bad args or
// truncation. The caller may append ",\"field\":..." pairs and MUST close with
// '}'. Bounded to cap; never writes past it.
static inline int pgr_rtd_base_json(char *buf, int cap, const char *stage,
                                    const char *reason, int32_t status, int32_t detail) {
  if (!buf || cap <= 0) return 0;
  int n = snprintf(buf, (size_t)cap,
                   "{\"kind\":\"startup\",\"stage\":\"%s\",\"reason\":\"%s\",\"status\":%d,\"detail\":%d",
                   stage ? stage : "", reason ? reason : "", status, detail);
  if (n < 0 || n >= cap) return 0;
  return n;
}

// De-dup / heartbeat throttle. Zero-initialize (have==0) before first use.
struct PgrStartupThrottle {
  char stage[24];
  char reason[32];
  int32_t status;
  int32_t detail;
  uint64_t last_ms;
  int have;
};

// Decide whether to emit the (stage,reason,status,detail) diagnostic now, and
// record it when the answer is yes. Emits on ANY change of the tuple; for an
// unchanged LATCHED negative status re-emits once per PGR_RTD_HEARTBEAT_MS; an
// unchanged non-negative status is suppressed. now_ms==0 (no usable clock)
// disables only the time-based heartbeat, never change detection. NOT
// thread-safe: the caller serializes access to a single throttle instance.
static inline int pgr_rtd_should_emit(struct PgrStartupThrottle *t, const char *stage,
                                      const char *reason, int32_t status, int32_t detail,
                                      uint64_t now_ms) {
  if (!t || !stage || !reason) return 0;
  const int same = t->have && status == t->status && detail == t->detail &&
                   strcmp(t->stage, stage) == 0 && strcmp(t->reason, reason) == 0;
  const int heartbeat = status < 0 && now_ms &&
                        (t->last_ms == 0 || now_ms - t->last_ms >= PGR_RTD_HEARTBEAT_MS);
  const int emit = !same || heartbeat;
  if (emit) {
    snprintf(t->stage, sizeof(t->stage), "%s", stage);
    snprintf(t->reason, sizeof(t->reason), "%s", reason);
    t->status = status;
    t->detail = detail;
    t->last_ms = now_ms;
    t->have = 1;
  }
  return emit;
}

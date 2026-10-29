#pragma once
#include <stdint.h>

// Feature 27 v1: Routes command authorization only. No hooks, managed objects or callbacks.
// One process-lifetime sequence high-water survives controller reconstruction. open() revokes
// the previous generation and clears its lease/pending command; stale handles cannot mutate it.
// Host time is monotonic milliseconds (boottime). Nonfinite, negative or backwards time revokes
// the lease and cancels pending work. A cancelled command still drains as an expired ack.
enum PgrRoutesGateStatus {
  PGR_RG_OK=0, PGR_RG_ACCEPTED=1, PGR_RG_NOT_ALLOWED=2, PGR_RG_NO_COMMAND=3,
  PGR_RG_INVALID_COMMAND=4, PGR_RG_STALE_SEQ=5, PGR_RG_BAD_TIME=-1,
  PGR_RG_BAD_HANDLE=-2, PGR_RG_BAD_ARG=-3
};
enum PgrRoutesAction { PGR_RG_DISCOVER=1, PGR_RG_START=2, PGR_RG_COMPLETE=3, PGR_RG_PAUSE=4 };
struct PgrRoutesGateResult {
  uint64_t seq;
  uint32_t action;
  uint32_t expired;
};
static_assert(sizeof(PgrRoutesGateResult)==16,"Routes result ABI");
struct PgrRoutesGateApi {
  uint32_t version, size;
  uint64_t (*open)(double lease_ms);
  // parsed: 1 valid command, 0 no-command (cancels), -1 malformed (does not cancel).
  int (*set)(uint64_t handle,int allowed,int parsed,uint64_t seq,uint32_t action,double now);
  // take: 1 output present, 0 empty, negative error. Invalid time still drains expired output.
  int (*take)(uint64_t handle,double now,PgrRoutesGateResult *out);
  int (*active)(uint64_t handle,double now);
  int (*clear)(uint64_t handle);
  uint64_t (*last_seq)(uint64_t handle);
};
static_assert(sizeof(PgrRoutesGateApi)==56,"Routes table ABI");
extern "C" const void *pgrazor_routes_gate_api(uint32_t version);

#pragma once
#include <stdint.h>

// Feature48 v2 foundation. Subscriber work follows a successful original static,
// zero-argument, void ExecuteTasks invocation, matching all three legacy owners.
// Unexpected nonnull results and managed exceptions suppress dispatch: no borrowed
// managed return value or exception crosses subscriber calls/GC safepoints.
// Missing exception-out also suppresses dispatch (success cannot be established).
// Installation is process lifetime and dormant until activate(). Caller MUST
// detach all three existing Gum owners before installing; ownership attestation is
// a required explicit argument, not runtime detection of another hook engine.
// BOTH invoke entrypoints must be quiescent throughout installation.
// Caller keeps config memory stable for install. verify_metadata must not reenter
// this API. Hook install failures permanently poison this owner. No uninstall.
// Only matched ExecuteTasks frames contribute to nesting depth: an unrelated
// outer runtime_invoke must not suppress the real scheduler pulse inside it.
enum PgrUnitySchedulerStatus {
 PGR_US_OK=0, PGR_US_BAD_CONFIG=-1, PGR_US_REBIND=-2,
 PGR_US_HOOK_FAILED=-3, PGR_US_ROLLBACK_FAILED=-4,
 PGR_US_POISONED=-5, PGR_US_STALE=-6
};
constexpr uint32_t PGR_US_MAX_SUBSCRIBERS=16;
using PgrUnityInvoke=void *(*)(const void *,void *,void **,void **);
using PgrUnityConvert=void *(*)(const void *,void *,void **,int32_t,void **);
struct PgrUnityPulse {
 uint64_t generation, sequence;
 int32_t tid;
 uint32_t reserved;
};
// Native compiled callbacks only; context is unmanaged, process-lifetime storage.
// begin/end are ordered work phases AFTER a successful original. Reentry from a
// subscriber does not dispatch a nested pulse. An original exception propagates
// unchanged with depth restored. A frame begun under an old generation cannot
// dispatch under a replacement generation activated inside the original.
// These are independent work phases, NOT a balanced scope: revocation may skip
// end. Every callback must finish its own temporary-resource cleanup before return.
// They may call managed functions. They MUST NOT throw C++ exceptions or unload
// code. On revoke, callbacks must test valid(generation) after managed operations
// before mutation. Revoke prevents subsequent dispatch, not an in-flight action.
using PgrUnityWork=void (*)(void *,const PgrUnityPulse *);
struct PgrUnitySubscriber { PgrUnityWork begin,end; void *context; };
struct PgrUnitySchedulerConfig {
 uint32_t version,size;
 void *runtime_invoke,*runtime_invoke_convert_args;
 const void *execute_tasks;
 void *(*thread_current)();
 // Independent metadata resolver must verify static System.Void ExecuteTasks(),
 // exact MethodInfo, export ABIs and supported executable owner/prologues.
 int (*verify_metadata)(const PgrUnitySchedulerConfig *);
 int32_t expected_owner_tid;
 uint32_t exclusive_owner; // exactly1: coordinator detached ALL Gum owners
 uint32_t subscriber_count,reserved;
 PgrUnitySubscriber subscribers[PGR_US_MAX_SUBSCRIBERS];
};
struct PgrUnitySchedulerSnapshot {
 uint64_t generation,sequence;
 int32_t owner_tid;
 uint32_t installed,poisoned,reserved;
};
struct PgrUnitySchedulerApi {
 uint32_t version,size;
 int (*install)(const PgrUnitySchedulerConfig *);
 uint64_t (*activate)(); // fresh generation replaces any prior one; no replay;0 before install/poison
 int (*revoke)(uint64_t);
 int (*valid)(uint64_t);
 int (*snapshot)(PgrUnitySchedulerSnapshot *);
};
// API3 preserves API2 scheduling/configuration and adds a read-only gate for
// native getter observers running INSIDE the original ExecuteTasks call. This
// does not introduce a pre-original work callback or another invoke hook owner.
struct PgrUnitySchedulerApi3 {
 uint32_t version,size;
 int (*install)(const PgrUnitySchedulerConfig *);
 uint64_t (*activate)();
 int (*revoke)(uint64_t);
 int (*valid)(uint64_t);
 int (*snapshot)(PgrUnitySchedulerSnapshot *);
 uint64_t (*executing_generation)(uint64_t tid); // zero outside original/owner/attached/live generation
};
// Which install() stage last failed. A non-OK install leaves this latched so a
// later diagnostics() reader (e.g. feature70) can describe the terminal cause
// without re-running install. Carries NO account id / token / coordinate.
enum PgrUnitySchedulerFailStage : uint32_t {
 PGR_US_STAGE_NONE=0, PGR_US_STAGE_CONFIG=1, PGR_US_STAGE_VERIFY=2,
 PGR_US_STAGE_RESOLVE=3, PGR_US_STAGE_INVOKE_HOOK=4, PGR_US_STAGE_CONVERT_HOOK=5
};
// Read-only install/hook diagnostic. ABI-additive: API2/API3 are unchanged. The
// body addresses are the validated executable veneer targets actually handed to
// the hook installer (aarch64 follows export veneers), useful for comparing
// failed hook sites across devices. Addresses alone do not prove another hook owner.
struct PgrUnitySchedulerDiag {
 uint32_t version,size;      // version==4, size==sizeof(PgrUnitySchedulerDiag)
 int32_t last_result;        // most recent install() return that reached hook staging
 uint32_t failed_stage;      // PgrUnitySchedulerFailStage
 int32_t invoke_hook_rc;     // hook-engine rc for the runtime_invoke hook (0 if not attempted)
 int32_t convert_hook_rc;    // hook-engine rc for the convert hook (0 if not attempted)
 uint32_t installed,poisoned;
 uint64_t invoke_body,convert_body; // resolved veneer body targets passed to the installer
};
static_assert(sizeof(PgrUnitySchedulerDiag)==48,"Scheduler diag ABI4");
// API4 preserves API3 scheduling/inspection and adds the read-only diagnostics()
// getter. It installs no hook, makes no managed call, and never mutates state.
struct PgrUnitySchedulerApi4 {
 uint32_t version,size;
 int (*install)(const PgrUnitySchedulerConfig *);
 uint64_t (*activate)();
 int (*revoke)(uint64_t);
 int (*valid)(uint64_t);
 int (*snapshot)(PgrUnitySchedulerSnapshot *);
 uint64_t (*executing_generation)(uint64_t tid);
 int (*diagnostics)(PgrUnitySchedulerDiag *out);
};
static_assert(sizeof(PgrUnitySchedulerApi4)==64,"Scheduler inspection ABI4");
static_assert(sizeof(PgrUnitySchedulerApi3)==56,"Scheduler inspection ABI3");
static_assert(sizeof(void *)==8,"Scheduler requires LP64");
static_assert(sizeof(PgrUnityPulse)==24,"Pulse ABI");
static_assert(sizeof(PgrUnitySubscriber)==24,"Subscriber ABI");
static_assert(sizeof(PgrUnitySchedulerConfig)==448,"Config ABI");
static_assert(sizeof(PgrUnitySchedulerSnapshot)==32,"Snapshot ABI");
static_assert(sizeof(PgrUnitySchedulerApi)==48,"API ABI");
extern "C" const void *pgrazor_unity_scheduler_api(uint32_t version);

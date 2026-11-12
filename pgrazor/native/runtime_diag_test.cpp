// Pure-logic fixture for the feature70 startup-diagnostic helpers. No device,
// no core, no transport: it validates the symbolic status mapping, the bounded
// JSON prefix builder, and the de-dup / heartbeat throttle that keeps a latched
// failure visible without per-poll spam.
#include "runtime_diag.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main(){
  // --- Symbolic mappings (stable tokens; unknown is reported, never dropped) --
  assert(!strcmp(pgr_rtd_sched_reason(-3),"hook-failed"));   // the status=-3 case
  assert(!strcmp(pgr_rtd_sched_reason(0),"ok"));
  assert(!strcmp(pgr_rtd_sched_reason(-5),"poisoned"));
  assert(!strcmp(pgr_rtd_sched_reason(42),"unknown"));
  assert(!strcmp(pgr_rtd_boot_reason(1),"not-ready"));
  assert(!strcmp(pgr_rtd_boot_reason(-6),"missing-export"));
  assert(!strcmp(pgr_rtd_boot_reason(-11),"thread-attach"));
  assert(!strcmp(pgr_rtd_boot_reason(99),"unknown"));
  assert(!strcmp(pgr_rtd_sched_stage(4),"invoke-hook"));
  assert(!strcmp(pgr_rtd_sched_stage(5),"convert-hook"));
  assert(!strcmp(pgr_rtd_sched_stage(3),"resolve-body"));
  assert(!strcmp(pgr_rtd_sched_stage(7),"unknown"));

  // --- Bounded base JSON builder ---------------------------------------------
  char buf[320];
  int n=pgr_rtd_base_json(buf,sizeof(buf),"scheduler-install","hook-failed",-3,0);
  assert(n>0 && (size_t)n<sizeof(buf));
  assert(buf[n]=='\0' && (int)strlen(buf)==n); // snprintf NUL-terminates exactly at the prefix length
  assert(strstr(buf,"\"kind\":\"startup\""));
  assert(strstr(buf,"\"stage\":\"scheduler-install\""));
  assert(strstr(buf,"\"reason\":\"hook-failed\""));
  assert(strstr(buf,"\"status\":-3"));
  assert(strstr(buf,"\"detail\":0"));
  // The caller closes the object; the builder itself leaves it open.
  assert(buf[n-1]!='}');
  // Truncation fails closed (returns 0, no partial object leaked as "built").
  char tiny[8];
  assert(pgr_rtd_base_json(tiny,sizeof(tiny),"scheduler-install","hook-failed",-3,0)==0);

  // --- Throttle: emit on change, dedup, latched-failure heartbeat ------------
  PgrStartupThrottle t; memset(&t,0,sizeof(t));
  const uint64_t base=1000000;
  // First observation of any tuple always emits.
  assert(pgr_rtd_should_emit(&t,"bootstrap","not-ready",1,0,base)==1);
  // Identical non-negative tuple is suppressed (no spam while merely waiting).
  assert(pgr_rtd_should_emit(&t,"bootstrap","not-ready",1,0,base+50)==0);
  assert(pgr_rtd_should_emit(&t,"bootstrap","not-ready",1,0,base+5000)==0);
  // A changed tuple emits immediately.
  assert(pgr_rtd_should_emit(&t,"scheduler-install","hook-failed",-3,0,base+6000)==1);
  // Same latched negative tuple is suppressed within the heartbeat window ...
  assert(pgr_rtd_should_emit(&t,"scheduler-install","hook-failed",-3,0,base+6000+1)==0);
  assert(pgr_rtd_should_emit(&t,"scheduler-install","hook-failed",-3,0,base+6000+PGR_RTD_HEARTBEAT_MS-1)==0);
  // ... and re-announced once the heartbeat elapses, so a late subscriber learns it.
  const uint64_t beat=base+6000+PGR_RTD_HEARTBEAT_MS;
  assert(pgr_rtd_should_emit(&t,"scheduler-install","hook-failed",-3,0,beat)==1);
  assert(pgr_rtd_should_emit(&t,"scheduler-install","hook-failed",-3,0,beat+1)==0);
  // A non-negative tuple never heartbeats, even after a long wait.
  assert(pgr_rtd_should_emit(&t,"ready","ok",0,0,beat+100)==1);
  assert(pgr_rtd_should_emit(&t,"ready","ok",0,0,beat+100+10*PGR_RTD_HEARTBEAT_MS)==0);

  // --- now_ms==0 (no usable clock): change detection still works, heartbeat off
  PgrStartupThrottle z; memset(&z,0,sizeof(z));
  assert(pgr_rtd_should_emit(&z,"scheduler-install","hook-failed",-3,0,0)==1);
  assert(pgr_rtd_should_emit(&z,"scheduler-install","hook-failed",-3,0,0)==0); // no clock -> emit once
  assert(pgr_rtd_should_emit(&z,"attach","not-attached",-2,0,0)==1);           // change still emits

  std::puts("PASS runtime_diag: status tokens, bounded JSON, change/dedup, latched heartbeat, clockless fallback");
  return 0;
}

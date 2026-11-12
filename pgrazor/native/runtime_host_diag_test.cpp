// Isolated feature70 startup-diagnostic fixture.
//
// Proves the central fix for the user report ("overlay remains, bonuses/VGP+
// never ready, waiting for readiness" with scheduler status=-3): when the Unity
// scheduler hook install fails, runtime_host must still deliver an actionable
// diagnostic through the transport event queue EVEN THOUGH the scheduler never
// pulses -- the path that previously latched `fault` and returned silently.
//
// Every dependency is a local stub, so this runs with no device, no real core,
// no Dobby. Gameplay host tables are never called (configure() touches only
// bootstrap/transport/scheduler); only the scheduler is forced to fail.
#include "runtime_host.h"
#include "shared_host.h"
#include "quickball_host.h"
#include "routes_host.h"
#include "inventory_host.h"
#include "spin_host.h"
#include "catch_wait.h"
#include "spawn_host.h"
#include "camera_host.h"
#include "combat_host.h"
#include "account_host.h"
#include "interaction_host.h"
#include "encounter_host.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "unity_scheduler.h"
#include "runtime_metadata.h"
#include "events/event_collector.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

namespace diagfix {
// Recorded transport events (channel,json) from the single emit() producer path.
std::vector<std::pair<std::string,std::string>> events;
int g_install_rc = PGR_US_HOOK_FAILED;          // -3: Dobby patch on runtime_invoke failed
int g_transport_fault = 0, g_transport_fresh = 1; // make the pre-scheduler gates pass
int token, dummy_exec, dummy_invoke, dummy_convert;
void *thread_current_stub(){ return &token; }

// --- Transport (feature52) ---------------------------------------------------
int tr_install(const PgrRuntimeTransportConfig *){ return 0; }
int tr_exchange(const uint8_t*,uint32_t,uint8_t*,uint32_t,uint32_t*){ return 0; }
int tr_emit(uint32_t op,const int64_t*,uint32_t,const PgrRuntimeTransportString *s,uint32_t n){
  if(op==19&&n==2&&s){ events.emplace_back(std::string(s[0].ptr,s[0].len),std::string(s[1].ptr,s[1].len)); }
  return PGR_TRANSPORT_OK;
}
int tr_get_bool(uint32_t,int*){ return PGR_TRANSPORT_STALE; }
int tr_get_int(uint32_t,int32_t*){ return PGR_TRANSPORT_STALE; }
int tr_get_double(uint32_t,double*,int*){ return PGR_TRANSPORT_STALE; }
int tr_get_string(uint32_t,char*,uint32_t,uint32_t*){ return PGR_TRANSPORT_STALE; }
int tr_get_cmd(char*,uint32_t,uint32_t*,char*,uint32_t,uint32_t*){ return PGR_TRANSPORT_STALE; }
int tr_take_pulse(uint32_t,int*){ return PGR_TRANSPORT_STALE; }
int tr_sample(PgrRuntimeTransportSample *out){
  if(!out) return PGR_TRANSPORT_ERR_NULL_ARG; *out={};
  out->version=PGR_TRANSPORT_SAMPLE_VERSION; out->size=sizeof(*out);
  out->snapshot_fresh=uint32_t(g_transport_fresh); out->fault=uint32_t(g_transport_fault); return 0;
}
int tr_stop(){ return 0; }
PgrRuntimeTransportApi transport{PGR_TRANSPORT_API_VERSION,sizeof(PgrRuntimeTransportApi),
  tr_install,tr_exchange,tr_emit,tr_get_bool,tr_get_int,tr_get_double,tr_get_string,
  tr_get_cmd,tr_take_pulse,tr_sample,tr_stop};

// --- Bootstrap (feature54) ---------------------------------------------------
int bt_probe(const PgrBootConfig*,PgrBootBinding *out){ if(out){*out={};out->version=PGR_BOOT_BINDING_VERSION;out->size=sizeof(*out);} return 0; }
int bt_snapshot(PgrBootBinding *out){
  if(!out) return PGR_BOOT_ERR_NULL_ARG; *out={};
  out->version=PGR_BOOT_BINDING_VERSION; out->size=sizeof(*out); out->handle=&token;
  out->execute_tasks=&dummy_exec; out->execute_tasks_code=&dummy_exec;
  out->runtime.runtime_invoke=&dummy_invoke; out->runtime.runtime_invoke_convert_args=&dummy_convert;
  out->runtime.thread_current=reinterpret_cast<void*>(&thread_current_stub);
  return PGR_BOOT_OK;
}
PgrBootApi boot{PGR_BOOT_API_VERSION,sizeof(PgrBootApi),bt_probe,bt_snapshot};

// --- Unity scheduler (feature48): install FAILS with -3 ----------------------
int sc_install(const PgrUnitySchedulerConfig*){ return g_install_rc; }
uint64_t sc_activate(){ return 1; }
int sc_revoke(uint64_t){ return PGR_US_OK; }
int sc_valid(uint64_t){ return 0; }         // never a live generation: no pulse can run
int sc_snapshot(PgrUnitySchedulerSnapshot *o){ if(o)*o={}; return PGR_US_OK; }
uint64_t sc_exec_gen(uint64_t){ return 0; }
int sc_diag(PgrUnitySchedulerDiag *o){
  if(!o) return PGR_US_BAD_CONFIG; *o={};
  o->version=4; o->size=sizeof(PgrUnitySchedulerDiag); o->last_result=PGR_US_HOOK_FAILED;
  o->failed_stage=PGR_US_STAGE_INVOKE_HOOK; o->invoke_hook_rc=-7; o->convert_hook_rc=0;
  o->poisoned=1; o->invoke_body=0xA000; o->convert_body=0xB000; return PGR_US_OK;
}
PgrUnitySchedulerApi  sched2{2,sizeof(PgrUnitySchedulerApi),sc_install,sc_activate,sc_revoke,sc_valid,sc_snapshot};
PgrUnitySchedulerApi4 sched4{4,sizeof(PgrUnitySchedulerApi4),sc_install,sc_activate,sc_revoke,sc_valid,sc_snapshot,sc_exec_gen,sc_diag};
PgrMetaApi metadata{};  // referenced only by verify(), which the failing install never calls

const void *scheduler_api(uint32_t v){ if(v==2) return &sched2; if(v==4) return &sched4; return nullptr; }
const void *transport_api(uint32_t){ return &transport; }
const void *boot_api(uint32_t){ return &boot; }
const void *metadata_api(uint32_t){ return &metadata; }
// Gameplay host tables are never entered on the configure() path (only pulse()
// and stop() touch them, and this fixture does neither).
int g_unused=0;
const void *host_stub(uint32_t){ return &g_unused; }
} // namespace diagfix

#define pgrazor_unity_scheduler_api   diagfix::scheduler_api
#define pgrazor_runtime_transport_api diagfix::transport_api
#define pgrazor_runtime_bootstrap_api diagfix::boot_api
#define pgrazor_runtime_metadata_api  diagfix::metadata_api
#define pgrazor_shared_host_api       diagfix::host_stub
#define pgrazor_quickball_host_api    diagfix::host_stub
#define pgrazor_routes_host_api       diagfix::host_stub
#define pgrazor_inventory_host_api    diagfix::host_stub
#define pgrazor_spin_host_api         diagfix::host_stub
#define pgrazor_catch_wait_api        diagfix::host_stub
#define pgrazor_spawn_host_api        diagfix::host_stub
#define pgrazor_camera_host_api       diagfix::host_stub
#define pgrazor_combat_host_api       diagfix::host_stub
#define pgrazor_account_host_api      diagfix::host_stub
#define pgrazor_interaction_host_api  diagfix::host_stub
#define pgrazor_encounter_host_api    diagfix::host_stub
#include "runtime_host.cpp"

// Collector boundary: never exercised (no pulse). Present for linkage only.
void pgr_event_collector_location(double,double,uint64_t){}
int pgr_event_collector_tick(bool,uint64_t){ return 0; }
void pgr_event_collector_stop(){}
PgrEventCollectorState pgr_event_collector_state(){ return {}; }

namespace {
bool has(const std::string &hay,const char *needle){ return hay.find(needle)!=std::string::npos; }
unsigned startup_events(){
  unsigned c=0; for(auto &e:diagfix::events) if(e.first=="native-runtime-start") ++c; return c;
}
}

int main(){
  const auto *host=static_cast<const PgrRuntimeHostApi*>(pgrazor_runtime_host_api(1));
  assert(host&&host->size==sizeof(PgrRuntimeHostApi));
  PgrRuntimeHostConfig cfg{1,sizeof(cfg),1,0};

  // 1) Scheduler hook install returns -3. configure() must latch the fault AND
  //    emit a startup diagnostic -- the behavior that was previously silent.
  assert(host->configure(&cfg)==PGR_US_HOOK_FAILED);
  assert(startup_events()==1);
  const std::string &json=diagfix::events.back().second;
  assert(has(json,"\"kind\":\"startup\""));
  assert(has(json,"\"stage\":\"scheduler-install\""));
  assert(has(json,"\"reason\":\"hook-failed\""));
  assert(has(json,"\"status\":-3"));
  // Method/hook identity sourced from the scheduler v4 diagnostics getter.
  assert(has(json,"\"hookStage\":\"invoke-hook\""));
  assert(has(json,"\"invokeRc\":-7"));
  assert(has(json,"\"hookOutcome\":\"installer-error\""));
  assert(has(json,"\"invokeBody\":\"0xa000\""));
  assert(has(json,"\"executeTasks\":\""));

  // 2) The scheduler never started: no pulse, not installed, fault is the -3.
  PgrRuntimeHostSample s{};
  assert(host->sample(&s)==0&&!s.installed&&!s.pulses&&s.fault==PGR_US_HOOK_FAILED);
  for(unsigned i=0;i<12;++i) assert(s.hosts[i].state!=PGR_RTH_READY);

  // 3) Re-polling the latched fault must NOT spam: within the heartbeat window
  //    the identical diagnostic is suppressed (deduped), still returning -3.
  assert(host->configure(&cfg)==PGR_US_HOOK_FAILED);
  assert(host->configure(&cfg)==PGR_US_HOOK_FAILED);
  assert(startup_events()==1);

  std::puts("PASS feature70 startup diagnostics: scheduler hook -3 delivered through transport without a pulse, method/hook identity, latched-fault dedup");
  return 0;
}

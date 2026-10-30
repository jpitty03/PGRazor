// Link this fixture with the production vgp_controller.cpp, not its sibling implementations.
#define main baseline_vgp_controller_main
#include "vgp_controller_test.cpp"
#undef main
#include "smart_catch.h"
#include "smart_catch_binding.h"
#include "runtime_transport.h"
#include <chrono>

namespace {
std::string smart_text="1;1;3;0;1;1500;30;3;1;";
int selected_calls=0,berry_calls=0,berry_confirmations=0,plans=0;
int desired_berry=705;
int encounter_accepted=1;
bool (*admission)(void*)=nullptr;void *admission_context=nullptr;
uint64_t clock_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
int smart_string(uint32_t index,char *out,uint32_t cap,uint32_t *len){assert(index==7);*len=smart_text.size();if(cap<*len)return -1;memcpy(out,smart_text.data(),*len);return 0;}
int smart_emit(uint32_t opcode,const int64_t *,uint32_t,const PgrRuntimeTransportString *,uint32_t count){assert(opcode==19&&count==2);return 0;}
PgrRuntimeTransportApi smart_transport{};
int selected_capture(const PgrVgpActionsTarget *request,int ball,int grade,int curve,uintptr_t *handle){
 assert(request->tid==tid&&request->target==candObj.p());assert(ball==3&&grade==3&&curve==1);
 if(desired_berry)assert(berry_confirmations>=selected_calls+1);
 ++selected_calls;*handle=gcNew(promiseObj.p(),0);return PGR_VA_ISSUED;
}
PgrVgpSmartActionsApi selected_api{2,sizeof(selected_api),selected_capture};
// Run the complete encounter/berry/retry lifecycle with the map hidden by a menu.
PgrVgpControllerTick smart_tick(){auto t=makeTick();t.mask=1;t.flags=PGR_VGC_TF_MANUAL_READY;return t;}
PgrVgpControllerOut step(){return run(smart_tick());}
void next_poll(){usleep(110000);}
uint64_t begin_wait(){
 resetScenario();smart_text="1;1;3;0;1;1500;30;3;1;";g_pollRc=PGR_VR_PENDING;
 auto o=step();assert(o.phase==PGR_VGC_PH_ENCOUNTER);
 g_pollRc=PGR_VR_DONE;g_success=true;next_poll();o=step();
 assert(o.phase==PGR_VGC_PH_SMART_WAIT);return clock_ms();
}
PgrVgpControllerOut await_phase(uint32_t phase,uint64_t deadline){
 PgrVgpControllerOut o{};
 do{usleep(20000);o=step();if(o.phase==phase)return o;}while(clock_ms()<deadline);
 std::fprintf(stderr,"wanted phase %u, got %u status %u\n",phase,o.phase,o.status);assert(false);return o;
}
void confirm_berry_then_throw(){
 int before=selected_calls;g_pollRc=PGR_VR_PENDING;next_poll();auto o=step();assert(o.phase==PGR_VGC_PH_SMART_BERRY&&selected_calls==before);
 g_pollRc=PGR_VR_DONE;next_poll();o=step();assert(o.phase==PGR_VGC_PH_SMART_WAIT&&selected_calls==before);
 next_poll();o=step();assert(o.phase==PGR_VGC_PH_CATCH&&selected_calls==before+1);
}
void assert_clean(){PgrVgpControllerSample s{};assert(api->sample(&s)==0);assert(s.roots==0&&roots.empty());}
}
extern "C" int pgr_smart_parse_settings(const char *text,size_t len,PgrSmartSettings *out){
 if(!out)return PGR_SMART_ERR_NULL_ARG;*out={};out->enabled=len>3&&text&&text[0]=='1'&&text[2]=='1';return 0;
}
extern "C" int pgr_smart_ensure_bound(uint64_t owner){assert(owner==tid);return 0;}
extern "C" int pgr_smart_spawn_fresh(void *target,uint64_t owner){assert(target==candObj.p()&&owner==tid);return 1;}
extern "C" int pgr_smart_encounter_success(void *target,void *response,uint64_t){assert(target==candObj.p()&&response==respObj.p());return encounter_accepted;}
extern "C" int pgr_smart_plan(void *target,void *bag,uint64_t owner,PgrSmartDecision *out){
 assert(target==candObj.p()&&bag==ibagObj.p()&&owner==tid);++plans;*out={};out->enabled=true;out->ball=3;out->grade=3;out->curve=true;out->berry=desired_berry;return 0;
}
void pgr_smart_set_admission_guard(bool (*guard)(void*),void *context){admission=guard;admission_context=context;}
extern "C" int pgr_smart_berry(void *target,void *bag,int32_t berry,uint64_t owner,uint64_t lease,uint64_t *promise,uint64_t *reflection){
 assert(target==candObj.p()&&bag==ibagObj.p()&&berry==705&&owner==tid&&lease>0);assert(admission&&admission(admission_context));
 ++berry_calls;*promise=gcNew(promiseObj.p(),0);*reflection=gcNew(reflObj.p(),0);return 1;
}
extern "C" int pgr_smart_berry_success(void *response,uint64_t owner){assert(response==respObj.p()&&owner==tid);++berry_confirmations;return 1;}
extern "C" const void *pgrazor_runtime_transport_api(uint32_t version){return version==1?&smart_transport:nullptr;}
extern "C" const void *pgrazor_vgp_actions_api(uint32_t version){return version==2?&selected_api:nullptr;}
extern "C" void pgr_vgp_smart_escape_status(int32_t);
int main(){
 api=static_cast<const PgrVgpControllerApi*>(pgrazor_vgp_controller_api(1));assert(api);tid=gettid();
 smart_transport.version=1;smart_transport.size=sizeof(smart_transport);smart_transport.get_string=smart_string;smart_transport.emit=smart_emit;
 auto cfg=makeConfig();assert(api->install(&cfg)==0);pgr_vgp_smart_escape_status(2);
 uint64_t entered=begin_wait();
 // Neither berry nor throw may issue in the first 1.2 seconds following encounter success.
 while(clock_ms()-entered<1150){usleep(20000);auto o=step();assert(o.phase==PGR_VGC_PH_SMART_WAIT&&berry_calls==0&&selected_calls==0);}
 await_phase(PGR_VGC_PH_SMART_BERRY,entered+2200);uint64_t elapsed=clock_ms()-entered;
 assert(elapsed>=1200&&elapsed<=2000);assert(berry_calls==1&&selected_calls==0);
 confirm_berry_then_throw();assert(selected_calls==1&&berry_confirmations==1);
 // Each escaped throw receives another delay and berry confirmation; initial + three retries maximum.
 for(int attempt=1;attempt<=4;attempt++){
  g_catchStatus=2;g_statusKnown=true;g_success=false;g_pollRc=PGR_VR_DONE;g_due=false;next_poll();auto o=step();
  if(attempt==4){assert(o.phase==PGR_VGC_PH_NONE&&selected_calls==4);assert_clean();break;}
  assert(o.phase==PGR_VGC_PH_SMART_WAIT);auto retry_started=clock_ms();g_due=true;
  await_phase(PGR_VGC_PH_SMART_BERRY,retry_started+2200);assert(clock_ms()-retry_started>=1200);
  confirm_berry_then_throw();assert(selected_calls==attempt+1);
 }
 assert(berry_calls==4&&berry_confirmations==4);for(int i=0;i<3;i++){next_poll();step();}assert(selected_calls==4);assert_clean();
 // Disabling settings during the encounter delay cancels the pending action and all roots.
 begin_wait();smart_text="1;0;3;0;1;1500;30;3;1;";next_poll();step();assert(selected_calls==4);assert_clean();
 // Losing manual readiness cancels the pending action before another ball can be issued.
 begin_wait();next_poll();auto t=smart_tick();t.flags&=~PGR_VGC_TF_MANUAL_READY;run(t);assert(selected_calls==4);assert_clean();
 // A real manual encounter still blocks menu catching, including a fresh scan.
 begin_wait();Obj gui(&nameplateClass),manual(&wildClass);
 assert(api->observe(gui.p(),manual.p(),tid)==0);next_poll();step();assert(selected_calls==4);
 g_due=true;next_poll();auto blocked=step();assert(blocked.phase==PGR_VGC_PH_NONE);
 assert(api->observe(gui.p(),nullptr,tid)==0);assert_clean();
 // Permanent stop during the delay also releases pending roots and prevents later ticks issuing throws.
 resetScenario();encounter_accepted=0;g_pollRc=PGR_VR_PENDING;step();
 g_pollRc=PGR_VR_DONE;next_poll();auto declined=step();
 assert(declined.status==PGR_VGC_ST_REJECTED&&selected_calls==4&&berry_calls==4);assert_clean();encounter_accepted=1;
 begin_wait();assert(api->stop(tid)==0);assert_clean();next_poll();step();assert(selected_calls==4&&berry_calls==4);assert_clean();
 std::puts("smart catch production controller pacing, berry confirmation, retry cap and cancellation passed");
}

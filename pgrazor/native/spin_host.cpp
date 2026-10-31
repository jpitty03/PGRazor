#include "spin_host.h"
#include "spin_camera.h"
#include "quick_spin.h"
#include "runtime_transport.h"
#include "shared_host.h"
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <time.h>
#include <unistd.h>
namespace {
using namespace pgr_shared_meta;
PgrSpinHostConfig config{},accepted{};
PgrSharedMetadataOutput metadata{};
PgrSharedMetaExtraExports extra{};
std::mutex setup_mutex,policy_mutex,event_mutex;
std::atomic<bool> configured{false},stopping{false},busy{false};
std::atomic<uint32_t> caps{0},owned{0};
std::atomic<int> camera_error{0},quick_error{0};
std::atomic<uint64_t> ticks{0};
std::atomic<uint64_t> revision{0};
PgrSpinHostOptions policy{};
uint64_t deadline=0,last_poll=0,transport_epoch=0;
bool transport_enabled=false,readiness_sent=false;
std::atomic<bool> revoked_readiness{false};
uint32_t fast_serial=0,camera_error_serial=0,request_serial=0,closed_serial=0,error_serial=0,cooldown_serial=0;
PgrSpinHostEvent events[32]{};unsigned head=0,count=0;
const PgrSpinCameraApi *camera(){return static_cast<const PgrSpinCameraApi*>(pgrazor_spin_camera_api(1));}
const PgrQuickSpinApi *quick(){return static_cast<const PgrQuickSpinApi*>(pgrazor_quick_spin_api(1));}
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
uint64_t now(){timespec t{};return clock_gettime(CLOCK_BOOTTIME,&t)||t.tv_sec<0?0:uint64_t(t.tv_sec)*1000+uint64_t(t.tv_nsec)/1000000;}
bool owner(uint64_t tid){return tid&&tid==uint64_t(gettid())&&tid==config.owner_tid&&config.thread_current&&config.thread_current();}
void revoke(){if(caps.load()&PGR_SPH_CAMERA)camera()->update(0,0,config.owner_tid,0);if(caps.load()&PGR_SPH_QUICK){quick()->update(0,0,config.owner_tid,0);quick()->clear();}}
int stop(){stopping=true;{std::lock_guard<std::mutex> lock(policy_mutex);deadline=0;++revision;}revoke();
 if(config.flags&PGR_SPH_TRANSPORT&&!revoked_readiness.exchange(true)){auto tr=transport();const char text[]="Unavailable: stopped";PgrRuntimeTransportString s{text,sizeof(text)-1};int64_t value=0;if(tr)tr->emit(44,&value,1,&s,1);}return 0;}
bool action(uint32_t opcode,const int64_t *numbers,uint32_t n,const PgrRuntimeTransportString *strings,uint32_t s){
 if(!(config.flags&PGR_SPH_TRANSPORT))return true;
 const auto t=transport();if(t&&t->emit(opcode,numbers,n,strings,s)==0)return true;stop();return false;
}
void event(const char *kind,const char *body){
 PgrSpinHostEvent e{1,sizeof(e)};std::snprintf(e.kind,sizeof(e.kind),"%s",kind);
 std::snprintf(e.json,sizeof(e.json),"{\"kind\":\"%s\",\"boottimeMs\":%llu%s%s}",kind,(unsigned long long)now(),*body?",":"",body);
 if(config.flags&PGR_SPH_TRANSPORT){const char channel[]="quick-close";PgrRuntimeTransportString s[]={{channel,uint32_t(sizeof(channel)-1)},{e.json,uint32_t(std::strlen(e.json))}};action(19,nullptr,0,s,2);}
 else {std::lock_guard<std::mutex> lock(event_mutex);if(count==32){stopping=true;return;}events[(head+count++)%32]=e;}
}
void readiness(){if(readiness_sent)return;readiness_sent=true;const bool ready=(caps.load()&PGR_SPH_QUICK)&&!quick_error.load()&&!stopping;
 const char *reason=ready?"Ready":"Unavailable: native quick-spin metadata or installation";int64_t value=ready;
 PgrRuntimeTransportString s{reason,uint32_t(std::strlen(reason))};action(44,&value,1,&s,1);
}
void *h(unsigned i){return const_cast<void*>(metadata.opt_handles[i]);}
void *k(unsigned i){return const_cast<void*>(metadata.opt_classes[i]);}
int configure(const PgrSpinHostConfig *c){
 if(!c||c->version!=1||c->size!=sizeof(*c)||c->flags&~1u||c->exclusive_owner!=1||!c->metadata||!c->extra||
 !c->object_get_class||!c->field_get_value||!c->runtime_invoke||!c->object_unbox||!c->thread_current||
 c->owner_tid!=uint64_t(gettid())||!c->thread_current()||c->metadata->version!=2||c->metadata->size!=sizeof(metadata)||
 !c->extra->gchandle_new||!c->extra->gchandle_get_target||!c->extra->gchandle_free)return PGR_SPH_BAD_CONFIG;
 std::lock_guard<std::mutex> lock(setup_mutex);if(configured)return std::memcmp(c,&accepted,sizeof(*c))||std::memcmp(c->metadata,&metadata,sizeof(metadata))||std::memcmp(c->extra,&extra,sizeof(extra))?PGR_SPH_REBIND:0;
 if(stopping)return PGR_SPH_STOPPED;accepted=*c;config=*c;metadata=*c->metadata;extra=*c->extra;config.metadata=&metadata;config.extra=&extra;
 if(metadata.optional[OPT_SPIN_CAMERA]==PGR_SHARED_OPT_AVAILABLE){
  PgrSpinCameraConfig p{};p.version=1;p.size=sizeof(p);
  void *values[]={extra.gchandle_new,extra.gchandle_get_target,extra.gchandle_free,c->object_get_class,c->field_get_value,
   h(H_SC_APP_ENTER_CODE),h(H_SC_APP_EXIT_CODE),h(H_SC_RES_ENTER_CODE),h(H_SC_RES_EXIT_CODE),h(H_SC_MOVE_CODE),
   h(H_SC_APPROACH),h(H_SC_RESOLVE),k(OC_SPLINE),h(H_SC_ITER),h(H_SC_APP_CAM),h(H_SC_RES_CAM),h(H_SC_THIS_F),h(H_SC_STATE_F)};
  static_assert(sizeof(values)==sizeof(p)-8,"Camera pointer order");std::memcpy(&p.gchandle_new,values,sizeof(values));
  int rc=camera()?camera()->install(&p):PGR_SPH_FAULT;camera_error=rc;if(!rc){caps.fetch_or(PGR_SPH_CAMERA);PgrSpinCameraSample s{};camera()->sample(&s);fast_serial=s.fast_serial;camera_error_serial=s.error_serial;event("native-spin-camera-ready","\"backend\":\"native-spin-camera\"");}
 }
 if(metadata.optional[OPT_SPIN]==PGR_SHARED_OPT_AVAILABLE){
  PgrQuickSpinConfig p{};p.version=1;p.size=sizeof(p);p.success_value=metadata.opt_ints[I_SPIN_SUCCESS];p.state_open_value=2;
  void *values[]={extra.gchandle_new,extra.gchandle_get_target,extra.gchandle_free,c->object_get_class,c->field_get_value,c->runtime_invoke,c->object_unbox,
   h(H_SPIN_ENTER_CODE),h(H_SPIN_EXIT_CODE),h(H_SPIN_RESULT_CODE),h(H_SPIN_SPIN_CODE),h(H_SPIN_SPIN_M),h(H_SPIN_REQUEST),h(H_SPIN_CAN),h(H_SPIN_ACTIVE),h(H_SPIN_CLOSE),const_cast<void*>(metadata.methods[5]),
   k(OC_SPINSTATE),k(OC_SPINGUI),k(OC_GESTURE),k(OC_SPINNER),k(OC_SPINRESULT),const_cast<void*>(metadata.classes[6]),
   h(H_SPIN_GUI_F),h(H_SPIN_GESTURE_F),h(H_SPIN_SPINNER_F),h(H_SPIN_RESULT_F),const_cast<void*>(metadata.fields[1])};
  static_assert(sizeof(values)==sizeof(p)-16,"Quick pointer order");std::memcpy(&p.gchandle_new,values,sizeof(values));
  int rc=quick()?quick()->install(&p):PGR_SPH_FAULT;quick_error=rc;if(!rc){caps.fetch_or(PGR_SPH_QUICK);PgrQuickSpinSample s{};quick()->sample(&s);request_serial=s.request_serial;closed_serial=s.closed_serial;error_serial=s.error_serial;cooldown_serial=s.cooldown_serial;event("native-quick-spin-ready","\"backend\":\"native-quick-spin\"");}
 }
 configured=true;readiness();return 0;
}
int update(const PgrSpinHostOptions *p){
 if(!p||p->version!=1||p->size!=sizeof(*p)||p->enabled>1||p->lease_ms>2500||!p->epoch){stop();return PGR_SPH_BAD_CONFIG;}
 if(!configured)return PGR_SPH_BAD_CONFIG;if(stopping)return PGR_SPH_STOPPED;
 const auto t=now();if(!t){stop();return PGR_SPH_FAULT;}
 bool changed=false;{std::lock_guard<std::mutex> lock(policy_mutex);if(p->epoch<policy.epoch)return PGR_SPH_BAD_CONFIG;changed=p->epoch!=policy.epoch||!p->enabled;policy=*p;deadline=p->enabled?t+p->lease_ms:0;++revision;}if(changed)revoke();return 0;
}
void poll(uint64_t t){
 if(!(config.flags&PGR_SPH_TRANSPORT)||t<last_poll+50)return;last_poll=t;
 PgrRuntimeTransportSample before{},after{};int enabled=0;const auto tr=transport();
 // A settings publication between reads is not a revocation. Retry a bounded
 // number of times; stale, disabled, or unreadable transport still fails closed.
 bool fresh=false;
 for(unsigned attempt=0;tr&&attempt<3;++attempt){
  if(tr->sample(&before)||!before.snapshot_fresh||tr->get_bool(24,&enabled)||tr->sample(&after)||!after.snapshot_fresh)break;
  if(before.snapshot_seq==after.snapshot_seq){fresh=true;break;}
 }
 bool allow=fresh&&enabled==1;
 uint64_t old_deadline;{std::lock_guard<std::mutex> lock(policy_mutex);old_deadline=deadline;}
 if(!transport_epoch||allow!=transport_enabled||(allow&&t>=old_deadline)){if(transport_epoch==UINT64_MAX){stop();return;}++transport_epoch;}
 // Opening a stop can stall a frame beyond 100ms. Keep its record through
 // short stalls while continuing to check transport every 50ms when ticking.
 transport_enabled=allow;PgrSpinHostOptions p{1,sizeof(p),uint32_t(allow),allow?1000u:0u,transport_epoch};update(&p);
}
void drain(){char body[512];
 if(caps.load()&PGR_SPH_CAMERA){PgrSpinCameraSample s{};if(camera()->sample(&s)){camera_error=PGR_SPH_FAULT;stop();}else{
  if(s.fast_serial!=fast_serial){fast_serial=s.fast_serial;if(std::isfinite(s.last_original_speed)&&s.last_original_speed>0){double ms=1000.0/s.last_original_speed;std::snprintf(body,sizeof(body),"\"phase\":\"%s\",\"originalMs\":%.9g,\"targetMs\":%.9g,\"backend\":\"native\"",s.last_phase==1?"in":s.last_phase==2?"out":"unknown",ms,std::min(ms,40.0));event("spin-camera-fast",body);}}
  if(s.error_serial!=camera_error_serial){camera_error_serial=s.error_serial;const char *reasons[]={"","gchandle-enter-failed","gchandle-iterator-failed","clock-boottime-failed","reentrant-overflow"};std::snprintf(body,sizeof(body),"\"reason\":\"%s%u\",\"backend\":\"native\"",s.last_error<5?reasons[s.last_error]:"native-error:",s.last_error<5?0:s.last_error);if(s.last_error<5)std::snprintf(body,sizeof(body),"\"reason\":\"%s\",\"backend\":\"native\"",reasons[s.last_error]);event("spin-camera-error",body);}
  if(s.fault){camera_error=PGR_SPH_FAULT;stop();}
 }}
 if(caps.load()&PGR_SPH_QUICK){PgrQuickSpinSample s{};if(quick()->sample(&s)){quick_error=PGR_SPH_FAULT;stop();return;}
  uint32_t delta=s.cooldown_serial-cooldown_serial;cooldown_serial=s.cooldown_serial;
  if(delta>1024){quick_error=PGR_SPH_FAULT;stop();return;} // bounded; never silently drop cooldown actions
  for(uint32_t i=0;i<delta;++i){event("cooldown-action","\"action\":\"Pokestop spin\"");const char text[]="Pokestop spin";PgrRuntimeTransportString str{text,sizeof(text)-1};action(17,nullptr,0,&str,1);}
  if(s.request_serial!=request_serial){request_serial=s.request_serial;std::snprintf(body,sizeof(body),"\"tid\":%llu,\"immediate\":true,\"guiState\":%d,\"openedMs\":%llu",(unsigned long long)s.last_tid,s.last_gui_state,(unsigned long long)s.last_opened_ms);event("spin-requested",body);}
  if(s.closed_serial!=closed_serial){closed_serial=s.closed_serial;std::snprintf(body,sizeof(body),"\"tid\":%llu,\"openedMs\":%llu",(unsigned long long)s.last_tid,(unsigned long long)s.last_opened_ms);event("spin-dismissed",body);}
  if(s.error_serial!=error_serial){error_serial=s.error_serial;const char *reasons[]={"","","gchandle-state-failed","gchandle-gui-failed","gchandle-temp-failed","spin-bool-type","alive-invoke-exception","active-invoke-exception","can-invoke-exception","spin-invoke-exception","request-invoke-exception","close-invoke-exception","clock-boottime-failed","reentrant-overflow"};
   if(s.last_error_code==1)std::snprintf(body,sizeof(body),"\"result\":%d,\"tid\":%llu",s.last_result,(unsigned long long)s.last_tid);
   else if(s.last_error_code<14)std::snprintf(body,sizeof(body),"\"reason\":\"%s\",\"tid\":%llu",reasons[s.last_error_code],(unsigned long long)s.last_tid);
   else std::snprintf(body,sizeof(body),"\"reason\":\"native-error:%u\",\"tid\":%llu",s.last_error_code,(unsigned long long)s.last_tid);event("spin-error",body);
  }
 }
}
int tick(uint64_t tid,uint64_t sequence,uint64_t opener){
 if(!configured)return PGR_SPH_BAD_CONFIG;if(!owner(tid))return PGR_SPH_OFF_OWNER;if(busy.exchange(true))return PGR_SPH_BUSY;
 struct Guard{~Guard(){busy=false;}} guard;const auto t=now();if(!t){stop();return PGR_SPH_FAULT;}++ticks;if(!stopping)poll(t);
 // Leaves may free retired GC roots during update. Never hold host locks across
 // their calls; re-check stop after publication before permitting any leaf tick.
 PgrSpinHostOptions p{};uint64_t until,entry_revision;{std::lock_guard<std::mutex> lock(policy_mutex);p=policy;until=deadline;entry_revision=revision;}
 bool enabled=!stopping&&ticks.load()>=3&&p.enabled&&t<until;uint64_t lease=enabled?std::min<uint64_t>(2500,until-t):0;
 int cr=0,qr=0;
 if(caps.load()&PGR_SPH_CAMERA)cr=camera()->update(enabled,lease,tid,p.epoch);
 if(stopping||entry_revision!=revision.load())revoke();
 if(caps.load()&PGR_SPH_QUICK){bool live=!stopping&&entry_revision==revision.load();qr=quick()->update(enabled&&live,live?lease:0,tid,p.epoch);}
 if(stopping||entry_revision!=revision.load())revoke();
 if(cr){camera_error=cr;stop();}if(qr){quick_error=qr;stop();}
 if(caps.load()&PGR_SPH_CAMERA){int r=camera()->tick(tid);if(r){camera_error=r;stop();}}
 if(caps.load()&PGR_SPH_QUICK){int r=quick()->tick(tid,sequence,opener);if(r){quick_error=r;stop();}}
 drain();uint32_t roots=0;PgrSpinCameraSample a{};PgrQuickSpinSample b{};
 if(caps.load()&PGR_SPH_CAMERA){if(camera()->sample(&a))++roots;else roots+=a.active_roots_mask!=0;}
 if(caps.load()&PGR_SPH_QUICK){if(quick()->sample(&b))++roots;else roots+=b.owned_root_count+(b.active_generation!=0);}
 owned=roots;return stopping?PGR_SPH_STOPPED:0;
}
int sample(PgrSpinHostSample *s){if(!s)return PGR_SPH_BAD_CONFIG;*s={1,sizeof(*s),uint32_t(configured.load()),caps.load(),uint32_t(stopping.load()),owned.load(),camera_error.load(),quick_error.load(),configured?config.owner_tid:0,ticks.load()};return 0;}
int next_event(PgrSpinHostEvent *e){if(!e)return PGR_SPH_BAD_CONFIG;std::lock_guard<std::mutex> lock(event_mutex);if(!count){*e={};return PGR_SPH_EMPTY;}*e=events[head];head=(head+1)%32;--count;return 0;}
void pulse(void*,const PgrUnityPulse *p){if(!p||p->reserved||p->tid<=0)return;auto scheduler=static_cast<const PgrUnitySchedulerApi*>(pgrazor_unity_scheduler_api(2));if(!scheduler||scheduler->valid(p->generation)!=1)return;
 uint64_t opener=1;auto shared=static_cast<const PgrSharedHostApi*>(pgrazor_shared_host_api(1));PgrSharedHostSample s{};if(shared&&!shared->sample(&s))opener=s.hook_frames;tick(p->tid,p->sequence,opener);
}
const PgrSpinHostApi api{1,sizeof(PgrSpinHostApi),configure,update,tick,stop,sample,next_event,pulse};
}
extern "C" const void *pgrazor_spin_host_api(uint32_t version){return version==1?&api:nullptr;}

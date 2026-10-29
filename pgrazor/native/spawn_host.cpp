#include "spawn_host.h"
#include "pgrazor_native_api.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "shared_host.h"
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <time.h>
#include <unistd.h>
namespace {
using namespace pgr_shared_meta;
std::mutex config_mutex,policy_mutex,event_mutex;
std::atomic<bool> configured{false},installed{false},stopping{false},busy{false},ready{false};
std::atomic<int> error{0},leaf_error{0};
std::atomic<uint64_t> owner_tid{0},ticks{0};
PgrSpawnHostConfig accepted{};uint32_t flags=0;bool bootstrap=false;
void *thread_current=nullptr;const void *spawn_code=nullptr;
PgrSpawnHostOptions policy{};uint64_t deadline=0;
uint64_t sample_at=0;double previous_original=0,previous_effective=0;bool sampled=false;
PgrSpawnHostEvent events[32]{};uint32_t head=0,count=0;
const pgrazor_native_api_v1 *leaf(){return static_cast<const pgrazor_native_api_v1*>(pgrazor_native_api(1));}
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
uint64_t now(){timespec t{};return clock_gettime(CLOCK_BOOTTIME,&t)||t.tv_sec<0?0:uint64_t(t.tv_sec)*1000+uint64_t(t.tv_nsec)/1000000;}
void disable(){auto l=leaf();if(l&&l->update)l->update(0,0,0);}
void fault(int status){error=status;stopping=true;ready=false;{std::lock_guard<std::mutex> lock(policy_mutex);deadline=0;}disable();if(flags&PGR_SPAH_TRANSPORT){auto tr=transport();int64_t value=0;if(tr)tr->emit(49,&value,1,nullptr,0);}}
bool action(uint32_t op,const int64_t *v,uint32_t n,const PgrRuntimeTransportString *s,uint32_t ns){if(!(flags&PGR_SPAH_TRANSPORT))return true;auto tr=transport();if(tr&&tr->emit(op,v,n,s,ns)==0)return true;fault(PGR_SPAH_TRANSPORT_ERROR);return false;}
void event(const char *kind,const char *body){PgrSpawnHostEvent e{1,sizeof(e)};std::snprintf(e.json,sizeof(e.json),"{\"kind\":\"%s\",\"boottimeMs\":%llu%s%s}",kind,(unsigned long long)now(),*body?",":"",body);
 if(flags&PGR_SPAH_TRANSPORT){const char channel[]="quick-close";PgrRuntimeTransportString s[]={{channel,sizeof(channel)-1},{e.json,uint32_t(std::strlen(e.json))}};action(19,nullptr,0,s,2);}
 else{bool overflow=false;{std::lock_guard<std::mutex> lock(event_mutex);if(count==32)overflow=true;else events[(head+count++)%32]=e;}if(overflow)fault(PGR_SPAH_TRANSPORT_ERROR);}
}
void publish_ready(bool value){value=value&&!stopping;int64_t b=value;action(49,&b,1,nullptr,0);event("spawn-ready",value?"\"available\":true":"\"available\":false,\"reason\":\"native-spawn-unavailable\"");}
int stop(){bool first=!stopping.exchange(true);ready=false;{std::lock_guard<std::mutex> lock(policy_mutex);deadline=0;disable();}if(first&&configured)publish_ready(false);return 0;}
int configure(const PgrSpawnHostConfig *c){
 if(c&&(c->version!=1||c->size!=sizeof(*c)||c->flags&~1u||c->exclusive_owner!=1||!c->metadata||c->metadata->version!=2||c->metadata->size!=sizeof(PgrSharedMetadataOutput)||!c->thread_current||c->owner_tid!=uint64_t(gettid())))return PGR_SPAH_BAD_CONFIG;
 std::lock_guard<std::mutex> lock(config_mutex);if(configured){if(bootstrap)return c?PGR_SPAH_REBIND:0;return c&&!std::memcmp(c,&accepted,sizeof(*c))&&spawn_code==(c->metadata->optional[OPT_SPAWN]==PGR_SHARED_OPT_AVAILABLE?c->metadata->opt_handles[H_SP_CODE]:nullptr)?0:PGR_SPAH_REBIND;}
 if(stopping)return PGR_SPAH_STOPPED;
 if(c){if(!reinterpret_cast<void*(*)()>(c->thread_current)())return PGR_SPAH_OFF_OWNER;accepted=*c;flags=c->flags;thread_current=c->thread_current;owner_tid=c->owner_tid;if(c->metadata->optional[OPT_SPAWN]==PGR_SHARED_OPT_AVAILABLE)spawn_code=c->metadata->opt_handles[H_SP_CODE];}
 else{bootstrap=true;flags=PGR_SPAH_TRANSPORT;}configured=true;return 0;
}
int update(const PgrSpawnHostOptions *p){
 if(!p||p->version!=1||p->size!=sizeof(*p)||p->enabled>1||p->allowed>1||p->lease_ms>2500||p->reserved){fault(PGR_SPAH_BAD_CONFIG);return PGR_SPAH_BAD_CONFIG;}
 if(!configured)return PGR_SPAH_BAD_CONFIG;if(stopping)return PGR_SPAH_STOPPED;auto t=now();if(!t){fault(PGR_SPAH_UNAVAILABLE);return PGR_SPAH_UNAVAILABLE;}
 std::lock_guard<std::mutex> lock(policy_mutex);policy=*p;deadline=p->enabled&&p->allowed?t+p->lease_ms:0;if(!deadline)disable();return 0;
}
int prepare(uint64_t tid){
 if(bootstrap&&!thread_current){PgrBootBinding b{};auto boot=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));if(!boot||boot->snapshot(&b)!=0)return PGR_SPAH_NOT_READY;
  auto scheduler=static_cast<const PgrUnitySchedulerApi*>(pgrazor_unity_scheduler_api(2));PgrUnitySchedulerSnapshot state{};
  if(!scheduler||scheduler->snapshot(&state)||state.owner_tid<=0||uint64_t(state.owner_tid)!=tid)return PGR_SPAH_NOT_READY;
  auto sh=static_cast<const PgrSharedHostApi*>(pgrazor_shared_host_api(1));auto md=sh&&sh->metadata?sh->metadata():nullptr;if(!md)return PGR_SPAH_NOT_READY;
  if(md->version!=2||md->size!=sizeof(*md)){fault(PGR_SPAH_BAD_CONFIG);return PGR_SPAH_BAD_CONFIG;}
  thread_current=b.runtime.thread_current;owner_tid=tid;if(md->optional[OPT_SPAWN]==PGR_SHARED_OPT_AVAILABLE)spawn_code=md->opt_handles[H_SP_CODE];
 }
 if(!thread_current||!reinterpret_cast<void*(*)()>(thread_current)())return PGR_SPAH_OFF_OWNER;
 if(tid!=owner_tid.load())return PGR_SPAH_OFF_OWNER;
 if(!spawn_code){fault(PGR_SPAH_UNAVAILABLE);publish_ready(false);return PGR_SPAH_UNAVAILABLE;}
 auto l=leaf();if(!l||l->version!=1||l->size!=sizeof(*l)||!l->install||!l->update||!l->sample){fault(PGR_SPAH_UNAVAILABLE);return PGR_SPAH_UNAVAILABLE;}
 const int rc=l->install(const_cast<void*>(spawn_code));leaf_error=rc;if(rc){fault(PGR_SPAH_UNAVAILABLE);publish_ready(false);return PGR_SPAH_UNAVAILABLE;}
 installed=true;ready=true;event("native-spawn-ready","\"backend\":\"dobby\",\"selftest\":false");publish_ready(true);return stopping?PGR_SPAH_STOPPED:0;
}
int tick(uint64_t tid){
 if(!configured)return PGR_SPAH_BAD_CONFIG;if(!tid||tid!=uint64_t(gettid()))return PGR_SPAH_OFF_OWNER;if(busy.exchange(true))return PGR_SPAH_BUSY;struct Guard{~Guard(){busy=false;}} guard;
 if(stopping){disable();return PGR_SPAH_STOPPED;}if(!installed){int rc=prepare(tid);if(rc)return rc;}
 if(tid!=owner_tid.load()||!thread_current||!reinterpret_cast<void*(*)()>(thread_current)())return PGR_SPAH_OFF_OWNER;
 ++ticks;auto t=now();if(!t){fault(PGR_SPAH_UNAVAILABLE);return PGR_SPAH_UNAVAILABLE;}
 if(flags&PGR_SPAH_TRANSPORT){auto tr=transport();PgrRuntimeTransportSample a{},b{};int allowed=0,enabled=0;
  bool coherent=tr&&!tr->sample(&a)&&a.snapshot_fresh&&!a.fault&&!tr->get_bool(27,&allowed)&&!tr->get_bool(28,&enabled)&&!tr->sample(&b)&&b.snapshot_fresh&&!b.fault&&a.snapshot_seq==b.snapshot_seq;
  PgrSpawnHostOptions p{1,sizeof(p),uint32_t(coherent&&enabled==1),uint32_t(coherent&&allowed==1),coherent?100u:0u,0};update(&p);
 }
 int rc=0;{std::lock_guard<std::mutex> lock(policy_mutex);bool active=!stopping&&policy.enabled&&policy.allowed&&t<deadline;rc=leaf()->update(active,active,active?std::min<uint64_t>(2500,deadline-t):0);}
 if(rc){leaf_error=rc;fault(PGR_SPAH_UNAVAILABLE);publish_ready(false);return PGR_SPAH_UNAVAILABLE;}
 pgrazor_spawn_snapshot s{};rc=leaf()->sample(&s);if(rc){leaf_error=rc;fault(PGR_SPAH_UNAVAILABLE);return PGR_SPAH_UNAVAILABLE;}
 if(std::isfinite(s.original)&&s.original>0&&std::isfinite(s.effective)&&s.effective>0&&(!sampled||t-sample_at>=2000||s.original!=previous_original||s.effective!=previous_effective)){
  sampled=true;sample_at=t;previous_original=s.original;previous_effective=s.effective;char body[128];std::snprintf(body,sizeof(body),"\"original\":%.17g,\"effective\":%.17g",s.original,s.effective);event("spawn-range",body);
  int64_t values[2];std::memcpy(values,&s.original,8);std::memcpy(values+1,&s.effective,8);action(48,values,2,nullptr,0);
 }return stopping?PGR_SPAH_STOPPED:0;
}
int sample(PgrSpawnHostSample *s){if(!s)return PGR_SPAH_BAD_CONFIG;pgrazor_spawn_snapshot v{};auto l=leaf();if(l&&l->sample)l->sample(&v);*s={1,sizeof(*s),uint32_t(configured.load()),uint32_t(installed.load()),uint32_t(stopping.load()),uint32_t(ready.load()),error.load(),leaf_error.load(),owner_tid.load(),ticks.load(),v.original,v.effective,v.count};return 0;}
int next(PgrSpawnHostEvent *e){if(!e)return PGR_SPAH_BAD_CONFIG;std::lock_guard<std::mutex> lock(event_mutex);if(!count){*e={};return PGR_SPAH_EMPTY;}*e=events[head];head=(head+1)%32;--count;return 0;}
void pulse(void*,const PgrUnityPulse *p){if(!p||p->reserved||p->tid<=0)return;auto a=static_cast<const PgrUnitySchedulerApi*>(pgrazor_unity_scheduler_api(2));if(a&&a->valid(p->generation)==1)tick(p->tid);}
const PgrSpawnHostApi api{1,sizeof(api),configure,update,tick,stop,sample,next,pulse};
}
extern "C" const void *pgrazor_spawn_host_api(uint32_t version){return version==1?&api:nullptr;}

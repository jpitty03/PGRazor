#include "deep_radar.h"
#include "deep_radar_metadata.h"
#include "deep_radar_dex.h"
#include "s2_geometry.h"
#include "runtime_transport.h"
#include "unity_scheduler.h"
#include "hook_engine.h"
#include <android/log.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <time.h>
#include <unistd.h>

namespace {
using namespace pgr::radar;
using Response=void(*)(void*,void*,bool,const void*);
Response original=nullptr;
PgrBootBinding boot{};ReaderConfig reader{};
std::unique_ptr<Packet[]> packets;
std::unique_ptr<Store> store;
ReadStats latest{};
std::atomic<uint64_t> owner{0},deadline{0},thread_skips{0},lease_skips{0};
std::atomic_flag busy=ATOMIC_FLAG_INIT;
uint64_t generation=1,sequence=0,published=0,next_publish=0,total_packets=0,last_packet=0,invalid=0,overflows=0;
uint64_t next_scene=0,last_scene_log=0;
unsigned head=0,queued=0;
bool attempted=false,installed=false,enabled=false,unsupported=false;
char reason[160]{};
std::array<int8_t,100001> uncaught{};
std::array<uint64_t,100001> dex_at{};
uint64_t now(){timespec ts{};if(clock_gettime(CLOCK_BOOTTIME,&ts)||ts.tv_sec<0)return 0;return uint64_t(ts.tv_sec)*1000+uint64_t(ts.tv_nsec)/1000000;}
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
bool allowed(){const auto t=now();if(!t||t>=deadline.load(std::memory_order_acquire))return false;auto tr=transport();int value=0;return tr&&tr->get_bool(41,&value)==0&&value==1;}
struct Guard {bool held=!busy.test_and_set(std::memory_order_acquire);~Guard(){if(held)busy.clear(std::memory_order_release);}};
void response(void *self,void *out,bool diff,const void *method){
 // Never adopt an arbitrary callback thread. Even GC handle creation is not
 // allowed here until the scheduler has independently established this owner.
 if(uint64_t(gettid())!=owner.load(std::memory_order_acquire))++thread_skips;
 else if(!allowed())++lease_skips;
 else if(reinterpret_cast<void*(*)()>(boot.runtime.thread_current)()){
  Guard guard;if(guard.held){
   if(queued==2)++overflows;
   else{Packet &p=packets[(head+queued)%2];p.sequence=++sequence;p.observed_ms=now();ReadStats stats;
    const int rc=read_response(reader,out,p,stats,deadline.load(std::memory_order_acquire));
    if(rc==ReadOk&&allowed()){++queued;latest=stats;++total_packets;last_packet=p.observed_ms;
     __android_log_print(ANDROID_LOG_INFO,"PGRazor","deep-radar packet=%llu cells=%u nearby=%u offstop=%u wild=%u catchable=%u top=%u distanceN=%u min=%.1f max=%.1f sentinel200=%u nonL15=%u shortTth=%u truncated=%d",(unsigned long long)total_packets,stats.cells,stats.nearby,stats.offstop,stats.wild,stats.catchable,stats.top_level,stats.distance_count,stats.distance_min,stats.distance_max,stats.distance_200,stats.non_l15,stats.short_tth,int(p.truncated));
     __android_log_print(ANDROID_LOG_INFO,"PGRazor","deep-radar detail fortN=%u offDistanceN=%u offMin=%.1f offMax=%.1f off200=%u displays=%u shinyTrue=%u missingPositions=%u",p.fort_count,stats.offstop_distance_count,stats.offstop_distance_min,stats.offstop_distance_max,stats.offstop_distance_200,stats.display_count,stats.shiny_true,stats.missing_positions);
    }else if(rc<0){++invalid;std::snprintf(reason,sizeof(reason),"Response rejected (%d)",rc);__android_log_print(ANDROID_LOG_WARN,"PGRazor","deep-radar rejected=%d",rc);}
   }
  }
 }
 // Observer has no allocation safepoints, no invoke, no retained references;
 // forward the original frame exactly once, even when radar rejected it.
 original(self,out,diff,method);
}
std::string quote(const char *v){std::string s="\"";for(const unsigned char *p=reinterpret_cast<const unsigned char*>(v);*p;++p){if(*p=='"'||*p=='\\'){s+='\\';s+=char(*p);}else if(*p<32){char b[7];std::snprintf(b,sizeof(b),"\\u%04x",*p);s+=b;}else s+=char(*p);}return s+'"';}
std::string number(double d){char b[48];std::snprintf(b,sizeof(b),"%.9f",d);return b;}
void publish(uint64_t t){
 if(t<next_publish)return;next_publish=t+1000;
 const char *status=unsupported?"unsupported":!total_packets?"waiting":t-last_packet>=90000?"stale":overflows?"overflow":"ready";
 std::string s="{\"version\":1,\"generation\":"+std::to_string(generation)+",\"sequence\":"+std::to_string(++published)+",\"sampledAt\":"+std::to_string(t)+",\"status\":"+quote(status)+",\"message\":"+quote(reason)+",\"packets\":"+std::to_string(total_packets)+",\"cells\":"+std::to_string(latest.cells)+",\"nearby\":"+std::to_string(latest.nearby)+",\"offstop\":"+std::to_string(latest.offstop)+",\"wild\":"+std::to_string(latest.wild)+",\"catchable\":"+std::to_string(latest.catchable)+",\"threadSkips\":"+std::to_string(thread_skips.load())+",\"leaseSkips\":"+std::to_string(lease_skips.load())+",\"rejected\":"+std::to_string(invalid)+",\"rows\":[";
 if(store&&!unsupported){for(size_t i=0;i<store->count();++i){const Entry &e=store->at(i);const Observation&o=e.observation;if(i)s+=',';
   const char *kind=o.kind==Exact?"exact":o.kind==FortLinked?"fort":o.kind==Cell?"cell":"unlocated";LatLng position{o.lat,o.lon};std::array<LatLng,4> corners{};
   const bool cell=o.kind==Cell&&cell_vertices(o.cell,corners);if(cell)position=cell_center(o.cell);
   s+="{\"encounter\":\""+std::to_string(o.encounter)+"\",\"cell\":\""+std::to_string(o.cell)+"\",\"species\":"+std::to_string(o.species)+",\"form\":"+std::to_string(o.form)+",\"costume\":"+std::to_string(o.costume)+",\"gender\":"+std::to_string(o.gender)+",\"kind\":"+quote(kind)+",\"lat\":"+number(position.lat)+",\"lon\":"+number(position.lon)+",\"seenAt\":"+std::to_string(e.seen_ms)+",\"expiresAt\":"+std::to_string(e.expires_ms)+",\"uncaught\":"+std::to_string(dex_at[o.species]?int(uncaught[o.species]):-1)+",\"fort\":"+quote(o.fort)+",\"corners\":[";
   if(cell)for(unsigned j=0;j<4;++j){if(j)s+=',';s+='['+number(corners[j].lat)+','+number(corners[j].lon)+']';}s+="]}";
   if(s.size()>900000){++overflows;std::snprintf(reason,sizeof(reason),"Snapshot exceeded bounded transport capacity");return;}
  }}s+="]}";PgrRuntimeTransportString arg{s.data(),uint32_t(s.size())};auto tr=transport();
 // A radar snapshot must not fill52's shared reliable action queue. At most one
 // small queue backlog is tolerated; keep latest local state for the next tick.
 PgrRuntimeTransportSample sample{};if(tr&&tr->sample(&sample)==0&&sample.queue_depth<32&&sample.queue_bytes+s.size()+128<1048576)tr->emit(59,nullptr,0,&arg,1);
}
void reset(){deadline=0;queued=head=0;if(store)store->clear();latest={};total_packets=last_packet=next_publish=next_scene=0;invalid=overflows=0;sequence=0;++generation;dex_at.fill(0);if(!unsupported)reason[0]=0;}
}
void pgr_deep_radar_tick(uint64_t tid,uint64_t remaining){
 if(!tid||uint64_t(gettid())!=tid)return;
 const auto *scheduler=static_cast<const PgrUnitySchedulerApi*>(pgrazor_unity_scheduler_api(2));PgrUnitySchedulerSnapshot ss{};
 if(!scheduler||scheduler->snapshot(&ss)||ss.owner_tid<=0||uint64_t(ss.owner_tid)!=tid||!ss.generation||!scheduler->valid(ss.generation))return;
 owner=tid;Guard guard;if(!guard.held)return;
 int on=0;auto tr=transport();const uint64_t t=now();const bool active=t&&remaining&&tr&&tr->get_bool(41,&on)==0&&on==1;
 if(!active){if(enabled)reset();enabled=false;return;}enabled=true;deadline=t+std::min<uint64_t>(remaining,2500);
 if(!attempted){
  const auto *bootstrap=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);
  if(!bootstrap||bootstrap->snapshot(&boot)||!boot.runtime.thread_current||!reinterpret_cast<void*(*)()>(boot.runtime.thread_current)())return;
  attempted=true;packets.reset(new(std::nothrow)Packet[2]);store.reset(new(std::nothrow)Store);
  const void *code=nullptr;installed=packets&&store&&pgr_deep_radar_bind(boot,reader,&code,reason,sizeof(reason));
  if(installed){reader.clock_ms=now;reader.allowed=allowed;installed=allowed()&&pgrazor_hook(const_cast<void*>(code),reinterpret_cast<void*>(response),reinterpret_cast<void**>(&original))==0&&original;}
  if(!installed){unsupported=true;if(!reason[0])std::snprintf(reason,sizeof(reason),"Radar observation unavailable");}
  __android_log_print(installed?ANDROID_LOG_INFO:ANDROID_LOG_WARN,"PGRazor","deep-radar: %s",installed?"passive observer installed":reason);
 }
 if(installed){while(queued){Packet &p=packets[head];if(!store->ingest(p)){++invalid;std::snprintf(reason,sizeof(reason),"Packet validation rejected scalar batch");}head=(head+1)%2;--queued;}store->expire(t);
  if(t>=next_scene&&allowed()){next_scene=t+2000;const auto current=now();if(current<deadline.load()){const unsigned resolved=pgr_deep_radar_scene(boot,*store,tid,current,deadline.load()-current,allowed);if(total_packets&&resolved&&last_scene_log!=last_packet){last_scene_log=last_packet;__android_log_print(ANDROID_LOG_INFO,"PGRazor","deep-radar sceneResolved=%u",resolved);}}}
  unsigned budget=4;for(size_t i=0;i<store->count()&&budget&&allowed();++i){const unsigned species=unsigned(store->at(i).observation.species);if(!dex_at[species]||t-dex_at[species]>=60000){uncaught[species]=int8_t(pgr_deep_radar_uncaught(boot,int(species),allowed));dex_at[species]=t;--budget;}}
 }
 publish(t);
}
void pgr_deep_radar_seed_forts(uint64_t tid,const PgrMapPoiOutput*out){
 if(!out||out->version!=PGR_MP_OUTPUT_VERSION||out->size!=sizeof(*out)||out->status!=PGR_MP_OK||out->count>PGR_MP_MAX_POINTS||tid!=owner.load()||tid!=uint64_t(gettid())||!allowed())return;
 Guard guard;if(!guard.held||!store)return;static std::array<Fort,PGR_MP_MAX_POINTS> points;size_t count=0;
 for(unsigned i=0;i<out->count;++i){const auto&p=out->points[i];if(!p.id_length||p.id_length>256)continue;auto&f=points[count];f=Fort{};bool ascii=true;for(unsigned j=0;j<p.id_length;++j){if(!p.id[j]||p.id[j]>127){ascii=false;break;}f.id[j]=char(p.id[j]);}if(!ascii)continue;f.lat=p.lat;f.lon=p.lon;++count;}
 store->seed_forts(points.data(),count,now());
}
void pgr_deep_radar_stop(uint64_t tid){deadline=0;if(tid&&tid==owner.load()&&tid==uint64_t(gettid())){Guard guard;if(guard.held){reset();enabled=false;}}}
extern "C" const void *pgrazor_deep_radar_api(uint32_t version){static const PgrDeepRadarApi api{1,sizeof(api),pgr_deep_radar_tick,pgr_deep_radar_stop};return version==1?&api:nullptr;}

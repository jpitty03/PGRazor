// Positive integration fixture: unchanged manager70 + actual54/47/48/52/58.
// Gameplay hosts are native callback spies, not gameplay implementations. This
// proves scheduling/orchestration boundaries, not end-to-end game behavior.
#include "runtime_host.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "unity_scheduler.h"
#include "shared_host.h"
#include "quickball_host.h"
#include "routes_host.h"
#include "inventory_host.h"
#include "spin_host.h"
#include "camera_host.h"
#include "combat_host.h"
#include "account_host.h"
#include "spawn_host.h"
#include "encounter_host.h"
#include "interaction_host.h"
#include "catch_wait.h"
#include "catch_wait_schema.h"
#include "pgrazor_native_api.h"
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
// Reuse source-known58 fixture descriptors/prologues/objects. All native tables
// below still come from the actual core. Its standalone main is not executed.
namespace universe {
#include "catch_wait_test.cpp"
}
PgrUnitySchedulerApi flow_scheduler{};PgrRuntimeTransportApi flow_transport{};
PgrBootApi flow_boot{};PgrMetaApi flow_metadata{};PgrCatchWaitApi flow_wait{};
PgrSharedHostApi flow_shared{};PgrQuickballHostApi flow_quick{};PgrRoutesHostApi flow_routes{};
PgrInventoryHostApi flow_inventory{};PgrSpinHostApi flow_spin{};PgrSpawnHostApi flow_spawn{};
PgrCameraHostApi flow_camera{};PgrCombatHostApi flow_combat{};PgrAccountHostApi flow_account{};
PgrEncounterHostApi flow_encounter{};PgrInteractionHostApi flow_interaction{};
#define TABLE(NAME,VALUE) const void *NAME(uint32_t){return &VALUE;}
TABLE(flow_scheduler_api,flow_scheduler) TABLE(flow_transport_api,flow_transport)
TABLE(flow_boot_api,flow_boot) TABLE(flow_metadata_api,flow_metadata) TABLE(flow_wait_api,flow_wait)
TABLE(flow_shared_api,flow_shared) TABLE(flow_quick_api,flow_quick) TABLE(flow_routes_api,flow_routes)
TABLE(flow_inventory_api,flow_inventory) TABLE(flow_spin_api,flow_spin) TABLE(flow_spawn_api,flow_spawn)
TABLE(flow_camera_api,flow_camera) TABLE(flow_combat_api,flow_combat) TABLE(flow_account_api,flow_account)
TABLE(flow_encounter_api,flow_encounter) TABLE(flow_interaction_api,flow_interaction)
#undef TABLE
#define pgrazor_unity_scheduler_api flow_scheduler_api
#define pgrazor_runtime_transport_api flow_transport_api
#define pgrazor_runtime_bootstrap_api flow_boot_api
#define pgrazor_runtime_metadata_api flow_metadata_api
#define pgrazor_catch_wait_api flow_wait_api
#define pgrazor_shared_host_api flow_shared_api
#define pgrazor_quickball_host_api flow_quick_api
#define pgrazor_routes_host_api flow_routes_api
#define pgrazor_inventory_host_api flow_inventory_api
#define pgrazor_spin_host_api flow_spin_api
#define pgrazor_spawn_host_api flow_spawn_api
#define pgrazor_camera_host_api flow_camera_api
#define pgrazor_combat_host_api flow_combat_api
#define pgrazor_account_host_api flow_account_api
#define pgrazor_encounter_host_api flow_encounter_api
#define pgrazor_interaction_host_api flow_interaction_api
#define pgrazor_runtime_host_api flow_manager_api
#include "runtime_host.cpp"

// Collector is another gameplay boundary spy. The real transport still supplies
// its OFF gate; this orchestration fixture must never start box interactions.
static unsigned flow_collector_ticks=0,flow_collector_locations=0;
static std::atomic<unsigned> flow_collector_stops{0};
void pgr_event_collector_location(double lat,double lng,uint64_t at){
 assert((std::isfinite(lat)&&std::isfinite(lng)&&at)||(!std::isfinite(lat)&&!std::isfinite(lng)&&!at));
 ++flow_collector_locations;
}
int pgr_event_collector_tick(bool enabled,uint64_t tid){
 assert(!enabled&&tid==uint64_t(gettid()));++flow_collector_ticks;return 0;
}
void pgr_event_collector_stop(){++flow_collector_stops;}
PgrEventCollectorState pgr_event_collector_state(){return {};}

namespace test70 {
const PgrRuntimeHostApi *manager=nullptr;
const PgrCatchWaitApi *real_wait=nullptr;
const pgrazor_native_api_v2 *core=nullptr;
uint64_t owner_tid=0;
struct Spy {unsigned configs=0,ticks=0;std::atomic<unsigned> stops{0};std::atomic<bool> stopped{false};bool ready=false;unsigned owned=1;} spy[72];
std::vector<unsigned> order;
std::vector<unsigned> stop_order;
unsigned encounter_installs=0,interaction_schedules=0,interaction_sites=0;
uint32_t forwarding_mask=0;
PgrSharedMetadataOutput shared_metadata{};
universe::FClass execute_class{};
universe::FType execute_type{"UnityEngine.UnitySynchronizationContext",&execute_class};
universe::FMethod execute_method{};
void *invoke_code=nullptr,*convert_code=nullptr,*current_code=nullptr;
bool invoke_wait=false,throw_original=false,reenter_tick=false,combat_failed=false,stop_inside=false;
unsigned originals=0;int handle_token=0;
PgrRuntimeHostSample snapshot(){PgrRuntimeHostSample s{};assert(manager->sample(&s)==0);return s;}
void run_original();
void pulse_spy(unsigned id,const PgrUnityPulse *p){
 assert(p&&uint64_t(p->tid)==owner_tid&&uint64_t(gettid())==owner_tid&&flow_scheduler.valid(p->generation)==1);
 // A child never runs until the real original has recorded its completion.
 assert(!order.empty()&&order.front()==0);order.push_back(id);const unsigned slot=id==640?64:id;++spy[slot].ticks;
 if(spy[slot].stopped.load())spy[slot].owned=0;
 if(id==61&&reenter_tick){reenter_tick=false;run_original();}
 if(id==62&&stop_inside){stop_inside=false;std::thread other([]{assert(manager->stop()==0);});other.join();}
}
int stop_spy(unsigned id){stop_order.push_back(id);++spy[id].stops;spy[id].stopped=true;
 // Recursion from a child's cleanup is harmless and cannot reenter managed work.
 if(id==61)assert(manager->stop()==0);return 0;}
int config_spy(unsigned id){assert(uint64_t(gettid())==owner_tid);const unsigned n=++spy[id].configs;
 if(id==60)return -77; // terminal configure failure must never be retried
 if(id==61&&n==1)return 1;
 if(id==68&&n==1){spy[id].ready=true;return 1;} // active Accounts + pending AS
 if(id==68&&n==2)return PGR_ACH_ERR_BUSY;
 spy[id].ready=true;return 0;
}
__attribute__((noinline)) void *native_invoke(const void *method,void*,void**,void **exc){
 assert(method==&execute_method);if(exc)*exc=nullptr;++originals;order.push_back(0);
 if(throw_original)throw 99;
 if(invoke_wait){universe::cw::reset_objects();universe::cw::invoke();}
 return nullptr;
}
__attribute__((noinline)) void *native_convert(const void *m,void *o,void **p,int32_t n,void **e){assert(n==0);return native_invoke(m,o,p,e);}
void run_original(){using Invoke=void *(*)(const void*,void*,void**,void**);auto fn=reinterpret_cast<Invoke>(invoke_code);void *exc=nullptr;assert(!fn(&execute_method,nullptr,nullptr,&exc)&&!exc);}
void *make_thunk(size_t offset,void *target){auto p=universe::cw::module+offset;
#if defined(__x86_64__)
 memset(p,0x90,32);p[32]=0x48;p[33]=0xb8;memcpy(p+34,&target,8);p[42]=0xff;p[43]=0xe0;
#elif defined(__aarch64__)
 for(unsigned i=0;i<8;++i){uint32_t nop=0xd503201f;memcpy(p+4*i,&nop,4);}uint32_t load=0x58000050,branch=0xd61f0200;memcpy(p+32,&load,4);memcpy(p+36,&branch,4);memcpy(p+40,&target,8);
#else
#error Unsupported fixture machine-code architecture
#endif
 __builtin___clear_cache(reinterpret_cast<char*>(p),reinterpret_cast<char*>(p+64));return p;}
void unused(){std::abort();}
void *loader(void*,const char *name,int flags){assert(!strcmp(name,"libil2cpp.so")&&(flags&RTLD_NOLOAD));return &handle_token;}
int module_lookup(void*,const char*,const void **base,uint64_t *size){*base=universe::cw::module;*size=universe::cw::module_size;return 0;}
const void *metadata_lookup(void*,uint32_t version){return core->query_feature(47,version);}
void *symbols(void*,void *handle,const char *name){assert(handle==&handle_token);
 static const char *meta_names[]={"domain_get","domain_get_assemblies","assembly_get_image","class_from_name","class_get_type","type_get_name","free","class_get_field_from_name","field_get_parent","field_get_flags","field_get_offset","field_get_type","class_from_type","class_is_valuetype","class_is_enum","class_enum_basetype","class_value_size","class_get_method_from_name","method_get_class","method_get_name","method_get_flags","method_get_param_count","method_get_param","method_get_return_type","method_is_generic","method_is_inflated","debug_get_method_info","class_is_assignable_from","class_get_methods","class_get_nested_types","class_get_declaring_type"};
 static const char *runtime_names[]={"runtime_invoke","runtime_invoke_convert_args","thread_attach","thread_detach","thread_current","gchandle_new","gchandle_new_weakref","gchandle_get_target","gchandle_free","field_get_value","field_set_value","field_static_get_value","field_static_set_value","object_new","object_unbox","object_get_class","value_box","string_new","string_chars","array_new"};
 assert(!strncmp(name,"il2cpp_",7));const char *suffix=name+7;void *value=nullptr;
 for(unsigned i=0;i<31;++i)if(!strcmp(suffix,meta_names[i])){memcpy(&value,reinterpret_cast<const char*>(&universe::cw::binding.meta)+8*i,8);return value;}
 for(unsigned i=0;i<20;++i)if(!strcmp(suffix,runtime_names[i])){memcpy(&value,reinterpret_cast<const char*>(&universe::cw::binding.runtime)+8*i,8);return value?value:reinterpret_cast<void*>(unused);}
 return nullptr;
}
int install_wait(const PgrCatchWaitConfig *p){assert(!p);return real_wait->install(&universe::cw::config);}
void setup_universe(){
 universe::cw::api=real_wait;universe::cw::setup();
 execute_class.type=&execute_type;execute_method.name="ExecuteTasks";execute_method.klass=&execute_class;execute_method.flags=0x10;execute_method.ret=&universe::cw::void_type;
 universe::g_classes.push_back({universe::IMGA,"UnityEngine","UnitySynchronizationContext",&execute_class});universe::g_methods.push_back(&execute_method);
 invoke_code=make_thunk(0x1000,reinterpret_cast<void*>(native_invoke));convert_code=make_thunk(0x1100,reinterpret_cast<void*>(native_convert));current_code=make_thunk(0x1200,reinterpret_cast<void*>(universe::cw::current));execute_method.code=invoke_code;
 assert(mprotect(universe::cw::module+0x1000,4096,PROT_READ|PROT_EXEC)==0);
 universe::cw::binding.runtime.runtime_invoke=invoke_code;universe::cw::binding.runtime.runtime_invoke_convert_args=convert_code;universe::cw::binding.runtime.thread_current=current_code;
 PgrBootConfig c{};c.version=1;c.size=sizeof(c);c.dlopen_fn=loader;c.dlsym_fn=symbols;c.module_fn=module_lookup;c.metadata_fn=metadata_lookup;c.seg_query=universe::cw::segment;c.reserved=1;
 PgrBootBinding b{};assert(flow_boot.probe(&c,&b)==0);assert(b.execute_tasks==&execute_method&&b.runtime.runtime_invoke==invoke_code&&!(b.flags&1));
}
void setup_spies(){
 shared_metadata.version=2;shared_metadata.size=sizeof(shared_metadata);
 flow_shared.configure=[](const PgrSharedHostConfig *p){assert(!p);return config_spy(61);};flow_shared.stop=[](){return stop_spy(61);};flow_shared.metadata=[]()->const PgrSharedMetadataOutput*{return spy[61].ready?&shared_metadata:nullptr;};flow_shared.on_unity_pulse=[](void*,const PgrUnityPulse*p){pulse_spy(61,p);};flow_shared.sample=[](PgrSharedHostSample*s){*s={};s->ready=spy[61].ready;s->stopping=spy[61].stopped;s->roots=spy[61].owned;s->capabilities=127;return 0;};
 flow_quick.configure=[](const PgrQuickballHostConfig*p){assert(!p);return config_spy(62);};flow_quick.stop=[](){return stop_spy(62);};flow_quick.on_unity_pulse=[](void*,const PgrUnityPulse*p){pulse_spy(62,p);};flow_quick.sample=[](PgrQuickballHostSample*s){*s={};s->ready=spy[62].ready;s->stopping=spy[62].stopped;s->owned=spy[62].owned;s->capabilities=127;return 0;};
 flow_routes.configure=[](const PgrRoutesHostConfig*c){assert(c&&c->flags==15);return config_spy(60);};flow_routes.stop=[](uint64_t){return stop_spy(60);};flow_routes.on_unity_pulse=[](void*,const PgrUnityPulse*p){pulse_spy(60,p);};
 flow_inventory.configure=[](const PgrInventoryHostConfig*p){assert(!p);return config_spy(65);};flow_inventory.stop=[](){return stop_spy(65);};flow_inventory.on_unity_pulse=[](void*,const PgrUnityPulse*p){pulse_spy(65,p);};flow_inventory.sample=[](PgrInventoryHostSample*s){*s={};s->ready=spy[65].ready;s->stopping=spy[65].stopped;s->roots=spy[65].owned;s->capabilities=63;return 0;};
 flow_spin.configure=[](const PgrSpinHostConfig*c){assert(c&&c->metadata==&shared_metadata&&c->extra->gchandle_new==universe::cw::binding.runtime.gchandle_new&&c->exclusive_owner==1&&c->flags==1&&c->owner_tid==owner_tid);return config_spy(67);};flow_spin.stop=[](){return stop_spy(67);};flow_spin.on_unity_pulse=[](void*,const PgrUnityPulse*p){pulse_spy(67,p);};flow_spin.sample=[](PgrSpinHostSample*s){*s={};s->capabilities=spy[67].ready?3:0;s->stopping=spy[67].stopped;s->owned=spy[67].owned;return 0;};
 flow_spawn.configure=[](const PgrSpawnHostConfig*p){assert(!p);return config_spy(71);};flow_spawn.stop=[](){return stop_spy(71);};flow_spawn.on_unity_pulse=[](void*,const PgrUnityPulse*p){pulse_spy(71,p);};flow_spawn.sample=[](PgrSpawnHostSample*s){*s={};s->ready=spy[71].ready;s->installed=1;s->stopping=spy[71].stopped;return 0;};
 flow_camera.configure=[](const PgrCameraHostConfig*p){assert(!p);return config_spy(64);};flow_camera.start=[](const PgrCameraHostOptions*p)->int{assert(p&&p->enabled&&p->sample_camera&&p->continuous&&!p->sample_requested&&!p->lease_ms);return PGR_CH_RETRY;};flow_camera.stop=[](const char*){return stop_spy(64);};flow_camera.tick=[](uint64_t tid){assert(tid==owner_tid&&uint64_t(gettid())==tid);spy[64].owned=0;++spy[64].ticks;return 0;};flow_camera.on_unity_begin=[](void*,const PgrUnityPulse*p){pulse_spy(640,p);};flow_camera.on_unity_end=[](void*,const PgrUnityPulse*p){pulse_spy(64,p);};flow_camera.sample=[](PgrCameraHostSample*s){assert(s->version==1&&s->size==sizeof(*s));*s={};s->active=!spy[64].stopped;s->ready=spy[64].ready;s->stopping=spy[64].stopped;return 0;};
 flow_combat.configure=[](const PgrCombatHostConfig*p){assert(!p);return config_spy(66);};flow_combat.stop=[](uint64_t){return stop_spy(66);};flow_combat.on_unity_pulse=[](void*,const PgrUnityPulse*p){pulse_spy(66,p);};flow_combat.sample=[](PgrCombatHostSample*s){*s={};s->ready=spy[66].ready;s->stopping=spy[66].stopped;s->roots=spy[66].owned;s->capabilities=3;s->failed=combat_failed;s->last_error=combat_failed?-88:0;return 0;};
 flow_account.configure=[](const PgrAccountHostConfig*p){assert(!p);return config_spy(68);};flow_account.stop=[](){return stop_spy(68);};flow_account.on_unity_pulse=[](void*,const PgrUnityPulse*p){pulse_spy(68,p);};flow_account.sample=[](PgrAccountHostSample*s){*s={};s->configured=spy[68].ready;s->capabilities=spy[68].configs<3?1:3;s->stopping=spy[68].stopped;s->active_roots=spy[68].owned;return 0;};
 flow_encounter.configure=[](const PgrEncounterHostConfig*c){assert(!c&&spy[61].ready);return config_spy(63);};
 flow_encounter.install=[](){assert(spy[63].configs&&spy[61].ready);++encounter_installs;return 0;};
 flow_encounter.tick=[](uint64_t tid){assert(tid==owner_tid);auto s=snapshot();PgrUnityPulse p{s.generation,s.pulses,int32_t(tid),0};pulse_spy(63,&p);return 0;};
 flow_encounter.stop=[](uint64_t tid){assert(tid==owner_tid||tid==uint64_t(gettid()));return stop_spy(63);};
 flow_encounter.sample=[](PgrEncounterHostSample*s){assert(s->version==1&&s->size==64);*s={};s->version=1;s->size=64;s->installed=spy[63].ready;s->active_capabilities=127;return 0;};
 flow_interaction.configure=[](const PgrInteractionHostConfig*c){assert(!c);return config_spy(69);};
 flow_interaction.shared_sites=[](uint32_t reserved,uint32_t forwarded){assert(reserved==PGR_IH_SHARED_MASK_ALL);assert(!(forwarded&~reserved));assert((forwarded&forwarding_mask)==forwarding_mask);forwarding_mask=forwarded;++interaction_sites;return 0;};
 flow_interaction.schedule=[](int64_t ms){assert(ms==25000&&spy[69].configs);++interaction_schedules;return 0;};
 flow_interaction.on_unity_pulse=[](void*,const PgrUnityPulse*p){assert(interaction_schedules==1);pulse_spy(69,p);};
 flow_interaction.stop=[](const char*reason){assert(!strcmp(reason,"manager-stop"));return stop_spy(69);};
 flow_interaction.sample=[](PgrInteractionHostSample*s){*s={};s->configured=spy[69].ready;s->installed_target_mask=forwarding_mask;s->in_flight=spy[69].owned;s->status=spy[69].stopped?PGR_IH_STATUS_STOPPED:PGR_IH_STATUS_SCHEDULED;return 0;};
}
unsigned observed_events=0,shortened_events=0;bool state63=false,state69=false;
void exchange(bool allowed){static uint64_t seq=0;std::vector<uint8_t> frame(PGR_TRANSPORT_SNAPSHOT_FIXED+PGR_TRANSPORT_STRINGS*sizeof(uint32_t),0),out(65536);auto put=[&](size_t off,uint32_t v){memcpy(frame.data()+off,&v,4);};put(PGR_TRANSPORT_OFF_MAGIC,PGR_TRANSPORT_SNAPSHOT_MAGIC);put(PGR_TRANSPORT_OFF_VERSION,PGR_TRANSPORT_FRAME_VERSION);put(PGR_TRANSPORT_OFF_FRAME_SIZE,frame.size());++seq;memcpy(frame.data()+PGR_TRANSPORT_OFF_SEQ,&seq,8);frame[PGR_TRANSPORT_OFF_BOOLS+25]=allowed;uint32_t length=0;assert(flow_transport.exchange(frame.data(),frame.size(),out.data(),out.size(),&length)==0);
 auto read=[&](size_t off){uint32_t v;memcpy(&v,out.data()+off,4);return v;};size_t at=32;for(unsigned i=0;i<read(12);++i){unsigned op=read(at),ns=read(at+4),nt=read(at+8);at+=12+8*ns;std::vector<std::string> strings;for(unsigned n=0;n<nt;++n){uint32_t len=read(at);at+=4;strings.emplace_back(reinterpret_cast<char*>(out.data()+at),len);at+=len;}if(op==19&&strings.size()==2&&strings[0]=="native-runtime"){state63|=strings[1].find("\"feature\":63")!=std::string::npos;state69|=strings[1].find("\"feature\":69")!=std::string::npos;}if(op==19&&strings.size()==2&&strings[0]=="quick-catch-wait"){observed_events+=strings[1].find("wait-observed")!=std::string::npos;shortened_events+=strings[1].find("wait-shortened")!=std::string::npos;}}assert(at==length);
}
}

int main(int argc,char **argv){using namespace test70;assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);auto get=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(get);core=static_cast<const pgrazor_native_api_v2*>(get(2));assert(core&&core->query_feature(70,1));owner_tid=gettid();
#define LOAD(VAR,TYPE,ID,VER) VAR=*static_cast<const TYPE*>(core->query_feature(ID,VER));
 LOAD(flow_scheduler,PgrUnitySchedulerApi,48,2) LOAD(flow_transport,PgrRuntimeTransportApi,52,1) LOAD(flow_boot,PgrBootApi,54,1) LOAD(flow_metadata,PgrMetaApi,47,1)
#undef LOAD
 assert(flow_transport.sample&&flow_transport.get_bool&&flow_transport.get_double&&flow_transport.emit);
 real_wait=static_cast<const PgrCatchWaitApi*>(core->query_feature(58,1));assert(real_wait);flow_wait=*real_wait;flow_wait.install=install_wait;setup_spies();manager=static_cast<const PgrRuntimeHostApi*>(flow_manager_api(1));
 PgrRuntimeHostConfig config{1,sizeof(config),1,0};assert(manager->configure(&config)==1);setup_universe();assert(manager->configure(&config)==1);
 PgrRuntimeTransportConfig tc{1,sizeof(tc),1000,256,10000,65536,0,0};assert(flow_transport.install(&tc)==0);config.exclusive_owner=0;assert(manager->configure(&config)==-1);PgrUnitySchedulerSnapshot before{};flow_scheduler.snapshot(&before);assert(!before.installed);config.exclusive_owner=1;
 assert(manager->configure(&config)==1);exchange(true);const int configured=manager->configure(&config);fprintf(stderr,"manager70 configure after fresh52 rc=%d\n",configured);assert(configured==0&&manager->configure(&config)==0);auto initial=snapshot();assert(initial.installed&&initial.generation&&initial.owner_tid==owner_tid&&!initial.pulses);exchange(true);
 order.clear();run_original();auto first=snapshot();assert(flow_collector_ticks==1&&flow_collector_locations==1);assert(first.pulses==1&&first.hosts[1].state==PGR_RTH_WAITING&&first.hosts[0].state==PGR_RTH_FAILED&&first.hosts[0].error==-77);assert(spy[68].ticks==1&&spy[68].configs==1&&first.hosts[8].state!=PGR_RTH_FAILED);assert(first.pending_mask==0);assert(spy[63].configs==0&&encounter_installs==0);assert(interaction_schedules==1&&forwarding_mask==PGR_IH_SHARED_MASK_49);assert(spy[65].configs==0&&spy[67].configs==0);
 const std::vector<unsigned> first_order={0,62,71,640,64,66,68,69};assert(order==first_order);
 // Real58 shortening and event52 serialization happen after real native policy.
 invoke_wait=true;order.clear();run_original();assert(std::fabs(universe::cw::duration()-0.1f)<1e-6);exchange(false);assert(observed_events==1&&shortened_events==1);invoke_wait=false;order.clear();run_original();invoke_wait=true;order.clear();run_original();assert(universe::cw::duration()==5.0f);invoke_wait=false;
 // Retry backoff does not suppress an already usable68 family or rehook failures.
 usleep(2050000);exchange(true);order.clear();run_original();assert(spy[61].configs==2&&spy[65].configs==1&&spy[67].configs==1&&spy[68].configs==2&&spy[68].ticks>=5&&spy[60].configs==1);assert(snapshot().hosts[8].state!=PGR_RTH_FAILED);
 usleep(2050000);exchange(true);order.clear();run_original();assert(spy[68].configs==3&&snapshot().hosts[8].capabilities==3);
 const auto settled=snapshot();assert(state63&&state69);assert(spy[63].configs==1&&encounter_installs==1&&spy[69].configs==1&&interaction_schedules==1&&interaction_sites>1&&forwarding_mask==PGR_IH_SHARED_MASK_ALL);assert((order==std::vector<unsigned>{0,61,62,63,65,67,71,640,64,66,68,69}));assert(settled.hosts[1].state==PGR_RTH_READY&&settled.hosts[4].state==PGR_RTH_READY&&settled.hosts[10].state==PGR_RTH_READY);
 // Foreign and detached originals remain callable, but never dispatch children.
 const auto count=settled.pulses;order.clear();std::thread foreign([]{run_original();PgrRuntimeHostConfig c{1,sizeof(c),1,0};assert(manager->configure(&c)==-2);});foreign.join();assert(snapshot().pulses==count);universe::cw::attached=false;order.clear();run_original();universe::cw::attached=true;assert(snapshot().pulses==count);
 // Original throw suppresses dispatch and restores48 nesting for the next call.
 throw_original=true;order.clear();bool caught=false;try{run_original();}catch(int e){caught=e==99;}throw_original=false;assert(caught&&snapshot().pulses==count);
 // Child-managed reentry calls the original, but not a second manager pulse.
 reenter_tick=true;order.clear();run_original();assert(snapshot().pulses==count+1);assert(originals>snapshot().pulses);
 combat_failed=true;order.clear();run_original();assert(snapshot().hosts[6].state==PGR_RTH_FAILED&&spy[66].configs==1&&spy[66].stops>0);order.clear();run_original();assert(spy[66].owned==0&&spy[62].stops==0);
 // Foreign stop joins from a running child without a manager lock/deadlock.
 stop_inside=true;stop_order.clear();order.clear();run_original();const std::vector<unsigned> stopping_order={61,62,63,65,67,71,64,66,68,69};assert(stop_order.size()>=stopping_order.size()&&std::equal(stopping_order.begin(),stopping_order.end(),stop_order.begin()));assert(snapshot().stopping&&flow_scheduler.valid(initial.generation)==1);order.clear();run_original();auto stopped=snapshot();for(unsigned id:{61u,62u,63u,64u,65u,66u,67u,68u,69u,71u})assert(spy[id].stopped&&spy[id].owned==0);assert(spy[60].ticks==0&&spy[60].configs==1);assert(universe::cw::roots.empty()&&universe::cw::makes==universe::cw::frees);assert(manager->stop()==0);assert(flow_collector_stops==1&&flow_collector_ticks==flow_collector_locations);
 puts("PASS70 unchanged manager + real54/47/48/Dobby/52/58: explicit install, post-original order, owner/exception/reentry gates, real wait policy/events, positive and partial68 BUSY retries, isolated terminal failure, foreign stop and owner cleanup; gameplay hosts are native spies");return 0;
}

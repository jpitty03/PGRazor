// Includes unchanged production63; actual18/42/52/etc come from the dlopened core.
// Synthetic IL2CPP universe is reused from the independently exercised berry fixture.
// This tests compiled orchestration and Dobby boundaries, not live game parity.
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <dlfcn.h>
#define pgrazor_encounter_host_api fixture_encounter_host_api
#include "encounter_host.cpp"
#undef pgrazor_encounter_host_api
#define PGR_BERRY_DIRECT_TEST 1
#define main berry_universe_unused_main
namespace berry_universe {
#include "encounter_berries_test.cpp"
}
#undef main
#define main map_universe_unused_main
namespace map_universe {
#include "map_poi_test.cpp"
}
#undef main
#define main vgp_universe_unused_main
namespace vgp_universe {
#include "vgp_controller_test.cpp"
}
#undef main
static const pgrazor_native_api_v2 *core;
extern "C" const void *pgrazor_encounter_berries_api(uint32_t v){return core?core->query_feature(18,v):nullptr;}
extern "C" const void *pgrazor_inventory_host_api(uint32_t v){return core?core->query_feature(65,v):nullptr;}
extern "C" const void *pgrazor_items_api(uint32_t v){return core?core->query_feature(14,v):nullptr;}
extern "C" const void *pgrazor_map_poi_api(uint32_t v){return core?core->query_feature(42,v):nullptr;}
extern "C" const void *pgrazor_runtime_bootstrap_api(uint32_t v){return core?core->query_feature(54,v):nullptr;}
extern "C" const void *pgrazor_runtime_metadata_api(uint32_t v){return core?core->query_feature(47,v):nullptr;}
extern "C" const void *pgrazor_runtime_transport_api(uint32_t v){return core?core->query_feature(52,v):nullptr;}
extern "C" const void *pgrazor_shared_host_api(uint32_t v){return core?core->query_feature(61,v):nullptr;}
extern "C" const void *pgrazor_shared_metadata_api(uint32_t v){return core?core->query_feature(56,v):nullptr;}
extern "C" const void *pgrazor_transfer_facts_api(uint32_t v){return core?core->query_feature(35,v):nullptr;}
const void *pgrazor_transfer_policy_api(uint32_t v){return core?core->query_feature(4,v):nullptr;}
extern "C" const void *pgrazor_vgp_actions_api(uint32_t v){return core?core->query_feature(26,v):nullptr;}
extern "C" const void *pgrazor_vgp_aux_api(uint32_t v){return core?core->query_feature(34,v):nullptr;}
extern "C" const void *pgrazor_vgp_candidates_api(uint32_t v){return core?core->query_feature(29,v):nullptr;}
extern "C" const void *pgrazor_vgp_controller_api(uint32_t v){return core?core->query_feature(33,v):nullptr;}
extern "C" const void *pgrazor_vgp_discovery_api(uint32_t v){return core?core->query_feature(31,v):nullptr;}
extern "C" const void *pgrazor_vgp_maintenance_api(uint32_t v){return core?core->query_feature(32,v):nullptr;}
extern "C" const void *pgrazor_vgp_marker_api(uint32_t v){return core?core->query_feature(28,v):nullptr;}
extern "C" const void *pgrazor_vgp_policy_api(uint32_t v){return core?core->query_feature(25,v):nullptr;}
extern "C" const void *pgrazor_vgp_results_api(uint32_t v){return core?core->query_feature(30,v):nullptr;}

using namespace berry_universe;
static int enter_calls,exit_calls,result_calls,error_calls,submit_calls,peek_calls,rewrite_calls;
static void *last_info,*last_ball;
static uint64_t last_first,last_second;
static int nameplate_calls,iv_begin_calls,iv_end_calls,last_weather;
static void *last_plate,*last_map_pokemon;
static void *refreshed_plate,*refreshed_pokemon;
static bool original_throws,foreign_stop;
static int(*registered_filter)(void*,uint64_t);
static Obj result_obj{&c_int};
static const PgrEncounterHostApi *host;
static int response_class,status_enum,status_field,result_method,error_method;
static void *attached(){return reinterpret_cast<void*>(1);}
static void *params(void *m,uint32_t){return m==&result_method?&response_class:nullptr;}
static void *ret_type(void *m){return m==&m_ball_type?&c_item:nullptr;}
static void *identity(void *p){return p;}
static void *fields(void *k,void **it){if(k!=&response_class||*it)return nullptr;*it=&status_field;return &status_field;}
static size_t offset(void*){return 16;}
static uint32_t flags(void*){return 0;}
static void *type(void*){return &status_enum;}
static bool enum_type(void *p){return p==&status_enum;}
static int value_size(void*,int *align){*align=4;return 4;}
static void field_read(void *o,void *f,void *out){if(f==&status_field)*static_cast<int32_t*>(out)=0;else field_get(o,f,out);}
#if defined(__aarch64__)
#define PAD asm volatile(".rept 20; nop; .endr" ::: "memory")
#else
#define PAD asm volatile(".rept 32; nop; .endr" ::: "memory")
#endif
__attribute__((noinline,aligned(16))) static void target_enter(void*,void *info){PAD;last_info=info;++enter_calls;}
__attribute__((noinline,aligned(16))) static void target_exit(void*,void *info){PAD;last_info=info;++exit_calls;}
__attribute__((noinline,aligned(16))) static void target_result(void*,void*,void *info){PAD;last_info=info;++result_calls;if(original_throws)throw std::runtime_error("original");}
__attribute__((noinline,aligned(16))) static void target_error(void*,void*,void *info){PAD;last_info=info;++error_calls;if(original_throws)throw std::runtime_error("original");}
__attribute__((noinline,aligned(16))) static void target_submit(void *ball,uint64_t a,uint64_t b,void *info){PAD;++submit_calls;last_ball=ball;last_first=a;last_second=b;last_info=info;if(foreign_stop){std::thread t([]{assert(host->stop(gettid())==PGR_EH_RETRY);});t.join();}if(original_throws)throw std::runtime_error("original");}
__attribute__((noinline,aligned(16))) static void *target_peek(void *ball,void *info){PAD;void *h=gc_new(ball,0);int held=registered_filter?registered_filter(gc_target(h),gettid()):0;gc_free(h);if(held)return nullptr;++peek_calls;last_info=info;return &result_obj;}
__attribute__((noinline,aligned(16))) static void target_nameplate(void *self,void *pokemon,int32_t weather,void *info){PAD;++nameplate_calls;last_plate=self;last_map_pokemon=pokemon;last_weather=weather;last_info=info;if(original_throws)throw std::runtime_error("nameplate original");}
extern "C" const void *pgrazor_catch_observers_api(uint32_t v){
 static BerryFilterApi f{2,sizeof(BerryFilterApi),+[](int(*cb)(void*,uint64_t),uint64_t owner)->int{assert(owner==uint64_t(gettid()));registered_filter=cb;return 0;},+[]()->int{return registered_filter?1:0;}};
 return v==2?&f:nullptr;
}
static int rewrite(void *ball,PgrThrowValue *v){++rewrite_calls;assert(g_live_roots>0);last_ball=ball;v->bullseye=1;v->radius=.75f;return 0;}
static uint32_t shared_mask;
static int shared(uint32_t mask){shared_mask|=mask;return 0;}
extern "C" const void *pgrazor_quickball_host_api(uint32_t version){
 static PgrQuickballHostApi api{};api.version=1;api.size=sizeof(api);api.shared_sites=&shared;api.rewrite_submit=&rewrite;api.sample=+[](PgrQuickballHostSample *o)->int{o->ready=1;o->stopping=0;return 0;};
 api.begin_catch=+[](void *plate,void *pokemon,uint64_t,void **refreshed)->uint64_t{assert(g_live_roots>=2);++iv_begin_calls;assert(refreshed);refreshed[0]=refreshed_plate?refreshed_plate:plate;refreshed[1]=refreshed_pokemon?refreshed_pokemon:pokemon;return 71;};
 api.end_catch=+[](uint64_t token,uint64_t)->int{assert(token==71&&g_live_roots>=2);++iv_end_calls;return 0;};return version==1?&api:nullptr;
}
static PgrSharedMetadataOutput md;
static PgrSharedMetaExtraExports extra;
static PgrMetaExports meta;
static PgrEncounterHostConfig config(){
 md.version=2;md.size=sizeof(md);md.optional[OPT_BERRIES]=PGR_SHARED_OPT_AVAILABLE;md.optional[OPT_ITEMS]=PGR_SHARED_OPT_AVAILABLE;
 md.classes[6]=&c_bool;md.methods[5]=&m_alive;
 md.opt_classes[OC_POKEBALL]=&c_pokeball;md.opt_classes[OC_BERRY]=&c_scope;md.opt_classes[OC_IBAG]=&c_bag;md.opt_classes[OC_INT]=&c_int;md.opt_classes[OC_ITEM]=&c_item;
 auto H=[](int id,void *v){md.opt_handles[id]=v;};
 H(H_BR_CALLBACK,&c_callback);H(H_BR_M_BALL_TYPE,&m_ball_type);H(H_BR_M_BREAD,&m_bread);H(H_BR_M_CAN,&m_can);H(H_BR_M_PHOTO,&m_photo);H(H_IT_QUANTITY_M,&m_quantity);H(H_BR_M_DIRECT,&m_direct);
 H(H_BR_F_BAG,&f_scope_bag);H(H_BR_F_UI,&f_scope_ui);md.fields[1]=&f_ui_state;H(H_BR_F_CB_SCOPE,&f_cb_scope);H(H_BR_F_CB_BERRY,&f_cb_berry);H(H_BR_M_RESULT,&result_method);H(H_BR_M_ERROR,&error_method);
 H(H_BR_M_ENTER_CODE,(void*)&target_enter);H(H_BR_M_EXIT_CODE,(void*)&target_exit);H(H_BR_M_RESULT_CODE,(void*)&target_result);H(H_BR_M_ERROR_CODE,(void*)&target_error);H(H_BR_M_SUBMIT_CODE,(void*)&target_submit);H(H_BR_M_PEEK_CODE,(void*)&target_peek);
 extra.gchandle_new=(void*)&gc_new;extra.gchandle_get_target=(void*)&gc_target;extra.gchandle_free=(void*)&gc_free;extra.class_get_fields=(void*)&fields;
 meta.method_get_param=(void*)&params;meta.method_get_return_type=(void*)&ret_type;meta.class_from_type=(void*)&identity;meta.field_get_offset=(void*)&offset;meta.field_get_flags=(void*)&flags;meta.field_get_type=(void*)&type;meta.class_is_enum=(void*)&enum_type;meta.class_value_size=(void*)&value_size;
 PgrEncounterHostConfig c{};c.version=1;c.size=sizeof(c);c.require_all=1;c.capability_mask=PGR_EH_CAP_BERRIES;c.resolved_metadata=&md;c.extra_exports=&extra;
 c.metadata_config.binding.exports=&meta;c.metadata_config.thread_current=(void*)&attached;
 c.runtime_invoke=(void*)&runtime_invoke;c.object_get_class=(void*)&obj_class;c.object_unbox=(void*)&object_unbox;c.object_new=(void*)&object_new;c.field_get_value=(void*)&field_read;c.field_set_value=(void*)&field_set_value;c.field_set_value_object=(void*)&field_set_value_object;
 return c;
}

static const PgrRuntimeTransportApi *wire;
static std::vector<uint8_t> wire_out(1100000);
static void put32(std::vector<uint8_t>& b,size_t at,uint32_t v){memcpy(b.data()+at,&v,4);}
static void exchange(const std::vector<uint8_t>& b={}){uint32_t n=0;assert(wire->exchange(b.empty()?nullptr:b.data(),b.size(),wire_out.data(),wire_out.size(),&n)==0);assert(n>=32);}
static void snapshot_wire(bool active,bool berries,bool map,const char *priority="701,705",const char *keep=""){
 static uint64_t seq;std::vector<uint8_t>b(180+strlen(priority)+strlen(keep));put32(b,0,PGR_TRANSPORT_SNAPSHOT_MAGIC);put32(b,4,1);put32(b,8,b.size());++seq;memcpy(b.data()+16,&seq,8);b[35]=active;b[39]=berries;b[53]=map;b[37]=1;put32(b,104,63);size_t at=152;
 for(unsigned i=0;i<7;++i){const char *v=i==2?priority:i==4?keep:"";uint32_t n=strlen(v);put32(b,at,n);at+=4;memcpy(b.data()+at,v,n);at+=n;}exchange(b);
}
static void install_wire(){wire=static_cast<const PgrRuntimeTransportApi*>(core->query_feature(52,1));assert(wire);PgrRuntimeTransportConfig c{1,sizeof(c),1000,256,1048576,1048576,0,0};assert(wire->install(&c)==0);}
static std::string map_snapshot;
static int map_case(){
 namespace u=map_universe;
 auto c=config();auto m=u::make_config(false,false,false,false);md={};md.version=2;md.size=sizeof(md);md.optional[OPT_MAP_POI]=md.optional[OPT_VGP]=PGR_SHARED_OPT_AVAILABLE;
 md.classes[6]=m.class_bool;md.methods[5]=m.m_alive;md.opt_classes[OC_VGP_STOP]=m.class_stop;md.opt_classes[OC_VGP_FORT]=m.class_fort;md.opt_classes[OC_STRING]=m.class_string;
 auto H=[](int id,void *v){md.opt_handles[id]=v;};H(H_MP_GYM,m.class_gym);H(H_MP_RAID,m.class_raid);H(H_VGP_M_FIND_ALL,m.m_find);H(H_VGP_M_FORT,m.m_fort);H(H_VGP_F_FORT_ID,m.f_fortId);H(H_VGP_F_FORT_LAT,m.f_lat);H(H_VGP_F_FORT_LON,m.f_lon);H(H_MP_INFO,m.f_raidInfo);H(H_MP_SPAWN,m.f_spawn);H(H_MP_END,m.f_end);H(H_MP_BATTLE,m.f_battle);H(H_MP_HIDDEN,m.f_hidden);
 c.capability_mask=PGR_EH_CAP_MAP_POI;c.runtime_invoke=m.runtime_invoke;c.object_get_class=m.object_get_class;c.field_get_value=m.field_get_value;c.object_unbox=m.object_unbox;c.object_get_virtual_method=m.object_get_virtual_method;c.string_length=m.string_length;c.string_chars=m.string_chars;c.metadata_config.thread_current=m.thread_current;
 extra.gchandle_new=m.gchandle_new;extra.gchandle_get_target=m.gchandle_get_target;extra.gchandle_free=m.gchandle_free;extra.type_get_object=m.type_get_object;extra.array_length=m.array_length;meta.class_get_type=m.class_get_type;
 c.set_map_poi_snapshot=+[](const char *json){map_snapshot=json;};u::reset();
 assert(host->configure(&c)==0);assert(host->install()==0);install_wire();snapshot_wire(true,false,true);
 u::FakeString id;u::set_id(id,"quote\"-stop");u::FakeFort fort{m.class_fort,&id,12.5,-42.25,nullptr,nullptr};u::FakeCandidate candidate{m.class_stop,1,&fort,0};u::FakeArray array{&u::cls_objarray,0,0,1,{&candidate}};u::g_stopArray=&array;
 assert(host->tick(gettid())==0);assert(map_snapshot.find("\"points\":[")!=std::string::npos);assert(map_snapshot.find("quote\\\"-stop")!=std::string::npos);assert(map_snapshot.find("12.500000")!=std::string::npos);assert(u::g_handles.empty());
 auto count=g_sample.map_poi_scan_serial;snapshot_wire(false,false,false);assert(host->tick(gettid())==0);assert(g_sample.map_poi_scan_serial==count);exchange();assert(host->stop(gettid())==0);assert(u::g_handles.empty());std::puts("encounter_host: actual42 metadata config/scan/escaped JSON + actual52 ON/OFF PASS");return 0;
}

static int vgp_case(){
 namespace u=vgp_universe;
 // Native test-only dependency injection: actual33+25, modeled other siblings.
 // This verifies63 tick packing/events/revoke, not full63 metadata-to-leaf configuration.
 auto pc=PgrVgpPolicyConfig{};pc.version=1;pc.size=sizeof(pc);pc.scan_interval_ms=2500;pc.catch_seen_ms=30000;pc.spin_seen_ms=300000;pc.reject_delay_ms=30000;pc.terminal_delay_ms=3600000;pc.retry_delay_ms=5000;pc.default_delay_ms=30000;pc.max_keys=2048;
 for(int i=0;i<4;++i)pc.status_terminal[i]=pc.status_retry[i]=-1;pc.status_terminal[0]=100;
 auto policy=static_cast<const PgrVgpPolicyApi*>(core->query_feature(25,1));assert(policy&&policy->install(&pc)==0);
 u::api=static_cast<const PgrVgpControllerApi*>(core->query_feature(33,1));u::tid=gettid();auto vc=u::makeConfig();vc.policy_table=const_cast<PgrVgpPolicyApi*>(policy);assert(u::api->install(&vc)==0);u::resetScenario();
 auto c=config();c.extra_exports=&extra;extra.gchandle_new=vc.gc_new;extra.gchandle_get_target=vc.gc_get;extra.gchandle_free=vc.gc_free;c.runtime_invoke=vc.runtime_invoke;c.object_get_class=vc.object_class;c.object_unbox=vc.object_unbox;
 assert(host->configure(&c)==0);g_vgc=u::api;g_active_caps=PGR_EH_CAP_VGP;g_installed=true;install_wire();
 PgrEncounterHostOptions o{1,sizeof(o)};o.vgp_allowed=1;o.vgp_enabled=1;o.vgp_catch_allowed=1;o.vgp_spin_allowed=1;o.lease_remaining_ms=2500;o.lat=10;o.lon=20;
 assert(host->update_options(&o,gettid())==0);assert(host->tick(gettid())==0);assert(g_last_vgp_status==PGR_VGC_ST_CATCHING);
 u::g_pollRc=PGR_VR_DONE;assert(host->tick(gettid())==0);assert(g_last_vgp_status==PGR_VGC_ST_CATCHING);
 u::g_success=true;u::g_statusKnown=true;u::g_catchStatus=100;assert(host->tick(gettid())==0);assert(g_last_vgp_status==PGR_VGC_ST_CAUGHT);assert(u::roots.empty());
 // OFF must revoke, not permanently stop33. ON remains capable of a fresh scan.
 o.vgp_enabled=0;assert(host->update_options(&o,gettid())==0);assert(host->tick(gettid())==0);
 o.vgp_enabled=1;assert(host->update_options(&o,gettid())==0);assert(host->tick(gettid())==0);assert(g_last_vgp_status!=PGR_VGC_ST_OFF);
 // Restrict catch while encounter is pending: generation must revoke immediately.
 auto generation=u::api->generation();o.vgp_catch_allowed=0;
 assert(host->update_options(&o,gettid())==0);assert(u::api->generation()>generation);
 // Native52 polling path must enforce the same restrictive generation boundary.
 snapshot_wire(true,false,false);
 // Manual branch above tests pending cancellation; polling branch is driven directly by real52 snapshot.
 auto make_vgp_frame=[&](int mask){static uint64_t seq=10;std::vector<uint8_t> f(180);put32(f,0,PGR_TRANSPORT_SNAPSHOT_MAGIC);put32(f,4,1);put32(f,8,f.size());++seq;memcpy(f.data()+16,&seq,8);f[35]=1;f[68]=1;f[69]=1;put32(f,108,mask);exchange(f);};
 make_vgp_frame(3);assert(host->tick(gettid())==0);generation=u::api->generation();
 make_vgp_frame(2);assert(host->tick(gettid())==0);assert(u::api->generation()>generation);
 exchange();
 // Broker event boundary:61's independent fixture proves routing;63 owns typed UI publication.
 static int broker_events=2;
 static TransferBroker broker{2,sizeof(TransferBroker),+[]()->int{return 1;},nullptr,+[](PgrTransferExecEvent *e)->int{
   if(!broker_events)return 0;*e={};e->kind=PGR_TX_EVENT_COMPLETE;e->token=2-broker_events;e->aux=broker_events==2;--broker_events;return 1;}};
 g_broker=&broker;g_active_caps|=PGR_EH_CAP_TRANSFER;pump_transfer_events(gettid(),0);exchange();
 std::string encoded(reinterpret_cast<const char*>(wire_out.data()),wire_out.size());
 assert(encoded.find("Transferred newly caught Pokemon")!=std::string::npos&&encoded.find("transferred")!=std::string::npos&&encoded.find("Transfer Error: server-result")!=std::string::npos&&encoded.find("transfer-error")!=std::string::npos);
 assert(host->stop(gettid())==0);assert(u::roots.empty());std::puts("encounter_host: real25+33 catch/result/status/OFF-ON + typed52; modeled other VGP leaves PASS");return 0;
}
static decltype(&target_enter) volatile call_enter=&target_enter;
static decltype(&target_exit) volatile call_exit=&target_exit;
static decltype(&target_submit) volatile call_submit=&target_submit;
static decltype(&target_peek) volatile call_peek=&target_peek;
static decltype(&target_result) volatile call_result=&target_result;
static decltype(&target_error) volatile call_error=&target_error;
static int nameplate_case(){
 auto c=config();assert(host->configure(&c)==0);g_installed=true;
 assert(install_one_hook((void*)&target_nameplate,(void*)&hook_nameplate_set_pokemon_ui,(void**)&g_orig_nameplate_ui)==0);
 decltype(&target_nameplate) volatile call=&target_nameplate;
 Obj plate{&c_scope},pokemon{&c_pokeball};void *info=reinterpret_cast<void*>(0x76543210);
 call(&plate,&pokemon,6,info);
 assert(nameplate_calls==1&&last_plate==&plate&&last_map_pokemon==&pokemon&&last_weather==6&&last_info==info);
 assert(iv_begin_calls==1&&iv_end_calls==1&&g_live_roots==0);
 Obj moved_plate{&c_scope},moved_pokemon{&c_pokeball};refreshed_plate=&moved_plate;refreshed_pokemon=&moved_pokemon;
 original_throws=true;try{call(&plate,&pokemon,-7,info);assert(false);}catch(const std::runtime_error&){}original_throws=false;
 assert(nameplate_calls==2&&last_plate==&moved_plate&&last_map_pokemon==&moved_pokemon&&last_weather==-7&&last_info==info&&iv_begin_calls==2&&iv_end_calls==2&&g_live_roots==0);
 g_stopped=true;call(&plate,&pokemon,3,info);
 assert(nameplate_calls==3&&last_plate==&plate&&last_map_pokemon==&pokemon&&last_weather==3&&last_info==info&&iv_begin_calls==2&&iv_end_calls==2&&g_live_roots==0);
 std::puts("encounter_host: real Dobby Nameplate two-argument refresh + weather/MethodInfo forwarding + IV unwind + stopped passthrough PASS");return 0;
}
int main(int argc,char **argv){
 assert(argc>=2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
 auto get=reinterpret_cast<const pgrazor_native_api_v2*(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(get);core=get(2);assert(core&&core->query_feature(63,1));
 host=static_cast<const PgrEncounterHostApi*>(fixture_encounter_host_api(1));g_api=static_cast<const pgr_berry_api_v1*>(core->query_feature(18,1));assert(g_api);
 pid_t child=fork();assert(child>=0);if(child==0)_exit(map_case());int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 child=fork();assert(child>=0);if(child==0)_exit(vgp_case());assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 child=fork();assert(child>=0);if(child==0)_exit(nameplate_case());assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 auto c=config();assert(host->configure(&c)==0);assert(host->install()==0);assert(shared_mask==PGR_QBH_SUBMIT);
 const auto tid=static_cast<uint64_t>(gettid());void *info=reinterpret_cast<void*>(0x1234);
 Obj bag{&c_bag},ball{&c_pokeball};Ui ui{nullptr,2};Scope scope{&c_scope,&bag,&ui};
 PgrEncounterHostOptions o{};o.version=1;o.size=sizeof(o);o.berries_allowed=1;o.berry_priority_count=1;o.berry_priority_ids[0]=701;o.lease_remaining_ms=2500;
 assert(host->update_options(&o,tid)==0);g_stock[701]=2;g_sync_result=true;
 call_enter(&scope,info);assert(enter_calls==1&&last_info==info);
 uint64_t a=1,b=0;float radius=.8f;uint32_t bits;memcpy(&bits,&radius,4);a|=uint64_t(bits)<<32;
 call_submit(&ball,a,b,info);assert(submit_calls==0);assert(call_peek(&ball,info)==nullptr);
 assert(host->tick(tid)==0);assert(host->tick(tid)==0);
 auto berry_state=berry_universe::snap();std::fprintf(stderr,"63 berry direct=%d submit=%d rewrite=%d info=%p/%p ball=%p/%p roots=%d owned=%u failed=%u busy=%u\n",g_direct_calls,submit_calls,rewrite_calls,last_info,info,last_ball,&ball,g_live_roots,berry_state.owned_roots,berry_state.failed,berry_state.overlay_busy);
 assert(g_direct_calls==1&&submit_calls==1&&rewrite_calls==1&&last_info==info&&last_ball==&ball);assert((last_second&255)==1);
 assert(call_peek(&ball,info)==&result_obj);
 original_throws=true;try{call_result(&g_cb_pool,&result_obj,info);assert(false);}catch(const std::runtime_error&){}original_throws=false;
 assert(result_calls==1);
 install_wire();snapshot_wire(true,true,false,"705,701");assert(host->tick(tid)==0);assert(g_opts.berry_priority_count==2&&g_opts.berry_priority_ids[0]==705);
 snapshot_wire(true,true,false,"705,701","025");assert(host->tick(tid)==0);assert(!g_opts.auto_transfer_allowed);
 snapshot_wire(false,false,false);assert(host->tick(tid)==0);assert(!g_opts.berries_allowed);exchange();
 call_exit(&scope,info);assert(exit_calls==1);
 // A callback joining a foreign stop must finish rather than block on host ownership.
 o.berries_allowed=0;assert(host->update_options(&o,tid)==0);foreign_stop=true;call_submit(&ball,a,b,info);foreign_stop=false;
 assert(host->tick(tid)==0);assert(g_live_roots==0);
 int before=submit_calls;call_submit(&ball,a,b,info);assert(submit_calls==before+1&&g_live_roots==0);
 std::puts("encounter_host: actual18 + Dobby submit/hold/resume/peek/unwind/foreign-stop PASS");return 0;
}

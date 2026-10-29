// Fixture linkage: compile the UNCHANGED production host below, forward its leaf
// getters to the actual dlopened production core, substitute only feature55's
// metadata OUTPUT. ARM64 fingerprints cannot be executed as x86_64 hook stubs.
// Separate quickball_metadata_test verifies actual55 reflection/fingerprints.
// This fixture proves host wiring/leases/shared-site callbacks/telemetry and real
// Dobby+leaf behavior; it does not prove live metadata resolution or ART/Houdini.
#include "quickball_host.cpp"
#include "pgrazor_native_api.h"
#include <cassert>
#include <thread>
#include <vector>

namespace fixture {
const pgrazor_native_api_v2 *core=nullptr;
int class_tokens[kClassCount]{},field_tokens[kFieldCount]{},method_tokens[kMethodCount]{};
PgrQuickballMetaOutput metadata{};
struct Object {void *klass;alignas(8) unsigned char data[64];};
Object ball{},animator{},response{},float_box{},bool_box{},wild{},ar_values{},latlng_box{},map_object{},ground_object{};
struct Root {void *object;bool live;};Root root_pool[512]{};unsigned root_next=0,root_count=0;
void *learned_caller=nullptr,*last_ar=nullptr;PgrThrowValue last_hit{};int btxu_calls=0,caught_calls=0,hit_calls=0,ar_calls=0;
bool attached_flag=true,stop_inside=false;const PgrQuickballHostApi *host=nullptr;
void *thread_current(){return attached_flag?&class_tokens[0]:nullptr;}
void *object_class(void *o){return o?static_cast<Object*>(o)->klass:nullptr;}
bool assignable(void *a,void *b){return a==b;}
void *new_root(void *object,int){assert(object&&root_next<512);auto &r=root_pool[root_next++];r={object,true};++root_count;return &r;}
void *target_root(void *handle){auto *r=static_cast<Root*>(handle);assert(r&&r->live);return r->object;}
void free_root(void *handle){auto *r=static_cast<Root*>(handle);assert(r&&r->live&&root_count);r->live=false;--root_count;}
void *unbox(void *o){return static_cast<Object*>(o)->data;}
void field_get(void *o,void *f,void *out){
 if(f==&field_tokens[0]){assert(o==&ball);*static_cast<void**>(out)=&animator;}
 else if(f==&field_tokens[1]){assert(o==&ball);*static_cast<uint8_t*>(out)=ball.data[0];}
 else if(f==&field_tokens[2]){assert(o==&response);std::memcpy(out,response.data,4);}
 else assert(false&&"unexpected field read");
}
void field_set(void *o,void *f,void *value){assert(o==&ar_values);
 if(f==&field_tokens[37])std::memcpy(ar_values.data,value,4);
 else if(f==&field_tokens[38])std::memcpy(ar_values.data+4,value,4);
 else if(f==&field_tokens[39])ar_values.data[8]=*static_cast<uint8_t*>(value);
 else assert(false);
}
void *allocate_object(void *k){assert(k==&class_tokens[C_ARVALUES]);ar_values={};ar_values.klass=k;ar_values.data[8]=1;return &ar_values;}
void *invoke(void *m,void *self,void **args,void **exc){*exc=nullptr;
 if(m==&method_tokens[2]){assert(self==&animator);std::memcpy(float_box.data,animator.data,4);return &float_box;}
 if(m==&method_tokens[3]){assert(self==&animator);std::memcpy(animator.data,args[0],4);return nullptr;}
 if(m==&method_tokens[4]){assert(!self&&args&&args[0]);bool_box.data[0]=1;return &bool_box;}
 if(m==&method_tokens[25]){assert(self==&map_object&&args[0]);return &latlng_box;}
 assert(false&&"unexpected managed invocation");return nullptr;
}
void *string_new(const char*){return nullptr;}int string_length(void*){return 0;}const uint16_t *string_chars(void*){return nullptr;}
void *type_object(void*){return nullptr;}
#define QPAD asm volatile("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory")
__attribute__((noinline)) void *btxu(void*,void*){QPAD;++btxu_calls;learned_caller=__builtin_return_address(0);if(stop_inside){stop_inside=false;host->stop();}return &response;}
decltype(&btxu) volatile call_btxu=&btxu;
__attribute__((noinline)) void *wrapped_btxu(){void *r=call_btxu(&ball,nullptr);asm volatile("":"+r"(r)::"memory");return r;}
__attribute__((noinline)) void caught(void*,void*,void*){QPAD;++caught_calls;}
__attribute__((noinline)) void escaped(void*,void*,void*){QPAD;caught_calls+=2;}
__attribute__((noinline)) void hit_stub(void*,PgrThrowValue value,void*){QPAD;++hit_calls;last_hit=value;}
__attribute__((noinline)) void submit_stub(void*,PgrThrowValue,void*){QPAD;}
__attribute__((noinline)) void collision(void*,void*,void*){QPAD;}
__attribute__((noinline)) void trigger(void*,void*,void*){QPAD;asm volatile("nop");}
__attribute__((noinline)) void launch(void*){QPAD;}
__attribute__((noinline)) void release(void*){QPAD;asm volatile("nop");}
__attribute__((noinline)) PgrAlwaysHitValue make_throw(void*,uint8_t,void*){QPAD;return {1,0.5f,0,0,0,0};}
__attribute__((noinline)) void *capture(void*,PgrThrowValue,void *values,void*){QPAD;++ar_calls;last_ar=values;return &class_tokens[0];}
struct V2 {float x,y;};
__attribute__((noinline)) uint8_t position(void*,V2,float *out,void*){QPAD;out[0]=1;out[1]=2;out[2]=3;return 1;}
decltype(&position) volatile call_position=&position;
__attribute__((noinline)) void tap(void*,void*,void*,void*){QPAD;float out[3];assert(call_position(&map_object,{4,5},out,nullptr)==1);}
__attribute__((noinline)) void owned_update(void*,void*,void*){QPAD;}
__attribute__((noinline)) void setup(void*,void*,int32_t,void*){QPAD;}
int resolve(const PgrQuickballMetaConfig *c,PgrQuickballMetaOutput *out){assert(c&&c->owner_tid==uint64_t(gettid()));*out=metadata;return 0;}
const PgrQuickballMetaApi metadata_api{2,sizeof(PgrQuickballMetaApi),sizeof(PgrQuickballMetaConfig),sizeof(PgrQuickballMetaOutput),resolve};
void initialize(){
 metadata.version=2;metadata.size=sizeof(metadata);
 for(unsigned i=0;i<kClassCount;++i)metadata.classes[i]=&class_tokens[i];
 for(unsigned i=0;i<kFieldCount;++i)metadata.fields[i]=&field_tokens[i];
 for(unsigned i=0;i<kMethodCount;++i){metadata.methods[i]=&method_tokens[i];metadata.method_code[i]=&method_tokens[i];}
 for(auto &g:metadata.groups)g.available=1;
 metadata.alwayshit_current=&field_tokens[12];metadata.success=1;for(unsigned i=0;i<6;++i)metadata.outcomes_values[i]=int32_t(i);
 metadata.method_code[0]=reinterpret_cast<void*>(escaped);metadata.method_code[1]=reinterpret_cast<void*>(btxu);metadata.method_code[6]=reinterpret_cast<void*>(caught);
 metadata.method_code[14]=reinterpret_cast<void*>(submit_stub);metadata.method_code[15]=reinterpret_cast<void*>(hit_stub);
 metadata.method_code[17]=reinterpret_cast<void*>(collision);metadata.method_code[18]=reinterpret_cast<void*>(trigger);
 metadata.method_code[19]=reinterpret_cast<void*>(launch);metadata.method_code[20]=reinterpret_cast<void*>(release);metadata.method_code[21]=reinterpret_cast<void*>(make_throw);
 metadata.method_code[22]=reinterpret_cast<void*>(capture);metadata.method_code[23]=reinterpret_cast<void*>(tap);metadata.method_code[24]=reinterpret_cast<void*>(position);
 metadata.method_code[26]=reinterpret_cast<void*>(setup);metadata.method_code[41]=reinterpret_cast<void*>(owned_update);
 ball.klass=&class_tokens[C_BALL];animator.klass=&class_tokens[C_ANIMATOR];response.klass=&class_tokens[C_RESPONSE];
 float_box.klass=&class_tokens[C_SINGLE];bool_box.klass=&class_tokens[C_BOOL];wild.klass=&class_tokens[C_WILDMAP];
 map_object.klass=&class_tokens[C_MAPVIEW];ground_object.klass=&class_tokens[C_GROUND];latlng_box.klass=&class_tokens[C_LATLNG];
 float normal=1;std::memcpy(animator.data,&normal,4);int32_t status=1;std::memcpy(response.data,&status,4);
 double lat=12.25,lon=-42.5;std::memcpy(latlng_box.data,&lat,8);std::memcpy(latlng_box.data+8,&lon,8);
}
float current_speed(){float v;std::memcpy(&v,animator.data,4);return v;}
void sleep_ms(unsigned ms){timespec ts{long(ms/1000),long(ms%1000)*1000000};nanosleep(&ts,nullptr);}
}

// Only55's resolver is substituted. Every gameplay leaf below is the production
// implementation in core.so; Dobby patches the fixture's real NOP-padded methods.
const void *pgrazor_quickball_metadata_api(uint32_t v){return v==2?&fixture::metadata_api:nullptr;}
#define QFORWARD(NAME,ID) const void *NAME(uint32_t v){return fixture::core?fixture::core->query_feature(ID,v):nullptr;}
QFORWARD(pgrazor_catch_animation_api,8)
QFORWARD(pgrazor_catch_observers_api,49)
QFORWARD(pgrazor_throws_api,1)
QFORWARD(pgrazor_always_hit_api,7)
QFORWARD(pgrazor_ar_throw_api,5)
QFORWARD(pgrazor_map_tap_api,13)
QFORWARD(pgrazor_iv_display_api,15)
QFORWARD(pgrazor_runtime_transport_api,52)
QFORWARD(pgrazor_runtime_bootstrap_api,54)
QFORWARD(pgrazor_unity_scheduler_api,48)
#undef QFORWARD

int main(int argc,char **argv){
 using namespace fixture;
 assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib)std::puts(dlerror());assert(lib);
 auto get=reinterpret_cast<const void*(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(get);
 core=static_cast<const pgrazor_native_api_v2*>(get(2));assert(core&&core->query_feature(62,1)&&!core->query_feature(62,2));
 host=static_cast<const PgrQuickballHostApi*>(pgrazor_quickball_host_api(1));assert(host&&host->size==96);
 initialize();wrapped_btxu();assert(learned_caller);
 PgrMetaExports meta_exports{};meta_exports.class_get_type=reinterpret_cast<void*>(type_object);
 PgrQuickballExtra extra_exports{};PgrQuickballMetaConfig metadata_config{};metadata_config.version=2;metadata_config.size=sizeof(metadata_config);
 metadata_config.binding.exports=&meta_exports;metadata_config.binding.module_base=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(learned_caller)-kCallerRva);
 metadata_config.thread_current=reinterpret_cast<void*>(thread_current);metadata_config.extra=&extra_exports;
 PgrQuickballHostRuntime runtime{reinterpret_cast<void*>(invoke),reinterpret_cast<void*>(new_root),reinterpret_cast<void*>(target_root),reinterpret_cast<void*>(free_root),
  reinterpret_cast<void*>(object_class),reinterpret_cast<void*>(unbox),reinterpret_cast<void*>(field_get),reinterpret_cast<void*>(assignable),
  reinterpret_cast<void*>(allocate_object),reinterpret_cast<void*>(field_set),reinterpret_cast<void*>(string_new),reinterpret_cast<void*>(string_length),reinterpret_cast<void*>(string_chars),reinterpret_cast<void*>(type_object)};
 PgrQuickballHostConfig config{1,sizeof(config),0,0,&metadata_config,&runtime};
 auto bad=config;bad.flags=4;assert(host->configure(&bad)==PGR_QBH_BAD_CONFIG);
 assert(host->configure(&config)==0);assert(host->configure(&config)==PGR_QBH_RECONFIG);
 assert(host->tick(uint64_t(gettid())+1)==PGR_QBH_OFF_OWNER);
 PgrQuickballHostOptions settings{1,sizeof(settings),1,1,1,1,0,3,1,0,1,2,0,0,2000,0,17};
 assert(host->set_options(&settings)==0);assert(host->tick(gettid())==0);
 PgrQuickballHostSample gauge{};host->sample(&gauge);assert(gauge.metadata_ready&&gauge.capabilities==127&&!gauge.ready);
 assert(host->shared_sites(4)==PGR_QBH_BAD_CONFIG);assert(host->shared_sites(3)==0);
 assert(host->tick(gettid())==0);assert(host->tick(gettid())==0);host->sample(&gauge);assert(gauge.ready);
 // Real49 + real8: exact caller, roots, outcome and speed20; completion restores1.
 assert(wrapped_btxu()==&response);assert(current_speed()==20&&root_count>0);
 assert(host->tick(gettid())==0);decltype(&caught) volatile call_caught=&caught;call_caught(nullptr,&response,nullptr);
 assert(current_speed()==1&&caught_calls==1);assert(host->tick(gettid())==0);
 // Shared submit transform uses real1. The paired hit forwards that exact value.
 PgrThrowValue value{1,0.5f,0,0,0,0xa5};assert(host->rewrite_submit(&ball,&value)==0);
 assert(value.spinning==1&&value.radius>=0&&value.radius<0.3f&&value.pad==0xa5);
 Object encounter{&class_tokens[C_ENCOUNTER],{}};decltype(&hit_stub) volatile call_hit=&hit_stub;call_hit(&encounter,value,nullptr);
 assert(hit_calls==1&&!std::memcmp(&value,&last_hit,sizeof(value)));
 // Real5 native allocation/rooting/AR fields preserves the original return.
 decltype(&capture) volatile call_capture=&capture;assert(call_capture(&wild,value,&ball,nullptr)==&class_tokens[0]);
 assert(ar_calls==1&&last_ar==&ar_values&&ar_values.data[8]==0);
 // Real13 nested OnTap/GetGroundPosition -> runtime_invoke location -> host event.
 decltype(&tap) volatile call_tap=&tap;call_tap(&ground_object,nullptr,nullptr,nullptr);assert(host->tick(gettid())==0);
 bool saw_outcome=false,saw_map=false,saw_speed=false;PgrQuickballHostEvent event_out{};
 while(host->next_event(&event_out)==0){
  if(!std::strcmp(event_out.kind,"outcome-confirmed")){saw_outcome=true;assert(std::strstr(event_out.json,"caught"));}
  if(!std::strcmp(event_out.kind,"map-tap")){saw_map=true;assert(std::strstr(event_out.json,"12.250000")&&std::strstr(event_out.json,"\"epoch\":17"));}
  if(!std::strcmp(event_out.kind,"speed-applied"))saw_speed=true;
 }assert(saw_outcome&&saw_map&&saw_speed);
 // Invalid settings revoke and cannot revive an earlier deadline on the next tick.
 bad=config;auto invalid=settings;invalid.grade=4;assert(host->set_options(&invalid)==PGR_QBH_BAD_CONFIG);assert(host->tick(gettid())==0);
 PgrThrowValue unchanged{1,0.5f,0,0,0,0};value=unchanged;host->rewrite_submit(&ball,&value);assert(!std::memcmp(&value,&unchanged,sizeof(value)));
 assert(root_count==0);
 // Deadline belongs to settings, never to repeated ticks.
 settings.lease_ms=1;assert(host->set_options(&settings)==0);sleep_ms(12);assert(host->tick(gettid())==0);
 value=unchanged;host->rewrite_submit(&ball,&value);assert(!std::memcmp(&value,&unchanged,sizeof(value)));
 // Immediate OFF shared-site attestation prevents both native callbacks.
 assert(host->shared_sites(0)==0);value=unchanged;assert(host->rewrite_submit(&ball,&value)==PGR_QBH_NOT_READY);
 void *forwarded[2]{};assert(host->begin_catch(nullptr,nullptr,gettid(),forwarded)==0);assert(host->shared_sites(3)==0);
 settings.lease_ms=2000;settings.iv_allowed=1;settings.iv_catch=1;assert(host->set_options(&settings)==0);assert(host->tick(gettid())==0);
 // Shared IV frame roots survive cancellation until the original's final caller pairs end.
 Object plate{&class_tokens[C_NAMEPLATE],{}},map_pokemon{&class_tokens[C_MAPPK],{}};
 const unsigned before_frame=root_count;
 uint64_t frame=host->begin_catch(&plate,&map_pokemon,gettid(),forwarded);
 assert(frame&&forwarded[0]==&plate&&forwarded[1]==&map_pokemon&&root_count==before_frame+2);
 assert(wrapped_btxu()==&response&&current_speed()==20);
 std::thread foreign([&]{assert(host->stop()==0);assert(host->tick(uint64_t(gettid()))==PGR_QBH_OFF_OWNER);});foreign.join();
 assert(current_speed()==20);host->sample(&gauge);assert(gauge.stopping&&gauge.owned>0);
 assert(host->shared_sites(0)==0);
 assert(host->tick(gettid())==0);assert(current_speed()==1&&root_count==2);
 assert(host->end_catch(frame,gettid())==0);assert(root_count==0);
 assert(host->end_catch(frame,gettid())!=0); // duplicate cannot steal another frame
 assert(host->tick(gettid())==0);host->sample(&gauge);assert(!gauge.owned&&!gauge.ready);
 assert(host->set_options(&settings)==PGR_QBH_STOPPED);
 std::puts("PASS quickball62 unchanged-host fixture: real Dobby+1/5/7/8/13/15/49 install; caller/catch speed/outcome/completion, shared submit+paired hit, AR roots, map coordinates, invalid/expired leases, site revocation, foreign-stop owner cleanup. Metadata55 output substituted; not live metadata proof.");
 return 0;
}

#include "egg_hatch.h"
#include "egg_hatch_handles.h"
#include "egg_hatch_types.h"
#include "patch_work_budget.h"
#include "egg_hatch_policy.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "shared_host.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <unistd.h>
namespace {
using namespace pgr_shared_meta;
PgrBootBinding b{};
const PgrMetaApi *md=nullptr;
const PgrSharedMetadataOutput *shared=nullptr;
const PgrRuntimeTransportApi *tr=nullptr;
pgr_egg::Policy policy;
std::atomic<bool> stopped{false};
bool ready=false,attempted=false,error=false,busy=false,native_off=false;
unsigned native_failures=0;
uint64_t next=0,retry=0,report_at=0,last_info_id=0,last_screen_id=0;
using pgr_egg_handles::Handle;
Handle last_info=0,last_screen=0;
void *hatch_class=nullptr,*info_class=nullptr,*gui_class=nullptr,*screen_class=nullptr,*pokemon_class=nullptr;
void *active_field=nullptr,*display_field=nullptr,*shiny_field=nullptr,*iv_fields[3]{};
const void *find_all=nullptr,*enabled_method=nullptr,*alive_method=nullptr,*skip_method=nullptr,*fast_method=nullptr;
void *type_object=nullptr,*array_length=nullptr;
void *target(Handle h){return h?reinterpret_cast<pgr_egg_handles::Target>(b.runtime.gchandle_get_target)(h):nullptr;}
Handle hold(void *o){if(!o)return 0;auto h=reinterpret_cast<pgr_egg_handles::Create>(b.runtime.gchandle_new)(o,1);if(!h)error=true;return h;}
void release(Handle &h){pgr_egg_handles::release(h,reinterpret_cast<pgr_egg_handles::Free>(b.runtime.gchandle_free));}
struct Root {Handle h=0;explicit Root(void *o=nullptr):h(hold(o)){}Root(Handle owned,bool):h(owned){}~Root(){release(h);}void *get()const{return target(h);}Root(const Root&)=delete;};
bool hatch_type(void *base,void *actual){return pgr_egg::hatch_type(base,actual,reinterpret_cast<pgr_egg::Assignable>(b.meta.class_is_assignable_from));}
void *invoke(const void *method,void *o,void **args=nullptr){if(!method){error=true;return nullptr;}
 if(o){void *parent=reinterpret_cast<void *(*)(const void*)>(b.meta.method_get_class)(method),*actual=reinterpret_cast<void *(*)(void*)>(b.runtime.object_get_class)(o);if(!hatch_type(parent,actual)){error=true;return nullptr;}}
 void *exc=nullptr;
 void *r=reinterpret_cast<void *(*)(const void*,void*,void**,void**)>(b.runtime.runtime_invoke)(method,o,args,&exc);
 if(exc)error=true;return exc?nullptr:r;
}
template<class T>T field(void *o,const void *f){T value{};if(!o||!f){error=true;return value;}
 reinterpret_cast<void(*)(void*,const void*,void*)>(b.runtime.field_get_value)(o,f,&value);return value;}
bool boolean(void *boxed){if(!boxed){error=true;return false;}Root r(boxed);if(!r.get()){error=true;return false;}auto p=reinterpret_cast<void *(*)(void*)>(b.runtime.object_unbox)(r.get());if(!p){error=true;return false;}return *static_cast<uint8_t*>(p)!=0;}
void event(const char *kind,const char *reason=""){
 char json[384];std::snprintf(json,sizeof(json),"{\"kind\":\"%s\",\"reason\":\"%s\",\"backend\":\"native\",\"taps\":%u,\"nativeOff\":%s}",kind,reason,policy.taps,native_off?"true":"false");
 const char channel[]="egg-hatch";PgrRuntimeTransportString s[]={{channel,sizeof(channel)-1},{json,uint32_t(std::strlen(json))}};tr->emit(19,nullptr,0,s,2);
}
// No managed calls here; safe even for a rejected foreign-thread tick.
std::atomic<int> last_block{-1};std::atomic<uint64_t> last_block_at{0};
void blocked(uint64_t now,int code,const char *reason,int fresh=-1,int allowed=-1,int fault=-1){
 uint64_t previous=last_block_at.load();int prior=last_block.load();
 if(prior==code&&now>=previous&&now-previous<30000)return;
 if(now>=previous&&now-previous<250)return;
 if(!last_block_at.compare_exchange_strong(previous,now))return;last_block=code;
 auto wire=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));
 char json[320];std::snprintf(json,sizeof(json),"{\"kind\":\"gate\",\"reason\":\"%s\",\"boottimeMs\":%llu,\"fresh\":%d,\"allowed\":%d,\"fault\":%d}",reason,(unsigned long long)now,fresh,allowed,fault);
 const char channel[]="egg-hatch";PgrRuntimeTransportString args[]={{channel,sizeof(channel)-1},{json,uint32_t(std::strlen(json))}};wire->emit(19,nullptr,0,args,2);
}
bool actions_allowed(){
 PgrRuntimeTransportSample a{},z{};int allowed=0,observe=1;
 return !stopped&&!tr->sample(&a)&&a.snapshot_fresh&&!a.fault&&!tr->get_bool(40,&allowed)&&allowed==1&&!tr->get_bool(42,&observe)&&observe==0&&!tr->sample(&z)&&z.snapshot_fresh&&!z.fault&&a.snapshot_seq==z.snapshot_seq&&a.generation==z.generation;
}
void publish(uint64_t now,bool active,bool tap,const char *status){
 static bool last_ready=false,last_active=false,last_tap=false;static char last_status[256]{};
 if(!active&&!tap&&!last_active&&!last_tap&&last_ready==ready&&std::strcmp(last_status,status)==0)return;
 last_ready=ready;last_active=active;last_tap=tap;std::snprintf(last_status,sizeof(last_status),"%s",status);
 int64_t values[]={ready?1:0,active?1:0,tap?1:0,int64_t(active?now+200:0)};
 PgrRuntimeTransportString text{status,uint32_t(std::strlen(status))};
 if(tr->emit(60,values,4,&text,1)){policy.suspend();next=now+15000;}
}
void *klass(const char *ns,const char *name,const char *canonical){
 PgrMetaClassQuery q{1,sizeof(q),ns,name,canonical,PGR_MD_MATCH_EXACT,0};PgrMetaClassResult r{};
 return md->resolve_class(&q,&r)?nullptr:const_cast<void*>(r.klass);
}
const void *method(void *owner,const char *name,const char *result,bool is_static,const char *arg=nullptr){
 if(!owner)return nullptr;PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=owner;q.name=name;
 q.return_type=result;q.return_match=PGR_MD_MATCH_EXACT;q.is_static=is_static?1:0;
 q.param_count=arg?1:0;q.param_types[0]=arg;q.param_match[0]=PGR_MD_MATCH_EXACT;PgrMetaMethodResult r{};
 return md->validate_method(&q,&r)?nullptr:r.method;
}
void *typed_field(void *owner,const char *name,const char *expected){
 if(!owner)return nullptr;void *f=reinterpret_cast<void *(*)(void*,const char*)>(b.meta.class_get_field_from_name)(owner,name);
 if(!f||reinterpret_cast<void *(*)(void*)>(b.meta.field_get_parent)(f)!=owner||
  (reinterpret_cast<uint32_t(*)(void*)>(b.meta.field_get_flags)(f)&0x50))return nullptr;
 void *t=reinterpret_cast<void *(*)(void*)>(b.meta.field_get_type)(f);if(!t)return nullptr;
 char *n=reinterpret_cast<char *(*)(void*)>(b.meta.type_get_name)(t);bool ok=n&&std::strcmp(n,expected)==0;
 if(n)reinterpret_cast<void(*)(void*)>(b.meta.mem_free)(n);return ok?f:nullptr;
}
void *export_code(const char *name){void *p=dlsym(b.handle,name);PgrMetaCodeQuery q{1,sizeof(q),p,4,nullptr,0,0};return p&&!md->validate_code(&q)?p:nullptr;}
bool prepare(){
 auto boot=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));if(boot->snapshot(&b))return false;
 auto sh=static_cast<const PgrSharedHostApi*>(pgrazor_shared_host_api(1));shared=sh->metadata();if(!shared)return false;
 md=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));attempted=true;
 hatch_class=klass("Niantic.Holoholo.EggHatch","EggHatchState","Niantic.Holoholo.EggHatch.EggHatchState");
 info_class=klass("","PokemonInfoEggHatch","PokemonInfoEggHatch");
 gui_class=klass("Niantic.Holoholo.UI","EggHatchGuiController","Niantic.Holoholo.UI.EggHatchGuiController");
 screen_class=const_cast<void*>(shared->opt_classes[OC_DGUI]);pokemon_class=const_cast<void*>(shared->opt_classes[OC_PK]);
 void *behaviour=klass("UnityEngine","Behaviour","UnityEngine.Behaviour"),*object=klass("UnityEngine","Object","UnityEngine.Object");
 void *state=klass("Niantic.Zeppelin.GameStates","GameState","Niantic.Zeppelin.GameStates.GameState");
 if(hatch_class&&object&&!reinterpret_cast<bool(*)(void*,void*)>(b.meta.class_is_assignable_from)(object,hatch_class))hatch_class=nullptr;
 active_field=typed_field(state,"<IsActiveState>k__BackingField","System.Boolean");
 find_all=method(object,"FindObjectsOfType","UnityEngine.Object[]",true,"System.Type");
 alive_method=method(object,"op_Implicit","System.Boolean",true,"UnityEngine.Object");
 enabled_method=method(behaviour,"get_isActiveAndEnabled","System.Boolean",false);
 skip_method=method(info_class,"SkipEggHatchAnimation","System.Void",false);
 fast_method=method(info_class,"set_FastForward","System.Void",false,"System.Boolean");
 type_object=export_code("il2cpp_type_get_object");array_length=export_code("il2cpp_array_length");
 display_field=typed_field(pokemon_class,"pokemonDisplay_","Holoholo.Rpc.PokemonDisplayProto");
 void *display=const_cast<void*>(shared->opt_classes[OC_DISPLAY]);shiny_field=typed_field(display,"Shiny","System.Boolean");
 iv_fields[0]=typed_field(pokemon_class,"individualAttack_","System.Int32");
 iv_fields[1]=typed_field(pokemon_class,"individualDefense_","System.Int32");
 iv_fields[2]=typed_field(pokemon_class,"individualStamina_","System.Int32");
 // Summary detection is mandatory even if its close/read metadata is unavailable.
 ready=info_class&&gui_class&&screen_class&&find_all&&alive_method&&enabled_method&&type_object&&array_length;
 const char *reason=!info_class?"PokemonInfoEggHatch":!gui_class?"EggHatchGuiController":!screen_class?"PokemonInfoGuiController":!find_all?"FindObjectsOfType":!alive_method?"UnityEngine.Object.op_Implicit":!enabled_method?"Behaviour.isActiveAndEnabled":!type_object?"il2cpp_type_get_object":!array_length?"il2cpp_array_length":skip_method?"skip-ready":"tap-fallback-only";
 event(ready?"metadata-ready":"metadata-unavailable",reason);return ready;
}
bool is_alive(void *o){void *args[]={o};return boolean(invoke(alive_method,nullptr,args));}
// One rooted array and one rooted candidate at a time; no raw managed pointer survives an invoke.
Handle scan(void *k,bool state=false){
 if(!k)return 0;
 void *type=reinterpret_cast<void *(*)(void*)>(b.meta.class_get_type)(k);
 Root type_obj(reinterpret_cast<void *(*)(void*)>(type_object)(type));if(!type_obj.get()){error=true;return 0;}
 void *arg[]={type_obj.get()};Root array(invoke(find_all,nullptr,arg));if(error||!array.get())return 0;
 uintptr_t count=reinterpret_cast<uintptr_t(*)(void*)>(array_length)(array.get());if(count>4096){error=true;return 0;}
 for(uintptr_t i=0;i<count&&!error;++i){void *o=nullptr;std::memcpy(&o,static_cast<char*>(array.get())+32+i*sizeof(void*),sizeof(o));Root candidate(o);
  if(!candidate.get())continue;void *actual=reinterpret_cast<void *(*)(void*)>(b.runtime.object_get_class)(candidate.get());if(!hatch_type(k,actual))continue;
  if(!is_alive(candidate.get())||error)continue;
  const bool showing=state?field<uint8_t>(candidate.get(),active_field)!=0:boolean(invoke(enabled_method,candidate.get()));
  if(showing&&!error){
   // Log the runtime type without guessing the Daily Adventure Egg class name.
   static void *last[4]{};unsigned slot=k==info_class?0:k==gui_class?1:k==screen_class?2:3;
   if(last[slot]!=actual){last[slot]=actual;void *t=reinterpret_cast<void *(*)(void*)>(b.meta.class_get_type)(actual);char *name=t?reinterpret_cast<char *(*)(void*)>(b.meta.type_get_name)(t):nullptr;
    if(name){char safe[240];unsigned n=0;for(const unsigned char *c=reinterpret_cast<unsigned char*>(name);*c&&n<sizeof(safe)-1;c++)safe[n++]=(*c>=32&&*c<127&&*c!='"'&&*c!='\\')?char(*c):'_';safe[n]=0;event("scene-type",safe);reinterpret_cast<void(*)(void*)>(b.meta.mem_free)(name);}}
   Handle owned=candidate.h;candidate.h=0;return owned;}
 }return 0;
}
uint64_t identity(void *o,Handle &previous,uint64_t &id){
 if(o&&o!=target(previous)){release(previous);previous=hold(o);++id;}
 return o?id:0;
}
const char *close_reason="not-checked";
bool close_ready(void *screen){
 auto reject=[](const char *why){close_reason=why;return false;};
 if(shared->optional[OPT_DETAILS]!=PGR_SHARED_OPT_AVAILABLE||!shared->opt_handles[H_DET_CLICK]||!screen)return reject("details-metadata-unavailable");
 Root gui(screen);
 if(!gui.get()||!hatch_type(screen_class,reinterpret_cast<void *(*)(void*)>(b.runtime.object_get_class)(gui.get()))||!is_alive(gui.get()))return reject("summary-not-alive-or-type-mismatch");
 // STATE_OPEN=2 is the same validated enum value used by shared_host details completion.
 if(field<int32_t>(gui.get(),shared->fields[1])!=2)return reject("summary-not-open");
 Root data(field<void*>(gui.get(),shared->opt_handles[H_DET_DATA]));
 if(!data.get()||reinterpret_cast<void *(*)(void*)>(b.runtime.object_get_class)(data.get())!=shared->opt_classes[OC_DATA])return reject("summary-data-type-mismatch");
 Root callback(field<void*>(gui.get(),shared->opt_handles[H_DET_CALLBACK]));
 if(!callback.get()||field<void*>(data.get(),shared->opt_handles[H_DET_ON_DISMISSED])!=callback.get())return reject("dismiss-callback-mismatch");
 Root list(field<void*>(data.get(),shared->opt_handles[H_DET_LIST]));
 if(!list.get()||reinterpret_cast<void *(*)(void*)>(b.runtime.object_get_class)(list.get())!=shared->opt_handles[H_DET_LIST_CLASS])return reject("pokemon-list-type-mismatch");
 int index=field<int>(data.get(),shared->opt_handles[H_DET_INDEX]);if(index<0)return reject("negative-pokemon-index");
 Root boxed_count(invoke(shared->opt_handles[H_DET_COUNT],list.get()));if(error||!boxed_count.get())return reject("pokemon-count-unavailable");
 void *unboxed=reinterpret_cast<void *(*)(void*)>(b.runtime.object_unbox)(boxed_count.get());if(!unboxed)return reject("pokemon-count-unbox-failed");
 int count=0;std::memcpy(&count,unboxed,sizeof(count));if(count<1||count>4096||index>=count)return reject("pokemon-index-out-of-range");
 void *args[]={&index};Root pk(invoke(shared->opt_handles[H_DET_ITEM],list.get(),args));
 if(error||!pk.get()||reinterpret_cast<void *(*)(void*)>(b.runtime.object_get_class)(pk.get())!=pokemon_class)return reject("pokemon-type-mismatch");
 // Managed reads may reenter the game: re-prove the backing data/callback and open state.
 bool stable=!error&&is_alive(gui.get())&&field<int32_t>(gui.get(),shared->fields[1])==2&&
  field<void*>(gui.get(),shared->opt_handles[H_DET_DATA])==data.get()&&
  field<void*>(gui.get(),shared->opt_handles[H_DET_CALLBACK])==callback.get()&&
  field<void*>(data.get(),shared->opt_handles[H_DET_ON_DISMISSED])==callback.get()&&
  field<void*>(data.get(),shared->opt_handles[H_DET_LIST])==list.get()&&
  field<int>(data.get(),shared->opt_handles[H_DET_INDEX])==index;
 close_reason=stable?"ready":"summary-context-changed";return stable;
}
bool special(void *screen){
 if(shared->optional[OPT_DETAILS]!=PGR_SHARED_OPT_AVAILABLE||!display_field||!shiny_field||!iv_fields[0]||!iv_fields[1]||!iv_fields[2])return true;
 Root gui(screen);Root data(field<void*>(gui.get(),shared->opt_handles[H_DET_DATA]));if(!data.get()||error)return true;
 Root list(field<void*>(data.get(),shared->opt_handles[H_DET_LIST]));int index=field<int>(data.get(),shared->opt_handles[H_DET_INDEX]);
 if(!list.get()||index<0||error)return true;void *args[]={&index};Root pk(invoke(shared->opt_handles[H_DET_ITEM],list.get(),args));
 if(!pk.get()||error||reinterpret_cast<void *(*)(void*)>(b.runtime.object_get_class)(pk.get())!=pokemon_class)return true;
 Root display(field<void*>(pk.get(),display_field));if(!display.get()||error)return true;
 if(field<uint8_t>(display.get(),shiny_field))return true;
 int iv[3];for(unsigned i=0;i<3;++i){iv[i]=field<int>(pk.get(),iv_fields[i]);if(iv[i]<0||iv[i]>15)return true;}
 return error||(iv[0]==15&&iv[1]==15&&iv[2]==15);
}
}
void pgr_egg_hatch_stop(){stopped=true;}
void pgr_egg_hatch_tick(uint64_t now,uint64_t owner_tid,bool cleanup){
 if(owner_tid!=uint64_t(gettid())){blocked(now,1,"wrong-thread");return;}if(busy){blocked(now,2,"reentry");return;}busy=true;struct Guard{~Guard(){busy=false;}}guard;
 tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));
 if(cleanup||stopped){blocked(now,3,"stopped-or-cleanup");policy.suspend();if(b.runtime.gchandle_free){release(last_info);release(last_screen);}if(ready){ready=false;publish(now,false,false,"Stopped");}return;}
 if(now<retry){blocked(now,4,"retry-backoff");return;}
 if(!ready){if(attempted){blocked(now,5,"metadata-unavailable");return;}if(!prepare()){blocked(now,6,"bootstrap-or-metadata-not-ready");if(attempted)publish(now,false,false,"Unavailable: hatch metadata mismatch; see egg-hatch log");return;}publish(now,false,false,"Ready");}
 if(!reinterpret_cast<void*(*)()>(b.runtime.thread_current)()){blocked(now,7,"thread-not-attached");return;}
 PgrRuntimeTransportSample a{},z{};int allowed=0,keep=1,observe=1;
 const char *wire_block=nullptr;
 if(tr->sample(&a))wire_block="transport-sample-failed";
 else if(!a.snapshot_fresh)wire_block="transport-stale";
 else if(a.fault)wire_block="transport-fault";
 else if(tr->get_bool(40,&allowed))wire_block="allowed-reader-failed";
 else if(tr->get_bool(41,&keep))wire_block="keep-reader-failed";
 else if(tr->get_bool(42,&observe))wire_block="observe-reader-failed";
 else if(tr->sample(&z))wire_block="transport-resample-failed";
 else if(!z.snapshot_fresh)wire_block="transport-expired-during-read";
 else if(z.fault)wire_block="transport-fault-during-read";
 else if(a.snapshot_seq!=z.snapshot_seq)wire_block="snapshot-changed-during-read";
 else if(a.generation!=z.generation)wire_block="generation-changed-during-read";
 bool fresh=wire_block==nullptr;
 if(!fresh||allowed!=1){blocked(now,!fresh?8:9,!fresh?wire_block:"java-authorization-denied",a.snapshot_fresh,allowed,a.fault||z.fault);bool had=policy.session;if(fresh)policy.step(now,false,keep!=0,{});else policy.suspend();if(had)publish(now,false,false,"Paused while settings are open or feature is Off");return;}
 blocked(now,0,"authorized",1,allowed,0);
 if(now<next)return;next=now+pgr_perf::egg_interval(policy.session&&!policy.done);error=false;
 Root active(hatch_class&&active_field?scan(hatch_class,true):0,true),info(scan(info_class),true),gui(scan(gui_class),true),screen(scan(screen_class),true);
 pgr_egg::Observation o;o.active=active.get()!=nullptr;o.gui=gui.get()!=nullptr;o.info=info.get()!=nullptr;o.screen=screen.get()!=nullptr;
 o.info_id=identity(info.get(),last_info,last_info_id);o.screen_id=identity(screen.get(),last_screen,last_screen_id);
 if(!error&&o.screen&&!policy.screen_seen&&keep){o.special=special(screen.get());if(error){o.special=true;event("read-special-unavailable");error=false;}}
 if(stopped||tr->get_bool(40,&allowed)||allowed!=1||tr->sample(&z)||!z.snapshot_fresh||z.fault){policy.suspend();publish(now,false,false,"Off");return;}
 if(error){policy.suspend();retry=now+15000;event("paused","managed-read");publish(now,false,false,"Paused: hatch scene read failed");return;}
 int latest_observe=1;if(tr->get_bool(42,&latest_observe))latest_observe=1;observe=observe||latest_observe;
 if(observe){
  policy.suspend();publish(now,false,false,"Diagnostic: observation only; no automatic actions");
  static unsigned last_scene=~0u;unsigned scene=(o.active?1:0)|(o.info?2:0)|(o.gui?4:0)|(o.screen?8:0);
  if(scene!=last_scene||now>=report_at){last_scene=scene;report_at=now+3000;
   char json[384];std::snprintf(json,sizeof(json),"{\"kind\":\"observe\",\"boottimeMs\":%llu,\"active\":%s,\"info\":%s,\"gui\":%s,\"screen\":%s,\"infoId\":%llu,\"screenId\":%llu,\"actionsEnabled\":false}",(unsigned long long)now,o.active?"true":"false",o.info?"true":"false",o.gui?"true":"false",o.screen?"true":"false",(unsigned long long)o.info_id,(unsigned long long)o.screen_id);
   const char channel[]="egg-hatch";PgrRuntimeTransportString args[]={{channel,sizeof(channel)-1},{json,uint32_t(std::strlen(json))}};tr->emit(19,nullptr,0,args,2);
  }return;
 }
 const bool was_closing=policy.session&&!policy.done&&policy.screen_seen&&policy.close_attempts;
 unsigned actions=policy.step(now,true,keep!=0,o);
 if(was_closing&&policy.done&&!o.screen)event("closed","summary-disappeared");
 next=now+pgr_perf::egg_interval(policy.session&&!policy.done);
 if(actions&pgr_egg::START)event("session",o.gui&&!o.info?"initial-gui":"state-or-animation");
 if((actions&pgr_egg::SKIP)&&actions_allowed()){bool ok=false;if(skip_method&&!native_off){invoke(skip_method,info.get());ok=!error;
   if(ok)native_failures=0;else if(++native_failures>=2)native_off=true;error=false;}
  policy.skip_result(ok);event(ok?"native-skip":"native-fallback",ok?"":"skip-unavailable-or-exception");}
 if(actions_allowed()&&(actions&pgr_egg::FAST)){if(fast_method&&!native_off){uint8_t value=1;void *args[]={&value};invoke(fast_method,info.get(),args);error=false;}}
 if(actions&pgr_egg::CLOSE){
  bool closed=false;
  if(actions_allowed()){
   if(close_ready(screen.get())&&!error&&actions_allowed()){
    event("close-before","validated-open-screen");invoke(shared->opt_handles[H_DET_CLICK],screen.get());closed=!error;event(error?"close-error":"close-requested");
   }else event("close-skipped",error?"summary-read-or-invoke-error":close_reason);
  }else event("close-skipped","authorization-revoked");
  policy.close_result(now,closed);error=false;
 }
 if(actions&pgr_egg::KEEP)event("kept","shiny-perfect-or-unreadable");
 if(actions&pgr_egg::TIMEOUT)event("timeout");
 // Authorizations are refreshed every scan and revoked as soon as any summary is visible.
 const bool tap_allowed=actions_allowed()&&policy.session&&!policy.done&&!o.screen&&(o.info||o.gui);
 const char *status=!policy.session?"Waiting for an egg to hatch":actions&pgr_egg::KEEP?"Shiny, 100% or unreadable: kept open":
  actions&pgr_egg::CLOSE?(policy.done?"Hatched Pokemon screen finished":"Waiting to close the hatched Pokemon screen"):actions&pgr_egg::TIMEOUT?"Hatch skip gave up; tap to continue":
  actions&pgr_egg::TAP?"Tapping through the hatch":policy.done?"Hatch finished":o.screen?"Hatched Pokemon screen":"Hatch detected";
 publish(now,tap_allowed,(actions&pgr_egg::TAP)!=0,status);
 if(now>=report_at){report_at=now+30000;event("scan",o.screen?"pokemon-screen":o.info?"animation":o.gui?"initial-gui":"no-scene");}
}

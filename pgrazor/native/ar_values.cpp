#include "ar_values.h"
#include "ar_throw.h"
#include "shared_metadata.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include "vgp_results.h"
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <time.h>
#include <unistd.h>
// AR+ throw values. The AR+ hook sends a proximity (meters; closer is smaller) and an awareness
// with every throw. Both must be above zero: zero is the protobuf default, which is never sent,
// so the server would see no AR+ values. Once the game master is ready this reads the encounter
// settings' close-proximity and low-awareness thresholds and hands the hook half of each, so the
// values sit inside the game's own bonus range. Until then the hook keeps its small defaults.
namespace {
using namespace pgr_shared_meta;
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
template<class F>F fn(void *p){require(p,"missing runtime export");return reinterpret_cast<F>(p);}
PgrBootBinding boot{};const PgrSharedMetadataOutput *md=nullptr;
uint64_t owner=0,next_try=0;bool busy=false,ready=false,done=false;int tries=0;
constexpr uint64_t kRetryMs=5000;constexpr int kMaxTries=60;
uint64_t clock_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
void *exp(const char *name){std::string n="il2cpp_";n+=name;void *p=dlsym(boot.handle,n.c_str());require(p,name);return p;}
struct Root {
 uintptr_t h=0;
 Root()=default;explicit Root(void *v){if(v){h=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(v,0);require(h,"GC root");}}
 Root(const Root&)=delete;Root& operator=(const Root&)=delete;
 Root(Root &&r) noexcept:h(r.h){r.h=0;}
 ~Root(){if(h)fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);}
 void *get()const{return h?fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h):nullptr;}
 explicit operator bool()const{return get()!=nullptr;}
};
void *klass(void *o){require(o,"null object");return fn<void*(*)(void*)>(boot.runtime.object_get_class)(o);}
void *cls(const char *ns,const char *name){
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api,"metadata API");
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult out{};require(api->resolve_class(&q,&out)==0,name);return const_cast<void*>(out.klass);
}
void *method(void *o,const char *name,int count){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(klass(o),name,count);require(m,name);return m;}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(o,name,n),o,args);}
template<class T>T unbox(void *o){require(o,"boxed value");T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");std::memcpy(&v,p,sizeof(v));return v;}
void *h(unsigned id){return const_cast<void*>(md->opt_handles[id]);}
Root type_object(void *k){return Root(fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k)));}
Root resolve(const char *ns,const char *name){
 Root type=type_object(h(H_IL_MAP));void *args[]={type.get()};Root map(invoke(h(H_IL_FIND_M),nullptr,args));if(!map)return {};
 Root container(invoke(h(H_IL_CONTAINER_M),map.get()));if(!container)return {};
 Root target=type_object(cls(ns,name));void *resolve_args[]={target.get()};return Root(invoke(h(H_IL_RESOLVE_M),container.get(),resolve_args));
}
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
void event(const std::string &json){auto tr=transport();if(!tr)return;PgrRuntimeTransportString strings[]={{"ar-values",9},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,strings,2);}

// IGameMasterData.Get<EncounterSettingsProto>(), closed once through reflection.
Root encounter_settings(void *gm){
 void *open=nullptr,*iter=nullptr;
 while(void *m=fn<void*(*)(void*,void**)>(exp("class_get_methods"))(klass(gm),&iter)){
  if(std::strcmp(fn<const char*(*)(void*)>(exp("method_get_name"))(m),"Get")!=0)continue;
  if(fn<bool(*)(void*)>(boot.meta.method_is_generic)(m)&&fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(m)==0){open=m;break;}
 }
 require(open,"generic Get");
 Root reflected(fn<void*(*)(void*,void*)>(exp("method_get_object"))(open,klass(gm)));
 Root types(fn<void*(*)(void*,uintptr_t)>(boot.runtime.array_new)(cls("System","Type"),1));
 Root t=type_object(cls("Holoholo.Rpc","EncounterSettingsProto"));
 void **slot=reinterpret_cast<void**>(static_cast<char*>(types.get())+32);fn<void(*)(void*,void**,void*)>(exp("gc_wbarrier_set_field"))(types.get(),slot,t.get());
 void *args[]={types.get()};Root closed_reflection(call(reflected.get(),"MakeGenericMethod",1,args));
 void *closed=fn<void*(*)(void*)>(exp("method_get_from_reflection"))(closed_reflection.get());require(closed,"closed Get");
 return Root(invoke(closed,gm));
}
// A System.Single setting: the proto's field of that name, else its get_ property.
float number(void *o,const char *name){
 if(void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name)){
  char *type=fn<char*(*)(void*)>(boot.meta.type_get_name)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));require(type,name);
  const bool single=std::strcmp(type,"System.Single")==0;fn<void(*)(void*)>(boot.meta.mem_free)(type);require(single,name);
  float v=0;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;
 }
 const std::string getter=std::string("get_")+name;return unbox<float>(call(o,getter.c_str()));
}
void step(){
 if(!ready){auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(md&&api&&api->snapshot(&boot)==0,"Runtime unavailable");ready=true;}
 Root gm=resolve("","IGameMasterData");require(bool(gm),"game master");
 if(!unbox<bool>(call(gm.get(),"get_IsReady")))return;  // try again later
 Root settings=encounter_settings(gm.get());require(bool(settings),"encounter settings");
 const float close=number(settings.get(),"ArCloseProximityThreshold");
 const float low=number(settings.get(),"ArLowAwarenessThreshold");
 const float close_mult=number(settings.get(),"ArCloseProximityMultiplier");
 const float low_mult=number(settings.get(),"ArLowAwarenessMaxMultiplier");
 const float penalty=number(settings.get(),"ArAwarenessPenaltyThreshold");
 if(close>0&&low>0)pgr_ar_throw_set_values(close*0.5f,low*0.5f);
 event("{\"phase\":\"ready\",\"closeProximityThreshold\":"+std::to_string(close)+",\"lowAwarenessThreshold\":"+std::to_string(low)+
  ",\"closeProximityMultiplier\":"+std::to_string(close_mult)+",\"lowAwarenessMaxMultiplier\":"+std::to_string(low_mult)+
  ",\"awarenessPenaltyThreshold\":"+std::to_string(penalty)+",\"applied\":"+(close>0&&low>0?"true":"false")+"}");
 done=true;
}

// The catch result of the last AR+ throw: whether the server listed the AR+ catch bonus.
uintptr_t promise=0;uint64_t promise_since=0;
std::string literal_name(void *k,int value){void *iter=nullptr;
 while(auto f=fn<void*(*)(void*,void**)>(exp("class_get_fields"))(k,&iter)){
  if((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)!=0x8050)continue;int n=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&n);
  if(n==value)return fn<const char*(*)(void*)>(exp("field_get_name"))(f);}
 return std::to_string(value);}
void *object_field(void *o,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name);require(f,name);void *v=nullptr;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;}
void report_catch(void *response){
 void *k=nullptr;std::string status;
 {void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(response),"Status");require(f,"Status");
  k=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));int v=0;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(response,f,&v);status=literal_name(k,v);}
 std::string activities;bool bonus=false;
 if(void *award=object_field(response,"Scores"))
  if(void *list=object_field(award,"ActivityType")){
   const int n=unbox<int>(call(list,"get_Count"));require(n>=0&&n<=64,"activity count");
   for(int i=0;i<n;i++){void *ix[]={&i};void *boxed=call(list,"get_Item",1,ix);const std::string name=literal_name(klass(boxed),unbox<int>(boxed));
    bonus=bonus||name.find("ArPlus")!=std::string::npos||name.find("AR_PLUS")!=std::string::npos;activities+=(activities.empty()?"":",")+name;}
  }
 event("{\"phase\":\"catch\",\"status\":\""+status+"\",\"arPlusBonus\":"+(bonus?"true":"false")+",\"activities\":\""+activities+"\"}");
}
void poll_catch(uint64_t tid){
 if(!promise){promise=reinterpret_cast<uintptr_t>(pgr_ar_throw_take_promise());promise_since=clock_ms();if(!promise)return;}
 auto api=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));
 void *target=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(promise);
 PgrVgpResultsResult out{};const int rc=api&&target?api->poll(target,PGR_VR_PHASE_ENCOUNTER,tid,&out):PGR_VR_DONE;
 if(rc==0&&clock_ms()-promise_since<20000)return;
 fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(promise);promise=0;
 Root result;result.h=out.value_handle;
 if(rc==PGR_VR_DONE&&result)report_catch(result.get());
 else event("{\"phase\":\"catch\",\"status\":\"unknown\",\"poll\":"+std::to_string(rc)+",\"state\":"+std::to_string(out.state)+
  ",\"promise\":"+(target?"true":"false")+",\"api\":"+(api?"true":"false")+"}");
}
}  // namespace

void pgr_ar_values_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid))return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;owner=tid;md=metadata;
 if(ready){
  try{poll_catch(tid);}
  catch(const Failure &f){if(promise)fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(promise);promise=0;
   event(std::string("{\"phase\":\"catch\",\"status\":\"unknown\",\"error\":\"")+f.where+"\"}");}
  catch(...){if(promise)fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(promise);promise=0;}
 }
 const uint64_t now=clock_ms();if(done||now<next_try)return;next_try=now+kRetryMs;
 try{step();}
 catch(const Failure &f){
  if(++tries>=kMaxTries)done=true;
  if(tries==1||done)event(std::string("{\"phase\":\"error\",\"message\":\"")+f.where+"\",\"final\":"+(done?"true":"false")+"}");
 }
 catch(...){done=true;event("{\"phase\":\"error\",\"message\":\"native failure\"}");}
}

#include "session_stats.h"
#include "shared_metadata.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

// Session stats: the app keeps a rolling history of the player's lifetime counters to show what changed
// in the last hour and the last 24 hours. This reads them from the game's own PlayerService, the same
// numbers the profile page shows, so every catch and spin counts whoever made it. Nothing is sent to the
// game's servers.
namespace {
using namespace pgr_shared_meta;
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
template<class F>F fn(void *p){require(p,"missing runtime export");return reinterpret_cast<F>(p);}
PgrBootBinding boot{};const PgrSharedMetadataOutput *md=nullptr;
uint64_t owner=0,next_sample=0,services_at=0,sent_at=0;bool busy=false,ready=false;
std::string sent;
// A reading every 10 s is two managed calls and a few field reads. The service lookup, which searches the
// scene, runs once a minute, and an unchanged reading is sent once a minute.
constexpr uint64_t kSampleMs=10000,kServicesMs=60000,kResendMs=60000,kRetryMs=15000;
uint64_t clock_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
void *exp(const char *name){std::string n="il2cpp_";n+=name;void *p=dlsym(boot.handle,n.c_str());require(p,name);return p;}
struct Root {
 uintptr_t h=0;
 Root()=default;explicit Root(void *v){if(v){h=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(v,0);require(h,"GC root");}}
 Root(const Root&)=delete;Root& operator=(const Root&)=delete;
 Root(Root &&r) noexcept:h(r.h){r.h=0;}
 Root& operator=(Root &&r) noexcept {if(this!=&r){reset();h=r.h;r.h=0;}return *this;}
 ~Root(){reset();}
 void reset(){if(h){fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);h=0;}}
 void *get()const{return h?fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h):nullptr;}
 explicit operator bool()const{return get()!=nullptr;}
};
void *klass(void *o){require(o,"null object");return fn<void*(*)(void*)>(boot.runtime.object_get_class)(o);}
std::string class_name(void *o){return fn<const char*(*)(void*)>(exp("class_get_name"))(klass(o));}
void *cls(const char *ns,const char *name){
 static std::map<std::string,void*> cache;std::string key=std::string(ns)+"."+name;auto it=cache.find(key);if(it!=cache.end())return it->second;
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api,"metadata API");
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult out{};require(api->resolve_class(&q,&out)==0,name);return cache[key]=const_cast<void*>(out.klass);
}
std::string typename_of(void *type){char *p=fn<char*(*)(void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
template<class T>T field(void *o,const char *name,const char *type){
 void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name);require(f,name);
 require(!(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10),name);
 require(typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))==type,name);
 T v{};fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;
}
void *method(void *o,const char *name,int count){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(klass(o),name,count);require(m,name);require(fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(m)==unsigned(count),name);return m;}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(o,name,n),o,args);}
void *h(unsigned id){return const_cast<void*>(md->opt_handles[id]);}
Root type_object(void *k){return Root(fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k)));}
Root resolve(const char *ns,const char *name){
 Root type=type_object(h(H_IL_MAP));void *args[]={type.get()};Root map(invoke(h(H_IL_FIND_M),nullptr,args));if(!map)return {};
 Root container(invoke(h(H_IL_CONTAINER_M),map.get()));if(!container)return {};
 Root target=type_object(cls(ns,name));void *resolve_args[]={target.get()};return Root(invoke(h(H_IL_RESOLVE_M),container.get(),resolve_args));
}
void event(const std::string &json){
 auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));if(!tr)return;
 PgrRuntimeTransportString strings[]={{"session-stats",13},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,strings,2);
}
Root player,iap;  // cached game services
void step(){
 if(!ready){auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(md&&api&&api->snapshot(&boot)==0,"Runtime unavailable");ready=true;}
 const uint64_t now=clock_ms();if(now<next_sample)return;next_sample=now+kRetryMs;
 // Looked up again each minute: the game rebuilds its services after an account switch.
 if(!player||now-services_at>=kServicesMs){
  player=resolve("Niantic.Holoholo","IPlayerService");services_at=now;
  try{iap=resolve("Niantic.Holoholo.Storefront","IIapService");}catch(const Failure&){iap.reset();}
 }
 if(!player)return;
 Root stats(call(player.get(),"get_PlayerStats"));if(!stats)return;  // not loaded yet after sign-in
 require(class_name(stats.get())=="PlayerStatsProto","Player stats identity");
 const long long xp=field<int64_t>(stats.get(),"Experience","System.Int64");
 const int caught=field<int32_t>(stats.get(),"NumPokemonCaptured","System.Int32");
 const int seen=field<int32_t>(stats.get(),"NumPokemonEncountered","System.Int32");
 const int stops=field<int32_t>(stats.get(),"PokeStopVisits","System.Int32");
 const float km=field<float>(stats.get(),"KmWalked","System.Single");
 const int hatched=field<int32_t>(stats.get(),"NumEggsHatched","System.Int32");
 if(xp<=0)return;  // an empty proto before the player's data arrives
 // The stardust balance the game's own screens show; -1 when it cannot be read.
 long long dust=-1;
 try{
  if(iap){void *box=call(iap.get(),"get_Stardusts");
   if(box&&class_name(box)=="Int32"){
    void *p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(box);int v=-1;
    if(p)std::memcpy(&v,p,sizeof(v));
    if(v>=0)dust=v;}}}
 catch(const Failure&){}
 char json[256];
 std::snprintf(json,sizeof(json),"{\"xp\":%lld,\"caught\":%d,\"seen\":%d,\"stops\":%d,\"km\":%.3f,\"hatched\":%d,\"dust\":%lld}",xp,caught,seen,stops,double(km),hatched,dust);
 if(sent!=json||now-sent_at>=kResendMs){event(json);sent=json;sent_at=now;}
 next_sample=now+kSampleMs;
}
}  // namespace

void pgr_session_stats_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid)||!metadata)return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 owner=tid;md=metadata;
 try{step();}
 catch(const Failure &f){next_sample=clock_ms()+kServicesMs;player.reset();iap.reset();event(std::string("{\"error\":\"")+f.where+"\"}");}
 catch(...){next_sample=clock_ms()+kServicesMs;player.reset();iap.reset();}
}

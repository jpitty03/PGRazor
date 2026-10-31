#include "buddy_emotion_policy.h"
#include "shared_metadata.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "vgp_results.h"
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <time.h>
#include <cstring>
#include <cmath>
#include <map>
#include <algorithm>
namespace {
using namespace pgr_shared_meta;
using namespace pgr_buddy_ep;
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
PgrBootBinding boot{};const PgrSharedMetadataOutput *md=nullptr;
bool initialized=false,busy=false,paused=false;uint64_t owner=0,next_poll=0,next_action=0,last_stats=0,last_publish=0;
std::string account,config,status;History history;int ep=-1,feed_burst=0;bool excited=false;int64_t prevention=0;
uint64_t clock_ms(clockid_t c=CLOCK_BOOTTIME){timespec t{};clock_gettime(c,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
template<class F>F fn(void *p){require(p,"missing runtime export");return reinterpret_cast<F>(p);}
void *exp(const char *name){std::string n="il2cpp_";n+=name;void *p=dlsym(boot.handle,n.c_str());require(p,name);return p;}
struct Root {
 uintptr_t h=0;
 Root()=default;explicit Root(void *v){if(v){h=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(v,0);require(h,"GC root");}}
 Root(const Root&)=delete;Root& operator=(const Root&)=delete;
 Root(Root &&r) noexcept:h(r.h){r.h=0;}
 Root& operator=(Root &&r) noexcept {if(this!=&r){reset();h=r.h;r.h=0;}return *this;}
 ~Root(){reset();}void reset(){if(h){fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);h=0;}}
 void *get()const{return h?fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h):nullptr;}
 explicit operator bool()const{return get()!=nullptr;}
};
void *klass(void *o){require(o,"null object");return fn<void*(*)(void*)>(boot.runtime.object_get_class)(o);}
void *cls(const char *ns,const char *name){
 static std::map<std::string,void*> cache;std::string key=std::string(ns)+"."+name;auto it=cache.find(key);if(it!=cache.end())return it->second;
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api,"metadata API");
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult out{};require(api->resolve_class(&q,&out)==0,name);return cache[key]=const_cast<void*>(out.klass);
}
std::string typename_of(void *type){char *p=fn<char*(*)(void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
void *field_info(void *k,const char *name,const char *type){
 void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);
 auto ft=fn<void*(*)(void*)>(boot.meta.field_get_type)(f);require(typename_of(ft)==type,name);
 require(!(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10),"instance field required");return f;
}
template<class T>T field(void *o,const char *name,const char *type){T value{};auto f=field_info(klass(o),name,type);fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&value);return value;}
template<class T>void put(void *o,const char *name,const char *type,T value){auto f=field_info(klass(o),name,type);fn<void(*)(void*,void*,void*)>(boot.runtime.field_set_value)(o,f,&value);}
void putref(void *o,const char *name,const char *type,void *value){auto f=field_info(klass(o),name,type);fn<void(*)(void*,void*,void*)>(exp("field_set_value_object"))(o,f,value);}
void *method(void *k,const char *name,int count){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,count);require(m,name);require(fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(m)==unsigned(count),name);return m;}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(klass(o),name,n),o,args);}
template<class T>T boxed(void *o,const char *type){require(o&&typename_of(fn<void*(*)(void*)>(boot.meta.class_get_type)(klass(o)))==type,type);T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");memcpy(&v,p,sizeof(v));return v;}
std::string string(void *o){if(!o)return {};require(klass(o)==cls("System","String"),"string identity");int n=fn<int(*)(void*)>(exp("string_length"))(o);require(n>=0&&n<=512,"string bound");auto p=fn<const uint16_t*(*)(void*)>(boot.runtime.string_chars)(o);std::string s;for(int i=0;i<n;i++){if(p[i]>127)return "<non-ascii>";s+=char(p[i]);}return s;}
void *h(unsigned id){return const_cast<void*>(md->opt_handles[id]);}
Root type_object(void *k){return Root(fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k)));}
Root resolve(void *contract){
 Root type=type_object(h(H_IL_MAP));void *args[]={type.get()};Root map(invoke(h(H_IL_FIND_M),nullptr,args));if(!map)return {};
 Root container(invoke(h(H_IL_CONTAINER_M),map.get()));if(!container)return {};
 Root target=type_object(contract);void *resolve_args[]={target.get()};return Root(invoke(h(H_IL_RESOLVE_M),container.get(),resolve_args));
}
Root resolve(const char *ns,const char *name){return resolve(cls(ns,name));}
int literal(void *k,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);require((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)==0x8050,name);int n=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&n);return n;}
int result_value(void *o){auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),"Result");require(f,"Result");int n=0;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&n);return n;}
bool success(void *o,bool opening_gift=false){auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),"Result");require(f,"Result");auto k=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));int result=result_value(o);return opening_gift?(result==literal(k,"SuccessAddedLootToInventory")||result==literal(k,"SuccessAddedSouvenirToCollections")):result==literal(k,"Success");}
int count(Root &list,int cap){int n=boxed<int>(call(list.get(),"get_Count"),"System.Int32");require(n>=0&&n<=cap,"collection bound");return n;}
Root item(Root &list,int index){void *args[]={&index};return Root(call(list.get(),"get_Item",1,args));}
struct Permission {Settings settings;std::string config,saved;bool valid=false,allowed=false,live=false,ready=false,map=false;};
Permission permission(){
 Permission p;auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));if(!tr)return p;
 PgrRuntimeTransportSample a{},z{};if(tr->sample(&a)||!a.snapshot_fresh||a.fault)return p;
 char text[257]{};uint32_t len=0;int active=0,ready=0,map=0;
 if(tr->get_string(9,text,sizeof(text),&len)||!parse(text,len,p.settings))return p;p.config.assign(text,len);
 if(tr->get_string(10,text,sizeof(text),&len)||len>256)return p;p.saved.assign(text,len);
 tr->get_bool(9,&active);tr->get_bool(16,&ready);tr->get_bool(38,&map);
 if(tr->sample(&z)||!z.snapshot_fresh||z.snapshot_seq!=a.snapshot_seq)return p;p.valid=true;p.live=active;p.ready=ready;p.map=map;p.allowed=p.settings.enabled&&active&&ready&&map;return p;
}
void publish(const std::string &message,bool force=false){
 uint64_t now=clock_ms();if(!force&&message==status&&now-last_publish<5000)return;status=message;last_publish=now;
 auto wall=clock_ms(CLOCK_REALTIME);uint64_t maintenance=history.interaction&&wall>=history.interaction?((wall-history.interaction>=1200000)?0:(1200000-(wall-history.interaction))/1000):0;
 std::string json="{\"message\":\""+message+"\",\"ep\":"+std::to_string(ep)+",\"excited\":"+(excited?"true":"false")+",\"maintenanceSeconds\":"+std::to_string(maintenance)+",\"history\":\""+encode(history)+"\",\"account\":\""+account+"\"}";
 __android_log_print(ANDROID_LOG_INFO,"PGRazorBuddyEP","%s EP=%d excited=%d",message.c_str(),ep,int(excited));
 auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));PgrRuntimeTransportString values[]={{"buddy-emotion",13},{json.c_str(),uint32_t(json.size())}};
 if(tr&&tr->emit(19,nullptr,0,values,2))paused=true;
}
struct Pending {Root promise,reflection;int phase=0;uint64_t since=0,buddy=0;std::string config,account;bool canceled=false;};Pending pending;
void clear(){pending=Pending{};}
void adopt_buddy(const std::string &id,uint64_t buddy,const std::string &saved_text){
 if(id==account&&buddy==history.buddy)return;
 if(pending.phase)pending.canceled=true;
 account=id;history={};history.buddy=buddy;History saved;
 if(saved_text.find(id+"|")==0&&parse_history(saved_text.substr(id.size()+1),saved)&&saved.buddy==buddy)history=saved;
 ep=-1;excited=false;last_stats=0;feed_burst=0;paused=false;next_action=next_poll=0;
}
uint64_t current_buddy(){Root svc=resolve("Niantic.Holoholo.Buddy","IBuddyPokemonService");if(!svc||!boxed<bool>(call(svc.get(),"get_HasBuddy"),"System.Boolean"))return 0;Root b(call(svc.get(),"get_BuddyPokemon"));return b?field<uint64_t>(b.get(),"BuddyPokemonId","System.UInt64"):0;}
Root stats(){Root s=resolve("Niantic.Holoholo.Buddy","IBuddyStatsService");return s?Root(call(s.get(),"get_BuddyStats")):Root{};}
void observe(Root &s){
 require(s&&klass(s.get())==cls("Holoholo.Rpc","BuddyObservedData"),"buddy data identity");
 ep=field<int>(s.get(),"CurrentEmotionPoints","System.Int32");require(ep>=0&&ep<100000,"buddy EP range");
 Root util=resolve("Niantic.Holoholo.Buddy","IBuddyEmotionLevelUtil");require(bool(util),"buddy mood service");void *args[]={s.get()};
 excited=boxed<bool>(call(util.get(),"IsInYatta",1,args),"System.Boolean");
 Root settings(call(util.get(),"CurrentEmotionSettings",1,args));require(bool(settings),"buddy mood settings");prevention=field<int64_t>(settings.get(),"DecayPreventionDurationMs","System.Int64");
}
bool hungry(Root &s){void *args[]={s.get()};if(boxed<bool>(invoke(h(H_BD_M_COOLDOWN),nullptr,args),"System.Boolean"))return false;
 float remain=boxed<float>(invoke(h(H_BD_M_REMAINING),nullptr,args),"System.Single"),full=boxed<float>(invoke(h(H_BD_M_FULLNESS),nullptr,args),"System.Single");
 return std::isfinite(remain)&&std::isfinite(full)&&remain<=0&&full<1;
}
bool on_map(Root &s){void *args[]={s.get()};float remaining=boxed<float>(invoke(h(H_BD_M_REMAINING),nullptr,args),"System.Single");return std::isfinite(remaining)&&remaining>0;}
int stock(int berry){if(!berry)return 0;Root bag=resolve("Niantic.Holoholo","IItemBag");if(!bag)return 0;void *args[]={&berry};return boxed<int>(call(bag.get(),"GetItemCount",1,args),"System.Int32");}
int feeding_berry(const Settings &s){int counts[5]{};for(int i=0;i<5;i++)counts[i]=stock(s.berries[i]);return select_berry(s,counts);}
Root gift(Root &s){return Root(field<void*>(s.get(),"BuddyGiftPickedUp","Holoholo.Rpc.BuddyGiftProto"));}
bool action_permission(const Permission &p){auto f=permission();return f.allowed&&f.config==p.config&&current_buddy()==history.buddy;}
Root photo_request(const Permission &p){
 Root service=resolve("Niantic.Holoholo.ARPhoto","IARPhotoRpcService");require(bool(service),"snapshot service unavailable");
 Root rpc(field<void*>(service.get(),"eiud","Niantic.Holoholo.Rpc.IRpcHandler"));require(bool(rpc),"snapshot RPC");
 Root req(fn<void*(*)(void*)>(boot.runtime.object_new)(cls("Holoholo.Rpc","CompleteSnapshotSessionProto")));call(req.get(),".ctor");
 put(req.get(),"PhotoPokemonId","System.UInt64",history.buddy);put(req.get(),"NumPhotosTaken","System.Int32",1);put(req.get(),"SnapshotSessionStartTime","System.Int64",int64_t(clock_ms(CLOCK_REALTIME)));
 void *open=method(klass(rpc.get()),"SendRpc",5);require(fn<bool(*)(void*)>(boot.meta.method_is_generic)(open),"generic snapshot RPC");
 Root reflected(fn<void*(*)(void*,void*)>(exp("method_get_object"))(open,klass(rpc.get())));Root types(fn<void*(*)(void*,uintptr_t)>(boot.runtime.array_new)(cls("System","Type"),2));
 void *ks[]={klass(req.get()),cls("Holoholo.Rpc","CompleteSnapshotSessionOutProto")};
 for(int i=0;i<2;i++){Root t=type_object(ks[i]);void **slot=reinterpret_cast<void**>(static_cast<char*>(types.get())+32+i*8);fn<void(*)(void*,void**,void*)>(exp("gc_wbarrier_set_field"))(types.get(),slot,t.get());}
 void *args[]={types.get()};pending.reflection=Root(call(reflected.get(),"MakeGenericMethod",1,args));void *closed=fn<void*(*)(void*)>(exp("method_get_from_reflection"))(pending.reflection.get());
 require(closed&&fn<bool(*)(void*)>(boot.meta.method_is_inflated)(closed),"closed snapshot RPC");require(action_permission(p),"permission changed");
 int id=literal(cls("Holoholo.Rpc","Method"),"CompleteSnapshotSession"),zero=0,timeout=10000;void *send[]={&id,req.get(),&zero,&timeout,&zero};return Root(invoke(closed,rpc.get(),send));
}
const char *labels[]={"Snapshot","Pet","Feed","Gift"};
void begin(int phase,const Permission &p){
 require(action_permission(p),"buddy permission changed");pending.phase=phase;pending.since=clock_ms();pending.buddy=history.buddy;pending.account=account;pending.config=p.config;
 Root rpc=resolve("Niantic.Holoholo.Buddy","IBuddyRpcService");require(bool(rpc),"buddy RPC service");
 if(phase==5){pending.promise=Root(call(rpc.get(),"RequestBuddyStats"));last_stats=clock_ms();}
 else{
  int berry=0;Action action=Action(phase-1);if(action==Feed){Root s=stats();berry=feeding_berry(p.settings);require(s&&hungry(s)&&berry,"berry reserve or hunger changed");}
  if(action==Gift){Root s=stats();require(s&&on_map(s)&&gift(s),"buddy gift changed");}
  mark_attempt(history,action,clock_ms(CLOCK_REALTIME));publish(std::string("Requesting ")+labels[action],true);
  require(action_permission(p),"permission changed before interaction");
  if(action==Snapshot)pending.promise=photo_request(p);
  else if(action==Pet)pending.promise=Root(call(rpc.get(),"Pet"));
  else if(action==Gift)pending.promise=Root(call(rpc.get(),"OpenGift"));
  else {int one=1;void *args[]={&berry,&one};__android_log_print(ANDROID_LOG_INFO,"PGRazorBuddyEP","Feeding selected berry %d",berry);pending.promise=Root(call(rpc.get(),"Feed",2,args));}
 }
 require(bool(pending.promise),"buddy request outcome unknown");
}
void poll(const Permission &p){
 if(!p.allowed||p.config!=pending.config||account!=pending.account||history.buddy!=pending.buddy)pending.canceled=true;
 uint64_t now=clock_ms();if(now-pending.since>15000){bool canceled=pending.canceled;paused=!canceled;clear();feed_burst=0;publish(canceled?"Canceled request expired; refreshing buddy":"Paused: request timed out; toggle off to resume",true);return;}
 auto api=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));require(api,"promise reader");PgrVgpResultsResult out{};int rc=api->poll(pending.promise.get(),1,owner,&out);if(!rc)return;
 if(rc!=PGR_VR_DONE){bool canceled=pending.canceled;clear();feed_burst=0;paused=!canceled;publish(canceled?"Canceled request ended; refreshing buddy":"Paused: buddy request rejected; toggle off to resume",true);return;}
 Root response;response.h=out.value_handle;if(pending.canceled){clear();feed_burst=0;next_action=now+5000;return;}
 int phase=pending.phase;const char *name=phase==1?"CompleteSnapshotSessionOutProto":phase==2?"BuddyPettingOutProto":phase==3?"BuddyFeedingOutProto":phase==4?"OpenBuddyGiftOutProto":"BuddyStatsOutProto";
 require(response&&klass(response.get())==cls("Holoholo.Rpc",name),"buddy response identity");
 bool ok=false;int result=0;
 if(phase==1){auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(response.get()),"Status");require(f,"snapshot status");auto k=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));require(fn<bool(*)(void*)>(boot.meta.class_is_enum)(k),"snapshot status enum");fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(response.get(),f,&result);ok=result==literal(k,"Success");}
 else {ok=success(response.get(),phase==4);result=result_value(response.get());}
 clear();next_action=now+2500;
 if(!ok){feed_burst=0;paused=phase!=4;publish(phase==4?"Gift declined ("+std::to_string(result)+"); continuing buddy care":"Paused: buddy action declined ("+std::to_string(result)+"); toggle off to resume",true);return;}
 if(phase!=1){Root observed(field<void*>(response.get(),"ObservedData","Holoholo.Rpc.BuddyObservedData"));observe(observed);last_stats=now;}
 if(phase<5){mark_success(history,clock_ms(CLOCK_REALTIME));if(phase==3&&feed_burst>0)--feed_burst;publish(std::string(labels[phase-1])+" accepted",true);if(phase==1)last_stats=0;}
 else publish("Buddy stats refreshed",true);
}
void validate(){
 field_info(cls("Holoholo.Rpc","BuddyObservedData"),"CurrentEmotionPoints","System.Int32");
 for(const char *n:{"Pet","RequestBuddyStats","OpenGift"})method(cls("Niantic.Holoholo.Buddy","BuddyRpcService"),n,0);
 method(cls("Niantic.Holoholo.Buddy","BuddyRpcService"),"Feed",2);
 field_info(cls("Holoholo.Rpc","CompleteSnapshotSessionProto"),"PhotoPokemonId","System.UInt64");field_info(cls("Holoholo.Rpc","CompleteSnapshotSessionProto"),"NumPhotosTaken","System.Int32");field_info(cls("Holoholo.Rpc","CompleteSnapshotSessionProto"),"SnapshotSessionStartTime","System.Int64");
 literal(cls("Holoholo.Rpc","Method"),"CompleteSnapshotSession");
 publish("Buddy interfaces validated",true);
}
}
void pgr_buddy_emotion_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||!tid||tid!=uint64_t(gettid())||(owner&&tid!=owner))return;struct Guard{Guard(){busy=true;}~Guard(){busy=false;}} guard;
 try{
  auto p=permission();if(p.valid&&!p.settings.enabled)paused=false;
  if(!initialized){if(!p.allowed)return;auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(api&&api->snapshot(&boot)==0,"runtime bootstrap");md=metadata;require(md,"metadata");owner=tid;validate();initialized=true;}
  if(p.valid&&p.settings.enabled){Root player=resolve("Niantic.Holoholo","IPlayerService");if(!player)return;std::string id=string(call(player.get(),"get_PlayerId"));if(id.empty())return;uint64_t buddy=current_buddy();
   adopt_buddy(id,buddy,p.saved);}
  if(pending.phase){poll(p);return;}if(paused)return;
  if(!p.allowed){publish(!p.valid?"Waiting for runtime settings":!p.settings.enabled?"Off":!p.live?"Waiting for foreground game session":!p.ready?"Waiting for item services":"Waiting for the map");return;}
  if(!history.buddy){publish("Choose a buddy first");return;}
  uint64_t now=clock_ms();if(now<next_action)return;
  if(!last_stats||now-last_stats>=60000){begin(5,p);return;}
  Root s=stats();if(!s){publish("Waiting for buddy data");return;}observe(s);
  if(now<next_poll)return;next_poll=now+1000;
  bool is_hungry=hungry(s);int selected=feeding_berry(p.settings),count=stock(selected);Action action=choose(p.settings,history,clock_ms(CLOCK_REALTIME),is_hungry,count,on_map(s)&&bool(gift(s)));
  if(feed_burst>0){if(p.settings.berry&&is_hungry&&count>p.settings.reserve)action=Feed;else feed_burst=0;}
  if(action==None){publish(!p.settings.berry?"No feeding berries selected":!selected?"No selected berry above reserve ("+std::to_string(p.settings.reserve)+" each)":is_hungry?"Hungry; waiting for feeding retry cooldown":excited?"Excited; buddy fed or in feeding cooldown":"Buddy fed or in feeding cooldown; waiting for next interaction");return;}
  if(action==Feed&&!feed_burst)feed_burst=3;
  begin(int(action)+1,p);
 }catch(const Failure &e){paused=true;clear();feed_burst=0;publish(std::string("Paused: ")+e.where,true);}catch(...){paused=true;clear();feed_burst=0;publish("Paused: buddy engine failure",true);}
}
void pgr_buddy_emotion_stop(uint64_t tid){if(initialized&&tid==owner&&tid==uint64_t(gettid())){if(pending.phase)pending.canceled=true;paused=true;}}

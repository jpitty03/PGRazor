#include "gym_automation.h"
#include "shared_metadata.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "vgp_results.h"
#include "inventory_host.h"
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <time.h>
#include <cstring>
#include <algorithm>
#include <utility>
#include <cmath>
namespace {
using namespace pgr_shared_meta;
using namespace pgr_gym;
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
PgrBootBinding boot{};const PgrSharedMetadataOutput *md=nullptr;
bool initialized=false,busy=false,paused=false;uint64_t owner=0,next_scan=0;
std::string status,account;History history;std::map<std::string,uint64_t> inspected;std::map<uint64_t,uint64_t> reserved;
uint64_t clock_ms(clockid_t c=CLOCK_BOOTTIME){timespec t{};clock_gettime(c,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
template<class F>F fn(void *p){require(p,"missing runtime export");return reinterpret_cast<F>(p);}
void *exp(const char *name){std::string n="il2cpp_";n+=name;void *p=dlsym(boot.handle,n.c_str());require(p,name);return p;}
void event(const std::string &message,int result=0,int dust=0){
 if(message==status&&result==0&&dust==0)return;status=message;
 __android_log_print(ANDROID_LOG_INFO,"PGRazorGym","%s result=%d dust=%d",message.c_str(),result,dust);
 const auto *tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));
 std::string json="{\"kind\":\"gym-automation\",\"message\":\""+message+"\",\"result\":"+std::to_string(result)+",\"dust\":"+std::to_string(dust)+"}";
 // Use the existing host diagnostic event action; no gameplay pointer crosses it.
 if(tr){PgrRuntimeTransportString strings[]={{"gym-automation",14},{json.c_str(),uint32_t(json.size())}};if(tr->emit(19,nullptr,0,strings,2)!=0)paused=true;}
}
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
bool success(void *o){auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),"Result");require(f,"Result");auto k=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));return result_value(o)==literal(k,"Success");}
int count(Root &list,int cap){int n=boxed<int>(call(list.get(),"get_Count"),"System.Int32");require(n>=0&&n<=cap,"collection bound");return n;}
Root item(Root &list,int index){void *args[]={&index};return Root(call(list.get(),"get_Item",1,args));}
struct Permission {Settings settings;std::string config;double lat=0,lon=0;uint64_t seq=0;bool allowed=false,valid_config=false;};
Permission permission(){Permission p;auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));if(!tr)return p;
 PgrRuntimeTransportSample a{},z{};if(tr->sample(&a)||!a.snapshot_fresh||a.fault)return p;
 char text[65]{};uint32_t len=0;int active=0,ready=0,map=0,has=0;
 if(tr->get_string(8,text,sizeof(text),&len)||len>64||!parse(text,len,p.settings))return p;p.config.assign(text,len);
 tr->get_bool(3,&active);tr->get_bool(16,&ready);tr->get_bool(38,&map);
 bool coords=tr->get_double(0,&p.lat,&has)==0&&has;has=0;coords=(tr->get_double(1,&p.lon,&has)==0&&has)&&coords;
 if(tr->sample(&z)||!z.snapshot_fresh||z.snapshot_seq!=a.snapshot_seq)return p;p.seq=a.snapshot_seq;p.valid_config=true;
 p.allowed=active&&ready&&map&&coords&&(p.settings.deploy||p.settings.feed||p.settings.spin)&&std::isfinite(distance(p.lat,p.lon,p.lat,p.lon));return p;
}
void *virtual_call(Root &o,void *m,void **args=nullptr){void *v=fn<void*(*)(void*,void*)>(exp("object_get_virtual_method"))(o.get(),m);require(v,"virtual method");return invoke(v,o.get(),args);}
Root fort(Root &gym){void *m=fn<void*(*)(void*,void*)>(exp("object_get_virtual_method"))(gym.get(),h(H_VGP_M_FORT));return Root(invoke(m,gym.get()));}
bool gym_valid(Root &gym,const Permission &p,std::string *why=nullptr,double *meters=nullptr,bool for_deploy=false,bool for_spin=false){
 auto reject=[why](const char *reason){if(why)*why=reason;return false;};
 if(!gym||klass(gym.get())!=cls("Niantic.Holoholo.Gym","MapGym"))return reject("gym identity");
 void *obj=gym.get();void *alive_args[]={obj};if(!boxed<bool>(invoke(const_cast<void*>(md->methods[5]),nullptr,alive_args),"System.Boolean"))return reject("inactive gym object");
 Root f=fort(gym);if(!f)return reject("missing gym data");
 double d=distance(p.lat,p.lon,field<double>(f.get(),"Latitude","System.Double"),field<double>(f.get(),"Longitude","System.Double"));if(meters)*meters=d;
 if(!std::isfinite(d)||d>40)return reject("outside 40m");
 if(!for_spin&&!boxed<bool>(call(gym.get(),"get_IsFriendly"),"System.Boolean"))return reject("not friendly");
 if(boxed<bool>(call(gym.get(),"get_InGymState"),"System.Boolean"))return reject("gym screen open");
 if(!field<bool>(f.get(),"Enabled","System.Boolean"))return reject("gym disabled");
 if(for_spin){
  if(boxed<bool>(virtual_call(gym,h(H_VGP_M_COOLING)),"System.Boolean"))return reject("disc cooldown");
  return true;
 }
 // The raw Closed flag is not a reliable action gate on loaded gyms.
 // Let the typed server response decide closure; deployment attempts retain
 // their ten-minute retry budget. Deployment lockouts remain placement-only.
 bool placement=for_deploy||!p.settings.feed;
 if(field<bool>(f.get(),"IsInBattle","System.Boolean"))return reject("gym under attack");
 if(placement&&(field<int64_t>(f.get(),"DeployLockoutEndMs","System.Int64")>int64_t(clock_ms(CLOCK_REALTIME))||field<int64_t>(f.get(),"SameTeamDeployLockoutEndMs","System.Int64")>int64_t(clock_ms(CLOCK_REALTIME))))return reject("deployment lockout");
 Root raid(field<void*>(f.get(),"RaidInfo","Holoholo.Rpc.RaidInfoProto"));
 if(raid){int64_t start=0,end=0;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(raid.get(),h(H_MP_BATTLE),&start);fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(raid.get(),h(H_MP_END),&end);auto wall=int64_t(clock_ms(CLOCK_REALTIME));if(start<=wall&&end>wall)return reject("active raid");}
 return true;
}
struct Pending {Root gym,promise,reflection,details,enumerator,bag,gm,buddy;std::string id,config;uint64_t since=0,defender=0;int phase=0,scanned=0,last_item=0,stock_before=0;bool canceled=false,await_stock=false;std::vector<uint64_t> tags;std::vector<Defender> candidates;};
Pending pending;
Defender candidate(Root&);
// The typed SendRpc uses the game's RPC handler. Inventory is re-read before
// each action; no synthetic removal, berry decrement or reward is applied.
bool spin_bag_room(){
 Root bag=resolve("Niantic.Holoholo","IItemBag"),player=resolve("Niantic.Holoholo","IPlayerService");
 if(!bag||!player)return false;
 int total=boxed<int>(invoke(h(H_VGP_M_TOTAL_ITEMS),bag.get()),"System.Int32");
 int maximum=boxed<int>(invoke(h(H_VGP_M_MAX_ITEMS),player.get()),"System.Int32");
 return total>=0&&maximum>0&&total<maximum;
}
Root send(Root &request,const char *out_name,const char *rpc_name,Root &reflection){
 Root bag=resolve("Niantic.Holoholo","IItemBag");require(bool(bag),"item bag unavailable");
 Root rpc(field<void*>(bag.get(),"cynr","Niantic.Holoholo.Rpc.IRpcHandler"));require(bool(rpc),"RPC unavailable");
 void *open=method(klass(rpc.get()),"SendRpc",5);require(fn<bool(*)(void*)>(boot.meta.method_is_generic)(open),"generic RPC");
 Root reflected(fn<void*(*)(void*,void*)>(exp("method_get_object"))(open,klass(rpc.get())));
 Root types(fn<void*(*)(void*,uintptr_t)>(boot.runtime.array_new)(cls("System","Type"),2));
 void *ks[]={klass(request.get()),cls("Holoholo.Rpc",out_name)};
 for(int i=0;i<2;i++){Root t=type_object(ks[i]);void **slot=reinterpret_cast<void**>(static_cast<char*>(types.get())+32+i*8);fn<void(*)(void*,void**,void*)>(exp("gc_wbarrier_set_field"))(types.get(),slot,t.get());}
 void *args[]={types.get()};reflection=Root(call(reflected.get(),"MakeGenericMethod",1,args));
 void *closed=fn<void*(*)(void*)>(exp("method_get_from_reflection"))(reflection.get());require(closed&&fn<bool(*)(void*)>(boot.meta.method_is_inflated)(closed),"closed RPC");
 auto p=permission();require(p.allowed&&p.config==pending.config&&gym_valid(pending.gym,p,nullptr,nullptr,!strcmp(rpc_name,"GymDeploy"),!strcmp(rpc_name,"FortSearch")),"permission changed");
 // Resolve inventory again after generic-method construction; a concurrent
 // cleanup or manual action may have changed stock since planning.
 if(!strcmp(rpc_name,"GymFeedPokemon")){
  int chosen=field<int>(request.get(),"Item","Holoholo.Rpc.Item");void *a[]={&chosen};int stock=boxed<int>(call(bag.get(),"GetItemCount",1,a),"System.Int32");
  require(stock>(chosen==703?p.settings.nanab:p.settings.razz),"berry reserve changed");put(request.get(),"StartingQuantity","System.Int32",stock);
  pending.last_item=chosen;pending.stock_before=stock;
 }else if(!strcmp(rpc_name,"GymDeploy")){
  uint64_t id=field<uint64_t>(request.get(),"PokemonId","System.UInt64");void *a[]={&id};Root pokemon(call(pending.bag.get(),"GetPokemon",1,a));require(bool(pokemon),"defender inventory changed");
  auto c=candidate(pokemon);require(c.eligible&&(c.tagged||(p.settings.fallback&&!c.protected_fallback)),"defender eligibility changed");
 }
 if(!strcmp(rpc_name,"FortSearch"))require(p.settings.spin&&spin_bag_room(),"spin bag full or disabled");
 put(request.get(),"PlayerLatDegrees","System.Double",p.lat);put(request.get(),"PlayerLngDegrees","System.Double",p.lon);
 auto final_permission=permission();require(final_permission.allowed&&final_permission.config==pending.config&&gym_valid(pending.gym,final_permission,nullptr,nullptr,!strcmp(rpc_name,"GymDeploy"),!strcmp(rpc_name,"FortSearch")),"permission expired before request");
 int id=literal(cls("Holoholo.Rpc","Method"),rpc_name),zero=0,timeout=10000;void *rpc_args[]={&id,request.get(),&zero,&timeout,&zero};
 if(!strcmp(rpc_name,"FortSearch"))require(history.spin_attempt(pending.id,clock_ms()),"spin already attempted");
 if(!strcmp(rpc_name,"GymDeploy"))reserved[pending.defender]=clock_ms()+1800000;
 return Root(invoke(closed,rpc.get(),rpc_args));
}
Root request(const char *name){Root r(fn<void*(*)(void*)>(boot.runtime.object_new)(cls("Holoholo.Rpc",name)));require(bool(r),"request allocation");call(r.get(),".ctor");return r;}
void validate_bindings(){
 for(const char *name:{"GymDeployProto","GymFeedPokemonProto"}){
  void *k=cls("Holoholo.Rpc",name);method(k,".ctor",0);
  field_info(k,!strcmp(name,"GymDeployProto")?"FortId":"GymId","System.String");field_info(k,"PokemonId","System.UInt64");
  field_info(k,"PlayerLatDegrees","System.Double");field_info(k,"PlayerLngDegrees","System.Double");
 }
 field_info(cls("Holoholo.Rpc","GymFeedPokemonProto"),"Item","Holoholo.Rpc.Item");field_info(cls("Holoholo.Rpc","GymFeedPokemonProto"),"StartingQuantity","System.Int32");
 for(const char *name:{"GymDeployOutProto","GymFeedPokemonOutProto"}){
  void *k=cls("Holoholo.Rpc",name);auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,"Result");require(f,"gym Result");
  auto ek=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));require(fn<bool(*)(void*)>(boot.meta.class_is_enum)(ek),"gym result enum");literal(ek,"Success");
  field_info(k,"GymStatusAndDefenders","Holoholo.Rpc.GymStatusAndDefendersProto");
 }
 literal(cls("Holoholo.Rpc","Method"),"GymDeploy");literal(cls("Holoholo.Rpc","Method"),"GymFeedPokemon");
 field_info(cls("Holoholo.Rpc","GymFeedPokemonOutProto"),"StardustAwarded","System.Int32");
 field_info(cls("Holoholo.Rpc","GymFeedPokemonOutProto"),"CooldownComplete","System.Int64");
 field_info(cls("Holoholo.Rpc","GymDefenderProto"),"MotivatedPokemon","Holoholo.Rpc.MotivatedPokemonProto");
 field_info(cls("Holoholo.Rpc","MotivatedPokemonProto"),"Pokemon","Holoholo.Rpc.PokemonProto");
 method(cls("Niantic.Holoholo.Gym","MapGym"),"GetGymDetailsFromServer",0);
 Root bag=resolve("Niantic.Holoholo","IItemBag");require(bool(bag),"item bag not ready");field_info(klass(bag.get()),"cynr","Niantic.Holoholo.Rpc.IRpcHandler");
 event("Gym request bindings validated");
}
void clear_pending(){
 if(pending.enumerator){try{virtual_call(pending.enumerator,h(H_INC_M_DISPOSE));}catch(...) {}}
 pending=Pending{};
}
// A read-only/no-action result is temporary, not a minute-long exclusion.
// Keep action history intact: refreshing details must never bypass feed/deploy limits.
void retry_no_action(uint64_t now,uint64_t delay=5000){
 if(!pending.id.empty())inspected[pending.id]=now+delay;
 clear_pending();next_scan=now+delay;
}
Root reference(void*,const char*,const char*);
std::vector<Defender> defenders(){
 require(bool(pending.details),"gym details missing");
 Root rows=reference(pending.details.get(),"GymDefender","Niantic.Protobuf.Collections.RepeatedField");
 // Canonical IL2CPP generic names use a backtick; the runtime helper below uses
 // the live type descriptor rather than assuming a collection memory layout.
 std::vector<Defender> result;int n=count(rows,6);
 for(int i=0;i<n;i++){Root row=item(rows,i);Root motivated(field<void*>(row.get(),"MotivatedPokemon","Holoholo.Rpc.MotivatedPokemonProto"));Root pokemon(field<void*>(motivated.get(),"Pokemon","Holoholo.Rpc.PokemonProto"));
  Defender d;d.id=field<uint64_t>(pokemon.get(),"id_","System.UInt64");d.species=field<int>(pokemon.get(),"pokemonId_","System.Int32");
  if(field<int64_t>(motivated.get(),"FeedCooldownDurationMillis","System.Int64")>0)history.refuse(d.id,clock_ms());result.push_back(d);}
 return result;
}
// References to generic collections are checked structurally through reference
// type + exact generic name prefix. Each element is then checked before reading.
Root reference(void *o,const char *name,const char *prefix){
 auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name);require(f,name);
 void *ft=fn<void*(*)(void*)>(boot.meta.field_get_type)(f);void *k=fn<void*(*)(void*)>(boot.meta.class_from_type)(ft);
 require(k&&!fn<bool(*)(void*)>(boot.meta.class_is_valuetype)(k)&&typename_of(ft).find(prefix)==0,name);
 void *v=nullptr;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return Root(v);
}
Defender candidate(Root &pokemon){
 Defender d;require(klass(pokemon.get())==cls("Holoholo.Rpc","PokemonProto"),"Pokemon identity");
 d.id=field<uint64_t>(pokemon.get(),"id_","System.UInt64");d.species=field<int>(pokemon.get(),"pokemonId_","System.Int32");d.cp=field<int>(pokemon.get(),"cp_","System.Int32");
 auto held=reserved.find(d.id);if(held!=reserved.end()){if(held->second>clock_ms())return d;reserved.erase(held);}
 if(!d.id||field<bool>(pokemon.get(),"isEgg_","System.Boolean")||field<bool>(pokemon.get(),"favorite_","System.Boolean")||field<bool>(pokemon.get(),"isBad_","System.Boolean")||field<bool>(pokemon.get(),"isComponent_","System.Boolean")||field<bool>(pokemon.get(),"isFusion_","System.Boolean")||field<bool>(pokemon.get(),"isActivelyTraining_","System.Boolean"))return d;
 if(field<int>(pokemon.get(),"stamina_","System.Int32")<=0||field<int>(pokemon.get(),"stamina_","System.Int32")!=field<int>(pokemon.get(),"maxStamina_","System.Int32")||!string(field<void*>(pokemon.get(),"deployedFortId_","System.String")).empty()||!string(field<void*>(pokemon.get(),"deployedStationId_","System.String")).empty())return d;
 void *buddy_args[]={&d.id};if(boxed<bool>(call(pending.buddy.get(),"GetIsBuddy",1,buddy_args),"System.Boolean"))return d;
 void *gm_args[]={pending.gm.get()};if(!boxed<bool>(call(pokemon.get(),"IsMarkedAsDeployable",1,gm_args),"System.Boolean"))return d;
 gm_args[0]=pending.gm.get();if(boxed<bool>(call(pokemon.get(),"IsLegendary",1,gm_args),"System.Boolean"))return d;
 gm_args[0]=pending.gm.get();if(boxed<bool>(call(pokemon.get(),"IsMythic",1,gm_args),"System.Boolean"))return d;
 Root tags=reference(pokemon.get(),"pokemonTagIds_","Niantic.Protobuf.Collections.RepeatedField");int n=count(tags,1000);
 for(int i=0;i<n;i++){Root v=item(tags,i);uint64_t id=boxed<uint64_t>(v.get(),"System.UInt64");if(std::find(pending.tags.begin(),pending.tags.end(),id)!=pending.tags.end())d.tagged=true;}
 d.protected_fallback=boxed<bool>(call(pokemon.get(),"get_IsShinyPokemon"),"System.Boolean")||field<bool>(pokemon.get(),"isLucky_","System.Boolean")||field<int>(pokemon.get(),"individualAttack_","System.Int32")+field<int>(pokemon.get(),"individualDefense_","System.Int32")+field<int>(pokemon.get(),"individualStamina_","System.Int32")>=41;
 d.eligible=true;return d;
}
bool prepare_selection(){
 Root tags=resolve("Niantic.Holoholo.Tagging","ITaggingRpcService");if(!tags)return false;
 Root ready(call(tags.get(),"WaitForTagCacheInitialized"));if(!ready||!field<bool>(ready.get(),"completeCalled","System.Boolean")||field<bool>(ready.get(),"errorCalled","System.Boolean"))return false;
 int n=boxed<int>(call(tags.get(),"get_NumberOfTags"),"System.Int32");require(n>=0&&n<=1000,"tag count");
 for(int i=0;i<n;i++){void *args[]={&i};Root row(call(tags.get(),"GetCachedTagToPokemonBindingForIndex",1,args));if(!row)continue;
  Root tag(field<void*>(row.get(),"TagProto","Holoholo.Rpc.PokemonTagProto"));std::string name=string(field<void*>(tag.get(),"Name","System.String"));for(char &c:name)if(c>='A'&&c<='Z')c+=32;
  if(name=="gym"||name=="#gym")pending.tags.push_back(field<uint64_t>(tag.get(),"Id","System.UInt64"));}
 pending.bag=resolve("Niantic.Holoholo","IPokemonBag");if(!pending.bag)return false;
 pending.gm=reference(pending.bag.get(),"cypm","IGameMasterData");pending.buddy=resolve("Niantic.Holoholo.Buddy","IBuddyPokemonService");if(!pending.gm||!pending.buddy)return false;
 Root all(call(pending.bag.get(),"get_AllPokemon"));pending.enumerator=Root(virtual_call(all,h(H_INC_M_ENUMERATE)));require(bool(pending.enumerator),"inventory enumerator");pending.phase=4;return true;
}
bool snapshot_valid(const Permission &p){
 if(!p.allowed||p.config!=pending.config||!gym_valid(pending.gym,p))return false;
 Root fresh=fort(pending.gym),reported(field<void*>(pending.details.get(),"PokemonFortProto","Holoholo.Rpc.PokemonFortProto"));
 return fresh&&reported&&string(field<void*>(fresh.get(),"FortId","System.String"))==pending.id&&string(field<void*>(reported.get(),"FortId","System.String"))==pending.id&&field<int>(fresh.get(),"Team","System.Int32")==field<int>(reported.get(),"Team","System.Int32");
}
void issue_deploy(uint64_t id,const Permission &p){
 void *args[]={&id};Root pokemon(call(pending.bag.get(),"GetPokemon",1,args));if(!pokemon)return;
 auto d=candidate(pokemon);std::vector<int> species;for(auto r:defenders())species.push_back(r.species);
 if(choose({d},species,p.settings.fallback)!=id||!snapshot_valid(p))return;
 Root req=request("GymDeployProto");Root fid(fn<void*(*)(const char*)>(boot.runtime.string_new)(pending.id.c_str()));
 putref(req.get(),"FortId","System.String",fid.get());put(req.get(),"PokemonId","System.UInt64",id);put(req.get(),"PlayerLatDegrees","System.Double",p.lat);put(req.get(),"PlayerLngDegrees","System.Double",p.lon);
 if(!history.deploy_attempt(pending.id,clock_ms()))return;pending.phase=2;pending.since=clock_ms();pending.defender=id;
 pending.promise=send(req,"GymDeployOutProto","GymDeploy",pending.reflection);require(bool(pending.promise),"deployment outcome unknown");event("Deploying defender");
}
bool issue_feed(const Permission &p){
 Root bag=resolve("Niantic.Holoholo","IItemBag");if(!bag)return false;
 int nanab=703,razz=701;void *na[]={&nanab},*ra[]={&razz};int ns=boxed<int>(call(bag.get(),"GetItemCount",1,na),"System.Int32"),rs=boxed<int>(call(bag.get(),"GetItemCount",1,ra),"System.Int32");
 int chosen=berry(ns,rs,p.settings);if(!chosen){event("Berry reserves reached");return false;}
 uint64_t id=0;for(auto d:defenders())if(history.feed_allowed(d.id,clock_ms())){id=d.id;break;}if(!id)return false;
 Root req=request("GymFeedPokemonProto");Root fid(fn<void*(*)(const char*)>(boot.runtime.string_new)(pending.id.c_str()));
 putref(req.get(),"GymId","System.String",fid.get());put(req.get(),"PokemonId","System.UInt64",id);put(req.get(),"Item","Holoholo.Rpc.Item",chosen);put(req.get(),"StartingQuantity","System.Int32",chosen==703?ns:rs);
 put(req.get(),"PlayerLatDegrees","System.Double",p.lat);put(req.get(),"PlayerLngDegrees","System.Double",p.lon);
 if(!snapshot_valid(p)||!history.feed_attempt(id,clock_ms()))return false;
 pending.phase=3;pending.since=clock_ms();pending.defender=id;pending.promise=send(req,"GymFeedPokemonOutProto","GymFeedPokemon",pending.reflection);require(bool(pending.promise),"feeding outcome unknown");event("Feeding gym defender");return true;
}
void cooldown(const char *label){const auto *t=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));PgrRuntimeTransportString s{label,uint32_t(strlen(label))};if(t&&t->emit(17,nullptr,0,&s,1)!=0)paused=true;}
void act(const Permission &p){
 if(!snapshot_valid(p)){clear_pending();return;}auto rows=defenders();
 if(p.settings.deploy&&rows.size()<6&&history.deploy_allowed(pending.id,clock_ms())&&gym_valid(pending.gym,p,nullptr,nullptr,true)){
  if(!prepare_selection()){event("Waiting for defender inventory and tags");retry_no_action(clock_ms());return;}
  event("Selecting gym defender");return;
 }
 if(p.settings.feed&&!rows.empty()&&issue_feed(p))return;
 if(status!="Berry reserves reached")event("No eligible gym action; retrying shortly");retry_no_action(clock_ms());
}
void poll(const Permission &p){
 auto now=clock_ms();if(paused||p.config!=pending.config||!p.allowed)pending.canceled=true;
 if(pending.phase==4){
  if(pending.canceled||!snapshot_valid(p)){clear_pending();return;}
  unsigned steps=0;while(steps++<128){bool more=boxed<bool>(virtual_call(pending.enumerator,h(H_INC_M_MOVE)),"System.Boolean");if(!more){
    virtual_call(pending.enumerator,h(H_INC_M_DISPOSE));pending.enumerator.reset();std::vector<int> species;for(auto d:defenders())species.push_back(d.species);
    uint64_t id=choose(pending.candidates,species,p.settings.fallback);pending.phase=0;
    if(id)issue_deploy(id,p);
    if(!pending.promise){if(p.settings.feed&&issue_feed(p))return;if(status!="Berry reserves reached")event("No eligible defender; retrying shortly");retry_no_action(now,10000);}return;
   }
   require(++pending.scanned<=25000,"inventory capacity bound");Root pokemon(virtual_call(pending.enumerator,h(H_INC_M_CURRENT)));if(pokemon)pending.candidates.push_back(candidate(pokemon));
  }return;
 }
 if(pending.phase==5){if(now<next_scan)return;if(pending.canceled){clear_pending();return;}
  if(pending.await_stock){Root bag=resolve("Niantic.Holoholo","IItemBag");if(!bag)return;void *a[]={&pending.last_item};int stock=boxed<int>(call(bag.get(),"GetItemCount",1,a),"System.Int32");
   if(stock>=pending.stock_before){if(now-pending.since>5000){paused=true;event("Paused: waiting for berry inventory update");clear_pending();}return;}pending.await_stock=false;}
  act(p);return;}
 if(now-pending.since>15000){paused=true;event("Paused: gym request timed out; toggle off to resume");clear_pending();return;}
 auto vr=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));PgrVgpResultsResult out{};int rc=vr->poll(pending.promise.get(),1,owner,&out);if(!rc)return;
 if(rc!=PGR_VR_DONE){if(pending.phase==3)history.refuse(pending.defender,now);event("Gym request rejected",rc);clear_pending();next_scan=now+10000;return;}
 Root response;response.h=out.value_handle;
 pending.promise.reset();pending.reflection.reset();
 if(pending.canceled){clear_pending();return;}
 if(pending.phase==6){
  require(response&&klass(response.get())==cls("Holoholo.Rpc","FortSearchOutProto"),"spin response identity");
  bool ok=success(response.get());int result=result_value(response.get());
  int64_t deadline=field<int64_t>(response.get(),"CooldownComplete","System.Int64");
  const int64_t wall=int64_t(clock_ms(CLOCK_REALTIME));
  if(deadline>wall){require(deadline-wall<=86400000,"invalid spin cooldown");history.spin_until(pending.id,now+uint64_t(deadline-wall));
   void *args[]={&deadline};virtual_call(pending.gym,h(H_VGP_M_COOLDOWN),args);
  }
  event(ok?"Gym spun":"Gym spin declined",result);
  if(ok)cooldown("Gym disc spin");
  clear_pending();next_scan=now+1500;return;
 }
 if(pending.phase==1){
  require(response&&klass(response.get())==cls("Niantic.Holoholo","MapPlaceDetails"),"gym details response identity");Root gd=reference(response.get(),"GymDetails","Niantic.Holoholo");
  pending.details=Root(field<void*>(gd.get(),"Status","Holoholo.Rpc.GymStatusAndDefendersProto"));act(p);return;
 }
 const char *name=pending.phase==2?"GymDeployOutProto":"GymFeedPokemonOutProto";require(response&&klass(response.get())==cls("Holoholo.Rpc",name),"gym response identity");
 bool ok=success(response.get());int result=result_value(response.get());int dust=pending.phase==3?field<int>(response.get(),"StardustAwarded","System.Int32"):0;
 if(pending.phase==2){event(ok?"Defender deployed":"Deployment declined",result);if(ok)cooldown("Gym defender deployed");else reserved.erase(pending.defender);}
 else{event(ok?"Gym berry accepted":"Gym berry declined",result,ok?dust:0);if(ok)cooldown("Gym berry feeding");else history.refuse(pending.defender,now);}
 if(!ok){clear_pending();next_scan=now+10000;return;}
 pending.details=Root(field<void*>(response.get(),"GymStatusAndDefenders","Holoholo.Rpc.GymStatusAndDefendersProto"));require(bool(pending.details),"missing updated gym details");
 pending.await_stock=pending.phase==3;pending.since=now;
 // Respect the server's next-feed deadline as well as a conservative interval.
 // The inventory delta must also arrive before another berry is attempted.
 next_scan=now+1500;
 if(pending.await_stock){int64_t deadline=field<int64_t>(response.get(),"CooldownComplete","System.Int64");int64_t wall=int64_t(clock_ms(CLOCK_REALTIME));
  if(deadline>wall){require(deadline-wall<=1800000,"invalid feed cooldown");next_scan=std::max(next_scan,now+uint64_t(deadline-wall));}}
 pending.phase=5;
}
void issue_spin(const Permission &p){
 Root f=fort(pending.gym);Root req=request("FortSearchProto");
 Root id(fn<void*(*)(const char*)>(boot.runtime.string_new)(pending.id.c_str()));
 putref(req.get(),"Id","System.String",id.get());
 put(req.get(),"PlayerLatDegrees","System.Double",p.lat);put(req.get(),"PlayerLngDegrees","System.Double",p.lon);
 put(req.get(),"FortLatDegrees","System.Double",field<double>(f.get(),"Latitude","System.Double"));
 put(req.get(),"FortLngDegrees","System.Double",field<double>(f.get(),"Longitude","System.Double"));
 pending.phase=6;pending.since=clock_ms();
 pending.promise=send(req,"FortSearchOutProto","FortSearch",pending.reflection);
 require(bool(pending.promise),"spin outcome unknown");event("Spinning gym disc");
}
void discover(const Permission &p){
 auto now=clock_ms();for(auto it=inspected.begin();it!=inspected.end();)if(it->second<=now)it=inspected.erase(it);else ++it;
 Root type=type_object(cls("Niantic.Holoholo.Gym","MapGym"));void *args[]={type.get()};Root gyms(invoke(h(H_VGP_M_FIND_ALL),nullptr,args));if(!gyms)return;
 size_t n=fn<size_t(*)(void*)>(exp("array_length"))(gyms.get());require(n<=2048,"gym array bound");
 std::string nearest_reason;double nearest=1e30;bool nearby_waiting=false;
 for(size_t i=0;i<n;i++){Root gym(*reinterpret_cast<void**>(static_cast<char*>(gyms.get())+32+i*8));std::string why;double meters=1e30;
  if(p.settings.spin&&gym_valid(gym,p,nullptr,nullptr,false,true)){
   Root sf=fort(gym);std::string sid=string(field<void*>(sf.get(),"FortId","System.String"));
   if(history.spin_allowed(sid,now)){
    if(spin_bag_room()){
     pending.gym=std::move(gym);pending.id=sid;pending.config=p.config;issue_spin(p);return;
    }else event("Bag full; gym spin waiting");
   }
  }
  if(!p.settings.deploy&&!p.settings.feed)continue;
  if(!gym_valid(gym,p,&why,&meters)){if(meters<nearest){nearest=meters;nearest_reason=why;}continue;}
  Root f=fort(gym);std::string id=string(field<void*>(f.get(),"FortId","System.String"));if(id.empty())continue;
  if(inspected.count(id)){nearby_waiting=true;continue;}if(inspected.size()>=512)continue;
  if(!p.settings.feed&&!history.deploy_allowed(id,clock_ms()))continue;
  pending.gym=std::move(gym);pending.id=id;pending.config=p.config;pending.phase=1;pending.since=clock_ms();
  inspected[id]=now+60000;
  pending.promise=Root(call(pending.gym.get(),"GetGymDetailsFromServer"));require(bool(pending.promise),"gym details promise");event("Reading nearby friendly gym");return;
 }if(p.settings.spin&&!p.settings.deploy&&!p.settings.feed){if(status!="Bag full; gym spin waiting"&&status!="Gym spun")event("Waiting for a nearby gym disc");}
 else if(nearby_waiting){if(status!="Berry reserves reached")event("Waiting to recheck nearby gym");}else if(!nearest_reason.empty())event("Nearest gym: "+nearest_reason+" ("+std::to_string(int(nearest))+"m)");else event("Waiting for a friendly gym within 40m");
}
}
void pgr_gym_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid))return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}} guard;
 try{
  auto p=permission();if(p.valid_config&&!p.settings.deploy&&!p.settings.feed&&!p.settings.spin)paused=false;
  if(!initialized){if(!p.allowed||paused)return;auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(api&&api->snapshot(&boot)==0,"runtime bootstrap");md=metadata;require(md,"shared metadata");owner=tid;validate_bindings();initialized=true;}
  require(fn<void*(*)()>(boot.runtime.thread_current)()!=nullptr,"owner not attached");
  if(p.allowed){Root player=resolve("Niantic.Holoholo","IPlayerService");if(!player){if(pending.phase)pending.canceled=true;return;}std::string id=string(call(player.get(),"get_PlayerId"));if(id.empty())return;
   if(account!=id){clear_pending();account=id;history=History{};inspected.clear();reserved.clear();}}
  if(pending.phase){poll(p);return;}
  if(paused)return;if(!p.allowed){event(p.settings.deploy||p.settings.feed||p.settings.spin?"Waiting for map readiness":"Off");return;}
  auto now=clock_ms();if(now<next_scan)return;next_scan=now+3000;
  discover(p);
 }catch(const Failure &e){paused=true;clear_pending();event(std::string("Paused: ")+e.where);}catch(...){paused=true;clear_pending();event("Paused: gym automation failure");}
}
void pgr_gym_stop(uint64_t tid){if(initialized&&tid==owner&&tid==uint64_t(gettid())){clear_pending();paused=true;}}

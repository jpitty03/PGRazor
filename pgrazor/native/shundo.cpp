#include "shundo.h"
#include "game_bindings.h"
#include "hook_engine.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include "vgp_results.h"
#include <cstdio>
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <string>
#include <unordered_set>
#include <time.h>
#include <unistd.h>
// Shundo Sniper: one teleported-to spawn at a time (see shundo.h). Every call goes through the game's
// own objects: WildMapPokemon.SendEncounterRequest for the encounter, the encounter response the game
// keeps on the map Pokemon for shiny and IVs, the RPC handler for the berry and MapPokemon.TryCapture
// for each throw. Only while the game is active and the map is showing.
namespace {
using namespace pgr_shared_meta;
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
PgrBootBinding boot{};const PgrSharedMetadataOutput *md=nullptr;
bool initialized=false,busy=false,faulted=false;uint64_t owner=0;
uint64_t clock_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
template<class F>F fn(const void *p){require(p,"missing runtime export");return reinterpret_cast<F>(const_cast<void*>(p));}
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
 static std::map<std::string,void*> cache;const std::string key=std::string(ns)+"."+name;auto it=cache.find(key);if(it!=cache.end())return it->second;
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api,"metadata API");
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult out{};require(api->resolve_class(&q,&out)==0,name);return cache[key]=const_cast<void*>(out.klass);
}
std::string typename_of(void *type){char *p=fn<char*(*)(void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
void *method(void *k,const char *name,int count){
 void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,count);require(m,name);
 require(fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(m)==unsigned(count),name);return m;
}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(klass(o),name,n),o,args);}
template<class T>T unbox(void *o){require(o,"null result");T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");std::memcpy(&v,p,sizeof(v));return v;}
template<class T>T at(void *o,size_t off){T v{};std::memcpy(&v,static_cast<char*>(o)+off,sizeof(v));return v;}
void *field_info(void *o,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name);require(f,name);require(!(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10),name);return f;}
std::string field_type(void *o,const char *name){return typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(field_info(o,name)));}
template<class T>T field(void *o,const char *name,const char *type){require(field_type(o,name)==type,name);T v{};fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,field_info(o,name),&v);return v;}
template<class T>void put(void *o,const char *name,const char *type,T value){require(field_type(o,name)==type,name);fn<void(*)(void*,void*,void*)>(boot.runtime.field_set_value)(o,field_info(o,name),&value);}
void put_object(void *o,const char *name,const char *type,void *value){require(field_type(o,name)==type,name);fn<void(*)(void*,void*,void*)>(exp("field_set_value_object"))(o,field_info(o,name),value);}
// An enum field's value plus its enum class, so literal names are read from the game.
int enum_field(void *o,const char *name,void **enum_class){
 void *f=field_info(o,name);void *k=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));
 require(k&&fn<bool(*)(void*)>(exp("class_is_enum"))(k),name);*enum_class=k;int v=0;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;
}
int literal(void *k,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);require((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)==0x8050,name);int n=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&n);return n;}
std::string literal_name(void *k,int value){void *iter=nullptr;
 while(auto f=fn<void*(*)(void*,void**)>(exp("class_get_fields"))(k,&iter)){
  if((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)!=0x8050)continue;int n=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&n);
  if(n==value)return fn<const char*(*)(void*)>(exp("field_get_name"))(f);}
 return "Unknown "+std::to_string(value);}
void *h(unsigned id){return const_cast<void*>(md->opt_handles[id]);}
Root type_object(void *k){return Root(fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k)));}
Root resolve(void *contract){
 Root type=type_object(h(H_IL_MAP));void *args[]={type.get()};Root map(invoke(h(H_IL_FIND_M),nullptr,args));if(!map)return {};
 Root container(invoke(h(H_IL_CONTAINER_M),map.get()));if(!container)return {};
 Root target=type_object(contract);void *resolve_args[]={target.get()};return Root(invoke(h(H_IL_RESOLVE_M),container.get(),resolve_args));
}
std::string quote(const std::string &s){std::string out="\"";for(unsigned char c:s){if(c=='"'||c=='\\')out+='\\';if(c>=32)out+=char(c);}return out+'"';}
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}

struct Config {uint64_t request=0,id=0;bool go=false,running=false,active=false,map=false;};
// "S1|<request>|<encounter id>|<go>|<running>"; request 0 means no spawn to check (running still says
// whether the sniper is on, for the tap block); anything malformed or stale means off.
// go turns 1 once a found shiny can be caught without cooldown; until then the encounter is held.
Config configured(){
 Config c;auto tr=transport();PgrRuntimeTransportSample a{};char s[64]{};uint32_t n=0;
 if(!tr||tr->sample(&a)||!a.snapshot_fresh||a.fault||tr->get_string(19,s,sizeof(s)-1,&n)||n>=sizeof(s))return c;
 s[n]=0;unsigned long long request=0,id=0;int go=0,running=0;char tail=0;
 if(std::sscanf(s,"S1|%llu|%llu|%d|%d%c",&request,&id,&go,&running,&tail)!=4||(go!=0&&go!=1)||(running!=0&&running!=1))return c;
 c.running=running==1;if(!request||!id)return c;
 int active=0,ready=0,map=0;tr->get_bool(3,&active);tr->get_bool(16,&ready);tr->get_bool(38,&map);
 c.request=request;c.id=id;c.go=go==1;c.active=active&&ready;c.map=map!=0;return c;
}

constexpr uint64_t kFindMs=75000,kResponseMs=15000,kBerryMs=10000;
// A search walks every wild Pokemon in the scene on the Unity thread. Short hops show up within 6 s, and after
// a jump the server holds wild Pokemon back for about 30 s, so search slowly in between.
constexpr uint64_t kFindFastMs=750,kFindSlowMs=3000,kFindSlowFromMs=6000,kFindSlowToMs=28000;
// Once wild Pokemon that were not on the map at the teleport have arrived, the requested one gets this
// long to arrive too before the search gives up on it (it is usually in the same batch).
constexpr uint64_t kAfterOthersMs=10000;
// The server sends the new area's wild Pokemon about 30 s after a jump (measured overnight: 136 of 152
// finds at 30-35 s, the rest at 60-65 s). Some Pokemon often show up within a second or two of the
// jump and some vanish again; those come before this point and count as already there, so they never
// start the countdown above.
constexpr uint64_t kServerHoldMs=25000;
constexpr int kMaxThrows=10;
enum Step {IDLE,FIND,ENCOUNTER,HOLD,BERRY,BERRY_WAIT,THROW_WAIT,DONE};
struct State {
 uint64_t request=0,id=0,started=0,since=0,next=0;Step step=IDLE;
 Root target,promise,items;
 // Kept from the moment a shiny is found, so it can still be caught once its map Pokemon is gone.
 Root rpc,spawn,berry_reflection,catch_reflection;void *berry_method=nullptr,*catch_method=nullptr;
 int throws=0,species=0;
 // What the last searches saw, for a "missing" report: searches run and wild Pokemon on the map.
 unsigned scans=0,seen=0;std::string sample;
 // The wild Pokemon on the map before the server's hold ends (the area left behind, and any that show up
 // early) and since when ones not among them are on the map: the new area's spawns have arrived.
 // first_fresh (ms after the teleport, 0 = none) and early (how many showed up after the first search but
 // before the hold ended, early_ms the first of them) are reported, so the wait can be tuned from logs.
 std::unordered_set<uint64_t> before;bool have_before=false;uint64_t fresh_at=0,first_fresh=0,early_ms=0;unsigned fresh=0,early=0;
 // The game's map content handler, asked to fetch map objects right away after a teleport (its own
 // fetch throttle otherwise leaves wild spawns off the map for 30-60 s).
 Root map_content;uint64_t next_refresh=0;unsigned refreshes=0;bool throttle_logged=false;
} st;

void event(const char *phase,const std::string &extra=""){
 auto tr=transport();if(!tr)return;
 const std::string json="{\"request\":"+std::to_string(st.request)+",\"phase\":\""+phase+"\""+extra+"}";
 PgrRuntimeTransportString s[]={{"shundo-sniper",13},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,s,2);
}
// A berry or a throw counts toward the cooldown estimate; the encounter itself does not.
void cooldown(const char *action){auto tr=transport();if(!tr)return;PgrRuntimeTransportString s[]={{action,uint32_t(std::strlen(action))}};tr->emit(17,nullptr,0,s,1);}
void reset(){st.promise.reset();st.target.reset();st.spawn.reset();st.step=IDLE;st.throws=0;st.species=0;st.scans=st.seen=0;st.sample.clear();st.next_refresh=0;st.refreshes=0;st.before.clear();st.have_before=false;st.fresh_at=st.first_fresh=st.early_ms=0;st.fresh=st.early=0;}
void finish(const char *phase,const std::string &message,const std::string &extra=""){
 event(phase,",\"message\":"+quote(message)+extra);st.promise.reset();st.target.reset();st.step=DONE;
}

// Asks the game's MapContentHandler for an immediate map objects fetch (ForceImmediateGetObjects, then
// ForceRefreshVisibleCells, the game's own refresh). The throttle settings are reported once.
void refresh_map(){
 void *handler_class=cls("Niantic.Holoholo.Map","MapContentHandler");
 if(!st.map_content){
  static void *finder=nullptr;if(!finder)finder=method(cls("UnityEngine","Object"),"FindObjectsOfType",2);
  Root type=type_object(handler_class);bool inactive=false;void *args[]={type.get(),&inactive};
  Root list(invoke(finder,nullptr,args));if(!list||at<uintptr_t>(list.get(),0x18)<1)return;
  void *first=at<void*>(list.get(),0x20);if(!first||klass(first)!=handler_class)return;
  st.map_content=Root(first);
 }
 void *handler=st.map_content.get();if(!handler){st.map_content.reset();return;}
 if(!st.throttle_logged){
  st.throttle_logged=true;
  try{Root throttle(field<void*>(handler,"fetchThrottle","Niantic.Holoholo.SpaceTimeThrottle"));
   if(throttle)event("throttle",",\"minIntervalS\":"+std::to_string(field<double>(throttle.get(),"minIntervalS","System.Double"))+
    ",\"maxIntervalS\":"+std::to_string(field<double>(throttle.get(),"maxIntervalS","System.Double"))+
    ",\"minDistanceM\":"+std::to_string(field<double>(throttle.get(),"minDistanceM","System.Double")));
  }catch(const Failure&){}
 }
 bool yes=true;void *a[]={&yes};call(handler,"set_ForceImmediateGetObjects",1,a);
 call(handler,"ForceRefreshVisibleCells");++st.refreshes;
}
// The requested spawn among the active wild map Pokemon, rooted in st.target.
bool find_target(uint64_t now){
 void *wild=cls("Niantic.Holoholo.Map","WildMapPokemon");
 static void *finder=nullptr;if(!finder)finder=method(cls("UnityEngine","Object"),"FindObjectsOfType",2);
 Root type=type_object(wild);bool inactive=true;void *args[]={type.get(),&inactive};
 Root list(invoke(finder,nullptr,args));if(!list)return false;
 const uintptr_t n=at<uintptr_t>(list.get(),0x18);require(n<=5000,"map Pokemon count");
 void *get_id=method(wild,"get_EncounterId",0);
 // The report's ids are the fresh ones when there are any: Java compares them across misses to notice a
 // game that stopped following teleports, and the area left behind can still be listed for a while.
 unsigned seen=0,fresh=0;std::string sample,fresh_sample;
 auto add=[](std::string &list,uint64_t id){list+=(list.empty()?"":",")+std::string("\"")+std::to_string(id)+"\"";};
 for(uintptr_t i=0;i<n;++i){
  void *p=at<void*>(list.get(),0x20+i*8);if(!p||klass(p)!=wild)continue;
  const uint64_t id=unbox<uint64_t>(invoke(get_id,p));
  if(id==st.id){st.target=Root(p);return true;}
  ++seen;if(seen<=6)add(sample,id);
  if(!st.have_before||now-st.started<kServerHoldMs){
   if(st.before.insert(id).second&&st.have_before&&!st.early++)st.early_ms=now-st.started;
  }
  else if(!st.before.count(id)&&++fresh<=6)add(fresh_sample,id);
 }
 st.have_before=true;
 // The countdown starts again if the fresh ones leave the map.
 st.fresh_at=fresh?(st.fresh_at?st.fresh_at:now):0;
 if(fresh&&!st.first_fresh)st.first_fresh=now-st.started;
 st.seen=seen;st.sample=fresh?fresh_sample:sample;st.fresh=fresh;
 return false;
}
// The encounter response the game keeps on the map Pokemon (its only EncounterOutProto field).
void *encounter_response(void *wild){
 static size_t offset=0;
 if(!offset){
  void *iter=nullptr;int found=0;
  while(auto f=fn<void*(*)(void*,void**)>(exp("class_get_fields"))(klass(wild),&iter)){
   if(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10)continue;
   if(typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))!="Holoholo.Rpc.EncounterOutProto")continue;
   offset=fn<size_t(*)(void*)>(exp("field_get_offset"))(f);++found;
  }
  require(found==1&&offset>=0x10&&offset<0x1000,"encounter response field");
 }
 return at<void*>(wild,offset);
}
int poll(Root &value){
 auto vr=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));require(vr,"response polling unavailable");
 PgrVgpResultsResult out{};const int rc=vr->poll(st.promise.get(),PGR_VR_PHASE_ENCOUNTER,owner,&out);value.h=out.value_handle;return rc;
}
int count(int item){void *a[]={&item};const int n=unbox<int32_t>(call(st.items.get(),"GetItemCount",1,a));return n>0?n:0;}
int item_id(const char *name){return literal(cls("Holoholo.Rpc","Item"),name);}

void throw_ball(uint64_t now);
void read_encounter(){
 Root response(encounter_response(st.target.get()));require(bool(response),"encounter response missing");
 void *k=nullptr;const int status=enum_field(response.get(),"Status",&k);const std::string name=literal_name(k,status);
 if(name!="EncounterSuccess"){finish("failed","Encounter refused: "+name);return;}
 Root wild(field<void*>(response.get(),"Pokemon","Holoholo.Rpc.WildPokemonProto"));require(bool(wild),"encounter Pokemon");
 Root pokemon(field<void*>(wild.get(),"Pokemon","Holoholo.Rpc.PokemonProto"));require(bool(pokemon),"encounter Pokemon");
 Root display(call(pokemon.get(),"get_PokemonDisplay"));require(bool(display),"Pokemon display");
 const bool shiny=field<uint8_t>(display.get(),"Shiny","System.Boolean")!=0;
 st.species=unbox<int32_t>(call(pokemon.get(),"get_PokemonId"));
 const int a=unbox<int32_t>(call(pokemon.get(),"get_IndividualAttack")),d=unbox<int32_t>(call(pokemon.get(),"get_IndividualDefense")),s=unbox<int32_t>(call(pokemon.get(),"get_IndividualStamina"));
 const std::string facts=",\"ms\":"+std::to_string(clock_ms()-st.started)+",\"refreshes\":"+std::to_string(st.refreshes)+",\"seen\":"+std::to_string(st.seen)+",\"fresh\":"+std::to_string(st.fresh)+",\"fresh_ms\":"+std::to_string(st.first_fresh)+",\"early\":"+std::to_string(st.early)+",\"early_ms\":"+std::to_string(st.early_ms)+",\"shiny\":"+std::string(shiny?"true":"false")+",\"species\":"+std::to_string(st.species)+",\"iv\":["+std::to_string(a)+","+std::to_string(d)+","+std::to_string(s)+"]";
 if(!shiny){finish("checked","Not shiny",facts);return;}
 // A shiny is held in its encounter until Java says a catch needs no cooldown (go).
 st.rpc=Root(field<void*>(st.target.get(),"rpcHandler","Niantic.Holoholo.Rpc.IRpcHandler"));st.spawn=Root(call(st.target.get(),"get_SpawnPointId"));
 require(st.rpc&&st.spawn,"RPC unavailable");
 event("shiny",facts);st.step=HOLD;
}
// The game's typed RPC handler closed over one request/response pair, made once per pair.
void *rpc_sender(const char *request,const char *response,Root &reflection,void *&closed_method){
 if(closed_method)return closed_method;
 void *open=method(klass(st.rpc.get()),"SendRpc",5);require(fn<bool(*)(void*)>(boot.meta.method_is_generic)(open),"generic RPC");
 Root reflected(fn<void*(*)(void*,void*)>(exp("method_get_object"))(open,klass(st.rpc.get())));
 Root types(fn<void*(*)(void*,uintptr_t)>(boot.runtime.array_new)(cls("System","Type"),2));require(reflected&&types,"RPC reflection");
 void *ks[]={cls("Holoholo.Rpc",request),cls("Holoholo.Rpc",response)};
 for(int i=0;i<2;i++){Root t=type_object(ks[i]);void **slot=reinterpret_cast<void**>(static_cast<char*>(types.get())+32+i*8);fn<void(*)(void*,void**,void*)>(exp("gc_wbarrier_set_field"))(types.get(),slot,t.get());}
 void *args[]={types.get()};reflection=Root(call(reflected.get(),"MakeGenericMethod",1,args));
 void *closed=fn<void*(*)(void*)>(exp("method_get_from_reflection"))(reflection.get());require(closed&&fn<bool(*)(void*)>(boot.meta.method_is_inflated)(closed),"closed RPC");
 return closed_method=closed;
}
Root new_request(const char *type){Root r(fn<void*(*)(void*)>(boot.runtime.object_new)(cls("Holoholo.Rpc",type)));require(bool(r),"request allocation");call(r.get(),".ctor");return r;}
Root send(void *sender,const char *method_name,void *request){
 int id=literal(cls("Holoholo.Rpc","Method"),method_name),zero=0,timeout=10000;void *args[]={&id,request,&zero,&timeout,&zero};
 return Root(invoke(sender,st.rpc.get(),args));
}
// Whether the map Pokemon is still there. A held encounter outlives its spawn timer, but the game takes
// the map Pokemon away when it despawns; then the berry and throws go straight to the game's RPCs.
bool on_map(){return st.target&&!unbox<bool>(call(st.target.get(),"get_Destroyed"));}
// A Golden Razz Berry, else a Razz Berry, before each throw; none on hand goes straight to the throw.
void feed_berry(uint64_t now){
 int berry=item_id("GoldenRazzBerry");if(!count(berry)){berry=item_id("RazzBerry");if(!count(berry))berry=0;}
 if(!berry){throw_ball(now);return;}
 void *sender=rpc_sender("UseItemEncounterProto","UseItemEncounterOutProto",st.berry_reflection,st.berry_method);
 Root request=new_request("UseItemEncounterProto");
 put<int>(request.get(),"Item","Holoholo.Rpc.Item",berry);put<uint64_t>(request.get(),"EncounterId","System.UInt64",st.id);
 put_object(request.get(),"SpawnPointGuid","System.String",st.spawn.get());
 st.promise=send(sender,"UseItemEncounter",request.get());require(bool(st.promise),"berry not sent");
 cooldown("Shundo Sniper berry");st.step=BERRY_WAIT;st.since=now;
}
// One excellent curveball with the best ball on hand: Ultra, Great, then Poke Ball (never a Master Ball).
// Through the map Pokemon's own TryCapture while it is on the map, else the CatchPokemon RPC.
void throw_ball(uint64_t now){
 int ball=0;for(const char *name:{"UltraBall","GreatBall","PokeBall"}){const int id=item_id(name);if(count(id)){ball=id;break;}}
 if(!ball){finish("failed","No Poké, Great or Ultra Balls left");return;}
 if(on_map()){
  uint8_t pitch[12]{};const float reticle=0.1f;std::memcpy(pitch,&ball,4);std::memcpy(pitch+4,&reticle,4);pitch[8]=1;pitch[9]=1;
  void *args[]={pitch,nullptr};st.promise=Root(call(st.target.get(),"TryCapture",2,args));
 }else{
  void *sender=rpc_sender("CatchPokemonProto","CatchPokemonOutProto",st.catch_reflection,st.catch_method);
  Root request=new_request("CatchPokemonProto");
  put<uint64_t>(request.get(),"EncounterId","System.UInt64",st.id);
  put<int>(request.get(),"Pokeball","System.Int32",ball);
  put<double>(request.get(),"NormalizedReticleSize","System.Double",1.95);
  put_object(request.get(),"SpawnPointGuid","System.String",st.spawn.get());
  put<bool>(request.get(),"HitPokemon","System.Boolean",true);
  put<double>(request.get(),"SpinModifier","System.Double",0.85);
  put<double>(request.get(),"NormalizedHitPosition","System.Double",1.0);
  st.promise=send(sender,"CatchPokemon",request.get());
 }
 require(bool(st.promise),"throw not sent");
 cooldown("Shundo Sniper throw");++st.throws;st.step=THROW_WAIT;st.since=now;
}
void read_catch(Root &response,uint64_t now){
 void *k=nullptr;const int v=enum_field(response.get(),"Status",&k);const std::string name=literal_name(k,v);
 const std::string throws=std::to_string(st.throws)+(st.throws==1?" throw":" throws");
 if(name=="CatchSuccess"){finish("caught","Caught after "+throws,",\"species\":"+std::to_string(st.species));return;}
 if((name=="CatchEscape"||name=="CatchMissed")&&st.throws<kMaxThrows){st.step=BERRY;st.next=now+1200;return;}
 if(name=="CatchEscape"||name=="CatchMissed"||name=="CatchFlee"){finish("fled",(name=="CatchFlee"?"Fled after ":"Still free after ")+throws,",\"species\":"+std::to_string(st.species));return;}
 finish("failed","Catch refused: "+name);
}
void step(const Config &c,uint64_t now){
 if(st.step==FIND){
  const std::string facts=",\"ms\":"+std::to_string(now-st.started)+",\"scans\":"+std::to_string(st.scans)+",\"seen\":"+std::to_string(st.seen)+",\"fresh\":"+std::to_string(st.fresh)+",\"fresh_ms\":"+std::to_string(st.first_fresh)+",\"early\":"+std::to_string(st.early)+",\"early_ms\":"+std::to_string(st.early_ms)+",\"ids\":["+st.sample+"]";
  if(now-st.started>kFindMs){finish("missing","Not on the map here; it may have despawned",facts);return;}
  if(st.fresh_at&&now-st.fresh_at>=kAfterOthersMs){finish("missing","Other Pokémon here appeared, this one did not",facts);return;}
  if(!c.map||now<st.next)return;
  {const uint64_t elapsed=now-st.started;st.next=now+(elapsed>=kFindSlowFromMs&&elapsed<kFindSlowToMs?kFindSlowMs:kFindFastMs);}
  // After a jump the server leaves wild Pokemon out of map objects for about 30 s however often the game
  // asks (measured: found at 31-33 s with refreshes every 3 s or every 10 s). So one refresh 2 s after the
  // teleport (for a short hop), then one at 30 s and every 3 s after, instead of the game's 10-30 s throttle.
  // A refresh that fails is reported and not tried again for this spawn.
  if(!st.next_refresh)st.next_refresh=st.started+2000;
  else if(now>=st.next_refresh){
   st.next_refresh=now-st.started<30000?st.started+30000:now+3000;
   try{refresh_map();}catch(const Failure &f){event("refresh-failed",",\"message\":"+quote(f.where));st.next_refresh=UINT64_MAX;}
  }
  ++st.scans;if(!find_target(now))return;
  st.promise=Root(call(st.target.get(),"SendEncounterRequest"));require(bool(st.promise),"encounter not sent");
  st.step=ENCOUNTER;st.since=now;return;
 }
 if(st.step==ENCOUNTER||st.step==BERRY_WAIT||st.step==THROW_WAIT){
  Root value;const int rc=poll(value);
  const uint64_t limit=st.step==BERRY_WAIT?kBerryMs:kResponseMs;
  if(rc==PGR_VR_PENDING&&now-st.since<=limit)return;
  if(st.step==ENCOUNTER){
   st.promise.reset();
   if(rc!=PGR_VR_DONE){finish("failed",rc==PGR_VR_PENDING?"No encounter answer":"Encounter failed");return;}
   read_encounter();return;
  }
  if(st.step==BERRY_WAIT){st.promise.reset();throw_ball(now);return;}  // the throw goes ahead either way
  st.promise.reset();
  if(rc!=PGR_VR_DONE||!value){finish("failed",rc==PGR_VR_PENDING?"No answer to the throw":"Throw failed");return;}
  read_catch(value,now);return;
 }
 if(st.step==HOLD){if(!c.go)return;st.step=BERRY;st.next=now;}
 if(st.step==BERRY&&now>=st.next){
  if(!st.items)st.items=resolve(cls("Niantic.Holoholo","IItemBag"));require(bool(st.items),"item bag unavailable");
  feed_berry(now);
 }
}

// While the sniper runs, the player's own taps on map Pokemon, PokeStops and gyms do nothing: the map's
// selection entry points (MapExploreState.PokemonSelected / PokestopSelected / GymSelected) return
// without running. Camera drags, the joystick and menus are untouched. Each hook is pinned by address and
// prologue for this game version; one that does not match is simply not installed.
std::atomic<bool> blocking{false};
std::atomic<void*> orig_pokemon{nullptr},orig_stop{nullptr},orig_gym{nullptr};
std::atomic<uint64_t> last_blocked{0},last_tick_ms{0};
// Blocks only while ticks are arriving, so a stopped host can never leave the player's taps blocked.
bool block_now(){return blocking.load(std::memory_order_acquire)&&clock_ms()-last_tick_ms.load(std::memory_order_acquire)<2000;}
bool hooks_tried=false;
void note_blocked(const char *what){
 const uint64_t now=clock_ms(),prev=last_blocked.load(std::memory_order_relaxed);
 if(now-prev<2000)return;last_blocked.store(now,std::memory_order_relaxed);
 event("blocked",std::string(",\"what\":\"")+what+"\"");
}
void hook_pokemon_selected(void *self,void *pokemon,void *flag,const void *m){
 if(block_now()){note_blocked("pokemon");return;}
 reinterpret_cast<void(*)(void*,void*,void*,const void*)>(orig_pokemon.load(std::memory_order_acquire))(self,pokemon,flag,m);
}
void hook_pokestop_selected(void *self,void *stop,void *done,void *flag,const void *m){
 if(block_now()){note_blocked("pokestop");return;}
 reinterpret_cast<void(*)(void*,void*,void*,void*,const void*)>(orig_stop.load(std::memory_order_acquire))(self,stop,done,flag,m);
}
void hook_gym_selected(void *self,void *gym,void *flag,void *done,void *context,const void *m){
 if(block_now()){note_blocked("gym");return;}
 reinterpret_cast<void(*)(void*,void*,void*,void*,void*,const void*)>(orig_gym.load(std::memory_order_acquire))(self,gym,flag,done,context,m);
}
bool pin(void *owner,const char *name,unsigned params,uint64_t rva,const uint8_t *prologue,void *replacement,std::atomic<void*> &original){
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));if(!api)return false;
 PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=owner;q.name=name;q.return_type="System.Void";q.return_match=PGR_MD_MATCH_EXACT;
 q.param_count=params;q.expected_code=static_cast<const char*>(boot.module_base)+rva;q.prologue=prologue;q.prologue_len=24;
 PgrMetaMethodResult r{};if(api->validate_method(&q,&r)!=0||!r.code)return false;
 void *o=nullptr;if(pgrazor_hook(const_cast<void*>(r.code),replacement,&o)!=0||!o)return false;
 original.store(o,std::memory_order_release);return true;
}
void install_block(){
 hooks_tried=true;
 void *state=nullptr;try{state=cls("Niantic.Holoholo.Map","MapExploreState");}catch(const Failure&){}
 if(!state){event("block-unavailable",",\"message\":\"Map taps not blocked: map state not found\"");return;}
 static const uint8_t p_pokemon[]=PGR_GAME_SHUNDO_CPP_POKEMON_SELECTED_FINGERPRINT,p_stop[]=PGR_GAME_SHUNDO_CPP_POKESTOP_SELECTED_FINGERPRINT,p_gym[]=PGR_GAME_SHUNDO_CPP_GYM_SELECTED_FINGERPRINT;
 static_assert(sizeof(p_pokemon)==24&&sizeof(p_stop)==24&&sizeof(p_gym)==24,"24-byte prologues");
 std::string missing;
 if(!pin(state,"PokemonSelected",2,PGR_GAME_SHUNDO_CPP_POKEMON_SELECTED_RVA,p_pokemon,reinterpret_cast<void*>(hook_pokemon_selected),orig_pokemon))missing+="Pokémon ";
 if(!pin(state,"PokestopSelected",3,PGR_GAME_SHUNDO_CPP_POKESTOP_SELECTED_RVA,p_stop,reinterpret_cast<void*>(hook_pokestop_selected),orig_stop))missing+="PokéStops ";
 if(!pin(state,"GymSelected",4,PGR_GAME_SHUNDO_CPP_GYM_SELECTED_RVA,p_gym,reinterpret_cast<void*>(hook_gym_selected),orig_gym))missing+="gyms ";
 if(!missing.empty())event("block-unavailable",",\"message\":\"Taps not blocked on: "+missing+"\"");
}
}  // namespace

void pgr_shundo_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||faulted||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid))return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 const Config c=configured();const uint64_t now=clock_ms();
 try{
  blocking.store(c.running&&!faulted,std::memory_order_release);last_tick_ms.store(now,std::memory_order_release);
  if(!c.request&&!c.running){if(st.step!=IDLE){reset();st.request=0;}return;}
  if(!initialized){
   auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);
   require(api&&api->snapshot(&boot)==0,"runtime unavailable");md=metadata;require(md,"shared metadata");owner=tid;initialized=true;
  }
  require(fn<void*(*)()>(boot.runtime.thread_current)()!=nullptr,"owner thread not attached");
  if(c.running&&!hooks_tried)install_block();
  if(!c.request){if(st.step!=IDLE){reset();st.request=0;}return;}
  if(c.request!=st.request){reset();st.request=c.request;st.id=c.id;st.step=FIND;st.started=now;st.next=0;event("looking");}
  if(st.step==DONE||st.step==IDLE)return;
  // A throw already sent is followed to its answer; nothing new starts off the map or in the background.
  if(!c.active&&st.step!=ENCOUNTER&&st.step!=BERRY_WAIT&&st.step!=THROW_WAIT)return;
  step(c,now);
 }catch(const Failure &f){
  finish("error",std::string("Shundo Sniper: ")+f.where);
 }catch(...){faulted=true;st.promise.reset();st.target.reset();event("error",",\"message\":\"Shundo Sniper stopped after an unexpected error\"");}
}
void pgr_shundo_stop(uint64_t tid){
 blocking.store(false,std::memory_order_release);
 if(!initialized||tid!=owner||tid!=uint64_t(gettid()))return;
 reset();st.items.reset();st.rpc.reset();st.map_content.reset();st.berry_reflection.reset();st.catch_reflection.reset();st.berry_method=st.catch_method=nullptr;st.request=0;
}

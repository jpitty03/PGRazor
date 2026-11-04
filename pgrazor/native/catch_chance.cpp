#include "catch_chance.h"
#include "game_bindings.h"
#include "shared_metadata.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <time.h>
#include <unistd.h>
// Catch chance and auto ball for manual encounters. encounter_host's existing
// EncounterInteractionState.EnterState/ExitState hooks (the berry feature's) report the live
// encounter through pgr_catch_chance_enter/exit. Each tick reads the server's per-ball capture probabilities
// (CaptureProbabilityProto, the numbers the game colors the ring with), the active berry, the
// selected ball and the ball counts, and applies the berry, throw and curve multipliers:
//   chance = 1 - (1 - p_ball) ^ (berry * throw * curve)
// p_ball already carries the type medal bonus (verified 2026-11-04: Normal/Flying with both medals
// Gold gave exactly 1-(1-BCR/(2*CPM))^1.3), so no medal term is added. With auto ball on it selects, through the game's own
// SelectPokeball, the cheapest of Poke/Great/Ultra that reaches the target (else the best one
// owned): once when the encounter opens and again after each escape, so a ball the player picks
// by hand is kept until the next escape. It never selects a Master Ball.
namespace {
using namespace pgr_shared_meta;
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
template<class F>F fn(void *p){require(p,"missing runtime export");return reinterpret_cast<F>(p);}
PgrBootBinding boot{};const PgrSharedMetadataOutput *md=nullptr;
uint64_t owner=0;bool busy=false,ready=false;
std::atomic<bool> installed{false},faulted{false};
std::atomic<uintptr_t> live{0};       // GC handle of the open encounter's state; owner thread writes
std::atomic<uint64_t> generation{0};  // bumped by each EnterState
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
std::string typename_of(const void *type){char *p=fn<char*(*)(const void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
void *cls(const char *ns,const char *name){
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api,"metadata API");
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult out{};require(api->resolve_class(&q,&out)==0,name);return const_cast<void*>(out.klass);
}
void *method(void *o,const char *name,int count){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(klass(o),name,count);require(m,name);return m;}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(o,name,n),o,args);}
template<class T>T unbox(void *o){require(o,"boxed value");T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");std::memcpy(&v,p,sizeof(v));return v;}
// An object field of any reference type (class or generic instance).
void *ref_field(void *o,const char *name){
 void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name);require(f,name);
 const int kind=fn<int(*)(const void*)>(exp("type_get_type"))(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));require(kind==0x12||kind==0x15,name);
 void *v=nullptr;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;}
void *h(unsigned id){return const_cast<void*>(md->opt_handles[id]);}
Root type_object(void *k){return Root(fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k)));}
Root resolve(const char *ns,const char *name){
 Root type=type_object(h(H_IL_MAP));void *args[]={type.get()};Root map(invoke(h(H_IL_FIND_M),nullptr,args));if(!map)return {};
 Root container(invoke(h(H_IL_CONTAINER_M),map.get()));if(!container)return {};
 Root target=type_object(cls(ns,name));void *resolve_args[]={target.get()};return Root(invoke(h(H_IL_RESOLVE_M),container.get(),resolve_args));
}
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
void event(const std::string &json){auto tr=transport();if(!tr)return;PgrRuntimeTransportString strings[]={{"catch-chance",12},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,strings,2);}
void status(const char *phase,const std::string &message){event(std::string("{\"phase\":\"")+phase+"\",\"message\":\""+message+"\"}");}
uint64_t clock_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}

struct Config {bool valid=false,show=false,autoball=false;int target=75;int grade=0;bool curve=false;int berry=0;};
// "C1|show|auto|target|berry" (berry: what PGRazor's berry picker feeds on release) plus the throw settings (throw allowed bool 29, curve bool 34, grade int 7).
Config config(){
 Config c;auto tr=transport();PgrRuntimeTransportSample a{};if(!tr||tr->sample(&a)||!a.snapshot_fresh||a.fault)return c;
 char s[32];uint32_t n=0;if(tr->get_string(15,s,sizeof(s),&n)||n<8||n>=sizeof(s)||std::memcmp(s,"C1|",3)!=0)return c;s[n]=0;
 int show=0,autoball=0,target=0,berry=0;if(std::sscanf(s+3,"%d|%d|%d|%d",&show,&autoball,&target,&berry)<3||target<1||target>99)return c;
 c.valid=true;c.show=show==1;c.autoball=autoball==1;c.target=target;c.berry=berry>=701&&berry<=799?berry:0;
 int allowed=0,curve=0;int32_t grade=0;
 if(tr->get_bool(29,&allowed)==0&&allowed==1){if(tr->get_int(7,&grade)==0&&grade>=0&&grade<=3)c.grade=grade;if(tr->get_bool(34,&curve)==0)c.curve=curve==1;}
 return c;
}
double berry_multiplier(int item){return item==701?1.5:item==706?2.5:item==708?1.8:1.0;}
// Throw multiplier is 2 minus the circle size; these are the middle of each throw's range.
double throw_multiplier(int grade){return grade==3?1.85:grade==2?1.5:grade==1?1.15:1.0;}
const char *grade_name(int grade){return grade==3?"Excellent":grade==2?"Great":grade==1?"Nice":"";}

void *select_method=nullptr;  // EncounterInteractionState.SelectPokeball(Holoholo.Rpc.Item)
void install(){
 void *k=cls("Niantic.Holoholo.Encounter","EncounterInteractionState");
 // The game's ball-picker action: the SelectPokeball overload taking the ball's item id.
 void *iter=nullptr;std::string select;
 while(void *m=fn<void*(*)(void*,void**)>(exp("class_get_methods"))(k,&iter)){
  if(std::strcmp(fn<const char*(*)(void*)>(exp("method_get_name"))(m),"SelectPokeball")!=0||fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(m)!=1)continue;
  const std::string t=typename_of(fn<const void*(*)(void*,unsigned)>(exp("method_get_param"))(m,0));select+=(select.empty()?"":",")+t;
  if(t=="Holoholo.Rpc.Item")select_method=m;
 }
 static std::string why;why="SelectPokeball(Item) not found; have "+select;require(select_method,why.c_str());
 installed.store(true);status("ready","Catch chance ready");
}

// One open encounter as last seen.
struct Seen {uint64_t generation=0;bool in_play=false,decided=false;std::string json;int selected=0,current=0;};
Seen seen;Root item_bag;
int item_count(int item){
 if(!item_bag)item_bag=resolve("Niantic.Holoholo","IItemBag");if(!item_bag)return -1;
 void *args[]={&item};return unbox<int>(call(item_bag.get(),"GetItemCount",1,args));
}
int boxed_item(void *o){if(!o)return 0;return unbox<int>(o);}
void step(){
 const uintptr_t handle=live.load();
 const Config c=config();
 void *state=handle?fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(handle):nullptr;
 if(!state||!c.valid||(!c.show&&!c.autoball)){
  if(!seen.json.empty()){seen=Seen{};event("{\"phase\":\"encounter\",\"active\":false}");}
  return;
 }
 const uint64_t gen=generation.load();
 if(gen!=seen.generation){seen=Seen{};seen.generation=gen;}
 void *probability=call(state,"get_CaptureProbability");if(!probability)return;  // not loaded yet
 void *types=ref_field(probability,"PokeballType"),*values=ref_field(probability,"CaptureProbability");if(!types||!values)return;
 const int n=unbox<int>(call(types,"get_Count"));require(n>=0&&n<=16&&unbox<int>(call(values,"get_Count"))==n,"capture probability count");
 // A berry already in the game wins; otherwise the one PGRazor's picker will feed on release.
 int berry=0;{void *b=call(state,"get_ActiveBerry");if(b&&fn<bool(*)(void*)>(exp("class_is_enum"))(klass(b)))berry=boxed_item(b);}
 // A fed berry is already in the server's rates (feeding a Razz moved Growlithe's Poke rate from
 // .2763 to exactly 1-(1-.2763)^1.5), so only a berry still to be fed adds its multiplier.
 const bool fed=berry>0;
 if(berry<=0)berry=c.berry;
 // The ball object is replaced between throws; keep the last ball seen while there is none.
 int current=0;if(void *ball=call(state,"get_Pokeball"))current=boxed_item(call(ball,"get_BallType"));
 if(current>0)seen.current=current;else current=seen.current;
 const bool in_play=unbox<bool>(call(state,"get_PokeballInPlay"));
 const double mult=(fed?1.0:berry_multiplier(berry))*throw_multiplier(c.grade)*(c.curve?1.7:1.0);
 // Poke, Great and Ultra the server priced for this encounter, cheapest first.
 std::string balls;int pick=0,best=0;double best_chance=-1;
 for(int item=1;item<=3;item++){
  for(int i=0;i<n;i++){
   int index=i;void *ia[]={&index};const int type=boxed_item(call(types,"get_Item",1,ia));if(type!=item)continue;
   const float base=unbox<float>(call(values,"get_Item",1,ia));if(!(base>0&&base<=1))break;
   const double chance=1-std::pow(1-double(base),mult);const int count=item_count(item);
   char row[160];std::snprintf(row,sizeof(row),"%s{\"item\":%d,\"base\":%.4f,\"chance\":%.4f,\"count\":%d}",balls.empty()?"":",",item,double(base),chance,count);balls+=row;
   if(count>0){if(!pick&&chance*100>=c.target)pick=item;if(chance>best_chance){best_chance=chance;best=item;}}
   break;
  }
 }
 if(!pick)pick=best;
 // Auto ball: at the start of the encounter and after each escape (a ball landed and came back).
 const bool escaped=seen.in_play&&!in_play;seen.in_play=in_play;
 if(escaped)seen.decided=false;
 if(c.autoball&&!in_play&&!seen.decided&&pick&&pick!=4&&current>0){
  seen.decided=true;
  if(pick!=current){int item=pick;void *args[]={&item};invoke(select_method,state,args);current=pick;seen.current=pick;seen.selected=pick;}
 }
 if(!c.autoball)seen.decided=false;
 char head[256];std::snprintf(head,sizeof(head),"{\"phase\":\"encounter\",\"active\":true,\"show\":%s,\"auto\":%s,\"target\":%d,\"current\":%d,\"pick\":%d,\"berry\":%d,\"throw\":\"%s\",\"curve\":%s,\"balls\":[",
  c.show?"true":"false",c.autoball?"true":"false",c.target,current,pick,berry,grade_name(c.grade),c.curve?"true":"false");
 const std::string json=std::string(head)+balls+"]}";
 if(json!=seen.json){seen.json=json;event(json);}
}
}  // namespace

void pgr_catch_chance_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid)||faulted.load())return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;owner=tid;md=metadata;
 try{
  if(!ready){auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(md&&api&&api->snapshot(&boot)==0,"Runtime unavailable");ready=true;}
  if(!installed.load()){const Config c=config();if(!c.valid||(!c.show&&!c.autoball))return;install();}
 }catch(const Failure &f){faulted.store(true);status("error",std::string("Catch chance unavailable: ")+f.where);return;}
 catch(...){faulted.store(true);status("error","Catch chance unavailable: native failure");return;}
 // A read that fails (an encounter still loading, an unusual encounter type) skips this tick and
 // is reported once per encounter; it does not turn the feature off.
 static uint64_t reported=~0ull;
 try{step();}
 catch(const Failure &f){if(reported!=generation.load()){reported=generation.load();status("skipped",f.where);}}
 catch(...){if(reported!=generation.load()){reported=generation.load();status("skipped","native failure");}}
}
void pgr_catch_chance_enter(void *state){
 if(!ready||!state)return;
 uintptr_t handle=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(state,0);if(!handle)return;
 if(uintptr_t old=live.exchange(handle))fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(old);
 generation.fetch_add(1);
}
void pgr_catch_chance_exit(void *state){
 if(!ready)return;
 uintptr_t current=live.load();
 if(current&&fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(current)==state&&live.compare_exchange_strong(current,0))
  fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(current);
}
bool pgr_catch_chance_base(void *encounter_out,double base[4]){
 if(!base)return false;for(int i=0;i<4;i++)base[i]=0;
 // Needs only the runtime snapshot; a failed manual SelectPokeball install does not matter here.
 if(!ready||busy||!encounter_out||!owner||owner!=uint64_t(gettid()))return false;
 try{
  void *probability=ref_field(encounter_out,"CaptureProbability");if(!probability)return false;
  void *types=ref_field(probability,"PokeballType"),*values=ref_field(probability,"CaptureProbability");if(!types||!values)return false;
  const int n=unbox<int>(call(types,"get_Count"));if(n<0||n>16||unbox<int>(call(values,"get_Count"))!=n)return false;
  bool any=false;
  for(int i=0;i<n;i++){
   int index=i;void *ia[]={&index};const int type=boxed_item(call(types,"get_Item",1,ia));if(type<1||type>3)continue;
   const float value=unbox<float>(call(values,"get_Item",1,ia));if(value>0&&value<=1){base[type]=value;any=true;}
  }
  return any;
 }catch(...){for(int i=0;i<4;i++)base[i]=0;return false;}
}
void pgr_catch_chance_stop(){
 if(owner&&owner!=uint64_t(gettid()))return;
 if(uintptr_t h=live.exchange(0))fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);
}

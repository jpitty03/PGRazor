#include "route_gift.h"
#include "route_gift_policy.h"
#include "game_bindings.h"
#include "hook_engine.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include "shared_metadata.h"
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

// Exchange gifts with Mateo. The game runs the exchange itself as an NPC encounter: his conversation
// (NpcDialogueGuiController), the "Exchange gift?" question (NpcTrainerInterstitialGui), the gift card
// (SendGiftGuiController) and his gift (OpenGiftGuiController). Hooks on NpcEncounterState.EnterState /
// ExitState tell us which encounter is running; only one whose steps include a gift exchange and no
// Pokemon trade is helped, so Team GO Rocket and other characters that share these screens are never
// pressed. Each press is the screen's own button handler, made on the Unity owner thread.
namespace {
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
template<class F>F fn(void *p){require(p,"missing runtime export");return reinterpret_cast<F>(p);}
PgrBootBinding boot{};
uint64_t owner=0;bool busy=false,ready=false,installed=false;
std::atomic<bool> faulted{false};
uint64_t clock_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
void *exp(const char *name){std::string n="il2cpp_";n+=name;void *p=dlsym(boot.handle,n.c_str());require(p,name);return p;}
uintptr_t root(void *o){return o?fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(o,0):0;}
void *target(uintptr_t h){return h?fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h):nullptr;}
void unroot(uintptr_t &h){if(h){fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);h=0;}}
struct Root {
 uintptr_t h=0;Root()=default;explicit Root(void *v):h(root(v)){}
 Root(const Root&)=delete;Root& operator=(const Root&)=delete;~Root(){unroot(h);}
 void *get()const{return target(h);}explicit operator bool()const{return get()!=nullptr;}
};
void *klass(void *o){require(o,"null object");return fn<void*(*)(void*)>(boot.runtime.object_get_class)(o);}
void *cls(const char *ns,const char *name){
 static std::map<std::string,void*> cache;std::string key=std::string(ns)+"."+name;auto it=cache.find(key);if(it!=cache.end())return it->second;
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api,"metadata API");
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult out{};require(api->resolve_class(&q,&out)==0,name);return cache[key]=const_cast<void*>(out.klass);
}
std::string typename_of(void *type){char *p=fn<char*(*)(void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
void *field_info(void *k,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);require(!(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10),name);return f;}
template<class T>T read(void *o,void *f){T v{};fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;}
void *method(void *k,const char *name,int count){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,count);require(m,name);return m;}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(klass(o),name,n),o,args);}
template<class T>T unbox(void *o){require(o,"boxed value");T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");std::memcpy(&v,p,sizeof(v));return v;}
// The only instance field of k whose type is exactly type_name.
void *field_of_type(void *k,const char *type_name){
 void *found=nullptr,*iter=nullptr;
 while(void *f=fn<void*(*)(void*,void**)>(exp("class_get_fields"))(k,&iter)){
  if(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10)continue;
  if(typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))!=type_name)continue;
  require(!found,type_name);found=f;
 }
 require(found,type_name);return found;
}
int enum_literal(void *k,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);require((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)==0x8050,name);int n=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&n);return n;}

const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
void event(const std::string &json){auto tr=transport();if(!tr||!tr->emit)return;PgrRuntimeTransportString s[]={{"route-gift",10},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,s,2);}
void status(const char *phase,const std::string &message){event(std::string("{\"phase\":\"")+phase+"\",\"message\":\""+message+"\"}");}
bool configured(){
 auto tr=transport();if(!tr||!tr->sample||!tr->get_string)return false;PgrRuntimeTransportSample a{};
 if(tr->sample(&a)||!a.snapshot_fresh||a.fault)return false;
 char s[16];uint32_t n=0;bool on=false;
 return tr->get_string(24,s,sizeof(s),&n)==0&&n<sizeof(s)&&pgr_route_gift::parse_config(s,n,on)&&on;
}

// --- Hooks: which NPC encounter is running ----------------------------------------------
using Enter=void(*)(void*,void*,void*,const void*);
using Exit=void(*)(void*,const void*);
std::atomic<void*> orig_enter{nullptr},orig_exit{nullptr};
// Written by the hooks and read by the tick, both on the Unity owner thread.
uintptr_t state_handle=0;uint64_t state_serial=0;bool state_exited=false;
void hook_enter(void *self,void *api,void *data,const void *m){
 reinterpret_cast<Enter>(orig_enter.load(std::memory_order_acquire))(self,api,data,m);
 try{unroot(state_handle);state_handle=root(self);++state_serial;state_exited=false;}catch(...){}
}
void hook_exit(void *self,const void *m){
 try{if(state_handle&&target(state_handle)==self)state_exited=true;}catch(...){}
 reinterpret_cast<Exit>(orig_exit.load(std::memory_order_acquire))(self,m);
}
void *validate_hook(void *k,const char *name,int params,uint64_t rva,const uint8_t *bytes){
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api&&api->validate_method,"metadata validate");
 PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=k;q.name=name;q.return_type="System.Void";q.return_match=PGR_MD_MATCH_EXACT;
 q.param_count=params;q.expected_code=static_cast<const char*>(boot.module_base)+rva;q.prologue=bytes;q.prologue_len=24;
 PgrMetaMethodResult r{};require(api->validate_method(&q,&r)==0&&r.code,name);return const_cast<void*>(r.code);
}
void install(){
 void *state=cls("Niantic.Holoholo.NpcEncounter","NpcEncounterState");
 static const uint8_t b_enter[]=PGR_GAME_ROUTE_GIFT_CPP_NPC_ENTER_BYTES,b_exit[]=PGR_GAME_ROUTE_GIFT_CPP_NPC_EXIT_BYTES;
 static_assert(sizeof(b_enter)==24&&sizeof(b_exit)==24,"24-byte prologues");
 void *enter=validate_hook(state,"EnterState",2,PGR_GAME_ROUTE_GIFT_CPP_NPC_ENTER_RVA,b_enter);
 void *exit=validate_hook(state,"ExitState",0,PGR_GAME_ROUTE_GIFT_CPP_NPC_EXIT_RVA,b_exit);
 void *o=nullptr;require(pgrazor_hook(enter,reinterpret_cast<void*>(hook_enter),&o)==0&&o,"EnterState hook");orig_enter.store(o,std::memory_order_release);
 o=nullptr;require(pgrazor_hook(exit,reinterpret_cast<void*>(hook_exit),&o)==0&&o,"ExitState hook");orig_exit.store(o,std::memory_order_release);
 installed=true;
}

// --- Encounter and screens ---------------------------------------------------------------
pgr_route_gift::State st;
uint64_t seen_serial=0;     // the encounter last classified
uint64_t last_scan=0;
bool reported_give_up=false;

// Event values of the encounter's steps, read from the game's own proto (no guessed numbers).
pgr_route_gift::EncounterKind classify_proto(void *proto,std::string &steps);
pgr_route_gift::EncounterKind classify(void *state,std::string &steps){
 void *proto=read<void*>(state,field_of_type(klass(state),"Holoholo.Rpc.NpcEncounterProto"));
 if(!proto)return pgr_route_gift::ENC_OTHER;
 return classify_proto(proto,steps);
}
pgr_route_gift::EncounterKind classify_proto(void *proto,std::string &steps){
 Root keep(proto);
 // Protobuf property getters are stripped from the game: read the RepeatedField's own fields.
 void *list=read<void*>(proto,field_info(klass(proto),"Steps"));if(!list)return pgr_route_gift::ENC_OTHER;
 Root keep_list(list);
 const int count=read<int>(list,field_info(klass(list),"count"));require(count>=0&&count<=64,"step count");
 void *items=read<void*>(list,field_info(klass(list),"array"));require(items||!count,"step array");
 Root keep_items(items);
 require(!count||fn<size_t(*)(void*)>(exp("array_length"))(items)>=size_t(count),"step array length");
 std::vector<int> events;int gift=-1,trade=-1;
 for(int i=0;i<count;++i){
  void *step=reinterpret_cast<void**>(static_cast<char*>(keep_items.get())+32)[i];if(!step){events.push_back(0);continue;}
  void *ev=read<void*>(step,field_info(klass(step),"Event"));if(!ev){events.push_back(0);continue;}
  void *f=field_info(klass(ev),"Event");void *k=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));
  require(k&&fn<bool(*)(void*)>(exp("class_is_enum"))(k),"NPC event type");
  if(gift<0){gift=enum_literal(k,"GiftExchange");trade=enum_literal(k,"PokemonTrade");}
  const int v=read<int>(ev,f);events.push_back(v);
  if(!steps.empty())steps+=",";steps+=std::to_string(v);
 }
 return pgr_route_gift::classify(events.data(),int(events.size()),gift,trade);
}

bool alive(void *o){
 if(!o)return false;void *args[]={o};
 static void *m=nullptr;if(!m)m=method(cls("UnityEngine","Object"),"op_Implicit",1);
 return unbox<bool>(invoke(m,nullptr,args));
}
// The open instance of a screen class, if any (GuiController.currentState 2 = open).
uintptr_t open_screen(void *k){
 static void *find=nullptr,*state_field=nullptr;
 if(!find)find=method(cls("UnityEngine","Resources"),"FindObjectsOfTypeAll",1);
 if(!state_field)state_field=field_info(cls("Niantic.Holoholo.NUI","GuiController"),"currentState");
 void *type=fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k));
 void *args[]={type};Root arr(invoke(find,nullptr,args));if(!arr)return 0;
 const size_t n=fn<size_t(*)(void*)>(exp("array_length"))(arr.get());
 for(size_t i=0;i<n&&i<16;++i){
  void *o=reinterpret_cast<void**>(static_cast<char*>(arr.get())+32)[i];
  if(o&&alive(o)&&read<int32_t>(o,state_field)==2)return root(o);
 }
 return 0;
}

// Starting the encounter: the same MapPokestop.OnTap a tap on Mateo makes. OnTap begins the stop's NPC
// encounter only while the stop shows its NPC (StyleGuideline.ShowNPC) and the encounter has a valid
// character; otherwise it opens the stop, so both are checked first, with the player in range.
uint64_t last_start_scan=0;
std::map<std::string,uint64_t> tapped;  // encounter id -> last tap
std::string last_tapped;
constexpr uint64_t kDone=~uint64_t(0);    // a tapped encounter whose gift was sent is never tapped again
bool map_ready(){
 auto tr=transport();int ready=0;
 return tr&&tr->get_bool&&tr->get_bool(16,&ready)==0&&ready==1;
}
std::string text(void *s){
 if(!s)return {};const int n=fn<int(*)(void*)>(exp("string_length"))(s);if(n<=0||n>256)return {};
 const uint16_t *p=fn<const uint16_t*(*)(void*)>(boot.runtime.string_chars)(s);std::string out;
 for(int i=0;i<n;++i)out+=p[i]<128?char(p[i]):'?';return out;
}
void *style_method(void *stop_class){
 static void *m=nullptr;if(m)return m;void *iter=nullptr;
 while(void *x=fn<void*(*)(void*,void**)>(exp("class_get_methods"))(stop_class,&iter)){
  if(fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(x)!=0)continue;
  const std::string r=typename_of(fn<void*(*)(void*)>(exp("method_get_return_type"))(x));
  if(r.size()>=14&&r.compare(r.size()-14,14,"StyleGuideline")==0){require(!m,"one style method");m=x;}
 }
 require(m,"stop style method");return m;
}
bool try_start(uint64_t now){
 using namespace pgr_route_gift;
 if(now-last_start_scan<2000||!map_ready())return false;
 last_start_scan=now;
 void *stop_class=cls("Niantic.Holoholo.Pokestop","MapPokestop");
 static void *find=nullptr;if(!find)find=method(cls("UnityEngine","Object"),"FindObjectsOfType",1);
 void *type=fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(stop_class));
 void *args[]={type};Root arr(invoke(find,nullptr,args));if(!arr)return false;
 const size_t n=fn<size_t(*)(void*)>(exp("array_length"))(arr.get());
 for(size_t i=0;i<n&&i<512;++i){
  Root stop(reinterpret_cast<void**>(static_cast<char*>(arr.get())+32)[i]);
  if(!stop||!alive(stop.get()))continue;
  Root helper(call(stop.get(),"get_MapPlaceNpcHelper"));if(!helper)continue;
  Root proto(call(helper.get(),"get_NpcEncounterProto"));if(!proto)continue;
  std::string steps;if(classify_proto(proto.get(),steps)!=ENC_GIFT)continue;
  const std::string id=text(read<void*>(proto.get(),field_info(klass(proto.get()),"EncounterId")));
  auto seen=tapped.find(id);if(seen!=tapped.end()&&(seen->second==kDone||now-seen->second<60000))continue;
  const char *why=nullptr;
  if(!unbox<bool>(call(stop.get(),"get_IsPlayerInRange")))why="out of range";
  else if(!unbox<bool>(call(helper.get(),"get_DoesNpcEncounterProtoHaveValidCharacter")))why="no valid character";
  else{Root style(invoke(style_method(stop_class),stop.get()));
   if(!style||!read<bool>(style.get(),field_info(klass(style.get()),"ShowNPC")))why="not shown on the stop";}
  if(why){
   static std::string last_skip;const std::string key=id+why;
   if(key!=last_skip){last_skip=key;event(std::string("{\"phase\":\"waiting\",\"reason\":\"")+why+"\"}");}
   continue;
  }
  tapped[id]=now;last_tapped=id;
  void *tap_args[]={nullptr,nullptr};call(stop.get(),"OnTap",2,tap_args);
  status("starting","Starting Mateo's gift exchange");
  return true;
 }
 return false;
}

void step(uint64_t now){
 using namespace pgr_route_gift;
 void *state=target(state_handle);
 if(st.active&&(state_exited||!state)){
  if(st.presses[SC_SEND]&&!last_tapped.empty())tapped[last_tapped]=kDone;
  status("finished",st.presses[SC_SEND]?"Gift exchange with Mateo finished":"Mateo's encounter ended");
  st.end();reported_give_up=false;
 }
 if(state&&!state_exited&&seen_serial!=state_serial){
  seen_serial=state_serial;std::string steps;
  const EncounterKind kind=classify(state,steps);
  event(std::string("{\"phase\":\"encounter\",\"gift\":")+(kind==ENC_GIFT?"true":"false")+",\"steps\":["+steps+"]}");
  if(kind==ENC_GIFT){st.begin(int64_t(now));status("exchanging","Exchanging gifts with Mateo");}
 }
 if(!st.active){if(!state||state_exited)try_start(now);return;}
 if(now-last_scan<250)return;last_scan=now;
 static const char *const names[SC_COUNT][2]={
  {"Niantic.Holoholo.UI.Conversations","NpcDialogueGuiController"},
  {"Niantic.Holoholo.Combat.NpcTrainers","NpcTrainerInterstitialGui"},
  {"Niantic.Holoholo.Social.Gifting.UI","SendGiftGuiController"},
  {"Niantic.Holoholo.Social.Gifting.UI","OpenGiftGuiController"}};
 static const char *const buttons[SC_COUNT]={"ClickedToContinue","PressChallenge","OnSendClick","OnOpenClick"};
 uintptr_t screens[SC_COUNT]={};bool open[SC_COUNT]={};
 for(int s=0;s<SC_COUNT;++s){screens[s]=open_screen(cls(names[s][0],names[s][1]));open[s]=screens[s]!=0;}
 const Action act=decide(st,true,open,int64_t(now));
 if(act!=ACT_NONE){
  void *screen=target(screens[act]);
  try{if(screen&&alive(screen))call(screen,buttons[act]);event(std::string("{\"phase\":\"pressed\",\"button\":\"")+buttons[act]+"\"}");}
  catch(const Failure &f){event(std::string("{\"phase\":\"press-failed\",\"button\":\"")+buttons[act]+"\",\"why\":\""+f.where+"\"}");}
 }
 for(auto &h:screens)unroot(h);
 if(st.gave_up&&!reported_give_up){reported_give_up=true;status("stopped",st.why);}
}
}  // namespace

void pgr_route_gift_tick(uint64_t tid,const PgrSharedMetadataOutput *){
 if(busy||!tid||tid!=uint64_t(gettid())||faulted.load(std::memory_order_acquire))return;
 if(owner&&owner!=tid)return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 owner=tid;
 const bool on=configured();
 if(!on){if(st.active){st.end();status("off","Mateo gift exchange turned off");}return;}
 try{
  if(!ready){auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(api&&api->snapshot(&boot)==0,"Runtime unavailable");ready=true;}
  if(!installed){install();status("ready","Mateo gift exchange ready");}
  step(clock_ms());
 }catch(const Failure &f){faulted.store(true,std::memory_order_release);st.end();status("error",std::string("Mateo gift exchange stopped: ")+f.where);}
 catch(...){faulted.store(true,std::memory_order_release);st.end();status("error","Mateo gift exchange stopped: native failure");}
}
void pgr_route_gift_stop(){st.end();}

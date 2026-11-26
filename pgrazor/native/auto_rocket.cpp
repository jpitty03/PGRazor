#include "auto_rocket.h"
#include "auto_rocket_policy.h"
#include "heal.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include "vgp_controller.h"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

// Auto Rocket. Each battle is the game's own flow, pressed through its own button handlers:
//   MapPokestop.OnTap -> (grunt dialogue, skipped by Skip cutscenes or pressed here)
//   -> NpcTrainerInterstitialGui.PressChallenge -> CombatPokemonPartyWidget.SubmitButtonPressed
//   -> the battle (Rocket auto attack / shield / swap fight it)
//   -> IncidentCombatResolveGuiController: a win's Close() starts the shadow encounter (Skip cutscenes
//      does the same); a loss presses DoneButtonClicked
//   -> the shadow encounter (Auto throw throws the Premier Balls; Java turns it on for the session)
//   -> the catch summary, then the next stop.
// Everything is resolved by name with reflection; no hooks and no fixed addresses. Managed objects
// are rooted only within a tick.
namespace {
using namespace pgr_auto_rocket;

struct Failure {std::string where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
template<class F>F fn(const void *p){require(p!=nullptr,"missing runtime export");return reinterpret_cast<F>(const_cast<void*>(p));}

PgrBootBinding boot{};
uint64_t owner=0;bool busy=false,ready=false;

int64_t clock_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return int64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
int64_t wall_ms(){timespec t{};clock_gettime(CLOCK_REALTIME,&t);return int64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
void *exp(const char *name){std::string n="il2cpp_";n+=name;void *p=dlsym(boot.handle,n.c_str());require(p,name);return p;}
uintptr_t root(void *o){return o?fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(o,0):0;}
void *target(uintptr_t h){return h?fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h):nullptr;}
void unroot(uintptr_t &h){if(h){fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);h=0;}}
struct Root {
 uintptr_t h=0;Root()=default;explicit Root(void *v):h(root(v)){}
 Root(const Root&)=delete;Root& operator=(const Root&)=delete;
 Root(Root&&o)noexcept:h(o.h){o.h=0;}
 Root& operator=(Root&&o)noexcept{if(this!=&o){unroot(h);h=o.h;o.h=0;}return *this;}
 ~Root(){unroot(h);}
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
std::string text(void *s){
 if(!s)return {};const int n=fn<int(*)(void*)>(exp("string_length"))(s);if(n<=0||n>256)return {};
 const uint16_t *p=fn<const uint16_t*(*)(void*)>(boot.runtime.string_chars)(s);std::string out;
 for(int i=0;i<n;++i)out+=p[i]<128?char(p[i]):'?';return out;
}
bool alive(void *o){
 if(!o)return false;void *args[]={o};
 static void *m=nullptr;if(!m)m=method(cls("UnityEngine","Object"),"op_Implicit",1);
 return unbox<bool>(invoke(m,nullptr,args));
}
void *type_object(void *k){return fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k));}
// Live (active) objects of a class, rooted array.
Root find_active(void *k){
 static void *find=nullptr;if(!find)find=method(cls("UnityEngine","Object"),"FindObjectsOfType",1);
 void *args[]={type_object(k)};return Root(invoke(find,nullptr,args));
}
size_t array_length(void *a){return a?fn<size_t(*)(void*)>(exp("array_length"))(a):0;}
void *element(void *a,size_t i){return reinterpret_cast<void**>(static_cast<char*>(a)+32)[i];}
int32_t gui_state(void *gui){
 static void *f=nullptr;if(!f)f=field_info(cls("Niantic.Holoholo.NUI","GuiController"),"currentState");
 return read<int32_t>(gui,f);
}
// The first instance of a screen class in the given GuiController states (1 opening, 2 open).
Root screen(void *k,bool opening_too=false){
 static void *find=nullptr;if(!find)find=method(cls("UnityEngine","Resources"),"FindObjectsOfTypeAll",1);
 void *args[]={type_object(k)};Root arr(invoke(find,nullptr,args));if(!arr)return Root();
 const size_t n=array_length(arr.get());
 for(size_t i=0;i<n&&i<16;++i){
  void *o=element(arr.get(),i);if(!o||!alive(o))continue;
  const int32_t s=gui_state(o);
  if(s==2||(opening_too&&s==1))return Root(o);
 }
 return Root();
}
// The first live, active object of a class (widgets that are not screens of their own).
Root live(void *k){
 Root arr=find_active(k);if(!arr)return Root();
 const size_t n=array_length(arr.get());
 for(size_t i=0;i<n&&i<16;++i){void *o=element(arr.get(),i);if(o&&alive(o))return Root(o);}
 return Root();
}

// --- Transport ------------------------------------------------------------------------------
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
struct Permission {bool coherent=false,empty=false,valid=false,foreground=false,map_ready=false,coords=false;double lat=0,lon=0;Command command;};
Permission read_permission(){
 Permission p;auto tr=transport();
 if(!tr||!tr->sample||!tr->get_string||!tr->get_bool||!tr->get_double)return p;
 PgrRuntimeTransportSample a{},z{};if(tr->sample(&a)||!a.snapshot_fresh||a.fault)return p;
 char raw[256]{};uint32_t n=0;int fg=0,map=0,hl=0,ho=0;double lat=0,lon=0;
 if(tr->get_string(12,raw,sizeof(raw),&n)||n>=sizeof(raw))return p;
 if(tr->get_bool(3,&fg)||tr->get_bool(16,&map)||tr->get_double(0,&lat,&hl)||tr->get_double(1,&lon,&ho))return p;
 if(tr->sample(&z)||!z.snapshot_fresh||z.fault||a.snapshot_seq!=z.snapshot_seq)return p;
 p.coherent=true;p.foreground=fg==1;p.map_ready=map==1;
 p.coords=hl==1&&ho==1&&valid_coordinates(lat,lon);p.lat=lat;p.lon=lon;
 if(!n){p.empty=true;return p;}
 p.valid=parse_command(raw,n,p.command);
 return p;
}

// --- Session --------------------------------------------------------------------------------
enum class Stage {Idle,Scan,Travel,Heal,Open,Challenge,Party,Battle,Resolve,Encounter,Settle};
const char *stage_name(Stage s){
 switch(s){case Stage::Scan:return "scan";case Stage::Travel:return "travel";case Stage::Heal:return "heal";case Stage::Open:return "open";
  case Stage::Challenge:return "challenge";case Stage::Party:return "party";case Stage::Battle:return "battle";case Stage::Resolve:return "resolve";
  case Stage::Encounter:return "encounter";case Stage::Settle:return "settle";default:return "idle";}
}
constexpr int64_t kHeartbeatMs=4500,kScanMs=1500,kStepMs=250,kArriveM=30,kTapRetryMs=6000,kOpenTimeoutMs=30000;
constexpr int64_t kPartyTimeoutMs=30000,kBattleTimeoutMs=420000,kResolveTimeoutMs=60000,kEncounterTimeoutMs=300000;
constexpr int64_t kSettleMs=2500,kHealWaitMs=30000,kLossSkipMs=1800000,kSkipMs=600000,kLongTravelSlackMs=180000;
constexpr int64_t kWinCloseMs=6000,kLossDoneMs=2500,kSummaryTimeoutMs=60000,kSummaryGraceMs=2000,kSpinnerGraceMs=12000;
constexpr int64_t kBeatenSkipMs=24LL*60*60*1000;  // a beaten invasion is never battled again
constexpr int64_t kVgpFinishMs=15000;  // longest wait at a stop for VGP+ to finish its catch or spin
// The server refuses to start an invasion ("try again") until it has our position at the stop, and the
// grunt then arrives seconds late. Walk right up to it and stand still briefly before tapping.
constexpr int64_t kArriveCloseM=10,kArriveMaxWaitMs=12000,kArriveSettleMs=3000;
constexpr int64_t kLateGruntMs=45000;  // a grunt that appears after we gave up on its stop is still battled

struct Session {
 bool active=false,stop_requested=false;
 Command command;
 Stage stage=Stage::Idle;
 int64_t stage_ms=0,last_step=0,last_scan=0,last_heartbeat=0,last_tap=0,travel_deadline=0,spinner_ms=0;
 uint64_t travel_seq=0;
 bool has_anchor=false,returned_to_anchor=false;double anchor_lat=0,anchor_lon=0;
 // VGP+ catches and spins while walking between stops; closed on arrival once it is idle.
 bool vgp_closed=false;int64_t arrive_ms=0,close_ms=0;
 Candidate abandoned;int64_t abandoned_ms=0;  // the stop given up on last, in case its grunt shows up late
 Candidate target;int taps=0,submits=0;
 bool won=false,acted=false,encounter_seen=false;int64_t resolve_open_ms=0,summary_ms=0,encounter_closed_ms=0;
 int battles=0,losses_in_row=0;
 std::string phase,message;
};
Session session;
uint64_t finished_token=0;
std::map<std::string,int64_t> quarantine;  // invasion key -> skip until (boot clock); kept across sessions
DisplayTypes display_types;bool display_types_ready=false;

void emit(const std::string &json){
 auto tr=transport();if(!tr||!tr->emit)return;
 PgrRuntimeTransportString s[]={{"auto-rocket",11},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,s,2);
}
std::string quote(const std::string &s){std::string o="\"";for(unsigned char c:s){if(c=='"'||c=='\\')o+='\\';if(c>=32)o+=char(c);}return o+"\"";}
void status(const char *phase,const std::string &message,const std::string &extra=""){
 if(!session.command.token)return;
 emit("{\"token\":"+quote(std::to_string(session.command.token))+",\"phase\":"+quote(phase)+",\"message\":"+quote(message)+
      ",\"completed\":"+std::to_string(session.battles)+extra+"}");
 session.phase=phase;session.message=message;session.last_heartbeat=clock_ms();
}
void diag(const char *phase,const std::string &detail){emit("{\"token\":"+quote(std::to_string(session.command.token))+",\"phase\":"+quote(phase)+",\"detail\":"+quote(detail)+"}");}
void heartbeat(int64_t now){
 if(!session.active||now-session.last_heartbeat<kHeartbeatMs)return;
 status(session.phase=="travel"?"walking":session.phase.empty()?"scanning":session.phase.c_str(),session.message.empty()?"Auto Rocket active":session.message);
}
std::string battles_text(int n){return std::to_string(n)+(n==1?" Rocket battle":" Rocket battles");}
void finish(const char *phase,const std::string &message){
 if(session.command.token){finished_token=std::max(finished_token,session.command.token);status(phase,message);}
 session=Session{};
}
void enter(Stage s,int64_t now){session.stage=s;session.stage_ms=now;session.taps=0;session.submits=0;session.last_tap=0;session.spinner_ms=0;}
// Back to looking for the next stop; the stop session is stopped there if Stop was pressed.
void next(int64_t now,const std::string &why){
 if(session.command.max_battles>0&&session.battles>=session.command.max_battles){finish("done","Finished "+battles_text(session.battles));return;}
 if(session.stop_requested){finish("stopped","Stopped after "+battles_text(session.battles));return;}
 session.target=Candidate{};session.won=session.acted=session.encounter_seen=false;
 session.resolve_open_ms=session.summary_ms=session.encounter_closed_ms=0;
 session.vgp_closed=false;session.arrive_ms=session.close_ms=0;
 enter(Stage::Settle,now);if(!why.empty())status("next",why);
}

// --- Game reads -----------------------------------------------------------------------------
struct FortFields {void *id,*lat,*lon,*displays,*display,*list_array,*list_count,*inc_id,*inc_end,*inc_hide,*inc_done,*inc_type;};
FortFields &fort_fields(){
 static FortFields f{};static bool ok=false;if(ok)return f;
 void *fort=cls("Holoholo.Rpc","PokemonFortProto"),*disp=cls("Holoholo.Rpc","PokestopIncidentDisplayProto");
 f.id=field_info(fort,"FortId");f.lat=field_info(fort,"Latitude");f.lon=field_info(fort,"Longitude");
 f.displays=field_info(fort,"PokestopDisplays");f.display=field_info(fort,"PokestopDisplay");
 void *list=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f.displays));require(list,"incident list");
 f.list_array=field_info(list,"array");f.list_count=field_info(list,"count");
 f.inc_id=field_info(disp,"IncidentId");f.inc_end=field_info(disp,"IncidentExpirationMs");f.inc_hide=field_info(disp,"HideIncident");
 f.inc_done=field_info(disp,"IncidentCompleted");f.inc_type=field_info(disp,"IncidentDisplayType");
 if(!display_types_ready){
  void *k=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f.inc_type));require(k,"incident type enum");
  display_types.grunt=enum_literal(k,"InvasionGrunt");display_types.grunt_b=enum_literal(k,"InvasionGruntb");
  display_types.leader=enum_literal(k,"InvasionLeader");display_types.giovanni=enum_literal(k,"InvasionGiovanni");display_types_ready=true;
 }
 ok=true;return f;
}
// The stop's current invasion: the first shown, unfinished, unexpired display.
bool read_invasion(void *fort,int64_t wall,Candidate &c){
 FortFields &f=fort_fields();
 auto take=[&](void *d)->bool{
  if(!d)return false;
  const int32_t type=read<int32_t>(d,f.inc_type);const int64_t end=read<int64_t>(d,f.inc_end);
  if(read<bool>(d,f.inc_hide)||read<bool>(d,f.inc_done)||end<=wall)return false;
  const Kind k=classify(type,display_types);if(k==KIND_NONE)return false;
  c.kind=k;c.end_ms=end;c.key=text(read<void*>(fort,f.id))+"|"+text(read<void*>(d,f.inc_id));return true;
 };
 if(void *list=read<void*>(fort,f.displays)){
  void *arr=read<void*>(list,f.list_array);int32_t n=read<int32_t>(list,f.list_count);
  if(arr&&n>0){if(size_t(n)>array_length(arr))n=int32_t(array_length(arr));for(int32_t i=0;i<n&&i<16;++i)if(take(element(arr,i)))return true;}
 }
 return take(read<void*>(fort,f.display));
}
// Invaded stops the game has loaded nearby. When `want` is given, returns that stop rooted.
Root scan(std::vector<Candidate> *out,const std::string *want){
 void *stop_class=cls("Niantic.Holoholo.Pokestop","MapPokestop");
 Root arr=find_active(stop_class);if(!arr)return Root();
 const int64_t wall=wall_ms();FortFields &f=fort_fields();
 const size_t n=array_length(arr.get());
 for(size_t i=0;i<n&&i<600;++i){
  Root stop(element(arr.get(),i));
  if(!stop||!alive(stop.get()))continue;
  try{
   if(!unbox<bool>(call(stop.get(),"get_IsIncidentActive")))continue;
   Root fort(call(stop.get(),"get_FortData"));if(!fort)continue;
   Candidate c;if(!read_invasion(fort.get(),wall,c))continue;
   c.fort_id=text(read<void*>(fort.get(),f.id));c.lat=read<double>(fort.get(),f.lat);c.lon=read<double>(fort.get(),f.lon);
   if(want&&*want==c.key)return stop;
   if(out)out->push_back(c);
  }catch(const Failure&){continue;}
 }
 return Root();
}
bool open_named(const char *ns,const char *name,const char *button){
 Root s=screen(cls(ns,name));if(!s)return false;call(s.get(),button);return true;
}
// Conversation screens that can appear between steps: a skipped-or-not grunt line, the first-shadow
// purification tutorial and similar. Each is pressed through once per tick.
bool press_conversations(){
 bool pressed=false;
 try{if(open_named("Niantic.Holoholo.UI.Conversations","NpcDialogueGuiController","ClickedToContinue"))pressed=true;}catch(const Failure&){}
 try{if(open_named("Niantic.Holoholo.UI.Conversations","ConversationLayoutGui","ConvoTap"))pressed=true;}catch(const Failure&){}
 return pressed;
}
bool encounter_open(){
 Root gui=live(cls("Niantic.Holoholo.Encounter","EncounterGuiController"));
 return gui&&unbox<bool>(call(gui.get(),"get_IsOpen"));
}
bool battle_open(){return bool(live(cls("Niantic.Holoholo.Combat.UI","CombatVNextGuiController")));}
bool vgp_busy(){
 auto api=static_cast<const PgrVgpControllerApi*>(pgrazor_vgp_controller_api(1));
 if(!api||!api->sample)return false;
 PgrVgpControllerSample sample{};sample.version=1;sample.size=sizeof(sample);
 return api->sample(&sample)==0&&sample.phase!=PGR_VGC_PH_NONE;
}
// A leader battle uses a Rocket Radar, and the game asks first in its generic modal ("Use" / "Cancel",
// ModalButton.UseCancelButtons = OK, Cancel). Only on a leader stop, once per dialog: its own
// ButtonPressed(OK) is what tapping "Use" does.
bool press_radar_prompt(){
 if(session.target.kind!=KIND_LEADER)return false;
 static void *modal=nullptr,*base=nullptr,*clicked=nullptr,*title=nullptr,*pressed=nullptr;static bool is_screen=false;static int32_t ok=0;
 if(!modal){
  void *k=cls("Niantic.Holoholo.UI","ModalDialog");base=cls("Niantic.Holoholo.UI","BaseModalDialog");
  clicked=field_info(base,"buttonClicked");title=field_info(base,"titleText");pressed=method(base,"ButtonPressed",1);
  ok=enum_literal(cls("Niantic.Holoholo.UI","ModalEvent"),"OK");
  is_screen=fn<bool(*)(void*,void*)>(boot.meta.class_is_assignable_from)(cls("Niantic.Holoholo.NUI","GuiController"),k);
  modal=k;
 }
 Root s=is_screen?screen(modal):live(modal);
 if(!s||read<bool>(s.get(),clicked))return false;
 std::string heading;
 try{if(void *t=read<void*>(s.get(),title))heading=text(call(t,"get_text"));}catch(const Failure&){}
 int32_t event=ok;void *args[]={&event};invoke(pressed,s.get(),args);
 diag("radar-prompt",heading.empty()?"pressed Use":"pressed Use: "+heading);
 status("radar","Using a Rocket Radar for the leader");
 return true;
}
Root resolve_screen(){return screen(cls("Niantic.Holoholo.Incident","IncidentCombatResolveGuiController"),true);}

// --- Stages ---------------------------------------------------------------------------------
// The grunt of a stop we just gave up on can still arrive (a slow start after "try again"). Its screens
// then cover the map; battle it rather than walking off with them open.
bool adopt_late_grunt(int64_t now){
 if(!session.abandoned_ms||session.stop_requested)return false;
 if(now-session.abandoned_ms>kLateGruntMs){session.abandoned_ms=0;return false;}
 if(!screen(cls("Niantic.Holoholo.Combat.NpcTrainers","NpcTrainerInterstitialGui"))&&
    !screen(cls("Niantic.Holoholo.Pokestop","PokestopIncidentBackgroundGUIController"),true))return false;
 session.target=session.abandoned;session.abandoned_ms=0;quarantine.erase(session.target.key);
 session.vgp_closed=true;
 status("opening","The Rocket grunt appeared after all; battling it");enter(Stage::Open,now);
 return true;
}
void step_scan(const Permission &p,int64_t now){
 if(adopt_late_grunt(now))return;
 if(now-session.last_scan<kScanMs)return;session.last_scan=now;
 if(!p.map_ready){if(session.message!="Waiting for the map")status("waiting","Waiting for the map");return;}
 if(session.stop_requested){finish("stopped","Stopped after "+battles_text(session.battles));return;}
 if(press_conversations())return;
 std::vector<Candidate> found;scan(&found,nullptr);
 for(auto it=quarantine.begin();it!=quarantine.end();)if(it->second<=now)it=quarantine.erase(it);else ++it;
 const int pick=select_target(found,p.lat,p.lon,session.command.targets,quarantine,now,wall_ms(),session.command.walk,
                              session.has_anchor,session.anchor_lat,session.anchor_lon,session.command.radius_m);
 if(pick<0){
  if(session.command.radius_m>0&&session.has_anchor&&!session.returned_to_anchor&&
     distance_m(p.lat,p.lon,session.anchor_lat,session.anchor_lon)>200){
   session.returned_to_anchor=true;session.target=Candidate{};session.target.lat=session.anchor_lat;session.target.lon=session.anchor_lon;
   ++session.travel_seq;char buf[96];std::snprintf(buf,sizeof(buf),",\"travel\":%llu,\"latitude\":%.7f,\"longitude\":%.7f",(unsigned long long)session.travel_seq,session.anchor_lat,session.anchor_lon);
   status("travel","Going back to the start to look for Rocket stops",buf);
   session.travel_deadline=now+int64_t(distance_m(p.lat,p.lon,session.anchor_lat,session.anchor_lon)/0.8*1000)+kLongTravelSlackMs;
   enter(Stage::Travel,now);return;
  }
  const std::string msg=found.empty()?"No Team GO Rocket stops nearby; waiting":"No reachable Rocket battles nearby; waiting";
  if(session.message!=msg)status("waiting",msg);
  return;
 }
 session.returned_to_anchor=false;
 session.target=found[size_t(pick)];session.vgp_closed=false;session.arrive_ms=session.close_ms=0;
 const double d=distance_m(p.lat,p.lon,session.target.lat,session.target.lon);
 const std::string who=session.target.kind==KIND_LEADER?"a Rocket leader":"a Rocket grunt";
 if(d<=kArriveM){status("arrived","At "+who);session.travel_deadline=now+kLongTravelSlackMs;enter(Stage::Travel,now);return;}
 ++session.travel_seq;
 char buf[96];std::snprintf(buf,sizeof(buf),",\"travel\":%llu,\"latitude\":%.7f,\"longitude\":%.7f",(unsigned long long)session.travel_seq,session.target.lat,session.target.lon);
 status("travel",std::string(session.command.walk?"Walking to ":"Going to ")+who+" ("+std::to_string(int(d))+" m)",buf);
 session.travel_deadline=now+(session.command.walk?int64_t(d/0.8*1000):0)+kLongTravelSlackMs;
 enter(Stage::Travel,now);
}
void step_travel(const Permission &p,int64_t now){
 if(adopt_late_grunt(now))return;
 if(session.stop_requested){finish("stopped","Stopped after "+battles_text(session.battles));return;}
 const bool to_anchor=session.target.key.empty();
 const double d=distance_m(p.lat,p.lon,session.target.lat,session.target.lon);
 // The deadline covers getting there; waiting out a cooldown at the stop is bounded by the invasion's end.
 if(d>kArriveM&&now>session.travel_deadline){
  if(!to_anchor)quarantine[session.target.key]=now+kSkipMs;
  next(now,"Could not reach the stop in time; trying another");return;
 }
 if(!to_anchor&&session.target.end_ms>0&&session.target.end_ms-wall_ms()<kArrivalMarginMs){
  quarantine[session.target.key]=now+kSkipMs;next(now,"That Rocket stop ends too soon; trying another");return;
 }
 if(to_anchor){if(d<=kArriveM)enter(Stage::Scan,now);return;}
 if(now-session.last_scan>=3000){  // the invasion can end or be beaten by someone's visit while walking
  session.last_scan=now;
  if(p.map_ready&&!scan(nullptr,&session.target.key)&&d<=400){quarantine[session.target.key]=now+kSkipMs;next(now,"That Rocket stop is gone; looking for another");return;}
 }
 if(d>kArriveM)return;
 // Let a VGP+ catch or spin in progress finish before the stop is opened; closing the window on an
 // idle tick revokes VGP+ before it can start anything else (encounter_host ticks it after us).
 if(!session.vgp_closed){
  if(!session.arrive_ms)session.arrive_ms=now;
  if(!session.close_ms&&(d<=kArriveCloseM||now-session.arrive_ms>=kArriveMaxWaitMs))session.close_ms=now;
  if(!session.close_ms||now-session.close_ms<kArriveSettleMs){
   if(session.message!="Arriving at the stop")status("arrived","Arriving at the stop");return;
  }
  if(session.command.walk&&vgp_busy()&&now-session.close_ms<kVgpFinishMs){
   if(session.message!="Waiting for VGP+ to finish")status("arrived","Waiting for VGP+ to finish");return;
  }
  session.vgp_closed=true;
 }
 if(session.command.cooldown_ms>0){if(session.message!="Waiting for cooldown")status("cooldown","Waiting for cooldown");return;}
 if(!p.map_ready)return;
 enter(Stage::Heal,now);
}
void step_heal(int64_t now){
 if(pgr_heal_busy()&&now-session.stage_ms<kHealWaitMs){if(session.message!="Healing before the battle")status("healing","Healing before the battle");return;}
 enter(Stage::Open,now);
}
void step_open(const Permission &p,int64_t now){
 if(press_conversations()){session.stage_ms=now;return;}
 if(press_radar_prompt()){session.stage_ms=now;session.spinner_ms=0;return;}
 if(Root s=screen(cls("Niantic.Holoholo.Combat.NpcTrainers","NpcTrainerInterstitialGui"))){
  call(s.get(),"PressChallenge");status("challenge","Starting the Rocket battle");enter(Stage::Party,now);return;
 }
 // The game opens the plain PokéStop screen first even for an invaded stop (Quick spin spins it), then
 // the grunt's screens follow. Only when that screen stays up with no grunt for a while has the tap
 // opened a stop without a battle: close it and move on.
 if(Root spinner=screen(cls("Niantic.Holoholo.Pokestop","PokestopSpinnerInteractionGuiController"))){
  if(screen(cls("Niantic.Holoholo.Pokestop","PokestopIncidentBackgroundGUIController"),true)){session.spinner_ms=0;return;}
  if(!session.spinner_ms)session.spinner_ms=now;
  if(now-session.spinner_ms<kSpinnerGraceMs)return;
  call(spinner.get(),"ClickExit");quarantine[session.target.key]=now+kSkipMs;
  session.abandoned=session.target;session.abandoned_ms=now;
  next(now,"That stop has no Rocket battle now; trying another");return;
 }
 session.spinner_ms=0;
 if(now-session.stage_ms>kOpenTimeoutMs){
  quarantine[session.target.key]=now+kSkipMs;session.abandoned=session.target;session.abandoned_ms=now;
  next(now,"The Rocket stop did not open; trying another");return;
 }
 if(session.taps>=3||now-session.last_tap<kTapRetryMs||!p.map_ready)return;
 Root stop=scan(nullptr,&session.target.key);
 if(!stop){quarantine[session.target.key]=now+kSkipMs;next(now,"That Rocket stop is gone; looking for another");return;}
 if(!unbox<bool>(call(stop.get(),"get_IsIncidentActiveAndInRange"))){
  if(session.taps==0&&now-session.stage_ms>8000){quarantine[session.target.key]=now+kSkipMs;next(now,"The Rocket stop is out of range; trying another");}
  return;
 }
 void *args[]={nullptr,nullptr};call(stop.get(),"OnTap",2,args);
 ++session.taps;session.last_tap=now;status("opening","Opening the Rocket stop");
}
void step_party(int64_t now){
 if(battle_open()){status("battle","Battling");enter(Stage::Battle,now);return;}
 if(press_conversations())return;
 if(press_radar_prompt()){session.stage_ms=now;return;}
 if(now-session.stage_ms>kPartyTimeoutMs){
  // A party that cannot start (fainted Pokemon, nothing healed) would fail every stop the same way.
  // The party screen is left open: its own Cancel asks "Exit the Trainer Battle?", which has no
  // handler to press here, so the player backs out (or heals and battles) themselves.
  quarantine[session.target.key]=now+kSkipMs;
  finish("stopped","Stopped: the battle party could not start. Heal or revive your Pokémon, then start again");return;
 }
 if(session.submits>=3||now-session.last_tap<5000)return;
 Root w=live(cls("Niantic.Holoholo.Combat.PokemonPartyState","CombatPokemonPartyWidget"));if(!w)return;
 call(w.get(),"SubmitButtonPressed");++session.submits;session.last_tap=now;status("party","Using the battle party");
}
void step_battle(int64_t now){
 if(resolve_screen()){enter(Stage::Resolve,now);session.resolve_open_ms=0;session.acted=false;return;}
 if(encounter_open()){session.won=true;session.losses_in_row=0;status("catching","Catching the shadow Pokémon");enter(Stage::Encounter,now);return;}
 if(now-session.stage_ms>kBattleTimeoutMs){finish("error","Stopped: the Rocket battle did not finish");return;}
}
void step_resolve(int64_t now){
 if(encounter_open()){session.won=true;session.losses_in_row=0;status("catching","Catching the shadow Pokémon");enter(Stage::Encounter,now);return;}
 Root gui=resolve_screen();
 if(!gui){
  if(!session.acted&&now-session.stage_ms<kResolveTimeoutMs)return;  // between the screen and the encounter
  if(session.won)return enter(Stage::Encounter,now);
  next(now,"");return;
 }
 if(press_conversations())return;
 static void *finish_state=nullptr;
 if(!finish_state)finish_state=field_of_type(cls("Niantic.Holoholo.Incident","IncidentCombatResolveGuiController"),"Holoholo.Rpc.CombatPlayerFinishState");
 const bool winner=read<int32_t>(gui.get(),finish_state)==0;
 const int32_t state=gui_state(gui.get());
 if(state==2&&!session.resolve_open_ms)session.resolve_open_ms=now;
 if(winner){
  session.won=true;session.losses_in_row=0;
  // Skip cutscenes closes a won results screen itself; Close() here only if it is still showing.
  if(state==2&&session.resolve_open_ms&&now-session.resolve_open_ms>kWinCloseMs&&!session.acted){call(gui.get(),"Close");session.acted=true;status("won","Won; starting the shadow encounter");}
  if(now-session.stage_ms>kResolveTimeoutMs){finish("error","Stopped: the shadow encounter did not start");}
  return;
 }
 if(state==2&&session.resolve_open_ms&&now-session.resolve_open_ms>kLossDoneMs&&!session.acted){
  call(gui.get(),"DoneButtonClicked");session.acted=true;
  ++session.battles;++session.losses_in_row;quarantine[session.target.key]=now+kLossSkipMs;
  if(session.losses_in_row>=kMaxLossesInRow){finish("stopped","Stopped after "+std::to_string(kMaxLossesInRow)+" lost Rocket battles in a row");return;}
  status("lost","Lost; skipping this stop");
 }
 if(now-session.stage_ms>kResolveTimeoutMs)finish("error","Stopped: the Rocket results screen did not close");
}
void step_encounter(const Permission &p,int64_t now){
 if(encounter_open()){
  session.encounter_seen=true;session.encounter_closed_ms=0;
  if(now-session.stage_ms>kEncounterTimeoutMs)finish("error","Stopped: the shadow encounter did not finish");
  return;
 }
 if(!session.encounter_closed_ms)session.encounter_closed_ms=now;
 if(press_conversations())return;
 // The catch summary opens a moment after the encounter closes.
 if(now-session.encounter_closed_ms<kSummaryGraceMs)return;
 Root summary=live(cls("Niantic.Holoholo.Encounter","EncounterSummaryGuiController"));
 if(summary&&unbox<bool>(call(summary.get(),"get_IsOpen"))){
  // Quick catch (on for the game during a session) presses the summary's own OK, whose exit restores
  // the map; closing it any other way leaves the map blank after Rocket catches (max_catch_flag.h).
  if(!session.summary_ms)session.summary_ms=now;
  if(now-session.summary_ms>kSummaryTimeoutMs)finish("error","Stopped: the catch summary did not close");
  return;
 }
 if(!p.map_ready){if(now-session.stage_ms>kEncounterTimeoutMs)finish("error","Stopped: the map did not come back after the catch");return;}
 ++session.battles;quarantine[session.target.key]=now+kBeatenSkipMs;
 next(now,"Rocket battle "+std::to_string(session.battles)+" done");
}

void step(const Permission &p,int64_t now){
 if(now-session.last_step<kStepMs)return;session.last_step=now;
 switch(session.stage){
  case Stage::Scan:step_scan(p,now);break;
  case Stage::Travel:step_travel(p,now);break;
  case Stage::Heal:step_heal(now);break;
  case Stage::Open:step_open(p,now);break;
  case Stage::Party:step_party(now);break;
  case Stage::Battle:step_battle(now);break;
  case Stage::Resolve:step_resolve(now);break;
  case Stage::Encounter:step_encounter(p,now);break;
  case Stage::Settle:if(adopt_late_grunt(now))break;if(now-session.stage_ms>=kSettleMs&&p.map_ready)enter(Stage::Scan,now);break;
  default:break;
 }
}
// Stop is honoured right away while only looking or walking; during a battle or catch the current
// one finishes first, so a fight or a shadow Pokemon is never abandoned midway.
bool stop_point(){return session.stage==Stage::Scan||session.stage==Stage::Travel||session.stage==Stage::Settle||session.stage==Stage::Heal;}

}  // namespace

bool pgr_auto_rocket_tick(uint64_t tid,const PgrSharedMetadataOutput*){
 if(busy||!tid||tid!=uint64_t(gettid()))return session.active;
 if(owner&&owner!=tid)return session.active;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 owner=tid;
 const Permission p=read_permission();
 if(!p.coherent)return session.active;
 const int64_t now=clock_ms();
 if(!session.active){
  if(!p.valid||!p.command.running||p.command.token<=finished_token)return false;
  if(!p.foreground||!p.coords)return false;
  session=Session{};session.active=true;session.command=p.command;enter(Stage::Scan,now);
  session.has_anchor=true;session.anchor_lat=p.lat;session.anchor_lon=p.lon;
  status("scanning","Looking for Team GO Rocket stops");
 }
 if(p.empty||!p.valid||p.command.token!=session.command.token){finish("stopped","Stopped");return false;}
 if(!p.foreground){finish("stopped","Stopped: the game left the foreground");return false;}
 session.command.cooldown_ms=p.command.cooldown_ms;
 if(!p.command.running&&!session.stop_requested){
  session.stop_requested=true;
  if(!stop_point())status(session.phase.c_str(),"Stopping after this battle");
 }
 if(!p.coords)return true;
 try{
  if(!ready){auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(api&&api->snapshot(&boot)==0,"Runtime unavailable");ready=true;}
  if(session.stop_requested&&stop_point()){finish("stopped","Stopped after "+battles_text(session.battles));return false;}
  step(p,now);
  heartbeat(now);
 }catch(const Failure &f){
  diag("diagnostic",std::string(stage_name(session.stage))+": "+f.where);
  finish("error","Auto Rocket stopped: "+f.where);
 }catch(...){finish("error","Auto Rocket stopped: native failure");}
 return session.active;
}
void pgr_auto_rocket_stop(uint64_t){if(session.active)finish("stopped","Stopped");}
extern "C" int pgr_auto_rocket_keeps_catches(){
 return session.active&&!session.command.transfer?1:0;
}
bool pgr_auto_rocket_between_battles(){
 return session.active&&(session.stage==Stage::Scan||session.stage==Stage::Travel||session.stage==Stage::Settle);
}
bool pgr_auto_rocket_vgp_window(){
 return pgr_auto_rocket_between_battles()&&session.command.walk&&!session.vgp_closed;
}

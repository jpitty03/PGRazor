// Executes the production controller with a small managed-game fixture. Game
// signature/ABI acceptance still requires the device check recorded separately.
#include <cassert>
#include <cstring>
#include <string>
#include <stdexcept>
#include <map>
#include <set>
#include "runtime_transport.h"
#include "vgp_results.h"
#include "safari_ball_policy.h"
#include "auto_throw_values.h"
namespace {
struct Failure {std::string where;};
void require(bool ok,const char* why){if(!ok)throw Failure{why};}
std::map<uintptr_t,void*> roots;uintptr_t next_root=1;
struct Root {
 uintptr_t h=0;Root()=default;explicit Root(void* p){if(p){h=next_root++;roots[h]=p;}}
 Root(const Root&)=delete;Root& operator=(const Root&)=delete;
 Root(Root&& o):h(o.h){o.h=0;}~Root(){reset();}
 Root& operator=(Root&& o){reset();h=o.h;o.h=0;return *this;}
 void reset(){if(h){assert(roots.erase(h)==1);h=0;}}
 void* get()const{return h?roots.at(h):nullptr;}explicit operator bool()const{return get()!=nullptr;}
};
int gui,interaction,state,pokemon,ball,bag,promise,response,probability;
union Box {bool b;int i;} boxed;
bool fresh=true,foreground=true,coherent=true,ownership=true,ready=true,in_play=false,photo=false;
int selected=1,stock=10,active_berry=0,attempts=0,polls=0,completions=0,last_completion=0,rearm=0,result=2,poll_rc=PGR_VR_PENDING;
int64_t now=10000;
constexpr int64_t kPromiseTimeoutMs=15000,kThrowCadenceMs=1200;
int g_berry_success=1,g_berry_active_exists=3,g_berry_no_stock=2;
std::string settings="C3|1|1|75|0|0|1|0",account="account";
std::string json_quote(const std::string& s){return "\""+s+"\"";}
int64_t clock_ms(){return now;}
unsigned sample_count=0;
int sample(PgrRuntimeTransportSample* out){*out={};out->snapshot_fresh=fresh;out->snapshot_seq=coherent?1:++sample_count;return 0;}
int get_string(uint32_t slot,char* out,uint32_t cap,uint32_t* len){assert(slot==15&&cap>settings.size());*len=settings.size();std::memcpy(out,settings.data(),*len);return 0;}
int get_bool(uint32_t slot,int* out){assert(slot==3);*out=foreground;return 0;}
int emit(uint32_t,const int64_t*,uint32_t,const PgrRuntimeTransportString*,uint32_t){return 0;}
PgrRuntimeTransportApi tr{};
const PgrRuntimeTransportApi* transport(){return &tr;}
void* resolve_class(const char*,const char*){return &gui;}
void* obj_klass(void* o){return o;}
std::string obj_class_name(void*){return "CatchPokemonOutProto";}
bool unity_obj_alive(void*){return ownership;}
Root find_live_unity_object(void*){return Root(&gui);}
Root resolve_di_service(const char*,const char*){return Root(&bag);}
std::string real_read_account_id(){return account;}
int read_enum_literal(void*,const char* n){return std::string(n)=="CapturedPokemon"?1:3;}
int item_literal_opt(const char* n){return std::string(n)=="PremierBall"?5:std::string(n)=="BeastBall"?6:7;}
void* find_field_by_name_checked(void*,const char*,const char* =nullptr){return nullptr;}
template<class T>T read_field_val(void*,void*){return T{};}
template<class T>T unbox_val(void* p){return *static_cast<T*>(p);}
void* call_obj(void* o,const char* method,int=0,void** args=nullptr){
 const std::string n=method;
 if(n=="btul")return ownership?&interaction:nullptr;
 if(n=="btuk")return &state;
 if(n=="get_MapPokemon")return &pokemon;
 if(n=="get_Pokeball")return &ball;
 if(n=="get_BallType"){boxed.i=selected;return &boxed.i;}
 if(n=="get_CanUseBerry"){boxed.b=active_berry==0;return &boxed.b;}
 if(n=="get_EncounterStrategyReady"){boxed.b=ready;return &boxed.b;}
 if(n=="get_IsInPhotoMode"){boxed.b=photo;return &boxed.b;}
 if(n=="get_PokeballInPlay"){boxed.b=in_play;return &boxed.b;}
 if(n=="get_IsRaidOrIncidentEncounter"){boxed.b=selected==5;return &boxed.b;}
 if(n=="GetItemCount"||n=="get_RaidBallCount"){boxed.i=stock;return &boxed.i;}
 if(n=="get_ActiveBerry"){boxed.i=active_berry;return &boxed.i;}
 if(n=="set_ActiveBerry"){active_berry=*static_cast<int*>(args[0]);return nullptr;}
 if(n=="EncounterStateComplete"){assert(o==&state);++completions;last_completion=*static_cast<int*>(args[0]);return nullptr;}
 throw Failure{"Unexpected fixture call: "+n};
}
void pgr_catch_chance_enter(void* o){assert(o==&interaction);++rearm;}
bool real_ensure_berry_ready(){return true;}
void* require_map_method(void*,const char*,int,const char*,const char*,const char*){return nullptr;}
void* invoke_method(void*,void*,void**){return &promise;}
int rewrite_throw(void* b,PgrThrowValue* v){assert(b==&ball);v->radius=0.5f;v->bullseye=1;v->spinning=0;return 0;}
PgrThrowApi throws_api{1,sizeof(PgrThrowApi),nullptr,nullptr,nullptr,rewrite_throw};
const void* pgrazor_throws_api(uint32_t){return &throws_api;}
Root submit_auto_throw(Root& s,Root& p,const PgrThrowValue& v){assert(s.get()==&state&&p.get()==&pokemon&&v.item==selected&&v.radius==0.5f&&v.bullseye==1&&v.spinning==0&&v.missed==0);++attempts;return Root(&promise);}
int poll_auto_throw(Root& p,uint64_t,Root& value,int& status){assert(p.get()==&promise);++polls;if(poll_rc==PGR_VR_DONE){value=Root(&response);status=result;}return poll_rc;}
const void* pgrazor_vgp_results_api(uint32_t){return nullptr;}
#include "auto_throw_controller.inc"
void reset(){
 manual_throw=ManualThrowSession{};assert(roots.empty());manual_throw.interaction=Root(&interaction);
 fresh=foreground=coherent=ownership=ready=true;in_play=photo=false;selected=1;stock=10;active_berry=0;
 attempts=polls=completions=rearm=0;poll_rc=PGR_VR_PENDING;result=2;now=10000;account="account";
 settings="C3|1|1|75|0|0|1|0";
}
void tick(bool raid=false){now+=1300;try{step_manual_throw(1,raid);}catch(const Failure& f){throw_block(f.where);}}
}
int main(){
 tr.sample=sample;tr.get_string=get_string;tr.get_bool=get_bool;tr.emit=emit;
 reset();settings="C3|1|1|75|0|0|0|0";tick();assert(!attempts);
 reset();active_berry=705;settings="C3|1|1|75|705|0|1|0";tick();assert(attempts==1); // Berry already active must NOT block throwing.
 reset();tick(true);assert(!attempts);tick();assert(attempts==1);
 for(int i=0;i<5;i++)tick();assert(attempts==1&&polls==5); // pending never resubmits
 poll_rc=PGR_VR_DONE;result=2;active_berry=701;tick();assert(attempts==1&&active_berry==0&&rearm==1);
 tick();assert(attempts==2);result=1;tick();assert(completions==1&&last_completion==1);tick();assert(attempts==2);
 reset();tick();poll_rc=PGR_VR_DONE;result=3;tick();assert(completions==1&&last_completion==3);
 reset();selected=5;tick();stock=0;poll_rc=PGR_VR_DONE;result=2;tick();assert(completions==1&&last_completion==3);
 reset();tick();now+=15001;tick();assert(manual_throw.blocked&&attempts==1);tick();assert(attempts==1);
 reset();tick();poll_rc=PGR_VR_REJECTED;tick();assert(manual_throw.blocked&&attempts==1);
 reset();tick();poll_rc=PGR_VR_DONE;result=0;tick();assert(manual_throw.blocked&&attempts==1);
 for(int loss=0;loss<5;loss++){
  reset();tick();if(loss==0)foreground=false;if(loss==1)fresh=false;if(loss==2)coherent=false;if(loss==3)ownership=false;if(loss==4)account="changed";
  tick();assert(manual_throw.blocked&&attempts==1&&polls==0);
  foreground=fresh=coherent=ownership=true;account="account";tick();assert(attempts==1);
 }
 reset();tick();settings="C3|1|1|75|0|0|0|0";tick();assert(attempts==1&&polls==1);
 poll_rc=PGR_VR_DONE;result=2;tick();tick();assert(attempts==1); // off drains result, no next throw
 for(int wait=0;wait<6;wait++){
  reset();if(wait==0)in_play=true;if(wait==1)photo=true;if(wait==2)ready=false;if(wait==3)selected=4;if(wait==4)stock=0;if(wait==5)settings="C3|1|1|75|0|0|1|100";
  tick();tick();assert(attempts==0&&!manual_throw.blocked);
 }
 manual_throw=ManualThrowSession{};assert(roots.empty());
 puts("PASS Auto Throw production controller: toggle, raid exclusion, pending, breakout, success/flee, exhausted supply, timeout/rejection/unknown, lifecycle/account, cooldown, Master Ball, stock and roots");
}

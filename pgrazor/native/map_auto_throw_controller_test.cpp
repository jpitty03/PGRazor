// Exercises the production map-catch state machine with counted game calls.
#include "runtime_transport.h"
#include "safari_ball_policy.h"
#include "throws.h"
#include "vgp_results.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <map>
#include <set>
#include <vector>
#include <string>
struct Failure{std::string where;};
void require(bool ok,const char* why){if(!ok)throw Failure{why};}
std::map<uintptr_t,void*> roots;uintptr_t sequence=1;
struct Root{
 uintptr_t h=0;Root()=default;explicit Root(void* p){if(p){h=sequence++;roots[h]=p;}}
 Root(const Root&)=delete;Root& operator=(const Root&)=delete;
 Root(Root&& other):h(other.h){other.h=0;}~Root(){reset();}
 Root& operator=(Root&& other){reset();h=other.h;other.h=0;return *this;}
 void reset(){if(h){assert(roots.erase(h)==1);h=0;}}
 void* get()const{return h?roots.at(h):nullptr;}explicit operator bool()const{return get()!=nullptr;}
};
int target,other,enc_promise,berry_promise,catch_promise,enc_response,berry_response;
int encounters=0,berries=0,captures=0,polls=0,values=0,ball=1,result=1,rc=PGR_VR_PENDING;
bool fresh=true,foreground=true,eligible=true,alive=true,fail_capture=false;
int64_t now=10000;constexpr int64_t kPromiseTimeoutMs=15000,kThrowCadenceMs=1200;
std::string account="one",settings="C4|0|1|75|701|0|1|0|1";
PgrThrowValue last{};
int64_t clock_ms(){return now;}
std::string json_quote(const std::string& value){return "\""+value+"\"";}
int sample(PgrRuntimeTransportSample* out){*out={};out->snapshot_fresh=fresh;out->snapshot_seq=1;return 0;}
int str(uint32_t slot,char* out,uint32_t cap,uint32_t* size){assert(slot==15&&cap>settings.size());*size=settings.size();memcpy(out,settings.data(),*size);return 0;}
int boolean(uint32_t slot,int* out){assert(slot==3);*out=foreground;return 0;}
int emit(uint32_t,const int64_t*,uint32_t,const PgrRuntimeTransportString*,uint32_t){return 0;}
PgrRuntimeTransportApi tr{};
const PgrRuntimeTransportApi* transport(){return &tr;}
#include "map_auto_throw_controller.inc"
const MapTapAdapter& map_tap_adapter(){
 static const MapTapAdapter a{
  +[]()->std::string{return account;},+[](void*){return eligible;},+[](void*)->uint64_t{return 42;},+[](void*){return alive;},
  +[](void*){++encounters;return Root(&enc_promise);},
  +[](void*,uint64_t,Root& out)->int{out=Root(&enc_response);return PGR_VR_DONE;},
  +[](void* response){return response==&berry_response?701:0;},
  +[](void*,int item){assert(item==701);++berries;return Root(&berry_promise);},
  +[](void*,uint64_t,Root& out,int& status)->int{out=Root(&berry_response);status=1;return PGR_VR_DONE;},
  +[](PgrThrowValue& value){++values;value={};value.item=1;value.radius=values%2?.8f:.4f;value.bullseye=1;value.spinning=values%2;return true;},
  +[](void*,void*,const PgrCatchChanceOptions&,const PgrThrowValue&){return ball;},
  +[](void*,const PgrThrowValue& value){++captures;last=value;return fail_capture?Root():Root(&catch_promise);},
  +[](void*,uint64_t,int& status){++polls;status=result;return rc;}
 };return a;
}
void reset(){
 map_reset();assert(roots.empty());map_finished.clear();encounters=berries=captures=polls=values=0;
 fresh=foreground=eligible=alive=true;fail_capture=false;ball=1;result=1;rc=PGR_VR_PENDING;now=10000;
 account="one";settings="C4|0|1|75|701|0|1|0|1";
}
void tick(bool blocked=false,int64_t delta=300){now+=delta;try{step_map_auto_throw(1,blocked);}catch(const Failure& f){map_block(f.where);}}
void begin_capture(){
 assert(map_tap_consume(&target,true));assert(!encounters);
 tick();assert(encounters==1);tick();tick();assert(berries==1);tick();tick();assert(captures==1&&map_tap.promise);
}
int main(){
 tr.sample=sample;tr.get_string=str;tr.get_bool=boolean;tr.emit=emit;
 // Reproduction of the first live failure: inactive slot-12 v1 is normal, not a running job.
 for(auto raw:{"","1|0|0|0|0|0|0|0","1|99|0|10|5|1|3|0"})assert(map_command_idle(raw,strlen(raw)));
 for(auto raw:{"1|0|1|0|0|0|0|0","1|0|0|0|0|0|0|1","R2|1|1","1|0|0|0|0|0|0|0|junk","1||0|0|0|0|0|0"})assert(!map_command_idle(raw,strlen(raw)));
 for(auto raw:{"C1|1|0|75","C2|1|0|75|0|0","C3|1|0|75|0|0|1|0"}){auto c=pgr_parse_catch_chance(raw);assert(c.valid&&!c.onmap);}
 assert(pgr_parse_catch_chance(settings.c_str()).onmap);
 for(auto raw:{"C4|1|0|75|0|0|1|0|2","C4|1|0|75|0|0|1|-1|1","C4|1|0|75|0|0|1|0|1x"})assert(!pgr_parse_catch_chance(raw).valid);
 reset();settings="C4|0|1|75|0|0|0|0|1";assert(!map_tap_consume(&target,true));
 reset();assert(!map_tap_consume(&target,false));eligible=false;assert(!map_tap_consume(&target,true));assert(roots.empty());
 reset();begin_capture();for(int i=0;i<8;i++){assert(map_tap_consume(&target,true));assert(map_tap_consume(&other,true));tick();}
 assert(encounters==1&&berries==1&&captures==1);alive=false;rc=PGR_VR_DONE;tick();assert(roots.empty());
 alive=true;assert(map_tap_consume(&target,true));assert(!map_tap.target); // terminal target cannot be caught twice
 reset();begin_capture();result=2;rc=PGR_VR_DONE;tick();rc=PGR_VR_PENDING;
 tick(false,1200);assert(berries==2);tick();tick();assert(captures==2&&values>=4&&!last.missed);
 result=3;rc=PGR_VR_DONE;tick();assert(roots.empty());
 reset();begin_capture();settings="C4|0|1|75|701|0|0|0|1";result=2;rc=PGR_VR_DONE;tick();tick(false,1200);assert(captures==1&&roots.empty());
 for(int failure=0;failure<5;failure++){
  reset();begin_capture();
  if(failure==0){tick(false,15001);}
  if(failure==1){rc=PGR_VR_REJECTED;tick();}
  if(failure==2){result=88;rc=PGR_VR_DONE;tick();}
  if(failure==3){foreground=false;tick();foreground=true;}
  if(failure==4){fresh=false;tick();fresh=true;}
  assert(captures==1&&roots.empty());assert(map_tap_consume(&target,true));assert(!map_tap.target);
 }
 reset();begin_capture();account="two";tick();assert(captures==1&&roots.empty()&&polls==0);
 reset();ball=0;assert(map_tap_consume(&target,true));tick();tick();tick();assert(!captures&&!berries&&roots.empty());
 reset();settings="C4|0|1|75|701|0|1|5000|1";assert(!map_tap_consume(&target,true));assert(!encounters&&roots.empty());
 reset();begin_capture();rc=PGR_VR_DONE;result=2;tick(true);tick(false,1200);assert(captures==1&&roots.empty());
 reset();fail_capture=true;assert(map_tap_consume(&target,true));tick();tick();tick();tick();tick();assert(captures==1&&roots.empty());assert(map_tap_consume(&target,true));assert(!map_tap.target);
 reset();puts("PASS production map Auto Throw: inactive-command regression, explicit tap, duplicates, berry breakout, settings per attempt, off/owner/account/foreground, uncertain dedup, no balls, roots and C1-C4");
}

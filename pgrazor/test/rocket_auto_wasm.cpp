#include "rocket_auto_policy.h"
#include "rocket_auto_receiver.h"
#include "rocket_auto_buttons.h"
using namespace pgr_rocket_auto;
namespace {
void *declaring(const void *method){return const_cast<void*>(method);}
void *actual(void *receiver){return receiver;}
bool value_type(void *type){return type==(void*)4;}
bool assignable(void *owner,void *type){return owner==type||(owner==(void*)1&&type==(void*)3);}
}
extern "C" {
void *memset(void*p,int c,unsigned long n){volatile unsigned char*d=(unsigned char*)p;for(unsigned long i=0;i<n;i++)d[i]=c;return p;}
void *memcpy(void*p,const void*q,unsigned long n){volatile unsigned char*d=(unsigned char*)p;auto*s=(const unsigned char*)q;for(unsigned long i=0;i<n;i++)d[i]=s[i];return p;}
int run(){
 for(unsigned i=0;i<64;i++)if(combat_mask(i)!=((i&7)|((i&8)?7:0)))return 1;
 if(combat_mask(64)||combat_mask(999))return 2;
 Policy p{};Frame f{};f.now=100000;f.authorized=true;f.mask=8;f.map=1;f.stop=2;
 if(p.step(f).action!=OpenStop)return 3;
 f.map=f.stop=0;f.spin=3;f.now=100400;if(p.step(f).action!=None)return 4;
 f.now=100800;if(p.step(f).action!=ExitStop)return 5;
 f.spin=0;f.intro=4;f.now=101200;if(p.step(f).action!=None)return 6;
 f.now=101600;if(p.step(f).action!=None)return 7;
 f.now=102000;if(p.step(f).action!=Challenge)return 8;
 f.intro=0;f.party=5;f.now=102400;if(p.step(f).action!=None)return 9;
 f.now=103200;if(p.step(f).action!=None)return 10;
 f.now=103600;if(p.step(f).action!=Submit)return 11;
 f.now=104000;if(p.step(f).action==Submit)return 12;
 f.party=0;f.result=6;f.now=104800;if(p.step(f).action!=None)return 13;
 f.now=106400;if(p.step(f).action!=Rescue)return 14;
 f.now=108800;if(p.step(f).action!=None)return 15;
 f.now=109200;if(p.step(f).action!=Done)return 16;
 f.result=0;f.capture_supported=true;f.encounter=7;f.interaction=8;f.ball=9;f.ball_type=0;f.now=109600;
 if(p.step(f).action!=SelectPremier)return 17;
 f.ball_type=5;f.now=110000;if(p.step(f).action!=Swipe)return 18;
 f.ball_in_play=true;f.now=114000;if(p.step(f).action==Swipe)return 19;
 f.ball_in_play=false;f.now=114400;if(p.step(f).action!=Swipe)return 20;
 f.now=114800;if(p.step(f).action==Swipe)return 21;
 f.authorized=false;f.now=115200;if(p.step(f).action!=None||p.flow(f.now))return 22;
 Policy skip{};f={};f.now=200000;f.authorized=true;f.mask=40;f.result=11;
 if(skip.step(f).action!=None)return 23;
 f.now=201600;if(skip.step(f).action!=Rescue)return 24;
 f.now=204400;if(skip.step(f).action!=Done)return 25;
 Policy retry{};f={};f.now=100000;f.mask=8;f.authorized=true;f.map=1;f.stop=2;if(retry.step(f).action!=OpenStop)return 26;
 retry.suspend();f.now=110000;if(retry.step(f).action==OpenStop)return 27;
 f.now=190001;if(retry.step(f).action!=OpenStop)return 28;
 Policy hidden{};f={};f.mask=8;f.authorized=true;f.party=5;f.rocket_battle=true;f.now=500000;hidden.step(f);f.now=502000;if(hidden.step(f).action==Submit)return 29;
 Cache cache{};cache.put(1,1000);cache.put(1,2000);auto*e=cache.find(1,2500,10000);if(!e||e->at!=2000)return 30;
 // The leader's stop has a separate 6 s exit delay.
 Policy leader{};f={};f.mask=24;f.authorized=true;f.now=100000;f.map=1;f.stop=2;f.leader=true;leader.step(f);f.map=f.stop=0;f.spin=3;f.now=101000;if(leader.step(f).action!=None)return 31;f.now=106000;if(leader.step(f).action!=ExitStop)return 32;
 // A lingering intro must not starve the already-open selected-party widget.
 Policy linger{};f={};f.mask=8;f.authorized=true;f.now=100000;f.dialog=3;f.dialog_button=4;linger.step(f);f.now=100800;if(linger.step(f).action!=DialogBattle)return 33;f.party=5;f.now=101200;linger.step(f);f.now=102400;if(linger.step(f).action!=Submit)return 34;
 // Once combat begins, old dialogue and party widgets must not trigger again.
 f.rocket_battle=true;f.now=104000;if(linger.step(f).action!=None)return 35;
 // Behaviour(1) can inspect derived GUI(3); CombatDirector(2) cannot.
 if(!receiver_matches((void*)1,(void*)3,declaring,actual,assignable))return 36;
 if(receiver_matches((void*)2,(void*)3,declaring,actual,assignable))return 37;
 if(receiver_matches(nullptr,(void*)3,declaring,actual,assignable))return 38;
 if(receiver_matches((void*)1,nullptr,declaring,actual,assignable))return 39;
 if(receiver_matches((void*)1,(void*)3,nullptr,actual,assignable))return 40;
 if(!battle_label("BattleButton")||!battle_label("FIGHT")||battle_label("ExitBattle")||battle_label("CancelFight")||battle_label("Back")||battle_label(nullptr))return 41;
 BattleChoice buttons;buttons.add(1,battle_label("CloseButton"));buttons.add(2,battle_label("BattleButton"));buttons.add(2,true);if(buttons.selected()!=2)return 42;
 buttons.add(3,battle_label("FightButton"));if(buttons.selected())return 43;
 // Replay the log: Rocket scope becomes active before the recruit BATTLE screen.
 Policy scope;f={};f.now=100000;f.mask=8;f.authorized=true;f.rocket_scope=true;f.intro=7;
 if(scope.step(f).action!=None)return 44;
 f.now=100800;if(scope.step(f).action!=Challenge)return 45;
 f.intro=0;f.party=9;f.now=101200;if(scope.step(f).action!=None)return 46;
 f.now=102400;if(scope.step(f).action!=Submit)return 47;
 f.now=102800;if(scope.step(f).action==Submit)return 48;
 // Once the real combat UI exists, lingering pre-battle widgets stay inert.
 f.rocket_battle=true;f.intro=7;f.now=112800;if(scope.step(f).action!=None)return 49;
 // After battle UI disappears, retained party/intro/dialog widgets must not
 // restart the battle during rescue. Capturing still uses the existing swipe.
 f.rocket_battle=false;f.dialog=0;f.now=113200;if(scope.step(f).action!=None)return 50;
 f.dialog=f.intro=f.party=0;f.encounter=10;f.interaction=11;f.ball=12;f.capture_supported=true;f.ball_type=5;f.now=113600;
 if(scope.step(f).action!=Swipe)return 51;
 f.ball_in_play=true;f.now=118000;if(scope.step(f).action!=None)return 52;
 f.ball_in_play=false;f.now=118400;if(scope.step(f).action!=Swipe)return 53;
 f.mask=40;f.now=123000;if(scope.step(f).action!=Swipe)return 54;
 f.mask=8;f.encounter=f.interaction=f.ball=0;f.dialog=3;f.now=123400;
 if(scope.step(f).action!=DialogNext)return 55;
 // Compatible declared interface/base return, concrete return, unrelated
 // reference and a boxed value return must be distinguished before invocation.
 if(!reference_return_matches((void*)1,(void*)3,value_type,assignable))return 56;
 if(!reference_return_matches((void*)3,(void*)3,value_type,assignable))return 57;
 if(reference_return_matches((void*)2,(void*)3,value_type,assignable))return 58;
 if(reference_return_matches((void*)4,(void*)4,value_type,assignable))return 59;
 if(reference_return_matches(nullptr,(void*)3,value_type,assignable))return 60;
 return 0;
}
}

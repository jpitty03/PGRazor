#pragma once
#include <stdint.h>
namespace pgr_manual {
// Only UI eligibility and warning decisions. No transfer/RPC API is exposed here.
struct Pokemon {bool known=false,favorite=false,egg=false,gym=false;};
inline bool eligible(const Pokemon &p){return p.known&&!p.favorite&&!p.egg&&!p.gym;}
inline bool transferable(bool original,bool enabled,bool argument_favorite,const Pokemon &p){
 return original||(enabled&&!argument_favorite&&eligible(p));
}
inline bool disabled(bool original,bool enabled,const Pokemon &p){return original&&!(enabled&&eligible(p));}
struct Window {
 uint64_t started=0,captured=0,generation=0,pending_generation=0;
 bool pending=false,consumed=false;
 void revoke(){started=captured=0;pending=false;consumed=false;++generation;}
 void begin(uint64_t now){revoke();started=now;}
 bool recent(uint64_t now,uint64_t limit)const{return started&&now>=started&&now-started<limit;}
 bool capture(uint64_t now){if(consumed||!recent(now,20000))return false;captured=now;pending_generation=generation;pending=true;return true;}
 bool due(uint64_t now)const{return pending&&pending_generation==generation&&now>=captured&&now-captured>=400&&now-captured<=5000&&recent(now,20000)&&!consumed;}
 bool expired(uint64_t now)const{return pending&&(now<captured||now-captured>5000||!recent(now,20000)||pending_generation!=generation);}
 void drop(){pending=false;captured=0;}
 void consume(){drop();consumed=true;}
};
// Require a category list, exactly two different buttons, exactly one Cancel/No,
// matching visible UI buttons and no ambiguity. Ordinary confirmation is never accepted.
inline int confirm(bool bullets,unsigned buttons,unsigned cancel_mask,unsigned ui_buttons,bool distinct){
 if(!bullets||buttons!=2||ui_buttons!=2||!distinct||(cancel_mask!=1&&cancel_mask!=2))return -1;
 return cancel_mask==1?1:0;
}
}

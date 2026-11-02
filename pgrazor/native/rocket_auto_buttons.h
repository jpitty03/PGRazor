#pragma once
#include <stdint.h>
namespace pgr_rocket_auto {
inline char button_lower(char c){return c>='A'&&c<='Z'?char(c+32):c;}
inline bool button_contains(const char *name,const char *word){
 if(!name)return false;
 for(;*name;++name){const char *a=name,*b=word;while(*a&&*b&&button_lower(*a)==*b){++a;++b;}if(!*b)return true;}
 return false;
}
inline bool battle_label(const char *name){
 return (button_contains(name,"battle")||button_contains(name,"fight"))&&
  !button_contains(name,"close")&&!button_contains(name,"exit")&&
  !button_contains(name,"back")&&!button_contains(name,"cancel");
}
struct BattleChoice {
 uint64_t key=0;bool ambiguous=false;
 void add(uint64_t candidate,bool named){if(!candidate||!named)return;if(key&&key!=candidate)ambiguous=true;else key=candidate;}
 uint64_t selected()const{return ambiguous?0:key;}
};
}

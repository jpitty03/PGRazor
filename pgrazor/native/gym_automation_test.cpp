#include "gym_automation.h"
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace pgr_gym;
int main(){
 Settings s;const char *p="1;1;1;1;50;10";assert(parse(p,strlen(p),s)&&s.deploy&&s.feed&&s.fallback);
 for(auto bad:{"","1;1;1;1;50;10;","2;1;1;1;50;10","1;2;1;1;50;10","1;1;1;1;-1;10","1;1;1;1;1001;10"})assert(!parse(bad,strlen(bad),s)&&!s.deploy);
 const char *v2="2;0;0;0;50;50;1";assert(parse(v2,strlen(v2),s)&&s.spin&&!s.deploy&&!s.feed);
 for(auto bad:{"2;0;0;0;50;50;2","1;0;0;0;50;50;1","2;0;0;0;50;50;1;0"})assert(!parse(bad,strlen(bad),s)&&!s.spin);
 assert(parse(p,strlen(p),s)&&!s.spin);assert(berry(51,100,s)==703);assert(berry(50,11,s)==701);assert(!berry(50,10,s));
 std::vector<Defender> c={{1,1,10,true,false,false},{2,2,20,true,true,false},{3,3,5,false,true,false},{4,4,1,true,false,true}};
 assert(choose(c,{},true)==2);assert(!choose(c,{2},false));assert(choose(c,{2},true)==1);assert(!choose(c,{1,2},true));
 assert(distance(0,0,0,0)==0&&distance(0,0,0,.001)>40&&distance(91,0,0,0)>40);
 History spins;assert(spins.spin_attempt("a",100));assert(!spins.spin_attempt("a",299999));
 assert(spins.spin_attempt("b",101));spins.spin_until("a",400000);spins.spin_until("a",200000);
 assert(!spins.spin_allowed("a",399999));assert(spins.spin_allowed("a",400000));
 History spin_caps;for(int i=0;i<512;i++)assert(spin_caps.spin_attempt(std::to_string(i),100));assert(!spin_caps.spin_attempt("overflow",101));
 History h;assert(h.deploy_attempt("gym",100));assert(!h.deploy_attempt("gym",600099));assert(h.deploy_attempt("gym",600100));
 for(int i=0;i<10;i++)assert(h.feed_attempt(42,100+i*400));assert(!h.feed_attempt(42,10000));
 assert(h.feed_attempt(42,1803700));h.refuse(42,1803701);assert(!h.feed_allowed(42,1804000));
 History caps;for(int i=1;i<=10;i++)assert(caps.feed_attempt(i,100));assert(!caps.feed_attempt(11,101));
 for(int i=0;i<512;i++)assert(caps.deploy_attempt(std::to_string(i),100));assert(!caps.deploy_attempt("overflow",101));
 puts("PASS gym policy: strict config, tag/fallback exclusions, species collisions, reserves, distance, TTLs and bounded attempt budgets");
}

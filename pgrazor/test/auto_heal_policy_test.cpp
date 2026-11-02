#include "../native/auto_heal_policy.h"
#include <cassert>
#include <cstdio>
using namespace pgr_heal;
int main(){
 assert(eligible({1,49,100,false},2));assert(!eligible({1,50,100,false},2));assert(!eligible({1,51,100,false},2));
 assert(eligible({1,50,101,false},2));assert(!eligible({1,51,101,false},2));
 assert(eligible({1,0,100,false},1));assert(!eligible({1,0,100,false},2));assert(!eligible({1,10,100,false},1));
 assert(!eligible({1,1,0,false},3));assert(!eligible({1,101,100,false},3));assert(!eligible({1,-1,100,false},3));assert(!eligible({0,0,100,false},3));assert(!eligible({1,0,100,true},3));
 assert(eligible({1,INT32_MAX/2,INT32_MAX,false},2));assert(!eligible({1,INT32_MAX/2+1,INT32_MAX,false},2));
 int stock[6]={1,1,1,1,1,1};assert(choose(stock,{1,0,100,false})==5);stock[5]=0;assert(choose(stock,{1,0,100,false})==4);stock[4]=0;assert(choose(stock,{1,0,100,false})==-1);
 assert(choose(stock,{1,80,100,false})==0);assert(choose(stock,{1,50,100,false})==1);assert(choose(stock,{1,49,100,false})==2);assert(choose(stock,{1,1,500,false})==3);
 int small[6]={1,0,0,0,0,0};assert(choose(small,{1,1,500,false})==0);int none[6]{};assert(choose(none,{1,1,500,false})==-1);
 Policy p;stock[4]=stock[5]=1;std::vector<Pokemon> rows{{1,0,100,false},{2,49,100,false},{3,50,100,false}};
 auto q=p.step(1000,1,rows,stock);assert(q.id==1&&q.item==5);assert(!p.step(2500,1,rows,stock).id);
 rows[0].hp=100;assert(!p.step(4000,1,rows,stock).id&&p.confirmed);
 q=p.step(5500,3,rows,stock);assert(q.id==2&&q.item==2);
 rows[1].hp=70;q=p.step(7000,3,rows,stock);assert(p.confirmed&&q.id==2&&q.item==1); // heal to full, not just over 50%
 rows[1].hp=100;assert(!p.step(8500,3,rows,stock).id);assert(p.healing.empty());
 Policy wait;rows={{5,20,101,false}};assert(wait.step(1000,2,rows,small).id==5);assert(!wait.step(10000,2,rows,small).id);
 assert(!wait.step(11500,2,rows,small).id&&wait.timed_out);assert(!wait.step(13000,2,rows,small).id);
 assert(wait.step(71500,2,rows,small).id==5);assert(!wait.step(71500,2,rows,small).id); // one global item at a time
 wait.reset();assert(!wait.pending.id&&wait.healing.empty()&&wait.later.empty());
 // A normal Revive leaves HP at half: finish healing even with threshold mode off.
 Policy revived;int normal[6]={0,1,0,0,1,0};rows={{8,0,100,false}};
 q=revived.step(1000,1,rows,normal);assert(q.id==8&&q.item==4);
 rows[0].hp=50;q=revived.step(2500,1,rows,normal);assert(q.id==8&&q.item==1&&revived.confirmed);
 rows[0].hp=100;assert(!revived.step(4000,1,rows,normal).id&&revived.healing.empty());
 rows={{6,0,100,false}};assert(!wait.step(80000,2,rows,stock).id);assert(wait.step(81500,3,rows,stock).id==6);
 rows.clear();assert(!wait.step(83000,3,rows,stock).id);assert(!wait.pending.id);
 puts("PASS native auto heal: strict 50% and odd/max HP, eligibility, item priority, full healing, single pending, HP confirmation, timeout/backoff, removal and reset");
}

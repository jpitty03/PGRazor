#include "../native/patch_work_budget.h"
#include <cassert>
#include <cstdio>
int main(){
 using namespace pgr_perf;
 assert(egg_interval(false)==1000&&egg_interval(true)==250);
 assert(heal_interval(false)==5000&&heal_interval(true)==1500);
 unsigned cursor=0,seen[96]{};
 for(unsigned pass=0;pass<8;pass++){
  unsigned work=0;
  for(unsigned i=0;i<96;i++)if(audit_cell(i,96,cursor,2000,1000,true,false)){seen[i]++;work++;}
  assert(work==12);cursor=next_cursor(cursor,96);
 }
 for(auto count:seen)assert(count==1);
 // Initial display and revocation are never postponed by the stable-row budget.
 for(unsigned i=0;i<96;i++){
  assert(audit_cell(i,96,0,500,1000,true,false));
  assert(audit_cell(i,96,0,2000,1000,false,false));
  assert(audit_cell(i,96,0,2000,1000,true,true));
 }
 assert(next_cursor(40,0)==0&&!audit_cell(0,0,0,2000,1000,true,false));
 assert(pvp_tables==192);
 puts("PASS performance budgets: 12/96 stable audits, every row within 8 ticks, initial/unapplied/revocation immediate, idle/active cadence");
}

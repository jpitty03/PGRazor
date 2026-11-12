#include "gym_repeat_policy.h"
#include <cassert>
#include <cstdio>
using namespace pgr_gym_repeat;
namespace {
constexpr uint64_t A=11,B=22;
Gate good(){Gate g;g.enabled=g.active=g.battles_allowed=true;return g;}
Timing tm;
void start(Policy &p,uint64_t now=1000){p.arm(A,now,good(),tm);assert(p.state==ST_ARMED);p.on_scope_enter(A,now,tm);assert(p.state==ST_IN_ROUND);}
void win(Policy &p,uint64_t now=1000){if(p.state==ST_IDLE)start(p,now);p.on_results(true,false);p.on_scope_exit(now,tm);assert(p.state==ST_WON);}
Action tick(Policy &p,uint64_t now,GymStatus live=GYM_UNKNOWN,bool healing=false){return p.tick(now,good(),tm,healing,live);}
void enter_map(Policy &p,uint64_t now){p.observe_map(true,now);assert(p.phase==PH_MAP_DWELL);}
void test_manual_start_and_identity(){
 Policy p;assert(tick(p,99999,GYM_HOSTILE)==ACT_NONE);
 p.on_scope_enter(A,1,tm);p.on_approach_open(A,tm);p.on_prep_open(A,true,tm);assert(p.state==ST_IDLE);
 p.arm(0,1,good(),tm);assert(p.state==ST_IDLE);
 for(int n=0;n<5;n++){Gate g=good();switch(n){case 0:g.enabled=false;break;case 1:g.active=false;break;case 2:g.battles_allowed=false;break;case 3:g.fresh=false;break;case 4:g.foreground=false;break;}p.arm(A,1,g,tm);assert(p.state==ST_IDLE);}
 p.arm(A,1,good(),tm);p.on_scope_enter(0,2,tm);assert(p.end==END_MANUAL_CANCEL);
 p.reset();p.arm(A,1,good(),tm);p.on_scope_enter(B,2,tm);assert(p.end==END_MANUAL_CANCEL);
}
void test_direct_repeat_and_once_only(){
 Policy p;win(p);assert(p.rounds==1&&p.phase==PH_RETURN);
 // Direct repeat: the game re-shows the approach; start the next round with ClickBattle
 // (never Back / never OnTapped), even after a long results animation.
 p.on_approach_open(A,tm);assert(tick(p,11000)==ACT_CLICK_BATTLE);assert(p.phase==PH_WAIT_PREP);
 // Once-only: repeated approach/prep readiness cannot re-issue a consumed action.
 p.on_approach_open(A,tm);assert(tick(p,11001)==ACT_NONE);
 p.on_prep_open(A,false,tm);assert(tick(p,11002)==ACT_NONE); // party not ready
 p.on_prep_open(A,true,tm);assert(tick(p,11003)==ACT_PREP_BATTLE);assert(p.state==ST_ARMED);
 // Scope enters during managed Battle. Post-invoke acknowledgment must preserve it.
 p.on_scope_enter(A,11003,tm);p.on_action_ok(ACT_PREP_BATTLE,11003,tm);assert(p.state==ST_IN_ROUND);
 assert(tick(p,500000)==ACT_NONE);assert(p.state==ST_IN_ROUND); // valid long round
 win(p,500001);assert(p.rounds==2&&p.phase==PH_RETURN);
 // A gym that is cleared on the re-shown approach ends successfully, no Battle/Back.
 p.decide_map_action(GYM_CLEARED);assert(p.ended()&&p.end==END_DEFEATED);
}
void test_map_fallback_cycle(){
 Policy p;win(p);assert(p.phase==PH_RETURN);
 // Optional natural map fallback: the game drops back to the map on its own (no Back),
 // dwell lets one heal scan run, then a single re-tap re-enters the same gym.
 enter_map(p,16000);
 assert(tick(p,20499,GYM_HOSTILE)==ACT_NONE); // clock begins at actual map entry
 assert(tick(p,20500,GYM_HOSTILE,true)==ACT_NONE); // healing pending
 assert(tick(p,20500,GYM_HOSTILE)==ACT_RESOLVE_GYM);assert(p.phase==PH_WAIT_APPROACH);
 for(int i=0;i<5;i++)assert(tick(p,21000+i,GYM_HOSTILE)==ACT_NONE); // once-only tap
 // Fresh approach can appear synchronously inside OnTapped; still ClickBattle, never Back.
 p.observe_map(false,21000);p.on_approach_open(A,tm);
 assert(tick(p,21000)==ACT_CLICK_BATTLE);assert(p.phase==PH_WAIT_PREP);
 p.on_approach_open(A,tm);assert(tick(p,21001)==ACT_NONE);
 p.on_prep_open(A,false,tm);assert(tick(p,21002)==ACT_NONE);
 p.on_prep_open(A,true,tm);assert(tick(p,21003)==ACT_PREP_BATTLE);assert(p.state==ST_ARMED);
 p.on_scope_enter(A,21003,tm);p.on_action_ok(ACT_PREP_BATTLE,21003,tm);assert(p.state==ST_IN_ROUND);
 win(p,30000);enter_map(p,30001);
 assert(tick(p,34501,GYM_CLEARED)==ACT_NONE);assert(p.end==END_DEFEATED);
}
void test_results_and_long_battle(){
 Policy p;start(p);assert(tick(p,300000)==ACT_NONE);assert(p.state==ST_IN_ROUND);
 p.on_results(false,false);assert(tick(p,300001)==ACT_NONE);assert(p.state==ST_IN_ROUND); // defender CONTINUE
 p.on_results(true,false);p.on_scope_exit(300002,tm);assert(p.state==ST_WON);
 for(int n=0;n<3;n++){Policy q;start(q);q.on_results(n==1,n==1);q.on_scope_exit(2000,tm);assert(q.ended());assert(q.end==(n==1?END_LOSS:END_MANUAL_CANCEL));}
}
void test_stop_and_navigation_deadlines(){
 for(int n=0;n<5;n++){Policy p;win(p);Gate g=good();EndReason reason=END_NONE;
 switch(n){case 0:g.enabled=false;reason=END_DISABLED;break;case 1:g.active=false;reason=END_BACKGROUND;break;case 2:g.foreground=false;reason=END_BACKGROUND;break;case 3:g.fresh=false;reason=END_STALE;break;case 4:g.battles_allowed=false;reason=END_UNAVAILABLE;break;}
 p.on_approach_open(A,tm);assert(p.tick(2000,g,tm,false,GYM_HOSTILE)==ACT_NONE);assert(p.end==reason);}
 Policy p;win(p);p.on_manual_cancel();assert(p.end==END_MANUAL_CANCEL);
 p.reset();win(p);p.on_account_change();assert(p.end==END_ACCOUNT);
 p.reset();p.arm(A,1000,good(),tm);assert(tick(p,16001)==ACT_NONE);assert(p.end==END_TIMEOUT);
 p.reset();p.arm(A,1000,good(),tm);p.on_scope_enter(A,16001,tm);assert(p.end==END_TIMEOUT);
 p.reset();win(p);enter_map(p,1100);assert(tick(p,61001,GYM_HOSTILE,true)==ACT_NONE);assert(p.end==END_TIMEOUT);
 p.reset();win(p);p.on_action_failed();assert(p.end==END_ERROR);
}
void test_live_gym_changes(){
 GymStatus observations[]={GYM_CLEARED,GYM_RAID,GYM_OUT_OF_RANGE,GYM_UNAVAILABLE};
 EndReason reasons[]={END_DEFEATED,END_RAID,END_OUT_OF_RANGE,END_UNAVAILABLE};
 for(int i=0;i<4;i++){Policy p;win(p);enter_map(p,1100);assert(tick(p,5600,observations[i])==ACT_NONE);assert(p.end==reasons[i]);}
 Policy p;win(p);p.on_approach_open(B,tm);assert(p.end==END_MANUAL_CANCEL);
 p.reset();win(p);enter_map(p,1100);p.observe_map(false,1200);assert(p.end==END_MANUAL_CANCEL);
 p.reset();win(p);enter_map(p,1100);assert(tick(p,5600,GYM_UNKNOWN)==ACT_NONE);assert(p.phase==PH_MAP_DWELL);
 assert(tick(p,5700,GYM_HOSTILE)==ACT_RESOLVE_GYM);p.on_approach_open(0,tm);assert(p.end==END_MANUAL_CANCEL);
}
}
int main(){test_manual_start_and_identity();test_direct_repeat_and_once_only();test_map_fallback_cycle();test_results_and_long_battle();test_stop_and_navigation_deadlines();test_live_gym_changes();puts("gym_repeat policy: PASS (direct repeat + optional map fallback)");}

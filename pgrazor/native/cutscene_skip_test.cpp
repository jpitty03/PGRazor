// Exercises the production raid lifecycle with managed-call failures, not a second implementation.
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#include "cutscene_skip.cpp"

namespace {
struct Obj {
 bool alive=true,raycasts=true,active=true,interactable=true;float alpha=1.f,speed=1.f;
 int state=2,result=1,taps=0;Obj *canvas=nullptr,*base=nullptr,*anim=nullptr,*action=nullptr,*target=nullptr;
 void *method=nullptr;
 Obj *confirm=nullptr,*rowroot=nullptr,*button=nullptr,*summary=nullptr,*rewards=nullptr,*visibility=nullptr;
};
Obj gui,canvas,base,anim,action,confirm,rowroot,button;
int ids[40];
enum{ALIVE,ROOT,ALPHA,RAY_GET,RAY_SET,SPEED_GET,SPEED_SET,TAP,LEADER,METHOD,TARGET,INVOKE,
 STATE,RESULT,CANVAS,ANIM,ACTION,CONFIRM,ROWROOT,ACTIVE,COMPONENT,BUTTON_ACTIVE,INTERACTABLE,PRESS,TYPE,
 SUMMARY,REWARDS,VISIBILITY,ALPHA_GET,CLAIM,REWARD_CLICK};
std::map<uintptr_t,void*> roots;uintptr_t serial=0;int allocations=0,frees=0;
std::vector<std::string> events;
void *fail_method=nullptr;void *fail_object=nullptr;int failures=0;
bool fail_reveal=false;
uintptr_t new_root(void *o,int){++allocations;roots[++serial]=o;return serial;}
void *get_root(uintptr_t h){auto it=roots.find(h);return it==roots.end()?nullptr:it->second;}
void free_root(uintptr_t h){assert(roots.erase(h)==1);++frees;}
void *unbox(void *o){return o;}
void get_field(void *o,void *f,void *out){
 auto x=static_cast<Obj*>(o);
 if(f==&ids[STATE])*static_cast<int*>(out)=x->state;
 else if(f==&ids[RESULT])*static_cast<int*>(out)=x->result;
 else if(f==&ids[CANVAS])*static_cast<void**>(out)=x->canvas;
 else if(f==&ids[ANIM])*static_cast<void**>(out)=x->anim;
 else if(f==&ids[ACTION])*static_cast<void**>(out)=x->action;
 else if(f==&ids[CONFIRM])*static_cast<void**>(out)=x->confirm;
 else if(f==&ids[ROWROOT])*static_cast<void**>(out)=x->rowroot;
 else if(f==&ids[SUMMARY])*static_cast<void**>(out)=x->summary;
 else if(f==&ids[REWARDS])*static_cast<void**>(out)=x->rewards;
 else if(f==&ids[VISIBILITY])*static_cast<void**>(out)=x->visibility;
 else assert(false);
}
void *call(void *m,void *o,void **args,void **ex){
 auto x=static_cast<Obj*>(o);*ex=nullptr;static bool boxed;
 if((m==fail_method&&(!fail_object||o==fail_object)&&failures-->0)||
    (fail_reveal&&m==&ids[ALPHA]&&*static_cast<float*>(args[0])==1.f)){
  *ex=&ids[19];return nullptr;
 }
 if(m==&ids[ALIVE]){boxed=args[0]&&static_cast<Obj*>(args[0])->alive;return &boxed;}
 if(m==&ids[ROOT])return x->base;
 if(m==&ids[ALPHA]){assert(x->alive);x->alpha=*static_cast<float*>(args[0]);return nullptr;}
 if(m==&ids[RAY_GET])return &x->raycasts;
 if(m==&ids[RAY_SET]){assert(x->alive);x->raycasts=*static_cast<bool*>(args[0]);return nullptr;}
 if(m==&ids[SPEED_GET])return &x->speed;
 if(m==&ids[SPEED_SET]){assert(x->alive);x->speed=*static_cast<float*>(args[0]);return nullptr;}
 if(m==&ids[TAP]){++x->taps;return nullptr;}
 if(m==&ids[METHOD])return x->method;
 if(m==&ids[TARGET])return x->target;
 if(m==&ids[INVOKE]){assert(x->method==&ids[LEADER]);++x->taps;x->target->action=nullptr;return nullptr;}
 if(m==&ids[ACTIVE]||m==&ids[BUTTON_ACTIVE])return &x->active;
 if(m==&ids[INTERACTABLE])return &x->interactable;
 if(m==&ids[COMPONENT]){assert(args[0]==&ids[TYPE]&&!*static_cast<bool*>(args[1]));return x->button;}
 if(m==&ids[PRESS]){assert(x->active&&x->interactable);++x->taps;return nullptr;}
 if(m==&ids[ALPHA_GET])return &x->alpha;
 if(m==&ids[CLAIM]||m==&ids[REWARD_CLICK]){++x->taps;return nullptr;}
 assert(false);return nullptr;
}
int emit(uint32_t,const int64_t*,uint32_t,const PgrRuntimeTransportString *s,uint32_t n){
 assert(n==2);events.emplace_back(s[1].ptr,s[1].len);return 0;
}
PgrRuntimeTransportApi transport_mock{};
void *from_reflection(void *o){return o;}
bool contains(const char *s){for(auto &e:events)if(e.find(s)!=std::string::npos)return true;return false;}
void setup(){
 assert(roots.empty());allocations=frees=0;events.clear();fail_method=fail_object=nullptr;failures=0;fail_reveal=false;
 gui={};canvas={};base={};anim={};action={};confirm={};rowroot={};button={};
 gui.confirm=&confirm;confirm.rowroot=&rowroot;rowroot.button=&button;rowroot.active=false;
 gui.canvas=&canvas;gui.base=&base;gui.anim=&anim;action.target=&gui;action.method=&ids[LEADER];
 boot.runtime.gchandle_new=reinterpret_cast<void*>(new_root);
 boot.runtime.gchandle_get_target=reinterpret_cast<void*>(get_root);
 boot.runtime.gchandle_free=reinterpret_cast<void*>(free_root);
 boot.runtime.runtime_invoke=reinterpret_cast<void*>(call);
 boot.runtime.object_unbox=reinterpret_cast<void*>(unbox);
 boot.runtime.field_get_value=reinterpret_cast<void*>(get_field);
 unity_alive=&ids[ALIVE];root_canvas=&ids[ROOT];canvas_alpha=&ids[ALPHA];
 canvas_raycasts_get=&ids[RAY_GET];canvas_raycasts=&ids[RAY_SET];
 animator_speed_get=&ids[SPEED_GET];animator_speed_set=&ids[SPEED_SET];
 raid_tap=&ids[TAP];raid_leaderboard_close=&ids[LEADER];delegate_method=&ids[METHOD];
 delegate_target=&ids[TARGET];action_invoke=&ids[INVOKE];reflection_method=reinterpret_cast<void*>(from_reflection);
 gui_state=&ids[STATE];raid_status=&ids[RESULT];raid_canvas_field=&ids[CANVAS];
 raid_animator_field=&ids[ANIM];gui_back_action=&ids[ACTION];raid_victory=1;
 raid_confirm_field=&ids[CONFIRM];raid_confirm_root=&ids[ROWROOT];raid_button_type=&ids[TYPE];
 game_object_active=&ids[ACTIVE];component_in_children=&ids[COMPONENT];button_active=&ids[BUTTON_ACTIVE];
 button_interactable=&ids[INTERACTABLE];button_press=&ids[PRESS];
 pending_raid=new_root(&gui,0);raid_since=clock_ms();raid_next=0;raid_last_state=-1;
 raid_taps=0;raid_wait_state=-1;raid_recovering=false;raid_finished=false;raid_confirmed=false;raid_animator=0;for(auto &c:raid_canvases)c={};
 transport_mock.emit=emit;pending_dialog=0;busy=false;
 owner.store(gettid());last_tick.store(clock_ms());enabled.store(true);
}
void cleared(){assert(!pending_raid&&roots.empty()&&allocations==frees);}
void prepared(){advance_raid(true);assert(pending_raid&&gui.canvas->alpha==0&&gui.base->alpha==0&&anim.speed==20);}
void due(){raid_next=clock_ms()-1;advance_raid(true);}
void finish(){gui.state=0;advance_raid(true);cleared();}
}
const void *pgrazor_runtime_transport_api(uint32_t){return &transport_mock;}
const void *pgrazor_runtime_bootstrap_api(uint32_t){return nullptr;}
const void *pgrazor_runtime_metadata_api(uint32_t){return nullptr;}
int pgrazor_hook(void*,void*,void**){return -1;}
bool pgr_catch_presentation_attached(const void*){return false;}
int pgr_catch_presentation_set_bypass(PgrCatchBypass,void*){return -1;}

int main(){
 // Live regression: normal skip actions reached the final summary but never pressed its button.
 // Only the active, enabled, interactable confirmation may run, once; it may close asynchronously.
 setup();prepared();due();assert(button.taps==0&&gui.taps==1);
 rowroot.active=true;button.active=false;due();assert(button.taps==0);
 button.active=true;button.interactable=false;due();assert(button.taps==0);
 button.interactable=true;gui.action=&action;due();assert(action.taps==1&&button.taps==0);
 due();assert(button.taps==1&&raid_confirmed&&contains("bonus-challenge"));
 int prior=gui.taps;due();assert(button.taps==1&&gui.taps==prior&&!contains("\"skipped\""));
 finish();assert(!canvas.raycasts&&!base.raycasts&&anim.speed==1);
 // A missing/destroyed confirmation must not become an early Close; failed Press restores UI.
 setup();prepared();rowroot.active=true;button.alive=false;due();assert(button.taps==0);advance_raid(false);cleared();
 setup();prepared();rowroot.active=true;fail_method=&ids[PRESS];failures=1;due();cleared();
 assert(canvas.alpha==1&&canvas.raycasts&&!contains("\"skipped\""));
 // Opening may initially disable raycasts; the game enables them when ready. Recovery must not
 // overwrite that newer state when the skipper never changed it (live Summary recovery regression).
 setup();canvas.raycasts=base.raycasts=false;prepared();canvas.raycasts=base.raycasts=true;
 advance_raid(false);cleared();assert(canvas.raycasts&&base.raycasts&&canvas.alpha==1);
 setup();canvas.raycasts=base.raycasts=false;prepared();canvas.raycasts=base.raycasts=true;
 gui.state=3;advance_raid(true);assert(!canvas.raycasts&&!base.raycasts);
 advance_raid(false);cleared();assert(canvas.raycasts&&base.raycasts);
 // Opening is hidden/accelerated but cannot advance until fully open, settled and dialogue finished.
 setup();gui.state=1;prepared();due();assert(gui.taps==0);
 gui.state=2;pending_dialog=77;due();assert(gui.taps==0);pending_dialog=0;
 due();assert(gui.taps==1);assert(!contains("\"skipped\""));
 // Leaderboard advancement must target this screen's currently registered callback only.
 gui.action=&action;due();assert(action.taps==1&&gui.action==nullptr&&gui.taps==1);
 gui.action=&action;action.target=&canvas;due();assert(action.taps==1&&gui.taps==2);
 action.target=&gui;action.method=&ids[TAP];due();assert(action.taps==1&&gui.taps==3);
 gui.state=3;advance_raid(true);assert(!canvas.raycasts&&!base.raycasts&&pending_raid);
 assert(!contains("\"skipped\""));finish();assert(!canvas.raycasts&&!base.raycasts&&anim.speed==1);
 assert(contains("\"completion\":\"dismissed\""));
 // Dismissal may complete between ticks, skipping state 3; hidden surviving canvases stay inert.
 setup();prepared();due();finish();assert(!canvas.raycasts&&!base.raycasts&&anim.speed==1);
 // A restoration error after confirmed dismissal cannot resurrect the invisible input layer.
 setup();prepared();due();fail_method=&ids[SPEED_SET];failures=2;gui.state=0;
 advance_raid(true);assert(pending_raid&&raid_recovering&&!canvas.raycasts&&canvas.alpha==0);
 advance_raid(true);cleared();assert(!canvas.raycasts&&canvas.alpha==0&&anim.speed==1);
 assert(!contains("\"skipped\""));
 // A managed hide failure after dismissal starts must recover, never be reported as destroyed.
 setup();prepared();gui.state=3;fail_method=&ids[RAY_SET];failures=1;
 advance_raid(true);cleared();assert(canvas.alpha==1&&base.alpha==1&&canvas.raycasts&&base.raycasts&&anim.speed==1);
 assert(contains("warning")&&!contains("\"skipped\""));
 // Retain roots and retry failed restoration; no further automatic actions during recovery.
 setup();prepared();fail_reveal=true;advance_raid(false);assert(raid_recovering&&pending_raid);
 assert(anim.speed==1&&canvas.raycasts&&gui.taps==0);fail_reveal=false;advance_raid(true);cleared();
 assert(canvas.alpha==1&&base.alpha==1&&!contains("\"skipped\""));
 // Foreground stop restores both input and per-screen animation, then releases every root.
 setup();prepared();gui.state=3;advance_raid(true);pgr_cutscene_skip_stop(gettid());cleared();
 assert(canvas.alpha==1&&base.alpha==1&&canvas.raycasts&&base.raycasts&&anim.speed==1);
 // Actual Unity destruction is a separate positive liveness check, not an exception.
 setup();prepared();due();gui.alive=false;canvas.alive=base.alive=anim.alive=false;
 advance_raid(true);cleared();assert(contains("\"completion\":\"destroyed\""));
 // Defeat/timeout/unknown battle results are untouched; unknown result may resolve while opening.
 for(int result:{0,2,3,4,5}){setup();gui.result=result;advance_raid(true);cleared();
  assert(gui.taps==0&&canvas.alpha==1&&anim.speed==1&&!contains("\"skipped\""));}
 setup();gui.state=1;gui.result=0;advance_raid(true);assert(pending_raid&&canvas.alpha==1);
 gui.result=1;gui.state=2;prepared();advance_raid(false);cleared();
 // Timeout reveals both canvas layers and restores the exact original raycast/speed values.
 setup();canvas.raycasts=false;anim.speed=2.f;prepared();raid_since=clock_ms()-kRaidWatchMs;
 advance_raid(true);cleared();assert(canvas.alpha==1&&!canvas.raycasts&&base.raycasts&&anim.speed==2.f);
 // Incomplete component capture must unwind the already-rooted/hidden components.
 setup();fail_method=&ids[ROOT];failures=1;advance_raid(true);cleared();assert(canvas.alpha==1&&anim.speed==1);
 // One shared root canvas is retained/restored once; initial closed state is not a completed skip.
 setup();gui.base=&canvas;gui.state=0;advance_raid(true);assert(pending_raid&&!contains("\"skipped\""));
 gui.state=2;prepared();assert(roots.size()==3);advance_raid(false);cleared();
 // A failed step remains a failure even if teardown concurrently destroys the controller.
 setup();prepared();fail_method=&ids[TAP];failures=1;due();cleared();assert(!contains("\"skipped\""));
 // Repeated raids do not inherit state or leave rooted components behind.
 for(int i=0;i<12;++i){setup();prepared();due();finish();}
 // Post-catch level-up regression: callbacks can already exist while opening, while a child is
 // hidden, or during avatar/coin-flip transitions. Those states must never advance either handler.
 setup();advance_raid(false);cleared();
 Obj summary,rewards,summary_canvas,rewards_canvas,summary_visibility,rewards_visibility;
 gui.summary=&summary;gui.rewards=&rewards;summary.canvas=&summary_canvas;rewards.canvas=&rewards_canvas;
 summary.visibility=&summary_visibility;rewards.visibility=&rewards_visibility;
 levelup_summary=&ids[SUMMARY];levelup_rewards=&ids[REWARDS];levelup_claim=&ids[CLAIM];levelup_click=&ids[REWARD_CLICK];
 levelup_summary_canvas=levelup_rewards_canvas=&ids[CANVAS];
 levelup_summary_visibility=levelup_rewards_visibility=&ids[VISIBILITY];
 levelup_active=levelup_visible=&ids[ACTIVE];levelup_interactable=&ids[INTERACTABLE];levelup_alpha=&ids[ALPHA_GET];
 PendingPopup p{0,1,clock_ms(),0,0};
 gui.state=0;assert(!advance_levelup(p,&gui)&&!contains("\"kind\":\"level-up\""));
 gui.state=1;assert(!advance_levelup(p,&gui)&&summary.taps==0&&rewards.taps==0);
 gui.state=2;summary_visibility.active=rewards_visibility.active=false;
 advance_levelup(p,&gui);assert(summary.taps==0&&rewards.taps==0);
 summary_visibility.active=true;summary.active=false;advance_levelup(p,&gui);assert(summary.taps==0);
 summary.active=true;summary_canvas.alpha=0.5f;advance_levelup(p,&gui);assert(summary.taps==0);
 summary_canvas.alpha=1;summary_canvas.interactable=false;advance_levelup(p,&gui);assert(summary.taps==0);
 summary_canvas.interactable=true;rewards_visibility.active=true;
 advance_levelup(p,&gui);assert(summary.taps==1&&rewards.taps==0&&p.steps==1);
 p.next=0;advance_levelup(p,&gui);assert(summary.taps==1&&rewards.taps==1&&p.steps==2);
 p.next=0;advance_levelup(p,&gui);assert(rewards.taps==1); // no repeat while old group remains visible
 rewards_canvas.alpha=0.3f;advance_levelup(p,&gui);assert(rewards.taps==1&&!p.rewards_wait);
 rewards_canvas.alpha=1;p.next=0;advance_levelup(p,&gui);assert(rewards.taps==2);
 gui.state=3;p.next=0;advance_levelup(p,&gui);assert(rewards.taps==2);
 gui.state=0;assert(advance_levelup(p,&gui)&&contains("\"kind\":\"level-up\""));
 // An unready screen times out visibly, and readiness failures use the same visible fallback.
 p={0,1,clock_ms()-kLevelUpWatchMs-1,0,0};gui.state=1;base.alpha=0;
 assert(advance_levelup(p,&gui)&&base.alpha==1);
 p={new_root(&gui,0),1,clock_ms(),0,0};gui.state=2;base.alpha=0;
 fail_method=&ids[ACTIVE];failures=1;assert(advance_popup(p)&&base.alpha==1);free_root(p.handle);cleared();
 std::puts("PASS level-up readiness: opening, hidden/inactive/fading/noninteractable children, one action per step, reward transition, completion and visible recovery");
 std::puts("PASS raid lifecycle: normal actions, dialogue/settle gates, real dismissal/destruction, failures, recovery, foreground stop, loss, timeout, shared canvases, repeated raids");
}

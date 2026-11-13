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
 void *method=nullptr,*cls=nullptr;
 Obj *confirm=nullptr,*rowroot=nullptr,*button=nullptr,*summary=nullptr,*rewards=nullptr,*visibility=nullptr;
 Obj *egg=nullptr,*tabs=nullptr,*content=nullptr,*page_transform=nullptr,*selected=nullptr,*completion=nullptr;
 int context=0,index=2,children=3;bool dragging=false,top=true,visible=true;float distance=0;
};
int egg_ids[17];
enum{E_PAGE,E_TABS,E_CONTEXT,E_CLOSE,E_TRANSFORM,E_CONTENT,E_INDEX,E_DRAG,E_DISTANCE,E_COUNT,E_CHILD,E_IS_CHILD,E_TOP,E_VISIBLE,E_SERVICE,E_CURRENT,E_COMPLETE};
Obj gui,canvas,base,anim,action,confirm,rowroot,button;
int ids[40];
enum{ALIVE,ROOT,ALPHA,RAY_GET,RAY_SET,SPEED_GET,SPEED_SET,TAP,LEADER,METHOD,TARGET,INVOKE,
 STATE,RESULT,CANVAS,ANIM,ACTION,CONFIRM,ROWROOT,ACTIVE,COMPONENT,BUTTON_ACTIVE,INTERACTABLE,PRESS,TYPE,
 SUMMARY,REWARDS,VISIBILITY,ALPHA_GET,CLAIM,REWARD_CLICK,GUI_DISMISS};
std::map<uintptr_t,void*> roots;uintptr_t serial=0;int allocations=0,frees=0;
std::vector<std::string> events;
std::string policy_text;
int flow_ids[3],intro_calls=0,hatch_calls=0,evolve_calls=0;
Obj completed,original_result;
bool result_bypassed=false;
Obj *reenter_hatch=nullptr;
bool throw_after_completion=false;
int policy_sample(PgrRuntimeTransportSample *out){*out={};out->snapshot_fresh=1;return 0;}
int policy_string(uint32_t field,char *out,uint32_t cap,uint32_t *len){
 assert(field==17&&cap>=policy_text.size());
 *len=uint32_t(policy_text.size());std::memcpy(out,policy_text.data(),*len);return 0;
}
void *fixture_new(void*){return &completed;}
void *fixture_method(void*,const char *name,int){assert(std::strcmp(name,"Invoke")==0);return &flow_ids[2];}
void fixture_intro(void*,void*,const void*){++intro_calls;}
void *fixture_hatch(void*,void*,float,void*,const void*){++hatch_calls;return &original_result;}
void *fixture_evolve(void*,void*,void*,const void*){++evolve_calls;return &original_result;}
void fixture_result(void*,void *callback,const void*){result_bypassed=bypass_show_info(nullptr,nullptr,nullptr,callback);}
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
 else if(f==&egg_ids[E_PAGE])*static_cast<void**>(out)=x->egg;
 else if(f==&egg_ids[E_TABS])*static_cast<void**>(out)=x->tabs;
 else if(f==&egg_ids[E_COMPLETE])*static_cast<void**>(out)=x->completion;
 else assert(false);
}
void *call(void *m,void *o,void **args,void **ex){
 auto x=static_cast<Obj*>(o);*ex=nullptr;static bool boxed;
 if(m==&flow_ids[0]||m==&flow_ids[1])return nullptr; // Promise / dismiss-parameters constructors and Complete
 if(m==&flow_ids[2]){
  ++x->taps;
  if(reenter_hatch){Obj *state=reenter_hatch;reenter_hatch=nullptr;hook_hatch_dismiss(state,nullptr,nullptr);}
  if(throw_after_completion)*ex=&flow_ids[2];
  return nullptr;
 } // hatch reveal, result-dismiss or state-completion callback
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
 if(m==&ids[GUI_DISMISS]){++x->taps;return nullptr;}  // base GuiController.Dismiss()
 if(m==&egg_ids[E_CONTEXT])return &x->context;
 if(m==&egg_ids[E_TOP])return &x->top;
 if(m==&egg_ids[E_VISIBLE])return &x->visible;
 if(m==&egg_ids[E_CLOSE]){++x->taps;return nullptr;}
 if(m==&egg_ids[E_TRANSFORM])return x->page_transform;
 if(m==&egg_ids[E_CONTENT])return x->content;
 if(m==&egg_ids[E_INDEX])return &x->index;
 if(m==&egg_ids[E_DRAG])return &x->dragging;
 if(m==&egg_ids[E_DISTANCE])return &x->distance;
 if(m==&egg_ids[E_COUNT])return &x->children;
 if(m==&egg_ids[E_CHILD]){assert(*static_cast<int*>(args[0])>=0);return x->selected;}
 if(m==&egg_ids[E_IS_CHILD]){boxed=x->base==args[0];return &boxed;}
 if(m==&egg_ids[E_SERVICE]||m==&egg_ids[E_CURRENT])return x->target;
 assert(false);return nullptr;
}
int emit(uint32_t,const int64_t*,uint32_t,const PgrRuntimeTransportString *s,uint32_t n){
 assert(n==2);events.emplace_back(s[1].ptr,s[1].len);return 0;
}
PgrRuntimeTransportApi transport_mock{};
void *from_reflection(void *o){return o;}
void *fixture_class(void *o){return static_cast<Obj*>(o)->cls;}  // il2cpp_object_get_class stand-in
int gym_open_calls=0;
void fixture_gui_open(void*,const void*){++gym_open_calls;}  // the game's own GuiController.Open
int hatch_dismiss_calls=0;
void fixture_hatch_dismiss(void *state,void *next,const void*){
 ++hatch_dismiss_calls;
 if(next){auto s=static_cast<Obj*>(state);s->target->target=static_cast<Obj*>(next);}
}
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
 // Gym badge "Share / Save" modal (Niantic.Holoholo.GymBadges.GymBadgeAwardGuiController): classified
 // by its class as it opens and dismissed once through the inherited GuiController.Dismiss, like the
 // medal pop-ups. The badge is awarded server-side so no step sequence is needed; the modal's own
 // HandleOpen/HandleDismiss are ret stubs and are never hooked. GuiController.Open still runs, so its
 // original cleanup is preserved. Exercises the production dismiss_kinds[] entry via hook_gui_open.
 int gym_idx=-1;for(int i=0;i<kDismissKinds;++i)if(std::string(dismiss_kinds[i].event)=="gym-badge")gym_idx=i;
 assert(gym_idx>=0&&!dismiss_kinds[gym_idx].step);
 int gym_class=0,other_class=0;
 auto wire_gym=[&]{
  setup();advance_raid(false);  // release setup()'s raid root; keep the setting enabled
  dismiss_kinds[gym_idx].klass=&gym_class;  // as install() would resolve it
  boot.runtime.object_get_class=reinterpret_cast<void*>(fixture_class);
  gui_open_original.store(reinterpret_cast<void*>(fixture_gui_open),std::memory_order_release);
  class_name_fn=class_namespace_fn=nullptr;  // skip the open trace; exercise only the dismiss path
  gui_dismiss=&ids[GUI_DISMISS];pending_popup_count=0;gym_open_calls=0;
 };
 // Ready open: hidden as it opens, original Open preserved, dismissed exactly once.
 wire_gym();
 Obj badge,badge_canvas;badge.base=&badge_canvas;badge.cls=&gym_class;badge.state=2;
 hook_gui_open(&badge,nullptr);
 assert(gym_open_calls==1&&pending_popup_count==1&&badge_canvas.alpha==0&&badge.taps==0);
 advance_popups();
 assert(pending_popup_count==0&&badge.taps==1&&contains("\"kind\":\"gym-badge\""));
 advance_popups();assert(badge.taps==1);  // the freed pop-up is not stepped again
 cleared();
 // Delayed open: left hidden while still opening (state 1), dismissed only once fully open.
 wire_gym();
 Obj slow,slow_canvas;slow.base=&slow_canvas;slow.cls=&gym_class;slow.state=1;
 hook_gui_open(&slow,nullptr);assert(pending_popup_count==1&&slow_canvas.alpha==0);
 advance_popups();assert(pending_popup_count==1&&slow.taps==0&&!contains("\"kind\":\"gym-badge\""));
 slow.state=2;advance_popups();
 assert(pending_popup_count==0&&slow.taps==1&&contains("\"kind\":\"gym-badge\""));
 cleared();
 // Disabled setting: the original Open runs untouched and nothing is queued or hidden.
 wire_gym();enabled.store(false,std::memory_order_release);
 Obj off,off_canvas;off.base=&off_canvas;off.cls=&gym_class;off.state=2;
 hook_gui_open(&off,nullptr);
 assert(gym_open_calls==1&&pending_popup_count==0&&off_canvas.alpha==1&&off.taps==0);
 enabled.store(true,std::memory_order_release);cleared();
 // An ordinary other screen passes straight through: not classified, not queued, not hidden.
 wire_gym();
 Obj plain,plain_canvas;plain.base=&plain_canvas;plain.cls=&other_class;plain.state=2;
 hook_gui_open(&plain,nullptr);
 assert(gym_open_calls==1&&pending_popup_count==0&&plain_canvas.alpha==1&&plain.taps==0);
 cleared();
 std::puts("PASS gym badge modal: classified and dismissed once, delayed open waits, disabled preserves original, other GUI untouched");
 // Either setting enables hatch cleanup, never a blanket inventory dismiss.
 assert(parse_policy("K1|1",4).cutscenes&&parse_policy("K1|1",4).inventory);
 assert(!parse_policy("K2|0|1",6).cutscenes&&parse_policy("K2|0|1",6).inventory);
 assert(parse_policy("K2|1|0",6).cutscenes&&parse_policy("K2|1|0",6).inventory);
 assert(parse_policy("K2|1|1",6).cutscenes&&parse_policy("K2|1|1",6).inventory);
 assert(!parse_policy("K2|1|x",6).cutscenes&&!parse_policy("K2|1|1x",7).inventory);
 Obj inv,egg,tab,content,page_transform,selected,other,hatch_state,inventory_service;int inventory_type;
 auto wire_inventory=[&](bool automatic=true){
  setup();advance_raid(false);cleared();
  inv={};egg={};tab={};content={};page_transform={};selected={};other={};
  inv.cls=&inventory_type;inv.egg=&egg;inv.tabs=&tab;egg.page_transform=&page_transform;
  tab.content=&content;content.selected=&selected;page_transform.base=&selected;
  boot.runtime.object_get_class=reinterpret_cast<void*>(fixture_class);
  gui_open_original.store(reinterpret_cast<void*>(fixture_gui_open));
  inventory_class=&inventory_type;inventory_egg_page=&egg_ids[E_PAGE];inventory_tabs=&egg_ids[E_TABS];
  inventory_context=&egg_ids[E_CONTEXT];inventory_close=&egg_ids[E_CLOSE];inventory_active=&ids[ACTIVE];
  inventory_top=&egg_ids[E_TOP];inventory_visible=&egg_ids[E_VISIBLE];
  inventory_page_transform=&egg_ids[E_TRANSFORM];inventory_tab_content=&egg_ids[E_CONTENT];
  inventory_tab_index=&egg_ids[E_INDEX];inventory_tab_dragging=&egg_ids[E_DRAG];inventory_tab_distance=&egg_ids[E_DISTANCE];
  inventory_child_count=&egg_ids[E_COUNT];inventory_child=&egg_ids[E_CHILD];inventory_is_child=&egg_ids[E_IS_CHILD];
  inventory_default_context=0;enabled.store(false);egg_inventory_enabled.store(true);
  hatch_inventory_service=&egg_ids[E_SERVICE];inventory_current=&egg_ids[E_CURRENT];
  hatch_state={};inventory_service={};hatch_state.target=&inventory_service;
  original[K_HATCH_DISMISS].store(reinterpret_cast<void*>(fixture_hatch_dismiss));hatch_dismiss_calls=0;
  hook_gui_open(&inv,nullptr);assert(!pending_inventory); // manual access never arms cleanup
  if(automatic){hook_hatch_dismiss(&hatch_state,&inv,nullptr);assert(pending_inventory&&hatch_dismiss_calls==1);}
  assert(inv.taps==0&&inv.alpha==1&&inv.raycasts);
 };
 auto settle_inventory=[&](){advance_inventory();if(inventory_settled)inventory_settled=clock_ms()-251;advance_inventory();};
 // Drive real transport -> tick -> hooks for all four settings, not just parser assertions.
 struct Case{const char *config;bool eggs,other_cutscenes;};
 for(const Case &c: {Case{"K2|0|0",false,false},Case{"K2|0|1",true,false},
                     Case{"K2|1|0",true,true},Case{"K2|1|1",true,true}}){
  wire_inventory(false);
  policy_text=c.config;transport_mock.sample=policy_sample;transport_mock.get_string=policy_string;
  tried=true;faulted.store(false);pgr_cutscene_skip_tick(gettid(),nullptr);
  // Manual access stays interactive even when either or both settings are on.
  hook_gui_open(&inv,nullptr);settle_inventory();assert(!pending_inventory&&inv.taps==0);
  boot.runtime.object_new=reinterpret_cast<void*>(fixture_new);
  boot.meta.class_get_method_from_name=reinterpret_cast<void*>(fixture_method);
  promise_class=&completed;promise_ctor=&flow_ids[0];promise_complete=&flow_ids[1];
  dismiss_class=&completed;dismiss_ctor=&flow_ids[0];
  egg_click=&ids[TAP];egg_is_open=&ids[ACTIVE];
  original[K_EGG_INTRO].store(reinterpret_cast<void*>(fixture_intro));
  original[K_HATCH].store(reinterpret_cast<void*>(fixture_hatch));
  original[K_EVOLVE].store(reinterpret_cast<void*>(fixture_evolve));
  original[K_HATCH_RESULT].store(reinterpret_cast<void*>(fixture_result));
  intro_calls=hatch_calls=evolve_calls=0;result_bypassed=false;
  Obj intro,reveal,dismiss;
  hook_egg_intro(&intro,nullptr,nullptr);assert(intro_calls==1&&bool(pending_egg)==c.eggs);
  pgr_cutscene_skip_tick(gettid(),nullptr);assert(!pending_egg&&intro.taps==int(c.eggs));
  void *hatch=hook_hatch(nullptr,nullptr,1.f,&reveal,nullptr);
  assert(hatch==(c.eggs?&completed:&original_result));
  assert(hatch_calls==int(!c.eggs)&&reveal.taps==int(c.eggs));
  // Non-hatch Pokemon details are never captured.
  assert(!bypass_show_info(nullptr,nullptr,nullptr,&dismiss));
  hook_hatch_result(nullptr,&dismiss,nullptr);assert(result_bypassed==c.eggs&&hatch_result_depth==0);
  pgr_cutscene_skip_tick(gettid(),nullptr);assert(!pending_dismiss&&dismiss.taps==int(c.eggs));
  void *evolved=hook_evolve(nullptr,nullptr,nullptr,nullptr);
  assert(evolved==(c.other_cutscenes?&completed:&original_result));
  assert(evolve_calls==int(!c.other_cutscenes));
  hook_hatch_dismiss(&hatch_state,&inv,nullptr);
  assert(hatch_dismiss_calls==1&&bool(pending_inventory)==c.eggs);
  settle_inventory();assert(inv.taps==int(c.eggs));
  assert(contains("post-hatch-egg-skip")==c.eggs);
  release_inventory();cleared();
  hook_gui_open(&inv,nullptr);settle_inventory();assert(!pending_inventory&&inv.taps==int(c.eggs));
  cleared();
 }
 std::puts("PASS egg setting matrix: real policy tick, intro, hatch/reveal callback, result/dismiss callback, post-hatch close; both off and manual pages preserved; auto-only leaves evolution alone");
 // Completion is the primary path: no follow-up inventory is created or tapped at all.
 // The existing matrix above deliberately covers the fallback with its binding unavailable.
 for(const Case &c: {Case{"K2|0|0",false,false},Case{"K2|0|1",true,false},
                     Case{"K2|1|0",true,true},Case{"K2|1|1",true,true}}){
  wire_inventory(false);policy_text=c.config;pgr_cutscene_skip_tick(gettid(),nullptr);
  Obj complete;hatch_state.completion=&complete;hatch_complete_field=&egg_ids[E_COMPLETE];
  hook_hatch_dismiss(&hatch_state,&inv,nullptr);
  assert(complete.taps==int(c.eggs)&&hatch_dismiss_calls==int(!c.eggs));
  assert(!pending_inventory&&inv.taps==0);
  assert(contains("hatch-completed-without-inventory")==c.eggs);
  hook_gui_open(&inv,nullptr);settle_inventory();assert(!pending_inventory&&inv.taps==0);
  hatch_complete_field=nullptr;cleared();
 }
 // Missing callback uses the existing game path exactly once and retains fallback diagnostics.
 wire_inventory(false);hatch_complete_field=&egg_ids[E_COMPLETE];
 hook_hatch_dismiss(&hatch_state,&inv,nullptr);
 assert(hatch_dismiss_calls==1&&pending_inventory&&contains("Hatch completion callback missing"));
 settle_inventory();assert(inv.taps==1);release_inventory();hatch_complete_field=nullptr;cleared();
 // Synchronous reentry and a callback throwing AFTER state advancement must never run original.
 for(bool fail: {false,true}){
  wire_inventory(false);Obj complete;hatch_state.completion=&complete;hatch_complete_field=&egg_ids[E_COMPLETE];
  reenter_hatch=&hatch_state;throw_after_completion=fail;
  hook_hatch_dismiss(&hatch_state,&inv,nullptr);
  assert(complete.taps==1&&hatch_dismiss_calls==0&&!pending_inventory&&!completing_hatch);
  assert(contains("reentry ignored"));
  assert(contains("not repeated")==fail);
  assert(contains("hatch-completed-without-inventory")==!fail);
  throw_after_completion=false;hatch_complete_field=nullptr;cleared();
 }
 // Independent later hatch batches are not suppressed by the reentry guard.
 wire_inventory(false);hatch_complete_field=&egg_ids[E_COMPLETE];
 Obj first_complete,second_complete;
 hatch_state.completion=&first_complete;hook_hatch_dismiss(&hatch_state,&inv,nullptr);
 hatch_state.completion=&second_complete;hook_hatch_dismiss(&hatch_state,&inv,nullptr);
 assert(first_complete.taps==1&&second_complete.taps==1&&hatch_dismiss_calls==0&&!pending_inventory);
 hatch_complete_field=nullptr;cleared();
 std::puts("PASS direct hatch completion: OR settings, no inventory, manual pages preserved, null fallback, reentry/throw-after-transition exactly once, later hatch batches");
 wire_inventory();inv.state=1;settle_inventory();assert(inv.taps==0);
 inv.state=2;settle_inventory();assert(inv.taps==1&&contains("egg-inventory"));
 advance_inventory();assert(inv.taps==1);inv.state=0;advance_inventory();cleared();
 // Manual Eggs access, even immediately after a hatch, is not captured.
 inv.state=2;hook_gui_open(&inv,nullptr);settle_inventory();assert(!pending_inventory&&inv.taps==1);cleared();
 wire_inventory(false);settle_inventory();assert(!pending_inventory&&inv.taps==0);cleared();
 // Branches that leave an existing inventory alone must not adopt it.
 wire_inventory(false);inventory_service.target=&inv;hook_hatch_dismiss(&hatch_state,nullptr,nullptr);
 settle_inventory();assert(hatch_dismiss_calls==1&&!pending_inventory&&inv.taps==0);cleared();
 // No UI created, failed observation, off/stale/wrong-thread: original always runs, no capture.
 wire_inventory(false);hook_hatch_dismiss(&hatch_state,nullptr,nullptr);assert(hatch_dismiss_calls==1&&!pending_inventory);cleared();
 wire_inventory(false);fail_method=&egg_ids[E_CURRENT];failures=1;hook_hatch_dismiss(&hatch_state,&inv,nullptr);
 assert(hatch_dismiss_calls==1&&!pending_inventory&&inventory_service.target==&inv);cleared();
 for(int gate=0;gate<3;++gate){wire_inventory(false);
  if(gate==0)egg_inventory_enabled.store(false);
  if(gate==1)last_tick.store(clock_ms()-3000);
  if(gate==2)owner.store(uint64_t(gettid())+1);
  hook_hatch_dismiss(&hatch_state,&inv,nullptr);assert(hatch_dismiss_calls==1&&!pending_inventory);cleared();
 }
 // Slow initialization is tied to this controller; timed-out captures cannot close later visits.
 wire_inventory();inv.state=0;settle_inventory();assert(pending_inventory&&inv.taps==0);
 inv.state=2;settle_inventory();assert(inv.taps==1);release_inventory();cleared();
 wire_inventory();inv.top=false;advance_inventory();inventory_since=clock_ms()-3001;settle_inventory();
 assert(!pending_inventory&&inv.taps==0&&contains("readiness-timeout")&&contains("covered-by-another-gui"));cleared();
 wire_inventory();content.selected=&other;settle_inventory();assert(inv.taps==0); // Pokemon/Tags
 content.selected=&selected;settle_inventory();assert(!pending_inventory&&inv.taps==0);cleared();
 wire_inventory();tab.dragging=true;settle_inventory();assert(!pending_inventory&&inv.taps==0);cleared();
 wire_inventory();tab.distance=0.5f;settle_inventory();assert(inv.taps==0);
 tab.distance=0;inv.context=3;settle_inventory();assert(inv.taps==0); // remote trade
 inv.context=0;egg_inventory_enabled.store(false);enabled.store(true);settle_inventory();assert(inv.taps==0);
 egg_inventory_enabled.store(true);settle_inventory();assert(!pending_inventory&&inv.taps==0);cleared();
 wire_inventory();last_tick.store(clock_ms()-3000);settle_inventory();assert(!pending_inventory&&inv.taps==0);cleared();
 wire_inventory();owner.store(uint64_t(gettid())+1);settle_inventory();assert(!pending_inventory&&inv.taps==0);cleared();
 wire_inventory();inv.active=false;settle_inventory();assert(inv.taps==0);
 inv.active=true;inv.top=false;settle_inventory();assert(inv.taps==0); // egg details/incubator modal above it
 inv.top=true;inv.visible=false;settle_inventory();assert(inv.taps==0);
 inv.visible=true;tab.index=9;settle_inventory();assert(inv.taps==0);
 release_inventory();cleared();
 wire_inventory();settle_inventory();inventory_clicked_at=clock_ms()-3001;inventory_since=clock_ms()-3500;
 advance_inventory();cleared();assert(inv.taps==1&&contains("close-not-confirmed"));
 wire_inventory();fail_method=&egg_ids[E_CLOSE];failures=1;settle_inventory();cleared();assert(inv.taps==0&&inv.raycasts&&inv.alpha==1);
 wire_inventory();pgr_cutscene_skip_stop(gettid());cleared();assert(inv.taps==0);
 inventory_class=nullptr;egg_inventory_enabled.store(false);
 std::puts("PASS post-hatch Eggs: manual access preserved, exact new-controller provenance, no-open/error/off/stale/thread branches, delayed open, expiry, user control, normal close and root cleanup");
 std::puts("PASS raid lifecycle: normal actions, dialogue/settle gates, real dismissal/destruction, failures, recovery, foreground stop, loss, timeout, shared canvases, repeated raids");
}

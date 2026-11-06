#include "cutscene_skip.h"
#include "catch_presentation.h"
#include "game_bindings.h"
#include "hook_engine.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <time.h>
#include <unistd.h>
// Skip cutscenes: the cutscenes are bypassed, not tapped through, so nothing is drawn.
//
// PokemonCutsceneService.Play*Cutscene (evolve, Mega, egg hatch, egg incubate, purify, fusion,
// unfusion, form change, new species) only build a cutscene GUI and return an IPromise that
// completes when that GUI closes; none of them sends a request or changes game data (checked in
// the 0.435.1 code). The replacements return a Promise that is already complete, so the caller
// goes straight on to what follows the cutscene (the hatched or evolved Pokemon's page, the catch
// summary). The egg hatch caller's System.Action, which the cutscene invokes once the Pokemon is
// revealed, is still invoked.
//
// IncidentInvasionDialogState.EnterState (a Team GO Rocket member's spoken lines before and after a
// battle) keeps only its first step, storing the state API and config, and builds no dialogue. On
// the next tick its own ConvoCompleted runs, which calls the config's OnComplete and exits the state,
// as the game does when the last line is tapped. The incident screen with the BATTLE button is a
// separate step, so it still appears and the battle starts only when BATTLE is pressed (checked on
// 0.435.1). Without a recent tick the dialogue plays normally, so the state is never left waiting.
//
// Each hook is installed the first time the setting is on and stays for the process; while the
// setting is off every hook calls the game's original.
namespace {
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
template<class F>F fn(const void *p){require(p,"missing runtime export");return reinterpret_cast<F>(const_cast<void*>(p));}

enum Kind {K_EVOLVE,K_MEGA,K_HATCH,K_PURIFY,K_FUSION,K_UNFUSION,K_FORM,K_SPECIES,K_INCUBATE,K_DIALOG,K_EGG_INTRO,K_HATCH_RESULT,K_RADAR,K_ROCKET_REWARDS,K_GEM,K_RAID_RESULTS,K_POWER_UP,K_COUNT};
const char *const kNames[K_COUNT]={"evolve","mega-evolve","egg-hatch","purify","fusion","unfusion","form-change","new-species","egg-incubate","rocket-dialogue","egg-hatch-intro","hatched-pokemon","rocket-radar-assembled","rocket-rewards","purified-gem","raid-results","power-up"};

PgrBootBinding boot{};
bool ready=false,busy=false,tried=false;
std::atomic<bool> enabled{false},faulted{false};
std::atomic<uint64_t> owner{0},last_tick{0};
std::atomic<void*> original[K_COUNT]{};
void *promise_class=nullptr,*promise_ctor=nullptr,*promise_complete=nullptr;
void *dialog_class=nullptr,*dialog_api=nullptr,*dialog_config=nullptr,*convo_completed=nullptr,*wbarrier=nullptr;
size_t api_offset=0,config_offset=0;
void *class_is_subclass=nullptr;
// Config.DialogMode: the grunt's spoken lines before a battle (RUN_DIALOG_PROTO_*, steal text, decoy)
// and after it (RUN_TEAM_ROCKET_VICTORY / _DEFEAT) are bypassed. Their OnComplete opens the next step,
// so the incident's own screen with the BATTLE button still appears and nothing starts a battle.
// RUN_DIALOG_2D_SEQUENCE (not seen on a grunt) and any unknown mode play as usual.
void *mode_field=nullptr;bool mode_skip[16]={};
std::string mode_names[16];
uintptr_t pending_dialog=0;  // GC root of a bypassed dialogue state; Unity thread only
// The hatch screen ("Oh?" over the egg) hosts the hatch sequence and waits for a tap, whose Click
// starts the game's own coroutine that awards XP and shows each hatched Pokemon. Its Click runs here
// as soon as the screen reports open, so it never waits; the per-egg cutscenes are bypassed above.
void *egg_click=nullptr,*egg_is_open=nullptr;
uintptr_t pending_egg=0;uint64_t pending_egg_since=0;
constexpr uint64_t kEggOpenWaitMs=3000;
// The hatched Pokemon's page. EggHatchState.bnai (obfuscated) opens it once per hatch through
// PokemonInfoGuiService.ShowPokemonInfo with a dismiss callback that ends the hatch state. While bnai
// runs, that one ShowPokemonInfo draws nothing; on the next tick its callback gets the
// DismissParameters (DismissType DEFAULT) that closing the page passes. Every other
// ShowPokemonInfo, and bnai's other work (telemetry, quest toasts), is untouched.
int hatch_result_depth=0;          // Unity thread only
uintptr_t pending_dismiss=0;       // the bypassed page's Action<DismissParameters>
void *dismiss_class=nullptr,*dismiss_ctor=nullptr;
// "You've assembled a Rocket Radar!" (Rocket Radar / Super Rocket Radar). ItemFragmentAssemblyService
// .TryShowFragmentUI takes the next assembled item from its queue and calls cbfq (obfuscated), which
// builds that modal; its EQUIP button equips the radar. With the setting on, cbfq builds nothing:
// the service's own UpdateMapEquippedUI equips the radar (if owned and not yet equipped, as the game
// does whenever the map appears) and cbfq returns null. TryShowFragmentUI then marks a modal as
// showing; on the next tick that flag is cleared, as the modal's close handler does, and
// TryShowFragmentUI runs again for anything still queued.
void *radar_update=nullptr,*radar_try_show=nullptr,*radar_showing=nullptr;
uintptr_t pending_radar=0;         // the service, until its showing flag is cleared
// The results and rewards screen after a won Team GO Rocket battle (IncidentCombatResolveGuiController).
// Its own Close() is what ends it: after a win it starts the shadow Pokemon encounter
// (IIncidentService.StartIncidentEncounter), then dismisses the screen. With the setting on, Close()
// runs as soon as the screen is opening, so the reward pop-ups are never stepped through. Rewards
// are already granted by the server. A loss or draw keeps the screen (Rematch / Done).
void *rewards_close=nullptr,*rewards_status=nullptr,*gui_state=nullptr,*rewards_incident=nullptr;
// Leader and Giovanni (radar) battles must not be closed early: that left the shadow encounter unable
// to throw (Pixel, 0.435.1). Their screen runs its own steps instead, hidden (root CanvasGroup alpha
// 0) and fast-forwarded with its own tap handlers (CancelAreaTapped, StartEncounterButtonClicked)
// until it closes itself; after 15 s it is shown again for normal taps.
void *rewards_tap=nullptr,*rewards_encounter=nullptr,*root_canvas=nullptr,*canvas_alpha=nullptr;
uintptr_t fast_rewards=0;uint64_t fast_since=0,fast_next=0;int fast_taps=0;
constexpr uint64_t kFastSettleMs=300,kFastGiveUpMs=15000;
uintptr_t pending_rewards=0;uint64_t pending_rewards_since=0;
constexpr uint64_t kRewardsOpenWaitMs=3000;
// The results screen after a raid (RaidResolveUIController): victory, rewards and bonus Premier Balls,
// the leaderboard. After a win its own Close() ends it and goes on to the raid boss encounter, as the
// last tap does; the rewards are already granted by the server. With the setting on, a won raid's
// screen is hidden (root CanvasGroup alpha 0) and, once it has finished opening (state 2, Open) and
// settled briefly, Close() runs. A bypassed post-raid dialogue completes first. Defeat, timeout or quit
// keep the screen. After 15 s it is shown again for normal taps.
void *raid_close=nullptr,*raid_status=nullptr;int32_t raid_victory=-1;
uintptr_t pending_raid=0;uint64_t raid_since=0,raid_next=0,raid_closed=0;int32_t raid_last_state=-1;
// After Close() the hidden screen also stops taking touches (CanvasGroup.blocksRaycasts) until it has
// fully dismissed (state 0): while it is still dismissing, an invisible screen would swallow the swipes
// of the encounter underneath. Followed for at most 60 s.
void *canvas_raycasts=nullptr;
// The "You're going too fast" warning (Niantic.Holoholo.Map.SpeedModal, opened through GuiController.Open).
// With the setting on it is hidden as it opens and, once open (state 2), dismissed with its own
// GuiController.Dismiss(), like tapping through it. Shown again for taps if it is not open within 10 s.
void *speed_modal_class=nullptr,*gui_dismiss=nullptr;uintptr_t pending_speed=0;uint64_t speed_since=0;
constexpr uint64_t kSpeedWatchMs=10000;
constexpr uint64_t kRaidDismissWatchMs=60000;
// The power-up animation on a Pokemon's page. PokemonInfoPanel.cift (obfuscated) handles the server's
// UpgradePokemonOutProto and starts the panel's own power-up coroutine, which paces each step (model
// spin, CP and HP count-up, sound) with the panel's serialized timings. With the setting on, those
// timings are near zero on that panel just before cift runs, so every step still runs and applies the
// new values, only without waiting. The confirmation dialogs are untouched. Turned off, the panel's
// original timings are put back the next time it powers up.
enum {PT_START,PT_CP_DELAY,PT_CP_TWEEN,PT_HP_DELAY,PT_HP_TWEEN,PT_SPIN,PT_AFTER,PT_COUNT};
const char *const kPowerTimingNames[PT_COUNT]={"upgradeStartDelayTime","upgradeCpDelayTime","upgradeCpTweenTime",
 "upgradeStaminaDelayTime","upgradeStaminaTweenTime","upgradeModelSpinTime","afterUpgradeDelayTime"};
// Delays become 0; tweens and the spin keep a short nonzero duration (a zero-length tween divides by it).
const float kPowerFast[PT_COUNT]={0.f,0.f,0.05f,0.f,0.05f,0.05f,0.f};
size_t power_offset[PT_COUNT]={};float power_original[PT_COUNT]={};bool power_saved=false,power_ready=false;
constexpr uint64_t kTickAliveMs=2000;

uint64_t clock_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
void *exp(const char *name){std::string n="il2cpp_";n+=name;void *p=dlsym(boot.handle,n.c_str());require(p,name);return p;}
void *klass(void *o){require(o,"null object");return fn<void*(*)(void*)>(boot.runtime.object_get_class)(o);}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
std::string typename_of(const void *type){char *p=fn<char*(*)(const void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
void event(const std::string &json){auto tr=transport();if(!tr)return;PgrRuntimeTransportString s[]={{"cutscene-skip",13},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,s,2);}
void status(const char *phase,const std::string &message){event(std::string("{\"phase\":\"")+phase+"\",\"message\":\""+message+"\"}");}
void skipped(Kind k){event(std::string("{\"phase\":\"skipped\",\"kind\":\"")+kNames[k]+"\"}");}

// "K1|1" turns skipping on; anything else (including a stale snapshot) means off.
bool configured(){
 auto tr=transport();char s[16];uint32_t n=0;PgrRuntimeTransportSample a{};
 if(!tr||tr->sample(&a)||!a.snapshot_fresh||a.fault)return false;
 return tr->get_string(17,s,sizeof(s),&n)==0&&n==4&&std::memcmp(s,"K1|1",4)==0;
}
// Skip only on the Unity thread the tick runs on, while ticks are arriving.
bool active(){
 if(!enabled.load(std::memory_order_acquire)||faulted.load(std::memory_order_acquire))return false;
 const uint64_t tid=owner.load(std::memory_order_acquire),seen=last_tick.load(std::memory_order_acquire);
 return tid&&tid==uint64_t(gettid())&&seen&&clock_ms()-seen<kTickAliveMs;
}
template<class F>F orig(Kind k){return reinterpret_cast<F>(original[k].load(std::memory_order_acquire));}

// A Niantic.Promises.Promise on which Complete() has already run.
void *completed_promise(){
 void *p=fn<void*(*)(void*)>(boot.runtime.object_new)(promise_class);require(p,"Promise");
 invoke(promise_ctor,p);invoke(promise_complete,p);return p;
}
void fault(const char *where){faulted.store(true,std::memory_order_release);status("error",std::string("Skip cutscenes stopped: ")+where);}
// The completed promise for a skipped cutscene, or null to play it.
void *skip(Kind k,void *action=nullptr){
 if(!active())return nullptr;
 void *p=nullptr;
 try{p=completed_promise();}
 catch(const Failure &f){fault(f.where);return nullptr;}
 catch(...){fault("native failure");return nullptr;}
 if(action){  // the hatch caller's reveal callback; a failure there must not bring the cutscene back
  try{invoke(fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(klass(action),"Invoke",0),action);}
  catch(...){status("warning","Egg hatch callback failed");}
 }
 skipped(k);return p;
}

using One=void*(*)(void*,void*,const void*);
using Two=void*(*)(void*,void*,void*,const void*);
using Mega=void*(*)(void*,void*,void*,bool,void*,bool,const void*);
using Hatch=void*(*)(void*,void*,float,void*,const void*);
using Fusion=void*(*)(void*,void*,void*,bool,int32_t,const void*);
using Species=void*(*)(void*,void*,bool,uint64_t,bool,const void*);  // Nullable<Form> is 8 bytes in one register
using Enter=void(*)(void*,void*,void*,const void*);

void *hook_evolve(void *s,void *a,void *b,const void *m){if(void *p=skip(K_EVOLVE))return p;return orig<Two>(K_EVOLVE)(s,a,b,m);}
void *hook_mega(void *s,void *a,void *b,bool c,void *d,bool e,const void *m){if(void *p=skip(K_MEGA))return p;return orig<Mega>(K_MEGA)(s,a,b,c,d,e,m);}
void *hook_hatch(void *s,void *a,float t,void *action,const void *m){if(void *p=skip(K_HATCH,action))return p;return orig<Hatch>(K_HATCH)(s,a,t,action,m);}
void *hook_purify(void *s,void *a,const void *m){if(void *p=skip(K_PURIFY))return p;return orig<One>(K_PURIFY)(s,a,m);}
void *hook_fusion(void *s,void *a,void *b,bool c,int32_t d,const void *m){if(void *p=skip(K_FUSION))return p;return orig<Fusion>(K_FUSION)(s,a,b,c,d,m);}
void *hook_unfusion(void *s,void *a,void *b,const void *m){if(void *p=skip(K_UNFUSION))return p;return orig<Two>(K_UNFUSION)(s,a,b,m);}
void *hook_form(void *s,void *a,void *b,const void *m){if(void *p=skip(K_FORM))return p;return orig<Two>(K_FORM)(s,a,b,m);}
void *hook_species(void *s,void *a,bool b,uint64_t form,bool c,const void *m){if(void *p=skip(K_SPECIES))return p;return orig<Species>(K_SPECIES)(s,a,b,form,c,m);}
void *hook_incubate(void *s,void *a,const void *m){if(void *p=skip(K_INCUBATE))return p;return orig<One>(K_INCUBATE)(s,a,m);}

// The dialogue state itself or a subclass of it: raids enter IncidentInvasionDialogStateShim (Giovanni's
// lines after a shadow raid), which adds nothing to it.
bool dialog_instance(void *self){
 void *k=klass(self);
 return k==dialog_class||(class_is_subclass&&fn<bool(*)(void*,void*,bool)>(class_is_subclass)(k,dialog_class,false));
}
int32_t dialog_mode(void *config){int32_t v=-1;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(config,mode_field,&v);return v;}
void hook_dialog(void *self,void *api,void *config,const void *m){
 if(active()&&self&&api&&config&&!pending_dialog&&dialog_class&&dialog_instance(self)){
  try{
   const int32_t mode=dialog_mode(config);
   const bool bypass=mode>=0&&mode<16&&mode_skip[mode];
   event(std::string("{\"phase\":\"dialogue\",\"mode\":\"")+(mode>=0&&mode<16&&!mode_names[mode].empty()?mode_names[mode]:std::to_string(mode))+"\",\"skipped\":"+(bypass?"true":"false")+"}");
   if(!bypass){orig<Enter>(K_DIALOG)(self,api,config,m);return;}
   // Reference fields are stored like EnterState stores them: the object pointer itself, through
   // the GC write barrier (il2cpp_field_set_value would store the address of a reference argument).
   auto store=fn<void(*)(void*,void**,void*)>(wbarrier);
   store(self,reinterpret_cast<void**>(static_cast<char*>(self)+api_offset),api);
   store(self,reinterpret_cast<void**>(static_cast<char*>(self)+config_offset),config);
   pending_dialog=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(self,0);require(pending_dialog,"GC root");
   skipped(K_DIALOG);return;
  }catch(const Failure &f){fault(f.where);}
  catch(...){fault("native failure");}
 }
 orig<Enter>(K_DIALOG)(self,api,config,m);
}

using EggInit=void(*)(void*,void*,const void*);
void hook_egg_intro(void *self,void *initializer,const void *m){
 orig<EggInit>(K_EGG_INTRO)(self,initializer,m);
 if(!active()||!self||!egg_click||!egg_is_open)return;
 if(pending_egg)fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(pending_egg);
 pending_egg=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(self,0);pending_egg_since=clock_ms();
}
void advance_egg(){
 void *gui=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(pending_egg);
 bool open=false;
 try{if(gui){void *boxed=invoke(egg_is_open,gui);open=boxed&&*static_cast<bool*>(fn<void*(*)(void*)>(boot.runtime.object_unbox)(boxed));}}
 catch(...){open=false;gui=nullptr;}
 if(gui&&!open&&clock_ms()-pending_egg_since<kEggOpenWaitMs)return;  // still opening
 fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(pending_egg);pending_egg=0;
 if(!gui||!open)return;  // left for a tap, as without the setting
 try{invoke(egg_click,gui);skipped(K_EGG_INTRO);}
 catch(...){status("warning","Egg hatch screen could not be continued");}
}

using HatchResult=void(*)(void*,void*,const void*);
void hook_hatch_result(void *self,void *list,const void *m){
 struct Depth{Depth(){++hatch_result_depth;}~Depth(){--hatch_result_depth;}}depth;  // also on a managed exception
 orig<HatchResult>(K_HATCH_RESULT)(self,list,m);
}
// Asked by the Quick Catch bridge's ShowPokemonInfo hook (catch_presentation), which owns that
// one hook, for every call it does not capture itself.
bool bypass_show_info(void *,void *,void *,void *on_dismiss){
 if(hatch_result_depth<=0||!active()||!on_dismiss||!dismiss_class||pending_dismiss)return false;
 pending_dismiss=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(on_dismiss,0);
 if(!pending_dismiss)return false;
 skipped(K_HATCH_RESULT);return true;
}
using RadarModal=void*(*)(void*,int32_t,bool,const void*);
void *hook_radar_modal(void *self,int32_t item,bool flag,const void *m){
 if(active()&&self&&radar_update&&!pending_radar&&(item==1502||item==1503)){
  try{
   int32_t value=item;void *args[]={&value};invoke(radar_update,self,args);
   pending_radar=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(self,0);require(pending_radar,"GC root");
   skipped(K_RADAR);return nullptr;
  }catch(const Failure &f){fault(f.where);}
  catch(...){fault("native failure");}
 }
 return orig<RadarModal>(K_RADAR)(self,item,flag,m);
}
// "You've created a Purified Gem!" (item 1505): the same service's cbfr (obfuscated) builds that modal
// and has nothing to equip; it is skipped like the radar one and the queue continues the same way.
void *hook_gem_modal(void *self,int32_t item,bool flag,const void *m){
 if(active()&&self&&radar_try_show&&!pending_radar&&item==1505){
  try{
   pending_radar=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(self,0);require(pending_radar,"GC root");
   skipped(K_GEM);return nullptr;
  }catch(const Failure &f){fault(f.where);}
  catch(...){fault("native failure");}
 }
 return orig<RadarModal>(K_GEM)(self,item,flag,m);
}
void finish_radar(){
 const uintptr_t h=pending_radar;pending_radar=0;
 void *service=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h);
 fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);
 if(!service)return;
 try{
  bool off=false;fn<void(*)(void*,void*,void*)>(boot.runtime.field_set_value)(service,radar_showing,&off);  // value type: copied from &off
  invoke(radar_try_show,service);
 }catch(...){status("error","Assembled-item queue could not be continued");}
}

int32_t int_field(void *o,void *f){int32_t v=-1;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;}
// Closes a won battle's results screen once it is opening or open (Close on a not-yet-opened
// GuiController would mark it closed but leave it to open later). True when handled.
// True for a leader / Giovanni / other radar character, from the screen's own IIncidentService.
bool leader_battle(void *gui){
 void *service=nullptr;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(gui,rewards_incident,&service);
 require(service,"incident service");
 auto m=[&](const char *name,int n){return fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(klass(service),name,n);};
 void *get=m("get_CurrentBattleCharacter",0),*boss=m("IsLeaderOrBossCharacter",1),*radar=m("IsRadarCharacter",1);
 require(get&&boss&&radar,"incident character checks");
 void *boxed=invoke(get,service);require(boxed,"battle character");
 int32_t character=*static_cast<int32_t*>(fn<void*(*)(void*)>(boot.runtime.object_unbox)(boxed));
 void *args[]={&character};
 auto yes=[&](void *mm){void *b=invoke(mm,service,args);return b&&*static_cast<bool*>(fn<void*(*)(void*)>(boot.runtime.object_unbox)(b));};
 return yes(boss)||yes(radar);
}
void set_alpha(void *gui,float alpha){
 void *group=invoke(root_canvas,gui);if(!group)return;
 void *args[]={&alpha};invoke(canvas_alpha,group,args);
}
bool close_rewards(void *gui){
 const int32_t state=int_field(gui,gui_state);
 if(state!=1&&state!=2)return false;
 if(int_field(gui,rewards_status)==0){  // 0 = Winner
  // Closing the screen while it is still starting up left the shadow encounter unable to throw now
  // and then (grunt and leader battles alike, timing dependent), so every won battle's screen runs
  // its own steps, hidden and fast-forwarded.
  {
   const bool leader=leader_battle(gui);
   if(!fast_rewards&&rewards_tap){
    fast_rewards=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(gui,0);
    fast_since=clock_ms();fast_next=0;fast_taps=0;set_alpha(gui,0.f);
    event(std::string("{\"phase\":\"fast-forward\",\"kind\":\"rocket-rewards\",\"leader\":")+(leader?"true":"false")+"}");
   }
   return true;
  }
 }
 return true;
}
// One fast-forward step for a leader's hidden results screen; ends when the screen leaves Opening/Open.
void advance_fast(){
 void *gui=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(fast_rewards);
 const uint64_t now=clock_ms();bool done=!gui;
 try{
  if(gui){
   const int32_t state=int_field(gui,gui_state);
   if(state!=1&&state!=2){done=true;event("{\"phase\":\"skipped\",\"kind\":\"rocket-rewards\",\"taps\":"+std::to_string(fast_taps)+",\"ms\":"+std::to_string(now-fast_since)+"}");}
   else if(now-fast_since>kFastGiveUpMs){done=true;set_alpha(gui,1.f);status("warning","Rocket results screen left for taps");}
   else{
    set_alpha(gui,0.f);  // every tick: transitions may restore the alpha
    // Its buttons only set Animator triggers, which do not advance it here. Once it has finished
    // opening (state 2, Open) and settled briefly, its own Close() runs: after a win that starts
    // the shadow encounter. Closing while still Opening (state 1) is what blocked throws.
    if(state==2){
     if(!fast_next)fast_next=now+kFastSettleMs;
     else if(now>=fast_next){++fast_taps;invoke(rewards_close,gui);}
    }
   }
  }
 }catch(...){done=true;if(gui){try{set_alpha(gui,1.f);}catch(...){}}status("warning","Rocket results screen left for taps");}
 if(!done)return;
 fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(fast_rewards);fast_rewards=0;
}
using RewardsInit=void(*)(void*,void*,void*,const void*);
void hook_rewards(void *self,void *info,void *func,const void *m){
 orig<RewardsInit>(K_ROCKET_REWARDS)(self,info,func,m);
 if(!active()||!self||!rewards_close||pending_rewards)return;
 try{
  // Initialize opens the grunt's post-battle dialogue; when that was bypassed it is finished first
  // (next tick, before advance_rewards), since closing this screen ends the state it reports to.
  if(!pending_dialog&&close_rewards(self))return;
  pending_rewards=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(self,0);pending_rewards_since=clock_ms();
 }catch(...){status("warning","Rocket results screen could not be closed");}
}
void advance_rewards(){
 if(pending_dialog)return;  // the post-battle dialogue completes first
 void *gui=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(pending_rewards);
 bool done=!gui||clock_ms()-pending_rewards_since>=kRewardsOpenWaitMs;
 try{if(gui&&close_rewards(gui))done=true;}catch(...){done=true;status("warning","Rocket results screen could not be closed");}
 if(!done)return;  // still waiting for it to open; afterwards it is left for taps, as without the setting
 fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(pending_rewards);pending_rewards=0;
}

// Diagnostic: names each NUI screen as it opens (GuiController.Open, which their Initialize calls)
// while the setting is on, so screens that still show can be identified from the log. It never
// changes what opens.
std::atomic<void*> gui_open_original{nullptr};
void *class_name_fn=nullptr,*class_namespace_fn=nullptr;
using GuiOpen=void(*)(void*,const void*);
void hook_gui_open(void *self,const void *m){
 if(active()&&self&&class_name_fn&&class_namespace_fn){
  try{
   void *k=klass(self);
   const char *ns=fn<const char*(*)(void*)>(class_namespace_fn)(k),*name=fn<const char*(*)(void*)>(class_name_fn)(k);
   event(std::string("{\"phase\":\"gui-open\",\"class\":\"")+(ns&&*ns?std::string(ns)+".":std::string())+(name?name:"?")+"\"}");
  }catch(...){}
 }
 reinterpret_cast<GuiOpen>(gui_open_original.load(std::memory_order_acquire))(self,m);
 if(active()&&self&&speed_modal_class&&gui_dismiss&&!pending_speed){
  try{
   if(klass(self)==speed_modal_class){
    pending_speed=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(self,0);speed_since=clock_ms();
    if(root_canvas&&canvas_alpha)set_alpha(self,0.f);
   }
  }catch(...){}
 }
}
void advance_speed(){
 void *gui=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(pending_speed);
 bool done=!gui;
 try{
  if(gui){
   const int32_t state=int_field(gui,gui_state);
   if(state==2){invoke(gui_dismiss,gui);done=true;event("{\"phase\":\"skipped\",\"kind\":\"speed-warning\",\"ms\":"+std::to_string(clock_ms()-speed_since)+"}");}
   else if(state==0||state==3)done=true;
   else if(clock_ms()-speed_since>kSpeedWatchMs){done=true;set_alpha(gui,1.f);status("warning","Speed warning left for taps");}
   else set_alpha(gui,0.f);
  }
 }catch(const Failure &f){done=true;if(gui){try{set_alpha(gui,1.f);}catch(...){}}status("warning",std::string("Speed warning left for taps: ")+f.where);}
 if(!done)return;
 fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(pending_speed);pending_speed=0;
}

using RaidInit=void(*)(void*,void*,const void*);
void hook_raid_results(void *self,void *info,const void *m){
 orig<RaidInit>(K_RAID_RESULTS)(self,info,m);
 if(!active()||!self||!raid_close||pending_raid)return;
 try{
  pending_raid=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(self,0);require(pending_raid,"GC root");
  raid_since=clock_ms();raid_next=0;raid_closed=0;raid_last_state=-1;
 }catch(...){status("warning","Raid results screen could not be skipped");}
}
void hide_raid(void *gui){
 set_alpha(gui,0.f);  // every tick: transitions may restore the alpha
 if(!raid_closed||!canvas_raycasts)return;
 void *group=invoke(root_canvas,gui);if(!group)return;
 bool off=false;void *args[]={&off};invoke(canvas_raycasts,group,args);
}
// One step for a raid results screen; ends once it has fully dismissed, or is left for taps.
void advance_raid(){
 void *gui=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(pending_raid);
 const uint64_t now=clock_ms();bool done=!gui;
 try{
  if(gui){
   const int32_t state=int_field(gui,gui_state),result=int_field(gui,raid_status);
   if(raid_closed&&state!=raid_last_state)
    event("{\"phase\":\"raid-results-state\",\"state\":"+std::to_string(state)+",\"ms\":"+std::to_string(now-raid_closed)+"}");
   raid_last_state=state;
   if(raid_closed){
    if(state==0){done=true;event("{\"phase\":\"skipped\",\"kind\":\"raid-results\",\"ms\":"+std::to_string(raid_closed-raid_since)+",\"dismissMs\":"+std::to_string(now-raid_closed)+"}");}
    else if(now-raid_closed>kRaidDismissWatchMs){done=true;status("warning","Raid results screen still dismissing (state "+std::to_string(state)+")");}
    else hide_raid(gui);
   }else if(result!=raid_victory){
    // Not a win (or not known yet while it opens): left as it is once it has opened.
    if(state==2||now-raid_since>=kRewardsOpenWaitMs)done=true;
   }else if(raid_next&&state!=1&&state!=2){
    raid_closed=now;hide_raid(gui);
    event("{\"phase\":\"raid-results-state\",\"state\":"+std::to_string(state)+",\"ms\":0}");
   }else if(now-raid_since>kFastGiveUpMs){
    done=true;set_alpha(gui,1.f);status("warning","Raid results screen left for taps");
   }else if(state==1||state==2){
    hide_raid(gui);
    if(pending_dialog)return;  // a bypassed post-raid dialogue completes first
    if(state==2){
     if(!raid_next)raid_next=now+kFastSettleMs;
     else if(now>=raid_next)invoke(raid_close,gui);  // repeated calls return at once (its own closed flag)
    }
   }
  }else if(raid_closed)event("{\"phase\":\"skipped\",\"kind\":\"raid-results\",\"ms\":"+std::to_string(raid_closed-raid_since)+",\"dismissMs\":-1}");
 }catch(...){
  done=true;
  // After Close() a failing call means its Unity objects are already destroyed: it is gone, not shown.
  if(raid_closed)event("{\"phase\":\"skipped\",\"kind\":\"raid-results\",\"ms\":"+std::to_string(raid_closed-raid_since)+",\"dismissMs\":-1}");
  else{if(gui){try{set_alpha(gui,1.f);}catch(...){}}status("warning","Raid results screen left for taps");}
 }
 if(!done)return;
 fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(pending_raid);pending_raid=0;
}

using PowerUp=void(*)(void*,void*,const void*);
void hook_power_up(void *self,void *result,const void *m){
 if(self&&power_ready){
  float *t[PT_COUNT];
  for(int i=0;i<PT_COUNT;++i)t[i]=reinterpret_cast<float*>(static_cast<char*>(self)+power_offset[i]);
  bool fast=true;for(int i=0;i<PT_COUNT;++i)fast=fast&&*t[i]==kPowerFast[i];
  if(active()){
   if(!fast&&!power_saved){for(int i=0;i<PT_COUNT;++i)power_original[i]=*t[i];power_saved=true;}
   for(int i=0;i<PT_COUNT;++i)*t[i]=kPowerFast[i];
   skipped(K_POWER_UP);
  }else if(fast&&power_saved)for(int i=0;i<PT_COUNT;++i)*t[i]=power_original[i];
 }
 orig<PowerUp>(K_POWER_UP)(self,result,m);
}

// Closes the bypassed page the way its own dismissal does, after bnai has returned.
void finish_dismiss(){
 const uintptr_t h=pending_dismiss;pending_dismiss=0;
 void *action=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h);
 fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);
 if(!action)return;
 try{
  void *params=fn<void*(*)(void*)>(boot.runtime.object_new)(dismiss_class);require(params,"DismissParameters");
  invoke(dismiss_ctor,params);
  void *args[]={params};
  invoke(fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(klass(action),"Invoke",1),action,args);
 }catch(...){status("error","Hatched Pokemon page could not be closed");}
}

// The bypassed dialogue finishes here, after EnterState has returned, like the game's own
// scheduled ConvoCompleted. It runs even if the setting was turned off meanwhile.
void finish_dialog(){
 const uintptr_t h=pending_dialog;pending_dialog=0;
 void *state=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h);
 fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);
 if(!state)return;
 // ExitState clears the config: the state already ended (e.g. the won battle's results screen was
 // closed, which exits it), so there is nothing left to complete.
 if(!*reinterpret_cast<void**>(static_cast<char*>(state)+config_offset))return;
 try{invoke(convo_completed,state);}
 catch(...){status("error","Rocket dialogue could not be completed");}
}

void *class_of(const PgrMetaApi *api,const char *ns,const char *name){
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult r{};require(api->resolve_class(&q,&r)==0,name);return const_cast<void*>(r.klass);
}
// The instance field of k whose type is exactly type_name; there must be one.
void *field_of_type(void *k,const char *type_name){
 void *found=nullptr,*iter=nullptr;
 while(void *f=fn<void*(*)(void*,void**)>(exp("class_get_fields"))(k,&iter)){
  if(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10)continue;  // static
  if(typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))!=type_name)continue;
  require(!found,type_name);found=f;
 }
 require(found,type_name);return found;
}
struct Target {Kind kind;void *owner;const char *name;unsigned params;uint64_t rva;const uint8_t *prologue;void *replacement;bool returns_promise;const char *return_prefix=nullptr;};

bool hook(const PgrMetaApi *api,const Target &t,std::string &why){
 PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=t.owner;q.name=t.name;
 if(t.return_prefix){q.return_type=t.return_prefix;q.return_match=PGR_MD_MATCH_PREFIX;}
 else{q.return_type=t.returns_promise?"Niantic.Promises.IPromise":"System.Void";q.return_match=PGR_MD_MATCH_EXACT;}
 q.param_count=t.params;
 q.expected_code=static_cast<const char*>(boot.module_base)+t.rva;q.prologue=t.prologue;q.prologue_len=24;
 PgrMetaMethodResult r{};const int rc=api->validate_method(&q,&r);
 if(rc!=0||!r.code){why+=std::string(why.empty()?"":", ")+kNames[t.kind]+" (code "+std::to_string(rc)+")";return false;}
 void *o=nullptr;const int hr=pgrazor_hook(const_cast<void*>(r.code),t.replacement,&o);
 if(hr!=0||!o){why+=std::string(why.empty()?"":", ")+kNames[t.kind]+" (hook)";return false;}
 original[t.kind].store(o,std::memory_order_release);return true;
}

void install(){
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api&&api->resolve_class&&api->validate_method,"metadata API");
 require(boot.module_base,"il2cpp module");
 promise_class=class_of(api,"Niantic.Promises","Promise");
 auto method=[&](void *k,const char *name,int n){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,n);require(m,name);return m;};
 promise_ctor=method(promise_class,".ctor",0);promise_complete=method(promise_class,"Complete",0);
 void *service=class_of(api,"Niantic.Holoholo.Cutscenes","PokemonCutsceneService");

 static const uint8_t p_evolve[]=PGR_GAME_CUTSCENE_SKIP_CPP_EVOLVE_FINGERPRINT,p_mega[]=PGR_GAME_CUTSCENE_SKIP_CPP_MEGA_FINGERPRINT,
  p_hatch[]=PGR_GAME_CUTSCENE_SKIP_CPP_EGG_HATCH_FINGERPRINT,p_purify[]=PGR_GAME_CUTSCENE_SKIP_CPP_PURIFY_FINGERPRINT,
  p_fusion[]=PGR_GAME_CUTSCENE_SKIP_CPP_FUSION_FINGERPRINT,p_unfusion[]=PGR_GAME_CUTSCENE_SKIP_CPP_UNFUSION_FINGERPRINT,
  p_form[]=PGR_GAME_CUTSCENE_SKIP_CPP_FORM_CHANGE_FINGERPRINT,p_species[]=PGR_GAME_CUTSCENE_SKIP_CPP_NEW_SPECIES_FINGERPRINT,
  p_incubate[]=PGR_GAME_CUTSCENE_SKIP_CPP_EGG_INCUBATE_FINGERPRINT,p_dialog[]=PGR_GAME_CUTSCENE_SKIP_CPP_INVASION_ENTER_FINGERPRINT,
  p_egg_intro[]=PGR_GAME_CUTSCENE_SKIP_CPP_EGG_INTRO_FINGERPRINT,
  p_hatch_result[]=PGR_GAME_CUTSCENE_SKIP_CPP_HATCH_RESULT_FINGERPRINT,p_show_info[]=PGR_GAME_CUTSCENE_SKIP_CPP_SHOW_INFO_FINGERPRINT,
  p_radar[]=PGR_GAME_CUTSCENE_SKIP_CPP_RADAR_MODAL_FINGERPRINT,p_rewards[]=PGR_GAME_CUTSCENE_SKIP_CPP_ROCKET_REWARDS_FINGERPRINT;
 static_assert(sizeof(p_evolve)==24&&sizeof(p_mega)==24&&sizeof(p_hatch)==24&&sizeof(p_purify)==24&&sizeof(p_fusion)==24&&
  sizeof(p_unfusion)==24&&sizeof(p_form)==24&&sizeof(p_species)==24&&sizeof(p_incubate)==24&&sizeof(p_dialog)==24&&sizeof(p_egg_intro)==24&&sizeof(p_hatch_result)==24&&sizeof(p_show_info)==24&&sizeof(p_radar)==24&&sizeof(p_rewards)==24,"24-byte prologues");
 const Target service_targets[]={
  {K_EVOLVE,service,"PlayEvolveCutscene",2,PGR_GAME_CUTSCENE_SKIP_CPP_EVOLVE_RVA,p_evolve,reinterpret_cast<void*>(hook_evolve),true},
  {K_MEGA,service,"PlayMegaEvolveCutscene",5,PGR_GAME_CUTSCENE_SKIP_CPP_MEGA_RVA,p_mega,reinterpret_cast<void*>(hook_mega),true},
  {K_HATCH,service,"PlayEggHatchCutscene",3,PGR_GAME_CUTSCENE_SKIP_CPP_EGG_HATCH_RVA,p_hatch,reinterpret_cast<void*>(hook_hatch),true},
  {K_PURIFY,service,"PlayPurifyCutscene",1,PGR_GAME_CUTSCENE_SKIP_CPP_PURIFY_RVA,p_purify,reinterpret_cast<void*>(hook_purify),true},
  {K_FUSION,service,"PlayFusionCutscene",4,PGR_GAME_CUTSCENE_SKIP_CPP_FUSION_RVA,p_fusion,reinterpret_cast<void*>(hook_fusion),true},
  {K_UNFUSION,service,"PlayUnfusionCutscene",2,PGR_GAME_CUTSCENE_SKIP_CPP_UNFUSION_RVA,p_unfusion,reinterpret_cast<void*>(hook_unfusion),true},
  {K_FORM,service,"PlayFormChangeCutscene",2,PGR_GAME_CUTSCENE_SKIP_CPP_FORM_CHANGE_RVA,p_form,reinterpret_cast<void*>(hook_form),true},
  {K_SPECIES,service,"PlayNewSpeciesCutscene",4,PGR_GAME_CUTSCENE_SKIP_CPP_NEW_SPECIES_RVA,p_species,reinterpret_cast<void*>(hook_species),true},
  {K_INCUBATE,service,"PlayEggIncubateCutscene",1,PGR_GAME_CUTSCENE_SKIP_CPP_EGG_INCUBATE_RVA,p_incubate,reinterpret_cast<void*>(hook_incubate),true},
 };
 std::string missing;int count=0;
 for(const Target &t:service_targets)count+=hook(api,t,missing);

 // The dialogue bypass needs the state's own fields and ConvoCompleted; without them it is left alone.
 try{
  dialog_class=class_of(api,"Niantic.Holoholo.Incident","IncidentInvasionDialogState");
  dialog_api=field_of_type(dialog_class,"Niantic.Platform.GameStates.StateLogic.IStateApi");
  dialog_config=field_of_type(dialog_class,"Niantic.Holoholo.Incident.IncidentInvasionDialogStateConfig");
  convo_completed=method(dialog_class,"ConvoCompleted",0);class_is_subclass=exp("class_is_subclass_of");
  auto offset=fn<size_t(*)(void*)>(exp("field_get_offset"));wbarrier=exp("gc_wbarrier_set_field");
  api_offset=offset(dialog_api);config_offset=offset(dialog_config);
  require(api_offset>=0x10&&api_offset<0x400&&config_offset>=0x10&&config_offset<0x400&&api_offset!=config_offset,"dialogue field offsets");
  // DialogMode's enum literals by name, so a reordered enum cannot turn the intro into a bypass.
  void *config_class=class_of(api,"Niantic.Holoholo.Incident","IncidentInvasionDialogStateConfig");
  mode_field=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(config_class,"DialogMode");require(mode_field,"DialogMode");
  void *mode_class=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(mode_field));require(mode_class,"DialogMode type");
  void *iter=nullptr;
  while(void *f=fn<void*(*)(void*,void**)>(exp("class_get_fields"))(mode_class,&iter)){
   if((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)!=0x8050)continue;  // static literal
   int32_t v=-1;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&v);
   const std::string name=fn<const char*(*)(void*)>(exp("field_get_name"))(f);
   if(v>=0&&v<16)mode_names[v]=name;
   static const char *const skip_names[]={"RUN_DIALOG_PROTO_NORMAL","RUN_DIALOG_PROTO_WITH_STEAL_TEXT","RUN_STANDALONE_STEAL_TEXT",
    "RUN_TEAM_ROCKET_VICTORY","RUN_TEAM_ROCKET_DEFEAT","RUN_GRUNT_DECOY"};
   for(const char *n:skip_names)if(v>=0&&v<16&&name==n)mode_skip[v]=true;
  }
  int skippable=0;for(bool b:mode_skip)skippable+=b;
  require(skippable==6,"DialogMode values");
  const Target t{K_DIALOG,dialog_class,"EnterState",2,PGR_GAME_CUTSCENE_SKIP_CPP_INVASION_ENTER_RVA,p_dialog,reinterpret_cast<void*>(hook_dialog),false};
  count+=hook(api,t,missing);
 }catch(const Failure &f){dialog_class=nullptr;missing+=std::string(missing.empty()?"":", ")+"rocket-dialogue ("+f.where+")";}

 // The hatch screen's own Click and IsOpen; the hook is installed only when both resolve.
 try{
  void *egg_gui=class_of(api,"Niantic.Holoholo.UI","EggHatchGuiController");
  egg_click=method(egg_gui,"Click",0);egg_is_open=method(egg_gui,"get_IsOpen",0);
  const Target t{K_EGG_INTRO,egg_gui,"Initialize",1,PGR_GAME_CUTSCENE_SKIP_CPP_EGG_INTRO_RVA,p_egg_intro,reinterpret_cast<void*>(hook_egg_intro),false};
  if(!hook(api,t,missing))egg_click=egg_is_open=nullptr;else ++count;
 }catch(const Failure &f){egg_click=egg_is_open=nullptr;missing+=std::string(missing.empty()?"":", ")+"egg-hatch-intro ("+f.where+")";}

 // The hatched Pokemon's page. The ShowPokemonInfo bypass only acts inside bnai, so it is inert if
 // the bnai hook fails.
 try{
  void *info_service=class_of(api,"Niantic.Holoholo.UI.Pokemon","PokemonInfoGuiService");
  void *hatch_state=class_of(api,"Niantic.Holoholo.EggHatch","EggHatchState");
  dismiss_class=class_of(api,"Niantic.Holoholo.UI.Pokemon","DismissParameters");dismiss_ctor=method(dismiss_class,".ctor",0);
  // ShowPokemonInfo is validated like a hook target; a prologue mismatch is accepted only when it is
  // the Quick Catch bridge's own hook at exactly that address.
  void *show_code=const_cast<char*>(static_cast<const char*>(boot.module_base))+PGR_GAME_CUTSCENE_SKIP_CPP_SHOW_INFO_RVA;
  PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=info_service;q.name="ShowPokemonInfo";
  q.return_type="System.Void";q.return_match=PGR_MD_MATCH_EXACT;q.param_count=10;q.expected_code=show_code;q.prologue=p_show_info;q.prologue_len=24;
  PgrMetaMethodResult r{};const int rc=api->validate_method(&q,&r);
  const bool shared=rc==PGR_MD_ERR_PROLOGUE&&pgr_catch_presentation_attached(show_code);
  if(rc!=0&&!shared)missing+=std::string(missing.empty()?"":", ")+"pokemon-info (code "+std::to_string(rc)+")";
  else if(const int br=pgr_catch_presentation_set_bypass(bypass_show_info,show_code))missing+=std::string(missing.empty()?"":", ")+"pokemon-info (bridge "+std::to_string(br)+")";
  else{
   const Target result{K_HATCH_RESULT,hatch_state,PGR_GAME_CUTSCENE_SKIP_CPP_HATCH_RESULT_NAME,1,PGR_GAME_CUTSCENE_SKIP_CPP_HATCH_RESULT_RVA,p_hatch_result,reinterpret_cast<void*>(hook_hatch_result),false};
   if(hook(api,result,missing))++count;
  }
 }catch(const Failure &f){dismiss_class=nullptr;missing+=std::string(missing.empty()?"":", ")+"hatched-pokemon ("+f.where+")";}

 // The Rocket Radar assembled modal: the service's public UpdateMapEquippedUI and TryShowFragmentUI
 // and its one instance Boolean (the "modal showing" flag) must all resolve before cbfq is hooked.
 try{
  void *service=class_of(api,"Niantic.Holoholo.UI","ItemFragmentAssemblyService");
  radar_update=method(service,"UpdateMapEquippedUI",1);radar_try_show=method(service,"TryShowFragmentUI",0);
  radar_showing=field_of_type(service,"System.Boolean");
  require(fn<size_t(*)(void*)>(exp("field_get_offset"))(radar_showing)==0x48,"Rocket Radar showing flag");
  const Target t{K_RADAR,service,PGR_GAME_CUTSCENE_SKIP_CPP_RADAR_MODAL_NAME,2,PGR_GAME_CUTSCENE_SKIP_CPP_RADAR_MODAL_RVA,p_radar,reinterpret_cast<void*>(hook_radar_modal),false,"Niantic.Holoholo.UI.UserResponsePromise"};
  if(hook(api,t,missing))++count;else radar_update=nullptr;
  static const uint8_t p_gem[]=PGR_GAME_CUTSCENE_SKIP_CPP_GEM_MODAL_FINGERPRINT;
  static_assert(sizeof(p_gem)==24,"24-byte prologue");
  const Target g{K_GEM,service,PGR_GAME_CUTSCENE_SKIP_CPP_GEM_MODAL_NAME,2,PGR_GAME_CUTSCENE_SKIP_CPP_GEM_MODAL_RVA,p_gem,reinterpret_cast<void*>(hook_gem_modal),false,"Niantic.Holoholo.UI.UserResponsePromise"};
  if(hook(api,g,missing))++count;
 }catch(const Failure &f){radar_update=nullptr;missing+=std::string(missing.empty()?"":", ")+"rocket-radar-assembled ("+f.where+")";}

 // The won Rocket battle's results screen: its Close, its finish-state field and GuiController.currentState.
 try{
  void *gui=class_of(api,"Niantic.Holoholo.Incident","IncidentCombatResolveGuiController");
  void *base=class_of(api,"Niantic.Holoholo.NUI","GuiController");
  rewards_close=method(gui,"Close",0);
  rewards_status=field_of_type(gui,"Holoholo.Rpc.CombatPlayerFinishState");
  rewards_incident=field_of_type(gui,"Niantic.Holoholo.Incident.IIncidentService");
  rewards_tap=method(gui,"CancelAreaTapped",0);rewards_encounter=method(gui,"StartEncounterButtonClicked",0);
  root_canvas=method(base,"get_RootCanvasGroup",0);
  canvas_alpha=method(class_of(api,"UnityEngine","CanvasGroup"),"set_alpha",1);
  gui_state=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(base,"currentState");require(gui_state,"currentState");
  const Target t{K_ROCKET_REWARDS,gui,"Initialize",2,PGR_GAME_CUTSCENE_SKIP_CPP_ROCKET_REWARDS_RVA,p_rewards,reinterpret_cast<void*>(hook_rewards),false};
  if(hook(api,t,missing))++count;else rewards_close=nullptr;
 }catch(const Failure &f){rewards_close=nullptr;missing+=std::string(missing.empty()?"":", ")+"rocket-rewards ("+f.where+")";}

 // The raid results screen: its Close, its BattleStatus field and the Victory literal by name. Uses the
 // GuiController state and CanvasGroup methods resolved for the Rocket results screen above.
 try{
  require(gui_state&&root_canvas&&canvas_alpha,"GuiController state");
  void *gui=class_of(api,"Niantic.Holoholo.Raid","RaidResolveUIController");
  raid_close=method(gui,"Close",0);
  canvas_raycasts=method(class_of(api,"UnityEngine","CanvasGroup"),"set_blocksRaycasts",1);
  raid_status=field_of_type(gui,"Niantic.Holoholo.Battle.BattleStatus");
  void *status_class=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(raid_status));
  void *victory=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(status_class,"Victory");require(victory,"BattleStatus.Victory");
  fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(victory,&raid_victory);require(raid_victory>0,"BattleStatus.Victory value");
  static const uint8_t p_raid[]=PGR_GAME_CUTSCENE_SKIP_CPP_RAID_RESULTS_FINGERPRINT;
  static_assert(sizeof(p_raid)==24,"24-byte prologue");
  const Target t{K_RAID_RESULTS,gui,"Initialize",1,PGR_GAME_CUTSCENE_SKIP_CPP_RAID_RESULTS_RVA,p_raid,reinterpret_cast<void*>(hook_raid_results),false};
  if(hook(api,t,missing))++count;else raid_close=nullptr;
 }catch(const Failure &f){raid_close=nullptr;missing+=std::string(missing.empty()?"":", ")+"raid-results ("+f.where+")";}

 // The speed warning: dismissed through GuiController.Dismiss once open (no hook of its own).
 try{
  require(gui_state&&root_canvas&&canvas_alpha,"GuiController state");
  gui_dismiss=method(class_of(api,"Niantic.Holoholo.NUI","GuiController"),"Dismiss",0);
  speed_modal_class=class_of(api,"Niantic.Holoholo.Map","SpeedModal");
 }catch(const Failure &f){gui_dismiss=nullptr;speed_modal_class=nullptr;missing+=std::string(missing.empty()?"":", ")+"speed-warning ("+f.where+")";}

 // The power-up animation: the panel's seven Single timing fields by name, then cift.
 try{
  void *panel=class_of(api,"Niantic.Holoholo.UI.Pokemon","PokemonInfoPanel");
  auto offset=fn<size_t(*)(void*)>(exp("field_get_offset"));
  for(int i=0;i<PT_COUNT;++i){
   void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(panel,kPowerTimingNames[i]);require(f,kPowerTimingNames[i]);
   require(typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))=="System.Single",kPowerTimingNames[i]);
   power_offset[i]=offset(f);require(power_offset[i]>=0x10&&power_offset[i]<0x2000,kPowerTimingNames[i]);
  }
  static const uint8_t p_power[]=PGR_GAME_CUTSCENE_SKIP_CPP_POWER_UP_FINGERPRINT;
  static_assert(sizeof(p_power)==24,"24-byte prologue");
  const Target t{K_POWER_UP,panel,PGR_GAME_CUTSCENE_SKIP_CPP_POWER_UP_NAME,1,PGR_GAME_CUTSCENE_SKIP_CPP_POWER_UP_RVA,p_power,reinterpret_cast<void*>(hook_power_up),false};
  power_ready=true;  // read by the hook only once it is attached
  if(hook(api,t,missing))++count;else power_ready=false;
 }catch(const Failure &f){power_ready=false;missing+=std::string(missing.empty()?"":", ")+"power-up ("+f.where+")";}

 // The screen-open trace; not a cutscene type, so it is not counted.
 try{
  class_name_fn=exp("class_get_name");class_namespace_fn=exp("class_get_namespace");
  static const uint8_t p_gui[]=PGR_GAME_CUTSCENE_SKIP_CPP_GUI_OPEN_FINGERPRINT;
  static_assert(sizeof(p_gui)==24,"24-byte prologue");
  PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=class_of(api,"Niantic.Holoholo.NUI","GuiController");q.name="Open";
  q.return_type="System.Void";q.return_match=PGR_MD_MATCH_EXACT;q.param_count=0;
  q.expected_code=static_cast<const char*>(boot.module_base)+PGR_GAME_CUTSCENE_SKIP_CPP_GUI_OPEN_RVA;q.prologue=p_gui;q.prologue_len=24;
  PgrMetaMethodResult r{};const int rc=api->validate_method(&q,&r);
  void *o=nullptr;
  if(rc!=0||!r.code)status("warning","Screen trace unavailable (code "+std::to_string(rc)+")");
  else if(pgrazor_hook(const_cast<void*>(r.code),reinterpret_cast<void*>(hook_gui_open),&o)!=0||!o)status("warning","Screen trace unavailable (hook)");
  else gui_open_original.store(o,std::memory_order_release);
 }catch(...){status("warning","Screen trace unavailable");}

 require(count>0,"no cutscene could be hooked");
 status("ready",std::to_string(count)+" of "+std::to_string(int(K_COUNT))+" cutscene types"+(missing.empty()?std::string():"; not skipped: "+missing));
}

}  // namespace

void pgr_cutscene_skip_tick(uint64_t tid,const PgrSharedMetadataOutput *){
 if(busy||!tid||tid!=uint64_t(gettid()))return;
 const uint64_t prior=owner.load(std::memory_order_acquire);if(prior&&prior!=tid)return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 owner.store(tid,std::memory_order_release);last_tick.store(clock_ms(),std::memory_order_release);
 if(pending_dialog)finish_dialog();
 if(pending_egg)advance_egg();
 if(pending_dismiss)finish_dismiss();
 if(pending_radar)finish_radar();
 if(pending_rewards)advance_rewards();
 if(fast_rewards)advance_fast();
 if(pending_raid)advance_raid();
 if(pending_speed)advance_speed();
 const bool on=configured();enabled.store(on,std::memory_order_release);
 if(!on||tried||faulted.load(std::memory_order_acquire))return;
 tried=true;
 try{
  if(!ready){auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(api&&api->snapshot(&boot)==0,"Runtime unavailable");ready=true;}
  install();
 }catch(const Failure &f){faulted.store(true,std::memory_order_release);status("error",std::string("Skip cutscenes unavailable: ")+f.where);}
 catch(...){faulted.store(true,std::memory_order_release);status("error","Skip cutscenes unavailable: native failure");}
}
void pgr_cutscene_skip_stop(uint64_t tid){
 enabled.store(false,std::memory_order_release);last_tick.store(0,std::memory_order_release);
 // No more ticks: a dialogue already bypassed must not be left waiting for one.
 if(!busy&&tid&&tid==uint64_t(gettid())&&tid==owner.load(std::memory_order_acquire)){
  if(pending_dialog)finish_dialog();
  if(pending_dismiss)finish_dismiss();
  if(pending_radar)finish_radar();
  if(pending_raid){  // shown again rather than left hidden without ticks
   void *gui=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(pending_raid);
   if(gui){try{set_alpha(gui,1.f);}catch(...){}}
   fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(pending_raid);pending_raid=0;
  }
 }
}

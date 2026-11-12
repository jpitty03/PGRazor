#include "cutscene_skip.h"
#include "catch_presentation.h"
#include "game_bindings.h"
#include "hook_engine.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <time.h>
#include <unistd.h>
// Skip cutscenes: bypass pure presentation; automatically advance screens that own game-state cleanup.
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

enum Kind {K_EVOLVE,K_MEGA,K_HATCH,K_PURIFY,K_FUSION,K_UNFUSION,K_FORM,K_SPECIES,K_INCUBATE,K_DIALOG,K_EGG_INTRO,K_HATCH_RESULT,K_RADAR,K_ROCKET_REWARDS,K_GEM,K_RAID_RESULTS,K_POWER_UP,K_HATCH_DISMISS,K_COUNT};
const char *const kNames[K_COUNT]={"evolve","mega-evolve","egg-hatch","purify","fusion","unfusion","form-change","new-species","egg-incubate","rocket-dialogue","egg-hatch-intro","hatched-pokemon","rocket-radar-assembled","rocket-rewards","purified-gem","raid-results","power-up","post-hatch-eggs"};

PgrBootBinding boot{};
bool ready=false,busy=false,tried=false;
std::atomic<bool> enabled{false},faulted{false};
std::atomic<bool> egg_inventory_enabled{false};
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
// Raid rewards must run their own animator/scheduler sequence. Calling Close early bypasses its
// reward/leaderboard steps. Advance with CancelAreaTapped and the registered CloseLeaderboard action,
// then press the active final confirmation button through its own handler. Speed up only this
// screen's animator. Never call Close early or mutate global input/time state.
void *raid_tap=nullptr,*raid_leaderboard_close=nullptr,*raid_status=nullptr;int32_t raid_victory=-1;
void *raid_animator_field=nullptr,*raid_canvas_field=nullptr,*gui_back_action=nullptr;
void *unity_alive=nullptr,*animator_speed_get=nullptr,*animator_speed_set=nullptr;
void *canvas_raycasts=nullptr,*canvas_raycasts_get=nullptr;
void *delegate_method=nullptr,*delegate_target=nullptr,*action_invoke=nullptr,*reflection_method=nullptr;
void *raid_confirm_field=nullptr,*raid_confirm_root=nullptr,*raid_button_type=nullptr;
void *game_object_active=nullptr,*component_in_children=nullptr,*button_active=nullptr;
void *button_interactable=nullptr,*button_press=nullptr;
uintptr_t pending_raid=0;uint64_t raid_since=0,raid_next=0;int32_t raid_last_state=-1;
struct RaidCanvas{uintptr_t handle=0;bool raycasts=true,raycasts_changed=false;};
RaidCanvas raid_canvases[2]{};
uintptr_t raid_animator=0;float raid_speed=1.f,raid_fast_speed=20.f;
bool raid_recovering=false,raid_finished=false,raid_confirmed=false;int raid_taps=0;
int raid_wait_state=-1;
constexpr uint64_t kRaidStepMs=250,kRaidWatchMs=30000,kRaidRecoveryMs=60000;
// Pop-ups closed with their own GuiController.Dismiss(), as tapping through them does: the "You're going
// too fast" warning (Niantic.Holoholo.Map.SpeedModal), the medal pop-ups (BadgeAwardGuiController for a new medal, BadgeIncrementGuiController for a medal
// tier), the "Gym Badge Earned" share/save modal (Niantic.Holoholo.GymBadges.GymBadgeAwardGuiController) and the weekly Adventure Sync summary (BgMode.UI.WeeklyFitnessDialog). With the setting on each is hidden as it opens (GuiController.Open) and dismissed once open
// (state 2); one not open within 15 s is shown again for taps.
// The gym badge modal only offers Share / Save over the already-awarded badge (the badge is granted
// server-side), so dismissing it loses nothing. Its HandleOpen/HandleOpenComplete/HandleDismiss are
// 4-byte ret stubs, so this bypasses only the inherited GuiController flow and hooks none of them.
// The level-up screen (LevelUpV3GuiController) is not dismissed: closed straight after opening it left
// the map ignoring taps (Pixel, until another pop-up closed). It is hidden and stepped through instead,
// as taps do: its summary's ClaimRewardsButtonClicked and its rewards screen's OnScreenClicked, every
// 250 ms, only after that child screen is visible, fully faded in and interactable,
// until the screen closes itself; one still open after 30 s is shown again for taps.
// Nothing here grants anything: level-up
// rewards are requested by PlayerService itself when the level changes (its Claim Rewards button only
// runs a UI callback), medals are awarded by the server before the pop-up opens, and the weekly summary
// is opened from the response to its own GetFitnessRewards request (BgModeWeeklyFitnessNotification).
struct DismissKind{const char *ns,*name,*event;bool step;void *klass;};
DismissKind dismiss_kinds[]={{"Niantic.Holoholo.Map","SpeedModal","speed-warning",false,nullptr},
 {"Niantic.Holoholo.UI","LevelUpV3GuiController","level-up",true,nullptr},
 {"Niantic.Holoholo.UI","BadgeAwardGuiController","medal",false,nullptr},
 {"Niantic.Holoholo.UI","BadgeIncrementGuiController","medal-tier",false,nullptr},
 {"Niantic.Holoholo.GymBadges","GymBadgeAwardGuiController","gym-badge",false,nullptr},
 {"Niantic.Holoholo.BgMode.UI","WeeklyFitnessDialog","weekly-fitness",false,nullptr}};
constexpr int kDismissKinds=int(sizeof(dismiss_kinds)/sizeof(dismiss_kinds[0]));
// The level-up screen's two step screens and their tap handlers.
void *levelup_summary=nullptr,*levelup_rewards=nullptr,*levelup_claim=nullptr,*levelup_click=nullptr;
void *levelup_summary_canvas=nullptr,*levelup_rewards_canvas=nullptr;
void *levelup_summary_visibility=nullptr,*levelup_rewards_visibility=nullptr;
void *levelup_active=nullptr,*levelup_visible=nullptr,*levelup_alpha=nullptr,*levelup_interactable=nullptr;
constexpr uint64_t kLevelUpStepMs=250,kLevelUpWatchMs=30000;
struct PendingPopup{uintptr_t handle;int kind;uint64_t since,next;int steps;bool summary_claimed=false,rewards_wait=false,opened=false;};
constexpr int kPendingPopups=8;  // medals often open several at once
PendingPopup pending_popups[kPendingPopups]={};int pending_popup_count=0;
void *gui_dismiss=nullptr;
// Only the inventory created by the hatch-result dismiss callback is eligible. Manual Eggs
// access never arms this policy. Inventory stays interactive while opening. Use the same
// ClickDismiss -> MaybeDismiss path as its X button, so input/selection cleanup runs normally.
void *inventory_class=nullptr,*inventory_egg_page=nullptr,*inventory_tabs=nullptr;
void *hatch_inventory_service=nullptr,*inventory_current=nullptr;
void *inventory_context=nullptr,*inventory_close=nullptr,*inventory_active=nullptr;
void *inventory_top=nullptr,*inventory_visible=nullptr;
void *inventory_page_transform=nullptr,*inventory_tab_content=nullptr,*inventory_tab_index=nullptr;
void *inventory_tab_dragging=nullptr,*inventory_tab_distance=nullptr;
void *inventory_child_count=nullptr,*inventory_child=nullptr,*inventory_is_child=nullptr;
int32_t inventory_default_context=-1;
uintptr_t pending_inventory=0;uint64_t inventory_settled=0,inventory_clicked_at=0;
uint64_t inventory_since=0;bool inventory_opened=false;
constexpr uint64_t kPopupWatchMs=15000;
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

// K2 separates cutscene skipping from post-hatch Auto incubate cleanup; stale snapshots mean off.
struct SkipPolicy {bool cutscenes=false,inventory=false;};
SkipPolicy parse_policy(const char *s,uint32_t n){
 if(n==4&&std::memcmp(s,"K1|",3)==0&&(s[3]=='0'||s[3]=='1'))return {s[3]=='1',false};
 if(n==6&&std::memcmp(s,"K2|",3)==0&&(s[3]=='0'||s[3]=='1')&&s[4]=='|'&&(s[5]=='0'||s[5]=='1'))
  return {s[3]=='1',s[5]=='1'};
 return {};
}
SkipPolicy configured(){
 auto tr=transport();char s[16];uint32_t n=0;PgrRuntimeTransportSample a{};
 if(!tr||tr->sample(&a)||!a.snapshot_fresh||a.fault)return {};
 return tr->get_string(17,s,sizeof(s),&n)==0?parse_policy(s,n):SkipPolicy{};
}
// Skip only on the Unity thread the tick runs on, while ticks are arriving.
bool live_tick(){
 if(faulted.load(std::memory_order_acquire))return false;
 const uint64_t tid=owner.load(std::memory_order_acquire),seen=last_tick.load(std::memory_order_acquire);
 return tid&&tid==uint64_t(gettid())&&seen&&clock_ms()-seen<kTickAliveMs;
}
bool active(){return enabled.load(std::memory_order_acquire)&&live_tick();}
void release_inventory(){
 if(pending_inventory)fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(pending_inventory);
 pending_inventory=0;inventory_settled=inventory_clicked_at=0;
 inventory_since=0;inventory_opened=false;
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
// 0.435.2 EggHatchState.bnaj is the result page's dismiss callback. It either finishes
// the state or calls ShowPokemon(StartOnEggsFlags) and registers the inventory's own
// dismissal callback. Preserve all of that work, then retain only the NEW controller
// it created. Identity survives delayed opening; no recent-hatch timer can capture a
// later manual visit, and branches that create no inventory cannot capture an old one.
void hook_hatch_dismiss(void *self,void *parameters,const void *m){
 struct Roots {
  uintptr_t service=0,before=0;
  ~Roots(){for(uintptr_t h:{before,service})if(h)fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);}
 } roots;
 bool observed=false;
 if(self&&egg_inventory_enabled.load(std::memory_order_acquire)&&live_tick()&&
    inventory_class&&inventory_close&&hatch_inventory_service&&inventory_current){
  try{
   void *service=invoke(hatch_inventory_service,self);require(service,"Hatch inventory service");
   roots.service=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(service,0);require(roots.service,"Service root");
   if(void *before=invoke(inventory_current,service)){
    roots.before=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(before,0);require(roots.before,"Inventory root");
   }
   observed=true;
  }catch(...){status("warning","Post-hatch inventory tracking unavailable");}
 }
 orig<HatchResult>(K_HATCH_DISMISS)(self,parameters,m);
 if(!observed||!egg_inventory_enabled.load(std::memory_order_acquire)||!live_tick())return;
 try{
  auto target=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target);
  void *service=target(roots.service);if(!service)return;
  void *gui=invoke(inventory_current,service);
  if(!gui||(roots.before&&gui==target(roots.before))||klass(gui)!=inventory_class)return;
  release_inventory();pending_inventory=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(gui,0);
  require(pending_inventory,"Post-hatch inventory root");inventory_since=clock_ms();
  event("{\"phase\":\"post-hatch-eggs-tracked\"}");
 }catch(...){release_inventory();status("warning","Post-hatch inventory left for manual close");}
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
 if(active()&&self&&gui_dismiss&&pending_popup_count<kPendingPopups){
  try{
   void *k=klass(self);
   for(int i=0;i<kDismissKinds;++i){
    if(!dismiss_kinds[i].klass||k!=dismiss_kinds[i].klass)continue;
    const uintptr_t h=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(self,0);
    if(h){pending_popups[pending_popup_count++]={h,i,clock_ms(),0,0};if(root_canvas&&canvas_alpha)set_alpha(self,0.f);}
    break;
   }
  }catch(...){}
 }
}
void *object_field(void *o,void *f){void *v=nullptr;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;}
template<class T>T inventory_value(void *m,void *o){
 void *boxed=invoke(m,o);require(boxed,"Egg inventory state");
 return *static_cast<T*>(fn<void*(*)(void*)>(boot.runtime.object_unbox)(boxed));
}
void advance_inventory(){
 if(!pending_inventory)return;
 try{
  void *gui=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(pending_inventory);
  if(!gui){release_inventory();return;}
  const int state=int_field(gui,gui_state);
  const uint64_t now=clock_ms();
  if(state==1||state==2)inventory_opened=true;
  if(state==3||(state==0&&inventory_opened)||now-inventory_since>3000){release_inventory();return;}
  if(!egg_inventory_enabled.load(std::memory_order_acquire)||!live_tick()){
   release_inventory();return;
  }
  if(inventory_clicked_at){
   if(now-inventory_clicked_at>3000){release_inventory();status("warning","Egg inventory left for manual close");}
   return;
  }
  if(state!=2||!inventory_value<bool>(inventory_active,gui)||!inventory_value<bool>(inventory_top,gui)||
     !inventory_value<bool>(inventory_visible,gui)||
     inventory_value<int32_t>(inventory_context,gui)!=inventory_default_context){inventory_settled=0;return;}
  void *tabs=object_field(gui,inventory_tabs),*page=object_field(gui,inventory_egg_page);
  if(!tabs||!page){inventory_settled=0;return;}
  const float distance=inventory_value<float>(inventory_tab_distance,tabs);
  if(inventory_value<bool>(inventory_tab_dragging,tabs)){release_inventory();return;} // user took control
  if(!std::isfinite(distance)||std::fabs(distance)>0.01f){inventory_settled=0;return;}
  void *content=invoke(inventory_tab_content,tabs),*page_transform=invoke(inventory_page_transform,page);
  if(!content||!page_transform){inventory_settled=0;return;}
  int32_t index=inventory_value<int32_t>(inventory_tab_index,tabs);
  if(index<0||index>=inventory_value<int32_t>(inventory_child_count,content)){inventory_settled=0;return;}
  void *args[]={&index};void *selected=invoke(inventory_child,content,args);
  if(!selected){inventory_settled=0;return;}
  void *child_args[]={selected};void *boxed=invoke(inventory_is_child,page_transform,child_args);
  if(!boxed||!*static_cast<bool*>(fn<void*(*)(void*)>(boot.runtime.object_unbox)(boxed))){release_inventory();return;}
  if(!inventory_settled){inventory_settled=now;return;}
  if(now-inventory_settled<250)return;
  inventory_clicked_at=now;
  invoke(inventory_close,gui);
  event("{\"phase\":\"skipped\",\"kind\":\"egg-inventory\",\"reason\":\"post-hatch-auto-incubate\"}");
 }catch(...){release_inventory();status("warning","Egg inventory left for manual close");}
}
bool levelup_screen_ready(void *screen,void *canvas_field,void *visibility_field){
 if(!screen)return false;
 auto yes=[&](void *m,void *o){void *v=invoke(m,o);require(v,"Level-up readiness");return *static_cast<bool*>(fn<void*(*)(void*)>(boot.runtime.object_unbox)(v));};
 if(!yes(levelup_active,screen))return false;
 void *visibility=object_field(screen,visibility_field),*canvas=object_field(screen,canvas_field);
 if(!visibility||!canvas||!yes(levelup_visible,visibility)||!yes(levelup_interactable,canvas))return false;
 void *v=invoke(levelup_alpha,canvas);require(v,"Level-up alpha");
 const float alpha=*static_cast<float*>(fn<void*(*)(void*)>(boot.runtime.object_unbox)(v));
 return std::isfinite(alpha)&&alpha>=0.99f;
}
// One step for the hidden level-up screen; true once it has closed itself or is left for taps.
bool advance_levelup(PendingPopup &p,void *gui){
 const uint64_t now=clock_ms();
 const int32_t state=int_field(gui,gui_state);
 if(state==2)p.opened=true;
 if(state==0&&p.opened){event("{\"phase\":\"skipped\",\"kind\":\"level-up\",\"ms\":"+std::to_string(now-p.since)+",\"steps\":"+std::to_string(p.steps)+"}");return true;}
 if(now-p.since>kLevelUpWatchMs){set_alpha(gui,1.f);status("warning","level-up left for taps (state "+std::to_string(state)+")");return true;}
 set_alpha(gui,0.f);  // every tick: its own transitions may restore the alpha
 if(state!=2)return false;
 void *summary=object_field(gui,levelup_summary),*rewards=object_field(gui,levelup_rewards);
 const bool rewards_ready=levelup_screen_ready(rewards,levelup_rewards_canvas,levelup_rewards_visibility);
 // A registered callback is not evidence that its screen is ready. In particular the summary
 // callback exists during the coin flip/avatar-stage setup. Never advance both screens together,
 // and after a rewards click observe its transition before allowing the next group to advance.
 if(!rewards_ready)p.rewards_wait=false;
 if(now<p.next)return false;
 if(!p.summary_claimed&&levelup_screen_ready(summary,levelup_summary_canvas,levelup_summary_visibility)){
  p.summary_claimed=true;p.next=now+kLevelUpStepMs;++p.steps;invoke(levelup_claim,summary);
  event("{\"phase\":\"level-up-action\",\"action\":\"claim-rewards\"}");
 }else if(rewards_ready&&!p.rewards_wait){
  p.rewards_wait=true;p.next=now+kLevelUpStepMs;++p.steps;invoke(levelup_click,rewards);
  event("{\"phase\":\"level-up-action\",\"action\":\"next-reward-group\"}");
 }
 return false;
}
// One step for a hidden pop-up; true once it is dismissed, gone or left for taps.
bool advance_popup(PendingPopup &p){
 void *gui=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(p.handle);
 if(!gui)return true;
 const DismissKind &k=dismiss_kinds[p.kind];
 try{
  if(k.step)return advance_levelup(p,gui);
  const int32_t state=int_field(gui,gui_state);
  if(state==2){invoke(gui_dismiss,gui);event(std::string("{\"phase\":\"skipped\",\"kind\":\"")+k.event+"\",\"ms\":"+std::to_string(clock_ms()-p.since)+"}");return true;}
  if(state==0||state==3)return true;
  if(clock_ms()-p.since>kPopupWatchMs){set_alpha(gui,1.f);status("warning",std::string(k.event)+" left for taps");return true;}
  set_alpha(gui,0.f);  // every tick: its own transitions may restore the alpha
  return false;
 }catch(const Failure &f){try{set_alpha(gui,1.f);}catch(...){}status("warning",std::string(k.event)+" left for taps: "+f.where);return true;}
 catch(...){try{set_alpha(gui,1.f);}catch(...){}status("warning",std::string(k.event)+" left for taps");return true;}
}
void advance_popups(){
 int kept=0;
 for(int i=0;i<pending_popup_count;++i){
  PendingPopup p=pending_popups[i];
  if(advance_popup(p))fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(p.handle);
  else pending_popups[kept++]=p;
 }
 pending_popup_count=kept;
}
// Without ticks a hidden pop-up is shown again rather than left invisible.
void release_popups(){
 for(int i=0;i<pending_popup_count;++i){
  void *gui=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(pending_popups[i].handle);
  if(gui){try{set_alpha(gui,1.f);}catch(...){}}
  fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(pending_popups[i].handle);
 }
 pending_popup_count=0;
}

using RaidInit=void(*)(void*,void*,const void*);
void *raid_target(uintptr_t h){return h?fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h):nullptr;}
bool raid_alive(void *o){
 if(!o)return false;
 void *args[]={o};void *boxed=invoke(unity_alive,nullptr,args);require(boxed,"Unity object liveness");
 return *static_cast<bool*>(fn<void*(*)(void*)>(boot.runtime.object_unbox)(boxed));
}
float raid_get_speed(void *animator){
 void *boxed=invoke(animator_speed_get,animator);require(boxed,"Raid animator speed");
 const float v=*static_cast<float*>(fn<void*(*)(void*)>(boot.runtime.object_unbox)(boxed));
 require(std::isfinite(v)&&v>=0.f,"Raid animator speed value");return v;
}
void raid_set_alpha(void *group,float alpha){void *args[]={&alpha};invoke(canvas_alpha,group,args);}
void raid_set_raycasts(void *group,bool value){void *args[]={&value};invoke(canvas_raycasts,group,args);}
void raid_free(uintptr_t &h){if(h)fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);h=0;}
// Each saved component is rooted independently: the controller can be destroyed during an animator
// callback. A failed managed call is NOT evidence of destruction, and must never report success.
bool restore_raid(bool reveal){
 bool ok=true;
 for(auto &c:raid_canvases){
  void *group=raid_target(c.handle);
  // GuiController.CleanUp hides its canvas on dismissal. Do not re-enable input on that hidden
  // screen: only a fallback to the still-open UI restores its original raycast setting.
  try{if((!reveal||c.raycasts_changed)&&raid_alive(group)){
   if(!reveal)c.raycasts_changed=true;
   raid_set_raycasts(group,reveal?c.raycasts:false);
  }}catch(...){ok=false;}
  // Attempt both properties independently; one failure must not skip the other components.
  if(reveal)try{if(raid_alive(group))raid_set_alpha(group,1.f);}catch(...){ok=false;}
 }
 try{void *animator=raid_target(raid_animator);if(raid_alive(animator)&&raid_get_speed(animator)==raid_fast_speed){
  void *args[]={&raid_speed};invoke(animator_speed_set,animator,args);
 }}catch(...){ok=false;}
 return ok;
}
void release_raid(){
 for(auto &c:raid_canvases){raid_free(c.handle);c={};}
 raid_free(raid_animator);raid_free(pending_raid);raid_recovering=false;raid_finished=false;raid_confirmed=false;
}
void recover_raid(const char *reason){
 if(!raid_recovering)status("warning",std::string("Raid skip stopped; restoring screen state: ")+reason);
 raid_recovering=true;
 if(restore_raid(!raid_finished))release_raid();
 else if(clock_ms()-raid_since>=kRaidRecoveryMs){
  status("error","Raid controls could not be fully restored; restart the game");release_raid();
 }
}
void capture_raid_canvas(void *group){
 require(raid_alive(group),"Raid canvas unavailable");
 for(auto &c:raid_canvases)if(raid_target(c.handle)==group)return;
 for(auto &c:raid_canvases)if(!c.handle){
  void *boxed=invoke(canvas_raycasts_get,group);require(boxed,"Raid canvas input state");
  c.raycasts=*static_cast<bool*>(fn<void*(*)(void*)>(boot.runtime.object_unbox)(boxed));
  c.handle=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(group,0);require(c.handle,"Raid canvas root");return;
 }
 throw Failure{"Raid canvas changed"};
}
void prepare_raid(void *gui){
 // The subclass's rootCanvasGroup is separate from GuiController's cached root canvas.
 capture_raid_canvas(object_field(gui,raid_canvas_field));
 capture_raid_canvas(invoke(root_canvas,gui));
 if(!raid_animator){
  void *animator=object_field(gui,raid_animator_field);require(raid_alive(animator),"Raid animator unavailable");
  raid_speed=raid_get_speed(animator);raid_fast_speed=std::max(raid_speed,20.f);
  raid_animator=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(animator,0);require(raid_animator,"Raid animator root");
  void *args[]={&raid_fast_speed};invoke(animator_speed_set,animator,args);
 }
 for(auto &c:raid_canvases)if(c.handle)raid_set_alpha(raid_target(c.handle),0.f);
}
bool raid_bool(void *method,void *object){
 void *boxed=invoke(method,object);require(boxed,"Raid button state");
 return *static_cast<bool*>(fn<void*(*)(void*)>(boot.runtime.object_unbox)(boxed));
}
bool raid_wait(int state,const char *reason){
 if(raid_wait_state!=state){
  raid_wait_state=state;
  event(std::string("{\"phase\":\"raid-results-wait\",\"reason\":\"")+reason+"\"}");
 }
 return false;
}
bool confirm_raid(void *gui){
 // CancelAreaTapped advances animations but cannot press GO TO BONUS CHALLENGE. The final
 // confirmation row is activated by the summary sequence; never bypass that readiness boundary.
 void *row=object_field(gui,raid_confirm_field);if(!row)return raid_wait(0,"confirmation-row-missing");
 void *root=object_field(row,raid_confirm_root);
 if(!raid_alive(root)||!raid_bool(game_object_active,root))return raid_wait(1,"confirmation-row-inactive");
 bool include_inactive=false;void *args[]={raid_button_type,&include_inactive};
 void *button=invoke(component_in_children,root,args);
 if(!raid_alive(button))return raid_wait(2,"confirmation-button-missing");
 if(!raid_bool(button_active,button))return raid_wait(3,"confirmation-button-inactive");
 if(!raid_bool(button_interactable,button))return raid_wait(4,"confirmation-button-not-interactable");
 // Press checks normal button eligibility and invokes its installed click listeners. This is a
 // managed callback, not a physical tap, and does not depend on the hidden canvas's alpha.
 raid_confirmed=true;
 event("{\"phase\":\"raid-results-action\",\"action\":\"bonus-challenge\"}");
 invoke(button_press,button);return true;
}
void step_raid(void *gui){
 if(raid_confirmed)return; // the final callback may finish asynchronously; never double-submit
 // Only invoke the leaderboard callback while the game has actually installed it for this screen.
 // Calling CloseLeaderboard at other stages would corrupt its saved back-button action.
 void *action=object_field(gui,gui_back_action);
 if(action&&invoke(delegate_target,action)==gui){
  void *reflected=invoke(delegate_method,action);require(reflected,"Raid back action method");
  if(fn<void*(*)(void*)>(reflection_method)(reflected)==raid_leaderboard_close){
   invoke(action_invoke,action);return;
  }
 }
 if(confirm_raid(gui))return;
 invoke(raid_tap,gui);
}
void hook_raid_results(void *self,void *info,const void *m){
 orig<RaidInit>(K_RAID_RESULTS)(self,info,m);
 if(!active()||!self||!raid_tap||pending_raid)return;
 try{
  pending_raid=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(self,0);require(pending_raid,"GC root");
  raid_since=clock_ms();raid_next=0;raid_last_state=-1;raid_wait_state=-1;raid_recovering=false;raid_finished=false;raid_confirmed=false;raid_taps=0;
 }catch(...){status("warning","Raid results screen could not be skipped");}
}
// One step for a raid results screen; ends once it has fully dismissed, or is left for taps.
void advance_raid(bool on){
 if(!on||raid_recovering){recover_raid(on?"retrying component restoration":"setting disabled");return;}
 void *gui=raid_target(pending_raid);const uint64_t now=clock_ms();
 try{
  const bool alive=raid_alive(gui);
  const int32_t state=alive?int_field(gui,gui_state):-1;
  if(!alive||(state==0&&raid_last_state>0)){
   raid_finished=true;
   require(restore_raid(false),"Raid component restoration");
   if(raid_taps)event("{\"phase\":\"skipped\",\"kind\":\"raid-results\",\"completion\":\""+
    std::string(alive?"dismissed":"destroyed")+"\",\"ms\":"+std::to_string(now-raid_since)+",\"taps\":"+std::to_string(raid_taps)+"}");
   release_raid();return;
  }
  if(state!=raid_last_state){
   event("{\"phase\":\"raid-results-state\",\"state\":"+std::to_string(state)+",\"ms\":"+std::to_string(now-raid_since)+"}");
   raid_last_state=state;
  }
  if(now-raid_since>=kRaidWatchMs){recover_raid("timed out waiting for normal dismissal");return;}
  if(int_field(gui,raid_status)!=raid_victory){
   if(state==2||now-raid_since>=kRewardsOpenWaitMs){
    if(restore_raid(true))release_raid();else recover_raid("battle result changed");
   }
   return;
  }
  if(state==3){
   // The game started dismissal itself. Only the captured screen's canvases stop intercepting input.
   for(auto &c:raid_canvases){void *group=raid_target(c.handle);if(raid_alive(group)){
    // Snapshot when taking ownership, not while the opening transition may still disable input.
    if(!c.raycasts_changed){
     void *boxed=invoke(canvas_raycasts_get,group);require(boxed,"Raid canvas input state");
     c.raycasts=*static_cast<bool*>(fn<void*(*)(void*)>(boot.runtime.object_unbox)(boxed));
     c.raycasts_changed=true;
    }
    raid_set_raycasts(group,false);
   }}
   return;
  }
  if(state!=1&&state!=2)return;
  prepare_raid(gui);
  if(state!=2||pending_dialog)return;
  if(!raid_next){raid_next=now+kFastSettleMs;return;}
  if(now<raid_next)return;
  raid_next=now+kRaidStepMs;++raid_taps;step_raid(gui);
 }catch(const Failure &f){recover_raid(f.where);}
 catch(...){recover_raid("unexpected native error");}
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

 // Resolve the normal raid tap actions and per-screen components; no early Close shortcut.
 try{
  require(gui_state&&root_canvas&&canvas_alpha,"GuiController state");
  void *gui=class_of(api,"Niantic.Holoholo.Raid","RaidResolveUIController");
  raid_tap=method(gui,"CancelAreaTapped",0);
  raid_leaderboard_close=method(gui,"CloseLeaderboard",0);
  canvas_raycasts=method(class_of(api,"UnityEngine","CanvasGroup"),"set_blocksRaycasts",1);
  canvas_raycasts_get=method(class_of(api,"UnityEngine","CanvasGroup"),"get_blocksRaycasts",0);
  unity_alive=method(class_of(api,"UnityEngine","Object"),"op_Implicit",1);
  animator_speed_get=method(class_of(api,"UnityEngine","Animator"),"get_speed",0);
  animator_speed_set=method(class_of(api,"UnityEngine","Animator"),"set_speed",1);
  auto named_field=[&](void *owner,const char *name,const char *type){
   void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(owner,name);require(f,name);
   require(typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))==type,name);return f;
  };
  raid_animator_field=named_field(gui,"animator","UnityEngine.Animator");
  raid_canvas_field=named_field(gui,"rootCanvasGroup","UnityEngine.CanvasGroup");
  gui_back_action=named_field(class_of(api,"Niantic.Holoholo.NUI","GuiController"),"customBackButtonAction","System.Action");
  raid_confirm_field=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(gui,"confirmBonusRow");require(raid_confirm_field,"confirmBonusRow");
  void *row_class=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(raid_confirm_field));require(row_class,"ConfirmBonusRow class");
  raid_confirm_root=named_field(row_class,"Root","UnityEngine.GameObject");
  void *game_object=class_of(api,"UnityEngine","GameObject"),*button=class_of(api,"UnityEngine.UI","Button");
  game_object_active=method(game_object,"get_activeInHierarchy",0);
  component_in_children=method(game_object,"GetComponentInChildren",2); // (System.Type, bool)
  button_active=method(class_of(api,"UnityEngine","Behaviour"),"get_isActiveAndEnabled",0);
  button_interactable=method(class_of(api,"UnityEngine.UI","Selectable"),"IsInteractable",0);
  button_press=method(button,"Press",0);
  raid_button_type=fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(button));require(raid_button_type,"Button reflection type");
  delegate_method=method(class_of(api,"System","Delegate"),"get_Method",0);
  delegate_target=method(class_of(api,"System","Delegate"),"get_Target",0);
  action_invoke=method(class_of(api,"System","Action"),"Invoke",0);
  reflection_method=exp("method_get_from_reflection");
  raid_status=field_of_type(gui,"Niantic.Holoholo.Battle.BattleStatus");
  void *status_class=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(raid_status));
  void *victory=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(status_class,"Victory");require(victory,"BattleStatus.Victory");
  fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(victory,&raid_victory);require(raid_victory>0,"BattleStatus.Victory value");
  static const uint8_t p_raid[]=PGR_GAME_CUTSCENE_SKIP_CPP_RAID_RESULTS_FINGERPRINT;
  static_assert(sizeof(p_raid)==24,"24-byte prologue");
  const Target t{K_RAID_RESULTS,gui,"Initialize",1,PGR_GAME_CUTSCENE_SKIP_CPP_RAID_RESULTS_RVA,p_raid,reinterpret_cast<void*>(hook_raid_results),false};
  if(hook(api,t,missing))++count;else raid_tap=nullptr;
 }catch(const Failure &f){raid_tap=nullptr;missing+=std::string(missing.empty()?"":", ")+"raid-results ("+f.where+")";}

 // The speed warning, level-up screen and medal pop-ups: dismissed through GuiController.Dismiss once
 // open (no hooks of their own). Each class is optional.
 try{
  require(gui_state&&root_canvas&&canvas_alpha,"GuiController state");
  gui_dismiss=method(class_of(api,"Niantic.Holoholo.NUI","GuiController"),"Dismiss",0);
  for(auto &k:dismiss_kinds){
   try{
    k.klass=class_of(api,k.ns,k.name);
    if(k.step){
     auto field=[&](const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k.klass,name);require(f,name);return f;};
     levelup_summary=field("levelUpSummaryScreen");levelup_rewards=field("levelUpRewardsScreen");
     levelup_claim=method(class_of(api,"Niantic.Holoholo.UI","LevelUpSummaryScreen"),"ClaimRewardsButtonClicked",0);
     levelup_click=method(class_of(api,"Niantic.Holoholo.UI","LevelUpRewardsScreen"),"OnScreenClicked",0);
     auto child_field=[&](const char *type,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(class_of(api,"Niantic.Holoholo.UI",type),name);require(f,name);return f;};
     levelup_summary_canvas=child_field("LevelUpSummaryScreen","canvasGroup");
     levelup_rewards_canvas=child_field("LevelUpRewardsScreen","canvasGroup");
     levelup_summary_visibility=child_field("LevelUpSummaryScreen","visibilityController");
     levelup_rewards_visibility=child_field("LevelUpRewardsScreen","visibilityController");
     levelup_active=method(class_of(api,"UnityEngine","Behaviour"),"get_isActiveAndEnabled",0);
     levelup_visible=method(class_of(api,"Niantic.Holoholo.UI","VisibilityController"),"get_IsVisible",0);
     levelup_alpha=method(class_of(api,"UnityEngine","CanvasGroup"),"get_alpha",0);
     levelup_interactable=method(class_of(api,"UnityEngine","CanvasGroup"),"get_interactable",0);
    }
   }catch(const Failure &f){k.klass=nullptr;missing+=std::string(missing.empty()?"":", ")+k.event+" ("+f.where+")";}
  }
 }catch(const Failure &f){gui_dismiss=nullptr;missing+=std::string(missing.empty()?"":", ")+"pop-ups ("+f.where+")";}

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

 // The auto-incubation inventory policy captures only the post-hatch callback's new GUI. Resolve tab
 // ownership structurally: no hardcoded tab index, and no assumption that a hidden egg page
 // means the user is looking at it. All views of Pokemon/Tags/party selection remain untouched.
 try{
  require(gui_state,"Inventory GUI state");
  void *inventory=class_of(api,"Niantic.Holoholo.UI.Pokemon","PokemonInventoryGuiController");
  inventory_egg_page=field_of_type(inventory,"Niantic.Holoholo.UI.IEggListPage");
  inventory_tabs=field_of_type(inventory,"Niantic.Holoholo.Widget.TabView");
  inventory_context=method(inventory,"get_InventoryContext",0);
  inventory_close=method(inventory,"ClickDismiss",0);
  inventory_active=method(class_of(api,"UnityEngine","Behaviour"),"get_isActiveAndEnabled",0);
  void *base_gui=class_of(api,"Niantic.Holoholo.NUI","GuiController");
  inventory_top=method(base_gui,"get_IsTopGui",0);inventory_visible=method(base_gui,"get_IsVisible",0);
  void *context_class=class_of(api,"Niantic.Holoholo.UI.Pokemon","PokemonInventoryContext");
  void *normal=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(context_class,"Default");require(normal,"Inventory context Default");
  fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(normal,&inventory_default_context);
  inventory_page_transform=method(class_of(api,"Niantic.Holoholo.UI","EggListPage"),"get_Transform",0);
  void *tabs=class_of(api,"Niantic.Holoholo.Widget","TabView");
  inventory_tab_content=method(tabs,"get_Content",0);inventory_tab_index=method(tabs,"get_SelectedTabIndex",0);
  inventory_tab_dragging=method(tabs,"get_IsDragging",0);inventory_tab_distance=method(tabs,"get_DistanceToTargetIndex",0);
  void *transform=class_of(api,"UnityEngine","Transform");
  inventory_child_count=method(transform,"get_childCount",0);inventory_child=method(transform,"GetChild",1);
  inventory_is_child=method(transform,"IsChildOf",1);
  inventory_class=inventory;
  void *hatch_state=class_of(api,"Niantic.Holoholo.EggHatch","EggHatchState");
  hatch_inventory_service=method(hatch_state,PGR_GAME_CUTSCENE_SKIP_CPP_HATCH_INVENTORY_SERVICE_NAME,0);
  inventory_current=method(class_of(api,"Niantic.Holoholo.UI.Pokemon","PokemonInventoryGuiService"),"get_ActivePokemonInventoryGui",0);
  static const uint8_t p_hatch_dismiss[]=PGR_GAME_CUTSCENE_SKIP_CPP_HATCH_DISMISS_FINGERPRINT;
  static_assert(sizeof(p_hatch_dismiss)==24,"24-byte prologue");
  const Target t{K_HATCH_DISMISS,hatch_state,PGR_GAME_CUTSCENE_SKIP_CPP_HATCH_DISMISS_NAME,1,
   PGR_GAME_CUTSCENE_SKIP_CPP_HATCH_DISMISS_RVA,p_hatch_dismiss,reinterpret_cast<void*>(hook_hatch_dismiss),false};
  if(hook(api,t,missing)){++count;event("{\"phase\":\"post-hatch-eggs-ready\"}");}
  else inventory_class=nullptr;
 }catch(const Failure &f){inventory_class=nullptr;status("warning",std::string("Egg inventory dismissal unavailable: ")+f.where);}

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
 const SkipPolicy policy=configured();const bool on=policy.cutscenes;
 enabled.store(on,std::memory_order_release);egg_inventory_enabled.store(policy.inventory,std::memory_order_release);
 if(pending_dialog)finish_dialog();
 if(pending_egg)advance_egg();
 if(pending_dismiss)finish_dismiss();
 if(pending_radar)finish_radar();
 if(pending_rewards)advance_rewards();
 if(fast_rewards)advance_fast();
 if(pending_raid)advance_raid(on&&!faulted.load(std::memory_order_acquire));
 if(pending_popup_count)advance_popups();
 if(pending_inventory)advance_inventory();
 if((!on&&!policy.inventory)||tried||faulted.load(std::memory_order_acquire))return;
 tried=true;
 try{
  if(!ready){auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(api&&api->snapshot(&boot)==0,"Runtime unavailable");ready=true;}
  install();
 }catch(const Failure &f){faulted.store(true,std::memory_order_release);status("error",std::string("Skip cutscenes unavailable: ")+f.where);}
 catch(...){faulted.store(true,std::memory_order_release);status("error","Skip cutscenes unavailable: native failure");}
}
void pgr_cutscene_skip_stop(uint64_t tid){
 enabled.store(false,std::memory_order_release);egg_inventory_enabled.store(false,std::memory_order_release);last_tick.store(0,std::memory_order_release);
 // No more ticks: a dialogue already bypassed must not be left waiting for one.
 if(!busy&&tid&&tid==uint64_t(gettid())&&tid==owner.load(std::memory_order_acquire)){
  if(pending_dialog)finish_dialog();
  if(pending_dismiss)finish_dismiss();
  if(pending_radar)finish_radar();
  if(pending_popup_count)release_popups();
  if(pending_inventory)release_inventory();
  if(pending_raid){
   // Keep roots on restoration failure; the next valid tick retries without advancing the sequence.
   raid_recovering=true;
   if(restore_raid(!raid_finished))release_raid();
   else status("error","Raid controls restoration pending after app suspension");
  }
 }
}

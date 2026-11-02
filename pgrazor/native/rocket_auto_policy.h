#pragma once
#include <stdint.h>
// Full-auto flow policy only. No pointers, hooks or calls into the game.
namespace pgr_rocket_auto {
enum Action {None,OpenStop,ExitStop,Challenge,DialogBattle,DialogNext,NpcNext,Radar,Submit,Rescue,Done,SelectPremier,Swipe};
inline unsigned combat_mask(unsigned mask){return mask<=63?((mask&7)|((mask&8)?7:0)):0;}
struct Cache {
 struct Entry {uint64_t key=0,at=0;bool consumed=false;} entries[64]{};
 Entry *find(uint64_t key,uint64_t now,uint64_t age){for(auto&e:entries){if(e.key&&now>=e.at&&now-e.at>age)e={};if(e.key==key&&key)return &e;}return nullptr;}
 Entry &put(uint64_t key,uint64_t now){for(auto&e:entries)if(e.key==key&&key){e={key,now,false};return e;}Entry*slot=&entries[0];for(auto&e:entries){if(!e.key){slot=&e;break;}if(e.at<slot->at)slot=&e;}*slot={key,now,false};return *slot;}
 void clear(){for(auto&e:entries)e={};}
};
struct Frame {
 uint64_t now=0,map=0,stop=0,spin=0,intro=0,dialog=0,dialog_button=0,npc=0,party=0,result=0,encounter=0,interaction=0,ball=0,radar=0;
 unsigned mask=0;bool authorized=false,rocket_scope=false,rocket_battle=false,leader=false,ball_in_play=false,capture_supported=false;int ball_type=-1;
};
struct Plan {Action action=None;uint64_t key=0;};
struct Policy {
 Cache tried,clicked,seen,pressed,submitted;
 uint64_t next=0,map_next=0,flow_until=0,exit_pending=0,exit_delay=700,battle_at=0,dialog_at=0,result_key=0,result_at=0,last_throw=0,selected_at=0;
 unsigned throws=0;bool rescue_tried=false,was_encounter=false,combat_seen=false;
 bool flow(uint64_t now)const{return now<flow_until;}
 void suspend(){next=map_next=flow_until=exit_pending=battle_at=dialog_at=result_key=result_at=last_throw=selected_at=0;throws=0;rescue_tried=was_encounter=combat_seen=false;clicked.clear();seen.clear();pressed.clear();submitted.clear();}
 bool stop_tried(uint64_t key,uint64_t now){return tried.find(key,now,90000)!=nullptr;}
 bool settled(uint64_t key,uint64_t now,uint64_t wait,uint64_t cooldown){auto*e=seen.find(key,now,30000);if(!e){seen.put(key,now);return false;}if(now<e->at||now-e->at<wait)return false;auto*p=pressed.find(key,now,30000);return !p||(now>=p->at&&now-p->at>=cooldown);}
 Plan battle(Action kind,uint64_t key,uint64_t now,uint64_t context=0){pressed.put(context?context:key,now);battle_at=now;submitted.clear();flow_until=now+120000;return {kind,key};}
 Plan step(const Frame &f){
  const auto now=f.now;if(!f.authorized||f.mask>63||!(f.mask&8)){suspend();return {};}
  if(now<next)return {};next=now+400;
  // A scope starts on entry to Rocket, before BATTLE and party selection.
  // Only a held CombatUI/vNext UI blocks pre-battle actions.
  if(f.rocket_scope||f.rocket_battle)flow_until=now+120000;
  if(f.rocket_battle)combat_seen=true;
  bool in_flow=flow(now);
  if(exit_pending){if(now-exit_pending>15000||f.dialog||f.intro)exit_pending=0;else if(f.spin&&now-exit_pending>=exit_delay){exit_pending=0;return {ExitStop,f.spin};}}
  if(in_flow&&!f.rocket_battle&&!combat_seen&&f.intro&&settled(f.intro,now,800,10000))return battle(Challenge,f.intro,now);
  if(in_flow&&(f.mask&16)&&f.radar&&battle_at&&now-battle_at<15000&&settled(f.radar,now,600,3000))return battle(Radar,f.radar,now);
  if(f.dialog&&!f.rocket_battle){
   if(f.dialog_button&&!combat_seen){if(settled(f.dialog,now,800,3000))return battle(DialogBattle,f.dialog_button,now,f.dialog);flow_until=now+120000;if(!f.party)return {};}
   else {auto*e=clicked.find(f.dialog,now,30000);if(!e||now-e->at>=700){clicked.put(f.dialog,now);dialog_at=now;submitted.clear();flow_until=now+120000;return {DialogNext,f.dialog};}}
  }
  if(in_flow&&!f.rocket_battle&&!combat_seen&&f.npc){auto*e=clicked.find(f.npc,now,30000);if(!e||now-e->at>=700){clicked.put(f.npc,now);dialog_at=now;flow_until=now+120000;return {NpcNext,f.npc};}}
  uint64_t started=battle_at>dialog_at?battle_at:dialog_at;
  if(in_flow&&!f.rocket_battle&&!combat_seen&&started&&now>=started&&now-started<20000&&f.party){auto*e=submitted.find(f.party,now,60000);if(!e)submitted.put(f.party,now);else if(!e->consumed&&now-e->at>=1200){e->consumed=true;return {Submit,f.party};}}
  if(f.result){
   if(result_key!=f.result){result_key=f.result;result_at=now;rescue_tried=false;}
   if(now-result_at>=1500){if(!rescue_tried){rescue_tried=true;result_at=now;throws=0;flow_until=now+120000;return {Rescue,f.result};}if(now-result_at>=2500){result_at=now;flow_until=now+6000;return {Done,f.result};}}
  }else result_key=0;
  if(in_flow&&f.capture_supported){
   if(!f.encounter&&was_encounter){was_encounter=false;if(flow_until>now+8000)flow_until=now+8000;}
   if(f.encounter){flow_until=now+120000;was_encounter=true;if(f.interaction&&!f.ball_in_play&&now>=last_throw+4000&&f.ball){if(f.ball_type!=5&&now>=selected_at+1500){selected_at=now;return {SelectPremier,f.interaction};}if(f.ball_type==5){last_throw=now;++throws;return {Swipe,f.interaction};}}return {};}
  }
  if(!in_flow&&now>=map_next){map_next=now+2000;if(f.map&&f.stop&&!stop_tried(f.stop,now)){tried.put(f.stop,now);combat_seen=false;exit_pending=now;exit_delay=f.leader?6000:700;flow_until=now+120000;return {OpenStop,f.stop};}}
  return {};
 }
};
}

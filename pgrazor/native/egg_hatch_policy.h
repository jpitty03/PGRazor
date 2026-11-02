#pragma once
#include <stdint.h>
// Pure decision engine. IDs are opaque rooted-object identities supplied by the owner.
namespace pgr_egg {
struct Observation {bool active=false,gui=false,info=false,screen=false,special=true;uint64_t info_id=0,screen_id=0;};
enum Action : unsigned { SKIP=1,FAST=2,TAP=4,CLOSE=8,START=16,KEEP=32,TIMEOUT=64 };
struct Policy {
 bool was_active=false,was_gui=false,session=false,done=false,native=false,native_ok=false,fast=false,screen_seen=false;
 uint64_t first=0,seen=0,scene_seen=0,last=0,native_at=0,screen_at=0,info_id=0,screen_id=0,closed_at=0;
 unsigned taps=0,close_attempts=0;uint64_t close_next=0;
 void close_result(uint64_t now,bool ok){if(ok&&session&&screen_seen&&close_attempts&&!done)close_next=now+1000;}
 bool config_seen=false,config_keep=true,await_new_scene=false;
 uint64_t cancelled_info=0;
 void suspend(){session=false;done=false;screen_seen=false;} // retain visibility edges across leases
 void skip_result(bool ok){native_ok=ok;}
 unsigned step(uint64_t now,bool enabled,bool keep,const Observation &o){
  if(config_seen&&keep!=config_keep){suspend();await_new_scene=true;cancelled_info=info_id;config_keep=keep;return 0;}
  config_seen=true;config_keep=keep;
  if(!enabled){suspend();return 0;}
  const bool starting=o.active&&!was_active,opening=o.gui&&!was_gui&&!o.screen&&(!closed_at||now-closed_at>=3000);
  was_active=o.active;was_gui=o.gui;
  if(await_new_scene){
   const bool new_info=o.info&&o.info_id&&o.info_id!=cancelled_info;
   if(!o.screen&&(starting||opening||new_info))await_new_scene=false;
   else {if(!o.active&&!o.gui&&!o.info&&!o.screen)await_new_scene=false;return 0;}
  }
  if(session&&!o.active&&!o.gui&&!o.info&&!o.screen&&now-seen>3000)session=false;
  unsigned actions=0;
  if(!o.screen&&(o.active||o.gui||o.info)&&((!session&&(starting||o.info||opening))||
    (session&&done&&(starting||(o.info&&o.info_id!=info_id)||opening)))){
   session=true;done=native=native_ok=fast=screen_seen=false;first=seen=now;scene_seen=last=native_at=screen_at=0;taps=close_attempts=0;close_next=0;
   info_id=o.info_id;screen_id=0;actions|=START;
  }
  if(!session||done)return actions;
  if(o.info)info_id=o.info_id;
  if(o.active||o.gui||o.info||o.screen)seen=now;
  if(now-first>45000){done=true;return actions|TIMEOUT;}
  if(o.screen){
   if(!screen_seen){screen_seen=true;screen_at=now;screen_id=o.screen_id;
    if(keep&&o.special){done=true;return actions|KEEP;}}
   // A different summary is no longer attributable to this hatch.
   if(screen_id!=o.screen_id){done=true;return actions;}
   if(now-screen_at>=1000&&now>=close_next){
    if(close_attempts>=6){done=true;return actions|TIMEOUT;}
    ++close_attempts;close_next=now+750;actions|=CLOSE;
   }
   return actions;
  }
  // Once a summary has appeared, never tap another view in this session.
  if(screen_seen){done=true;if(close_attempts)closed_at=now;return actions;}
  if(o.info||o.gui){
   const bool steady=scene_seen&&now-scene_seen<=400;scene_seen=now;
   if(!native&&o.info&&steady&&now-first>=600){native=true;native_at=now;actions|=SKIP;return actions;}
   const bool waiting=native&&native_ok&&now-native_at<3000;
   if(!waiting&&steady&&now-first>=700&&(!last||now-last>=900)&&taps<20){
    if(!fast&&o.info){fast=true;actions|=FAST;}++taps;last=now;actions|=TAP;
   }
  }else scene_seen=0;
  return actions;
 }
};
}

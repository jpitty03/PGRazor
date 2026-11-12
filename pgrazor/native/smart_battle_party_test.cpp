#include "smart_battle_party.h"
#include "battle_counter.h"
#include <cstdio>
#include <cstdlib>
using namespace pgr_smart_party;
void check(bool ok,const char *what){if(!ok){std::printf("FAIL %s\n",what);std::exit(1);}}
int main(){
    // Names from 0.435.1 InvasionCharacter metadata, in HoloPokemonType order.
    const char *types[]={"Normal","Fighting","Flying","Poison","Ground","Rock","Bug","Ghost","Metal","Fire","Water","Grass","Electric","Psychic","Ice","Dragon","Dark","Fairy"};
    for(int i=0;i<18;++i)for(const char *suffix:{"GruntMale","GruntFemale","BalloonGruntMale","BalloonGruntFemale"}){
        const std::string name=std::string("Character")+types[i]+suffix;
        check(grunt_type(name.c_str())==i+1,name.c_str());
    }
    check(grunt_type("CHARACTER_FIRE_GRUNT_MALE")==10,"protobuf spelling");
    for(const char *name:{"CharacterDarknessGruntMale","CharacterGruntMale","CharacterBalloonGruntFemale","CharacterExecutiveArlo","CharacterDecoyGruntMale","CharacterGiovanni","CharacterFireGruntMaleExtra","CharacterGruntbFemale"})
        check(grunt_type(name)==0,"unknown line-up remains unknown");
    check(grunt_type(nullptr)==0,"missing name");
    // One saved Pokemon, an empty party, or three slots all request three
    // distinct healthy counters. Gyms retain their existing six-slot capacity.
    const std::vector<std::vector<double>> scores={{4},{10},{7},{2}};
    for(int n:{0,1,2,3}){
        const auto picked=pgr_counter::pick_team(scores,1,team_size(n,true));
        check(picked==std::vector<int>({1,2,0}),"Rocket fills three strongest distinct candidates");
    }
    check(team_size(6,false)==6,"gym keeps six");
    check(team_size(0,false)==0,"empty non-Rocket party unchanged");
    check(team_size(7,true)==0,"invalid length rejected");
    for(int threshold:{25,50,75,100}){
        const std::string settings="H2|1|2|"+std::to_string(threshold)+"|1|0";
        const auto healing=healing_policy(settings.c_str());
        check(healing.on&&healing.threshold==threshold,"read active healing threshold");
        check(selection_hp(threshold,100,healing)==100,"exact healing boundary uses full HP");
        check(selection_hp(threshold-1,100,healing)==threshold-1,"below boundary keeps current HP");
        if(threshold<100)check(selection_hp(threshold+1,100,healing)==100,"above boundary uses full HP");
        check(selection_hp(0,100,healing)==0,"fainted Pokemon stays ineligible");
        check(selection_hp(75,0,healing)==75,"invalid max HP falls back");
        check(selection_hp(75,50,healing)==75,"inconsistent HP falls back");
    }
    for(const char *settings:{"H2|0|2|50|1|0","H1|1|2|50|1|0","H2|1|2|51|1|0","H2|1|2|50|1|0junk","H2|1|2|50|1","H2|1|3|50|1|0",""}){
        check(!healing_policy(settings).on,"disabled or malformed healing uses default selection");
        check(selection_hp(75,100,healing_policy(settings))==75,"current HP retained by default");
    }
    check(!healing_policy(nullptr).on,"absent healing settings default off");
    // A slightly stronger counter at 60% loses on current HP, but wins when
    // Smart healing is on at 50%. At a 75% threshold it retains its HP penalty.
    pgr_counter::Fighter better,healthy;
    better.attack=healthy.attack=200;better.defense=healthy.defense=150;
    better.hp=60;healthy.hp=100;
    better.fast=healthy.fast=pgr_counter::Move{1,100,1,10,true};
    better.attack=220;
    pgr_counter::Foe target;
    auto score=[&](pgr_counter::Fighter f,const HealingPolicy &policy){
        f.hp=selection_hp(int(f.hp),100,policy);return pgr_counter::matchup(f,target);
    };
    check(score(better,{})<score(healthy,{}),"healing off: healthier candidate wins as before");
    check(score(better,{true,50})>score(healthy,{true,50}),"healing on: best counter above threshold wins");
    check(score(better,{true,75})<score(healthy,{true,75}),"below threshold: current HP still matters");
    std::puts("PASS smart_battle_party: grunt selection and healing-aware pre-battle ranking");
}

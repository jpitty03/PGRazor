#pragma once
#include <stdint.h>
#include <map>
#include <vector>
namespace pgr_heal {
struct Pokemon {uint64_t id=0;int hp=0,max=0;bool egg=false;};
struct Choice {uint64_t id=0;int item=-1,hp=0,max=0;};
inline bool valid(const Pokemon&p){return p.id&&!p.egg&&p.max>0&&p.hp>=0&&p.hp<=p.max;}
inline bool eligible(const Pokemon&p,unsigned mask){return valid(p)&&((p.hp==0&&(mask&1))||(p.hp>0&&(mask&2)&&int64_t(p.hp)*2<p.max));}
// Item slots: Potion, Super Potion, Hyper Potion, Max Potion, Revive, Max Revive.
inline int choose(const int stock[6],const Pokemon&p){
 if(p.hp==0)return stock[5]>0?5:stock[4]>0?4:-1;
 const int missing=p.max-p.hp,amount[4]={20,50,200,INT32_MAX};int largest=-1;
 for(int i=0;i<4;i++)if(stock[i]>0){largest=i;if(amount[i]>=missing)return i;}
 return largest;
}
struct Policy {
 std::map<uint64_t,bool> healing;std::map<uint64_t,uint64_t> later;
 Choice pending{};uint64_t deadline=0,next=0;bool timed_out=false,confirmed=false;
 void reset(){healing.clear();later.clear();pending={};deadline=next=0;}
 Choice step(uint64_t now,unsigned mask,const std::vector<Pokemon>&rows,const int stock[6]){
  timed_out=confirmed=false;if(!mask||mask>3){reset();return {};}
  if(now<next)return {};next=now+1500;
  std::map<uint64_t,Pokemon> seen;
  for(auto &p:rows)if(valid(p))seen[p.id]=p;
  for(auto it=later.begin();it!=later.end();)if(now>=it->second)it=later.erase(it);else ++it;
  for(auto it=healing.begin();it!=healing.end();) {auto p=seen.find(it->first);if(p==seen.end()||p->second.hp>=p->second.max)it=healing.erase(it);else ++it;}
  if(pending.id){auto p=seen.find(pending.id);
   if(p==seen.end()){healing.erase(pending.id);pending={};}
   else if(p->second.hp!=pending.hp){confirmed=true;pending={};}
   else if(now>=deadline){timed_out=true;healing.erase(pending.id);later[pending.id]=now+60000;pending={};}
   else return {};
  }
  for(auto&p:seen)if(eligible(p.second,mask)&&!later.count(p.first))healing[p.first]=true;
  for(auto&p:seen)if(healing.count(p.first)&&!later.count(p.first)){
   int item=choose(stock,p.second);if(item<0)continue;
   pending={p.first,item,p.second.hp,p.second.max};deadline=now+10000;return pending;
  }
  return {};
 }
};
}

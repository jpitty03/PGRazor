#pragma once
#include <cstdint>
#include <set>
#include <string>
namespace pgr_evolve_transfer {
struct Config {
 bool enabled=false;
 int mode=0,stages=1;
 std::set<int> species;
 bool selects(int original_species)const{return mode==0||species.count(original_species)!=0;}
 bool operator==(const Config &o)const{return enabled==o.enabled&&mode==o.mode&&stages==o.stages&&species==o.species;}
};
inline bool species_list(const char *s,size_t n,std::set<int>&out){
 std::set<int> parsed;size_t at=0;
 while(at<n){int id=0;const size_t start=at;
  while(at<n&&s[at]!=','){if(s[at]<'0'||s[at]>'9'||at-start>=4)return false;id=id*10+(s[at++]-'0');}
  if(at==start||id<1||id>2000||!parsed.insert(id).second)return false;
  if(at<n&&++at==n)return false;
 }
 out=std::move(parsed);return true;
}
inline bool parse(const char *s,size_t n,Config &out){
 if(!s||n<8||n>10000||s[0]!='1'||s[1]!='|'||s[3]!='|'||s[5]!='|'||s[7]!='|'
    ||(s[2]!='0'&&s[2]!='1')||(s[4]!='0'&&s[4]!='1')||(s[6]!='1'&&s[6]!='2'))return false;
 Config c;c.enabled=s[2]=='1';c.mode=s[4]-'0';c.stages=s[6]-'0';
 if(!species_list(s+8,n-8,c.species))return false;out=std::move(c);return true;
}
// A broken/cyclic evolution graph must never cause unbounded candy spending.
constexpr unsigned kMaxStages=8,kQueueLimit=64,kSeenLimit=4096;
inline bool another_stage(const Config &c,unsigned completed){return completed<kMaxStages&&(completed==0||c.stages==2);}
}

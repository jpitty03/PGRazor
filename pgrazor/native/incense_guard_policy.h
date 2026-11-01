#pragma once
#include <string>
#include <vector>
namespace pgr_incense_guard {
struct Settings { std::string token; bool active=false; };
inline bool parse(const char *text, size_t size, Settings &out) {
 if(!text||size>256)return false;
 std::vector<std::string> v;size_t start=0;
 for(size_t i=0;i<=size;i++)if(i==size||text[i]=='|'){v.emplace_back(text+start,i-start);start=i+1;}
 if(v.size()!=8||v[0]!="1"||v[1].empty()||v[1].size()>64)return false;
 for(char c:v[1])if(c<'0'||c>'9')return false;
 for(size_t i=2;i<8;i++){
  if(v[i].empty()||v[i].size()>6)return false;
  for(char c:v[i])if(c<'0'||c>'9')return false;
  if((i==5||i==7)&&(v[i]!="0"&&v[i]!="1"))return false;
 }
 out.token=v[1];out.active=v[7]=="1";return true;
}
inline bool bird(int species,int form,int articuno,int zapdos,int moltres){
 return (species==144&&form==articuno)||(species==145&&form==zapdos)||(species==146&&form==moltres);
}
}

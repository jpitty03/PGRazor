#include "gym_automation.h"
#include <cmath>
#include <algorithm>
namespace pgr_gym {
bool parse(const char *p,size_t len,Settings &out){
 out={};if(!p||len>64)return false;int v[6]{},n=0,digits=0;
 for(size_t i=0;i<=len;++i){char c=i==len?';':p[i];if(c==';'){if(!digits||n>=6)return false;++n;digits=0;}
  else {if(c<'0'||c>'9'||n>=6||++digits>4)return false;v[n]=v[n]*10+c-'0';}}
 if(n!=6||v[0]!=1||v[1]>1||v[2]>1||v[3]>1||v[4]>1000||v[5]>1000)return false;
 out={bool(v[1]),bool(v[2]),bool(v[3]),v[4],v[5]};return true;
}
double distance(double a,double b,double c,double d){
 if(!std::isfinite(a)||!std::isfinite(b)||!std::isfinite(c)||!std::isfinite(d)||std::abs(a)>90||std::abs(c)>90||std::abs(b)>180||std::abs(d)>180)return INFINITY;
 constexpr double r=0.017453292519943295;double x=sin((c-a)*r/2),y=sin((d-b)*r/2);
 double h=x*x+cos(a*r)*cos(c*r)*y*y;return 12742000*asin(sqrt(std::max(0.,std::min(1.,h))));
}
uint64_t choose(const std::vector<Defender>& candidates,const std::vector<int>& species,bool fallback){
 const Defender *best=nullptr;
 for(const auto &p:candidates){if(!p.id||!p.eligible||p.cp<1||p.species<1||(!p.tagged&&(!fallback||p.protected_fallback))||std::find(species.begin(),species.end(),p.species)!=species.end())continue;
  if(!best||(p.tagged&&!best->tagged)||(p.tagged==best->tagged&&(p.cp<best->cp||(p.cp==best->cp&&p.id<best->id))))best=&p;
 }return best?best->id:0;
}
int berry(int n,int r,const Settings &s){return n>s.nanab?703:r>s.razz?701:0;}
bool History::deploy_allowed(const std::string &id,uint64_t now){
 for(auto i=gyms.begin();i!=gyms.end();)if(i->second<=now)i=gyms.erase(i);else ++i;
 return !id.empty()&&gyms.find(id)==gyms.end()&&gyms.size()<512;
}
bool History::feed_allowed(uint64_t id,uint64_t now){
 for(auto i=feeds.begin();i!=feeds.end();)if(i->second.until<=now)i=feeds.erase(i);else ++i;
 auto i=feeds.find(id);return id&&(i!=feeds.end()?i->second.count<10:feeds.size()<10);
}
bool History::deploy_attempt(const std::string &id,uint64_t now){if(!deploy_allowed(id,now))return false;gyms[id]=now+600000;return true;}
bool History::feed_attempt(uint64_t id,uint64_t now){if(!feed_allowed(id,now))return false;auto &f=feeds[id];++f.count;f.until=now+1800000;return true;}
void History::refuse(uint64_t id,uint64_t now){if(id){auto i=feeds.find(id);if(i!=feeds.end())i->second={now+1800000,10};else if(feeds.size()<10)feeds[id]={now+1800000,10};}}
}

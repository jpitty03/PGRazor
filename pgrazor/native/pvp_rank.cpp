#include "patch_work_budget.h"
#include "pvp_rank.h"
#include "pvp_data.h"

namespace pgr_pvp {
namespace {
constexpr int kSpecies=sizeof(species)/sizeof(species[0]);
constexpr int kKeys=sizeof(keys)/sizeof(keys[0]);
constexpr int limits[]={500,1500,2500};
struct Row {double product;unsigned short iv,order;unsigned char level;};
struct Table {bool valid;int index,league;unsigned long long age;unsigned short rank[4096];unsigned char level[4096];};
Table cache[pgr_perf::pvp_tables]{};unsigned long long serial=0,builds=0,hits=0,evictions=0;Row scratch[4096];
int cp(const Species &e,int a,int d,int s,double m){
 return static_cast<int>(__builtin_floor((e.attack+a)*__builtin_sqrt(e.defense+d)*__builtin_sqrt(e.stamina+s)*m*m/10.0));
}
int lookup(int dex,int form){
 int fallback=-1;
 for(int i=0;i<kKeys;++i)if(keys[i].dex==dex){if(keys[i].form==form)return keys[i].index;if(keys[i].form==-1)fallback=keys[i].index;}
 return fallback;
}
bool better(const Row &a,const Row &b){return a.product>b.product||(a.product==b.product&&a.order<b.order);}
void sift(int n,int i){
 for(;;){int child=i*2+1;if(child>=n)return;if(child+1<n&&better(scratch[child+1],scratch[child]))++child;
  if(!better(scratch[child],scratch[i]))return;Row t=scratch[i];scratch[i]=scratch[child];scratch[child]=t;i=child;}
}
Table &table(int index,int league){
 int slot=0;for(int i=0;i<int(pgr_perf::pvp_tables);++i){
  if(cache[i].valid&&cache[i].index==index&&cache[i].league==league){++hits;cache[i].age=++serial;return cache[i];}
  if(!cache[i].valid||cache[i].age<cache[slot].age)slot=i;
 }
 Table &t=cache[slot];++builds;if(t.valid)++evictions;t.valid=false;t.index=index;t.league=league;t.age=++serial;
 for(int i=0;i<4096;++i){t.rank[i]=0;t.level[i]=0;}
 const Species &e=species[index];int start=league==0?0:e.start;if(start<0)start=0;if(start>98)start=98;
 int count=0,order=0;
 for(int s=15;s>=e.floor;--s)for(int d=15;d>=e.floor;--d)for(int a=15;a>=e.floor;--a,++order){
  int lo=start,hi=98;if(cp(e,a,d,s,cpm[lo])>limits[league])continue;
  while(lo<hi){int mid=(lo+hi+1)/2;if(cp(e,a,d,s,cpm[mid])<=limits[league])lo=mid;else hi=mid-1;}
  const double m=cpm[lo];const double product=(e.attack+a)*m*(e.defense+d)*m*__builtin_floor((e.stamina+s)*m);
  scratch[count++]={product,static_cast<unsigned short>(a*256+d*16+s),static_cast<unsigned short>(order),static_cast<unsigned char>(lo)};
 }
 // Heap-sort with an explicit tie ordinal preserves the original stable JS ordering.
 for(int i=count/2-1;i>=0;--i)sift(count,i);
 for(int end=count-1;end>0;--end){Row x=scratch[0];scratch[0]=scratch[end];scratch[end]=x;sift(end,0);}
 for(int i=count-1,rank=1;i>=0;--i,++rank){t.rank[scratch[i].iv]=static_cast<unsigned short>(rank);t.level[scratch[i].iv]=scratch[i].level;}
 t.valid=true;return t;
}
}
unsigned cache_capacity(){return pgr_perf::pvp_tables;}
unsigned long long cache_builds(){return builds;}
unsigned long long cache_hits(){return hits;}
unsigned long long cache_evictions(){return evictions;}
const char *name(int i){return i>=0&&i<kSpecies?species[i].name:"";}
double multiplier(int i){return i>=0&&i<=98?cpm[i]:0;}
int ranks(int dex,int form,int a,int d,int s,double m,Fit *out,int capacity){
 if(!out||capacity<=0||a<0||a>15||d<0||d>15||s<0||s>15)return 0;
 int root=lookup(dex,form);if(root<0)return 0;
 int family[64],count=1;family[0]=root;
 for(int at=0;at<count;++at){const Species &e=species[family[at]];
  for(int n=0;n<e.count;++n){int next=edges[e.edge+n];bool seen=false;for(int j=0;j<count;++j)if(family[j]==next)seen=true;
   if(!seen){if(count==64)return 0;family[count++]=next;}}
 }
 const bool known=__builtin_isfinite(m)&&m>0;int written=0;
 for(int league=0;league<3;++league){Fit fits[64];int n=0;
  for(int j=0;j<count;++j){int index=family[j];const Species &e=species[index];
   if(a<e.floor||d<e.floor||s<e.floor||cp(e,15,15,15,cpm[98])<=limits[league]||(known&&cp(e,a,d,s,m)>limits[league]))continue;
   Table &t=table(index,league);int iv=a*256+d*16+s;if(!t.rank[iv])continue;
   Fit f{league,index,t.rank[iv],t.level[iv],cp(e,a,d,s,cpm[t.level[iv]])};
   int insert=n;while(insert>0&&fits[insert-1].rank>f.rank){fits[insert]=fits[insert-1];--insert;}fits[insert]=f;++n;
  }
  for(int j=0;j<n&&written<capacity;++j)out[written++]=fits[j];
 }
 return written;
}
}

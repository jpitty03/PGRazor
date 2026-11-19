// Standalone Android policy test, never packaged in the APK.
#include "transfer_policy.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
int main(int argc,char **argv) {
 if(argc!=2)return 1;
 void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib)return 2;
 auto get=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));
 auto core=get?static_cast<const pgrazor_native_api_v2 *>(get(2)):nullptr;
 auto api=core?static_cast<const PgrTransferApi *>(core->query_feature(4,1)):nullptr;
 if(!api||api->size!=16)return 3;
 PgrTransferInput v{1,sizeof(v),0,0,0,25,63,255,0,0,0,0,nullptr};
 if(api->decide(nullptr)!=16)return 4;
 for(int a=0;a<16;++a)for(int d=0;d<16;++d)for(int s=0;s<16;++s){
  v.attack=a;v.defense=d;v.stamina=s;
  if(api->decide(&v)!=(a+d+s>=41?9:0))return 5;
 }
 // An explicit keep-IV cutoff replaces 90%: keep when (a+d+s)/45 >= percent/100.
 for(int pct=0;pct<=100;++pct){
  v.keep_iv=PGR_TRANSFER_KEEP_IV_SET|unsigned(pct);
  for(int sum=0;sum<=45;++sum){v.attack=std::min(sum,15);v.defense=std::min(sum-v.attack,15);v.stamina=sum-v.attack-v.defense;
   if(api->decide(&v)!=(sum*100>=pct*45?9:0))return 14;}
 }
 // Malformed keep_iv values fail closed (kept), never transfer.
 for(uint32_t bad:{PGR_TRANSFER_KEEP_IV_SET|101u,0x200u|90u,90u,0x1ffu}){v.keep_iv=bad;v.attack=v.defense=v.stamina=0;if(api->decide(&v)!=17)return 15;}
 v.keep_iv=0;
 // Slot 25 parsing: canonical "K1|<0..100>" only; anything else means the 90% default (0).
 struct {const char *s;uint32_t want;} cases[]={{"K1|90",PGR_TRANSFER_KEEP_IV_SET|90},{"K1|0",PGR_TRANSFER_KEEP_IV_SET},{"K1|100",PGR_TRANSFER_KEEP_IV_SET|100},
  {"K1|101",0},{"K1|",0},{"K1|09",0},{"K1|9a",0},{"K2|90",0},{"K1|1000",0},{"",0}};
 for(auto &c:cases)if(pgr_transfer_keep_iv_parse(c.s,uint32_t(std::strlen(c.s)))!=c.want)return 16;
 v.attack=v.defense=v.stamina=0;
 for(unsigned flags=0;flags<512;++flags)for(unsigned protect=0;protect<256;++protect){
  v.flags=flags;v.protect=protect;int reason=0;
  if(flags&192)reason=8;
  else for(int bit=0;bit<6;++bit)if((flags&(1u<<bit))&&(protect&(1u<<bit))){reason=10+bit;break;}
  if(!reason&&(flags&256)&&(protect&128))reason=20; // costume kept only with its own bit
  if(!reason&&(protect&64))reason=19; // 0/0/0 kept only with its own bit
  if(api->decide(&v)!=reason)return 6;
 }
 // Only an exact 0/0/0 is kept by bit 6; any other low IV still transfers.
 v.flags=0;v.protect=127;for(int a=0;a<3;++a)for(int d=0;d<3;++d)for(int s=0;s<3;++s){v.attack=a;v.defense=d;v.stamina=s;if(api->decide(&v)!=(a+d+s==0?19:0))return 13;}
 v.attack=v.defense=v.stamina=0;
 v.flags=0;v.protect=63;
 for(unsigned bit=0;bit<8;++bit){v.known=255^(1u<<bit);if(api->decide(&v)!=(bit==4||bit==5?7:6))return 7;}
 v.known=255;int32_t ids[]{25,150};v.keep_ids=ids;v.keep_count=2;
 if(api->decide(&v)!=4)return 8;
 v.species=-1;if(api->decide(&v)!=3)return 9;
 v.keep_count=0xffffffff;if(api->decide(&v)!=2)return 10;
 v.keep_all=1;if(api->decide(&v)!=1)return 11;
 v.keep_all=0;v.keep_count=0;v.attack=-1;if(api->decide(&v)!=5)return 12;
 std::puts("PASS native transfer policy: exhaustive IV thresholds (default 90% and every keep-IV cutoff), 0/0/0 keep, category masks, unknown data and custom keep priority");
 return 0;
}

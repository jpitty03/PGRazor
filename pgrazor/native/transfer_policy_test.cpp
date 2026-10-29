// Standalone Android policy test, never packaged in the APK.
#include "transfer_policy.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <cstdio>
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
 v.attack=v.defense=v.stamina=0;
 for(unsigned flags=0;flags<256;++flags)for(unsigned protect=0;protect<64;++protect){
  v.flags=flags;v.protect=protect;int reason=0;
  if(flags&192)reason=8;
  else for(int bit=0;bit<6;++bit)if((flags&(1u<<bit))&&(protect&(1u<<bit))){reason=10+bit;break;}
  if(api->decide(&v)!=reason)return 6;
 }
 v.flags=0;v.protect=63;
 for(unsigned bit=0;bit<8;++bit){v.known=255^(1u<<bit);if(api->decide(&v)!=(bit==4||bit==5?7:6))return 7;}
 v.known=255;int32_t ids[]{25,150};v.keep_ids=ids;v.keep_count=2;
 if(api->decide(&v)!=4)return 8;
 v.species=-1;if(api->decide(&v)!=3)return 9;
 v.keep_count=0xffffffff;if(api->decide(&v)!=2)return 10;
 v.keep_all=1;if(api->decide(&v)!=1)return 11;
 v.keep_all=0;v.keep_count=0;v.attack=-1;if(api->decide(&v)!=5)return 12;
 std::puts("PASS native transfer policy: exhaustive IV thresholds, category masks, unknown data and custom keep priority");
 return 0;
}

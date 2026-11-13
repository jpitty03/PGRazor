// Standalone Android test. Exercises the actual native policy and hit trampoline.
#include "throws.h"
#include "auto_throw_values.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <limits>
#include <initializer_list>
int ball_class,owner_class,bread_field;
struct Object {void *klass;uint8_t bread;};
void *klass(void *o){return static_cast<Object *>(o)->klass;}
bool assignable(void *a,void *b){return a==b;}
void field(void *o,void *,void *out){*static_cast<uint8_t *>(out)=static_cast<Object *>(o)->bread;}
PgrThrowValue last{};int calls=0;void *last_info=nullptr;
__attribute__((noinline)) void hit(void *,PgrThrowValue value,void *info){
 asm volatile("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
 last=value;last_info=info;++calls;
}
__attribute__((noinline)) void submit(void *,PgrThrowValue,void *){asm volatile("nop" ::: "memory");}
int main(int argc,char **argv){
 if(argc!=2)return 1;
 void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){std::puts(dlerror());return 2;}
 auto get=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));
 auto core=get?static_cast<const pgrazor_native_api_v2 *>(get(2)):nullptr;
 auto api=core?static_cast<const PgrThrowApi *>(core->query_feature(1,1)):nullptr;
 auto map_api=core?static_cast<const PgrMapThrowApi*>(core->query_feature(1,2)):nullptr;
 if(!api||api->size!=40||!map_api||map_api->size!=sizeof(PgrMapThrowApi))return 3;
 PgrThrowConfig config{1,sizeof(config),reinterpret_cast<void *>(&submit),reinterpret_cast<void *>(&hit),
  &ball_class,&owner_class,reinterpret_cast<void *>(&klass),reinterpret_cast<void *>(&assignable),reinterpret_cast<void *>(&field),&bread_field};
 if(api->install(&config)||api->install(&config))return 4;
 Object ball{&ball_class,0},owner{&owner_class,0};
 decltype(&hit) volatile call=&hit;PgrThrowValue original{1,0.5f,0,0,0,0xa5},value=original;
 call(&owner,value,&config);if(calls!=1||last_info!=&config||std::memcmp(&last,&value,12))return 5;
 if(api->update(3,1,1,2500,gettid())||api->rewrite_submit(&ball,&value))return 6;
 if(value.radius<0.1f||value.radius>0.25f||value.bullseye!=1||value.spinning!=1||value.pad!=0xa5)return 7;
 const float paired=value.radius;call(&owner,original,&config);
 if(last.radius!=paired||last.pad!=0xa5||last_info!=&config)return 8;
 const float low[]{0,.75f,.35f,.10f},high[]{0,.95f,.65f,.25f};
 for(int grade=1;grade<=3;++grade){
  if(api->update(grade,0,1,2500,gettid()))return 9;
  float first=-1;bool varied=false;
  for(int i=0;i<128;++i){value=original;api->rewrite_submit(&ball,&value);
   if(value.radius<low[grade]||value.radius>high[grade]||value.spinning||value.pad!=0xa5)return 10;
   if(i==0)first=value.radius;else if(value.radius!=first)varied=true;
  }if(!varied)return 11;
 }
 // Throw variety (4-6): only the set's grades, each at roughly even odds, and the paired hit keeps the
 // grade (and radius) its submit picked, a plain throw included.
 for(int setting=4;setting<=6;++setting){
  if(api->update(setting,0,1,2500,gettid()))return 21;
  int seen[4]={};const int n=setting-2;
  for(int i=0;i<800;++i){value=original;api->rewrite_submit(&ball,&value);
   int grade=-1;
   if(!value.bullseye&&value.radius==original.radius)grade=0;
   else for(int g=1;g<=3;++g)if(value.bullseye&&value.radius>=low[g]&&value.radius<=high[g])grade=g;
   if(grade<0||grade<4-n)return 22;  // e.g. Excellent & Great never gives Nice or a plain throw
   ++seen[grade];
   call(&owner,original,&config);if(last.radius!=value.radius||last.bullseye!=value.bullseye)return 23;
  }
  for(int g=4-n;g<=3;++g)if(seen[g]<800/n/2)return 24;  // even odds: each well above half its share
 }
 for(int item : {4,6}){value=original;value.item=item;PgrThrowValue before=value;api->rewrite_submit(&ball,&value);if(std::memcmp(&before,&value,12))return 12;}
 value=original;value.missed=1;api->rewrite_submit(&ball,&value);if(value.radius!=original.radius||value.bullseye)return 13;
 value=original;value.radius=std::numeric_limits<float>::quiet_NaN();api->rewrite_submit(&ball,&value);if(!std::isnan(value.radius)||value.bullseye)return 14;
 ball.bread=1;value=original;api->rewrite_submit(&ball,&value);if(value.bullseye)return 15;ball.bread=0;
 api->update(3,1,1,2500,gettid()+1);value=original;api->rewrite_submit(&ball,&value);if(value.bullseye)return 16;
 api->update(3,1,1,1,gettid());timespec pause{0,10000000};nanosleep(&pause,nullptr);
 value=original;api->rewrite_submit(&ball,&value);if(value.bullseye)return 17;
 api->update(3,1,1,2500,gettid());if(api->update(99,1,1,2500,gettid())==0)return 18;
 value=original;api->rewrite_submit(&ball,&value);if(value.bullseye)return 19;
 api->update(3,1,1,2500,gettid());api->update(0,0,0,0,0);call(&owner,original,&config);if(std::memcmp(&last,&original,12))return 20;
 // Auto Throw uses this same live policy, starting plain and always hitting.
 for(int setting=0;setting<=6;++setting)for(int curve=0;curve<=1;++curve){
  if(api->update(setting,curve,1,2500,gettid()))return 30;
  int seen[4]={};
  for(int i=0;i<256;++i){
   if(!pgr_manual_throw_value(api,&ball,2,value))return 31;
   if(value.item!=2||value.missed||value.spinning!=curve)return 32;
   int grade=-1;
   if(!value.bullseye&&value.radius==1.0f)grade=0;
   else for(int g=1;g<=3;++g)if(value.bullseye&&value.radius>=low[g]&&value.radius<=high[g])grade=g;
   if(grade<0||(setting<=3?grade!=setting:grade<6-setting))return 33;
   ++seen[grade];
   const auto raid=pgr_raid_throw_value(5);
   if(raid.item!=5||raid.radius!=0.1f||!raid.bullseye||!raid.spinning||raid.missed)return 34;
  }
  if(setting>=4)for(int g=6-setting;g<=3;++g)if(!seen[g])return 35;
 }
 if(pgr_manual_throw_value(nullptr,&ball,1,value))return 36;
 // Map Auto Throw has no scene ball but must apply the identical live policy.
 for(int setting=0;setting<=6;++setting)for(int curve=0;curve<=1;++curve){
  if(api->update(setting,curve,1,2500,gettid()))return 37;
  int seen[4]={};
  for(int i=0;i<256;++i){
   if(!map_api->value(&value)||value.item!=1||value.missed||value.spinning!=curve)return 38;
   int grade=-1;
   if(!value.bullseye&&value.radius==1.0f)grade=0;
   else for(int g=1;g<=3;++g)if(value.bullseye&&value.radius>=low[g]&&value.radius<=high[g])grade=g;
   if(grade<0||(setting<=3?grade!=setting:grade<6-setting))return 39;
   ++seen[grade];
  }
  if(setting>=4)for(int g=6-setting;g<=3;++g)if(!seen[g])return 40;
 }
 api->update(3,1,0,0,gettid());if(map_api->value(&value))return 41;
 api->update(3,1,1,2500,gettid()+1);if(map_api->value(&value))return 42;
 api->update(3,1,1,1,gettid());nanosleep(&pause,nullptr);if(map_api->value(&value))return 43;

 std::puts("PASS native throws: ABI chaining, paired jitter, all bands, exclusions, thread/lease gates, invalid-update disable, Auto Throw settings/variety and independent raid defaults");
 return 0;
}

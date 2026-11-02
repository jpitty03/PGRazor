// Standalone Android test. Exercises the actual native policy and hit trampoline.
#include "throws.h"
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
 if(!api||api->size!=40)return 3;
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
 // Random is selected at submit; a renewal keeps the exact grade and radius for its hit.
 unsigned seen=0;
 for(int i=0;i<256;++i){
  if(api->update(PGR_THROW_RANDOM,1,1,2500,gettid()))return 21;
  value=original;api->rewrite_submit(&ball,&value);PgrThrowSample s{};api->sample(&s);
  if(s.grade<1||s.grade>3||value.radius<low[s.grade]||value.radius>high[s.grade]||!value.bullseye||!value.spinning||value.pad!=0xa5)return 22;
  seen|=1u<<s.grade;const float radius=value.radius;const uint32_t grade=s.grade;
  if(api->update(PGR_THROW_RANDOM,1,1,2500,gettid()))return 23;
  call(&owner,i%2?original:value,&config);api->sample(&s);
  if(last.radius!=radius||s.grade!=grade||last.pad!=0xa5||!last.spinning||!last.bullseye)return 24;
 }
 if(seen!=14)return 25;
 // Policy change and OFF must drop the pending random choice, including its original radius.
 api->update(PGR_THROW_RANDOM,0,1,2500,gettid());value=original;api->rewrite_submit(&ball,&value);
 api->update(1,0,1,2500,gettid());call(&owner,original,&config);PgrThrowSample changed{};api->sample(&changed);
 if(changed.grade!=1||last.radius<low[1]||last.radius>high[1])return 26;
 api->update(PGR_THROW_RANDOM,1,1,2500,gettid());api->update(0,0,0,0,0);value=original;api->rewrite_submit(&ball,&value);
 if(std::memcmp(&value,&original,12))return 27;
 api->update(3,0,1,2500,gettid());
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
 std::puts("PASS native throws: ABI chaining, paired jitter, all bands, exclusions, thread/lease gates and invalid-update disable");
 return 0;
}

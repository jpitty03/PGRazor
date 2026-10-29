// Standalone Android test of the real hook, argument ABI, roots and failure gate.
#include "ar_throw.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <ctime>
int wild,values,proximity,awareness,frightened,other;
struct Object {void *klass;float p,a;uint8_t f;};
Object created{&values,9,8,1};
bool alloc_fail=false;int roots=0,calls=0;void *last_value=nullptr,*last_method=nullptr;PgrThrowValue last_throw{};
void *klass(void *o){return static_cast<Object *>(o)->klass;}
int assignable(void *a,void *b){return a==b;}
void *allocate(void *k){if(alloc_fail||k!=&values)return nullptr;created={&values,9,8,1};return &created;}
void *root(void *o,int){++roots;return o;}
void *target(void *o){return o;}
void free_root(void *){--roots;}
void set(void *o,void *f,void *v){auto x=static_cast<Object *>(o);if(f==&proximity)x->p=*static_cast<float *>(v);else if(f==&awareness)x->a=*static_cast<float *>(v);else if(f==&frightened)x->f=*static_cast<uint8_t *>(v);}
__attribute__((noinline)) void *capture(void *,PgrThrowValue t,void *v,void *m){
 asm volatile("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
 ++calls;last_value=v;last_method=m;last_throw=t;return &other;
}
int main(int argc,char **argv){
 if(argc!=2)return 1;void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){std::puts(dlerror());return 2;}
 auto getter=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));
 auto core=getter?static_cast<const pgrazor_native_api_v2 *>(getter(2)):nullptr;
 auto api=core?static_cast<const PgrArThrowApi *>(core->query_feature(5,1)):nullptr;if(!api||api->size!=32)return 3;
 PgrArThrowConfig cfg{1,sizeof(cfg),(void *)&capture,&wild,&values,(void *)&klass,(void *)&assignable,(void *)&allocate,(void *)&root,(void *)&target,(void *)&free_root,(void *)&set,&proximity,&awareness,&frightened};
 if(api->install(&cfg)||api->install(&cfg))return 4;
 Object self{&wild,0,0,0};PgrThrowValue t{2,.2f,1,1,0,0xa5};decltype(&capture) volatile call=&capture;
 if(call(&self,t,&cfg,&self)!=&other||last_value!=&cfg||roots)return 5;
 api->update(1,1,2500,gettid());
 if(call(&self,t,&cfg,&self)!=&other||last_value!=&created||last_method!=&self||std::memcmp(&t,&last_throw,12)||roots||created.p||created.a||created.f)return 6;
 t.item=5;call(&self,t,&cfg,&self);if(last_value!=&cfg)return 7;
 t.item=1;t.missed=1;call(&self,t,&cfg,&self);if(last_value!=&cfg)return 8;t.missed=0;
 self.klass=&other;call(&self,t,&cfg,&self);if(last_value!=&cfg)return 9;self.klass=&wild;
 api->update(1,1,2500,gettid()+1);call(&self,t,&cfg,&self);if(last_value!=&cfg)return 10;
 api->update(1,1,1,gettid());timespec pause{0,10000000};nanosleep(&pause,nullptr);call(&self,t,&cfg,&self);if(last_value!=&cfg)return 11;
 api->update(1,1,2500,gettid());if(!api->update(1,1,3000,gettid()))return 12;call(&self,t,&cfg,&self);if(last_value!=&cfg)return 13;
 api->update(1,1,2500,gettid());alloc_fail=true;call(&self,t,&cfg,&self);if(last_value!=&cfg||roots)return 14;
 alloc_fail=false;call(&self,t,&cfg,&self);if(last_value!=&cfg)return 15;
 PgrArThrowSample sample{};api->sample(&sample);if(sample.applied!=1||sample.errors!=1)return 16;
 std::puts("PASS native AR metadata: exact ABI/return, root balance, exclusions, thread/lease, latched allocation failure");return 0;
}

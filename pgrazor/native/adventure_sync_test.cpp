#include "adventure_sync.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <ctime>
#include <cstddef>
int controller,delegate,confirm_info,decline_info,roots=0,confirmed=0,declined=0,updates=0,initialized=0;
struct Dialog{void *klass;void *confirm;void *decline;};
void *root(void *o,int){++roots;return o;}void *target(void *h){return h;}void free_root(void *){--roots;}
void *klass(void *o){return static_cast<Dialog *>(o)->klass;}
#define PAD asm volatile("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory")
__attribute__((noinline)) void confirm(void *,void *){PAD;++confirmed;}
__attribute__((noinline)) void decline(void *,void *){PAD;++declined;}
__attribute__((noinline)) void update(void *,void *){PAD;++updates;}
__attribute__((noinline)) void initial(Dialog *d,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t e,void *){PAD;initialized=int(a+b+c+e);d->decline=&delegate;d->confirm=nullptr;}
__attribute__((noinline)) void instruction(Dialog *d,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h,void *){PAD;initialized=int(a+b+c+e+f+g+h);d->decline=&delegate;d->confirm=nullptr;}
__attribute__((noinline)) void result(Dialog *d,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t e,uintptr_t f,void *){PAD;initialized=int(a+b+c+e+f);d->confirm=&delegate;d->decline=nullptr;}
void *invoke(void *method,void *o,void **,void **exc){*exc=nullptr;if(method==&confirm_info)confirm(o,method);else if(method==&decline_info)decline(o,method);return nullptr;}
void settle(){timespec t{0,120000000};nanosleep(&t,nullptr);}
int main(int argc,char **argv){
 if(argc!=2)return 1;auto lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){std::puts(dlerror());return 2;}
 auto getter=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));
 auto core=getter?static_cast<const pgrazor_native_api_v2 *>(getter(2)):nullptr;
 auto api=core?static_cast<const pgrazor_advsync_api_v1 *>(core->query_feature(2,1)):nullptr;if(!api)return 3;
 pgrazor_advsync_config_v1 cfg{1,sizeof(cfg),(void *)&invoke,(void *)&root,(void *)&target,(void *)&free_root,(void *)&klass,&confirm_info,&decline_info,(void *)&update,(void *)&confirm,(void *)&decline,(void *)&initial,(void *)&instruction,(void *)&result,&controller,offsetof(Dialog,confirm),offsetof(Dialog,decline)};
 if(api->install(&cfg)||api->install(&cfg))return 4;
 Dialog d{&controller,nullptr,nullptr};
 decltype(&initial) volatile init=&initial;decltype(&instruction) volatile instruct=&instruction;decltype(&result) volatile res=&result;decltype(&update) volatile tick=&update;
 api->update(1,2500,gettid());api->retire(gettid());
 init(&d,1,2,3,4,nullptr);if(initialized!=10||roots!=1)return 5;settle();tick(&d,nullptr);if(declined!=1||confirmed||roots)return 6;
 instruct(&d,1,2,3,4,5,6,7,nullptr);if(initialized!=28||roots!=1)return 7;settle();tick(&d,nullptr);if(declined!=2||roots)return 8;
 res(&d,1,2,3,4,5,nullptr);if(initialized!=15||roots!=1)return 9;settle();tick(&d,nullptr);if(confirmed!=1||roots)return 10;
 init(&d,1,2,3,4,nullptr);api->update(0,0,gettid());settle();tick(&d,nullptr);if(declined!=2||roots)return 11;
 api->update(1,1,gettid());init(&d,1,2,3,4,nullptr);settle();tick(&d,nullptr);if(declined!=2||roots)return 12;
 api->update(1,2500,gettid()+1);init(&d,1,2,3,4,nullptr);if(roots)return 13;
 std::puts("PASS native Adventure Sync: instrument argument preservation, decline/result-confirm, reentrant hooks, disable/expiry/thread and roots");return 0;
}

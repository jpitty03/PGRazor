// Standalone device harness, never linked into or packaged with the app.
#include "catch_presentation.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

struct FakeObject { void *klass; unsigned char rest[256]; };
int view_class, action_class, pokemon_class, roots=0, forwards=0;
uint64_t observed[6]{};
void *root_object(void *o,int) { if(o)++roots;return o; }
void free_object(void *o) { if(o)--roots; }
void *target_object(void *o) { return o; }
void *object_class(void *o) { return static_cast<FakeObject *>(o)->klass; }
template<class T> void put(void *o,size_t at,T value) { std::memcpy(static_cast<char *>(o)+at,&value,sizeof(value)); }
__attribute__((noinline)) void show(void *,void *,uint64_t a,uint64_t b,uint64_t c,
    void *,void *,void *,uint64_t d,uint64_t e,uint64_t f,void *) {
    asm volatile("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
    ++forwards;observed[0]=a;observed[1]=b;observed[2]=c;observed[3]=d;observed[4]=e;observed[5]=f;
}
int main(int argc,char **argv) {
    if(argc!=2)return 1;
    void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){std::puts(dlerror());return 2;}
    auto get=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));
    auto core=get?static_cast<const pgrazor_native_api_v2 *>(get(2)):nullptr;
    if(!core||core->size!=48||core->query_feature(99,1)||core->query_feature(3,99))return 3;
    auto api=static_cast<const PgrCatchApi *>(core->query_feature(3,1));if(!api||api->size!=40)return 4;
    PgrCatchConfig config{1,sizeof(config),reinterpret_cast<void *>(&show),root_object,free_object,
        target_object,object_class,&view_class,&action_class,&pokemon_class};
    if(api->install(&config)!=0||api->install(&config)==0)return 5;
    FakeObject service{},response{},pokemon{&pokemon_class,{}},action{&action_class,{}},view{&view_class,{}};
    put(&action,0x20,static_cast<void *>(&view));put(&view,0xe0,static_cast<void *>(&response));
    put(&pokemon,0x20,uint64_t(42));put(&response,0x20,uint64_t(42));
    decltype(&show) volatile call=&show;
    call(&service,&pokemon,0xab01,0xcd00,17,&action,nullptr,nullptr,0xef01,0x9901,0xaa00,nullptr);
    if(forwards!=1||observed[0]!=0xab01||observed[4]!=0x9901)return 6;
    if(api->begin(&response,gettid()+1)==0||api->begin(&response,gettid())!=0||roots!=1)return 7;
    if(api->dispose()==0||api->begin(&response,gettid())==0)return 8;
    // Mismatching captured Pokemon identity forwards without consuming the window.
    put(&pokemon,0x20,uint64_t(43));
    call(&service,&pokemon,1,0,17,&action,nullptr,nullptr,1,1,0,nullptr);
    if(forwards!=2||roots!=1)return 9;
    put(&pokemon,0x20,uint64_t(42));
    call(&service,&pokemon,0xab01,0xcd00,17,&action,nullptr,nullptr,0xef01,0x9901,0xaa00,nullptr);
    if(forwards!=2||roots!=4)return 10;
    PgrCatchCapture out{};
    if(api->end(&out)!=1||roots!=3||out.service!=&service||out.pokemon!=&pokemon||out.callback!=&action||
       out.confirm!=1||out.persist!=0||out.hide_ar!=1||out.post!=1||out.suppress!=0||out.open_hyper_training!=17)return 11;
    free_object(out.service);free_object(out.pokemon);free_object(out.callback);
    if(api->end(&out)!=0||roots!=0||api->dispose()!=0)return 12;
    call(&service,&pokemon,1,0,17,&action,nullptr,nullptr,1,1,0,nullptr);
    if(forwards!=3||api->install(&config)!=0||api->begin(&response,gettid())!=0)return 13;
    if(api->end(&out)!=0||roots!=0||api->dispose()!=0)return 14;
    std::puts("PASS native catch bridge: ABI forwarding, identity gates, one-shot ownership, cleanup and reinitialization");
    return 0;
}

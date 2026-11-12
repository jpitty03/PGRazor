#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "unity_scheduler.h"
#include <time.h>
#include <unistd.h>
#include <cassert>
#include <cstdio>
namespace test {
uint64_t now=100,generation=7;int tid=71,allocations=0,invokes=0,roots=0,frees=0,reads=0,writes=0;
bool attached=true;int object;
int clock(int,timespec* t){t->tv_sec=now/1000;t->tv_nsec=(now%1000)*1000000;return 0;}
int current_tid(){return tid;}
void* thread(){return attached?&object:nullptr;}
int snapshot(PgrUnitySchedulerSnapshot* s){*s={generation,1,71,1,0,0};return 0;}
int valid(uint64_t g){return g&&g==generation?1:0;}
void* allocate(void*){++allocations;return &object;}
void* invoke(void*,void*,void**,void** e){++invokes;*e=nullptr;return nullptr;}
uintptr_t hold(void*,int){++roots;return 1;}
void drop(uintptr_t){++frees;}
void* target(uintptr_t){return &object;}
void read(void*,void*,void* out){++reads;*static_cast<uint8_t*>(out)=2;}
void write(void*,void*,void*){++writes;}
void* symbol(void*,const char*){return nullptr;}
}
extern "C" const void* pgrazor_unity_scheduler_api(uint32_t){static PgrUnitySchedulerApi a{};a.snapshot=test::snapshot;a.valid=test::valid;return &a;}
extern "C" const void* pgrazor_runtime_bootstrap_api(uint32_t){return nullptr;}
extern "C" const void* pgrazor_runtime_metadata_api(uint32_t){return nullptr;}
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#define clock_gettime test::clock
#define gettid test::current_tid
#define dlsym test::symbol
#include "showcase_tag_runtime.cpp"
#undef clock_gettime
#undef gettid
#undef dlsym
int main(){
 using namespace pgr_showcase;
 TagRpcRequest req;req.owner=71;req.generation=7;req.deadline_ms=2600;
 Rt rt;rt.request=&req;
 rt.boot.runtime.thread_current=reinterpret_cast<void*>(test::thread);
 rt.boot.runtime.object_new=reinterpret_cast<void*>(test::allocate);
 rt.boot.runtime.runtime_invoke=reinterpret_cast<void*>(test::invoke);
 rt.boot.runtime.gchandle_new=reinterpret_cast<void*>(test::hold);
 rt.boot.runtime.gchandle_free=reinterpret_cast<void*>(test::drop);
 rt.boot.runtime.gchandle_get_target=reinterpret_cast<void*>(test::target);
 rt.boot.runtime.field_get_value=reinterpret_cast<void*>(test::read);
 rt.ref_setter=reinterpret_cast<void*>(test::write);
 assert(new_object(rt,&test::object)==&test::object&&test::allocations==1);
 {
  Root root(rt,&test::object);assert(test::roots==1);
  test::now=2600;
  try{new_object(rt,&test::object);assert(false);}catch(const GateFailure&){}
  bool threw=false;try{invoke(rt,&test::object,&test::object,nullptr,threw);assert(false);}catch(const GateFailure&){}
  try{gc_new(rt,&test::object);assert(false);}catch(const GateFailure&){}
  assert(test::allocations==1&&test::invokes==0&&test::roots==1);
 }
 assert(test::frees==1); // Cleanup is required even after expiry.
 test::now=100;test::attached=false;
 try{set_ref(rt,&test::object,&test::object,&test::object);assert(false);}catch(const GateFailure&){}
 assert(test::writes==0);
 test::attached=true;test::generation=8;
 try{target(rt,1);assert(false);}catch(const GateFailure&){}
 test::generation=7;
 bool boolean=false;try{read_field(rt,&test::object,&test::object,boolean);assert(false);}catch(const GateFailure&){}
 assert(test::reads==1); // Noncanonical managed bool is rejected.
 uintptr_t pending=999;
 assert(start_tag_rpc(req,pending)==TagStartStatus::bad_binding&&pending==0);
 uint64_t id=999;assert(poll_tag_rpc(req,1,id)==TagPollStatus::bad_binding&&id==0);
 {ReentryGuard guard;assert(start_tag_rpc(req,pending)==TagStartStatus::refused&&pending==0);}
 test::tid=72;assert(start_tag_rpc(req,pending)==TagStartStatus::refused);
 std::puts("PASS tag RPC primitive guards: lease, attached owner, generation, reentry, strict bool, cleanup and unavailable bindings");
}

#include "unity_scheduler.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstdio>
#include <dlfcn.h>
#include <thread>
#include <vector>
#include <unistd.h>

// Real dlopen production registry + real Dobby fixture entrypoints. Coordinator
// must register feature48 and link unity_scheduler.cpp before executing harness.
namespace {
const PgrUnitySchedulerApi *api;
const PgrUnitySchedulerApi3 *api3;
int execute_token,other_token,result_token,exception_token,thread_token,obj_token,arg_token;
thread_local bool attached=true;
PgrUnityInvoke volatile call_invoke;
PgrUnityConvert volatile call_convert;
uint64_t generation=0;
int calls=0,convert_calls=0,begins=0,ends=0;
bool nested_original=false,nested_callback=false,revoke_callback=false;
bool unexpected_result=false,managed_exception=false,native_throw=false,rotate_original=false;
bool nested_convert_original=false;
bool inspect_execution=false,nested_unrelated=false,rotate_nested=false;
uint64_t expected_execution=0;
void inspect(){if(inspect_execution){assert(api3->executing_generation(gettid())==expected_execution);assert(api3->executing_generation(0)==0);assert(api3->executing_generation(uint64_t(gettid())+1)==0);}}
struct OriginalFailure {int code;};
const void *seen_method=nullptr;void *seen_object=nullptr;void **seen_args=nullptr;
void **seen_exception=nullptr;int32_t seen_count=0;
std::vector<int> order;
void *thread_current(){return attached?&thread_token:nullptr;}
int verify(const PgrUnitySchedulerConfig *c){return c->execute_tasks==&execute_token?1:0;}
#define NOPS() asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;")
__attribute__((noinline)) void *fixture_invoke(const void *method,void *object,void **args,void **exception){
 NOPS();++calls;seen_method=method;seen_object=object;seen_args=args;seen_exception=exception;
 order.push_back(3);
 inspect();
 if(nested_unrelated){nested_unrelated=false;void *e=nullptr;assert(call_convert(&other_token,nullptr,nullptr,0,&e)==&result_token);assert(e==&exception_token);inspect();}
 if(nested_original){nested_original=false;void *e=nullptr;assert(call_convert(&execute_token,nullptr,nullptr,0,&e)==nullptr);assert(e==nullptr);}
 if(rotate_original){rotate_original=false;assert(api->revoke(generation)==0);generation=api->activate();assert(generation);if(inspect_execution){expected_execution=0;inspect();}if(rotate_nested){rotate_nested=false;void *e=nullptr;assert(call_convert(&execute_token,nullptr,nullptr,0,&e)==nullptr);inspect();}}
 if(native_throw)throw OriginalFailure{81};
 if(exception)*exception=(method!=&execute_token||managed_exception)?&exception_token:nullptr;
 return (method!=&execute_token||unexpected_result)?&result_token:nullptr;
}
__attribute__((noinline)) void *fixture_convert(const void *method,void *object,void **args,int32_t count,void **exception){
 NOPS();++convert_calls;seen_method=method;seen_object=object;seen_args=args;seen_count=count;seen_exception=exception;
 order.push_back(4);
 inspect();
 if(nested_convert_original){nested_convert_original=false;void *e=nullptr;assert(call_invoke(&execute_token,nullptr,nullptr,&e)==nullptr);assert(e==nullptr);}
 if(native_throw)throw OriginalFailure{81};
 if(exception)*exception=(method!=&execute_token||managed_exception)?&exception_token:nullptr;
 return (method!=&execute_token||unexpected_result)?&result_token:nullptr;
}
void begin(void *,const PgrUnityPulse *p){
 assert(api3->executing_generation(gettid())==0);
 assert(p->generation==generation&&p->tid==gettid()&&attached&&p->reserved==0);
 assert(api->valid(p->generation)==1);++begins;order.push_back(1);
 if(nested_callback){nested_callback=false;void *e=nullptr;assert(call_invoke(&execute_token,nullptr,nullptr,&e)==nullptr);assert(e==nullptr);}
 if(revoke_callback){revoke_callback=false;assert(api->revoke(p->generation)==0);}
}
void end(void *,const PgrUnityPulse *p){assert(api3->executing_generation(gettid())==0);assert(api->valid(p->generation)==1);++ends;order.push_back(2);}
void invoke(){void *e=nullptr;assert(call_invoke(&execute_token,nullptr,nullptr,&e)==nullptr);assert(e==nullptr);}
}
int main(int argc,char **argv){
 assert(argc==2);void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(library);
 auto get=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(library,"pgrazor_native_api"));assert(get);
 const auto *core=static_cast<const pgrazor_native_api_v2 *>(get(2));assert(core&&core->query_feature);
 assert(!core->query_feature(48,1));api=static_cast<const PgrUnitySchedulerApi *>(core->query_feature(48,2));
 assert(api&&api->version==2&&api->size==sizeof(*api));assert(!api->activate());
 api3=static_cast<const PgrUnitySchedulerApi3 *>(core->query_feature(48,3));
 assert(api3&&api3->version==3&&api3->size==56&&api3->size==sizeof(*api3));
 assert(api3->executing_generation(gettid())==0);
 PgrUnitySchedulerConfig c{};c.version=2;c.size=sizeof(c);
 c.runtime_invoke=reinterpret_cast<void *>(&fixture_invoke);c.runtime_invoke_convert_args=reinterpret_cast<void *>(&fixture_convert);
 c.execute_tasks=&execute_token;c.thread_current=thread_current;c.verify_metadata=verify;
 c.expected_owner_tid=gettid();c.subscriber_count=1;c.subscribers[0]={begin,end,nullptr};
 assert(api->install(&c)==PGR_US_BAD_CONFIG);c.exclusive_owner=1;
 auto bad=c;bad.version=1;assert(api->install(&bad)==PGR_US_BAD_CONFIG);
 bad=c;bad.execute_tasks=&other_token;assert(api->install(&bad)==PGR_US_BAD_CONFIG);
 bad=c;bad.runtime_invoke_convert_args=bad.runtime_invoke;assert(api->install(&bad)==PGR_US_BAD_CONFIG);
 bad=c;bad.subscriber_count=17;assert(api->install(&bad)==PGR_US_BAD_CONFIG);
 bad=c;bad.reserved=1;assert(api->install(&bad)==PGR_US_BAD_CONFIG);
 // No hooked target is running during install/rollback (required contract).
 call_invoke=fixture_invoke;call_convert=fixture_convert;
 assert(api->install(&c)==0);assert(api->install(&c)==0);
 bad=c;bad.subscribers[0].context=&other_token;assert(api->install(&bad)==PGR_US_REBIND);
 invoke();assert(calls==1&&begins==0&&ends==0);
 generation=api->activate();assert(generation);
 // Wrong OS thread cannot claim permanent owner, even if attached.
 std::thread wrong([]{invoke();});wrong.join();
 PgrUnitySchedulerSnapshot s{};assert(api->snapshot(&s)==0&&s.owner_tid==0&&s.sequence==0);
 attached=false;invoke();attached=true;assert(api->snapshot(&s)==0&&s.owner_tid==0&&s.sequence==0);
 order.clear();invoke();assert((order==std::vector<int>{3,1,2}));assert(begins==1&&ends==1);
 assert(api->snapshot(&s)==0&&s.owner_tid==gettid()&&s.sequence==1);
 // Both ABI signatures preserve arguments, outputs, result and exactly one original.
 void *arg=&arg_token;void *exc=nullptr;
 const int before=calls;assert(call_invoke(&other_token,&obj_token,&arg,&exc)==&result_token);
 assert(calls==before+1&&seen_method==&other_token&&seen_object==&obj_token&&seen_args==&arg&&seen_exception==&exc&&exc==&exception_token&&begins==1);
 assert(call_convert(&other_token,&obj_token,&arg,-17,&exc)==&result_token);
 assert(convert_calls==1&&seen_count==-17&&seen_args==&arg&&seen_exception==&exc&&exc==&exception_token&&begins==1);
 // Matching MethodInfo with noncanonical borrowed arguments must skip cleanup.
 assert(call_invoke(&execute_token,&obj_token,nullptr,nullptr)==nullptr&&begins==1);
 assert(call_convert(&execute_token,nullptr,nullptr,1,nullptr)==nullptr&&begins==1);
 order.clear();assert(call_convert(&execute_token,nullptr,nullptr,0,&exc)==nullptr);
 assert((order==std::vector<int>{4,1,2})&&begins==2&&ends==2);
 order.clear();nested_callback=true;invoke();assert((order==std::vector<int>{3,1,3,2})&&begins==3&&ends==3);
 order.clear();nested_original=true;invoke();assert((order==std::vector<int>{3,4,1,2})&&begins==4&&ends==4);
 // Original finishes first; revocation during begin skips remaining post work.
 order.clear();revoke_callback=true;invoke();assert((order==std::vector<int>{3,1})&&begins==5&&ends==4);
 assert(api->valid(generation)==0&&api->revoke(generation)==PGR_US_STALE);
 const auto old=generation;generation=api->activate();assert(generation>old);
 assert(api->revoke(old)==PGR_US_STALE&&api->valid(generation)==1);invoke();assert(begins==6&&ends==5);
 // Losing runtime attach proof after latch suppresses dispatch without relatching.
 attached=false;invoke();attached=true;assert(begins==6);
 assert(api->revoke(generation)==0);invoke();assert(begins==6);
 assert(api->snapshot(&s)==0&&s.installed==1&&!s.poisoned&&s.generation==0&&s.sequence==6);
 // Unrelated outer managed invocations must not suppress an inner ExecuteTasks.
 generation=api->activate();nested_original=true;order.clear();
 assert(call_invoke(&other_token,nullptr,nullptr,&exc)==&result_token);
 assert((order==std::vector<int>{3,4,1,2})&&begins==7&&ends==6);
 // A zero-parameter static method may receive a nonnull unused argument array.
 assert(call_convert(&execute_token,nullptr,&arg,0,&exc)==nullptr);
 assert(begins==8&&ends==7&&seen_args==&arg);
 // Unexpected borrowed result and managed exception suppress every subscriber.
 int b0=begins,e0=ends;unexpected_result=true;exc=nullptr;
 assert(call_invoke(&execute_token,nullptr,nullptr,&exc)==&result_token&&exc==nullptr);
 assert(call_convert(&execute_token,nullptr,nullptr,0,&exc)==&result_token&&exc==nullptr);
 unexpected_result=false;managed_exception=true;exc=nullptr;
 assert(call_convert(&execute_token,nullptr,nullptr,0,&exc)==nullptr&&exc==&exception_token);
 assert(call_invoke(&execute_token,nullptr,nullptr,&exc)==nullptr&&exc==&exception_token);
 managed_exception=false;assert(begins==b0&&ends==e0);
 // Missing exception output cannot prove success: forward with no subscriber work.
 assert(call_invoke(&execute_token,nullptr,nullptr,nullptr)==nullptr);
 assert(call_convert(&execute_token,nullptr,nullptr,0,nullptr)==nullptr);
 assert(begins==b0&&ends==e0);
 // Throw propagates unchanged and depth is released, permitting the next pulse.
 b0=begins;e0=ends;native_throw=true;bool caught=false;
 try{invoke();}catch(const OriginalFailure &failure){caught=failure.code==81;}
 assert(caught&&begins==b0&&ends==e0);caught=false;
 try{call_convert(&execute_token,nullptr,nullptr,0,&exc);}catch(const OriginalFailure &failure){caught=failure.code==81;}
 native_throw=false;assert(caught&&begins==b0&&ends==e0);invoke();
 assert(begins==b0+1&&ends==e0+1);
 // A revoke/reactivate within the original cannot deliver the old entry frame.
 b0=begins;e0=ends;const auto entering=generation;rotate_original=true;order.clear();invoke();
 assert(generation>entering&&begins==b0&&ends==e0&&order==std::vector<int>{3});
 invoke();assert(begins==b0+1&&ends==e0+1);
 // Noncanonical matching-method outer frames do not add canonical match depth.
 b0=begins;e0=ends;nested_original=true;order.clear();
 assert(call_invoke(&execute_token,&obj_token,nullptr,&exc)==nullptr&&exc==nullptr);
 assert((order==std::vector<int>{3,4,1,2})&&begins==b0+1&&ends==e0+1);
 b0=begins;e0=ends;nested_convert_original=true;order.clear();
 assert(call_convert(&execute_token,nullptr,nullptr,7,&exc)==nullptr&&exc==nullptr);
 assert((order==std::vector<int>{4,3,1,2})&&begins==b0+1&&ends==e0+1);
 assert(api->revoke(generation)==0);
 // API3 exposes only the original canonical ExecuteTasks execution context.
 generation=api3->activate();assert(generation);inspect_execution=true;expected_execution=generation;
 invoke();assert(api3->executing_generation(gettid())==0);
 assert(call_convert(&execute_token,nullptr,nullptr,0,&exc)==nullptr);
 assert(api3->executing_generation(gettid())==0);
 nested_unrelated=true;invoke(); // unrelated nested call retains the outer context
 nested_original=true;invoke(); // matching nested call shares the entry generation
 expected_execution=0;
 std::thread foreign([]{invoke();assert(api3->executing_generation(gettid())==0);});foreign.join();
 attached=false;invoke();attached=true;assert(api3->executing_generation(gettid())==0);
 expected_execution=generation;native_throw=true;caught=false;
 try{invoke();}catch(const OriginalFailure &failure){caught=failure.code==81;}
 assert(caught&&api3->executing_generation(gettid())==0);
 native_throw=false;invoke();assert(api3->executing_generation(gettid())==0);
 b0=begins;e0=ends;rotate_original=true;rotate_nested=true;invoke();
 assert(expected_execution==0&&begins==b0&&ends==e0);
 assert(api3->executing_generation(gettid())==0);
 expected_execution=generation;invoke();inspect_execution=false;
 assert(api3->revoke(generation)==0&&api3->executing_generation(gettid())==0);
 puts("PASS Unity scheduler: real Dobby invoke ABIs, exact forwarding, owner proof, nested calls, generations, post-original void success, exception depth recovery");
 // Process-lifetime Dobby trampolines: deliberately do not dlclose hooked library.
}

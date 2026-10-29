// Actual query57/Dobby forwarding fixture. Feature23 deliberately remains uninstalled:
// this verifies callback ABI/root lifetime, NOT transfer correlation or settlement.
#include "transfer_observers.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <thread>

namespace {
using Call=void(*)(void *,void *,const void *);
thread_local bool attached=true;
int thread_token,self_token,value_token,method_token,other_method;
uintptr_t next_handle=1;
std::map<uintptr_t,void *> roots;
unsigned created=0,freed=0,calls[2]={};
void *last_self[2]={},*last_value[2]={};
const void *last_method[2]={};
bool nested=false,throws=false,fail_pin=false;
struct OriginalFailure {int marker;};
void *current(){return attached?&thread_token:nullptr;}
uintptr_t make(void *o,int pinned){assert(pinned==1);if(fail_pin)return 0;
 assert(o);++created;uintptr_t h=next_handle++;roots[h]=o;return h;}
void *get(uintptr_t h){assert(roots.count(h));return roots[h];}
void release(uintptr_t h){assert(roots.erase(h)==1);++freed;}
extern "C" __attribute__((noinline)) void fixture_result(void *,void *,const void *);
extern "C" __attribute__((noinline)) void fixture_error(void *,void *,const void *);
Call volatile invoke_result=fixture_result,invoke_error=fixture_error;
void record(unsigned which,void *self,void *value,const void *method){
 ++calls[which];last_self[which]=self;last_value[which]=value;last_method[which]=method;
 if(nested&&which==0){nested=false;invoke_error(self,value,&other_method);}
 if(throws)throw OriginalFailure{71};
}
extern "C" __attribute__((noinline)) void fixture_result(void *s,void *v,const void *m){
 asm volatile(".rept 32; nop; .endr" ::: "memory");record(0,s,v,m);
}
extern "C" __attribute__((noinline)) void fixture_error(void *s,void *v,const void *m){
 asm volatile(".rept 32; nop; .endr" ::: "memory");record(1,s,v,m);
}
void check_call(unsigned which,void *s,void *v,const void *m){
 assert(last_self[which]==s&&last_value[which]==v&&last_method[which]==m);
 assert(roots.empty()&&created==freed);
}
}
int main(int argc,char **argv){
 assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
 auto getter=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(getter);
 auto core=static_cast<const pgrazor_native_api_v2 *>(getter(2));assert(core&&core->query_feature);
 assert(!core->query_feature(57,2));
 auto api=static_cast<const PgrTransferObserversApi *>(core->query_feature(57,1));
 assert(api&&api->version==1&&api->size==24&&api->installed()==0);
 PgrTransferObserversConfig cfg{1,sizeof(cfg),reinterpret_cast<void *>(fixture_result),
  reinterpret_cast<void *>(fixture_error),reinterpret_cast<void *>(current),
  reinterpret_cast<void *>(make),reinterpret_cast<void *>(get),reinterpret_cast<void *>(release)};
 assert(api->install(nullptr)<0);
 auto bad=cfg;bad.version=2;assert(api->install(&bad)<0);
 bad=cfg;bad.size--;assert(api->install(&bad)<0);
 bad=cfg;bad.error_address=bad.result_address;assert(api->install(&bad)<0);
 bad=cfg;bad.gchandle_get_target=nullptr;assert(api->install(&bad)<0);
 assert(api->installed()==0&&api->install(&cfg)==0&&api->installed()==1);
 assert(api->install(&cfg)==0);
 bad=cfg;bad.gchandle_free=reinterpret_cast<void *>(get);assert(api->install(&bad)<0);
 void *s=&self_token,*v=&value_token;const void *m=&method_token;
 invoke_result(s,v,m);check_call(0,s,v,m);assert(calls[0]==1&&created==2);
 invoke_error(s,v,m);check_call(1,s,v,m);assert(calls[1]==1&&created==4);
 unsigned before=created;
 std::thread detached([&]{attached=false;invoke_result(s,v,m);});detached.join();
 check_call(0,s,v,m);assert(calls[0]==2&&created==before);
 std::thread foreign([&]{invoke_error(s,v,m);});foreign.join();
 check_call(1,s,v,m);assert(calls[1]==2&&created==before+2);
 before=created;invoke_result(nullptr,nullptr,nullptr);
 check_call(0,nullptr,nullptr,nullptr);assert(calls[0]==3&&created==before);
 invoke_result(s,nullptr,m);check_call(0,s,nullptr,m);assert(created==before+1);
 nested=true;unsigned rc=calls[0],ec=calls[1];before=created;
 invoke_result(s,v,m);check_call(0,s,v,m);check_call(1,s,v,&other_method);
 assert(calls[0]==rc+1&&calls[1]==ec+1&&created==before+4);
 throws=true;rc=calls[0];before=created;bool caught=false;
 try{invoke_result(s,v,m);}catch(const OriginalFailure &e){caught=e.marker==71;}
 throws=false;assert(caught&&calls[0]==rc+1&&created==before+2);check_call(0,s,v,m);
 fail_pin=true;rc=calls[0];invoke_result(s,v,m);fail_pin=false;
 assert(calls[0]==rc+1);check_call(0,s,v,m);
 std::puts("PASS transfer observers actual-library: exact ABI, passthrough, pins, reentry, exception, null/config; feature23 correlation NOT tested");
 return 0;
}

// Reuse only the independent feature37 fake managed-runtime scaffold, not its baseline main.
#define main details_completion_lifetime_baseline_unused
#include "details_completion_lifetime_test.cpp"
#undef main
#include "details_recovery.h"
#include <functional>

namespace {
const PgrDetailsRecoveryApi *recovery=nullptr;
PgrDetailsRecoveryRequest recoveryRequest{};
std::function<void()> validationHook,countHook;
int countInvokes=0;
void *recoveryInvoke(const void *m,void *self,void **args,void **exc){
 if(m==&aliveMethod&&validationHook){auto cb=validationHook;validationHook=nullptr;cb();}
 if(m==&countMethod){++countInvokes;if(countHook){auto cb=countHook;countHook=nullptr;cb();}}
 return invoke(m,self,args,exc);
}
void resetRecovery(){
 reset();validationHook=nullptr;countHook=nullptr;countInvokes=0;
 recoveryRequest={};recoveryRequest.version=1;recoveryRequest.size=sizeof(recoveryRequest);
 memcpy(recoveryRequest.expected_key,req.expected_key,8);recoveryRequest.epoch=req.epoch;
 recoveryRequest.tid=owner;recoveryRequest.remaining_ms=2500;
 recoveryRequest.gui=req.gui;recoveryRequest.response=req.response;
}
void noOutputs(const PgrDetailsRecoveryOutput &o){
 assert(!o.gui&&!o.state&&!o.response&&!o.data&&!o.pokemon&&!o.dismissed);balanced();
}
void checkRecord(const PgrDetailsRecoveryOutput &o){
 Scene &s=scenes[sceneIndex];Obj *expected[]={&s.gui,&s.view,&s.response,&s.data,&s.pokemon,&s.action};
 void *slots[]={o.gui,o.state,o.response,o.data,o.pokemon,o.dismissed};
 std::set<void*> unique;
 for(int i=0;i<6;i++){
  assert(slots[i]&&unique.insert(slots[i]).second);
  assert(!borrowed.count(reinterpret_cast<uintptr_t>(slots[i])));
  assert(gcGet(reinterpret_cast<uintptr_t>(slots[i]))==expected[i]);
 }
 assert(gcGet(reinterpret_cast<uintptr_t>(o.response))==gcGet(reinterpret_cast<uintptr_t>(recoveryRequest.response)));
 assert(roots.size()==borrowed.size()+6&&clicks==0);
}
}

int main(int argc,char **argv){
 assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
 auto get=reinterpret_cast<const void*(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(get);
 const auto *core=static_cast<const pgrazor_native_api_v2*>(get(2));assert(core);
 api=static_cast<const PgrDetailsCompletionApi*>(core->query_feature(37,1));assert(api);
 recovery=static_cast<const PgrDetailsRecoveryApi*>(core->query_feature(38,1));assert(recovery&&recovery->size==32);
 assert(sizeof(PgrDetailsRecoveryRequest)==56&&sizeof(PgrDetailsRecoveryOutput)==64);
 owner=gettid();auto cfg=config();cfg.runtime_invoke=reinterpret_cast<void*>(recoveryInvoke);
 assert(recovery->install(&cfg)==0);assert(api->install(&cfg)==0); // actual feature37 prerequisite/config

 resetRecovery();PgrDetailsRecoveryOutput out{};
 assert(recovery->recover(&recoveryRequest,&out)==1);checkRecord(out);
 const int allAllocations=allocations;assert(countInvokes>=2); // recovery getter and actual37 validator
 // Simulate host adoption: independent copies stay live after release of all six native exports.
 void *slots[]={out.gui,out.state,out.response,out.data,out.pokemon,out.dismissed};uintptr_t hostCopies[6];
 for(int i=0;i<6;i++)hostCopies[i]=gcNew(gcGet(reinterpret_cast<uintptr_t>(slots[i])),0);
 assert(recovery->release(&out)==0);assert(recovery->release(&out)==0);
 assert(roots.size()==borrowed.size()+6);
 for(auto h:hostCopies){assert(gcGet(h));gcFree(h);}noOutputs(out);

 // Initial root allocation, both recovery getters, and feature37's alive invocation relocate the
 // entire scene to different addresses. Every old object is poisoned by the included scaffold.
 resetRecovery();moveInputs=true;moveBoxes=true;moveMethods=1|2|4;
 assert(recovery->recover(&recoveryRequest,&out)==1);assert(sceneIndex>=4);checkRecord(out);
 assert(recovery->release(&out)==0);noOutputs(out);

 // A changed authoritative response during actual37 validation can never be published as the
 // original pending catch, even if the replacement has the same class and a plausible scene.
 resetRecovery();Obj replacement{&cr};replacement.id=UINT64_MAX;replacement.value=3;
 validationHook=[&](){scenes[sceneIndex].view.a=&replacement;};
 assert(recovery->recover(&recoveryRequest,&out)==0);noOutputs(out);
 assert(gcGet(reinterpret_cast<uintptr_t>(recoveryRequest.response))==&scenes[sceneIndex].response);
 resetRecovery();validationHook=[](){scenes[sceneIndex].response.id^=uint64_t(1)<<63;};
 assert(recovery->recover(&recoveryRequest,&out)==0);noOutputs(out);

 for(int n=1;n<=allAllocations;n++){
  resetRecovery();failAllocation=n;const int rc=recovery->recover(&recoveryRequest,&out);
  assert(failedAllocation&&rc<=0);noOutputs(out);
 }

 resetRecovery();bool reentered=false;
 countHook=[&](){const size_t before=roots.size();PgrDetailsRecoveryOutput inner{};
  assert(recovery->recover(&recoveryRequest,&inner)==0);assert(!inner.gui&&!inner.response);
  assert(roots.size()==before);reentered=true;};
 assert(recovery->recover(&recoveryRequest,&out)==1&&reentered);checkRecord(out);recovery->release(&out);noOutputs(out);

 resetRecovery();recoveryRequest.remaining_ms=1;validationHook=[](){usleep(4000);};
 assert(recovery->recover(&recoveryRequest,&out)==0);noOutputs(out);
 roots.clear();borrowed.clear();
 puts("PASS recovery lifetime: actual37 validation, six OWN exports, expected response, release/adoption balance, real moving GC, root failures, expiry/reentry");
 dlclose(lib);
}

#include "summary_completion.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <set>
#include <unistd.h>

// Production feature36 ABI/lifetime test for Config168 and borrowed-host-handle Request72.
// No original worker harness is included, no hooks installed, no real game method invoked.
namespace {
int guiClass,stateClass,responseClass,boolClass,actionClass,otherClass;
int stateGui,stateProto,catchStatus,caughtId,currentState,onDismissed,aliveMethod,dismissMethod;
struct Gui {void *klass;int32_t state;void *callback;};
struct State {void *klass,*gui,*proto;};
struct Response {void *klass;int32_t status;uint8_t id[8];};
struct Action {void *klass;};
struct Boolean {void *klass;uint8_t value;};
struct Scene {Gui gui;State state;Response response;Action action;};
Scene scenes[8];int currentScene=0;
Boolean boxes[8];int currentBox=0;
std::map<uintptr_t,void*> handles;std::set<uintptr_t> hostHandles;
uintptr_t serial=0x1000;int newCalls=0,failNew=0,dismissCalls=0;
bool allocationFailed=false,moveAlive=false,moveDismiss=false,moveBox=false,moveOnInputRoot=false;
bool live=true,throwDismiss=false;
std::function<void()> onAlive;
const PgrSummaryCompletionApi *api=nullptr;
PgrSummaryCompletionRequest request{};uint64_t owner=0,token=0;

void relocate(){
 assert(currentScene<7);Scene &old=scenes[currentScene],&next=scenes[++currentScene];next=old;
 next.gui.callback=&next.action;next.state.gui=&next.gui;next.state.proto=&next.response;
 for(auto &entry:handles){
  if(entry.second==&old.gui)entry.second=&next.gui;
  else if(entry.second==&old.state)entry.second=&next.state;
  else if(entry.second==&old.response)entry.second=&next.response;
  else if(entry.second==&old.action)entry.second=&next.action;
 }
 old.gui.klass=old.state.klass=old.response.klass=old.action.klass=&otherClass;
 old.gui.state=99;old.gui.callback=nullptr;old.state.gui=old.state.proto=nullptr;
 old.response.status=-1;memset(old.response.id,0,8);
}
uintptr_t gcNew(void *o,int){
 ++newCalls;if(newCalls==failNew){allocationFailed=true;return 0;}if(!o)return 0;
 const uintptr_t h=++serial;handles[h]=o;
 // Collection inside root creation also tests the borrowed-handle protocol for later input roots.
 if(moveOnInputRoot&&o==&scenes[currentScene].gui){moveOnInputRoot=false;relocate();}
 if(moveBox&&o==&boxes[currentBox]){
  Boolean &old=boxes[currentBox],&next=boxes[++currentBox];next=old;
  for(auto &entry:handles)if(entry.second==&old)entry.second=&next;
  old.klass=&otherClass;old.value=7;
 }
 return h;
}
void *gcGet(uintptr_t h){auto i=handles.find(h);return i==handles.end()?nullptr:i->second;}
void gcFree(uintptr_t h){assert(!hostHandles.count(h));assert(handles.erase(h)==1);}
void *klass(void *o){return o?*static_cast<void**>(o):nullptr;}
void *unbox(void *o){assert(klass(o)==&boolClass);return &static_cast<Boolean*>(o)->value;}
void fieldGet(void *o,void *field,void *out){
 assert(o&&klass(o)!=&otherClass); // all obsolete addresses remain readable but are deliberately poisoned
 if(field==&stateGui)*static_cast<void**>(out)=static_cast<State*>(o)->gui;
 else if(field==&stateProto)*static_cast<void**>(out)=static_cast<State*>(o)->proto;
 else if(field==&catchStatus)*static_cast<int32_t*>(out)=static_cast<Response*>(o)->status;
 else if(field==&caughtId)memcpy(out,static_cast<Response*>(o)->id,8);
 else if(field==&currentState)*static_cast<int32_t*>(out)=static_cast<Gui*>(o)->state;
 else if(field==&onDismissed)*static_cast<void**>(out)=static_cast<Gui*>(o)->callback;
 else assert(false);
}
void *invoke(const void *method,void *receiver,void **params,void **exception){
 *exception=nullptr;
 if(method==&aliveMethod){
  assert(receiver==nullptr&&params&&params[0]==&scenes[currentScene].gui);
  // Reference arguments are actual managed object pointers, never &pointer-local.
  if(moveAlive){moveAlive=false;relocate();}
  if(onAlive){auto cb=onAlive;onAlive=nullptr;cb();}
  boxes[currentBox]={&boolClass,static_cast<uint8_t>(live?1:0)};return &boxes[currentBox];
 }
 assert(method==&dismissMethod&&receiver==&scenes[currentScene].gui);++dismissCalls;
 if(moveDismiss){moveDismiss=false;relocate();}
 scenes[currentScene].gui.state=3;
 if(throwDismiss)*exception=&otherClass;
 return nullptr;
}
void *borrow(void *o){const uintptr_t h=++serial;handles[h]=o;hostHandles.insert(h);return reinterpret_cast<void*>(h);}
void reset(bool withState=true){
 assert(handles.size()==hostHandles.size());handles.clear();hostHandles.clear();
 currentScene=currentBox=0;newCalls=failNew=dismissCalls=0;allocationFailed=false;
 moveAlive=moveDismiss=moveBox=moveOnInputRoot=throwDismiss=false;live=true;onAlive=nullptr;
 Scene &s=scenes[0];s={{&guiClass,2,&s.action},{&stateClass,&s.gui,&s.response},{&responseClass,3,{}},{&actionClass}};
 memset(s.response.id,255,8);boxes[0]={&boolClass,1};
 request={};request.version=PGR_SC_REQUEST_VERSION;request.size=sizeof(request);
 memset(request.expected_key,255,8);request.epoch=7;request.tid=owner;
 request.gui=borrow(&s.gui);request.state=withState?borrow(&s.state):nullptr;
 request.proto=borrow(&s.response);request.response=borrow(&s.response);request.dismissed=borrow(&s.action);
 assert(api->update(1,2500,7,owner)==PGR_SC_OK);
 token=0;assert(api->acquire(7,owner,&token)==PGR_SC_OK&&token);
}
void balanced(){assert(handles.size()==hostHandles.size());for(uintptr_t h:hostHandles)assert(handles.count(h));}
PgrSummaryCompletionOutput complete(int expected=PGR_SC_DONE){
 PgrSummaryCompletionOutput out{};const int rc=api->complete(&request,token,&out);
 assert(rc==expected);assert(out.version==1&&out.size==24);balanced();return out;
}
PgrSummaryCompletionConfig config(){
 PgrSummaryCompletionConfig c{};c.version=PGR_SC_CONFIG_VERSION;c.size=sizeof(c);
 c.object_get_class=reinterpret_cast<void*>(klass);c.field_get_value=reinterpret_cast<void*>(fieldGet);
 c.runtime_invoke=reinterpret_cast<void*>(invoke);c.object_unbox=reinterpret_cast<void*>(unbox);
 c.class_gui=&guiClass;c.class_state=&stateClass;c.class_response=&responseClass;c.class_bool=&boolClass;
 c.f_dscx=&stateGui;c.f_dscy=&stateProto;c.f_status=&catchStatus;c.f_captured_id=&caughtId;
 c.f_current_state=&currentState;c.f_on_dismissed=&onDismissed;c.m_alive=&aliveMethod;c.m_dismiss=&dismissMethod;
 c.gc_new=reinterpret_cast<void*>(gcNew);c.gc_get=reinterpret_cast<void*>(gcGet);c.gc_free=reinterpret_cast<void*>(gcFree);
 c.success_literal=3;c.state_open=2;return c;
}
}

int main(int argc,char **argv){
 assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
 auto get=reinterpret_cast<const void*(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(get);
 auto *core=static_cast<const pgrazor_native_api_v2*>(get(2));assert(core);
 api=static_cast<const PgrSummaryCompletionApi*>(core->query_feature(36,1));assert(api&&api->size==64);
 assert(sizeof(PgrSummaryCompletionConfig)==168&&sizeof(PgrSummaryCompletionRequest)==72);
 assert(offsetof(PgrSummaryCompletionConfig,gc_new)==136&&offsetof(PgrSummaryCompletionConfig,success_literal)==160);
 owner=gettid();auto cfg=config();assert(api->install(&cfg)==PGR_SC_OK);

 reset();assert(api->validate(&request)==PGR_SC_DONE);balanced();newCalls=0;
 auto out=complete();assert(dismissCalls==1&&(out.flags&PGR_SC_FLAG_INVOKED)&&out.after==3);
 const int completeAllocations=newCalls;assert(completeAllocations>=7);

 // Independent moving scenes: alive relocates every input, Dismiss relocates them again.
 for(bool withState:{false,true}){
  reset(withState);moveAlive=true;moveDismiss=true;moveBox=true;
  out=complete();assert(currentScene==2&&currentBox>=2&&dismissCalls==1);
  assert((out.flags&PGR_SC_FLAG_AFTER_KNOWN)&&out.after==3&&!(out.flags&PGR_SC_FLAG_GONE));
  reset(withState);moveOnInputRoot=true;moveAlive=true;moveBox=true;
  assert(api->validate(&request)==PGR_SC_DONE&&currentScene==2);balanced();
 }

 reset();newCalls=0;assert(api->validate(&request)==PGR_SC_DONE);const int validateAllocations=newCalls;balanced();
 assert(validateAllocations>=6);
 for(int n=1;n<=validateAllocations;++n){
  reset();failNew=n;assert(api->validate(&request)!=PGR_SC_DONE);assert(allocationFailed&&dismissCalls==0);balanced();
 }
 for(int n=1;n<=completeAllocations;++n){
  reset();failNew=n;PgrSummaryCompletionOutput failed{};api->complete(&request,token,&failed);
  assert(allocationFailed&&dismissCalls<=1);balanced();
  assert(!(failed.flags&PGR_SC_FLAG_GONE)); // inability to root an alive box is not proof of destruction
 }

 // A nested validation/completion from alive must be refused while the outer native call owns roots.
 for(bool outerComplete:{false,true}){
  reset();onAlive=[&](){
   const size_t before=handles.size();PgrSummaryCompletionOutput nested{};
   assert(api->validate(&request)!=PGR_SC_DONE);
   assert(api->complete(&request,token,&nested)!=PGR_SC_DONE&&!(nested.flags&PGR_SC_FLAG_INVOKED));
   assert(handles.size()==before);
  };
  if(outerComplete){out=complete();assert(dismissCalls==1);}
  else{assert(api->validate(&request)==PGR_SC_DONE);balanced();out=complete();assert(dismissCalls==1);}
 }

 reset();throwDismiss=true;moveDismiss=true;out=complete(PGR_SC_ERR_MANAGED);
 assert((out.flags&PGR_SC_FLAG_INVOKED)&&out.after==3&&dismissCalls==1);
 reset();onAlive=[](){live=false;};out=complete(PGR_SC_OK);assert(dismissCalls==0&&!(out.flags&PGR_SC_FLAG_INVOKED));
 // Unity-destroyed GUI after Dismiss still has a rooted managed wrapper; post-alive returns false.
 reset();onAlive=[](){onAlive=[](){live=false;};};out=complete();assert(dismissCalls==1&&(out.flags&PGR_SC_FLAG_GONE));
  // The post-invoke liveness call itself may invalidate the wrapper. A true Boolean from that call
  // cannot authorize reading currentState from a newly invalid class or manufacture advancement.
  reset();onAlive=[](){onAlive=[](){scenes[currentScene].gui.klass=&otherClass;};};
  out=complete();assert(dismissCalls==1&&(out.flags&PGR_SC_FLAG_INVOKED));
  assert(!(out.flags&PGR_SC_FLAG_AFTER_KNOWN)&&!(out.flags&PGR_SC_FLAG_GONE));
 balanced();handles.clear();hostHandles.clear();
 puts("PASS summary lifetime: borrowed handles, distinct-address relocation, boxed roots, failures, reentry, no-state, exception/gone");
 dlclose(lib);
}

#include "catch_canvas.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <thread>
#include <unistd.h>

// Independent feature39 production-table + real Dobby setter fixture. Borrowed host roots
// survive genuine relocation; old objects are poisoned rather than silently remaining valid.
namespace {
int detailsClass, summaryClass, boolClass, poisonClass, stateField, aliveMethod, canvasMethod;
struct Gui {void *klass; int32_t state;};
struct Box {void *klass; uint8_t value;};
struct Heap {Gui gui; Box box;};
Heap heaps[24]; int generation=0;
Gui unrelated{&detailsClass, 6};
std::map<uintptr_t,void*> handles;
uintptr_t nextHandle=100, hostHandle=0;
const PgrCatchCanvasApi *api=nullptr;
uint64_t owner=0, activeToken=0;
int invokes=0, originals=0, moves=0, foreignResult=0, busyResult=0;
void *lastSelf=nullptr, *lastMethod=nullptr; bool lastVisible=true;
bool moveOnAlive=false, revokeOnNew=false, releaseOnAlive=false;

Gui *gui(){return &heaps[generation].gui;}
Box *box(){return &heaps[generation].box;}
void relocate(){
 assert(generation+1<24);
 Heap *old=&heaps[generation], *fresh=&heaps[++generation];
 *fresh=*old;
 for(auto &entry:handles){
  if(entry.second==&old->gui)entry.second=&fresh->gui;
  else if(entry.second==&old->box)entry.second=&fresh->box;
 }
 old->gui.klass=&poisonClass;old->box.klass=&poisonClass;++moves;
}
uintptr_t gcNew(void *object,int){
 assert(object==gui()||object==box()||object==&unrelated);
 const uintptr_t h=++nextHandle;handles[h]=object;
 if(revokeOnNew){revokeOnNew=false;assert(api->stop()==0);}
 return h;
}
void *gcGet(uintptr_t h){auto it=handles.find(h);return it==handles.end()?nullptr:it->second;}
void gcFree(uintptr_t h){assert(handles.erase(h)==1);}
void *objectClass(void *object){
 assert(object==gui()||object==box()||object==&unrelated); // detects stale poisoned pointer
 return *static_cast<void**>(object);
}
void fieldGet(void *object,void *field,void *out){
 assert(object==gui()||object==&unrelated);assert(field==&stateField);
 *static_cast<int32_t*>(out)=static_cast<Gui*>(object)->state;
}
void *unbox(void *object){assert(object==box());return &box()->value;}

// Eight real instructions keep the hook prologue patchable on both project ABIs.
__attribute__((noinline)) void setter(void *self,bool visible,void *method){
 asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;" ::: "memory");
 ++originals;lastSelf=self;lastVisible=visible;lastMethod=method;
}
using Setter=void(*)(void*,bool,void*);
void callSetter(void *self,bool visible,void *method){
 Setter volatile target=&setter;target(self,visible,method);
}
void *invoke(const void *method,void *object,void **params,void **exception){
 ++invokes;*exception=nullptr;
 if(method==&aliveMethod){
  assert(object==nullptr&&params&&params[0]==gui()); // reference, never &GUI pointer
  if(releaseOnAlive){releaseOnAlive=false;busyResult=api->release(activeToken,owner);}
  if(moveOnAlive){moveOnAlive=false;relocate();}
  return box();
 }
 assert(method==&canvasMethod&&object==gui()&&params&&params[0]);
 const uint8_t value=*static_cast<uint8_t*>(params[0]);assert(value<=1);
 callSetter(object,value!=0,const_cast<void*>(method));return nullptr;
}
PgrCatchCanvasSample sample(){PgrCatchCanvasSample s{};assert(api->sample(&s)==0);return s;}
void arm(uint64_t epoch){assert(api->update(1,2500,epoch,owner)==0);}
uint64_t hide(uint64_t epoch){
 uint64_t token=0;assert(api->hide_opening(reinterpret_cast<void*>(hostHandle),epoch,owner,&token)==1);
 assert(token>0&&token<=((uint64_t{1}<<53)-1));return token;
}
void drained(){assert(sample().owned==0&&handles.size()==1);}
}

int main(int argc,char **argv){
 if(argc!=2)return 1;
 void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){std::puts(dlerror());return 2;}
 auto get=reinterpret_cast<const void*(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));
 auto core=get?static_cast<const pgrazor_native_api_v2*>(get(2)):nullptr;
 assert(core&&core->query_feature(39,99)==nullptr);
 api=static_cast<const PgrCatchCanvasApi*>(core->query_feature(39,1));
 assert(api&&api->version==1&&api->size==72);
 PgrCatchCanvasConfig c{};c.version=1;c.size=sizeof(c);
 c.object_get_class=reinterpret_cast<void*>(&objectClass);c.field_get_value=reinterpret_cast<void*>(&fieldGet);
 c.runtime_invoke=reinterpret_cast<void*>(&invoke);c.object_unbox=reinterpret_cast<void*>(&unbox);
 c.gc_new=reinterpret_cast<void*>(&gcNew);c.gc_get=reinterpret_cast<void*>(&gcGet);c.gc_free=reinterpret_cast<void*>(&gcFree);
 c.class_summary=&summaryClass;c.class_details=&detailsClass;c.class_bool=&boolClass;
 c.f_current_state=&stateField;c.m_alive=&aliveMethod;c.m_canvas=&canvasMethod;
 c.canvas_address=reinterpret_cast<void*>(&setter);c.state_open=6;
 assert(api->install(&c)==0);owner=static_cast<uint64_t>(gettid());
 heaps[0]={{&detailsClass,6},{&boolClass,1}};hostHandle=gcNew(gui(),0);

 // Hook only consults native ownership/gate: zero extra managed calls, exact original once.
 arm(1);activeToken=hide(1);assert(sample().owned==1&&handles.size()==2);
 int before=invokes, calls=originals;void *methodIdentity=&stateField;
 callSetter(gui(),true,methodIdentity);
 assert(invokes==before&&originals==calls+1&&lastSelf==gui()&&lastMethod==methodIdentity&&!lastVisible);
 calls=originals;callSetter(&unrelated,true,methodIdentity);
 assert(invokes==before&&originals==calls+1&&lastSelf==&unrelated&&lastVisible);
 std::thread foreign([&]{
  callSetter(gui(),true,methodIdentity);
  assert(lastVisible&&lastSelf==gui()&&lastMethod==methodIdentity);
  foreignResult=api->release(activeToken,owner);
 });foreign.join();
 assert(foreignResult==-12&&sample().owned==1&&invokes==before);

 // Cleanup refuses nested release distinctly; outer release detaches first and roots remain live.
 releaseOnAlive=true;busyResult=0;
 assert(api->release(activeToken,owner)==0);assert(busyResult==-13);drained();
 assert(lastVisible&&lastSelf==gui()&&lastMethod==&canvasMethod);

 // Restoring the stale epoch invokes alive and moves every managed object/root.
 // hide_opening must refresh its duplicate GUI after that cleanup, before object_get_class.
 arm(2);activeToken=hide(2);arm(3);moveOnAlive=true;
 const int moved=moves;const uint64_t replacement=hide(3);
 assert(moves==moved+1&&replacement!=activeToken&&sample().owned==1&&handles.size()==2);
 activeToken=replacement;assert(api->release(activeToken,owner)==0);drained();

 // Even same-GUI reuse must recheck generation after the duplicate root allocation reenters stop.
 arm(4);activeToken=hide(4);revokeOnNew=true;
 uint64_t refusedToken=999;const int hiddenBefore=originals;
 assert(api->hide_opening(reinterpret_cast<void*>(hostHandle),4,owner,&refusedToken)==0);
 assert(refusedToken==0&&originals==hiddenBefore&&handles.size()==2);
 assert(api->tick(owner)==0);drained();assert(lastVisible);
 assert(api->release(activeToken,owner)==0);drained(); // stale token is idempotent
 gcFree(hostHandle);assert(handles.empty());
 std::puts("PASS native canvas lifetime: actual hook, stale-slot relocation, reuse revoke, busy/foreign release, balanced roots");
 return 0;
}

#include <cstddef>
#include "routes_actions.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <sys/wait.h>
#include <unistd.h>

// Actual production feature43. Heap relocation on every allocation/invoke/free;
// COMPLETE synchronously probes real CURRENT, preserving callback-time route identity.
namespace {
int serviceClass,routesClass,playClass,sharedClass,dataClass,boolClass,stringClass,poisonClass,attached;
int currentMethod,startedMethod,endMethod,resumeMethod,completingMethod,initializedMethod,enabledMethod,consumingMethod,pausedMethod;
int startMethod,completeMethod,pauseMethod,discoverMethod,ctorMethod;
int routeField,pausedField,walkedField,rewardField,reverseField,idField,reversibleField,dataRouteField,dataReverseField;
enum {SERVICE,OTHER_SERVICE,PLAY,SHARED,ID,BOX,DATA,PROMISE,NOBJECTS};
struct Object {void *klass,*reference;uint8_t value;int32_t length;uint16_t text[128];};
struct Heap {Object object[NOBJECTS];};
Heap heaps[256];int generation=0,moves=0,managed=0,submissions=0,currentCalls=0;
std::map<uintptr_t,void*> roots;uintptr_t serial=100,hostService=0,hostOther=0;
const PgrRoutesActionsApi *api=nullptr;const char *mode=nullptr;
bool inConfirm=false,inLeaf=false,nestedAgain=false,cleanupChecked=false;
bool revokeCurrentCleanup=false;
int leafResult=999,recursiveResult=999,cleanupResult=999;PgrRoutesActionsOutput leafOutput{};
Object *obj(int i){return &heaps[generation].object[i];}
int index(void *p){for(int i=0;i<NOBJECTS;i++)if(p==obj(i))return i;assert(false&&"stale managed pointer");return -1;}
void move(){
 assert(generation+1<256);Heap &old=heaps[generation],&fresh=heaps[++generation];fresh=old;
 auto remap=[&](void *p){for(int i=0;i<NOBJECTS;i++)if(p==&old.object[i])return static_cast<void*>(&fresh.object[i]);return p;};
 for(auto &root:roots)root.second=remap(root.second);
 for(auto &object:fresh.object)object.reference=remap(object.reference);
 for(auto &object:old.object)object.klass=&poisonClass;++moves;
}
PgrRoutesActionsRequest request(uint32_t action,uint64_t seq=0){
 PgrRoutesActionsRequest r{};r.version=1;r.size=sizeof(r);r.tid=static_cast<uint64_t>(gettid());r.residual_ms=2500;r.generation=api->generation();
 r.service_handle=hostService;r.action=action;r.seq=seq;
 if(action==PGR_RA_ACT_COMPLETE||action==PGR_RA_ACT_START){r.id_length=7;const char *s="route-a";for(int i=0;i<7;i++)r.id[i]=s[i];}
 return r;
}
uintptr_t gcNew(void *p,int pinned){assert(!pinned);index(p);auto h=++serial;roots[h]=p;move();return h;}
void *gcGet(uintptr_t h){auto it=roots.find(h);return it==roots.end()?nullptr:it->second;}
void gcFree(uintptr_t h){
 const int which=index(roots.at(h));
 assert(roots.erase(h)==1);
 if(revokeCurrentCleanup&&which==SERVICE&&h!=hostService){revokeCurrentCleanup=false;api->revoke();}
 if(submissions&&!inConfirm&&!cleanupChecked){
  cleanupChecked=true;const int before=managed;auto r=request(PGR_RA_ACT_CURRENT);PgrRoutesActionsOutput out{};
  cleanupResult=api->execute(&r,&out);assert(cleanupResult<=0&&managed==before);
 }
 move();
}
void *objectClass(void *p){return obj(index(p))->klass;}
void *threadCurrent(){return &attached;}
void *unbox(void *p){assert(index(p)==BOX);return &obj(BOX)->value;}
void getField(void *p,void *field,void *out){
 const int i=index(p);
 if(field==&routeField){assert(i==PLAY);*static_cast<void**>(out)=obj(PLAY)->reference;}
 else if(field==&idField){assert(i==SHARED);*static_cast<void**>(out)=obj(SHARED)->reference;}
 else {assert(i==PLAY||i==SHARED);uint8_t value=(field==&walkedField||field==&reversibleField)?1:0;
  if((std::strcmp(mode,"invalid-paused")==0&&field==&pausedField)||(std::strcmp(mode,"invalid-reward")==0&&field==&rewardField))value=2;
  *static_cast<uint8_t*>(out)=value;}
}
void *newObject(void *klass){assert(klass==&dataClass);move();return obj(DATA);}
void setRef(void *p,void *field,void *value){assert(index(p)==DATA&&field==&dataRouteField&&index(value)==SHARED);obj(DATA)->reference=value;move();}
void setValue(void *p,void *field,void *value){assert(index(p)==DATA&&field==&dataReverseField);obj(DATA)->value=*static_cast<uint8_t*>(value);move();}
int32_t stringLength(void *p){assert(index(p)==ID);return obj(ID)->length;}
uint16_t *stringChars(void *p){assert(index(p)==ID);return obj(ID)->text;}
void *invoke(const void *method,void *receiver,void **args,void **exception){
 ++managed;*exception=nullptr;const int self=index(receiver);assert(self==SERVICE||self==OTHER_SERVICE||self==DATA);
 if(method==&completeMethod){
  assert(self==SERVICE&&!args);++submissions;move();inConfirm=true;
  if(std::strcmp(mode,"callback-current-id")==0)obj(ID)->text[6]='b';
  auto nested=request(PGR_RA_ACT_CURRENT);
  if(std::strcmp(mode,"wrong-service")==0)nested.service_handle=hostOther;
  const int before=currentCalls;inLeaf=true;leafResult=api->execute(&nested,&leafOutput);inLeaf=false;
  if(std::strcmp(mode,"wrong-service")==0)assert(currentCalls==before);
  inConfirm=false;return nullptr;
 }
 if(method==&currentMethod){
  assert(!args);++currentCalls;
  if(inConfirm&&inLeaf&&std::strcmp(mode,"revoke-leaf")==0)api->revoke();
  if(inConfirm&&inLeaf&&std::strcmp(mode,"one-leaf-depth")==0&&!nestedAgain){
   nestedAgain=true;const int before=managed;auto r=request(PGR_RA_ACT_CURRENT);PgrRoutesActionsOutput out{};
   recursiveResult=api->execute(&r,&out);assert(recursiveResult<=0&&managed==before);
  }
  move();return obj(PLAY);
 }
 if(method==&ctorMethod){assert(self==DATA&&!args);move();return nullptr;}
 assert(!args);obj(BOX)->value=method==&completingMethod||method==&pausedMethod?0:1;move();return obj(BOX);
}
int scenario(const char *name){
 mode=name;void *classes[NOBJECTS]={&serviceClass,&serviceClass,&playClass,&sharedClass,&stringClass,&boolClass,&dataClass,&dataClass};
 for(int i=0;i<NOBJECTS;i++)obj(i)->klass=classes[i];obj(PLAY)->reference=obj(SHARED);obj(SHARED)->reference=obj(ID);
 obj(ID)->length=7;for(int i=0;i<7;i++)obj(ID)->text[i]="route-a"[i];
 PgrRoutesActionsConfig c{};c.version=1;c.size=sizeof(c);
 c.runtime_invoke=reinterpret_cast<void*>(&invoke);c.object_get_class=reinterpret_cast<void*>(&objectClass);c.field_get_value=reinterpret_cast<void*>(&getField);c.object_unbox=reinterpret_cast<void*>(&unbox);
 c.gchandle_new=reinterpret_cast<void*>(&gcNew);c.gchandle_get_target=reinterpret_cast<void*>(&gcGet);c.gchandle_free=reinterpret_cast<void*>(&gcFree);c.thread_current=reinterpret_cast<void*>(&threadCurrent);
 c.object_new=reinterpret_cast<void*>(&newObject);c.field_set_value_object=reinterpret_cast<void*>(&setRef);c.field_set_value=reinterpret_cast<void*>(&setValue);
 c.class_consumption=&serviceClass;c.class_routes=&routesClass;c.class_play=&playClass;c.class_shared=&sharedClass;c.class_startData=&dataClass;c.class_bool=&boolClass;c.class_string=&stringClass;
 c.m_currentRoute=&currentMethod;c.m_hasStarted=&startedMethod;c.m_inRangeOfEnd=&endMethod;c.m_inRangeToResume=&resumeMethod;c.m_isCompleting=&completingMethod;
 c.m_flagInitialized=&initializedMethod;c.m_enabledForPlayer=&enabledMethod;c.m_isConsuming=&consumingMethod;c.m_isPaused=&pausedMethod;
 c.m_start=&startMethod;c.m_confirmComplete=&completeMethod;c.m_pause=&pauseMethod;c.m_requestNearby=&discoverMethod;c.m_ctor=&ctorMethod;
 c.f_playRoute=&routeField;c.f_playPaused=&pausedField;c.f_playCompletedWalk=&walkedField;c.f_playAcquiredReward=&rewardField;c.f_playTravelInReverse=&reverseField;
 c.f_sharedId=&idField;c.f_sharedReversible=&reversibleField;c.f_dataPublishedRoute=&dataRouteField;c.f_dataReversed=&dataReverseField;
 c.string_length=reinterpret_cast<void*>(&stringLength);c.string_chars=reinterpret_cast<void*>(&stringChars);assert(api->install(&c)==0);
 hostService=gcNew(obj(SERVICE),0);hostOther=gcNew(obj(OTHER_SERVICE),0);
 if(std::strcmp(mode,"current-cleanup-revoke")==0){
  revokeCurrentCleanup=true;auto current=request(PGR_RA_ACT_CURRENT);PgrRoutesActionsOutput result{};
  assert(api->execute(&current,&result)==PGR_RA_REFUSE&&result.status==PGR_RA_REFUSE&&result.flags==0&&result.id_length==0);
  assert(!revokeCurrentCleanup&&roots.size()==2&&submissions==0);gcFree(hostService);gcFree(hostOther);assert(roots.empty());
  std::puts("PASS independent Routes current-cleanup-revoke");return 0;
 }
 if(std::strcmp(mode,"invalid-paused")==0||std::strcmp(mode,"invalid-reward")==0){
  auto invalid=request(PGR_RA_ACT_COMPLETE,1);PgrRoutesActionsOutput result{};
  assert(api->execute(&invalid,&result)<0&&result.status<0&&result.flags==0&&submissions==0&&roots.size()==2);
  gcFree(hostService);gcFree(hostOther);assert(roots.empty());std::printf("PASS independent Routes %s\n",mode);return 0;
 }
 auto r=request(PGR_RA_ACT_COMPLETE,1);PgrRoutesActionsOutput out{};
 assert(api->execute(&r,&out)==PGR_RA_SUCCESS&&out.status==PGR_RA_SUCCESS&&submissions==1);
 if(std::strcmp(mode,"revoke-leaf")==0||std::strcmp(mode,"wrong-service")==0)assert(leafResult<=0&&leafOutput.flags==0);
 else {assert(leafResult==PGR_RA_SUCCESS&&leafOutput.id_length==7&&(leafOutput.flags&PGR_RA_FLAG_PRESENT));
  assert(leafOutput.id[6]==(std::strcmp(mode,"callback-current-id")==0?'b':'a'));}
 if(std::strcmp(mode,"one-leaf-depth")==0)assert(nestedAgain&&recursiveResult<=0);
 assert(cleanupChecked&&cleanupResult<=0&&roots.size()==2&&moves>15);
 r.generation=api->generation();out={};assert(api->execute(&r,&out)==PGR_RA_REFUSE&&out.reason==PGR_RA_REASON_STALE_SEQ&&submissions==1);
 gcFree(hostService);gcFree(hostOther);assert(roots.empty());std::printf("PASS independent Routes %s\n",name);return 0;
}
}
int main(int argc,char **argv){
 if(argc<2||argc>3)return 1;
 if(argc==2){const char *cases[]={"moving-complete","callback-current-id","revoke-leaf","wrong-service","one-leaf-depth","current-cleanup-revoke","invalid-paused","invalid-reward"};
  for(const char *name:cases){pid_t child=fork();if(child<0)return 3;if(!child){execl(argv[0],argv[0],argv[1],name,static_cast<char*>(nullptr));_exit(127);}
   int status=0;if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status)){std::printf("FAIL independent Routes %s (%d)\n",name,status);return 4;}}
  return 0;
 }
 void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!library){std::puts(dlerror());return 2;}
 auto get=reinterpret_cast<const void*(*)(uint32_t)>(dlsym(library,"pgrazor_native_api"));auto core=get?static_cast<const pgrazor_native_api_v2*>(get(2)):nullptr;
 assert(core&&core->query_feature(43,99)==nullptr);api=static_cast<const PgrRoutesActionsApi*>(core->query_feature(43,1));assert(api&&api->size==40);
 return scenario(argv[2]);
}

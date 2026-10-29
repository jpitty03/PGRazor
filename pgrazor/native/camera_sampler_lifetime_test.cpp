#include "camera_sampler.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <time.h>
#include <unistd.h>

// Independent production query41 fixture. All heap objects move on allocations,
// managed calls AND frees; old objects are poisoned. No game hooks or game data.
namespace {
int cameraClass,transformClass,vectorClass,rigClass,intClass,stringClass,poisonClass;
int mainMethod,transformMethod,forwardMethod,layerMethod,maskMethod,idMethod,fx,fy,fz,attached;
enum {RIG,CAMERA,TRANSFORM,VECTOR,STRING,LAYER,MASK,ID,NOBJECTS};
struct Object {void *klass;int32_t integer;float x,y,z;};
struct Heap {Object objects[NOBJECTS];};
Heap heaps[256];int generation=0,moves=0,invokes=0,newCalls=0;
std::map<uintptr_t,void*> handles;uintptr_t serial=100;
const PgrCameraSamplerApi *api=nullptr;PgrCameraSamplerOutput *outerOutput=nullptr;
bool reenterCleanup=false,sleepCleanup=false,sleepMain=false,sleepFirstRoot=false,finalPhase=false,slept=false;
bool checkedRig=false;int firstRoot=-1,nestedResult=0;
Object *object(int i){return &heaps[generation].objects[i];}
int index(void *p){for(int i=0;i<NOBJECTS;i++)if(p==object(i))return i;assert(false&&"stale managed pointer");return -1;}
void move(){
 assert(generation+1<256);Heap &old=heaps[generation],&fresh=heaps[++generation];fresh=old;
 for(auto &h:handles)for(int i=0;i<NOBJECTS;i++)if(h.second==&old.objects[i]){h.second=&fresh.objects[i];break;}
 for(auto &o:old.objects)o.klass=&poisonClass;++moves;
}
void sleepMs(int ms){timespec t{ms/1000,(ms%1000)*1000000L};nanosleep(&t,nullptr);slept=true;}
PgrCameraSamplerRequest request(uint64_t remaining){PgrCameraSamplerRequest r{};r.version=1;r.size=sizeof(r);r.tid=static_cast<uint64_t>(gettid());r.remaining_ms=remaining;r.rig=object(RIG);r.camera=object(CAMERA);return r;}
uintptr_t gcNew(void *p,int pinned){
 assert(!pinned);const int i=index(p);assert(checkedRig);
 if(newCalls++==0){firstRoot=i;assert(i==CAMERA);}
 const auto h=++serial;handles[h]=p;move();
 if(sleepFirstRoot&&newCalls==1){sleepFirstRoot=false;sleepMs(150);}
 return h;
}
void *gcGet(uintptr_t h){auto it=handles.find(h);return it==handles.end()?nullptr:it->second;}
void gcFree(uintptr_t h){
 assert(handles.erase(h)==1);
 if(outerOutput){assert(outerOutput->status==0&&outerOutput->camera_id==0&&outerOutput->x==0&&outerOutput->yaw==0);}
 if(reenterCleanup){reenterCleanup=false;const int before=invokes;
  auto r=request(2500);PgrCameraSamplerOutput nested{};
  nestedResult=api->sample(&r,&nested);assert(nestedResult==PGR_CS_ERR_BUSY&&nested.status==nestedResult&&invokes==before);
 }
 if(sleepCleanup&&finalPhase){sleepCleanup=false;sleepMs(250);}
 move();
}
void *objectClass(void *p){const int i=index(p);if(i==RIG){assert(newCalls==0);checkedRig=true;}return object(i)->klass;}
bool assignable(void *expected,void *actual){return expected==actual;}
void fieldGet(void *p,void *field,void *out){assert(index(p)==VECTOR);float value=field==&fx?object(VECTOR)->x:field==&fy?object(VECTOR)->y:object(VECTOR)->z;std::memcpy(out,&value,4);move();}
void *unbox(void *p){const int i=index(p);assert(i==LAYER||i==MASK||i==ID);return &object(i)->integer;}
void *threadCurrent(){return &attached;}
void *stringNew(const char *text){assert(std::strcmp(text,"WorldMap")==0);move();return object(STRING);}
void *invoke(const void *method,void *receiver,void **params,void **exception){
 ++invokes;*exception=nullptr;int result=-1;
 if(method==&mainMethod){assert(!receiver&&!params);if(sleepMain){sleepMain=false;sleepMs(150);}result=CAMERA;}
 else if(method==&layerMethod){assert(!receiver&&params&&params[0]==object(STRING));result=LAYER;}
 else {assert(!params);const int self=index(receiver);
  if(method==&transformMethod){assert(self==CAMERA);result=TRANSFORM;}
  else if(method==&forwardMethod){assert(self==TRANSFORM);result=VECTOR;}
  else if(method==&maskMethod){assert(self==CAMERA);result=MASK;}
  else {assert(method==&idMethod&&self==CAMERA);finalPhase=true;result=ID;}
 }
 move();return object(result);
}
int sample(uint64_t remaining,PgrCameraSamplerOutput &out){
 checkedRig=false;newCalls=0;firstRoot=-1;finalPhase=false;
 auto r=request(remaining);outerOutput=&out;const int rc=api->sample(&r,&out);outerOutput=nullptr;
 assert(out.status==rc&&handles.empty());return rc;
}
}
int main(int argc,char **argv){
 if(argc!=2)return 1;void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){std::puts(dlerror());return 2;}
 auto get=reinterpret_cast<const void*(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));
 auto core=get?static_cast<const pgrazor_native_api_v2*>(get(2)):nullptr;assert(core&&core->query_feature(41,99)==nullptr);
 api=static_cast<const PgrCameraSamplerApi*>(core->query_feature(41,1));assert(api&&api->size==24&&api->version==1);
 PgrCameraSamplerConfig c{};c.version=1;c.size=sizeof(c);
 c.runtime_invoke=reinterpret_cast<void*>(&invoke);c.object_get_class=reinterpret_cast<void*>(&objectClass);c.class_is_assignable_from=reinterpret_cast<void*>(&assignable);
 c.field_get_value=reinterpret_cast<void*>(&fieldGet);c.object_unbox=reinterpret_cast<void*>(&unbox);
 c.gchandle_new=reinterpret_cast<void*>(&gcNew);c.gchandle_get_target=reinterpret_cast<void*>(&gcGet);c.gchandle_free=reinterpret_cast<void*>(&gcFree);
 c.thread_current=reinterpret_cast<void*>(&threadCurrent);c.string_new=reinterpret_cast<void*>(&stringNew);
 c.class_camera=&cameraClass;c.class_transform=&transformClass;c.class_vector=&vectorClass;c.class_rig=&rigClass;c.class_int=&intClass;c.class_string=&stringClass;
 c.m_main=&mainMethod;c.m_transform=&transformMethod;c.m_forward=&forwardMethod;c.m_nametolayer=&layerMethod;c.m_culling=&maskMethod;c.m_instanceid=&idMethod;
 c.f_x=&fx;c.f_y=&fy;c.f_z=&fz;c.expected_layer=13;assert(api->install(&c)==0);
 void *classes[NOBJECTS]={&rigClass,&cameraClass,&transformClass,&vectorClass,&stringClass,&intClass,&intClass,&intClass};
 for(int i=0;i<NOBJECTS;i++)object(i)->klass=classes[i];
 object(VECTOR)->x=1;object(VECTOR)->y=0;object(VECTOR)->z=0;
 object(LAYER)->integer=13;object(MASK)->integer=8192;object(ID)->integer=1234;
 PgrCameraSamplerOutput out{};reenterCleanup=true;
 assert(sample(2500,out)==PGR_CS_SAMPLE&&firstRoot==CAMERA&&nestedResult==PGR_CS_ERR_BUSY);
 assert(out.camera_id==1234&&out.mask==8192&&out.layer==13&&out.x==1&&out.y==0&&out.z==0&&std::fabs(out.yaw-90)<1e-8&&moves>10);
 out={};slept=false;sleepFirstRoot=true;int allocationBefore=invokes;
 assert(sample(100,out)==PGR_CS_UNAVAILABLE&&slept&&out.reason==PGR_CS_REASON_LEASE);
 assert(invokes==allocationBefore); // the first natural-camera root exhausted the window
 out={};slept=false;sleepCleanup=true;
 assert(sample(200,out)==PGR_CS_UNAVAILABLE&&slept&&out.reason==PGR_CS_REASON_LEASE);
 assert(!out.camera_id&&!out.mask&&!out.layer&&out.x==0&&out.yaw==0);
 out={};slept=false;sleepMain=true;const int before=invokes;
 assert(sample(100,out)==PGR_CS_UNAVAILABLE&&slept&&out.reason==PGR_CS_REASON_LEASE);
 assert(invokes==before+1); // no further managed reads after the expired get_main
 auto bad=request(2500);bad.version=99;out={};const int rc=api->sample(&bad,&out);
 assert(rc==PGR_CS_ERR_BAD_VERSION&&out.status==rc&&handles.empty());
 object(CAMERA)->klass=&poisonClass;out={};
 assert(sample(2500,out)==PGR_CS_ERR_METADATA&&out.status==PGR_CS_ERR_METADATA);
 std::puts("PASS native camera lifetime: moving raw inputs/roots, direct args, cleanup busy+expiry, between-call lease, output status");
 return 0;
}

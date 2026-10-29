#include "catch_discovery.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <unistd.h>

// Independent actual-library feature40 fixture: every allocation moves the complete heap.
// release additionally reenters discovery using the SAME output buffer during gc_free.
namespace {
int layerClass,detailsClass,typeClass,arrayClass,poisonClass,topMethod,findMethod,typeMetadata;
struct Object {void *klass;};
struct Array {void *klass,*monitor,*bounds;uintptr_t length;void *values[16];};
static_assert(offsetof(Array,values)==32,"array layout");
struct Heap {Object layer,details[2],type;Array array;};
Heap heaps[96];int generation=0,moves=0,invokes=0;
std::map<uintptr_t,void*> handles;uintptr_t serial=100,host=0;
const PgrCatchDiscoveryApi *api=nullptr;
PgrCatchDiscoveryRequest request{};PgrCatchDiscoveryOutput *reenterOutput=nullptr;
int nestedResult=0;bool moveAlloc=true,reenterFree=false;
Heap &heap(){return heaps[generation];}
void *remap(void *p,Heap &old,Heap &next){
 if(p==&old.layer)return &next.layer;if(p==&old.type)return &next.type;
 if(p==&old.array)return &next.array;
 for(int i=0;i<2;++i)if(p==&old.details[i])return &next.details[i];
 return p;
}
void move(){
 assert(generation+1<96);Heap &old=heap(),&next=heaps[++generation];next=old;
 for(auto &entry:handles)entry.second=remap(entry.second,old,next);
 for(void *&p:next.array.values)p=remap(p,old,next);
 old.layer.klass=old.type.klass=old.array.klass=&poisonClass;
 for(auto &o:old.details)o.klass=&poisonClass;++moves;
}
bool current(void *p){return p==&heap().layer||p==&heap().type||p==&heap().array||p==&heap().details[0]||p==&heap().details[1];}
uintptr_t gcNew(void *p,int){assert(current(p));const auto h=++serial;handles[h]=p;if(moveAlloc)move();return h;}
void *gcGet(uintptr_t h){const auto it=handles.find(h);return it==handles.end()?nullptr:it->second;}
void gcFree(uintptr_t h){
 assert(handles.erase(h)==1);
 if(reenterFree){reenterFree=false;const int before=invokes;
  nestedResult=api->scene(&request,reenterOutput);
  assert(nestedResult==PGR_CD_ERR_BUSY&&invokes==before);
 }
}
void *objectClass(void *p){assert(current(p));return *static_cast<void**>(p);}
void *classType(void *p){assert(p==&detailsClass);return &typeMetadata;}
void *typeObject(void *p){assert(p==&typeMetadata);return &heap().type;}
uintptr_t length(void *p){assert(p==&heap().array);return heap().array.length;}
void *invoke(const void *method,void *receiver,void **params,void **exception){
 ++invokes;*exception=nullptr;
 if(method==&topMethod){assert(receiver==&heap().layer&&params==nullptr);move();return &heap().details[0];}
 assert(method==&findMethod&&receiver==nullptr&&params&&params[0]==&heap().type);
 move();return &heap().array;
}
void clean(const PgrCatchDiscoveryOutput &out){assert(out.count==0);for(void *h:out.handles)assert(!h);}
}
int main(int argc,char **argv){
 if(argc!=2)return 1;void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){std::puts(dlerror());return 2;}
 auto get=reinterpret_cast<const void*(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));
 auto core=get?static_cast<const pgrazor_native_api_v2*>(get(2)):nullptr;
 assert(core&&core->query_feature(40,99)==nullptr);
 api=static_cast<const PgrCatchDiscoveryApi*>(core->query_feature(40,1));assert(api&&api->version==1&&api->size==40);
 PgrCatchDiscoveryConfig c{};c.version=1;c.size=sizeof(c);
 c.object_get_class=reinterpret_cast<void*>(&objectClass);c.runtime_invoke=reinterpret_cast<void*>(&invoke);
 c.gchandle_new=reinterpret_cast<void*>(&gcNew);c.gchandle_get_target=reinterpret_cast<void*>(&gcGet);c.gchandle_free=reinterpret_cast<void*>(&gcFree);
 c.class_get_type=reinterpret_cast<void*>(&classType);c.type_get_object=reinterpret_cast<void*>(&typeObject);c.array_length=reinterpret_cast<void*>(&length);
 c.class_layer=&layerClass;c.class_details=&detailsClass;c.m_top=&topMethod;c.m_find=&findMethod;
 assert(api->install(&c)==0);
 heap().layer.klass=&layerClass;heap().type.klass=&typeClass;
 for(auto &o:heap().details)o.klass=&detailsClass;
 heap().array.klass=&arrayClass;heap().array.length=2;
 heap().array.values[0]=&heap().details[0];heap().array.values[1]=&heap().details[1];
 host=gcNew(&heap().layer,0);
 request.version=1;request.size=sizeof(request);request.tid=static_cast<uint64_t>(gettid());request.remaining_ms=2500;request.layer=reinterpret_cast<void*>(host);
 PgrCatchDiscoveryOutput out{};
 assert(api->top(&request,&out)==PGR_CD_OK&&out.count==1);
 assert(gcGet(reinterpret_cast<uintptr_t>(out.handles[0]))==&heap().details[0]);
 assert(api->release(&out)==PGR_CD_OK);clean(out);assert(handles.size()==1);
 assert(api->scene(&request,&out)==PGR_CD_OK&&out.count==2);
 for(int i=0;i<2;++i)assert(gcGet(reinterpret_cast<uintptr_t>(out.handles[i]))==&heap().details[i]);
 assert(handles.size()==3&&moves>=8);
 // All output roots must be detached before the first free. Rejected nested discovery
 // restamps/zeros this same buffer; it must not erase still-owned roots or invoke managed code.
 reenterOutput=&out;reenterFree=true;
 assert(api->release(&out)==PGR_CD_OK);assert(nestedResult==PGR_CD_ERR_BUSY);clean(out);assert(handles.size()==1);
 assert(api->release(&out)==PGR_CD_OK);clean(out);assert(handles.size()==1);
 gcFree(host);assert(handles.empty());
 std::puts("PASS native discovery lifetime: moving heap and same-output release reentry, balanced roots");
 return 0;
}

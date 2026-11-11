#include "deep_radar_dex.h"
#include "shared_host.h"
#include "vgp_discovery.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#ifdef _WIN32
extern "C" void *dlsym(void*,const char*) { return nullptr; }
#endif

namespace {
using namespace pgr_shared_meta;
enum Role { Array,Wild,Id,Location,Roles };
struct Object { unsigned role,epoch; };
unsigned epoch=1,invoke_count=0,throw_at=0,revoke_at=0,next_handle=1;
bool permitted=true,wrong_location=false;
std::vector<std::unique_ptr<Object>> heap;
Object*current[Roles]{};
std::map<uintptr_t,unsigned> roots;
int array_class,wild_class,id_class,location_class,id_method,location_method;
uint64_t encounter=UINT64_MAX;
double point[2]={35,139};
PgrSharedMetadataOutput metadata{};
PgrMetaApi meta{};PgrSharedHostApi host{};PgrVgpDiscoveryApi discovery{};
void move_heap(){++epoch;for(unsigned i=0;i<Roles;++i){heap.emplace_back(new Object{i,epoch});current[i]=heap.back().get();}}
Object*live(void*p){auto*o=static_cast<Object*>(p);assert(o&&o->epoch==epoch);return o;}
uintptr_t root_new(void*p,int){unsigned role=live(p)->role;uintptr_t h=next_handle++;roots[h]=role;move_heap();return h;}
void*root_get(uintptr_t h){assert(roots.count(h));return current[roots[h]];}
void root_free(uintptr_t h){assert(roots.erase(h)==1);move_heap();}
const void*object_class(void*p){switch(live(p)->role){case Array:return &array_class;case Wild:return &wild_class;case Id:return &id_class;default:return wrong_location?&id_class:&location_class;}}
const void*unbox(void*p){switch(live(p)->role){case Id:return &encounter;case Location:return point;default:assert(false);return nullptr;}}
void*invoke(const void*m,void*object,void**,void**exception){assert(permitted&&live(object)->role==Wild);assert(roots.size()>=2);++invoke_count;move_heap();if(invoke_count==revoke_at)permitted=false;if(invoke_count==throw_at){*exception=current[Id];return nullptr;}assert(m==&id_method||m==&location_method);return current[m==&id_method?Id:Location];}
int resolve_class(const PgrMetaClassQuery*q,PgrMetaClassResult*out){out->klass=!std::strcmp(q->name,"LatLng")?&location_class:!std::strcmp(q->name,"UInt64")?&id_class:nullptr;return out->klass?0:-1;}
int value_size(const void*k,uint32_t*alignment){*alignment=8;return k==&location_class?16:k==&id_class?8:0;}
int enumerate(uint32_t kind,uint64_t owner,uint64_t lease,PgrVgpDiscoveryEnumeration*out){assert(permitted&&kind==PGR_VD_KIND_CATCH&&owner==123&&lease<=2500);out->count=1;out->array_handle=root_new(current[Array],0);return PGR_VD_OK;}
int at(void*array,uint32_t i,uint64_t owner,uint64_t lease,uintptr_t*out){assert(permitted&&live(array)->role==Array&&i==0&&owner==123&&lease<=2500);*out=root_new(current[Wild],0);return PGR_VD_OK;}
bool allowed(){return permitted;}
void reset(){assert(roots.empty());permitted=true;wrong_location=false;invoke_count=throw_at=revoke_at=0;point[0]=35;point[1]=139;move_heap();}
}
extern "C" const void *pgrazor_runtime_metadata_api(uint32_t){return &meta;}
extern "C" const void *pgrazor_shared_host_api(uint32_t){return &host;}
extern "C" const void *pgrazor_vgp_discovery_api(uint32_t){return &discovery;}

int main(){
 meta.resolve_class=resolve_class;host.metadata=[]()->const PgrSharedMetadataOutput*{return &metadata;};discovery.enumerate=enumerate;discovery.at=at;
 metadata.opt_handles[H_VGP_M_LOCATION]=&location_method;metadata.opt_handles[H_VGP_M_ID]=&id_method;metadata.opt_classes[OC_VGP_WILD]=&wild_class;
 PgrBootBinding b{};b.meta.class_value_size=reinterpret_cast<void*>(value_size);b.runtime.runtime_invoke=reinterpret_cast<void*>(invoke);b.runtime.gchandle_new=reinterpret_cast<void*>(root_new);b.runtime.gchandle_get_target=reinterpret_cast<void*>(root_get);b.runtime.gchandle_free=reinterpret_cast<void*>(root_free);b.runtime.object_get_class=reinterpret_cast<void*>(object_class);b.runtime.object_unbox=reinterpret_cast<void*>(unbox);
 auto packet=std::make_unique<pgr::radar::Packet>();auto store=std::make_unique<pgr::radar::Store>();packet->sequence=1;packet->observed_ms=1000;packet->count=1;packet->observations[0].encounter=encounter;packet->observations[0].species=25;
 auto prepare_store=[&](){store->clear();assert(store->ingest(*packet));};
 reset();prepare_store();assert(pgr_deep_radar_scene(b,*store,123,1000,1000,allowed)==1);assert(roots.empty()&&invoke_count==2);assert(store->at(0).observation.kind==pgr::radar::Exact&&store->at(0).observation.lat==35&&store->at(0).observation.lon==139);assert(store->at(0).seen_ms==1000&&store->at(0).expires_ms==91000);
 reset();prepare_store();throw_at=2;assert(pgr_deep_radar_scene(b,*store,123,1000,1000,allowed)==0&&roots.empty());assert(store->at(0).observation.kind==pgr::radar::Unlocated);
 reset();prepare_store();revoke_at=1;assert(pgr_deep_radar_scene(b,*store,123,1000,1000,allowed)==0&&roots.empty()&&invoke_count==1);
 reset();prepare_store();wrong_location=true;assert(pgr_deep_radar_scene(b,*store,123,1000,1000,allowed)==0&&roots.empty());
 reset();prepare_store();point[0]=100;assert(pgr_deep_radar_scene(b,*store,123,1000,1000,allowed)==0&&roots.empty());
 reset();prepare_store();permitted=false;assert(pgr_deep_radar_scene(b,*store,123,1000,1000,allowed)==0&&roots.empty()&&invoke_count==0);
 std::puts("PASS radar scene: moving GC at invoke/new/free, exact uint64 joins, no TTL extension, exception/revocation/class/coordinate failure, all roots released");
}

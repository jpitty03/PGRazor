#include "deep_radar_dex.h"
#include "game_bindings.h"
#include "shared_host.h"
#include "vgp_discovery.h"
#include <cstring>
#include <dlfcn.h>
namespace {
struct Root {
 const PgrBootBinding &b;uintptr_t h=0;
 explicit Root(const PgrBootBinding&boot):b(boot){}
 Root(const Root&)=delete;Root&operator=(const Root&)=delete;
 ~Root(){if(h)reinterpret_cast<void(*)(uintptr_t)>(b.runtime.gchandle_free)(h);}
 bool set(void *v){if(!v||h)return false;h=reinterpret_cast<uintptr_t(*)(void*,int)>(b.runtime.gchandle_new)(v,0);return h&&get();}
 void *get()const{return h?reinterpret_cast<void*(*)(uintptr_t)>(b.runtime.gchandle_get_target)(h):nullptr;}
};
const void *contract=nullptr,*concrete=nullptr,*has_captured=nullptr,*bool_class=nullptr;
void *(*type_object)(const void*)=nullptr;
bool attempted=false,ready=false;
const void *klass(const PgrMetaApi *api,const char*ns,const char*name){PgrMetaClassQuery q{1,sizeof(q),ns,name,nullptr,PGR_MD_MATCH_EXACT,0};PgrMetaClassResult r{};return api->resolve_class(&q,&r)==0?r.klass:nullptr;}
bool prepare(const PgrBootBinding &b){if(attempted)return ready;attempted=true;const auto*api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));if(!api)return false;
 contract=klass(api,"Niantic.Holoholo","IPokedex");concrete=klass(api,"Niantic.Holoholo.Internal","PokedexImpl");bool_class=klass(api,"System","Boolean");if(!contract||!concrete||!bool_class)return false;
 type_object=reinterpret_cast<decltype(type_object)>(dlsym(b.handle,"il2cpp_type_get_object"));PgrMetaCodeQuery cq{1,sizeof(cq),reinterpret_cast<void*>(type_object),4,nullptr,0,0};if(!type_object||api->validate_code(&cq))return false;
 constexpr unsigned char bytes[]=PGR_GAME_DEEP_RADAR_CAPTURED_FINGERPRINT;PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=concrete;q.name=PGR_GAME_DEEP_RADAR_CAPTURED_NAME;q.return_type="System.Boolean";q.return_match=PGR_MD_MATCH_EXACT;q.param_count=1;q.param_types[0]="System.Int32";q.param_match[0]=PGR_MD_MATCH_EXACT;q.expected_code=static_cast<const char*>(b.module_base)+PGR_GAME_DEEP_RADAR_CAPTURED_RVA;q.prologue=bytes;q.prologue_len=sizeof(bytes);PgrMetaMethodResult r{};if(api->validate_method(&q,&r))return false;has_captured=r.method;return ready=true;
}
}
int pgr_deep_radar_uncaught(const PgrBootBinding &b,int32_t species,bool(*allowed)()){
 using namespace pgr_shared_meta;if(!allowed()||!prepare(b)||!allowed())return -1;
 const auto*host=static_cast<const PgrSharedHostApi*>(pgrazor_shared_host_api(1));const auto*md=host?host->metadata():nullptr;if(!md)return -1;
 auto h=[&](unsigned i){return md->opt_handles[i];};if(!h(H_IL_MAP)||!h(H_IL_FIND_M)||!h(H_IL_CONTAINER_M)||!h(H_IL_RESOLVE_M))return -1;
 auto invoke=[&](const void*m,void*self,void**args,Root&out){if(!allowed())return false;void *exception=nullptr;void *v=reinterpret_cast<void*(*)(const void*,void*,void**,void**)>(b.runtime.runtime_invoke)(m,self,args,&exception);return !exception&&v&&out.set(v)&&allowed();};
 // One root is established at each step, before acquiring the next reference.
 // Reference args are re-resolved immediately before their invocation.
 Root mapType(b),map(b),container(b),contractType(b),dex(b),result(b);
 auto classType=reinterpret_cast<const void*(*)(const void*)>(b.meta.class_get_type);
 if(!allowed()||!mapType.set(type_object(classType(h(H_IL_MAP))))||!allowed())return -1;
 void *findArgs[]={mapType.get()};if(!invoke(h(H_IL_FIND_M),nullptr,findArgs,map)||!invoke(h(H_IL_CONTAINER_M),map.get(),nullptr,container))return -1;
 if(!allowed()||!contractType.set(type_object(classType(contract)))||!allowed())return -1;
 void *resolveArgs[]={contractType.get()};if(!invoke(h(H_IL_RESOLVE_M),container.get(),resolveArgs,dex))return -1;
 auto objectClass=reinterpret_cast<const void*(*)(void*)>(b.runtime.object_get_class);if(objectClass(dex.get())!=concrete)return -1;
 void *args[]={&species};if(!invoke(has_captured,dex.get(),args,result)||objectClass(result.get())!=bool_class)return -1;
 const void *value=reinterpret_cast<const void*(*)(void*)>(b.runtime.object_unbox)(result.get());if(!value)return -1;uint8_t captured=0;std::memcpy(&captured,value,1);return captured<=1?1-int(captured):-1;
}

// Resolve only encounters already observed in a passive response. Enumeration
// and getters are local scene reads; no encounter/capture/refresh APIs are used.
unsigned pgr_deep_radar_scene(const PgrBootBinding&b,pgr::radar::Store&store,uint64_t owner,uint64_t now,uint64_t lease,bool(*allowed)()){
 using namespace pgr_shared_meta;
 if(!allowed()||!store.count())return 0;
 const auto*host=static_cast<const PgrSharedHostApi*>(pgrazor_shared_host_api(1));const auto*md=host?host->metadata():nullptr;
 const auto*api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));
 const auto*discovery=static_cast<const PgrVgpDiscoveryApi*>(pgrazor_vgp_discovery_api(1));
 if(!md||!api||!discovery||!md->opt_handles[H_VGP_M_LOCATION]||!md->opt_handles[H_VGP_M_ID])return 0;
 const void *latlng=klass(api,"Niantic","LatLng"),*u64=klass(api,"System","UInt64");if(!latlng||!u64||!allowed())return 0;
 auto size=reinterpret_cast<int(*)(const void*,uint32_t*)>(b.meta.class_value_size);uint32_t alignment=0;
 if(size(latlng,&alignment)!=16||size(u64,&alignment)!=8)return 0;
 auto objectClass=reinterpret_cast<const void*(*)(void*)>(b.runtime.object_get_class);
 auto unbox=reinterpret_cast<const void*(*)(void*)>(b.runtime.object_unbox);
 auto invoke=[&](const void*m,const Root&object,Root&result){if(!allowed())return false;void *exception=nullptr;void*v=reinterpret_cast<void*(*)(const void*,void*,void**,void**)>(b.runtime.runtime_invoke)(m,object.get(),nullptr,&exception);return !exception&&v&&result.set(v)&&allowed();};
 PgrVgpDiscoveryEnumeration e{};Root array(b);
 const int rc=discovery->enumerate(PGR_VD_KIND_CATCH,owner,lease,&e);array.h=e.array_handle;
 if(rc!=PGR_VD_OK||!array.h||e.count>2048||!allowed())return 0;
 unsigned resolved=0;
 for(uint32_t i=0;i<e.count&&i<256&&allowed();++i){Root object(b),id(b),location(b);uintptr_t owned=0;
  const int at=discovery->at(array.get(),i,owner,lease,&owned);object.h=owned;
  if(at!=PGR_VD_OK||!object.h||objectClass(object.get())!=md->opt_classes[OC_VGP_WILD])continue;
  if(!invoke(md->opt_handles[H_VGP_M_ID],object,id)||objectClass(id.get())!=u64)continue;
  uint64_t encounter=0;const void*value=unbox(id.get());if(!value)continue;std::memcpy(&encounter,value,8);
  bool present=false;for(size_t j=0;j<store.count();++j)if(store.at(j).observation.encounter==encounter){present=true;break;}if(!present)continue;
  if(!invoke(md->opt_handles[H_VGP_M_LOCATION],object,location)||objectClass(location.get())!=latlng)continue;
  double point[2]{};value=unbox(location.get());if(!value)continue;std::memcpy(point,value,sizeof(point));
  if(allowed()&&store.resolve(encounter,point[0],point[1],now))++resolved;
 }
 return resolved;
}

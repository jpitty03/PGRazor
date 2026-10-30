#define main discovery_baseline_main_unused
#include "vgp_discovery_test.cpp"
#undef main
extern "C" void pgr_vgp_discovery_loaded_method(void*);
namespace {
int loaded_method,loaded_calls=0;
void *menu_invoke(void *m,void *o,void **args,void **ex){
 if(m==&loaded_method){assert(args&&*static_cast<uint8_t*>(args[1])==1);++loaded_calls;return fake_invoke(&m_findAll,o,args,ex);}
 return fake_invoke(m,o,args,ex);
}
}
int main(){
 auto api=static_cast<const PgrVgpDiscoveryApi*>(pgrazor_vgp_discovery_api(1));
 auto c=make_config();c.runtime_invoke=reinterpret_cast<void*>(menu_invoke);assert(api->install(&c)==0);reset();
 PgrVgpDiscoveryEnumeration out{};
 assert(api->enumerate(PGR_VD_KIND_CATCH_LOADED,this_tid(),2000,&out)==PGR_VD_ERR_METADATA);assert(g_handles.empty());
 pgr_vgp_discovery_loaded_method(&loaded_method);
 assert(api->enumerate(PGR_VD_KIND_CATCH_LOADED,this_tid(),2000,&out)==PGR_VD_OK);assert(loaded_calls==1&&out.array_handle);fake_gc_free(out.array_handle);assert(g_handles.empty());
 assert(api->enumerate(PGR_VD_KIND_CATCH,this_tid(),2000,&out)==PGR_VD_OK);assert(loaded_calls==1);fake_gc_free(out.array_handle);assert(g_handles.empty());
 assert(api->enumerate(PGR_VD_KIND_CATCH_LOADED,this_tid(),0,&out)==PGR_VD_SKIP);assert(loaded_calls==1&&g_handles.empty());
 puts("PASS smart loaded discovery: validated overload required, includeInactive=true, normal discovery unchanged, lease and roots");
}

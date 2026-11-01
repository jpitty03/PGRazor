// Standalone fixture includes private state; compile this TU alone with -ldl.
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#include "incense_guard.cpp"
namespace {
std::map<uintptr_t,void*> test_roots;
uintptr_t test_next=1;
std::string test_config="1|123|0|0|0|0|0|1";
std::vector<std::string> test_events;
bool test_fresh=true;
unsigned test_invocations=0;
int test_object=0,test_class=0,test_alive_method=0;
uintptr_t root_new(void *p,int){uintptr_t h=test_next++;test_roots[h]=p;return h;}
void *root_get(uintptr_t h){return test_roots.at(h);}
void root_free(uintptr_t h){assert(test_roots.erase(h)==1);}
int transport_sample(PgrRuntimeTransportSample *out){*out={};out->snapshot_fresh=test_fresh;out->snapshot_seq=1;return 0;}
int transport_string(uint32_t field,char *out,uint32_t cap,uint32_t *n){assert(field==12&&cap>test_config.size());*n=test_config.size();memcpy(out,test_config.data(),*n);return 0;}
int transport_emit(uint32_t op,const int64_t*,uint32_t n,const PgrRuntimeTransportString *s,uint32_t count){assert(op==19&&!n&&count==2);test_events.emplace_back(s[1].ptr,s[1].len);return 0;}
int bootstrap_snapshot(PgrBootBinding*){return -1;}
int metadata_class(const PgrMetaClassQuery *q,PgrMetaClassResult *out){assert(!strcmp(q->name,"MapPokemon"));*out={};out->klass=&test_class;return 0;}
void *throwing_invoke(void *method,void *object,void **args,void **exception){assert(method==&test_alive_method&&!object&&args&&args[0]==&test_object);++test_invocations;*exception=&test_object;return nullptr;}
PgrRuntimeTransportApi test_transport{};
PgrBootApi test_bootstrap{};
PgrMetaApi test_metadata{};
}
extern "C" const void *pgrazor_runtime_transport_api(uint32_t v){return v==1?&test_transport:nullptr;}
extern "C" const void *pgrazor_runtime_bootstrap_api(uint32_t v){return v==1?&test_bootstrap:nullptr;}
extern "C" const void *pgrazor_runtime_metadata_api(uint32_t v){return v==1?&test_metadata:nullptr;}
int main(){
 test_transport.sample=transport_sample;test_transport.get_string=transport_string;test_transport.emit=transport_emit;
 test_bootstrap.snapshot=bootstrap_snapshot;test_metadata.resolve_class=metadata_class;
 assert(pgr_incense_guard_tick(123,nullptr)); // unresolved detector blocks immediately
 assert(blocked&&active&&token=="123");
 assert(test_events.back().find("\"token\":\"123\"")!=std::string::npos);
 assert(test_events.back().find("\"phase\":\"error\"")!=std::string::npos);
 test_fresh=false;assert(pgr_incense_guard_tick(123,nullptr)); // stale config cannot clear block
 test_fresh=true;test_config="1|124|0|0|0|0|0|1";assert(pgr_incense_guard_tick(123,nullptr));assert(token=="124");
 test_config="1|124|0|0|0|0|0|0";assert(!pgr_incense_guard_tick(123,nullptr));assert(!active&&!blocked);
 // Simulate one detected, rooted candidate. A managed failure after ownership is
 // consumed emits an open error, releases its root and never retries the action.
 boot.runtime.gchandle_new=reinterpret_cast<void*>(root_new);
 boot.runtime.gchandle_get_target=reinterpret_cast<void*>(root_get);
 boot.runtime.gchandle_free=reinterpret_cast<void*>(root_free);
 boot.runtime.runtime_invoke=reinterpret_cast<void*>(throwing_invoke);
 PgrSharedMetadataOutput metadata{};metadata.methods[5]=&test_alive_method;md=&metadata;
 owner=123;active=true;blocked=true;token="125";pending.reset(new Root(&test_object));
 pgr_incense_guard_open(999);assert(pending&&test_invocations==0); // wrong owner cannot consume
 pgr_incense_guard_open(123);assert(!pending&&test_invocations==1&&test_roots.empty()&&blocked);
 assert(test_events.back().find("\"kind\":\"incense-open\"")!=std::string::npos);
 assert(test_events.back().find("\"token\":\"125\"")!=std::string::npos);
 assert(test_events.back().find("\"phase\":\"error\"")!=std::string::npos);
 pgr_incense_guard_open(123);assert(test_invocations==1); // unknown failure is not replayed
 pgr_incense_guard_stop(123);assert(test_roots.empty());
 std::puts("PASS incense_guard runtime: sticky block, token, stale config, owner, no replay");
}

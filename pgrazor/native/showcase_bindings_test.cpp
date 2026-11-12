#include <time.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "unity_scheduler.h"
namespace fixture {
uint64_t now=100,generation=7;int tid=71,owner=71,calls=0,fail_class=0;
bool installed=true,poisoned=false,attached=true,bad_field=false,bad_method=false,expire_method=false;
uint64_t module_size=0x10000000;
int tokens[10];
void dummy(){}
void* thread(){return attached?&tokens[9]:nullptr;}
int snap(PgrUnitySchedulerSnapshot* s){*s={generation,1,owner,uint32_t(installed),uint32_t(poisoned),0};return 0;}
int valid(uint64_t g){return g&&g==generation?1:0;}
int boot(PgrBootBinding* b){
 *b={};b->module_base=reinterpret_cast<void*>(0x100000);b->module_size=module_size;
 b->runtime.thread_current=reinterpret_cast<void*>(thread);
 b->runtime.gchandle_new=b->runtime.gchandle_get_target=b->runtime.gchandle_free=
 b->runtime.object_get_class=b->runtime.runtime_invoke=b->runtime.object_unbox=
 b->runtime.field_get_value=reinterpret_cast<void*>(dummy);return 0;
}
int cls(const PgrMetaClassQuery* q,PgrMetaClassResult* r){
 ++calls;assert(q->version==1&&q->size==sizeof(*q));
 if(calls==fail_class)return -1;
 const char* names[]={"ContestService","PokemonProto","ContestProto","Single"};
 const char* spaces[]={"Niantic.Holoholo.Contests","Holoholo.Rpc","Holoholo.Rpc","System"};
 assert(calls>=1&&calls<=4);assert(!std::strcmp(q->name,names[calls-1]));
 assert(!std::strcmp(q->name_space,spaces[calls-1]));r->klass=&tokens[calls-1];return 0;
}
int field(const PgrMetaFieldQuery* q,PgrMetaFieldResult* r){
 assert(q->owner==&tokens[1]&&!std::strcmp(q->name,"id_"));
 assert(q->offset==32&&q->value_size==8&&q->forbidden_mask==0x50);
 assert(q->kind==PGR_MD_FIELD_VALUE&&!std::strcmp(q->type_name,"System.UInt64"));
 r->field=&tokens[4];return bad_field?-1:0;
}
int method(const PgrMetaMethodQuery* q,PgrMetaMethodResult* r){
 assert(q->owner==&tokens[0]&&!std::strcmp(q->name,"bnen"));
 assert(q->param_count==2&&!q->is_static&&!q->allow_inflated);
 assert(!std::strcmp(q->return_type,"System.Single"));
 assert(!std::strcmp(q->param_types[0],"Holoholo.Rpc.PokemonProto"));
 assert(!std::strcmp(q->param_types[1],"Holoholo.Rpc.ContestProto"));
 assert(q->expected_code==reinterpret_cast<void*>(0x100000+0xA8FAF60));
 assert(q->prologue&&q->prologue_len==24);
 r->method=&tokens[5];if(expire_method)now=2600;return bad_method?-1:0;
}
}
extern "C" const void* pgrazor_runtime_metadata_api(uint32_t v){
 static PgrMetaApi api{};assert(v==1);api.resolve_class=fixture::cls;
 api.validate_field=fixture::field;api.validate_method=fixture::method;return &api;
}
extern "C" const void* pgrazor_runtime_bootstrap_api(uint32_t v){
 static PgrBootApi api{};assert(v==1);api.snapshot=fixture::boot;return &api;
}
extern "C" const void* pgrazor_unity_scheduler_api(uint32_t v){
 static PgrUnitySchedulerApi api{};assert(v==2);api.snapshot=fixture::snap;api.valid=fixture::valid;return &api;
}
static int fixture_tid(){return fixture::tid;}
static int fixture_clock(int,timespec* t){t->tv_sec=fixture::now/1000;t->tv_nsec=(fixture::now%1000)*1000000;return 0;}
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#define gettid fixture_tid
#define clock_gettime fixture_clock
// Actual production resolver compiled with only OS clock/TID substituted.
#include "showcase_bindings.cpp"
#undef gettid
#undef clock_gettime
void test_showcase_bindings(){
 using namespace fixture;using namespace pgr_showcase;
 ScoreRequest request{71,2600,123,1,2,3,7};ScoreBindings output;
 auto reset=[](){now=100;generation=7;tid=owner=71;calls=fail_class=0;installed=attached=true;
   poisoned=bad_field=bad_method=expire_method=false;module_size=0x10000000;};
 auto reject=[&](){output.score_method=&tokens[5];assert(!resolve_live_score_bindings(request,output));assert(!output.score_method&&!output.root);};
 reset();assert(resolve_live_score_bindings(request,output));assert(output.score_method==&tokens[5]);
 assert(output.owner_valid(71,7));generation=8;assert(!output.owner_valid(71,7));
  generation=7;attached=false;assert(!output.owner_valid(71,7));
 reset();tid=72;reject();assert(!calls);
 reset();generation=8;reject();assert(!calls);
 reset();installed=false;reject();assert(!calls);
 reset();poisoned=true;reject();assert(!calls);
 reset();attached=false;reject();assert(!calls);
 for(int i=1;i<=4;++i){reset();fail_class=i;reject();}
 reset();bad_field=true;reject();
 reset();bad_method=true;reject();
 reset();expire_method=true;reject();
 reset();module_size=0x1000;reject();
 std::puts("PASS Showcase live binding resolver: exact contracts, class/field/method failures, owner generation, expiry, module bounds");
}

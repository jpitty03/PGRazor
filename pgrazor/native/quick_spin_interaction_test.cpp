// Link unchanged quick_spin.cpp + hook_engine.cpp + real Dobby static library.
#define main inactive_fixture_main
#include "quick_spin_test.cpp"
#undef main
#include "interaction_observer.h"
namespace {
bool observing=false;unsigned entries=0,leaves=0,aborts=0,last_id=0;
void *from=nullptr,*to=nullptr,*arg_from=nullptr,*arg_to=nullptr;
int wants(uint32_t id){return observing&&(id==6||id==7||id==13);}
uint64_t enter(uint32_t id,void *self,void *arg){
    assert(self==from&&g_live.size()==(arg?2u:1u));if(arg)assert(arg==arg_from);last_id=id;++entries;
    for(auto &entry:g_live){if(entry.second==from)entry.second=to;else if(entry.second==arg_from)entry.second=arg_to;}
    return entries;
}
void leave(uint64_t token,void *result){assert(token&&!result&&!g_live.empty());++leaves;}
void abort_call(uint64_t token){assert(token&&!g_live.empty());++aborts;}
const pgr_interaction_owner::Api observer{2,40,wants,enter,leave,abort_call};
}
extern "C" const void *pgrazor_interaction_host_api(uint32_t v){return v==2?&observer:nullptr;}
int main(){
    auto api=static_cast<const PgrQuickSpinApi*>(pgrazor_quick_spin_api(1));auto cfg=make_config();assert(api->install(&cfg)==0);
    FakeState original{&g_cls_state,nullptr,nullptr},moved=original;FakeResult result{&g_cls_result,1},result_moved=result;
    from=&original;to=&moved;arg_from=&result;arg_to=&result_moved;
    call_enter(&original,nullptr);assert(entries==0&&g_live.empty()&&g_enter_log.last_self==&original);
    observing=true;call_enter(&original,nullptr);assert(last_id==6&&leaves==1&&g_enter_log.last_self==&moved&&g_live.empty());
    call_exit(&original,nullptr);assert(last_id==7&&leaves==2&&g_exit_log.last_self==&moved&&g_live.empty());
    call_result(&original,&result,nullptr);assert(last_id==13&&leaves==3&&g_result_log.last_self==&moved&&g_last_result_arg==&result_moved&&g_live.empty());
    g_throw_observed=true;
    try{call_enter(&original,nullptr);assert(false);}catch(int e){assert(e==711);}
    try{call_exit(&original,nullptr);assert(false);}catch(int e){assert(e==711);}
    try{call_result(&original,&result,nullptr);assert(false);}catch(int e){assert(e==711);}
    assert(aborts==3&&leaves==3&&g_live.empty());
    puts("PASS quick_spin interaction owner: real Dobby, inactive, rooted refs, paired unwind");
}

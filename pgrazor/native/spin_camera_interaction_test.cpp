// Link unchanged spin_camera.cpp + hook_engine.cpp + real Dobby static library.
// Only feature69's native observer API is substituted; no fake hook engine.
#define main inactive_fixture_main
#include "spin_camera_test.cpp"
#undef main
#include "interaction_observer.h"
namespace {
bool observing=false;unsigned entries=0,leaves=0,aborts=0,last_id=0;
void *from=nullptr,*to=nullptr;
int wants(uint32_t id){return observing&&(id==8||id==9);}
uint64_t enter(uint32_t id,void *self,void *arg){
    assert(!arg&&self==from&&!g_live_handles.empty());last_id=id;++entries;
    for(auto &entry:g_live_handles)if(entry.second==from)entry.second=to;
    return entries;
}
void leave(uint64_t token,void *result){assert(token&&!result&&!g_live_handles.empty());++leaves;}
void abort_call(uint64_t token){assert(token&&!g_live_handles.empty());++aborts;}
const pgr_interaction_owner::Api observer{2,40,wants,enter,leave,abort_call};
}
extern "C" const void *pgrazor_interaction_host_api(uint32_t v){return v==2?&observer:nullptr;}
int main(){
    auto api=static_cast<const PgrSpinCameraApi*>(pgrazor_spin_camera_api(1));auto cfg=make_valid_config();assert(api->install(&cfg)==0);
    FakeObject original{},moved{};original.klass=moved.klass=&g_sentinel_out_class;from=&original;to=&moved;
    call_out_enter(&original,nullptr);assert(entries==0&&g_live_handles.empty()&&g_log_out_enter.last_self==&original);
    observing=true;call_out_enter(&original,nullptr);assert(last_id==8&&leaves==1&&g_log_out_enter.last_self==&moved&&g_live_handles.empty());
    call_out_exit(&original,nullptr);assert(last_id==9&&leaves==2&&g_log_out_exit.last_self==&moved&&g_live_handles.empty());
    g_throw_out=true;try{call_out_enter(&original,nullptr);assert(false);}catch(int e){assert(e==710);}
    assert(aborts==1&&leaves==2&&g_live_handles.empty());
    try{call_out_exit(&original,nullptr);assert(false);}catch(int e){assert(e==710);}
    assert(aborts==2&&leaves==2&&g_live_handles.empty());
    puts("PASS spin_camera interaction owner: real Dobby, inactive, rooted relocation, paired unwind");
}

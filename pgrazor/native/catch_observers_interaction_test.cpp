// Link unchanged catch_observers.cpp + hook_engine.cpp + real Dobby static library.
#define main inactive_fixture_main
#include "catch_observers_test.cpp"
#undef main
#include "interaction_observer.h"
#include <cassert>
namespace {
bool observing=false;unsigned entries=0,leaves=0,aborts=0,last_id=0;
void *from=nullptr,*to=nullptr,*arg_from=nullptr,*arg_to=nullptr;
int wants(uint32_t id){return observing&&(id==10||id==11||id==19);}
uint64_t enter(uint32_t id,void *self,void *arg){
    assert(self==from&&roots==(arg?2:1));if(arg)assert(arg==arg_from);last_id=id;++entries;
    for(int i=0;i<pooln;++i){if(pool[i].cur==from)pool[i].cur=to;else if(pool[i].cur==arg_from)pool[i].cur=arg_to;}
    return entries;
}
void leave(uint64_t token,void *result){
    assert(token&&roots>0);if(result){assert(last_id==19&&result==arg_from&&roots==2);
        for(int i=0;i<pooln;++i)if(pool[i].cur==arg_from)pool[i].cur=arg_to;}
    ++leaves;
}
void abort_call(uint64_t token){assert(token&&roots>0);++aborts;}
const pgr_interaction_owner::Api observer{2,40,wants,enter,leave,abort_call};
}
extern "C" const void *pgrazor_interaction_host_api(uint32_t v){return v==2?&observer:nullptr;}
int main(){
    auto api=static_cast<const PgrObsApi*>(pgrazor_catch_observers_api(1));
    PgrObsConfig cfg{};cfg.version=1;cfg.size=sizeof(cfg);
    cfg.object_get_class=(void*)&obj_class;cfg.field_get_value=(void*)&field_get;
    cfg.gchandle_new=(void*)&gc_new;cfg.gchandle_get_target=(void*)&gc_target;cfg.gchandle_free=(void*)&gc_free;
    cfg.code_btxu=(void*)&btxu_stub;cfg.code_caught=(void*)&caught_stub;cfg.code_escaped=(void*)&escaped_stub;
    cfg.class_ball=&ball_class;cfg.class_response=&response_class;cfg.class_animator=&animator_class;
    cfg.field_bread=&fld_bread;cfg.field_animator=&fld_animator;cfg.field_status=&fld_status;cfg.speed_api=&stub_speed;
    cfg.status_success=S_SUCCESS;cfg.status_escape=S_ESCAPE;cfg.status_flee=S_FLEE;cfg.status_missed=S_MISSED;
    cfg.status_error=S_ERROR;cfg.status_item_replacement=S_ITEM;cfg.thread_current=(void*)&fake_thread_current;
    assert(api->install(&cfg)==0);call_btxu=&btxu_stub;volatile CompPtr caught=&caught_stub,escaped=&escaped_stub;
    Ball moved_ball=g_ball;Response moved_result=g_resp_n;int moved_capture=0;
    from=&g_ball;to=&moved_ball;arg_from=&g_resp_n;arg_to=&moved_result;g_next_response=&g_resp_n;
    assert(call_btxu(&g_ball,nullptr)==&g_resp_n&&entries==0&&roots==0);
    observing=true;assert(call_btxu(&g_ball,nullptr)==&moved_result&&last_id==19&&leaves==1&&last_btxu_self==&moved_ball&&roots==0);
    from=&capture_obj;to=&moved_capture;caught(&capture_obj,&g_resp_n,nullptr);
    assert(last_id==10&&leaves==2&&last_caught_self==&moved_capture&&last_caught_resp==&moved_result&&roots==0);
    escaped(&capture_obj,&g_resp_n,nullptr);assert(last_id==11&&leaves==3&&last_escaped_resp==&moved_result&&roots==0);
    g_throw_observed=true;
    try{caught(&capture_obj,&g_resp_n,nullptr);assert(false);}catch(int e){assert(e==749);}
    try{escaped(&capture_obj,&g_resp_n,nullptr);assert(false);}catch(int e){assert(e==749);}
    from=&g_ball;to=&moved_ball;try{call_btxu(&g_ball,nullptr);assert(false);}catch(int e){assert(e==749);}
    assert(aborts==3&&leaves==3&&roots==0);
    puts("PASS catch observers interaction owner: real Dobby, inactive, relocated refs/result, paired unwind");
}

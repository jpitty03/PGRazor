// Reuse the actual feature23 managed-heap fixture, not a replacement native API.
#define main transfer_execution_regression_main
#include "transfer_execution_test.cpp"
#undef main
#include "transfer_observers.h"
namespace {
using ObservedCallback=void(*)(void *,void *,const void *);
int callback_calls=0,method_marker;
void *expected_self=nullptr,*expected_value=nullptr;
bool throw_original=false;
__attribute__((noinline)) void observed_result(void *self,void *value,const void *method){
 volatile unsigned pad=17;for(unsigned i=0;i<16;i++)pad=pad*33+i;
 assert(self==expected_self&&value==expected_value&&method==&method_marker);
 ++callback_calls;
 if(throw_original)throw 73;
}
__attribute__((noinline)) void observed_error(void *self,void *value,const void *method){
 volatile unsigned pad=29;for(unsigned i=0;i<16;i++)pad=pad*17+i;
 assert(self==expected_self&&value==expected_value&&method==&method_marker);
 ++callback_calls;
}
void *attached(){return &method_marker;}
void deliver(bool error,Scene &s){
 expected_self=&s.cb;expected_value=error?static_cast<void *>(&method_marker):static_cast<void *>(&s.ro);
 ObservedCallback volatile call=error?observed_error:observed_result;
 call(expected_self,expected_value,&method_marker);
}
}
int main(int argc,char **argv){
 assert(argc==2);
 void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
 auto getter=reinterpret_cast<GetApiFn>(dlsym(lib,"pgrazor_native_api"));assert(getter);
 auto *core=static_cast<const pgrazor_native_api_v2 *>(getter(2));assert(core);
 g_api=static_cast<const PgrTransferExecApi *>(core->query_feature(23,1));assert(g_api);
 auto *observers=static_cast<const PgrTransferObserversApi *>(core->query_feature(57,1));assert(observers);
 auto config=make_config();assert(g_api->install(&config)==0);g_owner=this_tid();
 PgrTransferObserversConfig c{1,sizeof(c),reinterpret_cast<void *>(observed_result),reinterpret_cast<void *>(observed_error),
  reinterpret_cast<void *>(attached),reinterpret_cast<void *>(fake_gc_new),reinterpret_cast<void *>(fake_gc_get),reinterpret_cast<void *>(fake_gc_free)};
 assert(observers->install(&c)==0);
 for(unsigned scenario=0;scenario<4;scenario++){
  Scene s;wire(s,next_id());g_promise=&s.prom;
  assert(g_api->update(1,2500,1,g_owner)==0);
  auto request=make_req(s,s.pk.id,base_input(),1,g_owner);uint64_t token=0;
  callback_calls=0;
  if(scenario==2){g_oneshot_method=&m_release;g_oneshot=[&](){deliver(false,s);};}
  assert(g_api->submit(&request,&token)==PGR_TX_ISSUED&&token);
  if(scenario!=2){
   throw_original=scenario==3;
   try{deliver(scenario==1,s);assert(!throw_original);}catch(int value){assert(throw_original&&value==73);}
   throw_original=false;
  }
  assert(callback_calls==1);
  const auto events=drain_all();assert(saw(events,PGR_TX_EVENT_REQUEST));
  if(scenario==3)assert(!saw(events,PGR_TX_EVENT_COMPLETE));
  else assert(saw_complete(events,scenario!=1));
  assert(take_sample().pending==0&&take_sample().open_tickets==0&&g_live.empty());
 }
 puts("PASS actual57 Dobby + actual23 lifecycle: result/error, synchronous release callback, thrown original propagated without COMPLETE, root cleanup");
 return 0;
}

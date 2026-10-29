#include "transfer_observers.h"
#include "transfer_execution.h"
#include "hook_engine.h"
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
namespace {
using Callback=void(*)(void *,void *,const void *);
using New=uintptr_t(*)(void *,int);
using Get=void *(*)(uintptr_t);
using Free=void(*)(uintptr_t);
struct Pins {
 Get get; Free free; uintptr_t self=0,value=0;
 ~Pins(){if(value)free(value);if(self)free(self);}
};
std::mutex lock;
PgrTransferObserversConfig cfg{};
std::atomic<Callback> originals[2];
std::atomic<bool> ready{false};
bool failed=false;
const PgrTransferExecApi *execution=nullptr;
Callback original(unsigned which){
 auto fn=originals[which].load(std::memory_order_acquire);
 if(!fn){std::lock_guard<std::mutex> guard(lock);fn=originals[which].load(std::memory_order_acquire);}
 if(!fn)std::abort();
 return fn;
}
// Keep arguments pinned until the original returns. Feature23 may allocate
// temporary roots while matching. Refresh the original arguments from these
// roots after matching; never retain a borrowed response beyond this call.
void dispatch(unsigned which,void *self,void *value,const void *method){
 auto fn=original(which);
 if(!ready.load(std::memory_order_acquire)||!reinterpret_cast<void *(*)()>(cfg.thread_current)()){
  fn(self,value,method);return;
 }
 const auto make=reinterpret_cast<New>(cfg.gchandle_new);
 const auto get=reinterpret_cast<Get>(cfg.gchandle_get_target);
 const auto free=reinterpret_cast<Free>(cfg.gchandle_free);
 const uint64_t tid=static_cast<uint64_t>(gettid());
 Pins pins{get,free};
 pins.self=self?make(self,1):0;
 pins.value=value?make(value,1):0;
 const bool rooted=(!self||pins.self)&&(!value||pins.value);
 uint64_t ticket=0;
 if(rooted)ticket=execution->observe_begin(pins.self?get(pins.self):nullptr,which?nullptr:(pins.value?get(pins.value):nullptr),which,tid);
 else execution->tick(tid,1); // revoke uncertain transfer bookkeeping, preserve game callback
 try{fn(pins.self?get(pins.self):self,pins.value?get(pins.value):value,method);}
 catch(...){execution->tick(tid,1);throw;} // uncertain callback is never reported COMPLETE
 if(ticket)execution->observe_end(ticket,tid);
}
void result(void *self,void *value,const void *method){dispatch(0,self,value,method);}
void error(void *self,void *value,const void *method){dispatch(1,self,value,method);}
int install(const PgrTransferObserversConfig *c){
 if(!c||c->version!=1||c->size!=sizeof(*c))return -1;
 if(!c->result_address||!c->error_address||c->result_address==c->error_address||
    !c->thread_current||!c->gchandle_new||!c->gchandle_get_target||!c->gchandle_free)return -2;
 std::lock_guard<std::mutex> guard(lock);
 if(failed)return -3;
 if(ready.load())return std::memcmp(c,&cfg,sizeof(cfg))==0?0:-4;
 execution=static_cast<const PgrTransferExecApi *>(pgrazor_transfer_execution_api(1));
 if(!execution||execution->size!=sizeof(PgrTransferExecApi))return -5;
 cfg=*c;
 void *addresses[]={c->result_address,c->error_address};
 Callback replacements[]={result,error};
 for(unsigned i=0;i<2;i++){
  void *trampoline=nullptr;
  const int rc=pgrazor_hook(addresses[i],reinterpret_cast<void *>(replacements[i]),&trampoline);
  if(trampoline)originals[i].store(reinterpret_cast<Callback>(trampoline),std::memory_order_release);
  if(rc||!trampoline){failed=true;return -3;}
 }
 ready.store(true,std::memory_order_release);return 0;
}
int installed(){return ready.load(std::memory_order_acquire)?1:0;}
const PgrTransferObserversApi api{1,sizeof(PgrTransferObserversApi),install,installed};
}
extern "C" const void *pgrazor_transfer_observers_api(uint32_t version){return version==1?&api:nullptr;}

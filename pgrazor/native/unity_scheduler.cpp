#include "unity_scheduler.h"
#include "hook_engine.h"
#include "runtime_metadata.h"
#include <atomic>
#include <mutex>
#include <cstdlib>
#include <limits>
#include <unistd.h>
#include <cstring>

namespace {
std::mutex state_mutex;
PgrUnitySchedulerConfig config{};
std::atomic<PgrUnityInvoke> original_invoke{nullptr};
std::atomic<PgrUnityConvert> original_convert{nullptr};
bool installed=false,poisoned=false;
std::atomic<uint64_t> active{0},sequence{0};
uint64_t next_generation=0;
std::atomic<int32_t> owner{0};
thread_local uint32_t depth=0;
thread_local uint32_t original_depth=0;
thread_local uint64_t original_generation=0;

#if defined(__aarch64__)
// Adjacent four-byte IL2CPP export veneers cannot hold Dobby's12/16-byte patch.
// Follow only validated unconditional B veneers; retain exports in config.
void *invoke_body(void *entry){
 uintptr_t address=reinterpret_cast<uintptr_t>(entry);
 if(!address||(address&3u))return nullptr;
 uint32_t word=0;std::memcpy(&word,entry,sizeof(word)); // verify_metadata proved export executable
 if((word&0xfc000000u)!=0x14000000u)return entry;
 const auto *metadata=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));
 if(!metadata||metadata->version!=1||metadata->size!=sizeof(*metadata)||!metadata->validate_code)return nullptr;
 uintptr_t seen[8]{};
 for(unsigned hop=0;hop<8;++hop){
  for(unsigned prior=0;prior<hop;++prior)if(seen[prior]==address)return nullptr;
  seen[hop]=address;
  PgrMetaCodeQuery check{1,sizeof(check),reinterpret_cast<void*>(address),4,nullptr,0,0};
  if(metadata->validate_code(&check)!=PGR_MD_OK)return nullptr;
  std::memcpy(&word,reinterpret_cast<void*>(address),sizeof(word));
  if((word&0xfc000000u)!=0x14000000u){
   check.length=16;
   return metadata->validate_code(&check)==PGR_MD_OK?reinterpret_cast<void*>(address):nullptr;
  }
  int64_t displacement=static_cast<int64_t>(word&0x03ffffffu);
  if(displacement&0x02000000LL)displacement-=0x04000000LL;
  displacement*=4;
  if(!displacement)return nullptr;
  if(displacement<0){const auto delta=static_cast<uintptr_t>(-displacement);if(address<delta)return nullptr;address-=delta;}
  else{const auto delta=static_cast<uintptr_t>(displacement);if(address>std::numeric_limits<uintptr_t>::max()-delta)return nullptr;address+=delta;}
  if(address&3u)return nullptr;
 }
 return nullptr;
}
#endif

bool same(const PgrUnitySchedulerConfig &a,const PgrUnitySchedulerConfig &b){
 if(a.version!=b.version||a.size!=b.size||a.runtime_invoke!=b.runtime_invoke||
 a.runtime_invoke_convert_args!=b.runtime_invoke_convert_args||a.execute_tasks!=b.execute_tasks||
 a.thread_current!=b.thread_current||a.verify_metadata!=b.verify_metadata||
 a.expected_owner_tid!=b.expected_owner_tid||a.exclusive_owner!=b.exclusive_owner||
 a.subscriber_count!=b.subscriber_count||a.reserved!=b.reserved)return false;
 for(uint32_t i=0;i<PGR_US_MAX_SUBSCRIBERS;++i){
 const auto &x=a.subscribers[i];const auto &y=b.subscribers[i];
 if(x.begin!=y.begin||x.end!=y.end||x.context!=y.context)return false;
 }return true;
}
int valid(uint64_t generation){return generation&&active.load(std::memory_order_acquire)==generation;}
// Called only after a successful void original. No borrowed managed result or
// exception is exposed across subscriber safepoints. An unused args
// array is accepted for verified zero-parameter methods. Noncanonical
// invocations are forwarded untouched and never treated as scheduler pulses.
void pulse(const void *method,void *object,void **,int32_t count,uint64_t entered){
 if(depth!=1||method!=config.execute_tasks||object||count!=0)return;
 const int32_t tid=static_cast<int32_t>(gettid());
 if(tid!=config.expected_owner_tid||!config.thread_current())return;
 PgrUnityPulse frame{};
 {
  // Serialize the diagnostic tuple, never a managed call or subscriber.
  std::lock_guard<std::mutex> guard(state_mutex);
  const uint64_t generation=active.load(std::memory_order_acquire);
  if(!generation||generation!=entered)return;
  if(!owner.load())owner.store(tid);
  if(owner.load()!=tid)return;
  const auto previous=sequence.load();
  if(previous==std::numeric_limits<uint64_t>::max()){
   poisoned=true;active.store(0,std::memory_order_release);return;
  }
  sequence.store(previous+1);
  frame={generation,previous+1,tid,0};
 }
 const uint64_t generation=frame.generation;
 for(uint32_t i=0;i<config.subscriber_count&&valid(generation);++i){
 const auto &s=config.subscribers[i];if(s.begin)s.begin(s.context,&frame);
 }
 for(uint32_t i=0;i<config.subscriber_count&&valid(generation);++i){
 const auto &s=config.subscribers[i];if(s.end)s.end(s.context,&frame);
 }
}
struct Depth {
 bool matched;
 Depth(const void *method,void *object,int32_t count):matched(method==config.execute_tasks&&!object&&!count){if(matched)++depth;}
 ~Depth(){if(matched)--depth;}
};
struct OriginalFrame {
 bool matched;uint64_t previous;
 explicit OriginalFrame(bool match):matched(match),previous(original_generation){
  if(matched){if(!original_depth)original_generation=active.load(std::memory_order_acquire);++original_depth;}
 }
 ~OriginalFrame(){if(matched){--original_depth;original_generation=previous;}}
};
uint64_t executing_generation(uint64_t supplied_tid){
 if(!original_depth||!original_generation||!supplied_tid||supplied_tid!=uint64_t(gettid())||
    supplied_tid!=uint64_t(config.expected_owner_tid)||!valid(original_generation)||
    !config.thread_current||!config.thread_current())return 0;
 return original_generation;
}
void *invoke(const void *method,void *object,void **args,void **exception){
 PgrUnityInvoke original=original_invoke.load(std::memory_order_acquire);
 if(!original){std::lock_guard<std::mutex> lock(state_mutex);original=original_invoke.load(std::memory_order_acquire);}
 if(!original)std::abort();
 Depth frame(method,object,0);
 const auto entered=active.load(std::memory_order_acquire);
 void *result;
 {OriginalFrame running(frame.matched);result=original(method,object,args,exception);}
 if(!result&&exception&&!*exception)pulse(method,object,args,0,entered);
 return result;
}
void *convert(const void *method,void *object,void **args,int32_t count,void **exception){
 PgrUnityConvert original=original_convert.load(std::memory_order_acquire);
 if(!original){std::lock_guard<std::mutex> lock(state_mutex);original=original_convert.load(std::memory_order_acquire);}
 if(!original)std::abort();
 Depth frame(method,object,count);
 const auto entered=active.load(std::memory_order_acquire);
 void *result;
 {OriginalFrame running(frame.matched);result=original(method,object,args,count,exception);}
 if(!result&&exception&&!*exception)pulse(method,object,args,count,entered);
 return result;
}
int install(const PgrUnitySchedulerConfig *value){
 if(!value||value->version!=2||value->size!=sizeof(*value)||
 !value->runtime_invoke||!value->runtime_invoke_convert_args||
 value->runtime_invoke==value->runtime_invoke_convert_args||!value->execute_tasks||
 !value->thread_current||!value->verify_metadata||value->expected_owner_tid<=0||
 value->exclusive_owner!=1||value->reserved||value->subscriber_count>PGR_US_MAX_SUBSCRIBERS)return PGR_US_BAD_CONFIG;
 for(uint32_t i=0;i<PGR_US_MAX_SUBSCRIBERS;++i){const auto &s=value->subscribers[i];
 if(i<value->subscriber_count){if(!s.begin&&!s.end)return PGR_US_BAD_CONFIG;}
 else if(s.begin||s.end||s.context)return PGR_US_BAD_CONFIG;
 }
 std::lock_guard<std::mutex> lock(state_mutex);
 if(poisoned)return PGR_US_POISONED;
 if(installed)return same(config,*value)?PGR_US_OK:PGR_US_REBIND;
 if(value->verify_metadata(value)!=1)return PGR_US_BAD_CONFIG;
 void *invoke_target=value->runtime_invoke,*convert_target=value->runtime_invoke_convert_args;
#if defined(__aarch64__)
 invoke_target=invoke_body(invoke_target);convert_target=invoke_body(convert_target);
 if(!invoke_target||!convert_target)return PGR_US_BAD_CONFIG;
 const auto a=reinterpret_cast<uintptr_t>(invoke_target),b=reinterpret_cast<uintptr_t>(convert_target);
 if((a>b?a-b:b-a)<16)return PGR_US_BAD_CONFIG;
#endif
 config=*value;
 void *first=nullptr;
 int rc=pgrazor_hook(invoke_target,reinterpret_cast<void *>(&invoke),&first);
 original_invoke.store(reinterpret_cast<PgrUnityInvoke>(first),std::memory_order_release);
 if(rc||!first){
 // Retain every published trampoline: an in-flight replacement may still need it.
 poisoned=true;return PGR_US_HOOK_FAILED;
 }
 void *second=nullptr;
 rc=pgrazor_hook(convert_target,reinterpret_cast<void *>(&convert),&second);
 original_convert.store(reinterpret_cast<PgrUnityConvert>(second),std::memory_order_release);
 if(rc||!second){
 // Remain passive and retain both originals until process exit. Never destroy
 // a trampoline that an already-entered replacement may still be forwarding to.
 poisoned=true;
 return PGR_US_HOOK_FAILED;
 }
 installed=true;return PGR_US_OK;
}
uint64_t activate(){
 std::lock_guard<std::mutex> lock(state_mutex);
 if(!installed||poisoned||next_generation==std::numeric_limits<uint64_t>::max())return 0;
 const uint64_t value=++next_generation;active.store(value,std::memory_order_release);return value;
}
int revoke(uint64_t value){
 if(!value)return PGR_US_STALE;
 std::lock_guard<std::mutex> guard(state_mutex);
 return active.compare_exchange_strong(value,0,std::memory_order_acq_rel)?PGR_US_OK:PGR_US_STALE;
}
int snapshot(PgrUnitySchedulerSnapshot *out){
 if(!out)return PGR_US_BAD_CONFIG;
 std::lock_guard<std::mutex> lock(state_mutex);
 *out={active.load(),sequence.load(),owner.load(),uint32_t(installed),uint32_t(poisoned),0};return PGR_US_OK;
}
const PgrUnitySchedulerApi api{2,sizeof(PgrUnitySchedulerApi),install,activate,revoke,valid,snapshot};
const PgrUnitySchedulerApi3 api3{3,sizeof(PgrUnitySchedulerApi3),install,activate,revoke,valid,snapshot,executing_generation};
}
extern "C" const void *pgrazor_unity_scheduler_api(uint32_t version){
 if(version==2)return &api;if(version==3)return &api3;return nullptr;
}

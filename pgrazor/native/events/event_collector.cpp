#include "event_collector.h"
#include <cmath>
#include "../runtime_bootstrap.h"
#include "../runtime_metadata.h"
#include "../hook_engine.h"
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits>
#include <limits.h>
#include <errno.h>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace {
using Tap=void(*)(void*,void*,uint8_t,const void*);
using Result=void(*)(void*,void*,void*,const void*);
using Update=void(*)(void*,const void*);
using Current=void*(*)();
std::mutex install_mutex;
std::atomic<Tap> original_tap{nullptr};
std::atomic<Result> original_result{nullptr},original_encounter{nullptr};
std::atomic<Update> original_update{nullptr};
std::atomic<bool> ready{false},stopped{false};
std::atomic<uint64_t> owner{0};
PgrBootBinding binding{};
const PgrMetaApi *metadata=nullptr;
const void *controller_class=nullptr,*response_class=nullptr,*string_class=nullptr;
const void *loot_class=nullptr,*loot_item_class=nullptr,*species_class=nullptr,*item_class=nullptr,*int32_class=nullptr;
struct RepeatedLayout {const void *klass=nullptr,*array_class=nullptr;};
RepeatedLayout reward_layout{},items_layout{};
int memory_pipe[2]={-1,-1};
std::mutex memory_mutex;
bool memory_healthy=false;
constexpr size_t kCopyMax=256;
static_assert(kCopyMax<=PIPE_BUF,"Fault-safe copies must fit an atomic pipe write");
bool initialize_memory_copy(){
  std::lock_guard<std::mutex> lock(memory_mutex);
  if(memory_pipe[0]>=0)return memory_healthy;
  if(pipe2(memory_pipe,O_CLOEXEC|O_NONBLOCK)!=0)return false;
  memory_healthy=true;return true;
}
// Private bounded pipe; drain every failed transaction before accepting another copy.
// Any unexpected descriptor failure poisons future reads instead of mixing records.
bool drain_memory_pipe(){
  unsigned char discard[kCopyMax];
  for(unsigned i=0;i<32;++i){
    ssize_t n=read(memory_pipe[0],discard,sizeof(discard));
    if(n>0)continue;
    if(n<0&&errno==EINTR)continue;
    if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return true;
    memory_healthy=false;return false;
  }
  memory_healthy=false;return false;
}
int state=1;
struct Record {
  unsigned reward_count=0,item_count=0,other_count=0;
  uint64_t candy=0,xl_candy=0,rare_candy=0,xl_rare_candy=0,loot_type_mask=0;
};
bool read_bytes(const void *object,size_t offset,void *out,size_t bytes) {
  uintptr_t base=reinterpret_cast<uintptr_t>(object);
  if(!base||!out||!bytes||bytes>kCopyMax||offset>UINTPTR_MAX-base||bytes>UINTPTR_MAX-(base+offset))return false;
  const uintptr_t at=base+offset;
  std::lock_guard<std::mutex> lock(memory_mutex);
  if(!memory_healthy||!drain_memory_pipe())return false;
  // The kernel copies from the untrusted address; unreadable source pages return
  // EFAULT through write(), without dereferencing them in this process's code.
  const ssize_t written=write(memory_pipe[1],reinterpret_cast<const void*>(at),bytes);
  if(written!=static_cast<ssize_t>(bytes)){drain_memory_pipe();return false;}
  unsigned char copy[kCopyMax];
  const ssize_t received=read(memory_pipe[0],copy,bytes);
  if(received!=static_cast<ssize_t>(bytes)){drain_memory_pipe();return false;}
  if(!drain_memory_pipe())return false;
  std::memcpy(out,copy,bytes);return true;
}
template<class T>bool read_field(const void *object,size_t offset,T &out){return read_bytes(object,offset,&out,sizeof(out));}
bool exact(const void *object,const void *klass){const void *actual=nullptr;return object&&klass&&read_field(object,0,actual)&&actual==klass;}
void read_key(void *object,size_t offset,char (&out)[97]) {
  void *key=nullptr;if(!read_field(object,offset,key)||!exact(key,string_class)){std::strcpy(out,"<unknown>");return;}
  // Pinned IL2CPP String layout: length +0x10, UTF-16 data +0x14.
  // The private kernel pipe copy rejects unreadable data without signal handlers.
  int32_t n=0;if(!read_field(key,0x10,n)||n<0||n>96){std::strcpy(out,"<oversize>");return;}
  uint16_t chars[96]{};if(n&&!read_bytes(key,0x14,chars,size_t(n)*2)){std::strcpy(out,"<unreadable>");return;}
  for(int32_t i=0;i<n;++i){auto c=chars[i];out[i]=(c>=32&&c<=126&&c!='"'&&c!=92)?char(c):'_';}
}
bool repeated(void *object,const RepeatedLayout &layout,void **array,int32_t *count) {
  if(!exact(object,layout.klass)||!read_field(object,0x18,*count)||*count<0||*count>64)return false;
  *array=nullptr;if(!*count)return true;
  uintptr_t capacity=0;
  return read_field(object,0x10,*array)&&exact(*array,layout.array_class)&&read_field(*array,0x18,capacity)&&capacity>=uint32_t(*count)&&capacity<=4096;
}
bool decode_rewards(void *reward,Record &r) {
  void *array=nullptr;int32_t size=0;if(!repeated(reward,reward_layout,&array,&size))return false;
  r.reward_count=unsigned(size);
  for(int32_t i=0;i<size;++i){
    void *loot=nullptr,*items=nullptr,*item_array=nullptr;int32_t n=0;
    if(!read_field(array,0x20+size_t(i)*8,loot)||!exact(loot,loot_class)||!read_field(loot,0x10,items)||!repeated(items,items_layout,&item_array,&n))return false;
    if(r.item_count+unsigned(n)>128)return false;r.item_count+=unsigned(n);
    for(int32_t j=0;j<n;++j){
      void *item=nullptr,*payload=nullptr;int32_t quantity=0,kind=0,value=0;
      if(!read_field(item_array,0x20+size_t(j)*8,item)||!exact(item,loot_item_class)||!read_field(item,0x10,quantity)||quantity<0||!read_field(item,0x20,kind))return false;
      if(kind<0||kind>=64)return false;r.loot_type_mask|=uint64_t(1)<<unsigned(kind);
      uint64_t *sum=nullptr;
      if(kind==4||kind==11||kind==1){
        // Public setters box the enum; protobuf wire readers can box System.Int32.
        // Accept only those exact validated classes, never arbitrary four-byte values.
        if(!read_field(item,0x18,payload)||
           !(exact(payload,kind==1?item_class:species_class)||exact(payload,int32_class))||
           !read_field(payload,0x10,value))return false;
        if(kind!=1&&value<=0)return false;
        if(kind==4)sum=&r.candy;else if(kind==11)sum=&r.xl_candy;
        else if(value==1301)sum=&r.rare_candy;else if(value==1302)sum=&r.xl_rare_candy;
      }
      if(sum){if(*sum>UINT64_MAX-uint32_t(quantity))return false;*sum+=uint32_t(quantity);}
      else ++r.other_count;
    }
  }
  return true;
}
int resolve(const char *ns,const char *name,const void **out) {
  PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;
  PgrMetaClassResult r{};int rc=metadata->resolve_class(&q,&r);if(!rc)*out=r.klass;return rc;
}
int check_field(const void *klass,const char *name,uint64_t offset,uint32_t kind,uint32_t bytes,const char *type=nullptr,PgrMetaFieldResult *output=nullptr) {
  PgrMetaFieldQuery q{};q.version=1;q.size=sizeof(q);q.owner=klass;q.name=name;q.offset=offset;
  q.forbidden_mask=0x50;q.kind=kind;q.value_size=bytes;q.type_name=type;q.type_match=PGR_MD_MATCH_EXACT;
  PgrMetaFieldResult r{};int rc=metadata->validate_field(&q,&r);if(!rc&&output)*output=r;return rc;
}
int repeated_layout(const void *owner_class,const char *name,uint64_t offset,RepeatedLayout &layout) {
  PgrMetaFieldResult r{},array{};
  int rc=check_field(owner_class,name,offset,PGR_MD_FIELD_REF,0,nullptr,&r);if(rc)return rc;
  layout.klass=r.type_class;if(!layout.klass)return -20;
  if((rc=check_field(layout.klass,"array",0x10,PGR_MD_FIELD_REF,0,nullptr,&array))||
     (rc=check_field(layout.klass,"count",0x18,PGR_MD_FIELD_VALUE,4,"System.Int32")))return rc;
  layout.array_class=array.type_class;return layout.array_class?0:-21;
}
bool constant(const void *klass,const char *name,int32_t expected) {
  using Find=void*(*)(void*,const char*);using Parent=void*(*)(void*);using Flags=uint32_t(*)(void*);using Read=void(*)(void*,void*);
  if(!klass||!reinterpret_cast<bool(*)(void*)>(binding.meta.class_is_enum)(const_cast<void*>(klass)))return false;
  void *f=reinterpret_cast<Find>(binding.meta.class_get_field_from_name)(const_cast<void*>(klass),name);
  if(!f||reinterpret_cast<Parent>(binding.meta.field_get_parent)(f)!=klass||
     (reinterpret_cast<Flags>(binding.meta.field_get_flags)(f)&0x8050)!=0x8050)return false;
  int32_t value=0;reinterpret_cast<Read>(binding.runtime.field_static_get_value)(f,&value);return value==expected;
}
using NewRoot=uintptr_t(*)(void*,int);
using GetRoot=void*(*)(uintptr_t);
using FreeRoot=void(*)(uintptr_t);
using Invoke=void*(*)(const void*,void*,void**,void**);
NewRoot root_new=nullptr;GetRoot root_get=nullptr;FreeRoot root_free=nullptr;Invoke invoke=nullptr;
const void *service_class=nullptr,*session_class=nullptr,*encounter_class=nullptr,*list_class=nullptr,*list_array_class=nullptr;
const void *tap_method=nullptr,*visible_method=nullptr;
uintptr_t session_root=0,controller_root=0,service_root=0;
uint64_t active_id=0,dispatched_ms=0,next_ms=0;
std::atomic<uint64_t> enable_until_ms{0},update_calls{0},owner_updates{0},last_update_tid{0};
bool dispatching=false,ambiguous=false;
unsigned result_depth=0;
struct Scope {uintptr_t controller=0,encounter=0;uint64_t id=0;};
thread_local Scope encounter_scope{};
thread_local unsigned natural_result_depth=0;
// Pinned bfvy 0xa4b3380 captures .cay and passes its Action to
// IDynamicTappable.AnimateInteraction at0xa4b342c. Method usage0xec3cef8
// resolves cay.bfvr (0xa4b6d4c), which tailcalls bfwa at0xa4b6d70.
// Therefore response-stack lifetime alone cannot authorize suppression.
struct OwnedEncounter {uintptr_t service=0,controller=0,encounter=0;uint64_t id=0,created=0;bool stale=false;};
// Missing animation callbacks must not hold the dispatch slot forever. Keep
// their exact ownership quarantined separately, with a hard bound on GC roots.
constexpr unsigned kOwnedEncounterLimit=128;
OwnedEncounter owned_encounters[kOwnedEncounterLimit]{};
PgrEventCollectorState counters{};
std::mutex state_mutex,location_mutex;
struct Location {double lat=0,lng=0;uint64_t at=0;} location;
struct Seen {uint64_t id=0,until=0;};Seen seen[128]{};unsigned seen_cursor=0;
uint64_t now_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return uint64_t(t.tv_sec)*1000+uint64_t(t.tv_nsec)/1000000;}
bool owner_thread(){return owner.load()==uint64_t(gettid())&&binding.runtime.thread_current&&reinterpret_cast<Current>(binding.runtime.thread_current)()!=nullptr;}
void release(uintptr_t &h){if(h){root_free(h);h=0;}}
uintptr_t pin(void *p){if(!p||!root_new||!root_get)return 0;uintptr_t h=root_new(p,1);if(h&&root_get(h)!=p){root_free(h);return 0;}return h;}
void remember(uint64_t id,uint64_t until){seen[seen_cursor++%128]={id,until};}
bool seen_before(uint64_t id,uint64_t now){
  // Quarantined IDs must survive rotation/expiry of the ordinary seen cache.
  for(const auto &r:owned_encounters)if(r.id==id)return true;
  for(const auto &s:seen)if(s.id==id&&s.until>now)return true;return false;
}
void clear_attempt(){release(controller_root);release(service_root);active_id=0;dispatched_ms=0;ambiguous=false;}
void clear_owned(OwnedEncounter &r){release(r.service);release(r.controller);release(r.encounter);r={};}
void revoke_owned(void *target,uint64_t id){
  bool revoked=false;for(auto &r:owned_encounters)if(r.id==id&&r.controller&&root_get(r.controller)==target){clear_owned(r);revoked=true;}
  if(revoked&&!active_id){std::lock_guard<std::mutex> l(state_mutex);counters.timed_out=0;}
}
bool retain_owned(void *service,void *target,void *enc,uint64_t id){
  if(!exact(enc,encounter_class))return false;
  for(auto &r:owned_encounters)if(!r.id){
    OwnedEncounter pending{};pending.id=id;pending.created=now_ms();pending.service=pin(service);pending.controller=pin(target);pending.encounter=pin(enc);
    if(!pending.service||!pending.controller||!pending.encounter){clear_owned(pending);return false;}
    r=pending;return true;
  }
  return false;
}
void inspect_owned_age(uint64_t n){
  for(auto &r:owned_encounters)if(r.id&&!r.stale&&n>=r.created&&n-r.created>=30000){
    r.stale=true;
    std::lock_guard<std::mutex> l(state_mutex);counters.timed_out=1;
  }
}

template<class Fn>Fn original(std::atomic<Fn>&s){auto f=s.load(std::memory_order_acquire);if(!f){std::lock_guard<std::mutex> l(install_mutex);f=s.load();}if(!f)std::abort();return f;}
void pump(bool enabled,uint64_t n,bool diagnose=false);
// Capture on normal live Update, including while automation is OFF. Strong pinned
// handle protects the captured object; no managed object is kept as a raw global.
void update_hook(void *self,const void *mi){
  update_calls.fetch_add(1,std::memory_order_relaxed);last_update_tid.store(uint64_t(gettid()),std::memory_order_relaxed);
  original(original_update)(self,mi);
  if(!ready.load()||!owner_thread())return;
  owner_updates.fetch_add(1,std::memory_order_relaxed);
  if(!exact(self,session_class)){std::lock_guard<std::mutex> l(state_mutex);counters.pump_reason=14;return;}
  uint8_t active=0;bool readable=read_field(self,0x98,active);
  {std::lock_guard<std::mutex> l(state_mutex);counters.session_active=readable?int(active):-1;}
  if(!readable||active!=1){std::lock_guard<std::mutex> l(state_mutex);counters.pump_reason=15;return;}
  if(!session_root||root_get(session_root)!=self){
    uintptr_t fresh=pin(self);if(!fresh){std::lock_guard<std::mutex> l(state_mutex);counters.pump_reason=16;return;}release(session_root);session_root=fresh;
  }
  bool enabled=false;{std::lock_guard<std::mutex> l(state_mutex);enabled=counters.enabled;}
  pump(enabled,now_ms(),true);
}
void tap_hook(void *self,void *target,uint8_t flag,const void *mi){
  auto f=original(original_tap);
  if(ready.load()&&owner_thread()&&active_id&&controller_root&&root_get(controller_root)==target){
    uint64_t id=0;
    if(read_field(target,0xd0,id)&&id==active_id){
      if(dispatching&&!service_root){
        if(!exact(self,service_class)){ambiguous=true;return;}
        service_root=pin(self);if(!service_root){ambiguous=true;return;}
      }
      else {ambiguous=true;encounter_scope={};revoke_owned(target,id);} // Explicit manual intent revokes even an active response scope.
    }
  }
  if(ready.load()&&owner_thread()&&!dispatching&&exact(target,controller_class)){
    uint64_t manual_id=0;if(read_field(target,0xd0,manual_id)&&manual_id){remember(manual_id,now_ms()+3600000);revoke_owned(target,manual_id);}
  }
  f(self,target,flag,mi);
}
void encounter_hook(void *self,void *target,void *encounter,const void *mi){
  auto f=original(original_encounter);
  if(ready.load()&&owner_thread()&&!natural_result_depth&&exact(target,controller_class)&&exact(encounter,encounter_class)){
    uint64_t id=0;
    if(read_field(target,0xd0,id)&&id){
      // bfvy schedules cay.bfvr through AnimateInteraction. The exact three
      // rooted objects survive that asynchronous boundary; an ID alone is never
      // authority to drop another encounter, and OFF does not revoke an open RPC.
      for(auto &r:owned_encounters)if(r.id==id&&r.service&&r.controller&&r.encounter&&
         root_get(r.service)==self&&root_get(r.controller)==target&&root_get(r.encounter)==encounter){
        clear_owned(r);std::lock_guard<std::mutex> l(state_mutex);++counters.skipped_encounters;if(!active_id)counters.timed_out=0;return;
      }
    }
  }
  f(self,target,encounter,mi);
}
struct SuppressionFrame {
  Scope saved;uintptr_t response,encounter;bool counted;
  SuppressionFrame(Scope next,uintptr_t r=0,uintptr_t e=0,bool count=false):saved(encounter_scope),response(r),encounter(e),counted(count){encounter_scope=next;if(counted)++result_depth;else ++natural_result_depth;}
  ~SuppressionFrame(){if(counted)--result_depth;else --natural_result_depth;encounter_scope=saved;release(encounter);release(response);}
};
void result_hook(void *self,void *target,void *response,const void *mi){
  auto f=original(original_result);
  bool ours=false,identity=false;uint64_t id=0;
  if(ready.load()&&owner_thread()&&!result_depth&&active_id&&service_root&&controller_root&&
     root_get(service_root)==self&&root_get(controller_root)==target&&exact(target,controller_class)&&
     exact(response,response_class)&&read_field(target,0xd0,id)&&id==active_id)identity=true;
  ours=identity&&!ambiguous;
  if(!ours){
    SuppressionFrame natural({});f(self,target,response,mi);
    if(identity){remember(id,now_ms()+3600000);next_ms=now_ms()+15000;clear_attempt();std::lock_guard<std::mutex> l(state_mutex);counters.inflight=0;counters.timed_out=0;++counters.failed;}
    return;
  }
  // Pin every managed response used across the original callback. Suppression is
  // scoped to this exact response's Encounter object, not an ID-only global gate.
  uintptr_t response_pin=pin(response);void *enc=nullptr;
  bool read_ok=response_pin&&read_field(root_get(response_pin),0x20,enc);
  uintptr_t enc_pin=read_ok&&enc?pin(enc):0;
  if(!read_ok||(enc&&!enc_pin)){
    release(enc_pin);release(response_pin);
    // Exact collector ownership was established above. Allocation/read failure
    // cannot justify forwarding an encounter screen; fail closed for this one
    // callback and halt collection. Never apply this to a natural/manual result.
    remember(id,now_ms()+3600000);clear_attempt();stopped.store(true);enable_until_ms.store(0);
    std::lock_guard<std::mutex> l(state_mutex);counters.error=-49;counters.inflight=0;++counters.failed;return;
  }
  if(enc&&!retain_owned(self,target,root_get(enc_pin),id)){
    release(enc_pin);release(response_pin);remember(id,now_ms()+3600000);clear_attempt();stopped.store(true);enable_until_ms.store(0);
    std::lock_guard<std::mutex> l(state_mutex);counters.error=-50;counters.inflight=0;++counters.failed;return;
  }
  Record rewards{};void *reward=nullptr;int32_t status=0;
  bool complete=read_field(root_get(response_pin),0x10,status)&&read_field(root_get(response_pin),0x18,reward)&&decode_rewards(reward,rewards);
  {
    SuppressionFrame frame({controller_root,enc_pin,id},response_pin,enc_pin,true);
    f(self,target,root_get(response_pin),mi);
  }
  const uint64_t n=now_ms();
  remember(id,n+((status==1)?3600000:60000));next_ms=n+((status==1)?2500:15000);
  {std::lock_guard<std::mutex> l(state_mutex);
   if(status==1){++counters.completed;if(complete){counters.candy+=rewards.candy;counters.xl_candy+=rewards.xl_candy;counters.rare_candy+=rewards.rare_candy;counters.xl_rare_candy+=rewards.xl_rare_candy;}}
   else ++counters.failed;
   counters.inflight=0;counters.timed_out=0;
  }
  clear_attempt();
}
int validate_method(const void *klass,const char *name,const char *ret,std::initializer_list<const char*>args,
                    uint64_t rva,const unsigned char (&prefix)[16],PgrMetaMethodResult &r){
  PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=klass;q.name=name;q.return_type=ret;q.return_match=PGR_MD_MATCH_EXACT;
  q.param_count=uint32_t(args.size());unsigned i=0;for(auto a:args){q.param_types[i]=a;q.param_match[i++]=PGR_MD_MATCH_EXACT;}
  q.expected_code=static_cast<const unsigned char*>(binding.module_base)+rva;q.prologue=prefix;q.prologue_len=16;
  return metadata->validate_method(&q,&r);
}
int validate_export(void *p,uint64_t rva,const unsigned char (&bytes)[16]){
  if(p!=static_cast<const unsigned char*>(binding.module_base)+rva)return -40;
  PgrMetaCodeQuery q{};q.version=1;q.size=sizeof(q);q.address=p;q.length=16;q.prologue=bytes;q.prologue_len=16;return metadata->validate_code(&q);
}
int install(uint64_t tid){
  auto boot=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));if(!boot)return 1;
  binding={};binding.version=1;binding.size=sizeof(binding);int rc=boot->snapshot(&binding);if(rc)return rc>0?1:rc;
  if(!binding.runtime.thread_current||!reinterpret_cast<Current>(binding.runtime.thread_current)())return 1;
  metadata=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));if(!metadata)return -2;
  const void *&service=service_class;
  struct Cls{const char *ns,*name;const void **out;};
  Cls classes[]={{"Niantic.Holoholo.Services.DynamicTappables","DynamicTappablesSession",&session_class},{"Niantic.Holoholo.Services.DynamicTappables","DynamicTappableBehaviorService",&service},{"","DynamicTappableController",&controller_class},{"Holoholo.Rpc","ProcessTappableOutProto",&response_class},{"Holoholo.Rpc","TappableEncounterProto",&encounter_class},{"System","String",&string_class},{"System","Int32",&int32_class},{"Holoholo.Rpc","LootProto",&loot_class},{"Holoholo.Rpc","LootItemProto",&loot_item_class},{"Holoholo.Rpc","HoloPokemonId",&species_class},{"Holoholo.Rpc","Item",&item_class}};
  for(auto &c:classes)if((rc=resolve(c.ns,c.name,c.out)))return rc;
  struct F{const void *k;const char *n;uint64_t off;uint32_t kind,size;const char *type;};
  F fields[]={{session_class,"<IsSessionActive>k__BackingField",0x98,PGR_MD_FIELD_VALUE,1,"System.Boolean"},
   {controller_class,"<EncounterId>k__BackingField",0xd0,PGR_MD_FIELD_VALUE,8,"System.UInt64"},
   {controller_class,"<TypeKey>k__BackingField",0xe0,PGR_MD_FIELD_REF,0,"System.String"},
   {controller_class,"<Location>k__BackingField",0xe8,PGR_MD_FIELD_VALUE,16,"Niantic.LatLng"},
   {controller_class,"<DespawnTimeMs>k__BackingField",0x100,PGR_MD_FIELD_VALUE,8,"System.Int64"},
   {response_class,"Status",0x10,PGR_MD_FIELD_ENUM_I32,4,nullptr},{response_class,"Encounter",0x20,PGR_MD_FIELD_REF,0,"Holoholo.Rpc.TappableEncounterProto"},
   {loot_item_class,"Count",0x10,PGR_MD_FIELD_VALUE,4,"System.Int32"},{loot_item_class,"csrn",0x18,PGR_MD_FIELD_REF,0,"System.Object"},{loot_item_class,"csro",0x20,PGR_MD_FIELD_ENUM_I32,4,nullptr},
   {int32_class,"m_value",0x10,PGR_MD_FIELD_VALUE,4,"System.Int32"},{species_class,"value__",0x10,PGR_MD_FIELD_VALUE,4,"System.Int32"},{item_class,"value__",0x10,PGR_MD_FIELD_VALUE,4,"System.Int32"}};
  for(auto &f:fields)if((rc=check_field(f.k,f.n,f.off,f.kind,f.size,f.type)))return rc;
  PgrMetaFieldResult list{},array{};
  if((rc=check_field(session_class,"visibleTappables",0x58,PGR_MD_FIELD_REF,0,nullptr,&list)))return rc;
  list_class=list.type_class;
  if((rc=check_field(list_class,"_items",0x10,PGR_MD_FIELD_REF,0,nullptr,&array))||
     (rc=check_field(list_class,"_size",0x18,PGR_MD_FIELD_VALUE,4,"System.Int32")))return rc;
  list_array_class=array.type_class;
  if(!list_class||!list_array_class)return -41;
  if((rc=repeated_layout(response_class,"Reward",0x18,reward_layout))||(rc=repeated_layout(loot_class,"LootItem",0x10,items_layout)))return rc;
  PgrMetaFieldResult cases{};if((rc=check_field(loot_item_class,"csro",0x20,PGR_MD_FIELD_ENUM_I32,4,nullptr,&cases)))return rc;
  if(!constant(cases.type_class,"Item",1)||!constant(cases.type_class,"PokemonCandy",4)||!constant(cases.type_class,"XlCandy",11)||!constant(item_class,"RareCandy",1301)||!constant(item_class,"XlRareCandy",1302))return -42;
  const unsigned char new_pre[16]={0xe8,0x03,0x00,0xaa,0x49,0x00,0x80,0x52,0x3f,0x00,0x00,0x72,0x20,0x05,0x89,0x1a};
  const unsigned char get_pre[16]={0x13,0x16,0x00,0x14,0xff,0x03,0x01,0xd1,0xfe,0x5f,0x01,0xa9,0xf6,0x57,0x02,0xa9};
  const unsigned char free_pre[16]={0x01,0x16,0x00,0x14,0x00,0x02,0x80,0x52,0xc0,0x03,0x5f,0xd6,0x00,0x04,0x80,0x52};
  const unsigned char invoke_pre[16]={0xb5,0xe9,0x01,0x14,0xb2,0xde,0x01,0x14,0xa1,0xec,0x01,0x14,0xfe,0x0f,0x1e,0xf8};
  if((rc=validate_export(binding.runtime.gchandle_new,0x4eed760,new_pre))||(rc=validate_export(binding.runtime.gchandle_get_target,0x4eed83c,get_pre))||(rc=validate_export(binding.runtime.gchandle_free,0x4eeda3c,free_pre))||(rc=validate_export(binding.runtime.runtime_invoke,0x4eee1d8,invoke_pre)))return rc;
  root_new=reinterpret_cast<NewRoot>(binding.runtime.gchandle_new);root_get=reinterpret_cast<GetRoot>(binding.runtime.gchandle_get_target);root_free=reinterpret_cast<FreeRoot>(binding.runtime.gchandle_free);invoke=reinterpret_cast<Invoke>(binding.runtime.runtime_invoke);
  if(!initialize_memory_copy())return -43;
  const unsigned char tap_pre[16]={0xfe,0x0f,0x1c,0xf8,0xf8,0x5f,0x01,0xa9,0xf6,0x57,0x02,0xa9,0xf4,0x4f,0x03,0xa9};
  const unsigned char result_pre[16]={0xff,0x83,0x02,0xd1,0xfd,0x7b,0x04,0xa9,0xfc,0x6f,0x05,0xa9,0xfa,0x67,0x06,0xa9};
  const unsigned char encounter_pre[16]={0xff,0x43,0x03,0xd1,0xe8,0x33,0x00,0xfd,0xfd,0x7b,0x07,0xa9,0xfc,0x6f,0x08,0xa9};
  const unsigned char update_pre[16]={0xee,0x0f,0x1a,0xfc,0xed,0xb3,0x00,0x6d,0xeb,0xab,0x01,0x6d,0xe9,0xa3,0x02,0x6d};
  const unsigned char visible_pre[16]={0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,0xe1,0x03,0x1f,0xaa,0xbc,0x89,0xe8,0x94};
  PgrMetaMethodResult tap{},result{},enc{},update{},session_tap{},visible{};
  const char *iface="Niantic.Holoholo.Services.DynamicTappables.IDynamicTappable";
  if((rc=validate_method(service,"ProcessTappableTapped","System.Void",{iface,"System.Boolean"},0xa4b2a98,tap_pre,tap))||
     (rc=validate_method(service,"bfvy","System.Void",{iface,"Holoholo.Rpc.ProcessTappableOutProto"},0xa4b3080,result_pre,result))||
     (rc=validate_method(service,"bfwa","System.Void",{iface,"Holoholo.Rpc.TappableEncounterProto"},0xa4b4ce0,encounter_pre,enc))||
     (rc=validate_method(session_class,"Update","System.Void",{},0xa4bafa8,update_pre,update))||
     (rc=validate_method(session_class,"TapTappable","System.Void",{iface},0xa4b9ed8,tap_pre,session_tap))||
     (rc=validate_method(controller_class,"get_VisibleOnMap","System.Boolean",{},0xa037a00,visible_pre,visible)))return rc;
  tap_method=session_tap.method;visible_method=visible.method;owner.store(tid);
  // All contracts validated before any mutation. Partial install never enables dispatch.
  void *out=nullptr;rc=pgrazor_hook(const_cast<void*>(enc.code),reinterpret_cast<void*>(&encounter_hook),&out);if(out)original_encounter.store(reinterpret_cast<Result>(out));if(rc||!out)return -44;
  out=nullptr;rc=pgrazor_hook(const_cast<void*>(result.code),reinterpret_cast<void*>(&result_hook),&out);if(out)original_result.store(reinterpret_cast<Result>(out));if(rc||!out)return -45;
  out=nullptr;rc=pgrazor_hook(const_cast<void*>(tap.code),reinterpret_cast<void*>(&tap_hook),&out);if(out)original_tap.store(reinterpret_cast<Tap>(out));if(rc||!out)return -46;
  out=nullptr;rc=pgrazor_hook(const_cast<void*>(update.code),reinterpret_cast<void*>(&update_hook),&out);if(out)original_update.store(reinterpret_cast<Update>(out));if(rc||!out)return -47;
  ready.store(true);return 0;
}
void pump(bool enabled,uint64_t n,bool diagnose){
  auto reason=[diagnose](int why){if(diagnose){std::lock_guard<std::mutex> l(state_mutex);counters.pump_reason=why;}};
  inspect_owned_age(n);
  static bool pumping=false;if(pumping)return;
  struct PumpGuard{bool &value;PumpGuard(bool &v):value(v){value=true;}~PumpGuard(){value=false;}} guard(pumping);
  if(active_id){
    if(n>=dispatched_ms&&n-dispatched_ms>=30000){std::lock_guard<std::mutex> l(state_mutex);counters.timed_out=1;}
    // A timeout quarantines the sole request rather than dropping its ownership.
    // This preserves late response suppression and prevents a retry stampede.
    reason(13);return;
  }
  unsigned pending_count=0;bool awaiting_animation=false;
  for(const auto &r:owned_encounters)if(r.id){++pending_count;if(!r.stale)awaiting_animation=true;}
  // Serialize normal animations, but let unrelated boxes progress after a lost
  // callback. Never evict exact ownership just to make space: at the hard bound
  // pause until a callback or explicit manual intent releases a record.
  if(awaiting_animation||pending_count>=kOwnedEncounterLimit){reason(17);return;}
  if(!enabled){reason(1);return;}
  if(stopped.load()){reason(2);return;}
  if(n>enable_until_ms.load()){reason(3);return;}
  if(n<next_ms){reason(4);return;}
  if(!session_root){reason(5);return;}
  Location loc;{std::lock_guard<std::mutex> l(location_mutex);loc=location;}
  if(!loc.at||n<loc.at||n-loc.at>5000){reason(6);return;}
  void *session=root_get(session_root);uint8_t active=0;
  if(!exact(session,session_class)||!read_field(session,0x98,active)||active!=1){release(session_root);reason(7);return;}
  void *list=nullptr,*array=nullptr;int32_t count=0;uintptr_t capacity=0;
  if(!read_field(session,0x58,list)||!exact(list,list_class)||!read_field(list,0x18,count)||count<0||count>128||
     !read_field(list,0x10,array)||!exact(array,list_array_class)||!read_field(array,0x18,capacity)||capacity<uint32_t(count)||capacity>4096){reason(8);return;}
  uintptr_t array_pin=pin(array);if(!array_pin){reason(9);return;}
  uintptr_t best=0;double nearest=1e30,nearest_any=1e30;
  int candidates=0,matched_boxes=0,in_range=0,visible_count=0,expired_count=0,zero_expiry=0;
  if(diagnose){std::lock_guard<std::mutex> l(state_mutex);++counters.scan_calls;}
  timespec wall{};clock_gettime(CLOCK_REALTIME,&wall);int64_t epoch_ms=int64_t(wall.tv_sec)*1000+wall.tv_nsec/1000000;
  for(int32_t i=0;i<count;++i){
    void *candidate=nullptr;uint64_t id=0;int64_t expiry=0;double pos[2]{};char key[97]{};
    if(!read_field(root_get(array_pin),0x20+size_t(i)*8,candidate)||!exact(candidate,controller_class))continue;
    ++candidates;
    uintptr_t candidate_pin=pin(candidate);if(!candidate_pin)continue;
    candidate=root_get(candidate_pin);
    read_key(candidate,0xe0,key);bool box_key=std::strcmp(key,"TAPPABLE_TYPE_BOX")==0;if(box_key)++matched_boxes;
    // Refresh can populate this animation/despawn timestamp, or leave it zero.
    // It is diagnostic, not an extra eligibility deadline: live visible boxes
    // with elapsed values still open manually. Match TapTappable's active-list
    // path and the game's VisibleOnMap getter; let the server validate the tap.
    bool expiry_ok=read_field(candidate,0x100,expiry);
    if(expiry_ok&&expiry==0)++zero_expiry;
    if(expiry_ok&&expiry!=0&&expiry<=epoch_ms)++expired_count;
    bool valid=read_field(candidate,0xd0,id)&&id&&!seen_before(id,n)&&expiry_ok&&expiry>=0&&
       read_bytes(candidate,0xe8,pos,sizeof(pos))&&std::isfinite(pos[0])&&std::isfinite(pos[1])&&std::fabs(pos[0])<=90&&std::fabs(pos[1])<=180;
    if(valid)valid=box_key;
    double distance=1e30;
    if(valid){
      double a=(pos[0]-loc.lat)*0.017453292519943295,b=(pos[1]-loc.lng)*0.017453292519943295;
      double h=std::sin(a/2)*std::sin(a/2)+std::cos(loc.lat*0.017453292519943295)*std::cos(pos[0]*0.017453292519943295)*std::sin(b/2)*std::sin(b/2);
      distance=12742000.0*std::asin(std::sqrt(std::fmin(1.0,std::fmax(0.0,h))));
      nearest_any=std::fmin(nearest_any,distance);++in_range;
      valid=distance<=nearest;
    }
    if(valid){
      void *exc=nullptr;void *visible=invoke(visible_method,root_get(candidate_pin),nullptr,&exc);uint8_t yes=0;
      valid=!exc&&visible&&read_field(visible,0x10,yes)&&yes==1;if(valid)++visible_count;
    }
    if(valid){release(best);best=candidate_pin;candidate_pin=0;nearest=distance;}
    release(candidate_pin);
  }
  release(array_pin);
  if(diagnose){std::lock_guard<std::mutex> l(state_mutex);counters.list_count=count;counters.candidate_count=candidates;counters.matched_box_count=matched_boxes;counters.in_range_count=in_range;counters.visible_count=visible_count;counters.expired_count=expired_count;counters.zero_expiry_count=zero_expiry;counters.nearest_meters=nearest_any==1e30?-1.0:nearest_any;}
  if(!best){reason(10);return;}
  void *candidate=root_get(best);uint64_t id=0;
  // A managed visibility call may reenter settings/session updates. Re-read the
  // short owner lease, fresh location and every mutable candidate action gate.
  const uint64_t action_now=now_ms();Location fresh;
  {std::lock_guard<std::mutex> l(location_mutex);fresh=location;}
  uint8_t still_active=0;int64_t expiry=0;double pos[2]{};char key[97]{};
  bool authorized=!stopped.load()&&action_now<=enable_until_ms.load()&&fresh.at&&action_now>=fresh.at&&action_now-fresh.at<=5000&&
    root_get(session_root)==session&&read_field(session,0x98,still_active)&&still_active==1&&
    exact(candidate,controller_class)&&read_field(candidate,0xd0,id)&&id&&!seen_before(id,action_now)&&
    read_field(candidate,0x100,expiry)&&expiry>=0&&read_bytes(candidate,0xe8,pos,sizeof(pos))&&
    std::isfinite(pos[0])&&std::isfinite(pos[1])&&std::fabs(pos[0])<=90&&std::fabs(pos[1])<=180;
  if(authorized){
    read_key(candidate,0xe0,key);authorized=std::strcmp(key,"TAPPABLE_TYPE_BOX")==0;
    double a=(pos[0]-fresh.lat)*0.017453292519943295,b=(pos[1]-fresh.lng)*0.017453292519943295;
    double h=std::sin(a/2)*std::sin(a/2)+std::cos(fresh.lat*0.017453292519943295)*std::cos(pos[0]*0.017453292519943295)*std::sin(b/2)*std::sin(b/2);
    authorized=authorized&&std::isfinite(h);
  }
  if(!authorized){release(best);reason(11);return;}
  controller_root=best;active_id=id;dispatched_ms=action_now;ambiguous=false;dispatching=true;
  {std::lock_guard<std::mutex> l(state_mutex);++counters.attempted;counters.inflight=1;}
  void *arg=root_get(controller_root),*exc=nullptr;void *args[]={arg};
  struct DispatchGuard{~DispatchGuard(){dispatching=false;}} dispatch_guard;
  if(stopped.load()||now_ms()>enable_until_ms.load()){
    clear_attempt();std::lock_guard<std::mutex> l(state_mutex);counters.inflight=0;return;
  }
  reason(12);invoke(tap_method,root_get(session_root),args,&exc);
  dispatching=false;
  if(active_id&&(exc||!service_root)){
    // No validated service callback means ownership cannot be attributed; stop
    // future dispatch rather than claim this request was safely handled.
    stopped.store(true);enable_until_ms.store(0);
    if(!service_root)clear_attempt();
    std::lock_guard<std::mutex> l(state_mutex);counters.error=-48;++counters.failed;counters.inflight=active_id!=0;
  }
}
} // namespace
void pgr_event_collector_location(double lat,double lng,uint64_t at){
  std::lock_guard<std::mutex> l(location_mutex);
  if(!std::isfinite(lat)||!std::isfinite(lng)||std::fabs(lat)>90||std::fabs(lng)>180||!at){location={};return;}
  location={lat,lng,at};
}
int pgr_event_collector_tick(bool enabled,uint64_t tid){
  if(!tid||tid!=uint64_t(gettid()))return -6;
  if(!ready.load()){
    std::lock_guard<std::mutex> l(install_mutex);
    if(state<0)return state;
    state=install(tid);
    if(state){std::lock_guard<std::mutex> s(state_mutex);counters.error=state;return state;}
  }
  if(!owner_thread())return -6;
  const uint64_t lease_now=now_ms();
  enable_until_ms.store(enabled&&!stopped.load()?lease_now+2500:0);
  {std::lock_guard<std::mutex> l(state_mutex);counters.ready=1;if(counters.error>0)counters.error=0;counters.enabled=enabled&&!stopped.load();}
  // Gameplay runs only after a natural session.Update, never nested inside
  // UnitySynchronizationContext.ExecuteTasks owner-pulse instrumentation.
  pump(false,now_ms());
  if(stopped.load()&&!active_id)release(session_root);
  return 0;
}
void pgr_event_collector_stop(){enable_until_ms.store(0);stopped.store(true,std::memory_order_release);}
PgrEventCollectorState pgr_event_collector_state(){std::lock_guard<std::mutex> l(state_mutex);auto out=counters;out.update_calls=update_calls.load();out.owner_updates=owner_updates.load();out.last_update_tid=last_update_tid.load();out.pending_encounters=0;out.stale_encounters=0;
  // Called on the proven owner pulse by runtime status reporting.
  if(owner_thread())for(const auto &r:owned_encounters)if(r.id){++out.pending_encounters;if(r.stale)++out.stale_encounters;}
  return out;}

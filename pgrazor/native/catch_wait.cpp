#include "catch_wait.h"
#include "catch_wait_schema.h"
#include "managed_handle_abi.h"
#include "hook_engine.h"
#include <atomic>
#include <mutex>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
namespace {
using namespace pgr_catch_wait_schema;
using Move=uint8_t(*)(void *,const void *);
using GetField=void(*)(void *,const void *,void *);
using ObjClass=void *(*)(void *);
using NewRoot=uintptr_t(*)(void *,int);
using GetRoot=void *(*)(uintptr_t);
using FreeRoot=void(*)(uintptr_t);
std::mutex install_mutex,queue_mutex,gate_mutex;
std::atomic<Move> original{nullptr};
std::atomic<bool> ready{false},active{false};
std::atomic<int32_t> fault{0},owner{0};
std::atomic<uint64_t> deadline{0},observed{0},shortened{0};
PgrBootBinding binding{};
const void *classes[4]{},*fields[7]{};
int32_t success=0;
bool poisoned=false;
uint64_t window=0;uint32_t hits=0;
PgrCatchWaitEvent queue[kQueueMax]{};uint32_t head=0,count=0;
thread_local bool in_callback=false;
uint64_t now(){timespec t{};if(clock_gettime(CLOCK_BOOTTIME,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000)return 0;return uint64_t(t.tv_sec)*1000+uint64_t(t.tv_nsec)/1000000;}
void fail(int reason){int32_t empty=0;fault.compare_exchange_strong(empty,reason);deadline.store(0);}
template<class T>T read(void *object,const void *field){T out{};reinterpret_cast<GetField>(binding.runtime.field_get_value)(object,field,&out);return out;}
bool exact(void *object,const void *klass){return object&&reinterpret_cast<ObjClass>(binding.runtime.object_get_class)(object)==klass;}
struct Scope {
 uintptr_t root=0;
 Scope(){in_callback=true;}
 ~Scope(){if(root)reinterpret_cast<FreeRoot>(binding.runtime.gchandle_free)(root);in_callback=false;}
 void *target(){return root?reinterpret_cast<GetRoot>(binding.runtime.gchandle_get_target)(root):nullptr;}
};
bool reserve(){std::lock_guard<std::mutex> guard(queue_mutex);return count+2<=kQueueMax;}
void event(const PgrCatchWaitEvent &e){std::lock_guard<std::mutex> guard(queue_mutex);if(count==kQueueMax){fail(-20);return;}queue[(head+count++)%kQueueMax]=e;}
uint8_t replacement(void *self,const void *method){
 Move invoke=original.load(std::memory_order_acquire);
 if(!invoke){std::lock_guard<std::mutex> guard(install_mutex);invoke=original.load(std::memory_order_acquire);}
 if(!invoke)std::abort();
 if(!ready.load(std::memory_order_acquire)||!active.load()||fault.load()||in_callback)return invoke(self,method);
 const auto tid=int32_t(gettid());
 if(!reinterpret_cast<void *(*)()>(binding.runtime.thread_current)()){fail(-21);return invoke(self,method);}
 int32_t unset=0;owner.compare_exchange_strong(unset,tid);
 if(owner.load()!=tid){fail(-22);return invoke(self,method);}
 const auto entered=now();if(!entered){fail(-23);return invoke(self,method);}
 if(entered-window>=1000){window=entered;hits=0;}
 if(++hits>kHitsPerSecondMax){fail(-24);return invoke(self,method);}
 if(!exact(self,classes[0])){fail(-25);return invoke(self,method);}
 Scope scope;
 scope.root=reinterpret_cast<NewRoot>(binding.runtime.gchandle_new)(self,1);
 if(!scope.root){fail(-26);return invoke(self,method);}
 void *pinned=scope.target();
 if(!exact(pinned,classes[0])){fail(-26);return invoke(self,method);}
 uint8_t result;
 try{result=invoke(pinned,method);}catch(...){fail(-27);throw;}
 if(!active.load()||fault.load()||result!=1)return result;
 self=scope.target();if(!exact(self,classes[0])){fail(-25);return result;}
 if(read<int32_t>(self,fields[0])!=kYieldState)return result;
 // The remaining IL2CPP accessors are non-allocating scalar/field reads and a
 // scalar write. No managed invocation or GC allocation crosses borrowed values.
 void *capture=read<void *>(self,fields[2]),*response=read<void *>(self,fields[3]),*wait=read<void *>(self,fields[1]);
 if(!exact(capture,classes[1])||!exact(response,classes[2])||!exact(wait,classes[3])){fail(-28);return result;}
 const int32_t status=read<int32_t>(response,fields[6]);if(status!=success)return result;
 const float duration=read<float>(wait,fields[5]);
 if(!std::isfinite(duration)||duration<0||duration>kMaxDurationSeconds){fail(-29);return result;}
 const auto branch=read<uint8_t>(self,fields[4]);if(branch>1){fail(-30);return result;}
 const auto time=now();if(!time){fail(-23);return result;}
 if(!reserve()){fail(-20);return result;}
 PgrCatchWaitEvent receipt{1,branch,tid,status,duration,0,time};event(receipt);observed.fetch_add(1);
 if(time>=deadline.load()||duration<=kMinimumDurationSeconds)return result;
 if(read<void *>(self,fields[1])!=wait||read<float>(wait,fields[5])!=duration){fail(-31);return result;}
 const auto before_write=now();
 if(!before_write||!active.load()||fault.load()||before_write>=deadline.load())return result;
 float replacement_value=float(kMinimumDurationSeconds);
 reinterpret_cast<GetField>(binding.runtime.field_set_value)(wait,fields[5],&replacement_value);
 const float actual=read<float>(wait,fields[5]);
 if(!std::isfinite(actual)||std::fabs(actual-kMinimumDurationSeconds)>1e-6){fail(-32);return result;}
 receipt.kind=2;receipt.new_seconds=actual;event(receipt);shortened.fetch_add(1);
 return result;
}
bool named_type(const PgrMetaExports &e,const void *type,const char *expected){
 char *name=reinterpret_cast<char *(*)(const void *)>(e.type_get_name)(type);
 if(!name)return false;
 const auto n=strnlen(name,kNameBytesMax);const bool same=n<kNameBytesMax&&std::strcmp(name,expected)==0;
 reinterpret_cast<void(*)(void *)>(e.mem_free)(name);return same;
}
int prepare(const PgrCatchWaitConfig *test,PgrBootBinding &b,void **target){
 const auto *md=static_cast<const PgrMetaApi *>(pgrazor_runtime_metadata_api(1));if(!md)return -2;
 if(test){if(test->version!=1||test->size!=sizeof(*test)||!test->binding||!test->segment||!test->class_get_name||!test->fixture_move_next)return -2;b=*test->binding;}
 else {const auto *boot=static_cast<const PgrBootApi *>(pgrazor_runtime_bootstrap_api(1));const int rc=boot->snapshot(&b);if(rc)return rc;}
 if(b.version!=1||b.size!=sizeof(b)||b.status||!b.module_base||!b.module_size||!b.runtime.thread_current)return -2;
 if(!b.runtime.gchandle_new||!b.runtime.gchandle_get_target||!b.runtime.gchandle_free||
    !b.runtime.field_get_value||!b.runtime.field_set_value||!b.runtime.object_get_class||!b.runtime.field_static_get_value)return -2;
 auto current=reinterpret_cast<void *(*)()>(b.runtime.thread_current);
 auto attach=reinterpret_cast<void *(*)(void *)>(b.runtime.thread_attach);
 auto detach=reinterpret_cast<void(*)(void *)>(b.runtime.thread_detach);
 struct Attachment{void *owned;void(*drop)(void *);~Attachment(){if(owned)drop(owned);}} attachment{nullptr,detach};
 if(!current()){if(!attach||!detach||!b.domain)return -3;attachment.owned=attach(const_cast<void *>(b.domain));if(!attachment.owned)return -3;}
 PgrMetaBindConfig config{};config.version=1;config.size=sizeof(config);config.exports=&b.meta;
 config.module_base=b.module_base;config.module_size=b.module_size;
 if(test){config.seg_query=test->segment;config.seg_ctx=test->segment_context;}
 else {config.debug_prologue=kDebugPrologue;config.debug_prologue_len=sizeof(kDebugPrologue);}
 int rc=md->bind(&config);if(rc)return rc;
 auto check=[&](uint64_t rva,uint64_t length,const void *bytes=nullptr,uint32_t n=0){
  if(rva>b.module_size||length>b.module_size-rva)return int(PGR_MD_ERR_CODE_RANGE);
  PgrMetaCodeQuery q{};q.version=1;q.size=sizeof(q);q.address=static_cast<const char *>(b.module_base)+rva;q.length=length;q.prologue=bytes;q.prologue_len=n;return md->validate_code(&q);
 };
 if(!test&&pgr_pointer_handles(md,b.runtime.gchandle_new,b.runtime.gchandle_get_target,b.runtime.gchandle_free))return -4;
 void *name_export=test?test->class_get_name:dlsym(b.handle,"il2cpp_class_get_name");if(!name_export)return -5;
 if(!test){PgrMetaCodeQuery q{};q.version=1;q.size=sizeof(q);q.address=name_export;q.length=4;if((rc=md->validate_code(&q)))return rc;}
 for(const auto &spec:kClasses){
  PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=spec.name_space;q.name=spec.name;q.canonical=spec.canonical;q.canonical_match=PGR_MD_MATCH_EXACT;
  PgrMetaClassResult out{};rc=md->resolve_class(&q,&out);if(rc)return rc==PGR_MD_ERR_NOT_FOUND?1:rc;classes[spec.owner]=out.klass;
 }
 void *iterator=nullptr;bool ended=false;
 const auto nested=reinterpret_cast<void *(*)(const void *,void **)>(b.meta.class_get_nested_types);
 const auto class_name=reinterpret_cast<const char *(*)(const void *)>(name_export);
 for(unsigned i=0;i<kNestedMax;i++){
  const void *klass=nested(classes[1],&iterator);if(!klass){ended=true;break;}
  const char *name=class_name(klass);if(!name||strnlen(name,kNameBytesMax)==kNameBytesMax)return -6;
  if(std::strcmp(name,kNestedName))continue;
  if(classes[0]||reinterpret_cast<const void *(*)(const void *)>(b.meta.class_get_declaring_type)(klass)!=classes[1])return -6;
  classes[0]=klass;
 }
 if(!ended||!classes[0])return -6;
 const auto field_from=reinterpret_cast<void *(*)(const void *,const char *)>(b.meta.class_get_field_from_name);
 const auto offset=reinterpret_cast<size_t(*)(const void *)>(b.meta.field_get_offset);
 const auto field_type=reinterpret_cast<void *(*)(const void *)>(b.meta.field_get_type);
 for(unsigned i=0;i<kFieldCount;i++){
  const auto &spec=kFields[i];const void *f=field_from(classes[spec.owner],spec.name);if(!f)return -7;
  const auto off=offset(f);if(off<spec.min_offset||off>spec.max_offset)return -7;
  PgrMetaFieldQuery q{};q.version=1;q.size=sizeof(q);q.owner=classes[spec.owner];q.name=spec.name;
  q.offset=spec.offset_is_exact?spec.exact_offset:off;q.forbidden_mask=kRejectedFieldFlags;
  q.kind=i==6?PGR_MD_FIELD_ENUM_I32:PGR_MD_FIELD_ANY;q.type_name=spec.type_name;q.type_match=PGR_MD_MATCH_EXACT;
  PgrMetaFieldResult out{};if((rc=md->validate_field(&q,&out)))return rc;fields[i]=out.field;
 }
 const void *enum_class=reinterpret_cast<void *(*)(const void *)>(b.meta.class_from_type)(field_type(fields[6]));
 const void *literal=field_from(enum_class,kSuccessLiteral);if(!literal)return -8;
 if(reinterpret_cast<void *(*)(const void *)>(b.meta.field_get_parent)(literal)!=enum_class||
    (reinterpret_cast<uint32_t(*)(const void *)>(b.meta.field_get_flags)(literal)&kSuccessFlagsMask)!=kSuccessFlagsExpected||
    !named_type(b.meta,field_type(literal),kStatusEnum))return -8;
 if(!b.runtime.field_static_get_value)return -8;
 reinterpret_cast<void(*)(const void *,void *)>(b.runtime.field_static_get_value)(literal,&success);
 for(const auto &spec:kMethods){
  if(spec.rva>b.module_size)return PGR_MD_ERR_CODE_RANGE;
  PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=classes[spec.owner];q.name=spec.name;
  q.return_type=spec.return_type;q.return_match=PGR_MD_MATCH_EXACT;q.param_count=spec.param_count;q.is_static=spec.is_static;
  q.expected_code=static_cast<const char *>(b.module_base)+spec.rva;q.prologue=spec.prologue;q.prologue_len=spec.prologue_size;
  PgrMetaMethodResult out{};if((rc=md->validate_method(&q,&out)))return rc;
  if(spec.owner==0)*target=const_cast<void *>(out.code);
 }
 for(const auto &site:{kYieldTailSite,kYieldReturnSite,kStepSite})if((rc=check(site.rva,site.size,site.bytes,site.size)))return rc;
 for(uint64_t off=0;off<kMoveNextSpan;off+=PGR_MD_CODE_MAX){const auto left=kMoveNextSpan-off;if((rc=check(kMoveNextRva+off,left>PGR_MD_CODE_MAX?PGR_MD_CODE_MAX:left)))return rc;}
 if(test)*target=test->fixture_move_next;
 return 0;
}
int install(const PgrCatchWaitConfig *test){
 std::unique_lock<std::mutex> guard(install_mutex,std::try_to_lock);if(!guard.owns_lock())return -10;
 if(poisoned)return -11;if(ready.load())return 0;
 std::memset(classes,0,sizeof(classes));std::memset(fields,0,sizeof(fields));
 PgrBootBinding resolved{};void *target=nullptr;const int rc=prepare(test,resolved,&target);if(rc)return rc;
 binding=resolved;void *trampoline=nullptr;
 const int hook=pgrazor_hook(target,reinterpret_cast<void *>(replacement),&trampoline);
 if(trampoline)original.store(reinterpret_cast<Move>(trampoline),std::memory_order_release);
 if(hook||!trampoline){poisoned=true;return -12;}
 active.store(true);ready.store(true,std::memory_order_release);return 0;
}
int update(int allow,uint32_t lease){std::lock_guard<std::mutex> guard(gate_mutex);deadline.store(0);if(allow!=0&&allow!=1)return -2;
 if(!allow)return 0;if(!ready.load()||!active.load()||fault.load())return -13;
 const auto time=now();if(!time){fail(-23);return -23;}deadline.store(time+(lease>kLeaseMs?kLeaseMs:lease));return 0;
}
int sample(PgrCatchWaitSample *out){if(!out)return -2;const bool installed=ready.load(std::memory_order_acquire);std::lock_guard<std::mutex> guard(queue_mutex);*out={1,sizeof(*out),uint32_t(installed),uint32_t(active.load()),fault.load(),owner.load(),count,installed?success:0,observed.load(),shortened.load()};return 0;}
int next(PgrCatchWaitEvent *out){if(!out)return -2;std::lock_guard<std::mutex> guard(queue_mutex);if(!count)return 0;*out=queue[head];head=(head+1)%kQueueMax;--count;return 1;}
int stop(){std::lock_guard<std::mutex> guard(gate_mutex);active.store(false);deadline.store(0);return 0;}
const PgrCatchWaitApi api{1,sizeof(PgrCatchWaitApi),install,update,sample,next,stop};
}
extern "C" const void *pgrazor_catch_wait_api(uint32_t version){return version==1?&api:nullptr;}

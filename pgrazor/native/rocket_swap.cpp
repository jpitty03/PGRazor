#include "rocket_swap.h"
#include "game_bindings.h"
#include "hook_engine.h"
#include "rocket_combat.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include "smart_battle.h"
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <time.h>
#include <unistd.h>
// Team GO Rocket auto swap. In a Rocket battle (vNext), when the active Pokemon faints the game shows
// CombatVNextSwapPromptWidget with one SwapButtonVNext per party Pokemon and waits for a tap. Its
// ShowPromptWidget is hooked only to note that the prompt opened; on later ticks, while the prompt is
// open, the first button whose UI Button is interactable and that shows neither the fainted nor the
// active-Pokemon overlay gets its
// own Swap(), which is exactly what a tap on it runs. One press per second at most, until the prompt
// closes or 8 seconds pass. Only while Team GO Rocket automation is on (transport: active,
// rocketAllowed, rocketOptions != 0) and the Rocket combat module holds a Rocket battle scope, so GO
// Battle League and trainer battles are never touched.
namespace {
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
template<class F>F fn(const void *p){require(p,"missing runtime export");return reinterpret_cast<F>(const_cast<void*>(p));}

PgrBootBinding boot{};
bool ready=false,busy=false,tried=false;
std::atomic<bool> enabled{false},faulted{false},installed{false};
std::atomic<uint64_t> owner{0};
using Show=void(*)(void*,const void*);
std::atomic<Show> original{nullptr};
void *prompt_list=nullptr,*prompt_is_open=nullptr,*button_field=nullptr,*swap_method=nullptr,*interactable=nullptr;
void *fainted_field=nullptr,*active_field=nullptr,*active_self=nullptr;  // SwapButtonVNext overlays, GameObject.activeSelf
uintptr_t pending=0;uint64_t pending_since=0,next_try=0;int presses=0;
constexpr uint64_t kSettleMs=400,kRetryMs=1000,kGiveUpMs=8000;

uint64_t clock_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
void *exp(const char *name){std::string n="il2cpp_";n+=name;void *p=dlsym(boot.handle,n.c_str());require(p,name);return p;}
void *klass(void *o){require(o,"null object");return fn<void*(*)(void*)>(boot.runtime.object_get_class)(o);}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
void *method(void *k,const char *name,int n){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,n);require(m,name);return m;}
template<class T>T unbox(void *o){require(o,"boxed value");T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");std::memcpy(&v,p,sizeof(v));return v;}
void *ref_field(void *o,void *f){void *v=nullptr;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;}
std::string typename_of(const void *type){char *p=fn<char*(*)(const void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
void event(const std::string &json){auto tr=transport();if(!tr)return;PgrRuntimeTransportString s[]={{"rocket-swap",11},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,s,2);}
void status(const char *phase,const std::string &message){event(std::string("{\"phase\":\"")+phase+"\",\"message\":\""+message+"\"}");}

// Team GO Rocket automation on: active, rocketAllowed and any rocketOptions bit, from one fresh snapshot.
bool configured(){
 auto tr=transport();PgrRuntimeTransportSample a{},z{};
 if(!tr||tr->sample(&a)||!a.snapshot_fresh||a.fault)return false;
 int active=0,allowed=0;int32_t options=0;
 if(tr->get_bool(3,&active)||tr->get_bool(26,&allowed)||tr->get_int(6,&options))return false;
 if(tr->sample(&z)||!z.snapshot_fresh||z.snapshot_seq!=a.snapshot_seq)return false;
 return active==1&&allowed==1&&(options>0||pgr_smart_battle_enabled());
}
bool rocket_battle(){
 auto api=static_cast<const pgr_rocket_combat_api_v1*>(pgrazor_rocket_combat_api(1));
 if(!api||!api->sample)return false;
 pgr_rocket_combat_sample_v1 s{};s.version=PGR_RC_API_VERSION;s.size=sizeof(s);
 return api->sample(&s)==0&&s.scope_active==1;
}
bool gate(){
 const uint64_t tid=owner.load(std::memory_order_acquire);
 return enabled.load(std::memory_order_acquire)&&!faulted.load(std::memory_order_acquire)&&tid&&tid==uint64_t(gettid());
}

void hook_show(void *self,const void *m){
 original.load(std::memory_order_acquire)(self,m);
 if(!self||!gate()||pending)return;
 pending=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(self,0);
 pending_since=clock_ms();next_try=pending_since+kSettleMs;presses=0;
}
void release(){if(pending)fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(pending);pending=0;}

// One step while the prompt is open: press the first interactable button.
void step(){
 const uint64_t now=clock_ms();
 void *widget=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(pending);
 if(!widget||now-pending_since>kGiveUpMs){release();return;}
 if(now<next_try)return;
 if(!unbox<bool>(invoke(prompt_is_open,widget))){release();return;}  // closed: picked or timed out
 if(!rocket_battle()){release();return;}
 void *list=ref_field(widget,prompt_list);if(!list){next_try=now+kSettleMs;return;}
 const int count=unbox<int>(invoke(method(klass(list),"get_Count",0),list));
 void *get_item=method(klass(list),"get_Item",1);
 void *usable[16];int slots[16],n=0;
 for(int i=0;i<count&&i<16;i++){
  void *args[]={&i};void *button=invoke(get_item,list,args);if(!button)continue;
  void *ui=ref_field(button,button_field);if(!ui)continue;
  if(!unbox<bool>(invoke(interactable,ui)))continue;
  // The fainted Pokemon's and the active Pokemon's buttons can still be interactable; their overlays tell.
  auto shown=[&](void *f){void *go=ref_field(button,f);return go&&unbox<bool>(invoke(active_self,go));};
  if(shown(fainted_field)||shown(active_field))continue;
  usable[n]=button;slots[n]=i;++n;
 }
 if(n){
  int pick=pgr_smart_battle_pick_prompt(usable,n);if(pick<0||pick>=n)pick=0;  // Smart battling off or unsure: first available.
  invoke(swap_method,usable[pick]);++presses;
  event("{\"phase\":\"swapped\",\"slot\":"+std::to_string(slots[pick])+",\"presses\":"+std::to_string(presses)+"}");
  next_try=now+kRetryMs;return;
 }
 next_try=now+kSettleMs;  // buttons not ready yet
}

void *class_of(const PgrMetaApi *api,const char *ns,const char *name){
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult r{};require(api->resolve_class(&q,&r)==0,name);return const_cast<void*>(r.klass);
}
void install(){
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api&&api->resolve_class&&api->validate_method,"metadata API");
 require(boot.module_base,"il2cpp module");
 void *widget=class_of(api,"Niantic.Holoholo.Combat.UI","CombatVNextSwapPromptWidget");
 void *button=class_of(api,"Niantic.Holoholo.VNext","SwapButtonVNext");
 void *selectable=class_of(api,"UnityEngine.UI","Selectable");
 prompt_is_open=method(widget,"get_IsOpen",0);
 swap_method=method(button,"Swap",0);
 interactable=method(selectable,"IsInteractable",0);
 button_field=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(button,"button");require(button_field,"SwapButtonVNext.button");
 require(typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(button_field))=="UnityEngine.UI.Button","SwapButtonVNext.button type");
 auto go_field=[&](const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(button,name);require(f,name);
  require(typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))=="UnityEngine.GameObject",name);return f;};
 fainted_field=go_field("faintedOverlay");active_field=go_field("activePokemonBackground");
 active_self=method(class_of(api,"UnityEngine","GameObject"),"get_activeSelf",0);
 // The prompt's one List<SwapButtonVNext> field.
 void *iter=nullptr;
 while(void *f=fn<void*(*)(void*,void**)>(exp("class_get_fields"))(widget,&iter)){
  if(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10)continue;
  const std::string t=typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));
  if(t.rfind("System.Collections.Generic.List",0)==0&&t.find("SwapButtonVNext")!=std::string::npos){require(!prompt_list,"swap button list");prompt_list=f;}
 }
 require(prompt_list,"swap button list");
 static const uint8_t prologue[]=PGR_GAME_ROCKET_SWAP_CPP_SHOW_PROMPT_FINGERPRINT;
 static_assert(sizeof(prologue)==24,"24-byte prologue");
 PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=widget;q.name="ShowPromptWidget";q.return_type="System.Void";q.return_match=PGR_MD_MATCH_EXACT;q.param_count=0;
 q.expected_code=static_cast<const char*>(boot.module_base)+PGR_GAME_ROCKET_SWAP_CPP_SHOW_PROMPT_RVA;q.prologue=prologue;q.prologue_len=sizeof(prologue);
 PgrMetaMethodResult r{};const int rc=api->validate_method(&q,&r);
 require(rc==0&&r.code,"ShowPromptWidget signature");
 void *o=nullptr;require(pgrazor_hook(const_cast<void*>(r.code),reinterpret_cast<void*>(hook_show),&o)==0&&o,"ShowPromptWidget hook");
 original.store(reinterpret_cast<Show>(o),std::memory_order_release);
 installed.store(true,std::memory_order_release);
 status("ready","Rocket auto swap ready");
}
}  // namespace

void pgr_rocket_swap_tick(uint64_t tid){
 if(busy||!tid||tid!=uint64_t(gettid()))return;
 const uint64_t prior=owner.load(std::memory_order_acquire);if(prior&&prior!=tid)return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 owner.store(tid,std::memory_order_release);
 const bool on=configured();enabled.store(on,std::memory_order_release);
 if(pending){
  if(!on)release();
  else{
   try{step();}
   catch(const Failure &f){release();status("error",std::string("Auto swap stopped this time: ")+f.where);}
   catch(...){release();status("error","Auto swap stopped this time: native failure");}
  }
 }
 if(!on||tried||faulted.load(std::memory_order_acquire))return;
 tried=true;
 try{
  if(!ready){auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(api&&api->snapshot(&boot)==0,"Runtime unavailable");ready=true;}
  install();
 }catch(const Failure &f){faulted.store(true,std::memory_order_release);status("error",std::string("Rocket auto swap unavailable: ")+f.where);}
 catch(...){faulted.store(true,std::memory_order_release);status("error","Rocket auto swap unavailable: native failure");}
}
void pgr_rocket_swap_stop(){enabled.store(false,std::memory_order_release);}

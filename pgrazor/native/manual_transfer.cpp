#include "manual_transfer.h"
#include "manual_transfer_policy.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "hook_engine.h"
#include <atomic>
#include <mutex>
#include <type_traits>
#include <cstring>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
namespace {
PgrBootBinding boot{};
const PgrMetaApi *md=nullptr;
const PgrRuntimeTransportApi *tr=nullptr;
std::atomic<bool> stopped{false},ready{false},paused{false};
std::mutex install_mutex;
bool attempted=false,busy=false,warning_ready=false,selection_busy=false;
static_assert(sizeof(uintptr_t)==8,"Manual transfer requires LP64 handles");
const char *prepare_stage="bootstrap";
std::atomic<uint64_t> owner{0};
uint64_t next=0,report_at=0,unlocked=0,multi_unlocked=0,selection_unlocked=0;
pgr_manual::Window window,selection_window;
void *dialog_class=nullptr,*data_class=nullptr,*button_class=nullptr,*modal_class=nullptr,*pokemon_class=nullptr;
void *favorite=nullptr,*egg=nullptr,*fort=nullptr,*buttons_field=nullptr,*bullets_field=nullptr,*back_field=nullptr,*cancel_field=nullptr,*no_field=nullptr;
void *type_object=nullptr,*array_length=nullptr,*string_length=nullptr,*type_byref=nullptr;
const void *children=nullptr,*on_click=nullptr,*fire=nullptr,*alive=nullptr,*enabled=nullptr,*interactable=nullptr;
void *start_codes[3]{},*init_code=nullptr,*unlock_code=nullptr,*multi_code=nullptr,*selection_code=nullptr;
unsigned start_count=0;
uintptr_t pending_dialog=0,pending_data=0;
uint64_t ui_generation=0,pending_ui_generation=0;
using UnlockFn=bool(*)(void*,void*,bool,const void*);
using CheckFn=bool(*)(void*,void*,const void*);
std::atomic<UnlockFn> original_unlock{nullptr};
std::atomic<CheckFn> original_multi{nullptr},original_selection{nullptr};
bool error=false;
uint64_t now(){timespec t{};return clock_gettime(CLOCK_BOOTTIME,&t)||t.tv_sec<0?0:uint64_t(t.tv_sec)*1000+uint64_t(t.tv_nsec)/1000000;}
void *target(uintptr_t h){return h?reinterpret_cast<void *(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h):nullptr;}
uintptr_t hold(void *o){if(!o)return 0;uintptr_t h=reinterpret_cast<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(o,1);if(!h)error=true;return h;}
void release(uintptr_t &h){uintptr_t old=h;h=0;if(old)reinterpret_cast<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(old);}
struct Root {uintptr_t h=0;explicit Root(void *o=nullptr):h(hold(o)){}~Root(){release(h);}void *get()const{return target(h);}Root(const Root&)=delete;};
void drop(){release(pending_dialog);release(pending_data);window.drop();}
void revoke(){drop();window.revoke();selection_window.revoke();}
void *invoke(const void *m,void *self,void **args=nullptr){if(!m){error=true;return nullptr;}void *exception=nullptr;
 if(self){void *parent=reinterpret_cast<void*(*)(const void*)>(boot.meta.method_get_class)(m);void *actual=reinterpret_cast<void*(*)(void*)>(boot.runtime.object_get_class)(self);
  if(!parent||!actual||!reinterpret_cast<bool(*)(void*,void*)>(boot.meta.class_is_assignable_from)(parent,actual)){error=true;return nullptr;}}
 void *r=reinterpret_cast<void *(*)(const void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,self,args,&exception);
 if(exception)error=true;return exception?nullptr:r;
}
template<class T>T read(void *o,const void *f){T value{};if(!o||!f){error=true;return value;}reinterpret_cast<void(*)(void*,const void*,void*)>(boot.runtime.field_get_value)(o,f,&value);return value;}
void *read_static(void *f){void *o=nullptr;if(!f){error=true;return nullptr;}reinterpret_cast<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&o);return o;}
bool exact(void *o,void *k){return o&&k&&reinterpret_cast<void *(*)(void*)>(boot.runtime.object_get_class)(o)==k;}
bool boolean(void *o){Root value(o);if(!value.get()){error=true;return false;}void *p=reinterpret_cast<void *(*)(void*)>(boot.runtime.object_unbox)(value.get());if(!p){error=true;return false;}return *static_cast<uint8_t*>(p)!=0;}
bool live(void *o){if(!o)return false;void *args[]={o};return boolean(invoke(alive,nullptr,args));}
void event(const char *kind,const char *reason=""){
 char json[512];std::snprintf(json,sizeof(json),"{\"kind\":\"%s\",\"reason\":\"%s\",\"backend\":\"native\",\"unlock\":%s,\"multi\":%s,\"selection\":%s,\"unlocked\":%llu,\"multiChanged\":%llu,\"selectionChanged\":%llu}",
  kind,reason,original_unlock.load()?"true":"false",original_multi.load()?"true":"false",original_selection.load()?"true":"false",
  (unsigned long long)unlocked,(unsigned long long)multi_unlocked,(unsigned long long)selection_unlocked);
 const char channel[]="manual-transfer";PgrRuntimeTransportString s[]={{channel,sizeof(channel)-1},{json,uint32_t(std::strlen(json))}};tr->emit(19,nullptr,0,s,2);
}
void status(const char *text){int64_t available=ready&&!paused&&!stopped;PgrRuntimeTransportString s{text,uint32_t(std::strlen(text))};tr->emit(61,&available,1,&s,1);}
void fault(const char *why){paused=true;revoke();event("paused",why);status("Paused: manual transfer read/call failed; toggle Off to reset");}
bool gate(){
 if(!ready||paused||stopped||owner!=uint64_t(gettid())||!reinterpret_cast<void*(*)()>(boot.runtime.thread_current)())return false;
 PgrRuntimeTransportSample a{},b{};int allowed=0;
 return !tr->sample(&a)&&a.snapshot_fresh&&!a.fault&&!tr->get_bool(43,&allowed)&&allowed==1&&
  !tr->sample(&b)&&b.snapshot_fresh&&!b.fault&&a.snapshot_seq==b.snapshot_seq&&a.generation==b.generation;
}
void *klass(const char *ns,const char *name,const char *canonical){PgrMetaClassQuery q{1,sizeof(q),ns,name,canonical,PGR_MD_MATCH_EXACT,0};PgrMetaClassResult r{};return md->resolve_class(&q,&r)?nullptr:const_cast<void*>(r.klass);}
std::string type_name(void *type){if(!type)return {};char *n=reinterpret_cast<char *(*)(void*)>(boot.meta.type_get_name)(type);if(!n)return {};std::string result(n);reinterpret_cast<void(*)(void*)>(boot.meta.mem_free)(n);return result;}
bool reference(void *type){if(!type||reinterpret_cast<bool(*)(void*)>(type_byref)(type))return false;void *k=reinterpret_cast<void *(*)(void*)>(boot.meta.class_from_type)(type);return k&&!reinterpret_cast<bool(*)(void*)>(boot.meta.class_is_valuetype)(k);}
void *field(void *k,const char *name,const char *expected=nullptr,bool is_static=false){
 if(!k)return nullptr;void *f=reinterpret_cast<void *(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);if(!f)return nullptr;
 auto flags=reinterpret_cast<uint32_t(*)(void*)>(boot.meta.field_get_flags)(f);if(bool(flags&0x10)!=is_static||(flags&0x40))return nullptr;
 void *parent=reinterpret_cast<void *(*)(void*)>(boot.meta.field_get_parent)(f);
 if(!parent||!reinterpret_cast<bool(*)(void*,void*)>(boot.meta.class_is_assignable_from)(parent,k))return nullptr;
 void *type=reinterpret_cast<void *(*)(void*)>(boot.meta.field_get_type)(f);
 if(expected)return type_name(type)==expected?f:nullptr;
 return reference(type)?f:nullptr;
}
void *method(void *k,const char *name,unsigned count,const char *result=nullptr,const char *p0=nullptr,const char *p1=nullptr,bool inherited=false){
 if(!k||count>2)return nullptr;void *m=reinterpret_cast<void *(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,count);if(!m)return nullptr;
 void *parent=reinterpret_cast<void *(*)(void*)>(boot.meta.method_get_class)(m);
 if(parent!=k&&(!inherited||!reinterpret_cast<bool(*)(void*,void*)>(boot.meta.class_is_assignable_from)(parent,k)))return nullptr;
 std::string rn=type_name(reinterpret_cast<void *(*)(void*)>(boot.meta.method_get_return_type)(m));
 if(rn.empty()||(result&&rn!=result))return nullptr;
 std::string types[2];for(unsigned i=0;i<count;++i){types[i]=type_name(reinterpret_cast<void *(*)(void*,uint32_t)>(boot.meta.method_get_param)(m,i));if(types[i].empty())return nullptr;}
 if(p0&&types[0]!=p0)return nullptr;if(p1&&types[1]!=p1)return nullptr;
 PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=parent;q.name=name;q.return_type=rn.c_str();q.return_match=PGR_MD_MATCH_EXACT;q.param_count=count;
 for(unsigned i=0;i<count;++i){q.param_types[i]=types[i].c_str();q.param_match[i]=PGR_MD_MATCH_EXACT;}
 PgrMetaMethodResult r{};return md->validate_method(&q,&r)||r.method!=m?nullptr:m;
}
void *code(void *m){if(!m)return nullptr;void *info[3]{};if(!reinterpret_cast<bool(*)(void*,void*)>(boot.meta.debug_get_method_info)(m,info)||!info[0])return nullptr;
 PgrMetaCodeQuery q{1,sizeof(q),info[0],24,nullptr,0,0};return md->validate_code(&q)?nullptr:info[0];}
void *symbol(const char *name){void *p=dlsym(boot.handle,name);PgrMetaCodeQuery q{1,sizeof(q),p,4,nullptr,0,0};return p&&!md->validate_code(&q)?p:nullptr;}
pgr_manual::Pokemon pokemon(void *o){
 pgr_manual::Pokemon p;if(!exact(o,pokemon_class)||!favorite||!egg||!fort)return p;
 Root proto(o);if(!proto.get())return p;
 p.favorite=read<uint8_t>(proto.get(),favorite)!=0;p.egg=read<uint8_t>(proto.get(),egg)!=0;
 Root deployed(read<void*>(proto.get(),fort));if(deployed.get()){
  int n=reinterpret_cast<int(*)(void*)>(string_length)(deployed.get());if(n<0||n>1048576)return p;p.gym=n>0;
 }p.known=!error;return p;
}
template<class Fn>Fn original(std::atomic<Fn> &slot){Fn fn=slot.load(std::memory_order_acquire);if(!fn){std::lock_guard<std::mutex> lock(install_mutex);fn=slot.load(std::memory_order_acquire);}if(!fn)std::abort();return fn;}
// The original is always called exactly once, even when Off, stale, foreign-thread or stopped.
bool unlock_replacement(void *self,void *proto,bool fav,const void *info){
 auto fn=original(original_unlock);if(!gate()||busy)return fn(self,proto,fav,info);
 busy=true;struct Guard{~Guard(){busy=false;}}guard;error=false;Root receiver(self),pk(proto);
 bool result=fn(receiver.get()?receiver.get():self,pk.get()?pk.get():proto,fav,info);if(result||!gate())return result;
 auto p=pokemon(pk.get());if(error){fault("unlock-read");return result;}
 bool out=pgr_manual::transferable(result,gate(),fav,p);if(out!=result)++unlocked;return out;
}
bool multi_replacement(void *self,void *proto,const void *info){
 auto fn=original(original_multi);if(!gate()||busy)return fn(self,proto,info);
 // A multi-select eligibility query occurs BEFORE ClickBatchRelease.
 // Establish intent here, before its original can call IsSelectionDisabled.
 if(!selection_window.recent(now(),30000))selection_window.begin(now());
 busy=true;struct Guard{~Guard(){busy=false;}}guard;error=false;Root receiver(self),pk(proto);
 bool result=fn(receiver.get()?receiver.get():self,pk.get()?pk.get():proto,info);if(!result||!gate())return result;
 auto p=pokemon(pk.get());if(error){fault("multi-read");return result;}bool out=pgr_manual::disabled(result,gate(),p);if(out!=result)++multi_unlocked;return out;
}
bool selection_replacement(void *proto,void *game_master,const void *info){
 auto fn=original(original_selection);if(!gate()||selection_busy||!(window.recent(now(),30000)||selection_window.recent(now(),30000)))return fn(proto,game_master,info);
 bool was_busy=busy;busy=selection_busy=true;struct Guard{bool previous;~Guard(){selection_busy=false;busy=previous;}}guard{was_busy};if(!was_busy)error=false;Root pk(proto);
 bool result=fn(pk.get()?pk.get():proto,game_master,info);if(!result||!gate()||!(window.recent(now(),30000)||selection_window.recent(now(),30000)))return result;
 auto p=pokemon(pk.get());if(error){fault("selection-read");return result;}bool out=pgr_manual::disabled(result,gate(),p);if(out!=result)++selection_unlocked;return out;
}
void *arg(DobbyRegisterContext *ctx,unsigned index){if(!ctx)return nullptr;
#if defined(__aarch64__)
 return reinterpret_cast<void*>(index?ctx->general.regs.x1:ctx->general.regs.x0);
#elif defined(__x86_64__)
 return reinterpret_cast<void*>(index?ctx->general.regs.rsi:ctx->general.regs.rdi);
#else
 return nullptr;
#endif
}
// Entry instruments preserve every argument/result and leave the game's original execution intact.
void start_instrument(void*,DobbyRegisterContext *ctx){if(!arg(ctx,0)||!gate()||busy)return;drop();window.begin(now());event("manual-start");}
void init_instrument(void*,DobbyRegisterContext *ctx){
 if(owner!=uint64_t(gettid())||!ready||stopped)return;
 ++ui_generation;
 if(busy||!gate()||!window.recent(now(),20000)||window.consumed)return;
 void *dialog=arg(ctx,0),*data=arg(ctx,1);if(!exact(dialog,dialog_class)||!exact(data,data_class))return;
 busy=true;struct Guard{~Guard(){busy=false;}}guard;error=false;Root d(dialog),v(data);
 drop();if(d.get()&&v.get()&&window.capture(now())){pending_dialog=hold(d.get());pending_data=hold(v.get());pending_ui_generation=ui_generation;}
 if(error)fault("capture-root");
}
struct Elements {uintptr_t roots[16]{};unsigned count=0;~Elements(){for(auto &h:roots)release(h);}void *get(unsigned i)const{return i<count?target(roots[i]):nullptr;}};
bool elements(void *object,Elements &out,bool data_buttons){
 if(!object)return false;Root collection(object);Root array;
 int count=0;
 if(!collection.get())return false;
 void *actual=reinterpret_cast<void *(*)(void*)>(boot.runtime.object_get_class)(collection.get());
 const std::string name=type_name(reinterpret_cast<void *(*)(void*)>(boot.meta.class_get_type)(actual));
 const std::string expected="Niantic.Holoholo.UI.ModalButton[]";
 if(data_buttons&&name!=expected){
  void *items=field(actual,"_items",expected.c_str()),*size=field(actual,"_size","System.Int32");
  if(!items||!size)return false;
  array.h=hold(read<void*>(collection.get(),items));count=read<int>(collection.get(),size);
  if(!array.get())return false;
  void *ak=reinterpret_cast<void *(*)(void*)>(boot.runtime.object_get_class)(array.get());
  if(type_name(reinterpret_cast<void *(*)(void*)>(boot.meta.class_get_type)(ak))!=expected)return false;
 }else{
  if(name.size()<2||name.substr(name.size()-2)!="[]")return false;
  array.h=hold(collection.get());if(!array.get())return false;
  uintptr_t n=reinterpret_cast<uintptr_t(*)(void*)>(array_length)(array.get());if(n>16)return false;count=int(n);
 }
 if(error||!array.get()||count<0||count>16||uintptr_t(count)>reinterpret_cast<uintptr_t(*)(void*)>(array_length)(array.get()))return false;
 for(int i=0;i<count;++i){void *o=nullptr;std::memcpy(&o,static_cast<char*>(array.get())+32+i*sizeof(void*),sizeof(o));out.roots[i]=hold(o);if(!out.roots[i])return false;}
 out.count=unsigned(count);return !error;
}
bool prepare_warning(){
 if(!type_object||!array_length||!type_byref)return false;
 dialog_class=klass("Niantic.Holoholo.UI","WarningModalDialog","Niantic.Holoholo.UI.WarningModalDialog");
 modal_class=klass("Niantic.Holoholo.UI","ModalButton","Niantic.Holoholo.UI.ModalButton");
 button_class=klass("UnityEngine.UI","Button","UnityEngine.UI.Button");
 pokemon_class=klass("Holoholo.Rpc","PokemonProto","Holoholo.Rpc.PokemonProto");
 void *panel=klass("Niantic.Holoholo.UI.Pokemon","PokemonInfoPanel","Niantic.Holoholo.UI.Pokemon.PokemonInfoPanel");
 void *multi=klass("Niantic.Holoholo.UI","PokemonInventoryMultiSelect","Niantic.Holoholo.UI.PokemonInventoryMultiSelect");
 void *line=klass("Niantic.Holoholo.UI","PokemonInventoryListLine","Niantic.Holoholo.UI.PokemonInventoryListLine");
 void *component=klass("UnityEngine","Component","UnityEngine.Component"),*behaviour=klass("UnityEngine","Behaviour","UnityEngine.Behaviour");
 void *selectable=klass("UnityEngine.UI","Selectable","UnityEngine.UI.Selectable"),*object=klass("UnityEngine","Object","UnityEngine.Object");
 prepare_stage="dialog-classes";
 if(!dialog_class||!modal_class||!button_class||!pokemon_class||!component||!behaviour||!selectable||!object)return false;
 if(reinterpret_cast<bool(*)(void*)>(boot.meta.class_is_valuetype)(modal_class))return false;
 void *init=method(dialog_class,"Initialize",2,"System.Void");if(!init)return false;
 void *data_type=reinterpret_cast<void *(*)(void*,uint32_t)>(boot.meta.method_get_param)(init,0);if(!reference(data_type))return false;
 data_class=reinterpret_cast<void *(*)(void*)>(boot.meta.class_from_type)(data_type);
 prepare_stage="dialog-fields";
 buttons_field=field(data_class,"Buttons");bullets_field=field(data_class,"BulletListData");
 back_field=field(dialog_class,"backButton","UnityEngine.UI.Button");
 cancel_field=field(modal_class,"CancelButton","Niantic.Holoholo.UI.ModalButton",true);no_field=field(modal_class,"NoButton","Niantic.Holoholo.UI.ModalButton",true);
 if(!buttons_field||!bullets_field||!back_field||!cancel_field||!no_field)return false;
 prepare_stage="unity-button-methods";
 // Enumerate the non-generic (Type,bool) overload; count-only lookup can select a generic overload.
 void *child_iter=nullptr;
 for(unsigned i=0;i<512;++i){void *m=reinterpret_cast<void *(*)(void*,void**)>(boot.meta.class_get_methods)(component,&child_iter);if(!m)break;
  const char *n=reinterpret_cast<const char *(*)(void*)>(boot.meta.method_get_name)(m);uint32_t impl=0;
  if(!n||std::strcmp(n,"GetComponentsInChildren")||reinterpret_cast<bool(*)(void*)>(boot.meta.method_is_generic)(m)||
    (reinterpret_cast<uint32_t(*)(void*,uint32_t*)>(boot.meta.method_get_flags)(m,&impl)&0x10)||
    reinterpret_cast<uint32_t(*)(void*)>(boot.meta.method_get_param_count)(m)!=2)continue;
  if(type_name(reinterpret_cast<void *(*)(void*)>(boot.meta.method_get_return_type)(m))=="UnityEngine.Component[]"&&
   type_name(reinterpret_cast<void *(*)(void*,uint32_t)>(boot.meta.method_get_param)(m,0))=="System.Type"&&
   type_name(reinterpret_cast<void *(*)(void*,uint32_t)>(boot.meta.method_get_param)(m,1))=="System.Boolean"){children=m;break;}
 }
 on_click=method(button_class,"get_onClick",0);if(!on_click)return false;
 void *event_type=reinterpret_cast<void *(*)(void*)>(boot.meta.method_get_return_type)(const_cast<void*>(on_click));if(!reference(event_type))return false;
 fire=method(reinterpret_cast<void *(*)(void*)>(boot.meta.class_from_type)(event_type),"Invoke",0,"System.Void",nullptr,nullptr,true);
 enabled=method(behaviour,"get_isActiveAndEnabled",0,"System.Boolean");interactable=method(selectable,"get_interactable",0,"System.Boolean");
 // op_Implicit is static, so validate it separately through the metadata API.
 PgrMetaMethodQuery aq{};aq.version=1;aq.size=sizeof(aq);aq.owner=object;aq.name="op_Implicit";aq.return_type="System.Boolean";aq.return_match=1;aq.param_count=1;aq.param_types[0]="UnityEngine.Object";aq.param_match[0]=1;aq.is_static=1;
 PgrMetaMethodResult ar{};if(!md->validate_method(&aq,&ar))alive=ar.method;
 init_code=code(init);
 return children&&fire&&enabled&&interactable&&alive&&init_code;
}
bool prepare(){
 auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));if(!api||api->snapshot(&boot))return false;
 if(!reinterpret_cast<void*(*)()>(boot.runtime.thread_current)())return false;
 md=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));if(!md)return false;attempted=true;
 prepare_stage="pokemon-fields";
 string_length=symbol("il2cpp_string_length");type_byref=symbol("il2cpp_type_is_byref");
 type_object=symbol("il2cpp_type_get_object");array_length=symbol("il2cpp_array_length");
 pokemon_class=klass("Holoholo.Rpc","PokemonProto","Holoholo.Rpc.PokemonProto");
 void *panel=klass("Niantic.Holoholo.UI.Pokemon","PokemonInfoPanel","Niantic.Holoholo.UI.Pokemon.PokemonInfoPanel");
 void *multi=klass("Niantic.Holoholo.UI","PokemonInventoryMultiSelect","Niantic.Holoholo.UI.PokemonInventoryMultiSelect");
 void *line=klass("Niantic.Holoholo.UI","PokemonInventoryListLine","Niantic.Holoholo.UI.PokemonInventoryListLine");
 if(!pokemon_class||!string_length)return false;
 favorite=field(pokemon_class,"favorite_","System.Boolean");egg=field(pokemon_class,"isEgg_","System.Boolean");fort=field(pokemon_class,"deployedFortId_","System.String");
 if(!favorite||!egg||!fort){prepare_stage="pokemon-protection-fields";event("metadata-unavailable",!favorite?"favorite_":!egg?"isEgg_":"deployedFortId_");return false;}
 if(favorite&&egg&&fort){
  void *util=klass("Niantic.Holoholo.Assets.Character","PokemonUtilityService","Niantic.Holoholo.Assets.Character.PokemonUtilityService");
  unlock_code=code(method(util,"IsTransferable",2,"System.Boolean","Holoholo.Rpc.PokemonProto","System.Boolean"));
  void *iter=nullptr,*candidate=nullptr;unsigned count=0;bool ended=false;
  for(unsigned i=0;multi&&i<1024;++i){void *m=reinterpret_cast<void *(*)(void*,void**)>(boot.meta.class_get_methods)(multi,&iter);if(!m){ended=true;break;}
   uint32_t impl=0;uint32_t flags=reinterpret_cast<uint32_t(*)(void*,uint32_t*)>(boot.meta.method_get_flags)(m,&impl);
   if((flags&0x10)||(flags&7)==6||reinterpret_cast<bool(*)(void*)>(boot.meta.method_is_generic)(m)||reinterpret_cast<uint32_t(*)(void*)>(boot.meta.method_get_param_count)(m)!=1)continue;
   if(type_name(reinterpret_cast<void *(*)(void*)>(boot.meta.method_get_return_type)(m))=="System.Boolean"&&type_name(reinterpret_cast<void *(*)(void*,uint32_t)>(boot.meta.method_get_param)(m,0))=="Holoholo.Rpc.PokemonProto"){candidate=m;++count;}
  }
  if(!ended||count!=1)event("optional-hook-unavailable",!ended?"multi-enumeration-incomplete":count?"multi-filter-ambiguous":"multi-filter-not-found");
  if(ended&&count==1){const char *name=reinterpret_cast<const char *(*)(void*)>(boot.meta.method_get_name)(candidate);multi_code=code(method(multi,name,1,"System.Boolean","Holoholo.Rpc.PokemonProto"));}
  void *selection=method(pokemon_class,"IsSelectionDisabled",1,"System.Boolean");
  if(selection&&type_byref&&reference(reinterpret_cast<void *(*)(void*,uint32_t)>(boot.meta.method_get_param)(selection,0)))selection_code=code(selection);
 }

 if(multi_code==unlock_code)multi_code=nullptr;
 if(selection_code==unlock_code||selection_code==multi_code)selection_code=nullptr;
 prepare_stage="eligibility-hooks";
 std::lock_guard<std::mutex> lock(install_mutex);
 auto hook=[&](void *address,void *replacement,auto &slot,const char *name){
  if(!address){event("optional-hook-unavailable",name);return;}
  void *fn=nullptr;int rc=pgrazor_hook(address,replacement,&fn);
  if(fn)slot.store(reinterpret_cast<typename std::decay_t<decltype(slot)>::value_type>(fn),std::memory_order_release);
  event(rc||!fn?"optional-hook-unavailable":"hook-ready",name);
 };
 hook(unlock_code,reinterpret_cast<void*>(unlock_replacement),original_unlock,"IsTransferable");
 hook(multi_code,reinterpret_cast<void*>(multi_replacement),original_multi,"multi-select-filter");
 hook(selection_code,reinterpret_cast<void*>(selection_replacement),original_selection,"IsSelectionDisabled");
 ready=original_unlock.load()||original_multi.load()||original_selection.load();
 if(!ready)return false;
 // Entry instruments are optional. Multi-select establishes its own intent.
 start_codes[0]=code(method(panel,"TryTransferPokemon",2));start_codes[1]=code(method(multi,"ClickBatchRelease",0));start_codes[2]=code(method(line,"TransferAllSelected",1));
 for(unsigned i=0;i<3;++i)if(start_codes[i]){
  bool duplicate=false;for(unsigned j=0;j<i;++j)if(start_codes[i]==start_codes[j])duplicate=true;
  if(start_codes[i]==unlock_code||start_codes[i]==multi_code||start_codes[i]==selection_code)duplicate=true;
  if(!duplicate&&pgrazor_instrument(start_codes[i],start_instrument))event("entry-unavailable","manual-start");
 }
 bool modal=prepare_warning();
 if(modal&&init_code!=unlock_code&&init_code!=multi_code&&init_code!=selection_code){warning_ready=pgrazor_instrument(init_code,init_instrument)==0;}
 if(!warning_ready)event("warning-unavailable",prepare_stage);
 prepare_stage="ready";event("metadata-ready");
 status(warning_ready?"Ready: manual filters and special warning":"Ready: manual filters; confirm warnings manually");return true;
}
void tick_pending(){
 if(pending_ui_generation!=ui_generation){drop();event("left-open","dialog-reinitialized");return;}
 const uint64_t observed=ui_generation;
 Root dialog(target(pending_dialog)),data(target(pending_data));
 if(!exact(dialog.get(),dialog_class)||!exact(data.get(),data_class)||!live(dialog.get())||!boolean(invoke(enabled,dialog.get()))){drop();return;}
 if(!read<void*>(data.get(),bullets_field)){drop();event("left-open","ordinary-confirmation");return;}
 Elements buttons;if(!elements(read<void*>(data.get(),buttons_field),buttons,true)){drop();event("left-open","buttons-unreadable");return;}
 Root cancel(read_static(cancel_field)),no(read_static(no_field));unsigned mask=0;
 for(unsigned i=0;i<buttons.count;++i){if(!exact(buttons.get(i),modal_class)){drop();return;}if(buttons.get(i)==cancel.get()||buttons.get(i)==no.get())mask|=1u<<i;}
 Root type(reinterpret_cast<void *(*)(void*)>(type_object)(reinterpret_cast<void *(*)(void*)>(boot.meta.class_get_type)(button_class)));
 uint8_t include_inactive=0;void *args[]={type.get(),&include_inactive};Root found(invoke(children,dialog.get(),args));Elements ui;
 if(error||!found.get()||!elements(found.get(),ui,false)){drop();return;}
 Root back(read<void*>(dialog.get(),back_field));unsigned indices[16]{},n=0;
 for(unsigned i=0;i<ui.count;++i)if(ui.get(i)!=back.get())indices[n++]=i;
 const bool distinct=buttons.count==2&&buttons.get(0)&&buttons.get(1)&&buttons.get(0)!=buttons.get(1)&&n==2&&ui.get(indices[0])!=ui.get(indices[1]);
 const int chosen=pgr_manual::confirm(true,buttons.count,mask,n,distinct);
 if(chosen<0){drop();event("left-open","unexpected-buttons");return;}
 Root button(ui.get(indices[chosen]));
 if(!exact(button.get(),button_class)||!live(button.get())||!boolean(invoke(enabled,button.get()))||!boolean(invoke(interactable,button.get()))){drop();return;}
 Root action(invoke(on_click,button.get()));if(error||!action.get()||!gate()||observed!=ui_generation||!live(dialog.get())||!boolean(invoke(enabled,dialog.get()))||observed!=ui_generation){drop();return;}
 // Consume before invoking: callbacks/reentry can never confirm a second warning.
 window.consume();release(pending_dialog);release(pending_data);
 invoke(fire,action.get());if(!error){event("special-warning-confirmed");status("Special warning skipped; normal confirmation stays");}
}
}
void pgr_manual_transfer_stop(){stopped=true;}
void pgr_manual_transfer_tick(uint64_t time,uint64_t owner_tid,bool cleanup){
 if(owner_tid!=uint64_t(gettid())||busy)return;busy=true;struct Guard{~Guard(){busy=false;}}guard;
 tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));owner=owner_tid;
 if(cleanup||stopped){if(boot.runtime.gchandle_free)revoke();if(ready){ready=false;status("Stopped");}return;}
 if(!ready){if(attempted)return;if(!prepare()){if(attempted){event("metadata-unavailable",prepare_stage);char text[160];std::snprintf(text,sizeof(text),"Unavailable: manual transfer contract (%s)",prepare_stage);status(text);}return;}}
 // Off resets a managed failure, but never retries a partially installed metadata contract.
 int allowed=0;if(tr->get_bool(43,&allowed)||allowed!=1){revoke();if(paused.exchange(false))status("Ready: manual transfers only");return;}
 if(!gate()){revoke();return;}
 if(time<next)return;next=time+100;error=false;
 if(window.expired(time)){drop();event("left-open","expired");}
 if(warning_ready&&window.due(time))tick_pending();
 if(error)fault("warning-read-or-invoke");
 if(time>=report_at){report_at=time+30000;event("check-counts");}
}

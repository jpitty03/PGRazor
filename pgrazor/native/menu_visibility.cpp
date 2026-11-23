#include "menu_visibility.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "hook_engine.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <android/log.h>

namespace {
using Action=void(*)(void*,const void*);
Action original_open=nullptr,original_close=nullptr,original_initialize=nullptr;
Action original_dismiss=nullptr;
std::atomic<bool> menu_open{false};
bool attempted=false,ready=false;
uint64_t last_emit=0;
const void *getter=nullptr;
const void *top_getter=nullptr,*visible_getter=nullptr,*open_getter=nullptr;
// One bounded managed root; replaced on a new map and released on map dismissal.
// The encounter host has already validated the pointer-sized GC handle ABI.
void *controller_handle=nullptr;
void *(*gc_new)(void*,int)=nullptr;
void *(*gc_target)(void*)=nullptr;
void (*gc_free)(void*)=nullptr;
void *(*invoke)(const void*,void*,void**,void**)=nullptr;
void *(*unbox)(void*)=nullptr;
bool read_bool(const void *method,void *self,bool &out){
 void *exception=nullptr;
 void *boxed=invoke(method,self,nullptr,&exception);
 void *value=(!exception&&boxed)?unbox(boxed):nullptr;
 if(!value)return false;
 out=*static_cast<bool*>(value);return true;
}
void retain_controller(void *self){
 if(controller_handle&&gc_target(controller_handle)==self)return;
 if(controller_handle)gc_free(controller_handle);
 controller_handle=self?gc_new(self,0):nullptr;
}
bool map_controls_on_top(){
 void *self=controller_handle?gc_target(controller_handle):nullptr;
 bool top=false,visible=false,opened=false;
 return self&&read_bool(top_getter,self,top)&&top&&
  read_bool(visible_getter,self,visible)&&visible&&
  read_bool(open_getter,self,opened)&&opened;
}
void observe(void *self){
 void *exception=nullptr;
 void *boxed=invoke(getter,self,nullptr,&exception);
 // A failed observation hides the joystick until a later confirmed transition.
 void *value=(!exception&&boxed)?unbox(boxed):nullptr;
 menu_open.store(!value||*static_cast<bool*>(value),std::memory_order_release);
}
void open_menu(void *self,const void *method){
 menu_open.store(true,std::memory_order_release);
 original_open(self,method);
 if(self)observe(self);
}
void close_menu(void *self,const void *method){
 original_close(self,method);
 if(self)observe(self);
}
void initialize_menu(void *self,const void *method){
 original_initialize(self,method);
 if(self){retain_controller(self);observe(self);}
}
void dismiss_menu(void *self,const void *method){
 if(controller_handle&&gc_target(controller_handle)==self)retain_controller(nullptr);
 original_dismiss(self,method);
}
bool resolve_method(const PgrMetaApi *api,const void *owner,const char *name,const char *ret,PgrMetaMethodResult &out){
 PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=owner;q.name=name;
 q.return_type=ret;q.return_match=PGR_MD_MATCH_EXACT;q.param_count=0;
 q.is_static=0;
 return api->validate_method(&q,&out)==0;
}
void install(){
 const auto *boot=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));
 PgrBootBinding b{};b.version=1;b.size=sizeof(b);
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));
 if(!boot||!api||boot->snapshot(&b)!=0)return;
 attempted=true;
 PgrMetaClassQuery cq{1,sizeof(cq),"Niantic.Holoholo.Map.UI","MainMenuGuiController",nullptr,PGR_MD_MATCH_EXACT,0};
 PgrMetaClassResult cls{},base{};PgrMetaMethodResult op{},cl{},init{},get{},dismiss{},top{},vis{},opened{};
 PgrMetaClassQuery bq{1,sizeof(bq),"Niantic.Holoholo.NUI","GuiController",nullptr,PGR_MD_MATCH_EXACT,0};
 bool ok=api->resolve_class(&cq,&cls)==0&&
  api->resolve_class(&bq,&base)==0&&
  resolve_method(api,base.klass,"get_IsTopGui","System.Boolean",top)&&
  resolve_method(api,base.klass,"get_IsVisible","System.Boolean",vis)&&
  resolve_method(api,base.klass,"get_IsOpen","System.Boolean",opened)&&
  resolve_method(api,cls.klass,"OpenMenu","System.Void",op)&&
  resolve_method(api,cls.klass,"CloseMenu","System.Void",cl)&&
  resolve_method(api,cls.klass,"HandleOpen","System.Void",init)&&
  resolve_method(api,cls.klass,"HandleDismiss","System.Void",dismiss)&&
  resolve_method(api,cls.klass,"get_MenuOpen","System.Boolean",get);
 void *op_code=nullptr,*cl_code=nullptr,*init_code=nullptr,*dismiss_code=nullptr;
 if(ok){
  memcpy(&op_code,op.method,sizeof(void*));memcpy(&cl_code,cl.method,sizeof(void*));
  memcpy(&init_code,init.method,sizeof(void*));
  memcpy(&dismiss_code,dismiss.method,sizeof(void*));
  PgrMetaCodeQuery a{1,sizeof(a),op_code,16,nullptr,0,0},c{1,sizeof(c),cl_code,16,nullptr,0,0},i{1,sizeof(i),init_code,16,nullptr,0,0};
  PgrMetaCodeQuery d{1,sizeof(d),dismiss_code,16,nullptr,0,0};
  ok=api->validate_code(&a)==0&&api->validate_code(&c)==0&&api->validate_code(&i)==0&&api->validate_code(&d)==0;
 }
 if(ok){
  getter=get.method;invoke=reinterpret_cast<decltype(invoke)>(b.runtime.runtime_invoke);
  unbox=reinterpret_cast<decltype(unbox)>(b.runtime.object_unbox);
  top_getter=top.method;visible_getter=vis.method;open_getter=opened.method;
  gc_new=reinterpret_cast<decltype(gc_new)>(b.runtime.gchandle_new);
  gc_target=reinterpret_cast<decltype(gc_target)>(b.runtime.gchandle_get_target);
  gc_free=reinterpret_cast<decltype(gc_free)>(b.runtime.gchandle_free);
  ok=invoke&&unbox&&gc_new&&gc_target&&gc_free;
  if(ok)ok=pgrazor_hook(dismiss_code,reinterpret_cast<void*>(dismiss_menu),reinterpret_cast<void**>(&original_dismiss))==0&&original_dismiss;
  if(ok)ok=pgrazor_hook(cl_code,reinterpret_cast<void*>(close_menu),reinterpret_cast<void**>(&original_close))==0&&original_close;
  if(ok)ok=pgrazor_hook(init_code,reinterpret_cast<void*>(initialize_menu),reinterpret_cast<void**>(&original_initialize))==0&&original_initialize;
  if(ok)ok=pgrazor_hook(op_code,reinterpret_cast<void*>(open_menu),reinterpret_cast<void**>(&original_open))==0&&original_open;
 }
 ready=ok;
 __android_log_print(ok?ANDROID_LOG_INFO:ANDROID_LOG_WARN,"PGRazor","menu-visibility: %s",ok?"installed":"unsupported; joystick hidden");
}
}
void pgr_menu_visibility_tick(uint64_t now_ms){
 if(!attempted)install();
 if(now_ms-last_emit<200)return;
 const auto *tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));
 if(!tr)return;
 char detail[64];snprintf(detail,sizeof(detail),"%llu|%d",static_cast<unsigned long long>(now_ms),ready&&!menu_open.load(std::memory_order_acquire)&&map_controls_on_top()?1:0);
 PgrRuntimeTransportString strings[]={{"joystick-menu",13},{detail,uint32_t(strlen(detail))}};
 if(tr->emit(19,nullptr,0,strings,2)==0)last_emit=now_ms;
}

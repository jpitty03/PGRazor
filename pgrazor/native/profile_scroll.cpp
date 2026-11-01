#include "profile_scroll.h"
#include "runtime_bootstrap.h"
#include "hook_engine.h"
#include <android/log.h>
#include <cstdint>
#include <cstring>

namespace {
// IL2CPP PlayerInfo is a 32-byte value: three references and an Int64.
// Use a value aggregate so both AArch64's indirect and x86_64's stack ABI apply.
struct PlayerInfo { void *player, *stats, *logins; int64_t day; };
static_assert(sizeof(PlayerInfo)==32,"PlayerInfo ABI");
using Refresh = void (*)(void*,PlayerInfo,const void*);
using SetPosition = void (*)(void*,float,int,const void*);
Refresh original_refresh=nullptr;
SetPosition original_position=nullptr;
void (*read_field)(void*,void*,void*)=nullptr;
const void *scroll_field=nullptr;
thread_local void *refresh_scroll=nullptr;
bool installed=false;

void refresh(void *self,PlayerInfo info,const void *method){
 void *scroll=nullptr;
 if(self)read_field(self,const_cast<void*>(scroll_field),&scroll);
 struct Scope {void *previous; Scope(void *s):previous(refresh_scroll){refresh_scroll=s;} ~Scope(){refresh_scroll=previous;}} scope(scroll);
 // Keep every normal player-stat, XP, inventory and widget update.
 original_refresh(self,info,method);
}
void set_position(void *self,float value,int axis,const void *method){
 // Only the current profile's synchronous player-data callback may suppress
 // its vertical reset. Initial Reinitialize, other lists, dragging and all
 // other positions still use the game's original implementation.
 if(self&&self==refresh_scroll&&axis==1&&value==1.0f)return;
 original_position(self,value,axis,method);
}
const void *resolve(const PgrMetaApi *api,const char *ns,const char *name){
 PgrMetaClassQuery q{1,sizeof(q),ns,name,nullptr,PGR_MD_MATCH_EXACT,0};
 PgrMetaClassResult out{};
 return api->resolve_class(&q,&out)==0?out.klass:nullptr;
}
bool method(const PgrMetaApi *api,const void *owner,const char *name,const char *p0,const char *p1,PgrMetaMethodResult &out){
 if(!owner)return false;
 PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=owner;q.name=name;
 q.return_type="System.Void";q.return_match=PGR_MD_MATCH_EXACT;
 q.param_count=p1?2:1;q.param_types[0]=p0;q.param_match[0]=p1?PGR_MD_MATCH_EXACT:PGR_MD_MATCH_SUFFIX;
 if(p1){q.param_types[1]=p1;q.param_match[1]=PGR_MD_MATCH_EXACT;}
 return api->validate_method(&q,&out)==0;
}
}
void pgr_profile_scroll_install(){
 if(installed)return;
 const auto *bootstrap=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));
 PgrBootBinding b{};b.version=1;b.size=sizeof(b);
 if(!bootstrap||bootstrap->snapshot(&b)!=0)return;
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));
 if(!api)return;
 installed=true; // Optional: unsupported metadata must not disable gameplay.
 const void *page=resolve(api,"Niantic.Holoholo.UI","PlayerProfilePage");
 const void *base=resolve(api,"Niantic.Holoholo.UI","GUISubPageBase");
 const void *scroll=resolve(api,"Niantic.Holoholo.Widget","ScrollRectBase");
 PgrMetaMethodResult update{},position{};
 PgrMetaFieldQuery fq{1,sizeof(fq),base,"scrollRect",0x70,0x50,0,PGR_MD_FIELD_REF,0,"Niantic.Holoholo.Widget.ScrollRectEx",PGR_MD_MATCH_EXACT,0};
 PgrMetaFieldResult field{};
 bool ok=page&&base&&scroll&&api->validate_field(&fq,&field)==0&&
  method(api,page,"ccyf","PlayerInfo",nullptr,update)&&method(api,scroll,"bega","System.Single","System.Int32",position);
 if(ok){
  auto param=reinterpret_cast<void*(*)(const void*,uint32_t)>(b.meta.method_get_param)(update.method,0);
  auto klass=reinterpret_cast<void*(*)(void*)>(b.meta.class_from_type)(param);
  int align=0;
  ok=klass&&reinterpret_cast<int(*)(void*,int*)>(b.meta.class_value_size)(klass,&align)==32&&align==8;
  const char *names[]={"<PlayerProto>k__BackingField","<PlayerStats>k__BackingField","cuzr","<PlayerDay>k__BackingField"};
  for(unsigned i=0;ok&&i<4;i++){
   PgrMetaFieldQuery q{1,sizeof(q),klass,names[i],16+i*8,0x50,0,i==3?PGR_MD_FIELD_VALUE:PGR_MD_FIELD_REF,i==3?8u:0u,i==3?"System.Int64":nullptr,i==3?PGR_MD_MATCH_EXACT:PGR_MD_MATCH_ANY,0};
   PgrMetaFieldResult out{};ok=api->validate_field(&q,&out)==0;
  }
 }
 void *update_code=nullptr,*position_code=nullptr;
 if(ok){
  memcpy(&update_code,update.method,sizeof(void*));memcpy(&position_code,position.method,sizeof(void*));
  PgrMetaCodeQuery a{1,sizeof(a),update_code,16,nullptr,0,0},c{1,sizeof(c),position_code,16,nullptr,0,0};
  ok=api->validate_code(&a)==0&&api->validate_code(&c)==0;
 }
 if(ok){
  read_field=reinterpret_cast<decltype(read_field)>(b.runtime.field_get_value);scroll_field=field.field;
  // Install the inert position wrapper first; a failed second install leaves
  // its scope null and forwards all calls unchanged. Never tear down trampolines.
  ok=pgrazor_hook(position_code,reinterpret_cast<void*>(set_position),reinterpret_cast<void**>(&original_position))==0&&original_position;
  if(ok)ok=pgrazor_hook(update_code,reinterpret_cast<void*>(refresh),reinterpret_cast<void**>(&original_refresh))==0&&original_refresh;
 }
 __android_log_print(ok?ANDROID_LOG_INFO:ANDROID_LOG_WARN,"PGRazor","profile-scroll: %s",ok?"installed":"unsupported; original behavior retained");
}

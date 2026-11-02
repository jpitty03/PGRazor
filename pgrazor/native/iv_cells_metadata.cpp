#include "pvp_metadata.h"
#include <cstring>

bool pgr_resolve_iv_cells(const PgrMetaExports &e,const PgrMetaApi &md,void *byref,PgrIvCellConfig &out){
 out={};const void *required[]={e.class_get_field_from_name,e.field_get_parent,e.field_get_flags,e.field_get_type,
  e.type_get_name,e.mem_free,e.class_from_type,e.class_is_valuetype,e.class_get_method_from_name,e.method_get_param,
  e.debug_get_method_info,byref};
 for(auto p:required)if(!p)return false;if(!md.resolve_class||!md.validate_method||!md.validate_code)return false;
 auto typename_is=[&](void *type,const char *expected){if(!type)return false;char *s=reinterpret_cast<char *(*)(void*)>(e.type_get_name)(type);
  if(!s)return false;bool ok=std::strcmp(s,expected)==0;reinterpret_cast<void(*)(void*)>(e.mem_free)(s);return ok;};
 auto field=[&](void *owner,const char *name,const char *expected){
  void *f=reinterpret_cast<void *(*)(void*,const char*)>(e.class_get_field_from_name)(owner,name);
  if(!f||reinterpret_cast<void *(*)(void*)>(e.field_get_parent)(f)!=owner||
    (reinterpret_cast<uint32_t(*)(void*)>(e.field_get_flags)(f)&0x50))return static_cast<void*>(nullptr);
  void *type=reinterpret_cast<void *(*)(void*)>(e.field_get_type)(f);
  return typename_is(type,expected)?f:nullptr;
 };
 PgrMetaClassQuery cq{};cq.version=1;cq.size=sizeof(cq);cq.name_space="Niantic.Holoholo.UI";cq.name="PokemonInventoryCellView";
 cq.canonical="Niantic.Holoholo.UI.PokemonInventoryCellView";cq.canonical_match=PGR_MD_MATCH_EXACT;PgrMetaClassResult cr{};
 if(md.resolve_class(&cq,&cr)||!cr.klass)return false;PgrIvCellConfig c{1,sizeof(c)};c.cell_class=const_cast<void*>(cr.klass);
 c.name_text=field(c.cell_class,"nameText","UnityEngine.UI.Text");if(!c.name_text)return false;
 void *method=reinterpret_cast<void *(*)(void*,const char*,int)>(e.class_get_method_from_name)(c.cell_class,"Initialize",1);if(!method)return false;
 void *type=reinterpret_cast<void *(*)(void*,uint32_t)>(e.method_get_param)(method,0);if(!type||reinterpret_cast<bool(*)(void*)>(byref)(type))return false;
 c.initializer_class=reinterpret_cast<void *(*)(void*)>(e.class_from_type)(type);
 if(!c.initializer_class||reinterpret_cast<bool(*)(void*)>(e.class_is_valuetype)(c.initializer_class))return false;
 c.pokemon_proto=field(c.initializer_class,"PokemonProto","Holoholo.Rpc.PokemonProto");if(!c.pokemon_proto)return false;
 char *param_name=reinterpret_cast<char *(*)(void*)>(e.type_get_name)(type);if(!param_name)return false;
 PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=c.cell_class;q.name="Initialize";
 q.return_type="System.Void";q.return_match=PGR_MD_MATCH_EXACT;q.param_count=1;q.param_types[0]=param_name;q.param_match[0]=PGR_MD_MATCH_EXACT;q.is_static=0;
 PgrMetaMethodResult mr{};int rc=md.validate_method(&q,&mr);reinterpret_cast<void(*)(void*)>(e.mem_free)(param_name);
 if(rc||mr.method!=method)return false;
 uint64_t info[3]{};if(!reinterpret_cast<bool(*)(void*,void*)>(e.debug_get_method_info)(method,info)||!info[0])return false;
 c.initialize_code=reinterpret_cast<void*>(info[0]);PgrMetaCodeQuery code{};code.version=1;code.size=sizeof(code);code.address=c.initialize_code;code.length=24;
 if(md.validate_code(&code))return false;out=c;return true;
}

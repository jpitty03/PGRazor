#include "pvp_metadata.h"
#include <cstring>

namespace {
struct Reflect {
 const PgrMetaExports &e;
 void *field(void *owner,const char *name)const {
  void *f=reinterpret_cast<void *(*)(void*,const char*)>(e.class_get_field_from_name)(owner,name);
  if(!f||reinterpret_cast<void *(*)(void*)>(e.field_get_parent)(f)!=owner||
     (reinterpret_cast<uint32_t(*)(void*)>(e.field_get_flags)(f)&0x10))return nullptr;
  return f;
 }
 bool type(void *t,const char *expected)const {
  if(!t)return false;char *n=reinterpret_cast<char *(*)(void*)>(e.type_get_name)(t);
  if(!n)return false;const bool ok=std::strcmp(n,expected)==0;reinterpret_cast<void(*)(void*)>(e.mem_free)(n);return ok;
 }
 bool int32(void *t)const {
  if(type(t,"System.Int32"))return true;
  void *k=t?reinterpret_cast<void *(*)(void*)>(e.class_from_type)(t):nullptr;
  if(!k||!reinterpret_cast<bool(*)(void*)>(e.class_is_enum)(k))return false;
  int alignment=0;const int size=reinterpret_cast<int(*)(void*,int*)>(e.class_value_size)(k,&alignment);
  return size==4&&alignment==4&&type(reinterpret_cast<void *(*)(void*)>(e.class_enum_basetype)(k),"System.Int32");
 }
 void *integer_field(void *owner,const char *name)const {
  void *f=field(owner,name);return f&&int32(reinterpret_cast<void *(*)(void*)>(e.field_get_type)(f))?f:nullptr;
 }
};
}
bool pgr_resolve_pvp_metadata(const PgrMetaExports &e,const PgrMetaApi &md,void *static_reader,
 void *pokemon,void *text,PgrIvPvpConfig &out){
 out={};
 const void *required[]={e.class_get_field_from_name,e.field_get_parent,e.field_get_flags,e.field_get_type,
  e.class_from_type,e.class_is_valuetype,e.class_is_enum,e.class_enum_basetype,e.class_value_size,
  e.type_get_name,e.mem_free,static_reader,pokemon,text};
 for(auto p:required)if(!p)return false;
 if(!md.resolve_class||!md.validate_method)return false;
 Reflect r{e};PgrIvPvpConfig c{};c.version=1;c.size=sizeof(c);
 c.species=r.integer_field(pokemon,"pokemonId_");c.display=r.field(pokemon,"pokemonDisplay_");
 if(!c.species||!c.display)return false;
 c.display_class=reinterpret_cast<void *(*)(void*)>(e.class_from_type)(reinterpret_cast<void *(*)(void*)>(e.field_get_type)(c.display));
 if(!c.display_class||reinterpret_cast<bool(*)(void*)>(e.class_is_valuetype)(c.display_class))return false;
 c.form=r.integer_field(c.display_class,"Form");
 if(!c.form)c.form=r.integer_field(c.display_class,"form_");
 if(!c.form)return false;
 PgrMetaClassQuery cq{};cq.version=1;cq.size=sizeof(cq);cq.name_space="UnityEngine";cq.name="VerticalWrapMode";
 cq.canonical="UnityEngine.VerticalWrapMode";cq.canonical_match=PGR_MD_MATCH_EXACT;PgrMetaClassResult cr{};
 if(md.resolve_class(&cq,&cr)||!cr.klass)return false;c.vertical_class=const_cast<void*>(cr.klass);
 if(!reinterpret_cast<bool(*)(void*)>(e.class_is_enum)(c.vertical_class)||
    !r.type(reinterpret_cast<void *(*)(void*)>(e.class_enum_basetype)(c.vertical_class),"System.Int32"))return false;
 int alignment=0;if(reinterpret_cast<int(*)(void*,int*)>(e.class_value_size)(c.vertical_class,&alignment)!=4||alignment!=4)return false;
 void *literal=reinterpret_cast<void *(*)(void*,const char*)>(e.class_get_field_from_name)(c.vertical_class,"Overflow");
 if(!literal||reinterpret_cast<void *(*)(void*)>(e.field_get_parent)(literal)!=c.vertical_class||
    (reinterpret_cast<uint32_t(*)(void*)>(e.field_get_flags)(literal)&0x8050)!=0x8050)return false;
 if(!r.type(reinterpret_cast<void *(*)(void*)>(e.field_get_type)(literal),"UnityEngine.VerticalWrapMode"))return false;
 reinterpret_cast<void(*)(void*,void*)>(static_reader)(literal,&c.vertical_overflow);
 for(int i=0;i<2;++i){PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=text;
  q.name=i?"set_verticalOverflow":"get_verticalOverflow";q.return_type=i?"System.Void":"UnityEngine.VerticalWrapMode";
  q.return_match=PGR_MD_MATCH_EXACT;q.param_count=i;q.is_static=0;
  if(i){q.param_types[0]="UnityEngine.VerticalWrapMode";q.param_match[0]=PGR_MD_MATCH_EXACT;}
  PgrMetaMethodResult result{};if(md.validate_method(&q,&result)||!result.method)return false;
  (i?c.set_vertical:c.get_vertical)=const_cast<void*>(result.method);
 }
 out=c;return true;
}

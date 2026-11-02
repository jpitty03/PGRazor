// Read-only reflection fixture: optional failure must never expose a partial config.
#include "pvp_metadata.cpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace fixture {
int pk,text,display,integer,form,vertical,method_get,method_set;
int species_field,display_field,form_field,public_form_field,overflow_field;
bool public_form=false,legacy_form=true,bad_public_form=false;
bool absent=false,wrong_parent=false,static_species=false,bad_enum=false,bad_size=false,
 display_value=false,no_vertical=false,bad_method=false,bad_literal=false;
int allocated=0;
void *find(void *owner,const char *name){
 if(owner==&pk&&std::strcmp(name,"pokemonId_")==0)return absent?nullptr:&species_field;
 if(owner==&pk&&std::strcmp(name,"pokemonDisplay_")==0)return &display_field;
 if(owner==&display&&std::strcmp(name,"Form")==0)return public_form?&public_form_field:nullptr;
 if(owner==&display&&std::strcmp(name,"form_")==0)return legacy_form?&form_field:nullptr;
 if(owner==&vertical&&std::strcmp(name,"Overflow")==0)return &overflow_field;return nullptr;
}
void *parent(void *f){if(wrong_parent)return &text;return (f==&form_field||f==&public_form_field)?&display:f==&overflow_field?&vertical:&pk;}
uint32_t flags(void *f){return f==&public_form_field&&bad_public_form?0x10:f==&overflow_field?(bad_literal?0:0x8050):f==&species_field&&static_species?0x10:0;}
void *field_type(void *f){return f==&species_field?&integer:f==&display_field?&display:(f==&form_field||f==&public_form_field)?&form:&vertical;}
void *identity(void *t){return t;}
bool valuetype(void *k){return k==&display?display_value:true;}
bool enumeration(void *k){return k==&form||k==&vertical;}
void *base(void*){return bad_enum?&display:&integer;}
int size(void*,int *align){*align=4;return bad_size?8:4;}
char *type_name(void *t){const char *name=t==&integer?"System.Int32":t==&vertical?"UnityEngine.VerticalWrapMode":t==&form?"Holoholo.Rpc.Form":"Holoholo.Rpc.PokemonDisplayProto";
 size_t n=std::strlen(name)+1;char *copy=static_cast<char*>(std::malloc(n));std::memcpy(copy,name,n);++allocated;return copy;}
void release(void *p){--allocated;std::free(p);}
void read_static(void *f,void *value){assert(f==&overflow_field);*static_cast<int*>(value)=1;}
int resolve(const PgrMetaClassQuery *q,PgrMetaClassResult *out){assert(!std::strcmp(q->canonical,"UnityEngine.VerticalWrapMode"));out->klass=&vertical;return no_vertical?-9:0;}
int method(const PgrMetaMethodQuery *q,PgrMetaMethodResult *out){assert(q->owner==&text&&q->param_count<=1&&q->is_static==0);
 if(q->param_count){assert(!std::strcmp(q->param_types[0],"UnityEngine.VerticalWrapMode"));out->method=&method_set;}
 else out->method=&method_get;return bad_method?-16:0;}
}
int main(){using namespace fixture;
 PgrMetaExports e{};e.class_get_field_from_name=reinterpret_cast<void*>(find);e.field_get_parent=reinterpret_cast<void*>(parent);e.field_get_flags=reinterpret_cast<void*>(flags);
 e.field_get_type=reinterpret_cast<void*>(field_type);e.class_from_type=reinterpret_cast<void*>(identity);e.class_is_valuetype=reinterpret_cast<void*>(valuetype);
 e.class_is_enum=reinterpret_cast<void*>(enumeration);e.class_enum_basetype=reinterpret_cast<void*>(base);e.class_value_size=reinterpret_cast<void*>(size);
 e.type_get_name=reinterpret_cast<void*>(type_name);e.mem_free=reinterpret_cast<void*>(release);
 PgrMetaApi md{};md.resolve_class=resolve;md.validate_method=method;PgrIvPvpConfig out{};
 auto check=[&](){return pgr_resolve_pvp_metadata(e,md,reinterpret_cast<void*>(read_static),&pk,&text,out);};
 assert(check()&&out.species==&species_field&&out.form==&form_field&&out.display_class==&display&&out.get_vertical==&method_get&&out.set_vertical==&method_set&&out.vertical_overflow==1&&!allocated);
 for(bool *fault:{&absent,&wrong_parent,&static_species,&bad_enum,&bad_size,&display_value,&no_vertical,&bad_method,&bad_literal}){
  *fault=true;assert(!check()&&!out.species&&!out.get_vertical&&!allocated);*fault=false;assert(check()&&!allocated);
 }
 public_form=true;legacy_form=false;assert(check()&&out.form==&public_form_field&&!allocated);
 legacy_form=true;assert(check()&&out.form==&public_form_field&&!allocated);
 bad_public_form=true;assert(check()&&out.form==&form_field&&!allocated);
 legacy_form=false;assert(!check()&&!out.species&&!allocated);
 bad_public_form=false;public_form=false;assert(!check()&&!out.species&&!allocated);
 legacy_form=true;assert(check()&&out.form==&form_field&&!allocated);
 e.mem_free=nullptr;assert(!check()&&!out.species);
 std::puts("PASS PvP optional metadata: owner/flags/int32/reference/enum size/method/literal checks; no partial output or leaked type names");return 0;
}

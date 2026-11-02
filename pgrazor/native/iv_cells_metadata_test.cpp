#include "iv_cells_metadata.cpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
namespace fixture {
int cell,init,text,pokemon,name_field,proto_field,method;
alignas(4) unsigned char code_bytes[32]{};
bool no_class=false,no_field=false,wrong_parent=false,static_field=false,bad_type=false,no_method=false,
 init_value=false,init_byref=false,invalid_signature=false,bad_debug=false,bad_code=false;
int allocated=0;
void *field(void *owner,const char *name){
 if(no_field)return nullptr;
 if(owner==&cell&&!std::strcmp(name,"nameText"))return &name_field;
 if(owner==&init&&!std::strcmp(name,"PokemonProto"))return &proto_field;return nullptr;
}
void *parent(void *f){return wrong_parent?&text:f==&name_field?&cell:&init;}
uint32_t flags(void*){return static_field?0x10:0;}
void *type(void *f){return f==&name_field?&text:&pokemon;}
char *name(void *t){const char *s=bad_type?"System.Int32":t==&text?"UnityEngine.UI.Text":t==&pokemon?"Holoholo.Rpc.PokemonProto":"Niantic.Holoholo.UI.PokemonInventoryCellView.Initializer";
 size_t n=std::strlen(s)+1;char *out=static_cast<char*>(std::malloc(n));std::memcpy(out,s,n);++allocated;return out;}
void free_name(void *p){--allocated;std::free(p);}
void *identity(void *t){return t;}
bool value(void*){return init_value;}
void *lookup_method(void *owner,const char *n,int count){assert(owner==&cell&&!std::strcmp(n,"Initialize")&&count==1);return no_method?nullptr:&method;}
void *param(void *m,uint32_t index){assert(m==&method&&index==0);return &init;}
bool byref(void*){return init_byref;}
bool debug(void *m,void *out){assert(m==&method);*static_cast<uint64_t*>(out)=reinterpret_cast<uint64_t>(code_bytes);return !bad_debug;}
int resolve(const PgrMetaClassQuery *q,PgrMetaClassResult *out){assert(!std::strcmp(q->canonical,"Niantic.Holoholo.UI.PokemonInventoryCellView"));out->klass=&cell;return no_class?-9:0;}
int validate(const PgrMetaMethodQuery *q,PgrMetaMethodResult *out){assert(q->owner==&cell&&!std::strcmp(q->name,"Initialize")&&!std::strcmp(q->return_type,"System.Void")&&q->param_count==1&&q->is_static==0&&q->allow_inflated==0);
 assert(!std::strcmp(q->param_types[0],"Niantic.Holoholo.UI.PokemonInventoryCellView.Initializer"));out->method=&method;return invalid_signature?-16:0;}
int executable(const PgrMetaCodeQuery *q){assert(q->address==code_bytes&&q->length==24);return bad_code?-23:0;}
}
int main(){using namespace fixture;PgrMetaExports e{};
 e.class_get_field_from_name=reinterpret_cast<void*>(field);e.field_get_parent=reinterpret_cast<void*>(parent);e.field_get_flags=reinterpret_cast<void*>(flags);e.field_get_type=reinterpret_cast<void*>(type);
 e.type_get_name=reinterpret_cast<void*>(name);e.mem_free=reinterpret_cast<void*>(free_name);e.class_from_type=reinterpret_cast<void*>(identity);e.class_is_valuetype=reinterpret_cast<void*>(value);
 e.class_get_method_from_name=reinterpret_cast<void*>(lookup_method);e.method_get_param=reinterpret_cast<void*>(param);e.debug_get_method_info=reinterpret_cast<void*>(debug);
 PgrMetaApi md{};md.resolve_class=resolve;md.validate_method=validate;md.validate_code=executable;PgrIvCellConfig out{};
 auto check=[&](){return pgr_resolve_iv_cells(e,md,reinterpret_cast<void*>(byref),out);};
 assert(check()&&out.cell_class==&cell&&out.initializer_class==&init&&out.name_text==&name_field&&out.pokemon_proto==&proto_field&&out.initialize_code==code_bytes&&!allocated);
 for(bool *fault:{&no_class,&no_field,&wrong_parent,&static_field,&bad_type,&no_method,&init_value,&init_byref,&invalid_signature,&bad_debug,&bad_code}){
  *fault=true;assert(!check()&&!out.cell_class&&!out.initialize_code&&!allocated);*fault=false;assert(check()&&!allocated);
 }
 e.mem_free=nullptr;assert(!check()&&!out.cell_class);
 std::puts("PASS cell metadata: named fields, ownership/static/type checks, reference initializer, signature, executable address and no leaked/partial metadata");return 0;
}

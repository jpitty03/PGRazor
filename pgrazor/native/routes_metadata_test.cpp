// Actual production dlopen/query53 fixture. Metadata exports are fake; the
// compiled Routes descriptor resolver and shared feature47 validators are REAL.
// Sparse virtual module reservation touches only the known confirmation prefix
// page. Injected segment permissions are synthetic, not a /proc/Houdini test.
#include "routes_metadata.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using namespace pgr_routes_schema;
struct Class;
struct Type {std::string name;Class *klass=nullptr;};
struct Class {Type *type=nullptr;bool value=false,enumeration=false;Type *base=nullptr;int size=0,align=0;};
struct Field {Class *lookup,*owner;std::string name;Type *type;uint32_t flags;uint64_t offset;};
struct Method {
 void *code; // MethodInfo's first pointer is independently read for Dismiss.
 Class *lookup,*owner;std::string name;Type *result,*param;uint32_t count,flags=0;
 bool generic=false,inflated=false;
};
static_assert(offsetof(Method,code)==0,"fixture MethodInfo code pointer");
std::vector<std::unique_ptr<Type>> types;
std::vector<std::unique_ptr<Class>> extra_classes;
std::vector<std::unique_ptr<Field>> fields;
std::vector<std::unique_ptr<Method>> methods;
Class classes[kClassCount];
Method *extra_methods[5]{};
Class *list_class,*waypoint_list_class,*duplicate_class;
Type *int32_type,*void_type,*generic_ref_type;
int domain_token,image_a,image_b,thread_token;
void *assemblies[]={&image_a,&image_b};
bool attached=true,duplicate=false,reenter=false;
unsigned thread_checks=0,detach_at=0,names_allocated=0,names_freed=0,reentries=0;
unsigned char *module_base=nullptr;uint64_t module_size=0;
const PgrRoutesMetadataApi *api=nullptr;
PgrRoutesMetadataConfig config{};

Type *new_type(const std::string &name,Class *klass){types.emplace_back(new Type{name,klass});return types.back().get();}
Class *new_class(const std::string &name,bool value=false,int size=0,int align=0){
 extra_classes.emplace_back(new Class);Class *klass=extra_classes.back().get();
 klass->type=new_type(name,klass);klass->value=value;klass->size=size;klass->align=align;return klass;
}
Type *type(const char *name){
 assert(name);for(const auto &candidate:types)if(candidate->name==name)return candidate.get();
 return new_class(name)->type;
}
Method *new_method(Class *owner,const char *name,Type *result,Type *param=nullptr,bool inflated=false){
 methods.emplace_back(new Method{nullptr,owner,owner,name,result,param,uint32_t(param?1:0),0,false,inflated});
 return methods.back().get();
}
bool zero(const PgrRoutesMetadataOutput &out){PgrRoutesMetadataOutput empty{};return std::memcmp(&out,&empty,sizeof(out))==0;}
void *domain_get(){
 if(reenter){reenter=false;PgrRoutesMetadataOutput out;std::memset(&out,0xa5,sizeof(out));
  assert(api->resolve(&config,&out)==PGR_MD_ERR_BUSY&&zero(out));++reentries;}
 return &domain_token;
}
void *domain_assemblies(void *,size_t *count){*count=2;return assemblies;}
void *assembly_image(void *assembly){return assembly;}
void *class_from_name(void *image,const char *ns,const char *name){
 for(size_t i=0;i<kClassCount;i++)if(std::strcmp(ns,kClasses[i].name_space)==0&&std::strcmp(name,kClasses[i].name)==0){
  if(image==&image_a)return &classes[i];
  if(image==&image_b&&duplicate&&i==2)return duplicate_class;
 }return nullptr;
}
void *class_type(void *klass){assert(klass);return static_cast<Class *>(klass)->type;}
char *type_name(void *raw){assert(raw);const auto &name=static_cast<Type *>(raw)->name;
 char *copy=static_cast<char *>(std::malloc(name.size()+1));assert(copy);std::memcpy(copy,name.c_str(),name.size()+1);++names_allocated;return copy;}
void mem_free(void *pointer){assert(pointer);++names_freed;std::free(pointer);}
void *field_by_name(void *owner,const char *name){for(auto &f:fields)if(f->lookup==owner&&f->name==name)return f.get();return nullptr;}
void *field_parent(void *raw){return static_cast<Field *>(raw)->owner;}
int field_flags(void *raw){return static_cast<Field *>(raw)->flags;}
size_t field_offset(void *raw){return static_cast<Field *>(raw)->offset;}
void *field_type(void *raw){return static_cast<Field *>(raw)->type;}
void *from_type(void *raw){return raw?static_cast<Type *>(raw)->klass:nullptr;}
bool value_type(void *raw){return static_cast<Class *>(raw)->value;}
bool is_enum(void *raw){return static_cast<Class *>(raw)->enumeration;}
void *enum_base(void *raw){return static_cast<Class *>(raw)->base;}
int value_size(void *raw,int *align){auto *klass=static_cast<Class *>(raw);*align=klass->align;return klass->size;}
void *method_by_name(void *owner,const char *name,int count){for(auto &m:methods)if(m->lookup==owner&&m->name==name&&int(m->count)==count)return m.get();return nullptr;}
void *method_class(void *raw){return static_cast<Method *>(raw)->owner;}
char *method_name(void *raw){return const_cast<char *>(static_cast<Method *>(raw)->name.c_str());}
uint32_t method_flags(void *raw,uint32_t *impl){*impl=0;return static_cast<Method *>(raw)->flags;}
uint32_t param_count(void *raw){return static_cast<Method *>(raw)->count;}
void *param(void *raw,uint32_t index){assert(index==0);return static_cast<Method *>(raw)->param;}
void *return_type(void *raw){return static_cast<Method *>(raw)->result;}
bool generic(void *raw){return static_cast<Method *>(raw)->generic;}
bool inflated(void *raw){return static_cast<Method *>(raw)->inflated;}
bool debug_info(void *raw,void *out){static_cast<void **>(out)[0]=static_cast<Method *>(raw)->code;return true;}
bool assignable(void *,void *){return false;}
void *enumerate(void *,void **){return nullptr;}
void *declaring(void *){return nullptr;}
void *thread_current(){++thread_checks;return attached&&(!detach_at||thread_checks<detach_at)?&thread_token:nullptr;}
int segment(void *,const void *address,uint64_t length,PgrMetaSegment *out){
 *out={};uintptr_t a=reinterpret_cast<uintptr_t>(address),base=reinterpret_cast<uintptr_t>(module_base);
 if(a<base||a-base>module_size||!length||length>module_size-(a-base))return 0;
 out->found=out->readable=out->host_exec=out->writable=1;out->seg_base=module_base;out->seg_size=module_size;return 0;
}
PgrMetaExports exports(){
 PgrMetaExports e{};
 e.domain_get=reinterpret_cast<void *>(domain_get);e.domain_get_assemblies=reinterpret_cast<void *>(domain_assemblies);
 e.assembly_get_image=reinterpret_cast<void *>(assembly_image);e.class_from_name=reinterpret_cast<void *>(class_from_name);
 e.class_get_type=reinterpret_cast<void *>(class_type);e.type_get_name=reinterpret_cast<void *>(type_name);e.mem_free=reinterpret_cast<void *>(mem_free);
 e.class_get_field_from_name=reinterpret_cast<void *>(field_by_name);e.field_get_parent=reinterpret_cast<void *>(field_parent);
 e.field_get_flags=reinterpret_cast<void *>(field_flags);e.field_get_offset=reinterpret_cast<void *>(field_offset);
 e.field_get_type=reinterpret_cast<void *>(field_type);e.class_from_type=reinterpret_cast<void *>(from_type);
 e.class_is_valuetype=reinterpret_cast<void *>(value_type);e.class_is_enum=reinterpret_cast<void *>(is_enum);
 e.class_enum_basetype=reinterpret_cast<void *>(enum_base);e.class_value_size=reinterpret_cast<void *>(value_size);
 e.class_get_method_from_name=reinterpret_cast<void *>(method_by_name);e.method_get_class=reinterpret_cast<void *>(method_class);
 e.method_get_name=reinterpret_cast<void *>(method_name);e.method_get_flags=reinterpret_cast<void *>(method_flags);
 e.method_get_param_count=reinterpret_cast<void *>(param_count);e.method_get_param=reinterpret_cast<void *>(param);
 e.method_get_return_type=reinterpret_cast<void *>(return_type);e.method_is_generic=reinterpret_cast<void *>(generic);
 e.method_is_inflated=reinterpret_cast<void *>(inflated);e.debug_get_method_info=reinterpret_cast<void *>(debug_info);
 e.class_is_assignable_from=reinterpret_cast<void *>(assignable);e.class_get_methods=reinterpret_cast<void *>(enumerate);
 e.class_get_nested_types=reinterpret_cast<void *>(enumerate);e.class_get_declaring_type=reinterpret_cast<void *>(declaring);return e;
}
static constexpr unsigned char PREFIX[]={0xfe,0x5f,0xbd,0xa9,0xf6,0x57,0x01,0xa9,0xf4,0x4f,0x02,0xa9,
 0xd4,0x50,0x02,0xb0,0xf3,0x03,0x00,0xaa,0x88,0x3a,0x4e,0x39};
constexpr uint64_t CONFIRM_RVA=174801868u,DISMISS_RVA=0xa375344u;
void build_fixture(){
 // Independent facts from routes.js, not generated success expectations alone.
 assert(kClassCount==15&&kFieldCount==24&&kMethodCount==18);
 assert(std::strcmp(kClasses[2].canonical,"Niantic.Holoholo.Routes.RoutesService")==0);
 assert(std::strcmp(kClasses[5].canonical,"Holoholo.Rpc.SharedRouteProto")==0);
 assert(std::strcmp(kClasses[7].canonical,"Holoholo.Rpc.RouteWaypointProto")==0);
 assert(kFields[2].owner==2&&kFields[2].offset==0x210&&std::strcmp(kFields[2].name,"didn")==0);
 assert(kFields[3].owner==2&&kFields[3].offset==0x80&&std::strcmp(kFields[3].name,"dica")==0);
 assert(kFields[4].owner==3&&std::strcmp(kFields[4].name,"<Routes>k__BackingField")==0);
 assert(kFields[11].owner==5&&std::strcmp(kFields[11].name,"Waypoints")==0);
 assert(kFields[10].offset==0x10&&kFields[18].offset==0x10);
 assert(kMethods[2].owner==10&&std::strcmp(kMethods[2].key,"dismissGui")==0&&std::strcmp(kMethods[2].name,"Dismiss")==0);
 long page=sysconf(_SC_PAGESIZE);assert(page>0);module_size=((CONFIRM_RVA+sizeof(PREFIX)+page-1)/page)*page;
 void *mapped=mmap(nullptr,module_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(mapped!=MAP_FAILED);
 module_base=static_cast<unsigned char *>(mapped);std::memcpy(module_base+CONFIRM_RVA,PREFIX,sizeof(PREFIX));
 for(size_t i=0;i<kClassCount;i++)classes[i].type=new_type(kClasses[i].canonical,&classes[i]);
 classes[14].value=true;classes[14].size=classes[14].align=1;
 int32_type=new_class("System.Int32",true,4,4)->type;void_type=new_class("System.Void",true,0,1)->type;
 new_class("System.Int64",true,8,8);new_class("System.Double",true,8,8);
 Class *enum_class=new_class("Fixture.Status",true,4,4);enum_class->enumeration=true;enum_class->base=int32_type;
 generic_ref_type=new_class("Fixture.Reference")->type;new_class("System.String");
 list_class=new_class("Fixture.RouteList");waypoint_list_class=new_class("Fixture.WaypointList");
 duplicate_class=new_class("Niantic.Holoholo.Routes.RoutesService");
 for(size_t i=0;i<kFieldCount;i++){
  const auto &s=kFields[i];Type *t=s.type_name?type(s.type_name):s.kind==PGR_MD_FIELD_ENUM_I32?enum_class->type:generic_ref_type;
  if(i==4)t=list_class->type;if(i==11)t=waypoint_list_class->type;
  fields.emplace_back(new Field{&classes[s.owner],&classes[s.owner],s.name,t,0,s.offset});
 }
 for(size_t i=0;i<kMethodCount;i++){
  const auto &s=kMethods[i];new_method(&classes[s.owner],s.name,s.return_type?type(s.return_type):nullptr,s.param_count?type(s.param0):nullptr);
 }
 methods[2]->code=module_base+DISMISS_RVA;
 methods[14]->code=module_base+CONFIRM_RVA;
 extra_methods[0]=new_method(list_class,"get_Count",int32_type,nullptr,true);
 extra_methods[1]=new_method(list_class,"get_Item",classes[5].type,int32_type,true);
 extra_methods[2]=new_method(waypoint_list_class,"get_Count",int32_type,nullptr,true);
 extra_methods[3]=new_method(waypoint_list_class,"get_Item",classes[7].type,int32_type,true);
 extra_methods[4]=new_method(&classes[4],".ctor",void_type);
}
PgrRoutesMetadataOutput success(){
 PgrRoutesMetadataOutput out{};assert(api->resolve(&config,&out)==0&&out.version==1&&out.size==sizeof(out));
 for(size_t i=0;i<kClassCount;i++)assert(out.classes[i]==&classes[i]);
 for(size_t i=0;i<kFieldCount;i++)assert(out.fields[i]==fields[i].get());
 for(size_t i=0;i<kMethodCount;i++)assert(out.methods[i]==methods[i].get());
 assert(out.list_count==extra_methods[0]&&out.list_item==extra_methods[1]);
 assert(out.waypoint_count==extra_methods[2]&&out.waypoint_item==extra_methods[3]&&out.constructor==extra_methods[4]);
 assert(names_allocated==names_freed);return out;
}
void failure(int expected,const PgrRoutesMetadataConfig *c=nullptr){
 PgrRoutesMetadataOutput out;std::memset(&out,0xa5,sizeof(out));assert(api->resolve(c?c:&config,&out)==expected);
 assert(zero(out)&&names_allocated==names_freed);
}
}
int main(int argc,char **argv){
 assert(argc==2);void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(library);
 auto get=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(library,"pgrazor_native_api"));assert(get);
 auto *core=static_cast<const pgrazor_native_api_v2 *>(get(2));assert(core&&core->query_feature&&!core->query_feature(53,2));
 api=static_cast<const PgrRoutesMetadataApi *>(core->query_feature(53,1));
 assert(api&&api->version==1&&api->size==24&&api->config_size==88&&api->output_size==504);
 build_fixture();PgrMetaExports e=exports();config.version=1;config.size=sizeof(config);
 config.binding.version=1;config.binding.size=sizeof(PgrMetaBindConfig);config.binding.exports=&e;
 config.binding.module_base=module_base;config.binding.module_size=module_size;config.binding.seg_query=segment;
 config.thread_current=reinterpret_cast<void *>(thread_current);config.owner_tid=gettid();
 success();success(); // immutable binding and repeated accessor pointer identity
 {auto c=config;c.version=2;failure(-2,&c);c=config;c.size--;failure(-2,&c);c=config;c.thread_current=nullptr;failure(-2,&c);}
 {PgrRoutesMetadataOutput out;std::memset(&out,0xa5,sizeof(out));assert(api->resolve(nullptr,&out)==-2&&zero(out));assert(api->resolve(&config,nullptr)==-1);}
 attached=false;failure(-30);attached=true;
 {auto c=config;c.owner_tid=0;failure(-30,&c);c.owner_tid=config.owner_tid+1;failure(-30,&c);}
 {std::thread foreign([]{failure(-30);});foreign.join();}
 thread_checks=0;detach_at=5;failure(-30);detach_at=0;success();
 {auto saved=fields[2]->offset;fields[2]->offset++;failure(PGR_MD_ERR_OFFSET);fields[2]->offset=saved;}
 {auto saved=fields[10]->type;fields[10]->type=generic_ref_type;failure(PGR_MD_ERR_TYPE);fields[10]->type=saved;}
 fields[2]->flags=0x10;failure(PGR_MD_ERR_FLAGS);fields[2]->flags=0;
 {auto saved=methods[2]->result;methods[2]->result=int32_type;failure(PGR_MD_ERR_TYPE);methods[2]->result=saved;}
 {auto saved=methods[2]->owner;methods[2]->owner=&classes[0];failure(PGR_MD_ERR_OWNER_MISMATCH);methods[2]->owner=saved;}
 methods[2]->flags=0x10;failure(PGR_MD_ERR_STATIC);methods[2]->flags=0;
 methods[2]->generic=true;failure(PGR_MD_ERR_GENERIC);methods[2]->generic=false;
 methods[2]->inflated=true;failure(PGR_MD_ERR_INFLATED);methods[2]->inflated=false;
 {auto saved=methods[16]->param;methods[16]->param=int32_type;failure(PGR_MD_ERR_TYPE);methods[16]->param=saved;}
 duplicate=true;failure(PGR_MD_ERR_DUPLICATE);duplicate=false;
 module_base[CONFIRM_RVA]^=1;failure(PGR_MD_ERR_PROLOGUE);module_base[CONFIRM_RVA]^=1;
 {void *saved=methods[2]->code;methods[2]->code=module_base+DISMISS_RVA+4;failure(-33);methods[2]->code=saved;}
 {void *saved=methods[14]->code;methods[14]->code=module_base+CONFIRM_RVA+4;failure(-33);methods[14]->code=saved;}
 {auto c=config;c.binding.seg_query=nullptr;unsigned before=thread_checks;failure(PGR_MD_ERR_CODE_RANGE,&c);assert(thread_checks==before);}
 {auto c=config;c.binding.module_base=reinterpret_cast<void *>(UINTPTR_MAX-3);c.binding.module_size=8;unsigned before=thread_checks;
  failure(PGR_MD_ERR_CODE_RANGE,&c);assert(thread_checks==before);}
 {auto saved=extra_methods[0]->result;extra_methods[0]->result=type("System.Int64");failure(PGR_MD_ERR_TYPE);extra_methods[0]->result=saved;}
 {auto saved=extra_methods[3]->result;extra_methods[3]->result=classes[5].type;failure(PGR_MD_ERR_TYPE);extra_methods[3]->result=saved;}
 extra_methods[4]->flags=0x10;failure(PGR_MD_ERR_STATIC);extra_methods[4]->flags=0;
 reenter=true;success();assert(reentries==1);success();
 assert(names_allocated==names_freed);assert(munmap(module_base,module_size)==0);
 std::puts("PASS Routes metadata actual-library: all15 classes/24 fields/18 methods+5 derived positions, exact source-known mappings, cleared failures, owner/attachment, metadata faults, duplicate class, pinned address/prefix, reentrant BUSY, balanced type-name frees");
 // No hooks installed. Keep production library until process exit to avoid other
 // process-lifetime native owners' destructor assumptions; no production runtime claim.
 return 0;
}

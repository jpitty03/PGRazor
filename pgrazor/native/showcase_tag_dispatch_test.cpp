#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "unity_scheduler.h"
#include <time.h>
#include <unistd.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <memory>
#include <string>
namespace fixture {
struct K{std::string name;bool enumeration=false;};
struct F{K* owner;std::string name;K* type;int flags=0,literal=0;};
struct M{K* owner;std::string name;K* result;std::vector<K*> args;};
struct O{K* type;bool alive=true;std::map<std::string,uint64_t> values;std::map<std::string,size_t> refs;std::vector<size_t> list;std::string text;};
std::map<std::string,K> classes;std::map<std::string,F> fields;std::map<std::string,M> methods;
std::vector<std::unique_ptr<O>> objects,retired;std::map<uintptr_t,size_t> handles;
uint64_t now=100,generation=7;uintptr_t serial=10;int tid=71,dispatches=0,allocs=0;
bool attached=true,expire_dispatch=false,expire_cleanup=false,throw_dispatch=false,null_dispatch=false,bad_graph=false;
std::string missing_field,missing_method;uint64_t observed_pokemon=0,observed_tag=0;bool observed_remove=false;
size_t service_id,pokemon_id,tag_id,promise_id,output_id,created_id;
K* type(const std::string& name){auto& k=classes[name];k.name=name;return &k;}
F* field(K* owner,const char* name,K* ft=nullptr,int flags=0,int literal=0){auto key=owner->name+":"+name;auto it=fields.find(key);if(!ft)return it==fields.end()?nullptr:&it->second;return &(fields[key]={owner,name,ft,flags,literal});}
M* method(K* owner,const char* name,K* result,std::vector<K*> args={}){return &(methods[owner->name+":"+name]={owner,name,result,args});}
size_t identify(void* p){for(size_t i=0;i<objects.size();++i)if(objects[i].get()==p){assert(objects[i]->alive);return i;}assert(false&&"stale/unrooted managed argument");return 0;}
void* ptr(size_t id){return objects.at(id).get();}
void relocate(){for(auto& p:objects){auto copy=std::make_unique<O>(*p);p->alive=false;retired.push_back(std::move(p));p=std::move(copy);}}
size_t make(K* k){objects.push_back(std::make_unique<O>());objects.back()->type=k;return objects.size()-1;}
void* allocate(void* k){++allocs;relocate();return ptr(make(static_cast<K*>(k)));}
void* string_new(const char* text){auto p=allocate(type("System.String"));static_cast<O*>(p)->text=text;return p;}
uintptr_t root(void* p,int pinned){assert(!pinned);auto id=identify(p);relocate();handles[++serial]=id;return serial;}
void release(uintptr_t h){assert(h>3&&handles.erase(h)==1);relocate();if(expire_cleanup&&dispatches)now=2600;}
void* target(uintptr_t h){auto it=handles.find(h);return it==handles.end()?nullptr:ptr(it->second);}
void* object_class(void* p){return objects[identify(p)]->type;}
void* current_thread(){return attached?&now:nullptr;}
int clock(int,timespec* t){t->tv_sec=now/1000;t->tv_nsec=(now%1000)*1000000;return 0;}
int get_tid(){return tid;}
int scheduler_snapshot(PgrUnitySchedulerSnapshot* s){*s={generation,1,71,1,0,0};return 0;}
int scheduler_valid(uint64_t g){return g&&g==generation;}
char* type_name(void* k){const auto& s=static_cast<K*>(k)->name;auto p=static_cast<char*>(std::malloc(s.size()+1));std::memcpy(p,s.c_str(),s.size()+1);return p;}
void mem_free(void* p){std::free(p);}
void* class_from_type(void* k){return k;}
bool assignable(void* a,void* b){return a==b;}
void* find_field(void* k,const char* name){if(missing_field==name)return nullptr;return field(static_cast<K*>(k),name);}
void* field_type(void* f){return static_cast<F*>(f)->type;}
int field_flags(void* f){return static_cast<F*>(f)->flags;}
bool is_enum(void* k){return static_cast<K*>(k)->enumeration;}
void* enum_base(void*){return type("System.Int32");}
void* find_method(void* k,const char* name,int count){if(missing_method==name)return nullptr;auto it=methods.find(static_cast<K*>(k)->name+":"+name);return it!=methods.end()&&int(it->second.args.size())==count?&it->second:nullptr;}
void* method_class(void* m){return static_cast<M*>(m)->owner;}
unsigned argc(void* m){return unsigned(static_cast<M*>(m)->args.size());}
void* param(void* m,unsigned i){return static_cast<M*>(m)->args.at(i);}
void* result_type(void* m){return static_cast<M*>(m)->result;}
void static_get(void* f,void* out){std::memcpy(out,&static_cast<F*>(f)->literal,4);}
void get(void* p,void* fp,void* out){
 auto& o=*objects[identify(p)];auto& f=*static_cast<F*>(fp);assert(o.type==f.owner);
 if(f.type->name=="System.Boolean"){uint8_t n=uint8_t(o.values[f.name]);std::memcpy(out,&n,1);}
 else if(f.type->name=="System.UInt64"){auto n=o.values[f.name];std::memcpy(out,&n,8);}
 else if(f.type->enumeration){int n=int(o.values[f.name]);std::memcpy(out,&n,4);}
 else {auto it=o.refs.find(f.name);void* value=it==o.refs.end()?nullptr:ptr(it->second);std::memcpy(out,&value,sizeof(value));}
}
void set_ref(void* p,void* fp,void* value){auto id=identify(p),other=identify(value);auto& f=*static_cast<F*>(fp);assert(objects[id]->type==f.owner);objects[id]->refs[f.name]=other;}
void* invoke(void* mp,void* self,void** args,void** exception){
 *exception=nullptr;auto& m=*static_cast<M*>(mp);auto receiver=identify(self);assert(objects[receiver]->type==m.owner);
 std::vector<size_t> references;for(unsigned i=0;i<m.args.size();++i)if(!m.args[i]->enumeration)references.push_back(identify(args[i]));
 int color=m.name=="AddTag"?*static_cast<int*>(args[1]):0;
 relocate();
 if(m.name==".ctor")return nullptr;
 if(m.name=="Add"){objects[receiver]->list.push_back(references.at(0));return nullptr;}
 ++dispatches;
 if(m.name=="AddTag"){
  assert(color==19);assert(objects[references.at(0)]->text=="Showcase \xC2\xB7 Fire");
  objects[promise_id]->type=type("CreatePromise");objects[promise_id]->refs["completedValue"]=output_id;
 }else{
  assert(m.name=="SetPokemonTags");auto& changes=objects[references.at(0)]->list;assert(changes.size()==1);
  auto& change=*objects[changes[0]];auto& add=objects[change.refs.at("TagsToAdd")]->list;auto& remove=objects[change.refs.at("TagsToRemove")]->list;
  assert(add.size()+remove.size()==1);observed_remove=!remove.empty();
  observed_pokemon=objects[change.refs.at("PokemonProto")]->values["id_"];
  observed_tag=objects[observed_remove?remove[0]:add[0]]->values["Id"];
  objects[promise_id]->type=type("BoolPromise");objects[promise_id]->values["completedValue"]=1;
 }
 if(expire_dispatch)now=2600;if(throw_dispatch)*exception=ptr(output_id);
 return null_dispatch?nullptr:ptr(promise_id);
}
int resolve(const PgrMetaClassQuery* q,PgrMetaClassResult* out){auto key=std::string(q->name_space)+(q->name_space[0]?".":"")+q->name;auto it=classes.find(key);if(it==classes.end())return -1;out->klass=&it->second;return 0;}
int validate(const PgrMetaMethodQuery* q,PgrMetaMethodResult* out){out->method=find_method(const_cast<void*>(q->owner),q->name,q->param_count);return out->method?0:-1;}
int bootstrap(PgrBootBinding* b){*b={};b->handle=&now;
#define R(member,fn) b->runtime.member=reinterpret_cast<void*>(fn)
 R(gchandle_new,root);R(gchandle_get_target,target);R(gchandle_free,release);R(object_get_class,object_class);R(object_new,allocate);R(runtime_invoke,invoke);R(field_get_value,get);R(field_static_get_value,static_get);R(string_new,string_new);R(thread_current,current_thread);
#undef R
#define M(member,fn) b->meta.member=reinterpret_cast<void*>(fn)
 M(class_get_field_from_name,find_field);M(field_get_type,field_type);M(class_from_type,class_from_type);M(type_get_name,type_name);M(mem_free,mem_free);M(class_get_method_from_name,find_method);M(method_get_param_count,argc);M(method_get_param,param);M(method_get_return_type,result_type);M(field_get_flags,field_flags);M(class_is_enum,is_enum);M(class_enum_basetype,enum_base);M(method_get_class,method_class);M(class_is_assignable_from,assignable);
#undef M
 return 0;
}
void* symbol(void*,const char* name){return std::strcmp(name,"il2cpp_field_set_value_object")?nullptr:reinterpret_cast<void*>(set_ref);}
bool authorize(void* context){assert(context==&allocs);return allocs==0;}
void reset(){
 objects.clear();retired.clear();handles.clear();classes.clear();fields.clear();methods.clear();serial=10;now=100;generation=7;tid=71;dispatches=allocs=0;
 attached=true;expire_dispatch=expire_cleanup=throw_dispatch=null_dispatch=bad_graph=false;missing_field.clear();missing_method.clear();observed_pokemon=observed_tag=0;observed_remove=false;
 auto svc=type("Niantic.Holoholo.Tagging.TaggingRpcService"),pk=type("Holoholo.Rpc.PokemonProto"),tag=type("Holoholo.Rpc.PokemonTagProto");
 auto str=type("System.String"),v=type("System.Void"),boolean=type("System.Boolean"),u64=type("System.UInt64"),color=type("Holoholo.Rpc.PokemonTagColor");color->enumeration=true;
 auto out=type("Holoholo.Rpc.CreatePokemonTagOutProto"),status=type("CreateResult");status->enumeration=true;
 auto cp=type("CreatePromise"),bp=type("BoolPromise"),tl=type("System.Collections.Generic.List<Holoholo.Rpc.PokemonTagProto>"),ch=type("Niantic.Holoholo.Tagging.PokemonTagChanges"),cl=type("System.Collections.Generic.List<Niantic.Holoholo.Tagging.PokemonTagChanges>");
 field(color,"Yellow",color,0x8050,19);field(status,"Success",status,0x8050,37);field(pk,"id_",u64);field(tag,"Id",u64);
 field(out,"Result",status);field(out,"CreatedTag",tag);field(cp,"completedValue",out);field(bp,"completedValue",boolean);
 for(auto p:{cp,bp}){field(p,"completeCalled",boolean);field(p,"errorCalled",boolean);}
 field(ch,"PokemonProto",pk);field(ch,"TagsToAdd",tl);field(ch,"TagsToRemove",tl);
 method(svc,"AddTag",type("Niantic.Promises.IPromise<Holoholo.Rpc.CreatePokemonTagOutProto>"),{str,color});
 method(svc,"SetPokemonTags",type("Niantic.Promises.IPromise<System.Boolean>"),{cl});
 for(auto k:{tl,ch,cl})method(k,".ctor",v);method(tl,"Add",v,{tag});method(cl,"Add",v,{ch});
 service_id=make(svc);pokemon_id=make(pk);tag_id=make(tag);promise_id=make(cp);output_id=make(out);created_id=make(tag);
 objects[pokemon_id]->values["id_"]=123;objects[tag_id]->values["Id"]=456;objects[created_id]->values["Id"]=700;
 objects[output_id]->values["Result"]=37;objects[output_id]->refs["CreatedTag"]=created_id;
 handles={{1,service_id},{2,pokemon_id},{3,tag_id}};
}
}
extern "C" const void* pgrazor_runtime_bootstrap_api(uint32_t){static PgrBootApi api{};api.snapshot=fixture::bootstrap;return &api;}
extern "C" const void* pgrazor_runtime_metadata_api(uint32_t){static PgrMetaApi api{};api.resolve_class=fixture::resolve;api.validate_method=fixture::validate;return &api;}
extern "C" const void* pgrazor_unity_scheduler_api(uint32_t){static PgrUnitySchedulerApi api{};api.snapshot=fixture::scheduler_snapshot;api.valid=fixture::scheduler_valid;return &api;}
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#define clock_gettime fixture::clock
#define gettid fixture::get_tid
#define dlsym fixture::symbol
#include "showcase_tag_runtime.cpp"
#undef clock_gettime
#undef gettid
#undef dlsym
int main(){
 using namespace pgr_showcase;
 auto req=[](TagRpcOp op){TagRpcRequest r;r.owner=71;r.generation=7;r.deadline_ms=2600;r.service=1;r.pokemon=2;r.tag=3;r.pokemon_id=123;r.tag_id=456;r.type_index=9;r.op=op;return r;};
 auto clean=[](){assert(fixture::handles.size()==3);};
 for(auto op:{TagRpcOp::create,TagRpcOp::add,TagRpcOp::remove}){
  fixture::reset();auto r=req(op);uintptr_t pending=0;assert(start_tag_rpc(r,pending)==TagStartStatus::dispatched&&pending&&fixture::dispatches==1);assert(fixture::handles.size()==4);
  if(op!=TagRpcOp::create)assert(fixture::observed_pokemon==123&&fixture::observed_tag==456&&fixture::observed_remove==(op==TagRpcOp::remove));
  uint64_t id=999;assert(poll_tag_rpc(r,pending,id)==TagPollStatus::pending&&id==0);
  fixture::objects[fixture::promise_id]->values["completeCalled"]=1;
  assert(poll_tag_rpc(r,pending,id)==(op==TagRpcOp::create?TagPollStatus::created:TagPollStatus::confirmed));assert(id==(op==TagRpcOp::create?700:0));
  fixture::objects[fixture::promise_id]->values["errorCalled"]=1;assert(poll_tag_rpc(r,pending,id)==TagPollStatus::rejected&&id==0);
  fixture::release(pending);clean();
 }
 for(int mode=0;mode<4;++mode){fixture::reset();fixture::expire_dispatch=mode==0;fixture::expire_cleanup=mode==1;fixture::throw_dispatch=mode==2;fixture::null_dispatch=mode==3;
  uintptr_t p=99;assert(start_tag_rpc(req(TagRpcOp::add),p)==TagStartStatus::outcome_unknown&&!p&&fixture::dispatches==1);clean();}
 fixture::reset();auto bad=req(TagRpcOp::remove);bad.tag_id=999;uintptr_t pending=0;assert(start_tag_rpc(bad,pending)==TagStartStatus::bad_identity&&!fixture::dispatches);clean();
 fixture::reset();fixture::missing_field="TagsToRemove";assert(start_tag_rpc(req(TagRpcOp::add),pending)==TagStartStatus::bad_binding&&!fixture::dispatches);clean();
 fixture::reset();fixture::missing_method=".ctor";assert(start_tag_rpc(req(TagRpcOp::add),pending)==TagStartStatus::refused&&!fixture::dispatches);clean();
 fixture::reset();fixture::generation=8;assert(start_tag_rpc(req(TagRpcOp::create),pending)==TagStartStatus::refused&&!fixture::dispatches);clean();
 fixture::reset();auto revoked=req(TagRpcOp::create);revoked.authorized=fixture::authorize;revoked.authorization_context=&fixture::allocs;
 assert(start_tag_rpc(revoked,pending)==TagStartStatus::refused&&!fixture::dispatches&&!pending);clean();
 fixture::reset();auto r=req(TagRpcOp::create);assert(start_tag_rpc(r,pending)==TagStartStatus::dispatched);fixture::objects[fixture::promise_id]->values["completeCalled"]=1;
 fixture::missing_field="CreatedTag";uint64_t id=999;assert(poll_tag_rpc(r,pending,id)==TagPollStatus::bad_binding&&id==0);fixture::release(pending);clean();
 std::puts("PASS tag RPC dispatch: moving GC, create/add/remove graph, promise polling, ownership, identities, metadata, expiry and uncertain outcomes");
}

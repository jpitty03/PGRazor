#include "showcase_inventory.h"
#include "showcase_bindings.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <cmath>
namespace fixture {
struct Class {std::string name;};std::map<std::string,Class> classes;
void* cls(const std::string& name){auto& c=classes[name];c.name=name;return &c;}
struct Object {
 alignas(8) std::array<unsigned char,128> bytes{};
 void* type=nullptr;int scalar=0;std::vector<size_t> elements;
};
std::vector<std::unique_ptr<Object>> objects,retired;
std::map<uintptr_t,size_t> handles;uintptr_t next_handle;
uint64_t now;int copies,scores,root_count;bool egg,change_count,change_id,change_height,expired,bad_type,reenter;
bool wrong_class,bad_field,missing_method;float height;
int prediction_species,prediction_form;bool change_prediction,dual_type;
size_t pokemon,display,settings,collection;uint64_t pokemon_id;
pgr_showcase::InventoryRequest request;
size_t identify(void* p){for(size_t i=0;i<objects.size();++i)if(objects[i]->bytes.data()==p)return i;assert(false&&"stale managed pointer");return 0;}
void* ptr(size_t id){return objects.at(id)->bytes.data();}
void refresh_arrays(){for(auto& o:objects)for(size_t i=0;i<o->elements.size();++i){void* value=ptr(o->elements[i]);std::memcpy(o->bytes.data()+32+i*8,&value,8);}}
void move(){for(auto& o:objects){auto next=std::make_unique<Object>(*o);o->bytes.fill(0xee);retired.push_back(std::move(o));o=std::move(next);}refresh_arrays();}
size_t make(void* type,int scalar=0){auto o=std::make_unique<Object>();o->type=type;o->scalar=scalar;objects.push_back(std::move(o));return objects.size()-1;}
void* target(uintptr_t h){auto it=handles.find(h);return it==handles.end()?nullptr:ptr(it->second);}
uintptr_t root(void* p,int pinned){assert(!pinned);size_t i=identify(p);move();++root_count;handles[++next_handle]=i;return next_handle;}
void release(uintptr_t h){assert(h>4&&handles.erase(h)==1);--root_count;move();}
void* object_class(void* p){return objects[identify(p)]->type;}
void* unbox(void* p){return &objects[identify(p)]->scalar;}
uint64_t clock(){return now;}
bool owner(uint64_t tid,uint64_t gen){return tid==71&&gen==7;}
struct Method{void* owner;std::string name;};std::map<std::string,Method> methods;
void* method(void* owner,const char* name,int){if(missing_method)return nullptr;std::string key=static_cast<Class*>(owner)->name+":"+name;auto& m=methods[key];m={owner,name};return &m;}
void* method_class(void* p){return static_cast<Method*>(p)->owner;}
bool assignable(void* expected,void* actual){return expected==actual;}
struct Field{std::string name;int literal=0;};std::map<std::string,Field> fields;
void* field(void*,const char* name){auto& f=fields[name];f.name=name;return &f;}
int flags(void*){return 0x8050;}
void static_value(void* p,void* out){std::memcpy(out,&static_cast<Field*>(p)->literal,4);}
void get(void* p,void* f,void* out){
 auto index=identify(p);const auto& name=static_cast<Field*>(f)->name;
 if(name=="id_"){uint64_t id=pokemon_id+(change_id&&copies==2?1:0);std::memcpy(out,&id,8);}
 else if(name=="pokemonId_"){int v=1;std::memcpy(out,&v,4);}
 else if(name=="creationTimeMs_"){int64_t v=123;std::memcpy(out,&v,8);}
 else if(name=="isEgg_"){uint8_t v=egg?1:0;std::memcpy(out,&v,1);}
 else if(name=="heightM_"){float v=height+(change_height&&copies==2?1.0f:0.0f);std::memcpy(out,&v,4);}
 else if(name=="weightKg_"){float v=10;std::memcpy(out,&v,4);}
 else if(name=="pokemonDisplay_"){void* value=ptr(display);std::memcpy(out,&value,8);}
 else if(name=="ScalarScoreReferencePokemon"){int v=prediction_species+(change_prediction&&scores?1:0);std::memcpy(out,&v,4);}
 else if(name=="ScalarScoreReferencePokemonForm")std::memcpy(out,&prediction_form,4);
 else {int v=name=="Form"?0:name=="size_"?3:10;std::memcpy(out,&v,4);}
 (void)index;
}
void* box(void* type,int value){size_t i=make(type,value);move();return ptr(i);}
void* invoke(const void* m,void* self,void** args,void** exc){
 *exc=nullptr;auto name=static_cast<const Method*>(m)->name;identify(self);
 if(name=="get_AllPokemon"){move();return ptr(collection);}
 if(name=="get_Count")return box(cls("System.Int32"),change_count&&scores?2:1);
 if(name=="CopyTo"){
  size_t a=identify(args[0]);assert(*static_cast<int*>(args[1])==0);++copies;
  objects[a]->elements={pokemon};move();return nullptr;
 }
 if(name=="GetPokemonSettings"){assert(args[0]==target(2));assert(!*static_cast<bool*>(args[1]));move();return ptr(settings);}
 if(name=="get_Type1")return box(cls("Holoholo.Rpc.HoloPokemonType"),bad_type?999:109);
 if(name=="get_Type2")return box(cls("Holoholo.Rpc.HoloPokemonType"),dual_type?110:-1);
 assert(name=="bnen");assert(args[0]==ptr(pokemon)&&args[1]==target(4));++scores;
 if(reenter){std::vector<pgr_showcase::Row> rows;const char* error=nullptr;assert(!pgr_showcase::scan_inventory(request,rows,error)&&error);}
 if(expired)now=2600;
 float result=900;int bits;std::memcpy(&bits,&result,4);return box(cls("System.Single"),bits);
}
void* array_new(void* type,uintptr_t n){assert(type==cls("Holoholo.Rpc.PokemonProto")&&n<=2);size_t a=make(cls("PokemonArray"));objects[a]->elements.assign(n,pokemon);move();return ptr(a);}
uintptr_t array_length(void* p){return objects[identify(p)]->elements.size();}
int rank(void* p){assert(p==cls("PokemonArray"));return 1;}
void* element(void*){return cls("Holoholo.Rpc.PokemonProto");}
int element_size(void*){return 8;}
void* symbol(void*,const char* name){
 if(!std::strcmp(name,"il2cpp_array_new"))return reinterpret_cast<void*>(array_new);
 if(!std::strcmp(name,"il2cpp_array_length"))return reinterpret_cast<void*>(array_length);
 if(!std::strcmp(name,"il2cpp_class_get_rank"))return reinterpret_cast<void*>(rank);
 if(!std::strcmp(name,"il2cpp_class_get_element_class"))return reinterpret_cast<void*>(element);
 if(!std::strcmp(name,"il2cpp_array_element_size"))return reinterpret_cast<void*>(element_size);
 return nullptr;
}
int resolve(const PgrMetaClassQuery* q,PgrMetaClassResult* out){out->klass=cls(std::string(q->name_space)+(q->name_space[0]?".":"")+q->name);return 0;}
int validate_method(const PgrMetaMethodQuery* q,PgrMetaMethodResult* out){out->method=method(const_cast<void*>(q->owner),q->name,q->param_count);return out->method?0:-1;}
int validate_field(const PgrMetaFieldQuery* q,PgrMetaFieldResult* out){out->field=field(nullptr,q->name);return bad_field?-1:0;}
int bootstrap(PgrBootBinding* out){*out={};out->handle=&now;
 out->meta.class_get_method_from_name=reinterpret_cast<void*>(method);out->meta.method_get_class=reinterpret_cast<void*>(method_class);
 out->meta.class_is_assignable_from=reinterpret_cast<void*>(assignable);out->meta.class_get_field_from_name=reinterpret_cast<void*>(field);
 out->meta.field_get_flags=reinterpret_cast<void*>(flags);out->runtime.field_static_get_value=reinterpret_cast<void*>(static_value);return 0;
}
void reset(){
 objects.clear();retired.clear();handles.clear();methods.clear();fields.clear();next_handle=4;now=100;copies=scores=root_count=0;
 egg=change_count=change_id=change_height=expired=bad_type=reenter=wrong_class=bad_field=missing_method=false;height=2;pokemon_id=123;
 prediction_species=prediction_form=0;change_prediction=dual_type=false;
 auto bag=make(cls("Niantic.Holoholo.Internal.PokemonBagImpl")),gm=make(cls("IGameMasterData"));
 auto service=make(cls("ContestService")),contest=make(cls("ContestProto"));
 handles={{1,bag},{2,gm},{3,service},{4,contest}};
 pokemon=make(cls("Holoholo.Rpc.PokemonProto"));display=make(cls("Holoholo.Rpc.PokemonDisplayProto"));
 settings=make(cls("Holoholo.Rpc.IPokemonSettings"));collection=make(cls("Collection"));
 const char* names[]={"Normal","Fighting","Flying","Poison","Ground","Rock","Bug","Ghost","Steel","Fire","Water","Grass","Electric","Psychic","Ice","Dragon","Dark","Fairy"};
 static_cast<Field*>(field(nullptr,"PokemonTypeNone"))->literal=-1;
 for(int i=0;i<18;++i)static_cast<Field*>(field(nullptr,(std::string("PokemonType")+names[i]).c_str()))->literal=100+i;
 request={};request.gate={71,2600,0,3,0,0,7};request.bag=1;request.game_master=2;request.contests[9]=4;
}
}
extern "C" const void* pgrazor_runtime_bootstrap_api(uint32_t){static PgrBootApi api{};api.snapshot=fixture::bootstrap;return &api;}
extern "C" const void* pgrazor_runtime_metadata_api(uint32_t){static PgrMetaApi api{};api.resolve_class=fixture::resolve;api.validate_field=fixture::validate_field;api.validate_method=fixture::validate_method;return &api;}
namespace pgr_showcase {
bool resolve_live_score_bindings(const ScoreRequest&,ScoreBindings& b){using namespace fixture;
 b={root,target,release,object_class,invoke,unbox,get,clock,owner,cls("ContestService"),cls("Holoholo.Rpc.PokemonProto"),cls("ContestProto"),cls("System.Single"),method(cls("ContestService"),"bnen",2),field(nullptr,"id_")};return true;
}
}
#define dlsym fixture::symbol
#include "showcase_inventory.cpp"
#undef dlsym
int main(){
 using namespace fixture;using namespace pgr_showcase;
 auto run=[](bool expected){std::vector<Row> rows(1);const char* error=nullptr;bool ok=scan_inventory(request,rows,error);
  assert(ok==expected);assert(root_count==0&&handles.size()==4);
  if(!ok){assert(rows.empty()&&error);return;}
  if(egg){assert(rows.empty());return;}
  assert(!error&&rows.size()==1&&rows[0].id==123&&rows[0].types==((1u<<9)|(dual_type?(1u<<10):0))&&rows[0].scores[9]==900);
  if(dual_type)assert(rows[0].scores[10]==900);
 };
 reset();run(true);assert(copies==2&&scores==1);
 reset();egg=true;run(true);assert(!scores);
 reset();change_count=true;run(false);
 reset();change_id=true;run(false);
 reset();change_height=true;run(false);
 reset();expired=true;run(false);
 reset();bad_type=true;run(false);
 reset();request.contests[9]=0;run(false);assert(!scores);
 reset();request.contests[9]=0;request.prediction_reference=4;run(true);assert(scores==1);
 reset();dual_type=true;request.prediction_reference=4;run(true);assert(scores==1);
 reset();request.authorized=[](void*){return scores==0;};run(false);assert(scores==1);
 reset();request.contests[9]=0;request.prediction_reference=4;prediction_species=1;run(false);assert(!scores);
 reset();request.contests[9]=0;request.prediction_reference=4;prediction_form=1;run(false);assert(!scores);
 reset();request.contests[9]=0;request.prediction_reference=4;change_prediction=true;run(false);assert(scores==1);
 reset();reenter=true;run(true);
 reset();bad_field=true;run(false);
 reset();missing_method=true;run(false);
 reset();objects[pokemon]->type=cls("WrongPokemon");run(false);
 reset();height=std::nanf("");run(false);
 reset();request.gate.generation=8;run(false);
 std::puts("PASS production inventory reader: moving GC, copied bag, egg exclusion, type mapping, score, mutations, missing references, reentry, metadata and lease failures");
}

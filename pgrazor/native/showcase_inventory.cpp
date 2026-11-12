#include "showcase_inventory.h"
#include "showcase_bindings.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include <dlfcn.h>
#include <cstring>
#include <cmath>
#include <set>
#include <map>

namespace pgr_showcase {
namespace {
struct Failure {const char* text;};
void require(bool yes,const char* text){if(!yes)throw Failure{text};}
thread_local bool busy=false;
struct Busy {Busy(){busy=true;}~Busy(){busy=false;}};
struct Reader {
 const InventoryRequest& request;ScoreBindings b;PgrBootBinding boot{};const PgrMetaApi* md;
 Reader(const InventoryRequest& r):request(r) {
  require(resolve_live_score_bindings(r.gate,b),"Showcase score bindings unavailable");
  auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));
  require(api&&api->snapshot(&boot)==0,"Bootstrap unavailable");
  md=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(md,"Metadata unavailable");
 }
 void gate(){auto now=b.boottime_ms();require(now&&request.gate.deadline_ms>now&&request.gate.deadline_ms-now<=2500&&
   b.owner_valid(request.gate.owner,request.gate.generation)&&
   (!request.authorized||request.authorized(request.authorization_context)),"Inventory lease expired or permission revoked");}
 template<class F>F symbol(const char* name){gate();void* p=dlsym(boot.handle,name);require(p,name);return reinterpret_cast<F>(p);}
 void* get(uintptr_t h){gate();void* p=b.target(h);require(p,"Missing rooted object");return p;}
 void* cls(const char* ns,const char* name){gate();PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;
   PgrMetaClassResult out{};require(md->resolve_class(&q,&out)==0,name);return const_cast<void*>(out.klass);}
 void exact(uintptr_t h,void* c){require(b.object_class(get(h))==c,"Unexpected managed class");}
 void* method(void* c,const char* name,const char* result,std::initializer_list<const char*> args={}){
  gate();auto find=reinterpret_cast<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name);
  void* candidate=find(c,name,int(args.size()));require(candidate,name);
  void* declaring=reinterpret_cast<void*(*)(void*)>(boot.meta.method_get_class)(candidate);
  require(reinterpret_cast<bool(*)(void*,void*)>(boot.meta.class_is_assignable_from)(declaring,c),"Method owner mismatch");
  PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=declaring;q.name=name;q.return_type=result;
  q.param_count=unsigned(args.size());q.allow_inflated=1;unsigned i=0;for(auto a:args)q.param_types[i++]=a;
  PgrMetaMethodResult out{};require(md->validate_method(&q,&out)==0,name);return const_cast<void*>(out.method);
 }
 void* field(void* c,const char* name,const char* type,uint64_t offset,unsigned size,unsigned kind=PGR_MD_FIELD_VALUE){
  gate();PgrMetaFieldQuery q{};q.version=1;q.size=sizeof(q);q.owner=c;q.name=name;q.type_name=type;
  q.offset=offset;q.forbidden_mask=0x50;q.value_size=size;q.kind=kind;
  PgrMetaFieldResult out{};require(md->validate_field(&q,&out)==0,name);return const_cast<void*>(out.field);
 }
 template<class T>T value(uintptr_t h,void* f){T out{};b.field_get(get(h),f,&out);return out;}
 void* invoke(void* m,uintptr_t self,void** args=nullptr){gate();void* exception=nullptr;
  void* out=b.invoke(m,get(self),args,&exception);require(!exception,"Inventory managed exception");return out;}
};
struct Root {
 Reader& r;uintptr_t h=0;
 Root(Reader& reader,void* object):r(reader){r.gate();require(object,"Null managed result");h=r.b.root(object,0);require(h,"GC root failed");}
 Root(const Root&)=delete;Root& operator=(const Root&)=delete;
 ~Root(){if(h)r.b.release(h);}
};
int scalar(Reader& r,void* m,uintptr_t obj,void* expected){Root box(r,r.invoke(m,obj));
 r.exact(box.h,expected);int result;void* p=r.b.unbox(r.get(box.h));require(p,"Invalid scalar box");std::memcpy(&result,p,4);return result;}
int literal(Reader& r,void* cls,const char* name){
 r.gate();auto find=reinterpret_cast<void*(*)(void*,const char*)>(r.boot.meta.class_get_field_from_name);
 auto flags=reinterpret_cast<int(*)(void*)>(r.boot.meta.field_get_flags);
 void* f=find(cls,name);require(f&&(flags(f)&0x8050)==0x8050,"Pokemon type enum literal unavailable");int out=0;
 reinterpret_cast<void(*)(void*,void*)>(r.boot.runtime.field_static_get_value)(f,&out);return out;
}
constexpr const char* type_names[]={"Normal","Fighting","Flying","Poison","Ground","Rock","Bug","Ghost","Steel","Fire","Water","Grass","Electric","Psychic","Ice","Dragon","Dark","Fairy"};
struct Identity {uint64_t id;int species;int64_t created;uint8_t egg;std::array<uint32_t,7> score_inputs{};};
}
bool scan_inventory(const InventoryRequest& request,std::vector<Row>& rows,const char*& error){
 rows.clear();error=nullptr;if(busy){error="Inventory scan reentry";return false;}Busy guard;
 try {
  Reader r(request);std::vector<Row> built;
  {
   auto bag_cls=r.cls("Niantic.Holoholo.Internal","PokemonBagImpl");r.exact(request.bag,bag_cls);
   auto int_cls=r.cls("System","Int32"),type_cls=r.cls("Holoholo.Rpc","HoloPokemonType");
   auto settings_iface=r.cls("Holoholo.Rpc","IPokemonSettings");
   auto gm_iface=r.cls("","IGameMasterData");
   require(reinterpret_cast<bool(*)(void*,void*)>(r.boot.meta.class_is_assignable_from)(gm_iface,r.b.object_class(r.get(request.game_master))),"Invalid Game Master object");
   auto get_all=r.method(bag_cls,"get_AllPokemon","System.Collections.Generic.ICollection<Holoholo.Rpc.PokemonProto>");
   auto get_settings=r.method(r.b.pokemon_class,"GetPokemonSettings","Holoholo.Rpc.IPokemonSettings",{"IGameMasterData","System.Boolean"});
   void* prediction_species=nullptr;void* prediction_form=nullptr;
   auto validate_prediction=[&](){
    if(!request.prediction_reference)return;
    r.exact(request.prediction_reference,r.b.contest_class);
    require(r.value<int>(request.prediction_reference,prediction_species)==0&&
      r.value<int>(request.prediction_reference,prediction_form)==0,"Prediction reference changed");
   };
   if(request.prediction_reference){
    prediction_species=r.field(r.b.contest_class,"ScalarScoreReferencePokemon","Holoholo.Rpc.HoloPokemonId",72,4,PGR_MD_FIELD_ENUM_I32);
    prediction_form=r.field(r.b.contest_class,"ScalarScoreReferencePokemonForm","Form",76,4,PGR_MD_FIELD_ENUM_I32);
    validate_prediction();
   }
   void* id=r.b.pokemon_id_field;
   auto species=r.field(r.b.pokemon_class,"pokemonId_","System.Int32",40,4);
   auto created=r.field(r.b.pokemon_class,"creationTimeMs_","System.Int64",160,8);
   auto egg=r.field(r.b.pokemon_class,"isEgg_","System.Boolean",80,1);
   void* input_fields[]={
    r.field(r.b.pokemon_class,"heightM_","System.Single",104,4),
    r.field(r.b.pokemon_class,"weightKg_","System.Single",108,4),
    r.field(r.b.pokemon_class,"individualAttack_","System.Int32",112,4),
    r.field(r.b.pokemon_class,"individualDefense_","System.Int32",116,4),
    r.field(r.b.pokemon_class,"individualStamina_","System.Int32",120,4),
    r.field(r.b.pokemon_class,"size_","Holoholo.Rpc.HoloPokemonSize",424,4,PGR_MD_FIELD_ENUM_I32)};
   auto display_cls=r.cls("Holoholo.Rpc","PokemonDisplayProto");
   auto display=r.field(r.b.pokemon_class,"pokemonDisplay_","Holoholo.Rpc.PokemonDisplayProto",216,0,PGR_MD_FIELD_REF);
   auto form=r.field(display_cls,"Form","Form",28,4,PGR_MD_FIELD_ENUM_I32);
   auto inputs=[&](void* pokemon){
    std::array<uint32_t,7> result{};
    for(unsigned i=0;i<6;++i)r.b.field_get(pokemon,input_fields[i],&result[i]);
    float height=0,weight=0;std::memcpy(&height,&result[0],4);std::memcpy(&weight,&result[1],4);
    require(std::isfinite(height)&&height>0&&std::isfinite(weight)&&weight>0&&
      result[2]<=15&&result[3]<=15&&result[4]<=15,"Invalid Pokemon scoring inputs");
    void* d=nullptr;r.b.field_get(pokemon,display,&d);
    require(d&&r.b.object_class(d)==display_cls,"Invalid Pokemon display");r.b.field_get(d,form,&result[6]);return result;
   };
   int type_values[kTypes];std::set<int> unique;int none=literal(r,type_cls,"PokemonTypeNone");
   for(unsigned t=0;t<kTypes;++t){std::string name="PokemonType";name+=type_names[t];type_values[t]=literal(r,type_cls,name.c_str());require(type_values[t]!=none&&unique.insert(type_values[t]).second,"Ambiguous Pokemon type enum");}
   Root collection(r,r.invoke(get_all,request.bag));auto cc=r.b.object_class(r.get(collection.h));
   auto count=r.method(cc,"get_Count","System.Int32");auto copy=r.method(cc,"CopyTo","System.Void",{"Holoholo.Rpc.PokemonProto[]","System.Int32"});
   int n=scalar(r,count,collection.h,int_cls);require(n>=0&&n<=int(kMaxPokemon),"Inventory bound exceeded");
   auto allocate=r.symbol<void*(*)(void*,uintptr_t)>("il2cpp_array_new");
   auto length=r.symbol<uintptr_t(*)(void*)>("il2cpp_array_length");
   auto rank=r.symbol<int(*)(void*)>("il2cpp_class_get_rank");
   auto element=r.symbol<void*(*)(void*)>("il2cpp_class_get_element_class");
   auto element_size=r.symbol<int(*)(void*)>("il2cpp_array_element_size");
   r.gate();Root array(r,allocate(r.b.pokemon_class,uintptr_t(n)));
   auto ac=r.b.object_class(r.get(array.h));require(rank(ac)==1&&element(ac)==r.b.pokemon_class&&element_size(ac)==8,"Unsupported Pokemon array layout");
   int zero=0;void* args[]={r.get(array.h),&zero};r.invoke(copy,collection.h,args);
   require(length(r.get(array.h))==uintptr_t(n)&&scalar(r,count,collection.h,int_cls)==n,"Inventory changed during copy");
   std::set<uint64_t> seen;std::vector<Identity> identities;identities.reserve(n);built.reserve(n);
   auto at=[&](uintptr_t h,int i){void* object=nullptr;auto base=static_cast<const char*>(r.get(h));std::memcpy(&object,base+32+size_t(i)*8,8);return object;};
   std::map<void*,std::array<void*,2>> type_getters;
   for(int i=0;i<n;++i){
    Root pokemon(r,at(array.h,i));r.exact(pokemon.h,r.b.pokemon_class);
    Identity identity{r.value<uint64_t>(pokemon.h,id),r.value<int>(pokemon.h,species),r.value<int64_t>(pokemon.h,created),r.value<uint8_t>(pokemon.h,egg)};
    require(identity.id&&seen.insert(identity.id).second&&identity.egg<=1,"Malformed inventory identity");
    if(identity.egg){identities.push_back(identity);continue;}
    identity.score_inputs=inputs(r.get(pokemon.h));identities.push_back(identity);
    require(identity.species>0&&identity.species<=2000&&identity.created>0,"Malformed Pokemon facts");
    bool use_override=false;void* settings_args[]={r.get(request.game_master),&use_override};
    Root settings(r,r.invoke(get_settings,pokemon.h,settings_args));auto sc=r.b.object_class(r.get(settings.h));
    require(reinterpret_cast<bool(*)(void*,void*)>(r.boot.meta.class_is_assignable_from)(settings_iface,sc),"Invalid Pokemon settings");
    auto methods=type_getters.find(sc);
    if(methods==type_getters.end())methods=type_getters.emplace(sc,std::array<void*,2>{
      r.method(sc,"get_Type1","Holoholo.Rpc.HoloPokemonType"),
      r.method(sc,"get_Type2","Holoholo.Rpc.HoloPokemonType")}).first;
    Row row;row.id=identity.id;row.created=uint64_t(identity.created);row.species=identity.species;
    for(unsigned which=0;which<2;++which){
     int v=scalar(r,methods->second[which],settings.h,type_cls);
     if(v==none){require(which==1,"Missing primary type");continue;}
     bool found=false;for(unsigned t=0;t<kTypes;++t)if(v==type_values[t]){require(!(row.types&(1u<<t)),"Duplicate Pokemon type");row.types|=1u<<t;found=true;}
     require(found,"Unknown Pokemon type");
    }
    uintptr_t previous_reference=0;double previous_score=0;
    for(unsigned t=0;t<kTypes;++t)if(row.types&(1u<<t)){
     uintptr_t reference=request.contests[t]?request.contests[t]:request.prediction_reference;
     require(reference,"Missing Showcase scoring reference");ScoreRequest score=request.gate;
     validate_prediction();
     score.pokemon=pokemon.h;score.pokemon_id=row.id;score.contest=reference;
     if(reference==previous_reference)row.scores[t]=previous_score;
     else require(read_score(r.b,score,row.scores[t])==ScoreStatus::valid,"Showcase score unavailable");
     previous_reference=reference;previous_score=row.scores[t];
    }
    built.push_back(row);
   }
   // Fresh copy detects same-count replacement as well as removals/additions.
   Root final_collection(r,r.invoke(get_all,request.bag));r.exact(final_collection.h,cc);
   require(scalar(r,count,final_collection.h,int_cls)==n,"Inventory count changed");
   r.gate();Root final_array(r,allocate(r.b.pokemon_class,uintptr_t(n)));
   r.exact(final_array.h,ac);
   void* final_args[]={r.get(final_array.h),&zero};r.invoke(copy,final_collection.h,final_args);
   require(length(r.get(final_array.h))==uintptr_t(n),"Inventory array changed");
   for(int i=0;i<n;++i){
    void* p=at(final_array.h,i);require(p&&r.b.object_class(p)==r.b.pokemon_class,"Inventory element changed");
    Identity current{};r.b.field_get(p,id,&current.id);r.b.field_get(p,species,&current.species);r.b.field_get(p,created,&current.created);r.b.field_get(p,egg,&current.egg);
    const auto& old=identities[i];require(current.id==old.id&&current.species==old.species&&current.created==old.created&&current.egg==old.egg,"Inventory changed while scoring");
    if(!current.egg)require(inputs(p)==old.score_inputs,"Pokemon changed while scoring");
   }
   validate_prediction();
  }
  r.gate();rows=std::move(built);return true;
 }catch(const Failure& e){error=e.text;}catch(...){error="Inventory scan failed";}
 rows.clear();return false;
}
}

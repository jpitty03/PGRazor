#include "incense_guard.h"
#include "incense_guard_policy.h"
#include "shared_metadata.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include <dlfcn.h>
#include <cstring>
#include <set>
#include <string>
#include <memory>

namespace {
using namespace pgr_shared_meta;
struct Failure {const char *message;};
void require(bool ok,const char *message){if(!ok)throw Failure{message};}
template<class F>F fn(void *p){require(p,"missing IL2CPP export");return reinterpret_cast<F>(p);}
PgrBootBinding boot{};
uint64_t owner=0;
bool active=false,blocked=false;
std::string token,last_status;
std::set<uint64_t> seen;
const PgrSharedMetadataOutput *md=nullptr;
void *exp(const char *name){std::string s="il2cpp_";s+=name;void *p=dlsym(boot.handle,s.c_str());require(p,name);return p;}
struct Root {
 uintptr_t handle=0;
 explicit Root(void *v){if(v){handle=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(v,0);require(handle,"GC root failed");}}
 Root(const Root&)=delete;Root& operator=(const Root&)=delete;
 ~Root(){if(handle)fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(handle);}
 void *get()const{return handle?fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(handle):nullptr;}
};
std::unique_ptr<Root> pending;
uint64_t pending_id=0;
int pending_species=0,pending_form=0;
void *klass(void *o){require(o,"missing managed object");return fn<void*(*)(void*)>(boot.runtime.object_get_class)(o);}
std::string type_name(void *t){char *p=fn<char*(*)(void*)>(boot.meta.type_get_name)(t);require(p,"missing managed type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
void *cls(const char *ns,const char *name){
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api,"metadata API unavailable");
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult out{};require(api->resolve_class(&q,&out)==0,name);return const_cast<void*>(out.klass);
}
void *method(void *k,const char *name,const char *ret){
 void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,0);require(m,name);
 require(fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(m)==0,name);
 require(type_name(fn<void*(*)(void*)>(boot.meta.method_get_return_type)(m))==ret,name);return m;
}
void *typed_field(void *k,const char *name,const char *type){
 void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);
 require(!(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10),"observer field must be instance");
 require(type_name(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))==type,name);return f;
}
template<class T>T field(void *o,void *k,const char *name,const char *type){T v{};void *f=typed_field(k,name,type);fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;}
void *invoke(void *m,void *o,void **args=nullptr){require(m,"missing shared method");void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed observer call threw");return v;}
void *call(void *o,const char *name,const char *ret){return invoke(method(klass(o),name,ret),o);}
template<class T>T value(void *v,const char *type){Root box(v);require(box.get(),"missing boxed observer value");require(type_name(fn<void*(*)(void*)>(boot.meta.class_get_type)(klass(box.get())))==type,"observer boxed type mismatch");void *p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(box.get());require(p,"observer unbox failed");T x{};memcpy(&x,p,sizeof(x));return x;}
bool emit(const std::string &kind,const std::string &json){auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));if(!tr)return false;PgrRuntimeTransportString s[]={{kind.c_str(),uint32_t(kind.size())},{json.c_str(),uint32_t(json.size())}};return tr->emit(19,nullptr,0,s,2)==0;}
void status(const char *message,bool error=true){std::string key=std::string(error?"error:":"ready:")+message;if(last_status==key)return;if(emit("incense-session",std::string("{\"kind\":\"incense-session\",\"token\":\"")+token+"\",\"phase\":\""+(error?"error":"ready")+"\",\"message\":\""+message+"\"}"))last_status=key;}
int literal(void *k,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);require((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)==0x8050,"Galarian form is not enum literal");int v=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&v);return v;}
void scan(){
 void *display_class=cls("Holoholo.Rpc","PokemonDisplayProto");
 void *form_field=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(display_class,"Form");require(form_field,"PokemonDisplayProto.Form unavailable");
 void *form_type=fn<void*(*)(void*)>(boot.meta.field_get_type)(form_field);
 void *form_class=fn<void*(*)(void*)>(boot.meta.class_from_type)(form_type);require(form_class,"PokemonDisplayProto.Form class unavailable");
 int a=literal(form_class,"ArticunoGalarian"),z=literal(form_class,"ZapdosGalarian"),m=literal(form_class,"MoltresGalarian");
 require(a!=z&&a!=m&&z!=m&&a>0&&z>0&&m>0,"invalid Galarian form literals");
 for(const char *name:{"IncenseMapPokemon","WildMapPokemon"}){
  void *k=cls("Niantic.Holoholo.Map",name);
  Root type(fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k)));
  require(type.get(),"missing scene query type");void *args[]={type.get()};
  Root array(invoke(const_cast<void*>(md->opt_handles[H_VGP_M_FIND_ALL]),nullptr,args));require(array.get(),"map pokemon scene query returned null");
  size_t count=fn<size_t(*)(void*)>(exp("array_length"))(array.get());require(count<=2048,"map pokemon scene query exceeded bound");
  for(size_t i=0;i<count;i++){
   // IL2CPP LP64 Object[] layout, shared with the existing scene discovery modules.
   Root pokemon(*reinterpret_cast<void**>(static_cast<char*>(array.get())+32+i*8));
   if(!pokemon.get())continue;require(klass(pokemon.get())==k,"map pokemon scene class mismatch");
   void *alive_args[]={pokemon.get()};if(!value<bool>(invoke(const_cast<void*>(md->methods[5]),nullptr,alive_args),"System.Boolean"))continue;
   if(value<bool>(call(pokemon.get(),"get_Destroyed","System.Boolean"),"System.Boolean"))continue;
   if(!strcmp(name,"WildMapPokemon")&&!value<bool>(call(pokemon.get(),"get_VisibleOnMap","System.Boolean"),"System.Boolean"))continue;
   int species=value<int>(call(pokemon.get(),"get_PokemonId","System.Int32"),"System.Int32");
   if(species<144||species>146)continue;
   Root display(call(pokemon.get(),"get_PokemonDisplay","Holoholo.Rpc.PokemonDisplayProto"));require(display.get(),"visible bird display not loaded");require(klass(display.get())==display_class,"bird display class mismatch");
   int form=0;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(display.get(),form_field,&form);
   if(!pgr_incense_guard::bird(species,form,a,z,m))continue;
   blocked=true;
   uint64_t id=value<uint64_t>(call(pokemon.get(),"get_EncounterId","System.UInt64"),"System.UInt64");require(id!=0,"visible Galarian bird encounter ID unavailable");
   if(seen.count(id))continue;require(seen.size()<512,"bird session dedup capacity reached");
   std::string json="{\"kind\":\"incense-bird\",\"token\":\""+token+"\",\"encounterId\":\""+std::to_string(id)+"\",\"species\":"+std::to_string(species)+",\"form\":"+std::to_string(form)+"}";
   require(emit("incense-bird",json),"bird event delivery unavailable");seen.insert(id);
   if(!pending){pending.reset(new Root(pokemon.get()));require(pending->get(),"bird UI candidate root unavailable");pending_id=id;pending_species=species;pending_form=form;}
   return;
  }
 }
}
}
bool pgr_incense_guard_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(owner&&owner!=tid)return true;
 auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));
 char text[257]{};uint32_t length=0;pgr_incense_guard::Settings settings;
 PgrRuntimeTransportSample before{},after{};
 bool valid=tr&&tr->sample(&before)==0&&before.snapshot_fresh&&!before.fault&&tr->get_string(12,text,sizeof(text),&length)==0&&pgr_incense_guard::parse(text,length,settings)&&tr->sample(&after)==0&&after.snapshot_fresh&&!after.fault&&before.snapshot_seq==after.snapshot_seq;
 if(!valid){if(active){blocked=true;status("Incense observer settings unavailable; automatic catches paused");}return active;}
 if(!settings.active){pgr_incense_guard_stop(tid);return false;}
 if(!active||token!=settings.token){pending.reset();seen.clear();last_status.clear();blocked=false;token=settings.token;}
 active=true;owner=tid;
 try{
  require(tid!=0,"incense observer owner unavailable");
  auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(api&&api->snapshot(&boot)==0,"incense observer runtime bootstrap unavailable");
  md=metadata;require(md,"incense observer shared metadata unavailable");
  scan();if(!blocked)status("Manual incense session observer active",false);
 }catch(const Failure &f){blocked=true;status(f.message);}catch(...){blocked=true;status("Incense observer failed; automatic catches paused");}
 return blocked;
}
void pgr_incense_guard_open(uint64_t tid){
 if(!active||!blocked||!tid||owner!=tid||!pending)return;
 // Consume before any managed work: a throwing/no-op request is never replayed.
 std::unique_ptr<Root> target=std::move(pending);
 auto report=[](const char *phase,const char *message){emit("incense-open",std::string("{\"kind\":\"incense-open\",\"token\":\"")+token+"\",\"phase\":\""+phase+"\",\"message\":\""+message+"\"}");};
 try{
  void *base=cls("Niantic.Holoholo.Map","MapPokemon");
  require(target->get(),"Bird no longer loaded; open encounter manually");
  void *alive_args[]={target->get()};require(value<bool>(invoke(const_cast<void*>(md->methods[5]),nullptr,alive_args),"System.Boolean"),"Bird no longer active; open encounter manually");
  require(!value<bool>(call(target->get(),"get_Destroyed","System.Boolean"),"System.Boolean"),"Bird destroyed before opening");
  require(field<bool>(target->get(),base,"_selectionEnabled","System.Boolean"),"Bird selection disabled; open encounter manually");
  Root map(field<void*>(target->get(),base,"mapExploreState","Niantic.Holoholo.Map.IMapExploreState"));require(map.get(),"Map explore state unavailable");
  require(value<bool>(call(map.get(),"get_IsActiveAndVisible","System.Boolean"),"System.Boolean"),"Map is not active and visible; open encounter manually");
  require(value<uint64_t>(call(target->get(),"get_EncounterId","System.UInt64"),"System.UInt64")==pending_id,"Bird encounter changed before opening");
  require(value<int>(call(target->get(),"get_PokemonId","System.Int32"),"System.Int32")==pending_species,"Bird species changed before opening");
  Root display(call(target->get(),"get_PokemonDisplay","Holoholo.Rpc.PokemonDisplayProto"));require(display.get(),"Bird display unavailable before opening");
  void *ff=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(display.get()),"Form");require(ff,"Bird form unavailable before opening");int form=0;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(display.get(),ff,&form);require(form==pending_form,"Bird form changed before opening");
  void *tap=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(base,"OnTap",2);require(tap,"MapPokemon.OnTap unavailable");
  require(fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(tap)==2,"MapPokemon.OnTap arity mismatch");
  require(type_name(fn<void*(*)(void*)>(boot.meta.method_get_return_type)(tap))=="System.Void","MapPokemon.OnTap return mismatch");
  uint32_t implementation_flags=0;uint32_t flags=fn<uint32_t(*)(void*,uint32_t*)>(boot.meta.method_get_flags)(tap,&implementation_flags);require(!(flags&0x10),"MapPokemon.OnTap must be instance");
  require(type_name(fn<void*(*)(void*,uint32_t)>(boot.meta.method_get_param)(tap,0))=="System.Object"&&type_name(fn<void*(*)(void*,uint32_t)>(boot.meta.method_get_param)(tap,1))=="System.EventArgs","MapPokemon.OnTap parameter mismatch");
  // The verified normal tap body ignores sender/e and preserves allowHidden=false.
  void *args[]={nullptr,nullptr};invoke(tap,target->get(),args);
  report("opened","Encounter opening requested; catch manually when the game presents it");
 }catch(const Failure &f){report("error",f.message);}catch(...){report("error","Encounter opening request failed; open it manually");}
}
void pgr_incense_guard_stop(uint64_t tid){if(owner&&owner!=tid)return;pending.reset();active=false;blocked=false;owner=0;token.clear();seen.clear();last_status.clear();md=nullptr;}

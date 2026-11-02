#include "auto_heal.h"
#include "auto_heal_policy.h"
#include "patch_work_budget.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "shared_host.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <dlfcn.h>
#include <unistd.h>
namespace {
using namespace pgr_shared_meta;
PgrBootBinding b{};const PgrMetaApi*md=nullptr;const PgrSharedMetadataOutput*shared=nullptr;
const auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));
std::atomic<bool> stopped{false};bool ready=false,attempted=false,busy=false,fault=false;
uint64_t command=0,generation=0,next=0;unsigned mask=0;std::string last;
pgr_heal::Policy policy;
template<class T>T fn(void*p){return reinterpret_cast<T>(p);}
void status(const char*text){if(last==text)return;last=text;PgrRuntimeTransportString v[]={{"auto-heal",9},{text,uint32_t(std::strlen(text))}};tr->emit(19,nullptr,0,v,2);}
void detail(const char*kind,const pgr_heal::Choice&q){char text[200];std::snprintf(text,sizeof(text),"{\"pokemonId\":\"%llu\",\"itemSlot\":%d,\"hp\":%d,\"maxHp\":%d}",(unsigned long long)q.id,q.item,q.hp,q.max);PgrRuntimeTransportString v[]={{kind,uint32_t(std::strlen(kind))},{text,uint32_t(std::strlen(text))}};tr->emit(19,nullptr,0,v,2);}
struct Root {
 uintptr_t h=0;Root()=default;explicit Root(void*p){if(p){h=fn<uintptr_t(*)(void*,int)>(b.runtime.gchandle_new)(p,1);if(!h)fault=true;}}
 Root(Root&&r):h(std::exchange(r.h,0)){}Root(const Root&)=delete;
 ~Root(){auto old=std::exchange(h,0);if(old)fn<void(*)(uintptr_t)>(b.runtime.gchandle_free)(old);}
 void*get()const{void*p=h?fn<void*(*)(uintptr_t)>(b.runtime.gchandle_get_target)(h):nullptr;if(h&&!p)fault=true;return p;}
};
bool is_type(void*o,void*k){return o&&k&&fn<bool(*)(void*,void*)>(b.meta.class_is_assignable_from)(k,fn<void*(*)(void*)>(b.runtime.object_get_class)(o));}
Root invoke(const void*m,const Root*o=nullptr,void**args=nullptr){if(!m){fault=true;return {};}
 void*self=o?o->get():nullptr;if(o&&!is_type(self,fn<void*(*)(const void*)>(b.meta.method_get_class)(m))){fault=true;return {};}
 void*exc=nullptr;void*p=fn<void*(*)(const void*,void*,void**,void**)>(b.runtime.runtime_invoke)(m,self,args,&exc);if(exc){fault=true;return {};}return Root(p);
}
template<class T>T read(const Root&o,void*f){T v{};if(!o.get()||!f){fault=true;return v;}fn<void(*)(void*,void*,void*)>(b.runtime.field_get_value)(o.get(),f,&v);return v;}
template<class T>T scalar(const Root&r){T v{};if(!r.get()){fault=true;return v;}void*p=fn<void*(*)(void*)>(b.runtime.object_unbox)(r.get());if(!p){fault=true;return v;}std::memcpy(&v,p,sizeof(v));return v;}
void*klass(const char*ns,const char*name,const char*canonical){PgrMetaClassQuery q{1,sizeof(q),ns,name,canonical,PGR_MD_MATCH_EXACT,0};PgrMetaClassResult r{};return md->resolve_class(&q,&r)?nullptr:const_cast<void*>(r.klass);}
std::string type_name(void*t){if(!t)return {};char*p=fn<char*(*)(void*)>(b.meta.type_get_name)(t);std::string s=p?p:"";if(p)fn<void(*)(void*)>(b.meta.mem_free)(p);return s;}
const void*method(void*k,const char*n,const char*ret,unsigned count=0,const char*a=nullptr,const char*c=nullptr){if(!k)return nullptr;PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=k;q.name=n;q.return_type=ret;q.return_match=PGR_MD_MATCH_EXACT;q.param_count=count;q.param_types[0]=a;q.param_types[1]=c;q.param_match[0]=q.param_match[1]=PGR_MD_MATCH_EXACT;PgrMetaMethodResult r{};return md->validate_method(&q,&r)?nullptr:r.method;}
void*field(void*k,const char*n,const char*type){if(!k)return nullptr;void*f=fn<void*(*)(void*,const char*)>(b.meta.class_get_field_from_name)(k,n);if(!f||(fn<uint32_t(*)(void*)>(b.meta.field_get_flags)(f)&0x50))return nullptr;return type_name(fn<void*(*)(void*)>(b.meta.field_get_type)(f))==type?f:nullptr;}
void*pk_class=nullptr,*bag_class=nullptr,*item_class=nullptr,*map_class=nullptr,*pk_contract=nullptr,*item_contract=nullptr;
void *id_field=nullptr,*hp_field=nullptr,*max_field=nullptr,*egg_field=nullptr,*active_field=nullptr;
const void *all_method=nullptr,*get_method=nullptr,*use_method=nullptr,*visible_method=nullptr,*enumerate=nullptr,*move=nullptr,*current=nullptr,*dispose=nullptr;
void *virtual_method=nullptr,*type_object=nullptr,*static_value=nullptr;int items[6]{};
void*exported(const char*n){void*p=dlsym(b.handle,n);PgrMetaCodeQuery q{1,sizeof(q),p,4,nullptr,0,0};return p&&!md->validate_code(&q)?p:nullptr;}
bool prepare(){
 const auto boot=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));if(boot->snapshot(&b))return false;
 shared=static_cast<const PgrSharedHostApi*>(pgrazor_shared_host_api(1))->metadata();if(!shared||shared->optional[OPT_ITEMS_LOOKUP]!=PGR_SHARED_OPT_AVAILABLE||shared->optional[OPT_ITEMS]!=PGR_SHARED_OPT_AVAILABLE)return false;
 md=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));attempted=true;
 pk_class=klass("Holoholo.Rpc","PokemonProto","Holoholo.Rpc.PokemonProto");bag_class=klass("Niantic.Holoholo.Internal","PokemonBagImpl","Niantic.Holoholo.Internal.PokemonBagImpl");item_class=klass("Niantic.Holoholo.Internal","ItemBagImpl","Niantic.Holoholo.Internal.ItemBagImpl");
 pk_contract=klass("Niantic.Holoholo","IPokemonBag","Niantic.Holoholo.IPokemonBag");item_contract=klass("Niantic.Holoholo","IItemBag","Niantic.Holoholo.IItemBag");map_class=klass("Niantic.Holoholo.Map","MapExploreState","Niantic.Holoholo.Map.MapExploreState");
 auto*state=klass("Niantic.Zeppelin.GameStates","GameState","Niantic.Zeppelin.GameStates.GameState");
 active_field=field(state,"<IsActiveState>k__BackingField","System.Boolean");if(!state||!map_class||!fn<bool(*)(void*,void*)>(b.meta.class_is_assignable_from)(state,map_class))active_field=nullptr;visible_method=method(map_class,"get_IsActiveAndVisible","System.Boolean");
 id_field=field(pk_class,"id_","System.UInt64");hp_field=field(pk_class,"stamina_","System.Int32");max_field=field(pk_class,"maxStamina_","System.Int32");egg_field=field(pk_class,"isEgg_","System.Boolean");
 all_method=method(bag_class,"get_AllPokemon",nullptr);if(all_method&&type_name(fn<void*(*)(const void*)>(b.meta.method_get_return_type)(all_method)).find("System.Collections.Generic.ICollection")!=0)all_method=nullptr;
 get_method=method(bag_class,"GetPokemon","Holoholo.Rpc.PokemonProto",1,"System.UInt64");
 use_method=method(item_class,"UseItemOnPokemon",nullptr,2,"Holoholo.Rpc.Item","System.UInt64");if(use_method&&type_name(fn<void*(*)(const void*)>(b.meta.method_get_return_type)(use_method)).find("Niantic.Promises.IPromise")!=0)use_method=nullptr;
 enumerate=method(klass("System.Collections","IEnumerable","System.Collections.IEnumerable"),"GetEnumerator","System.Collections.IEnumerator");move=method(klass("System.Collections","IEnumerator","System.Collections.IEnumerator"),"MoveNext","System.Boolean");current=method(klass("System.Collections","IEnumerator","System.Collections.IEnumerator"),"get_Current","System.Object");dispose=method(klass("System","IDisposable","System.IDisposable"),"Dispose","System.Void");
 virtual_method=exported("il2cpp_object_get_virtual_method");type_object=exported("il2cpp_type_get_object");static_value=exported("il2cpp_field_static_get_value");
 auto*item=klass("Holoholo.Rpc","Item","Holoholo.Rpc.Item");bool literals=item&&static_value&&fn<bool(*)(void*)>(b.meta.class_is_enum)(item)&&type_name(fn<void*(*)(void*)>(b.meta.class_enum_basetype)(item))=="System.Int32";
 const char*names[]={"Potion","SuperPotion","HyperPotion","MaxPotion","Revive","MaxRevive"};
 if(literals)for(int i=0;i<6;i++){auto*f=fn<void*(*)(void*,const char*)>(b.meta.class_get_field_from_name)(item,names[i]);if(!f||(fn<uint32_t(*)(void*)>(b.meta.field_get_flags)(f)&0x8050)!=0x8050){literals=false;break;}fn<void(*)(void*,void*)>(static_value)(f,&items[i]);if(items[i]<=0)literals=false;for(int j=0;j<i;j++)if(items[i]==items[j])literals=false;}
 ready=pk_class&&bag_class&&item_class&&pk_contract&&item_contract&&map_class&&state&&active_field&&visible_method&&id_field&&hp_field&&max_field&&egg_field&&all_method&&use_method&&enumerate&&move&&current&&dispose&&virtual_method&&type_object&&literals;
 status(ready?"Ready; waiting for the map":!all_method?"Unavailable: Pokemon bag contract":!use_method?"Unavailable: UseItemOnPokemon contract":!literals?"Unavailable: healing item enum":"Unavailable: healing metadata mismatch");return ready;
}
Root type(void*k){if(!k){fault=true;return {};}return Root(fn<void*(*)(void*)>(type_object)(fn<void*(*)(void*)>(b.meta.class_get_type)(k)));}
Root resolve(const Root&container,void*contract,void*concrete){auto t=type(contract);void*v=t.get();void*args[]={v};auto result=invoke(shared->opt_handles[H_IL_RESOLVE_M],&container,args);if(!is_type(result.get(),concrete))fault=true;return result;}
Root virt(const void*m,const Root&o){auto*v=fn<void*(*)(void*,const void*)>(virtual_method)(o.get(),m);return invoke(v,&o);}
struct Enumerator {Root root;explicit Enumerator(const Root&collection):root(virt(enumerate,collection)){}~Enumerator(){if(root.get()&&dispose){auto r=virt(dispose,root);}}};
bool rows(const Root&bag,std::vector<pgr_heal::Pokemon>&out,uint64_t wanted=0,pgr_heal::Pokemon*found=nullptr){
 auto collection=invoke(all_method,&bag);if(fault||!collection.get())return false;Enumerator it(collection);if(fault||!it.root.get())return false;
 for(unsigned i=0;i<=25000&&!fault;i++){
  auto moved=virt(move,it.root);if(fault)return false;if(!scalar<uint8_t>(moved))return !fault;if(i==25000){fault=true;break;}
  auto pk=virt(current,it.root);if(fault)return false;if(!is_type(pk.get(),pk_class))continue;
  pgr_heal::Pokemon p{read<uint64_t>(pk,id_field),read<int>(pk,hp_field),read<int>(pk,max_field),read<uint8_t>(pk,egg_field)!=0};
  if(wanted){if(p.id==wanted){if(found)*found=p;return !fault;}}else out.push_back(p);
 }return false;
}
struct Config {uint64_t command=0,generation=0;unsigned mask=0;bool allowed=false,valid=false;};
Config config(){Config c{};PgrRuntimeTransportSample a{},z{};char text[100]{};uint32_t len=0;unsigned long long cmd=0;unsigned flags=0,on=0;int end=0;
 if(stopped||tr->sample(&a)||!a.snapshot_fresh||a.fault||tr->get_string(14,text,sizeof(text)-1,&len)||len>=sizeof(text))return c;text[len]=0;
 if(std::sscanf(text,"v1:%llu:%u:%u%n",&cmd,&flags,&on,&end)!=3||end!=int(len)||cmd>INT64_MAX||flags>3||on>1||tr->sample(&z)||!z.snapshot_fresh||z.fault||a.snapshot_seq!=z.snapshot_seq||a.generation!=z.generation)return c;
 return {uint64_t(cmd),a.generation,flags,on!=0,true};
}
bool authorized(const Config&c){auto fresh=config();return !fault&&fresh.valid&&fresh.allowed&&fresh.mask==c.mask&&fresh.command==c.command&&fresh.generation==c.generation;}
bool map_live(const Root&map){if(!is_type(map.get(),map_class)||!read<uint8_t>(map,active_field))return false;auto visible=invoke(visible_method,&map);return !fault&&scalar<uint8_t>(visible)!=0;}
}
void pgr_auto_heal_stop(){stopped=true;}
void pgr_auto_heal_tick(uint64_t now,uint64_t tid,bool cleanup){
 if(tid!=uint64_t(gettid())||busy)return;busy=true;struct Guard{~Guard(){busy=false;}} guard;
 if(cleanup||stopped){policy.reset();return;}auto c=config();if(!c.valid)return;
 // Panel changes revoke the current call, but preserve a revived Pokemon's
 // healing chain and any in-flight item while the owner is temporarily paused.
 if(c.generation!=generation||c.mask!=mask){generation=c.generation;mask=c.mask;policy.reset();fault=false;}command=c.command;
 if(!c.mask){status("Off");return;}if(!c.allowed){status("Waiting for foreground map; settings pause");return;}if(fault){status("Paused after a managed call error; toggle off/on");return;}
 if(now<next)return;next=now+pgr_perf::heal_interval(policy.pending.id!=0);
 if(!ready){if(attempted)return;if(!prepare())return;}
 if(!fn<void*(*)()>(b.runtime.thread_current)())return;
 {
  auto map_type=type(map_class);void*map_arg=map_type.get();void*ma[]={map_arg};auto map=invoke(shared->opt_handles[H_IL_FIND_M],nullptr,ma);
  if(!map.get()||!map_live(map)){if(!fault)status("Waiting for the map");return;}
  auto scene_type=type(const_cast<void*>(shared->opt_handles[H_IL_MAP]));void*scene_arg=scene_type.get();void*sa[]={scene_arg};auto scene=invoke(shared->opt_handles[H_IL_FIND_M],nullptr,sa);
  if(!scene.get())return;auto container=invoke(shared->opt_handles[H_IL_CONTAINER_M],&scene);if(!container.get())return;
  auto bag=resolve(container,pk_contract,bag_class);auto item_bag=resolve(container,item_contract,item_class);
  std::vector<pgr_heal::Pokemon> inventory;if(fault||!rows(bag,inventory))return;
  int stock[6]{};for(int i=0;i<6&&!fault;i++){void*args[]={&items[i]};auto quantity=invoke(shared->opt_handles[H_IT_QUANTITY_M],&item_bag,args);stock[i]=scalar<int>(quantity);if(stock[i]<0)fault=true;}
  if(fault||!authorized(c))return;auto old=policy.pending;auto choice=policy.step(now,c.mask,inventory,stock);
  if(policy.confirmed)detail("auto-heal-confirmed",old);if(policy.timed_out)detail("auto-heal-timeout",old);
  if(choice.id||policy.pending.id)next=now+pgr_perf::heal_interval(true);
  if(!choice.id){status(policy.pending.id?"Waiting for HP confirmation":policy.healing.empty()?"Watching Pokemon HP":"Waiting for revives or potions");return;}
  pgr_heal::Pokemon final{};
  if(get_method){uint64_t target=choice.id;void*ga[]={&target};auto pk=invoke(get_method,&bag,ga);if(is_type(pk.get(),pk_class))final={read<uint64_t>(pk,id_field),read<int>(pk,hp_field),read<int>(pk,max_field),read<uint8_t>(pk,egg_field)!=0};}
  else {std::vector<pgr_heal::Pokemon> unused;rows(bag,unused,choice.id,&final);}
  if(fault||final.id!=choice.id||!pgr_heal::valid(final)||final.hp!=choice.hp||final.max!=choice.max||!map_live(map)||!authorized(c))return;
  uint64_t id=choice.id;int item=items[choice.item];void*args[]={&item,&id};detail("auto-heal-request",choice);auto promise=invoke(use_method,&item_bag,args);
  if(!fault)status(choice.item>=4?"Reviving Pokemon; waiting for HP change":"Healing Pokemon; waiting for HP change");
 }
 if(fault)status("Paused after a managed call error; toggle off/on");
}

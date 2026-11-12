// Executes production queue, managed request construction and confirmation flow.
#if defined(_MSC_VER)
// The fixture defines every optional weak hook itself; MSVC has no ELF weak attribute.
#define __attribute__(value)
#endif
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
extern "C" void *fixture_symbol(void *,const char *);
#define dlsym fixture_symbol
#include "evolve_transfer.cpp"
#undef dlsym
#include "transfer_policy.cpp"
namespace {
struct K {std::string name;bool value=false,enumeration=false;};
struct F {std::string name;K *type;};
struct M {std::string name;int count;};
struct O {K *type;std::map<std::string,uint64_t> data;std::u16string text;std::vector<O*> list;};
std::map<std::string,K> classes;std::map<std::string,F> fields;std::map<std::string,M> methods;
std::vector<std::unique_ptr<O>> objects;std::map<uintptr_t,void*> handles;uintptr_t handle_next=1;
std::map<uint64_t,O*> inventory;
O *bag,*gm,*player,*buddy,*map_object,*container,*account,*pk,*settings_object,*branch,*branches,*quests,*promise,*response,*evolved;
PgrSharedMetadataOutput fixture_md{};PgrBootBinding fixture_boot{};
std::string config="1|1|0|1|",keep_config,pvp_config="P2|0|0|0|150";
bool allowed=true,fresh=true,keep_all=false,pvp_protected=false,response_ready=false,poll_error=false,foreign_bag=false,batch_busy=false;
bool move_on_root=false;
bool showcase_protected=false;
int iv_attack=1,iv_defense=2,iv_stamina=3;
bool not_evolvable=false,special=false,synchronous=true,cancel_during_check=false,change_account=false;
int protect=127,stock=100,cost=12,evolution_calls=0,release_calls=0,next_target=2,result_status=1;
uint64_t returned_id=10;int returned_species=2;std::vector<std::string> messages;
K *type(const std::string &n){auto [it,newone]=classes.emplace(n,K{n});return &it->second;}
O *object(const std::string &n){objects.emplace_back(new O{type(n),{}, {}, {}});return objects.back().get();}
O *boxed_int(int v){O *o=object("System.Int32");o->data["value"]=v;return o;}
O *boxed_bool(bool v){O *o=object("System.Boolean");o->data["value"]=v;return o;}
uintptr_t gc_new(void *o,int){if(!o)return 0;
 if(move_on_root&&o==bag){move_on_root=false;O *new_bag=object("Bag");*new_bag=*bag;bag=new_bag;O *new_pk=object("Holoholo.Rpc.PokemonProto");*new_pk=*pk;pk=new_pk;inventory[10]=pk;o=bag;}
 handles[++handle_next]=o;return handle_next;}
void *gc_get(uintptr_t h){return handles.at(h);}void gc_free(uintptr_t h){assert(handles.erase(h)==1);}
void *object_class(void *o){return static_cast<O*>(o)->type;}
void *class_type(void *k){return k;}char *type_name(void *k){return strdup(static_cast<K*>(k)->name.c_str());}
void free_name(void *p){free(p);}bool value_type(void *k){return static_cast<K*>(k)->value;}
bool enum_type(void *k){return static_cast<K*>(k)->enumeration;}
void *unbox(void *o){return &static_cast<O*>(o)->data["value"];}
void *field_lookup(void *k,const char *name){
 std::string n=name,t="System.Int32";
 if(n=="id_"||n=="PokemonId")t="System.UInt64";
 if(n=="isEgg_"||n=="favorite_"||n=="isBad_"||n=="isComponent_"||n=="isFusion_"||n=="isActivelyTraining_"||n=="inEscrow_"||n=="MustBeBuddy"||n=="OnlyDaytime"||n=="OnlyNighttime"||n=="OnlyUpsideDown"||n=="OnlyDuskPeriod"||n=="OnlyFullMoon"||n=="ShouldHideButton"||n=="NoCandyCostViaTrade")t="System.Boolean";
 if(n=="deployedFortId_"||n=="deployedStationId_")t="System.String";
 if(n=="KmBuddyDistanceRequirement")t="System.Single";
 if(n=="QuestDisplay")t="Niantic.Protobuf.Collections.RepeatedField<Quest>";
 if(n==PGR_GAME_BATCH_EVOLUTION_CPP_ERIK_NAME)t="IGameMasterData";
 if(n=="EvolvedPokemon")t="Holoholo.Rpc.PokemonProto";
 if(n=="Evolution"||n=="Form"||n=="EvolutionItemRequirement"||n=="GenderRequirement"||n=="LureItemRequirement"||n=="TemporaryEvolution"||n=="EvolutionMoveRequirement"||n=="TargetPokemonId"||n=="TargetPokemonForm"||n=="Result")t="Test.Enum";
 auto key=static_cast<K*>(k)->name+"."+n;auto [it,inserted]=fields.emplace(key,F{n,type(t)});return &it->second;
}
void *field_type(void *f){return static_cast<F*>(f)->type;}int field_flags(void *f){return static_cast<F*>(f)->name=="Success"?0x8050:0;}
void field_read(void *o,void *field,void *out){auto *f=static_cast<F*>(field);auto *p=static_cast<O*>(o);
 if(cancel_during_check&&f->name=="favorite_"){allowed=false;cancel_during_check=false;}
 uint64_t v=p->data[f->name];size_t n=f->type->name=="System.Boolean"?1:(f->type->name=="System.UInt64"||!f->type->value?8:4);memcpy(out,&v,n);
}
void field_write(void *o,void *field,void *in){auto *f=static_cast<F*>(field);uint64_t v=0;size_t n=f->type->name=="System.UInt64"?8:4;memcpy(&v,in,n);static_cast<O*>(o)->data[f->name]=v;}
void field_static(void*,void*out){*static_cast<int*>(out)=1;}
void *method_lookup(void*,const char *name,int n){auto key=std::string(name)+std::to_string(n);auto [it,created]=methods.emplace(key,M{name,n});return &it->second;}
unsigned method_count(void*m){return static_cast<M*>(m)->count;}
void *object_new(void*k){return object(static_cast<K*>(k)->name);}
void *fixture_type_object(void*k){return k;}
int string_length(void*o){return static_cast<O*>(o)->text.size();}
const uint16_t *string_chars(void*o){return reinterpret_cast<const uint16_t*>(static_cast<O*>(o)->text.data());}
void *invoke_fixture(void *method,void *receiver,void **args,void **ex){
 *ex=nullptr;auto n=static_cast<M*>(method)->name;
 if(n=="find")return map_object;if(n=="container")return container;
 if(n=="resolve"){auto type_name=static_cast<K*>(args[0])->name;
  if(type_name=="Niantic.Holoholo.IPokemonBag")return foreign_bag?nullptr:bag;
  if(type_name=="Niantic.Holoholo.IPlayerService")return player;
  if(type_name=="Niantic.Holoholo.Buddy.IBuddyPokemonService")return buddy;
  assert(false&&"unknown service");
 }
 if(n=="get_PlayerId"){account->text=change_account?u"other":u"account";return account;}
 if(n=="GetPokemon"){auto it=inventory.find(*static_cast<uint64_t*>(args[0]));return it==inventory.end()?nullptr:it->second;}
 if(n=="GetIsBuddy"||n=="get_IsShadowPokemon"||n=="get_IsPurifiedPokemon"||n=="get_HasBeenTraded")return boxed_bool(false);
 if(n=="IsNoEvolvePokemon")return boxed_bool(not_evolvable);
 if(n=="GetPokemonSettings")return settings_object;
 if(n=="get_EvolutionBranch"){branch->data["Evolution"]=next_target;branch->data["CandyCost"]=cost;branch->data["OnlyDaytime"]=special;return branches;}
 if(n=="get_CandyToEvolve")return boxed_int(cost);
 if(n=="GetFamilyId"){O *o=object("Test.Enum");o->data["value"]=1;return o;}
 if(n=="get_Count")return boxed_int(static_cast<O*>(receiver)->list.size());
 if(n=="get_Item")return static_cast<O*>(receiver)->list.at(*static_cast<int*>(args[0]));
 if(n=="GetCandyCountForPokemonFamily")return boxed_int(stock);
 if(n==".ctor")return nullptr;
 if(n=="EvolvePokemon"){
  auto *req=static_cast<O*>(args[1]);assert(args[0]==inventory.at(req->data["PokemonId"]));assert(req->data["TargetPokemonId"]==uint64_t(next_target));
  assert(req->data["EvolutionItemRequirement"]==0&&req->data["EvolutionItemRequirementCount"]==0);++evolution_calls;response_ready=false;return promise;
 }
 assert(false&&"unexpected managed invoke");return nullptr;
}
int metadata_class(const PgrMetaClassQuery*q,PgrMetaClassResult*out){out->klass=type(std::string(q->name_space)+"."+q->name);return 0;}
int bootstrap(PgrBootBinding*out){*out=fixture_boot;return 0;}
int transport_sample(PgrRuntimeTransportSample*out){*out={};out->snapshot_fresh=fresh;out->snapshot_seq=1;return 0;}
int transport_string(uint32_t id,char*out,uint32_t cap,uint32_t*len){std::string s=id==22?config:id==4?keep_config:id==16?pvp_config:"";if(cap<=s.size())return -1;memcpy(out,s.c_str(),s.size()+1);*len=s.size();return 0;}
int transport_bool(uint32_t id,int*out){*out=id==5?allowed:id==35?keep_all:1;return 0;}
int transport_int(uint32_t,int32_t*out){*out=protect;return 0;}
int transport_emit(uint32_t,const int64_t*,uint32_t,const PgrRuntimeTransportString*s,uint32_t n){if(n)messages.emplace_back(s[0].ptr,s[0].len);return 0;}
int inspect(const PgrTransferFactsRequest*q,PgrTransferFactsOut*out){auto*p=static_cast<O*>(q->pokemon);*out={1,sizeof(*out),iv_attack,iv_defense,iv_stamina,int32_t(p->data["pokemonId_"]),255,uint32_t(p->data["favorite_"]?64:0),q->caught_id};return 1;}
int ready(){return 1;}
int submit(const PgrTransferExecRequest*q,uint64_t*token){assert(pgrazor_transfer_policy_api(1));assert(q->captured_id==job.id);assert(q->pokemon==inventory.at(job.id));++release_calls;*token=100+release_calls;
 if(synchronous){inventory.erase(job.id);PgrTransferExecEvent e{PGR_TX_EVENT_COMPLETE,0,*token,1,0};assert(pgr_evolve_transfer_event(&e));}return 1;
}
int poll_response(void*,uint32_t,uint64_t,PgrVgpResultsResult*out){*out={};if(poll_error)return -1;if(!response_ready)return 0;
 response->data["Result"]=result_status;response->data["EvolvedPokemon"]=reinterpret_cast<uint64_t>(evolved);response->data["CandyAwarded"]=1;
 evolved->data["id_"]=returned_id;evolved->data["pokemonId_"]=returned_species;out->value_handle=gc_new(response,0);return PGR_VR_DONE;
}
PgrRuntimeTransportApi transport_api{};PgrMetaApi meta_api{};PgrBootApi boot_api{};PgrSharedHostApi host_api{};PgrSharedHostTransferApi broker_api{};PgrTransferFactsApi facts_api{};PgrVgpResultsApi result_api{};
void fixture(){
 queue.clear();job=Job{};assert(handles.empty());seen.clear();objects.clear();inventory.clear();messages.clear();initialized=false;busy=false;owner=0;boot={};md=nullptr;
 move_on_root=false;iv_attack=1;iv_defense=2;iv_stamina=3;config="1|1|0|1|";keep_config="";pvp_config="P2|0|0|0|150";allowed=fresh=true;keep_all=pvp_protected=response_ready=poll_error=foreign_bag=batch_busy=not_evolvable=special=cancel_during_check=change_account=false;synchronous=true;protect=127;stock=100;cost=12;evolution_calls=release_calls=0;next_target=2;returned_id=10;returned_species=2;result_status=1;
 for(auto n:{"System.Int32","System.UInt64","System.Boolean","System.Single","Test.Enum"})type(n)->value=true;type("Test.Enum")->enumeration=true;
 bag=object("Bag");gm=object("IGameMasterData");bag->data[PGR_GAME_BATCH_EVOLUTION_CPP_ERIK_NAME]=reinterpret_cast<uint64_t>(gm);
 player=object("Player");buddy=object("Buddy");map_object=object("Map");container=object("Container");account=object("System.String");account->text=u"account";
 pk=object("Holoholo.Rpc.PokemonProto");pk->data["id_"]=10;pk->data["pokemonId_"]=1;inventory[10]=pk;
 settings_object=object("Settings");branch=object("Holoholo.Rpc.EvolutionBranchProto");branches=object("List");branches->list={branch};quests=object("Niantic.Protobuf.Collections.RepeatedField<Quest>");branch->data["QuestDisplay"]=reinterpret_cast<uint64_t>(quests);
 promise=object("Promise");response=object("Holoholo.Rpc.EvolvePokemonOutProto");evolved=object("Holoholo.Rpc.PokemonProto");
 fixture_md={};fixture_md.opt_handles[H_IL_MAP]=type("Map");fixture_md.opt_handles[H_IL_FIND_M]=method_lookup(nullptr,"find",1);fixture_md.opt_handles[H_IL_CONTAINER_M]=method_lookup(nullptr,"container",0);fixture_md.opt_handles[H_IL_RESOLVE_M]=method_lookup(nullptr,"resolve",1);
 fixture_boot={};auto &r=fixture_boot.runtime;auto &m=fixture_boot.meta;
 #define BIND(group,name,value) group.name=reinterpret_cast<void*>(value)
 BIND(r,gchandle_new,gc_new);BIND(r,gchandle_get_target,gc_get);BIND(r,gchandle_free,gc_free);BIND(r,object_get_class,object_class);BIND(r,runtime_invoke,invoke_fixture);BIND(r,field_get_value,field_read);BIND(r,field_set_value,field_write);BIND(r,field_static_get_value,field_static);BIND(r,object_new,object_new);BIND(r,object_unbox,unbox);BIND(r,string_chars,string_chars);
 BIND(m,class_get_type,class_type);BIND(m,type_get_name,type_name);BIND(m,mem_free,free_name);BIND(m,class_get_field_from_name,field_lookup);BIND(m,field_get_type,field_type);BIND(m,class_from_type,class_type);BIND(m,class_is_valuetype,value_type);BIND(m,class_is_enum,enum_type);BIND(m,field_get_flags,field_flags);BIND(m,class_get_method_from_name,method_lookup);BIND(m,method_get_param_count,method_count);
 #undef BIND
 boot_api.snapshot=bootstrap;meta_api.resolve_class=metadata_class;host_api.metadata=[](){return static_cast<const PgrSharedMetadataOutput*>(&fixture_md);};
 transport_api.sample=transport_sample;transport_api.get_string=transport_string;transport_api.get_bool=transport_bool;transport_api.get_int=transport_int;transport_api.emit=transport_emit;
 facts_api.inspect=inspect;broker_api.ready=ready;broker_api.submit=submit;result_api.poll=poll_response;
}
int offer(bool manual=true){PgrTransferExecRequest q{1,sizeof(q),10,1,uint64_t(gettid()),bag,pk,{}};q.input.species=1;return pgr_evolve_transfer_offer(&q,manual);}
void tick(){job.next=0;pgr_evolve_transfer_tick(gettid(),&fixture_md);}
void accept(){response_ready=true;tick();assert(job.await_inventory);}
void reconcile(uint64_t id=10){O *p=object("Holoholo.Rpc.PokemonProto");p->data["id_"]=id;p->data["pokemonId_"]=next_target;inventory.erase(job.sending.id);inventory[id]=p;stock-=cost-1;tick();}
}
void *fixture_symbol(void*,const char*n){if(!strcmp(n,"il2cpp_type_get_object"))return reinterpret_cast<void*>(fixture_type_object);if(!strcmp(n,"il2cpp_string_length"))return reinterpret_cast<void*>(string_length);return nullptr;}
const void *pgrazor_runtime_bootstrap_api(uint32_t){return &boot_api;}const void *pgrazor_runtime_metadata_api(uint32_t){return &meta_api;}
const void *pgrazor_runtime_transport_api(uint32_t){return &transport_api;}
extern "C" const void *pgrazor_shared_host_api(uint32_t v){return v==2?static_cast<void*>(&broker_api):static_cast<void*>(&host_api);}
const void *pgrazor_transfer_facts_api(uint32_t){return &facts_api;}const void *pgrazor_vgp_results_api(uint32_t){return &result_api;}
extern "C" int pgr_pvp_keep(void*,int,int,int,int,uint64_t,char*,uint32_t){return pvp_protected?1:0;}
bool pgr_evolution_busy(){return batch_busy;}
bool pgr_showcase_keep(uint64_t,uint64_t){return showcase_protected;}
int main(){
 fixture();showcase_protected=true;assert(offer()==1);tick();assert(evolution_calls==0&&release_calls==0&&!job.id);showcase_protected=false;
 fixture();assert(offer()==1);tick();assert(evolution_calls==1&&release_calls==0);tick();assert(release_calls==0);accept();tick();assert(release_calls==0);reconcile();assert(job.completed==1);tick();assert(release_calls==1);tick();assert(!job.id&&handles.empty());
 fixture();stock=0;assert(offer()==1);tick();assert(evolution_calls==0&&release_calls==1);tick();
 fixture();special=true;assert(offer()==1);tick();assert(evolution_calls==0&&release_calls==1);tick();
 fixture();config="1|1|1|1|10,13";assert(offer()==2);tick();assert(evolution_calls==0&&release_calls==0);
 fixture();config="1|0|0|1|";assert(offer()==0);
 fixture();config="1|1|1|2|1";assert(offer()==1);tick();accept();reconcile();next_target=3;returned_species=3;tick();assert(evolution_calls==2&&release_calls==0);accept();reconcile();not_evolvable=true;tick();assert(release_calls==1);tick();
 fixture();config="1|1|0|2|";assert(offer()==1);tick();accept();reconcile();stock=0;next_target=3;tick();assert(evolution_calls==1&&release_calls==1);tick();
 fixture();assert(offer()==1);tick();returned_id=20;accept();reconcile(20);tick();assert(release_calls==1);tick();
 fixture();assert(offer()==1);tick();returned_id=20;accept();O *replacement=object("Holoholo.Rpc.PokemonProto");replacement->data["id_"]=20;replacement->data["pokemonId_"]=2;inventory[20]=replacement;stock-=cost-1;tick();assert(job.await_inventory&&release_calls==0);allowed=false;tick();assert(!job.id);
 fixture();assert(offer()==1);tick();accept();reconcile();inventory.at(10)->data["favorite_"]=1;tick();assert(release_calls==0&&!job.id);
 fixture();assert(offer()==1);tick();accept();reconcile();keep_config="2";tick();assert(release_calls==0&&!job.id);
 fixture();assert(offer()==1);tick();accept();reconcile();pvp_protected=true;tick();assert(release_calls==0&&!job.id);
 fixture();assert(offer()==1);tick();allowed=false;tick();assert(release_calls==0&&!job.id&&handles.empty());allowed=true;assert(offer()==2);
 fixture();assert(offer()==1);cancel_during_check=true;tick();assert(evolution_calls==0&&release_calls==0);
 fixture();assert(offer()==1);change_account=true;tick();assert(evolution_calls==0&&release_calls==0);
 fixture();assert(offer()==1);tick();poll_error=true;tick();assert(release_calls==0&&!job.id);
 fixture();assert(offer()==1);tick();result_status=0;response_ready=true;tick();assert(release_calls==0&&!job.id);
 fixture();assert(offer()==1);tick();returned_species=99;response_ready=true;tick();assert(release_calls==0&&!job.id);
 fixture();assert(offer()==1);tick();job.since=clock_ms()-16000;tick();assert(release_calls==0&&!job.id);
 fixture();keep_all=true;assert(offer(true)==2);assert(offer(false)==1);stock=0;tick();assert(release_calls==1);tick();
 fixture();assert(offer()==1);assert(offer()==2);tick();assert(evolution_calls==1);pgr_evolve_transfer_stop(gettid());assert(handles.empty());
 fixture();batch_busy=true;assert(offer()==1);tick();assert(evolution_calls==0);batch_busy=false;tick();assert(evolution_calls==1);pgr_evolve_transfer_stop(gettid());
 fixture();move_on_root=true;assert(offer()==1);tick();assert(evolution_calls==1);pgr_evolve_transfer_stop(gettid());assert(handles.empty());
 fixture();iv_attack=iv_defense=iv_stamina=15;assert(offer()==1);tick();assert(!job.id&&evolution_calls==0&&release_calls==0);
 fixture();not_evolvable=true;assert(offer()==1);tick();assert(release_calls==1&&evolution_calls==0);tick();
 fixture();branch->data["EvolutionItemRequirement"]=1;assert(offer()==1);tick();assert(release_calls==1&&evolution_calls==0);tick();
 fixture();assert(offer()==1);tick();config="1|1|1|2|1";tick();assert(!job.id&&release_calls==0);
 fixture();assert(offer()==1);tick();keep_all=true;tick();assert(!job.id&&release_calls==0);
 fixture();config="broken";assert(offer()==2);fresh=false;config="1|1|0|1|";assert(offer()==2);
 pgr_evolve_transfer::Config c;assert(pgr_evolve_transfer::parse("1|1|1|2|1,10",12,c)&&c.selects(1)&&!c.selects(2));assert(!pgr_evolve_transfer::parse("1|1|1|2|1,1",11,c));
 assert(!pgr_evolve_transfer::another_stage(c,8));
 queue.clear();job=Job{};assert(handles.empty());puts("PASS evolve/transfer: confirmed inventory, once/all stages, insufficient candy, special conditions, independent list, replacement IDs, favorites/keep/PvP, settings/account changes, failures/timeouts, Keep All, duplicate quarantine, batch exclusion and root cleanup");
}

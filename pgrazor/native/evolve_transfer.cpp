#include "game_bindings.h"
#include "evolve_transfer.h"
#include "showcase_host.h"
#include "evolve_transfer_policy.h"
#include "batch_evolution_policy.h"
#include "batch_evolution.h"
#include <vector>
#include "shared_host.h"
#include "shared_metadata.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "transfer_execution.h"
#include "transfer_facts.h"
#include "pvp_keep.h"
#include "vgp_results.h"
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <time.h>
#include <cstring>
#include <deque>
#include <set>
namespace {
using namespace pgr_shared_meta;
using namespace pgr_evolution;
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
PgrBootBinding boot{};const PgrSharedMetadataOutput *md=nullptr;
bool initialized=false,busy=false;uint64_t owner=0;
const char *managed_context="managed call failed";
struct CallContext {const char *previous=managed_context;~CallContext(){managed_context=previous;}};
uint64_t clock_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
template<class F>F fn(void *p){require(p,"missing runtime export");return reinterpret_cast<F>(p);}
void *exp(const char *name){std::string n="il2cpp_";n+=name;void *p=dlsym(boot.handle,n.c_str());require(p,name);return p;}
struct Root {
 uintptr_t h=0;
 Root()=default;explicit Root(void *v){if(v){h=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(v,0);require(h,"GC root");}}
 Root(const Root&)=delete;Root& operator=(const Root&)=delete;
 Root(Root &&r) noexcept:h(r.h){r.h=0;}
 Root& operator=(Root &&r) noexcept {if(this!=&r){reset();h=r.h;r.h=0;}return *this;}
 ~Root(){reset();}void reset(){if(h){fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);h=0;}}
 void *get()const{return h?fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h):nullptr;}
 explicit operator bool()const{return get()!=nullptr;}
};
void *klass(void *o){require(o,"null object");return fn<void*(*)(void*)>(boot.runtime.object_get_class)(o);}
void *cls(const char *ns,const char *name){
 static std::map<std::string,void*> cache;std::string key=std::string(ns)+"."+name;auto it=cache.find(key);if(it!=cache.end())return it->second;
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api,"metadata API");
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult out{};require(api->resolve_class(&q,&out)==0,name);return cache[key]=const_cast<void*>(out.klass);
}
std::string typename_of(void *type){char *p=fn<char*(*)(void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);for(size_t pos=0;(pos=s.find("`1<",pos))!=std::string::npos;)s.erase(pos,2);return s;}
void *field_info(void *k,const char *name,const char *type){
 void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);
 auto ft=fn<void*(*)(void*)>(boot.meta.field_get_type)(f);require(typename_of(ft)==type,name);
 require(!(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10),"instance field required");return f;
}
template<class T>T field(void *o,const char *name,const char *type){T value{};auto f=field_info(klass(o),name,type);fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&value);return value;}
template<class T>void put(void *o,const char *name,const char *type,T value){auto f=field_info(klass(o),name,type);fn<void(*)(void*,void*,void*)>(boot.runtime.field_set_value)(o,f,&value);}
void *method(void *k,const char *name,int count){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,count);require(m,name);require(fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(m)==unsigned(count),name);return m;}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);if(ex){char detail[512]{};auto format=reinterpret_cast<void(*)(void*,char*,int)>(dlsym(boot.handle,"il2cpp_format_exception"));if(format)format(ex,detail,sizeof(detail));__android_log_print(ANDROID_LOG_ERROR,"PGRazorEvolveTransfer","%s: %s",managed_context,detail);throw Failure{managed_context};}return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(klass(o),name,n),o,args);}
template<class T>T boxed(void *o,const char *type){require(o&&typename_of(fn<void*(*)(void*)>(boot.meta.class_get_type)(klass(o)))==type,type);T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");memcpy(&v,p,sizeof(v));return v;}
std::string string(void *o){
 if(!o)return {};require(klass(o)==cls("System","String"),"string identity");int n=fn<int(*)(void*)>(exp("string_length"))(o);require(n>=0&&n<=512,"string bound");auto p=fn<const uint16_t*(*)(void*)>(boot.runtime.string_chars)(o);std::string s;
 for(int i=0;i<n;i++){uint32_t c=p[i];if(c>=0xd800&&c<=0xdbff){require(i+1<n&&p[i+1]>=0xdc00&&p[i+1]<=0xdfff,"invalid UTF16");c=0x10000+((c-0xd800)<<10)+(p[++i]-0xdc00);}else require(c<0xdc00||c>0xdfff,"invalid UTF16");
 if(c<128)s+=char(c);else if(c<2048){s+=char(0xc0|(c>>6));s+=char(0x80|(c&63));}else if(c<65536){s+=char(0xe0|(c>>12));s+=char(0x80|((c>>6)&63));s+=char(0x80|(c&63));}else{s+=char(0xf0|(c>>18));s+=char(0x80|((c>>12)&63));s+=char(0x80|((c>>6)&63));s+=char(0x80|(c&63));}}
 return s;
}
void *h(unsigned id){return const_cast<void*>(md->opt_handles[id]);}
Root type_object(void *k){return Root(fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k)));}
Root resolve(void *contract){
 Root type=type_object(h(H_IL_MAP));void *args[]={type.get()};Root map(invoke(h(H_IL_FIND_M),nullptr,args));if(!map)return {};
 Root container(invoke(h(H_IL_CONTAINER_M),map.get()));if(!container)return {};
 Root target=type_object(contract);void *resolve_args[]={target.get()};return Root(invoke(h(H_IL_RESOLVE_M),container.get(),resolve_args));
}
Root resolve(const char *ns,const char *name){return resolve(cls(ns,name));}
int literal(void *k,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);require((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)==0x8050,name);int n=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&n);return n;}
int result_value(void *o){auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),"Result");require(f,"Result");int n=0;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&n);return n;}
bool success(void *o){auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),"Result");require(f,"Result");auto k=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));return result_value(o)==literal(k,"Success");}
int count(Root &list,int cap){int n=boxed<int>(call(list.get(),"get_Count"),"System.Int32");require(n>=0&&n<=cap,"collection bound");return n;}
Root item(Root &list,int index){void *args[]={&index};return Root(call(list.get(),"get_Item",1,args));}
Root reference(void *o,const char *name,const char *prefix){
 auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name);require(f,name);
 void *ft=fn<void*(*)(void*)>(boot.meta.field_get_type)(f);void *k=fn<void*(*)(void*)>(boot.meta.class_from_type)(ft);
 require(k&&!fn<bool(*)(void*)>(boot.meta.class_is_valuetype)(k)&&typename_of(ft).find(prefix)==0,name);
 void *v=nullptr;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return Root(v);
}
int enum_field(void *o,const char *name){auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name);require(f,name);auto k=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));require(k&&fn<bool(*)(void*)>(boot.meta.class_is_enum)(k),name);int v=0;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;}
void put_enum(void *o,const char *name,int v){enum_field(o,name);auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name);fn<void(*)(void*,void*,void*)>(boot.runtime.field_set_value)(o,f,&v);}
int enum_box(void *o){require(o&&fn<bool(*)(void*)>(boot.meta.class_is_enum)(klass(o)),"enum return");int v=0;memcpy(&v,fn<void*(*)(void*)>(boot.runtime.object_unbox)(o),4);return v;}

using Config=pgr_evolve_transfer::Config;
struct Permission {
 Config config;bool valid=false,allowed=false,keep_all=false;
 uint32_t protect=127;std::set<int> keep;std::string pvp;
};
Permission permission(){
 Permission p;auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));if(!tr)return p;
 PgrRuntimeTransportSample a{},z{};char text[10001]{};uint32_t n=0;int allowed=0,keep_all=0;int32_t protect=127;
 if(tr->sample(&a)||!a.snapshot_fresh||a.fault||tr->get_string(22,text,sizeof(text),&n)||!pgr_evolve_transfer::parse(text,n,p.config))return p;
 if(tr->get_bool(5,&allowed)||tr->get_bool(35,&keep_all)||tr->get_int(8,&protect)||protect<0||protect>127)return p;
 if(tr->get_string(4,text,sizeof(text),&n)||!pgr_evolve_transfer::species_list(text,n,p.keep))return p;
 if(tr->get_string(16,text,sizeof(text),&n))return p;p.pvp.assign(text,n);
 if(tr->sample(&z)||!z.snapshot_fresh||z.fault||a.snapshot_seq!=z.snapshot_seq)return p;
 p.valid=true;p.allowed=allowed&&p.config.enabled;p.keep_all=keep_all;p.protect=protect;return p;
}
struct Pending {
 uint64_t id=0;int species=0;bool manual=false;Config config;std::string account;
 Root bag,gm,player,buddy;
};
struct Job : Pending {
 unsigned completed=0;int original_species=0,candy_before=0,candy_awarded=0;
 uint64_t result_id=0,since=0,next=0,release_token=0;
 bool await_inventory=false,release_done=false,release_success=false;
 Candidate sending;Root promise;std::set<int> evolved_species;
};
Job job;std::deque<Pending> queue;std::set<std::pair<std::string,uint64_t>> seen;
void status(const std::string &message){
 __android_log_print(ANDROID_LOG_INFO,"PGRazorEvolveTransfer","%s",message.c_str());
 auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));
 PgrRuntimeTransportString s{message.c_str(),uint32_t(message.size())};if(tr)tr->emit(56,nullptr,0,&s,1);
}
void finish(const char *message){status(message);job=Job{};}
void initialize(const PgrSharedMetadataOutput *metadata=nullptr){
 if(initialized)return;auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));
 require(api&&api->snapshot(&boot)==0,"Evolution runtime unavailable");
 if(!metadata){auto host=static_cast<const PgrSharedHostApi*>(pgrazor_shared_host_api(1));metadata=host&&host->metadata?host->metadata():nullptr;}
 require(metadata,"Transfer metadata unavailable");md=metadata;owner=gettid();initialized=true;
}
Root pokemon(uint64_t id){void *a[]={&id};return Root(call(job.bag.get(),"GetPokemon",1,a));}
int candy_stock(int family){void *a[]={&family};return boxed<int>(call(job.player.get(),"GetCandyCountForPokemonFamily",1,a),"System.Int32");}
bool authorized(const Permission &p){return p.valid&&p.allowed&&p.config==job.config&&p.config.selects(job.original_species)&&!(job.manual&&p.keep_all);}
bool same_rules(const Permission &a,const Permission &b){return authorized(b)&&a.protect==b.protect&&a.keep==b.keep&&a.pvp==b.pvp;}
bool identity(){
 Root player=resolve("Niantic.Holoholo","IPlayerService"),bag=resolve("Niantic.Holoholo","IPokemonBag");
 return player&&bag&&bag.get()==job.bag.get()&&string(call(player.get(),"get_PlayerId"))==job.account;
}
bool safe_to_release(Root &p){
 if(!p||klass(p.get())!=cls("Holoholo.Rpc","PokemonProto")||field<uint64_t>(p.get(),"id_","System.UInt64")!=job.id)return false;
 for(const char *name:{"isEgg_","favorite_","isBad_","isComponent_","isFusion_","isActivelyTraining_","inEscrow_"})
  if(field<bool>(p.get(),name,"System.Boolean"))return false;
 if(!string(field<void*>(p.get(),"deployedFortId_","System.String")).empty()||!string(field<void*>(p.get(),"deployedStationId_","System.String")).empty())return false;
 void *args[]={&job.id};return !boxed<bool>(call(job.buddy.get(),"GetIsBuddy",1,args),"System.Boolean");
}
bool classify(Root &p,const Permission &settings,PgrTransferInput &input,std::vector<int32_t> &kept){
 if(pgr_showcase_keep(job.id,owner))return false;
 if(!authorized(settings)||!safe_to_release(p))return false;
 auto facts=static_cast<const PgrTransferFactsApi*>(pgrazor_transfer_facts_api(1));
 auto policy=static_cast<const PgrTransferApi*>(pgrazor_transfer_policy_api(1));require(facts&&policy,"Transfer protections unavailable");
 PgrTransferFactsRequest req{1,sizeof(req),owner,2500,job.id,p.get(),job.gm.get(),settings.protect,0};PgrTransferFactsOut out{};
 if(facts->inspect(&req,&out)!=1)return false;
 kept.assign(settings.keep.begin(),settings.keep.end());
 input={1,sizeof(input),out.attack,out.defense,out.stamina,out.species,settings.protect,out.known,out.flags,uint32_t(job.manual&&settings.keep_all),uint32_t(kept.size()),0,kept.data()};
 if(policy->decide(&input)!=0)return false;
 char reason[128]{};if(!pgr_pvp_keep||pgr_pvp_keep(p.get(),out.attack,out.defense,out.stamina,out.species,owner,reason,sizeof(reason))>0)return false;
 return same_rules(settings,permission());
}
bool evolution_candidate(Root &p,Candidate &c,std::string *reason=nullptr){
 auto reject=[&](const char *why){if(reason)*reason=why;return false;};
 if(!p)return reject("Pokemon no longer available");require(klass(p.get())==cls("Holoholo.Rpc","PokemonProto"),"Pokemon identity");c.id=field<uint64_t>(p.get(),"id_","System.UInt64");c.species=field<int>(p.get(),"pokemonId_","System.Int32");
 if(!c.id)return reject("Missing inventory ID");
 const char *flags[]={"isEgg_","favorite_","isBad_","isComponent_","isFusion_","isActivelyTraining_","inEscrow_"};const char *reasons[]={"Egg","Favorite","Unavailable Pokemon","Fusion component","Fused Pokemon","Actively training","Reserved for trade"};for(int i=0;i<7;i++)if(field<bool>(p.get(),flags[i],"System.Boolean"))return reject(reasons[i]);
 if(!string(field<void*>(p.get(),"deployedFortId_","System.String")).empty()||!string(field<void*>(p.get(),"deployedStationId_","System.String")).empty())return reject("Deployed defender");
 void *buddy_args[]={&c.id};if(boxed<bool>(call(job.buddy.get(),"GetIsBuddy",1,buddy_args),"System.Boolean"))return reject("Current buddy");
 // Shadow/purified costs and quest/lure/time/branch decisions need their own
 // explicit picker. Do not guess them in a standard XP job.
 if(boxed<bool>(call(p.get(),"get_IsShadowPokemon"),"System.Boolean")||boxed<bool>(call(p.get(),"get_IsPurifiedPokemon"),"System.Boolean"))return reject("Shadow or purified evolution not supported yet");
 // IsSelectionDisabled includes transfer-only protection (e.g. shiny/lucky).
 // Transfer protections are independently checked before this eligibility probe.
 void *gm_args[]={job.gm.get()};
 if(boxed<bool>(call(p.get(),"IsNoEvolvePokemon",1,gm_args),"System.Boolean"))return reject("This Pokemon cannot evolve");
 bool flag=false;void *args[]={job.gm.get(),&flag};Root settings(call(p.get(),"GetPokemonSettings",2,args));if(!settings)return reject("Evolution settings unavailable");
 Root branches(call(settings.get(),"get_EvolutionBranch"));if(!branches||count(branches,100)==0)return reject("No evolution available");if(count(branches,100)!=1)return reject("Multiple evolution branches require a choice");Root branch=item(branches,0);require(klass(branch.get())==cls("Holoholo.Rpc","EvolutionBranchProto"),"branch identity");
 const char *conditions[]={"MustBeBuddy","OnlyDaytime","OnlyNighttime","OnlyUpsideDown","OnlyDuskPeriod","OnlyFullMoon","ShouldHideButton"};const char *labels[]={"Requires buddy evolution","Daytime evolution","Nighttime evolution","Upside-down evolution","Dusk evolution","Full-moon evolution","Evolution hidden by game"};for(int i=0;i<7;i++)if(field<bool>(branch.get(),conditions[i],"System.Boolean"))return reject(labels[i]);
 if(field<float>(branch.get(),"KmBuddyDistanceRequirement","System.Single")!=0||enum_field(branch.get(),"GenderRequirement")!=0||enum_field(branch.get(),"LureItemRequirement")!=0||enum_field(branch.get(),"TemporaryEvolution")!=0||enum_field(branch.get(),"EvolutionMoveRequirement")!=0)return reject("Requires distance, gender, lure, move or temporary evolution handling");
 Root quests=reference(branch.get(),"QuestDisplay","Niantic.Protobuf.Collections.RepeatedField");if(count(quests,100)!=0)return reject("Requires an evolution task");
 c.target=enum_field(branch.get(),"Evolution");c.form=enum_field(branch.get(),"Form");c.item=enum_field(branch.get(),"EvolutionItemRequirement");c.item_count=c.item?field<int>(branch.get(),"EvolutionItemRequirementCost","System.Int32"):0;if(c.item&&c.item_count==0)c.item_count=1;
 if(c.item)return reject("Evolution requires an item");
 c.candy=field<int>(branch.get(),"CandyCost","System.Int32");if(c.candy==0)c.candy=boxed<int>(call(settings.get(),"get_CandyToEvolve"),"System.Int32");
 if(field<bool>(branch.get(),"NoCandyCostViaTrade","System.Boolean")&&boxed<bool>(call(p.get(),"get_HasBeenTraded"),"System.Boolean"))c.candy=0;
 gm_args[0]=job.gm.get();c.family=enum_box(call(p.get(),"GetFamilyId",1,gm_args));return (c.target>0&&c.target!=c.species&&c.candy>=0&&c.item_count>=0)||reject("Invalid evolution target or cost");
}

void release(Root &current,const Permission &settings){
 PgrTransferInput input{};std::vector<int32_t> keep;
 if(!classify(current,settings,input,keep)||!identity()||!same_rules(settings,permission())){finish("Kept: transfer protections or session changed");return;}
 auto broker=static_cast<const PgrSharedHostTransferApi*>(pgrazor_shared_host_api(2));require(broker&&broker->ready&&broker->ready(),"Transfer service unavailable");
 PgrTransferExecRequest request{1,sizeof(request),job.id,0,owner,job.bag.get(),current.get(),input};
 job.since=clock_ms();job.release_done=false;
 // Pass the durable token slot directly: a synchronous release observer may
 // complete while submit() is still running. The shared FIFO routes it here.
 const int rc=broker->submit(&request,&job.release_token);
 if(!job.release_token){finish(rc==0?"Kept: transfer rules changed":"Kept: transfer request refused");return;}
 status("Transfer requested after evolution check");
}
void act(){
 const auto settings=permission();if(!authorized(settings)||!identity()){finish("Kept: evolve/transfer settings or account changed");return;}
 Root current=pokemon(job.id);PgrTransferInput input{};std::vector<int32_t> keep;
 if(!classify(current,settings,input,keep)){finish("Kept: current transfer protections apply");return;}
 if(!pgr_evolve_transfer::another_stage(job.config,job.completed)){release(current,settings);return;}
 Candidate next;std::string why;
 if(!evolution_candidate(current,next,&why)){release(current,settings);return;}
 require(!job.evolved_species.count(next.target),"Kept: cyclic evolution target");
 const int candy=candy_stock(next.family);require(candy>=0,"Candy count unavailable");
 if(candy<next.candy){release(current,settings);return;}
 Root req(fn<void*(*)(void*)>(boot.runtime.object_new)(cls("Holoholo.Rpc","EvolvePokemonProto")));require(bool(req),"Evolution request allocation");call(req.get(),".ctor");
 put(req.get(),"PokemonId","System.UInt64",next.id);put_enum(req.get(),"TargetPokemonId",next.target);put_enum(req.get(),"TargetPokemonForm",next.form);
 put_enum(req.get(),"EvolutionItemRequirement",0);put(req.get(),"EvolutionItemRequirementCount","System.Int32",0);
 // Re-read inventory identity and protections after constructing the request.
 Root fresh=pokemon(job.id);if(!fresh||fresh.get()!=current.get()||!classify(fresh,settings,input,keep)||!identity()||!same_rules(settings,permission())){finish("Kept: Pokemon or rules changed before evolution");return;}
 Candidate checked;
 if(!evolution_candidate(fresh,checked)||checked.id!=next.id||checked.species!=next.species||checked.target!=next.target||checked.form!=next.form||checked.family!=next.family||checked.candy!=next.candy){finish("Kept: evolution requirements changed");return;}
 const int final_candy=candy_stock(next.family);require(final_candy>=0,"Candy count unavailable");
 if(final_candy<next.candy){release(fresh,settings);return;}
 if(!same_rules(settings,permission())){finish("Kept: evolve/transfer permission changed");return;}
 job.sending=next;job.candy_before=final_candy;job.since=clock_ms();job.next=job.since+1500;
 void *args[]={fresh.get(),req.get()};job.promise=Root(call(job.bag.get(),"EvolvePokemon",2,args));
 require(bool(job.promise),"Evolution outcome unknown; Pokemon kept");status("Evolving before transfer");
}
void poll(){
 if(job.release_token){
  if(job.release_done){finish(job.release_success?"Evolution/transfer complete":"Transfer unconfirmed; not retried");return;}
  if(clock_ms()-job.since>=15000)finish("Transfer timed out; not retried");return;
 }
 if(job.await_inventory){
  Root original=pokemon(job.sending.id),replacement=job.result_id!=job.sending.id?pokemon(job.result_id):Root{};
  int a=original?field<int>(original.get(),"pokemonId_","System.Int32"):0,b=replacement?field<int>(replacement.get(),"pokemonId_","System.Int32"):0;
  const bool matches=reconciled_identity(job.sending.id,job.result_id,job.sending.target,bool(original),a,bool(replacement),b);
  const bool costs=candy_stock(job.sending.family)<=int64_t(job.candy_before)-job.sending.candy+job.candy_awarded;
  if(matches&&costs){job.id=job.result_id;++job.completed;job.evolved_species.insert(job.sending.target);job.await_inventory=false;status("Evolution confirmed; rechecking transfer rules");return;}
  require(clock_ms()-job.since<15000,"Evolution inventory update unconfirmed; Pokemon kept");return;
 }
 auto vr=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));require(vr,"Evolution promise observer unavailable");
 PgrVgpResultsResult out{};int rc=vr->poll(job.promise.get(),1,owner,&out);Root response;response.h=out.value_handle;
 if(!rc){require(clock_ms()-job.since<15000,"Evolution response timed out; Pokemon kept");return;}
 job.promise.reset();require(rc==PGR_VR_DONE&&response,"Evolution request failed; Pokemon kept");
 require(klass(response.get())==cls("Holoholo.Rpc","EvolvePokemonOutProto")&&success(response.get()),"Evolution declined; Pokemon kept");
 Root evolved(field<void*>(response.get(),"EvolvedPokemon","Holoholo.Rpc.PokemonProto"));uint64_t result_id=0;int species=0;
 if(evolved){require(klass(evolved.get())==cls("Holoholo.Rpc","PokemonProto"),"Unexpected evolution identity");result_id=field<uint64_t>(evolved.get(),"id_","System.UInt64");species=field<int>(evolved.get(),"pokemonId_","System.Int32");}
 require(species==0||species==job.sending.target,"Evolution returned a different species; Pokemon kept");
 job.result_id=result_id?result_id:job.sending.id;
 if(job.result_id!=job.sending.id){require(seen.size()<pgr_evolve_transfer::kSeenLimit&&seen.insert({job.account,job.result_id}).second,"Evolution returned an already processed Pokemon; kept");}
 job.candy_awarded=field<int>(response.get(),"CandyAwarded","System.Int32");require(job.candy_awarded>=0,"Invalid evolution award");
 job.await_inventory=true;job.since=clock_ms();status("Evolution accepted; waiting for inventory confirmation");
}
} // namespace

int pgr_evolve_transfer_offer(const PgrTransferExecRequest *request,bool manual){
 auto settings=permission();if(settings.valid&&!settings.config.enabled)return 0;
 if(!settings.allowed||!request||request->version!=1||request->size!=sizeof(*request)||request->tid!=uint64_t(gettid())||busy)return 2;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}} guard;
 try{
  if(!settings.config.selects(request->input.species)){status("Kept: not in the evolve/transfer list");return 2;}
  if(manual&&settings.keep_all)return 2;
  initialize();require(owner==uint64_t(gettid()),"Wrong evolution thread");
  require(queue.size()<pgr_evolve_transfer::kQueueLimit&&seen.size()<pgr_evolve_transfer::kSeenLimit,"Evolution queue full; Pokemon kept");
  Pending pending;pending.id=request->captured_id;pending.species=request->input.species;pending.manual=manual;pending.config=settings.config;
  pending.bag=Root(request->bag);require(bool(pending.bag),"Catch inventory unavailable");
  pending.gm=reference(pending.bag.get(),PGR_GAME_BATCH_EVOLUTION_CPP_ERIK_NAME,"IGameMasterData");
  void *lookup[]={&pending.id};Root pokemon_root(call(pending.bag.get(),"GetPokemon",1,lookup));
  require(pending.id&&pending.bag&&pending.gm&&pokemon_root,"Incomplete catch identity");
  require(field<uint64_t>(pokemon_root.get(),"id_","System.UInt64")==pending.id&&field<int>(pokemon_root.get(),"pokemonId_","System.Int32")==pending.species,"Changed catch identity");
  pending.player=resolve("Niantic.Holoholo","IPlayerService");pending.buddy=resolve("Niantic.Holoholo.Buddy","IBuddyPokemonService");require(pending.player&&pending.buddy,"Evolution services unavailable; Pokemon kept");
  pending.account=string(call(pending.player.get(),"get_PlayerId"));require(!pending.account.empty(),"Account identity unavailable");
  auto final=permission();if(!final.allowed||!(final.config==settings.config)||(manual&&final.keep_all))return 2;
  if(!seen.insert({pending.account,pending.id}).second)return 2;
  queue.push_back(std::move(pending));status("Queued to evolve before transfer");return 1;
 }catch(const Failure&e){status(e.where);}catch(...){status("Evolution queue failed; Pokemon kept");}return 2;
}
void pgr_evolve_transfer_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid))return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 try{
  if(queue.empty()&&!job.id)return;
  initialize(metadata);const auto settings=permission();
  if(!settings.allowed){queue.clear();if(job.id)finish("Evolve/transfer stopped; pending Pokemon kept");return;}
  if(job.id&&(!authorized(settings)||!identity())){finish("Evolve/transfer stopped; rules or account changed");return;}
  if(job.promise||job.await_inventory||job.release_token){poll();return;}
  if(pgr_evolution_busy())return;
  if(!job.id){
   while(!queue.empty()){
    Pending next=std::move(queue.front());queue.pop_front();
    if(!(next.config==settings.config)||(next.manual&&settings.keep_all))continue;
    job=Job{};static_cast<Pending&>(job)=std::move(next);job.original_species=job.species;job.evolved_species.insert(job.species);break;
   }
  }
  if(job.id&&clock_ms()>=job.next)act();
 }catch(const Failure&e){finish(e.where);}catch(...){finish("Evolution failed; Pokemon kept");}
}
void pgr_evolve_transfer_stop(uint64_t tid){if(tid==owner&&tid==uint64_t(gettid())){queue.clear();job=Job{};}}
bool pgr_evolve_transfer_event(const PgrTransferExecEvent *event){
 if(!event||uint64_t(gettid())!=owner||!job.release_token||job.release_token!=event->token)return false;
 if(event->kind!=PGR_TX_EVENT_REQUEST){job.release_done=true;job.release_success=event->kind==PGR_TX_EVENT_COMPLETE&&(event->aux&1);}
 return true;
}
bool pgr_evolve_transfer_busy(){return bool(job.promise)||job.await_inventory||job.release_token!=0;}

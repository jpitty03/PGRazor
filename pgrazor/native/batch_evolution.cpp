#include "batch_evolution.h"
#include "batch_evolution_policy.h"
#include "shared_metadata.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "vgp_results.h"
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <time.h>
#include <cstring>
#include <set>
namespace {
using namespace pgr_shared_meta;
using namespace pgr_evolution;
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
PgrBootBinding boot{};const PgrSharedMetadataOutput *md=nullptr;
bool initialized=false,busy=false;uint64_t owner=0;
void *reference_setter=nullptr;const char *managed_context="managed call failed";
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
void putref(void *o,const char *name,const char *type,void *value){auto f=field_info(klass(o),name,type);if(!reference_setter)reference_setter=exp("field_set_value_object");fn<void(*)(void*,void*,void*)>(reference_setter)(o,f,value);}
void *method(void *k,const char *name,int count){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,count);require(m,name);require(fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(m)==unsigned(count),name);return m;}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);if(ex){char detail[512]{};auto format=reinterpret_cast<void(*)(void*,char*,int)>(dlsym(boot.handle,"il2cpp_format_exception"));if(format)format(ex,detail,sizeof(detail));__android_log_print(ANDROID_LOG_ERROR,"PGRazorEvolution","%s: %s",managed_context,detail);throw Failure{managed_context};}return v;}
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
struct Permission {Command cmd;bool valid=false,allowed=false;};
Permission permission(){Permission p;auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));if(!tr)return p;PgrRuntimeTransportSample a{},z{};char text[128]{};uint32_t len=0;int live=0,ready=0;
 if(tr->sample(&a)||!a.snapshot_fresh||a.fault||tr->get_string(11,text,sizeof(text),&len)||!parse(text,len,p.cmd))return p;
 tr->get_bool(9,&live);tr->get_bool(16,&ready);
 if(tr->sample(&z)||!z.snapshot_fresh||z.snapshot_seq!=a.snapshot_seq)return p;p.valid=true;p.allowed=live&&ready&&p.cmd.mode!=0;return p;
}
struct Batch {uint64_t token=0,tag_id=0,started=0,last_event=0,next=0,since=0;std::string tag,account,phase="idle";Root bag,gm,buddy,player,itembag,rows,promise,tag_service,tag_proto;int index=0,scan_index=0,total_rows=0,skipped=0,completed=0;int64_t xp=0,candy_cost=0,item_count=0;Budget budget;std::vector<Candidate> candidates;std::set<uint64_t> seen;std::vector<std::string> skipped_details;Candidate sending;int candy_before=0,item_before=0,candy_awarded=0,pending_xp=0;uint64_t result_id=0;bool canceled=false,await_inventory=false,remove_tag=false,tag_pending=false,await_tag=false;};Batch batch;
std::set<uint64_t> uncertain;
std::string escape(const std::string&s){std::string out;for(unsigned char c:s){if(c=='"'||c=='\\')out+='\\';if(c>=32)out+=char(c);}return out;}
void publish(const std::string &phase,const std::string &message){batch.phase=phase;batch.last_event=clock_ms();
 std::string j="{\"kind\":\"batch-evolution\",\"token\":"+std::to_string(batch.token)+",\"phase\":\""+phase+"\",\"message\":\""+escape(message)+"\",\"total\":"+std::to_string(batch.candidates.size())+",\"completed\":"+std::to_string(batch.completed)+",\"skipped\":"+std::to_string(batch.skipped)+",\"xp\":"+std::to_string(batch.xp)+",\"elapsedMs\":"+std::to_string(batch.started?clock_ms()-batch.started:0)+",\"candyCost\":"+std::to_string(batch.candy_cost)+",\"itemCount\":"+std::to_string(batch.item_count)+"}";
 __android_log_print(ANDROID_LOG_INFO,"PGRazorEvolution","%s",j.c_str());auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));PgrRuntimeTransportString a[]={{"batch-evolution",15},{j.c_str(),uint32_t(j.size())}};if(!tr||tr->emit(19,nullptr,0,a,2)){batch.canceled=true;batch.phase="error";}
}
int enum_field(void *o,const char *name){auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name);require(f,name);auto k=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));require(k&&fn<bool(*)(void*)>(boot.meta.class_is_enum)(k),name);int v=0;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;}
void put_enum(void *o,const char *name,int v){enum_field(o,name);auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name);fn<void(*)(void*,void*,void*)>(boot.runtime.field_set_value)(o,f,&v);}
int enum_box(void *o){require(o&&fn<bool(*)(void*)>(boot.meta.class_is_enum)(klass(o)),"enum return");int v=0;memcpy(&v,fn<void*(*)(void*)>(boot.runtime.object_unbox)(o),4);return v;}
Root pokemon(uint64_t id){void *a[]={&id};return Root(call(batch.bag.get(),"GetPokemon",1,a));}
int candy_stock(int family){void *a[]={&family};return boxed<int>(call(batch.player.get(),"GetCandyCountForPokemonFamily",1,a),"System.Int32");}
int item_stock(int id){if(!id)return 0;void *a[]={&id};return boxed<int>(call(batch.itembag.get(),"GetItemCount",1,a),"System.Int32");}
bool tagged(Root &p){Root tags=reference(p.get(),"pokemonTagIds_","Niantic.Protobuf.Collections.RepeatedField");int n=count(tags,1000);for(int i=0;i<n;i++){Root v=item(tags,i);if(boxed<uint64_t>(v.get(),"System.UInt64")==batch.tag_id)return true;}return false;}
bool candidate(Root &p,Candidate &c,std::string *reason=nullptr){
 auto reject=[&](const char *why){if(reason)*reason=why;return false;};
 if(!p)return reject("Pokemon no longer available");require(klass(p.get())==cls("Holoholo.Rpc","PokemonProto"),"Pokemon identity");c.id=field<uint64_t>(p.get(),"id_","System.UInt64");c.species=field<int>(p.get(),"pokemonId_","System.Int32");
 if(!c.id)return reject("Missing inventory ID");if(uncertain.count(c.id))return reject("Earlier evolution outcome unconfirmed");if(!tagged(p))return reject("No longer tagged");
 const char *flags[]={"isEgg_","favorite_","isBad_","isComponent_","isFusion_","isActivelyTraining_","inEscrow_"};const char *reasons[]={"Egg","Favorite","Unavailable Pokemon","Fusion component","Fused Pokemon","Actively training","Reserved for trade"};for(int i=0;i<7;i++)if(field<bool>(p.get(),flags[i],"System.Boolean"))return reject(reasons[i]);
 if(!string(field<void*>(p.get(),"deployedFortId_","System.String")).empty()||!string(field<void*>(p.get(),"deployedStationId_","System.String")).empty())return reject("Deployed defender");
 void *buddy_args[]={&c.id};if(boxed<bool>(call(batch.buddy.get(),"GetIsBuddy",1,buddy_args),"System.Boolean"))return reject("Current buddy");
 // Shadow/purified costs and quest/lure/time/branch decisions need their own
 // explicit picker. Do not guess them in a standard XP batch.
 if(boxed<bool>(call(p.get(),"get_IsShadowPokemon"),"System.Boolean")||boxed<bool>(call(p.get(),"get_IsPurifiedPokemon"),"System.Boolean"))return reject("Shadow or purified evolution not supported yet");
 // IsSelectionDisabled includes transfer-only protection (e.g. shiny/lucky).
 // Explicitly tagged Pokemon use evolution eligibility, not transfer eligibility.
 void *gm_args[]={batch.gm.get()};
 if(boxed<bool>(call(p.get(),"IsNoEvolvePokemon",1,gm_args),"System.Boolean"))return reject("This Pokemon cannot evolve");
 bool flag=false;void *args[]={batch.gm.get(),&flag};Root settings(call(p.get(),"GetPokemonSettings",2,args));if(!settings)return reject("Evolution settings unavailable");
 Root branches(call(settings.get(),"get_EvolutionBranch"));if(!branches||count(branches,100)==0)return reject("No evolution available");if(count(branches,100)!=1)return reject("Multiple evolution branches require a choice");Root branch=item(branches,0);require(klass(branch.get())==cls("Holoholo.Rpc","EvolutionBranchProto"),"branch identity");
 const char *conditions[]={"MustBeBuddy","OnlyDaytime","OnlyNighttime","OnlyUpsideDown","OnlyDuskPeriod","OnlyFullMoon","ShouldHideButton"};const char *labels[]={"Requires buddy evolution","Daytime evolution","Nighttime evolution","Upside-down evolution","Dusk evolution","Full-moon evolution","Evolution hidden by game"};for(int i=0;i<7;i++)if(field<bool>(branch.get(),conditions[i],"System.Boolean"))return reject(labels[i]);
 if(field<float>(branch.get(),"KmBuddyDistanceRequirement","System.Single")!=0||enum_field(branch.get(),"GenderRequirement")!=0||enum_field(branch.get(),"LureItemRequirement")!=0||enum_field(branch.get(),"TemporaryEvolution")!=0||enum_field(branch.get(),"EvolutionMoveRequirement")!=0)return reject("Requires distance, gender, lure, move or temporary evolution handling");
 Root quests=reference(branch.get(),"QuestDisplay","Niantic.Protobuf.Collections.RepeatedField");if(count(quests,100)!=0)return reject("Requires an evolution task");
 c.target=enum_field(branch.get(),"Evolution");c.form=enum_field(branch.get(),"Form");c.item=enum_field(branch.get(),"EvolutionItemRequirement");c.item_count=c.item?field<int>(branch.get(),"EvolutionItemRequirementCost","System.Int32"):0;if(c.item&&c.item_count==0)c.item_count=1;
 c.candy=field<int>(branch.get(),"CandyCost","System.Int32");if(c.candy==0)c.candy=boxed<int>(call(settings.get(),"get_CandyToEvolve"),"System.Int32");
 if(field<bool>(branch.get(),"NoCandyCostViaTrade","System.Boolean")&&boxed<bool>(call(p.get(),"get_HasBeenTraded"),"System.Boolean"))c.candy=0;
 gm_args[0]=batch.gm.get();c.family=enum_box(call(p.get(),"GetFamilyId",1,gm_args));return (c.target>0&&c.target!=c.species&&c.candy>=0&&c.item_count>=0)||reject("Invalid evolution target or cost");
}
bool same(const Candidate&a,const Candidate&b){return a.id==b.id&&a.species==b.species&&a.target==b.target&&a.form==b.form&&a.family==b.family&&a.item==b.item&&a.item_count==b.item_count&&a.candy==b.candy;}
void validate_tag_removal();
void prepare(const Command &cmd){
 batch=Batch{};batch.token=cmd.token;batch.tag=cmd.tag;batch.remove_tag=cmd.remove_tag;
 batch.bag=resolve("Niantic.Holoholo","IPokemonBag");batch.player=resolve("Niantic.Holoholo","IPlayerService");batch.buddy=resolve("Niantic.Holoholo.Buddy","IBuddyPokemonService");batch.itembag=resolve("Niantic.Holoholo","IItemBag");require(batch.bag&&batch.player&&batch.buddy&&batch.itembag,"inventory services unavailable");batch.gm=reference(batch.bag.get(),"cypm","IGameMasterData");require(bool(batch.gm),"game settings unavailable");batch.account=string(call(batch.player.get(),"get_PlayerId"));require(!batch.account.empty(),"account identity unavailable");
 Root tags=resolve("Niantic.Holoholo.Tagging","ITaggingRpcService");require(bool(tags),"tag service unavailable");Root ready(call(tags.get(),"WaitForTagCacheInitialized"));require(ready&&field<bool>(ready.get(),"completeCalled","System.Boolean")&&!field<bool>(ready.get(),"errorCalled","System.Boolean"),"tag cache not ready; preview again");
 int n=boxed<int>(call(tags.get(),"get_NumberOfTags"),"System.Int32");require(n>=0&&n<=1000,"tag count");for(int i=0;i<n;i++){void *a[]={&i};Root row(call(tags.get(),"GetCachedTagToPokemonBindingForIndex",1,a));if(!row)continue;Root tag(field<void*>(row.get(),"TagProto","Holoholo.Rpc.PokemonTagProto"));if(normalize_tag(string(field<void*>(tag.get(),"Name","System.String")))!=normalize_tag(cmd.tag))continue;
 require(!batch.rows,"multiple tags share this name");batch.tag_id=field<uint64_t>(tag.get(),"Id","System.UInt64");batch.rows=reference(row.get(),"PokemonProtos","System.Collections.Generic.List");if(batch.remove_tag)batch.tag_proto=std::move(tag);}
 if(batch.remove_tag){batch.tag_service=std::move(tags);validate_tag_removal();}
 require(bool(batch.rows),"tag not found; create it in Pokemon storage first");batch.total_rows=count(batch.rows,25000);publish("scanning","Checking tagged Pokemon and available candy/items");
}
void scan(){
 int end=std::min(batch.scan_index+8,batch.total_rows);for(;batch.scan_index<end;++batch.scan_index){
  if(count(batch.rows,25000)!=batch.total_rows)throw Failure{"tag changed while scanning; preview again"};Root row=item(batch.rows,batch.scan_index);Candidate c;
  std::string reason;bool eligible=candidate(row,c,&reason);
  if(eligible&&!batch.seen.insert(c.id).second){eligible=false;reason="Duplicate tag entry";}
  if(eligible){int candy=candy_stock(c.family),items=item_stock(c.item);auto used=batch.budget.candy.find(c.family);int64_t reserved_candy=used==batch.budget.candy.end()?0:used->second;
   if(!batch.budget.reserve(c,candy,items)){eligible=false;reason=int64_t(candy)-reserved_candy<c.candy?"Not enough candy for this batch (needs "+std::to_string(c.candy)+", remaining "+std::to_string(std::max<int64_t>(0,int64_t(candy)-reserved_candy))+")":"Not enough evolution items for this batch";}}
  if(!eligible){++batch.skipped;std::string label="#"+std::to_string(c.species);if(row){auto nickname=string(field<void*>(row.get(),"nickname_","System.String"));if(!nickname.empty())label=nickname+" ("+label+")";}std::string detail=label+": "+reason;
   __android_log_print(ANDROID_LOG_INFO,"PGRazorEvolution","Preview skip id=%llu %s",(unsigned long long)c.id,detail.c_str());if(batch.skipped_details.size()<12)batch.skipped_details.push_back(detail);continue;}
  batch.candidates.push_back(c);batch.candy_cost+=c.candy;batch.item_count+=c.item_count;
 }
 if(batch.scan_index==batch.total_rows){batch.rows.reset();std::string summary=batch.candidates.empty()?"No eligible Pokemon with available resources":"Preview ready; each Pokemon will evolve once";for(const auto &detail:batch.skipped_details)summary+="; Skipped "+detail;if(batch.skipped>int(batch.skipped_details.size()))summary+="; more skipped entries in logs";publish("ready",summary);}
 else if(clock_ms()>batch.last_event+1000)publish("scanning","Checking tagged Pokemon and available candy/items");
}
bool authorized(const Permission &p){return p.valid&&p.allowed&&p.cmd.token==batch.token&&p.cmd.tag==batch.tag&&p.cmd.remove_tag==batch.remove_tag&&p.cmd.mode==2&&!batch.canceled;}
void start_request(){
 if(batch.index>=int(batch.candidates.size())){publish("done","Batch complete");return;}
 Candidate expected=batch.candidates[batch.index++],fresh;Root current=pokemon(expected.id);
 if(!candidate(current,fresh)||!same(expected,fresh)){++batch.skipped;publish("running","Skipped changed or unavailable Pokemon");return;}
 int candy=candy_stock(fresh.family),items=item_stock(fresh.item);Budget budget;if(!budget.reserve(fresh,candy,items)){++batch.skipped;publish("running","Skipped: candy or item stock changed");return;}
 Root req(fn<void*(*)(void*)>(boot.runtime.object_new)(cls("Holoholo.Rpc","EvolvePokemonProto")));require(bool(req),"request allocation");call(req.get(),".ctor");put(req.get(),"PokemonId","System.UInt64",fresh.id);put_enum(req.get(),"TargetPokemonId",fresh.target);put_enum(req.get(),"TargetPokemonForm",fresh.form);put_enum(req.get(),"EvolutionItemRequirement",fresh.item);put(req.get(),"EvolutionItemRequirementCount","System.Int32",fresh.item_count);
 auto final=permission();if(!authorized(final))return;
 batch.sending=fresh;batch.candy_before=candy;batch.item_before=items;batch.since=clock_ms();batch.next=batch.since+1500;uncertain.insert(fresh.id);
 // Game-owned async service reconciles inventory through its own cache. Never
 // synthesize Pokemon/candy mutations and never invoke a cutscene controller.
 void *args[]={current.get(),req.get()};batch.promise=Root(call(batch.bag.get(),"EvolvePokemon",2,args));require(bool(batch.promise),"evolution outcome unknown; inspect inventory");publish("running","Evolving tagged Pokemon");
}
void finish_evolution(){publish(batch.canceled?"stopped":"running",batch.canceled?"Stopped after the in-flight evolution":"Evolution confirmed in inventory");}
Root new_instance(void *k){Root value(fn<void*(*)(void*)>(boot.runtime.object_new)(k));require(bool(value),"tag request allocation");call(value.get(),".ctor");return value;}
Root new_tag_list(void *type,const char *expected){
 require(typename_of(type)==expected,"tag list type mismatch");return new_instance(fn<void*(*)(void*)>(boot.meta.class_from_type)(type));
}
Root build_tag_changes(void *set,Root &current){
 CallContext context;
 managed_context="Tag removal: changes list construction failed";
 Root changes=new_tag_list(fn<void*(*)(void*,unsigned)>(boot.meta.method_get_param)(set,0),"System.Collections.Generic.List<Niantic.Holoholo.Tagging.PokemonTagChanges>");
 managed_context="Tag removal: change construction failed";
 Root change=new_instance(cls("Niantic.Holoholo.Tagging","PokemonTagChanges"));
 putref(change.get(),"PokemonProto","Holoholo.Rpc.PokemonProto",current.get());
 for(const char *name:{"TagsToAdd","TagsToRemove"}){
  void *f=field_info(klass(change.get()),name,"System.Collections.Generic.List<Holoholo.Rpc.PokemonTagProto>");
  managed_context="Tag removal: tag list construction failed";
  Root list=new_tag_list(fn<void*(*)(void*)>(boot.meta.field_get_type)(f),"System.Collections.Generic.List<Holoholo.Rpc.PokemonTagProto>");
  if(!strcmp(name,"TagsToRemove")){managed_context="Tag removal: adding selected tag failed";void *args[]={batch.tag_proto.get()};call(list.get(),"Add",1,args);}
  putref(change.get(),name,"System.Collections.Generic.List<Holoholo.Rpc.PokemonTagProto>",list.get());
 }
 managed_context="Tag removal: adding Pokemon change failed";
 void *add[]={change.get()};call(changes.get(),"Add",1,add);
 return changes;
}
void validate_tag_removal(){
 // Resolve the optional service contract during preview, before consuming candy.
 void *set=method(klass(batch.tag_service.get()),"SetPokemonTags",1);
 require(typename_of(fn<void*(*)(void*)>(boot.meta.method_get_return_type)(set))=="Niantic.Promises.IPromise<System.Boolean>","tag promise type mismatch");
 require(typename_of(fn<void*(*)(void*,unsigned)>(boot.meta.method_get_param)(set,0))=="System.Collections.Generic.List<Niantic.Holoholo.Tagging.PokemonTagChanges>","tag request type mismatch");
 void *change=cls("Niantic.Holoholo.Tagging","PokemonTagChanges");method(change,".ctor",0);
 field_info(change,"PokemonProto","Holoholo.Rpc.PokemonProto");
 field_info(change,"TagsToAdd","System.Collections.Generic.List<Holoholo.Rpc.PokemonTagProto>");
 field_info(change,"TagsToRemove","System.Collections.Generic.List<Holoholo.Rpc.PokemonTagProto>");
 if(batch.rows&&count(batch.rows,25000)>0){Root current=item(batch.rows,0);Root preflight=build_tag_changes(set,current);}
}
void remove_confirmed_tag(){
 // Stop/account changes suppress any new tag request, even when evolution finished.
 if(!batch.remove_tag||batch.canceled||!authorized(permission())){finish_evolution();return;}
 Root current=pokemon(batch.result_id);require(current&&field<int>(current.get(),"pokemonId_","System.Int32")==batch.sending.target,"evolved Pokemon changed before tag removal");
 if(!tagged(current)){finish_evolution();return;}
 require(batch.tag_service&&batch.tag_proto,"tag service unavailable after evolution");
 void *set=method(klass(batch.tag_service.get()),"SetPokemonTags",1);
 require(typename_of(fn<void*(*)(void*)>(boot.meta.method_get_return_type)(set))=="Niantic.Promises.IPromise<System.Boolean>","tag promise type mismatch");
 Root changes=build_tag_changes(set,current);
 if(!authorized(permission())){finish_evolution();return;}
 CallContext context;managed_context="Evolved; tag service call failed. Tag retained; batch stopped.";
 void *args[]={changes.get()};batch.promise=Root(invoke(set,batch.tag_service.get(),args));
 require(bool(batch.promise),"Evolved; tag removal outcome unknown. Inspect tags before retrying.");
 batch.tag_pending=true;batch.since=clock_ms();publish("running","Evolved; removing the selected tag");
}
void poll_tag_removal(){
 if(batch.tag_pending){
  // SetPokemonTags returns IPromise<bool>, not an RPC reference payload.
  require(!field<bool>(batch.promise.get(),"errorCalled","System.Boolean"),"Evolved; tag removal failed. Batch stopped.");
  if(!field<bool>(batch.promise.get(),"completeCalled","System.Boolean")){
   require(clock_ms()<batch.since+15000,"Evolved; tag removal timed out. Inspect tags before retrying.");return;
  }
  bool accepted=field<bool>(batch.promise.get(),"completedValue","System.Boolean");batch.promise.reset();batch.tag_pending=false;
  require(accepted,"Evolved; tag removal declined. Batch stopped.");batch.await_tag=true;batch.since=clock_ms();
 }
 Root current=pokemon(batch.result_id);
 if(current&&!tagged(current)){batch.await_tag=false;publish(batch.canceled?"stopped":"running",batch.canceled?"Stopped after tag removal":"Evolution confirmed; selected tag removed");return;}
 require(clock_ms()<batch.since+15000,"Evolved; tag update not confirmed. Inspect tags before retrying.");
}
void poll_request(){
 if(batch.tag_pending||batch.await_tag){poll_tag_removal();return;}

 if(batch.await_inventory){Root original=pokemon(batch.sending.id);Root replacement=batch.result_id!=batch.sending.id?pokemon(batch.result_id):Root{};
  int original_species=original?field<int>(original.get(),"pokemonId_","System.Int32"):0;
  int replacement_species=replacement?field<int>(replacement.get(),"pokemonId_","System.Int32"):0;
  bool matches=reconciled_identity(batch.sending.id,batch.result_id,batch.sending.target,bool(original),original_species,bool(replacement),replacement_species);
  bool costs=candy_stock(batch.sending.family)<=int64_t(batch.candy_before)-batch.sending.candy+batch.candy_awarded&&item_stock(batch.sending.item)<=batch.item_before-batch.sending.item_count;
  if(matches&&costs){batch.await_inventory=false;uncertain.erase(batch.sending.id);uncertain.erase(batch.result_id);++batch.completed;batch.xp+=batch.pending_xp;remove_confirmed_tag();return;}
  require(clock_ms()<batch.since+15000,"inventory update not confirmed; inspect before retrying");return;
 }
 auto vr=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));require(vr,"promise polling unavailable");PgrVgpResultsResult out{};int rc=vr->poll(batch.promise.get(),1,owner,&out);if(!rc){require(clock_ms()<batch.since+15000,"evolution response timed out; inspect before retrying");return;}
 Root response;response.h=out.value_handle;batch.promise.reset();require(rc==PGR_VR_DONE&&response,"evolution request failed; inspect before retrying");require(klass(response.get())==cls("Holoholo.Rpc","EvolvePokemonOutProto"),"unexpected evolution response");
 if(!success(response.get())){uncertain.erase(batch.sending.id);++batch.skipped;publish("error","Evolution declined (result "+std::to_string(result_value(response.get()))+"); batch stopped");return;}
 Root evolved(field<void*>(response.get(),"EvolvedPokemon","Holoholo.Rpc.PokemonProto"));
 uint64_t returned_id=0;int returned_species=0;
 if(evolved){require(klass(evolved.get())==cls("Holoholo.Rpc","PokemonProto"),"unexpected evolved Pokemon type");returned_id=field<uint64_t>(evolved.get(),"id_","System.UInt64");returned_species=field<int>(evolved.get(),"pokemonId_","System.Int32");}
 // The response belongs to this one outstanding request. Its Pokemon payload
 // can be partial or carry a replacement storage ID. Confirm against the bag;
 // never invent fields or silently accept a different target species.
 batch.result_id=(returned_id&&returned_species==batch.sending.target)?returned_id:batch.sending.id;
 __android_log_print(ANDROID_LOG_INFO,"PGRazorEvolution","Response original=%llu expectedSpecies=%d returned=%llu returnedSpecies=%d lookup=%llu",(unsigned long long)batch.sending.id,batch.sending.target,(unsigned long long)returned_id,returned_species,(unsigned long long)batch.result_id);
 require(batch.result_id==batch.sending.id||!batch.seen.count(batch.result_id),"evolution returned another batch Pokemon; inspect inventory");
 batch.pending_xp=field<int>(response.get(),"ExpAwarded","System.Int32");batch.candy_awarded=field<int>(response.get(),"CandyAwarded","System.Int32");require(batch.pending_xp>=0&&batch.candy_awarded>=0,"invalid evolution reward");
 uncertain.insert(batch.result_id);batch.await_inventory=true;batch.since=clock_ms();publish("running","Server accepted evolution; waiting for inventory update");
}
} // namespace
void pgr_evolution_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid))return;struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 try{auto p=permission();
 // Stop can arrive before the first preview tick. Acknowledge that token even
 // before services are initialized, so Java does not wait forever for a drain.
 if(p.valid&&p.cmd.mode==0&&p.cmd.token&&p.cmd.token!=batch.token&&!batch.promise&&!batch.await_inventory&&!batch.await_tag){batch=Batch{};batch.token=p.cmd.token;publish("stopped","Batch stopped");return;}
 if(!initialized){if(!p.allowed)return;auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(api&&api->snapshot(&boot)==0,"runtime bootstrap");md=metadata;require(md,"shared metadata");owner=tid;initialized=true;}
 require(fn<void*(*)()>(boot.runtime.thread_current)()!=nullptr,"owner not attached");
 if(batch.token&&batch.phase!="idle"&&batch.phase!="done"&&batch.phase!="stopped"&&batch.phase!="error"){
  if(!p.allowed||p.cmd.token!=batch.token||p.cmd.tag!=batch.tag||p.cmd.remove_tag!=batch.remove_tag||p.cmd.mode==0)batch.canceled=true;
  Root player=resolve("Niantic.Holoholo","IPlayerService");if(!player||string(call(player.get(),"get_PlayerId"))!=batch.account)batch.canceled=true;
 }
 if(batch.promise||batch.await_inventory||batch.await_tag){poll_request();return;}
 if(batch.canceled&&batch.phase!="stopped"&&batch.phase!="error"&&batch.phase!="done")publish("stopped","Batch stopped; preview again to continue");
 if(!p.allowed)return;
 if(p.cmd.token!=batch.token){if(p.cmd.mode==1)prepare(p.cmd);return;}
 if(batch.canceled||batch.phase=="done"||batch.phase=="error"||batch.phase=="stopped")return;
 if(batch.phase=="scanning"){scan();return;}
 if(p.cmd.mode==2&&batch.phase=="ready"){batch.started=clock_ms();publish("running","Starting batch");}
 if(batch.phase=="running"&&authorized(p)&&clock_ms()>=batch.next)start_request();
 }catch(const Failure &e){batch.promise.reset();batch.await_inventory=false;batch.tag_pending=false;batch.await_tag=false;publish("error",e.where);}catch(...){batch.promise.reset();batch.await_inventory=false;batch.tag_pending=false;batch.await_tag=false;publish("error","Evolution engine stopped after an unexpected error");}
}
void pgr_evolution_stop(uint64_t tid){if(initialized&&tid==owner&&tid==uint64_t(gettid())){batch.canceled=true;batch.promise.reset();batch.await_inventory=false;batch.tag_pending=false;batch.await_tag=false;publish("stopped","Session ended; preview again to continue");}}

#include "research.h"
#include "game_bindings.h"
#include "research_policy.h"
#include "shared_metadata.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "vgp_results.h"
#include "vgp_aux.h"
#include <cctype>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#include <time.h>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
// Field research automation: claims completed field research, removes field research whose template
// the player selected, and optionally catches the waiting research reward encounters without the
// encounter screen. Every request goes through the game's own quest services or RPC handler, one at a
// time. The field research catalog (template, localized task text, reward) is published for the
// settings list. Caught Pokemon go through the VGP transfer handoff, honoring Keep All.
extern "C" int pgr_research_transfer_caught(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,int);
namespace {
using namespace pgr_shared_meta;
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
template<class F>F fn(void *p){require(p,"missing runtime export");return reinterpret_cast<F>(p);}
PgrBootBinding boot{};const PgrSharedMetadataOutput *md=nullptr;
uint64_t owner=0;bool busy=false,ready=false;
constexpr uint64_t kScanMs=2000,kRetryMs=10000,kTimeoutMs=20000,kGapMs=1500,kWindowMs=600000;
constexpr unsigned kMaxQuests=200,kMaxActionsPerWindow=40;
uint64_t clock_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
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
std::string class_name(void *o){return fn<const char*(*)(void*)>(exp("class_get_name"))(klass(o));}
void *cls(const char *ns,const char *name){
 static std::map<std::string,void*> cache;std::string key=std::string(ns)+"."+name;auto it=cache.find(key);if(it!=cache.end())return it->second;
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api,"metadata API");
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult out{};require(api->resolve_class(&q,&out)==0,name);return cache[key]=const_cast<void*>(out.klass);
}
std::string typename_of(void *type){char *p=fn<char*(*)(void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
void *field_info(void *o,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name);require(f,name);require(!(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10),name);return f;}
std::string field_type(void *o,const char *name){return typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(field_info(o,name)));}
template<class T>T field(void *o,const char *name,const char *type){require(field_type(o,name)==type,name);T v{};fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,field_info(o,name),&v);return v;}
template<class T>void put(void *o,const char *name,const char *type,T value){require(field_type(o,name)==type,name);fn<void(*)(void*,void*,void*)>(boot.runtime.field_set_value)(o,field_info(o,name),&value);}
void put_object(void *o,const char *name,const char *type,void *value){require(field_type(o,name)==type,name);fn<void(*)(void*,void*,void*)>(exp("field_set_value_object"))(o,field_info(o,name),value);}
void *object_field(void *o,const char *name,const char *type){return field<void*>(o,name,type);}
// The enum value plus its class, so literals are read from the game instead of guessed.
int enum_field(void *o,const char *name,const char *type,void **enum_class){
 void *f=field_info(o,name);void *t=fn<void*(*)(void*)>(boot.meta.field_get_type)(f);require(typename_of(t)==type,name);
 void *k=fn<void*(*)(void*)>(boot.meta.class_from_type)(t);require(k&&fn<bool(*)(void*)>(exp("class_is_enum"))(k),name);
 if(enum_class)*enum_class=k;int v=0;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;
}
int literal(void *k,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);require((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)==0x8050,name);int n=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&n);return n;}
std::string literal_name(void *k,int value){void *iter=nullptr;
 while(auto f=fn<void*(*)(void*,void**)>(exp("class_get_fields"))(k,&iter)){
  if((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)!=0x8050)continue;int n=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&n);
  if(n==value)return fn<const char*(*)(void*)>(exp("field_get_name"))(f);}
 return "Unknown "+std::to_string(value);}
void *method(void *o,const char *name,int count){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(klass(o),name,count);require(m,name);require(fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(m)==unsigned(count),name);return m;}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(o,name,n),o,args);}
template<class T>T unbox(void *o){require(o,"boxed value");T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");std::memcpy(&v,p,sizeof(v));return v;}
// UTF-16 managed string to UTF-8, bounded.
std::string text(void *o){
 if(!o)return {};require(klass(o)==cls("System","String"),"string identity");
 int n=fn<int(*)(void*)>(exp("string_length"))(o);require(n>=0&&n<=512,"string bound");auto p=fn<const uint16_t*(*)(void*)>(boot.runtime.string_chars)(o);
 std::string s;for(int i=0;i<n;i++){uint32_t c=p[i];
  if(c>=0xd800&&c<0xdc00&&i+1<n&&p[i+1]>=0xdc00&&p[i+1]<0xe000){c=0x10000+((c-0xd800)<<10)+(p[i+1]-0xdc00);++i;}
  else if(c>=0xd800&&c<0xe000)c=0xfffd;
  if(c<0x80)s+=char(c);else if(c<0x800){s+=char(0xc0|c>>6);s+=char(0x80|(c&63));}
  else if(c<0x10000){s+=char(0xe0|c>>12);s+=char(0x80|(c>>6&63));s+=char(0x80|(c&63));}
  else{s+=char(0xf0|c>>18);s+=char(0x80|(c>>12&63));s+=char(0x80|(c>>6&63));s+=char(0x80|(c&63));}}
 return s;}
Root managed_string(const std::string &s){return Root(fn<void*(*)(const char*)>(boot.runtime.string_new)(s.c_str()));}
void *h(unsigned id){return const_cast<void*>(md->opt_handles[id]);}
Root type_object(void *k){return Root(fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k)));}
Root resolve(const char *ns,const char *name){
 Root type=type_object(h(H_IL_MAP));void *args[]={type.get()};Root map(invoke(h(H_IL_FIND_M),nullptr,args));if(!map)return {};
 Root container(invoke(h(H_IL_CONTAINER_M),map.get()));if(!container)return {};
 Root target=type_object(cls(ns,name));void *resolve_args[]={target.get()};return Root(invoke(h(H_IL_RESOLVE_M),container.get(),resolve_args));
}
int count(void *list,unsigned cap){int n=unbox<int>(call(list,"get_Count"));require(n>=0&&unsigned(n)<=cap,"collection bound");return n;}
Root item(void *list,int index){void *args[]={&index};return Root(call(list,"get_Item",1,args));}
std::string quote(const std::string &s){std::string out="\"";for(unsigned char c:s){if(c=='"'||c=='\\')out+='\\';if(c>=32)out+=char(c);}return out+'"';}
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
void event(const std::string &json){auto tr=transport();if(!tr)return;PgrRuntimeTransportString strings[]={{"research",8},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,strings,2);}
void status(const char *phase,const std::string &message,const std::string &templ=""){
 event("{\"kind\":\"research\",\"phase\":"+quote(phase)+",\"template\":"+quote(templ)+",\"message\":"+quote(message)+"}");}
// The snapshot config, read between two equal fresh samples. Invalid or stale means no actions.
pgr_research::Config config(){
 pgr_research::Config c;auto tr=transport();if(!tr)return c;PgrRuntimeTransportSample a{},z{};
 if(tr->sample(&a)||!a.snapshot_fresh||a.fault)return c;
 static char s[pgr_research::kMaxTemplates*(pgr_research::kMaxTemplateBytes+1)+16];uint32_t n=0;
 if(tr->get_string(13,s,sizeof(s),&n)||n>=sizeof(s))return c;
 if(tr->sample(&z)||!z.snapshot_fresh||z.fault||a.snapshot_seq!=z.snapshot_seq)return c;
 pgr_research::Config parsed;if(n&&pgr_research::parse(s,n,parsed))c=parsed;return c;
}
// VGP+ ball, throw rating and curveball (transport slot 7: <1|2>;enabled;ball;berry;premium;cp;level;grade;curve;...).
// The first nine fields are the same in versions 1 and 2; VGP+ auto ball is not applied here.
struct Throw {int ball=1,grade=3;bool curve=true;};
Throw throw_settings(){
 Throw t;auto tr=transport();char s[512];uint32_t n=0;if(!tr||tr->get_string(7,s,sizeof(s),&n)||n>=sizeof(s))return t;
 std::vector<int> v;std::string part;
 for(uint32_t i=0;i<=n&&v.size()<9;i++){if(i==n||s[i]==';'){v.push_back(part.empty()||part.size()>6?-1:std::atoi(part.c_str()));part.clear();}else part+=s[i];}
 if(v.size()==9&&(v[0]==1||v[0]==2)){if(v[2]>=1&&v[2]<=3)t.ball=v[2];if(v[7]>=0&&v[7]<=3)t.grade=v[7];if(v[8]==0||v[8]==1)t.curve=v[8]==1;}
 return t;
}
bool keep_all(){auto tr=transport();int v=0;return tr&&tr->get_bool(35,&v)==0&&v==1;}
enum Encounter {E_IDLE,E_BEGIN,E_CATCH,E_TRANSFER};
constexpr int kMaxThrows=12;constexpr uint64_t kTransferMs=20000;
struct State {
 Root quests,rpc,i18n,player;std::string account;
 Encounter enc=E_IDLE;Root encounter,catch_response,pokemon_bag,send_reflection;void *send_method=nullptr;
 std::string enc_id;uint64_t enc_pokemon=0;int throws=0;Throw ball;
 std::set<std::string> enc_handled;  // waiting encounters already attempted; never attempted twice
 int challenge=-1,active=-1;
 Root promise;pgr_research::Action pending=pgr_research::NONE;std::string pending_templ;uint64_t since=0;
 std::set<std::string> handled;      // quest instances requested (or given up on); not requested again
 std::string pending_id;std::map<std::string,int> failed;std::map<std::string,uint64_t> not_before;  // removal retries
 std::map<std::string,std::string> labels;  // template -> "task text|reward", localized once
 std::string catalog;uint64_t next_scan=0,next_action=0,retry=0,window=0,recheck=0;unsigned actions=0;
};
State st;
constexpr uint64_t kRecheckMs=60000;
bool services(){
 if(st.quests&&st.rpc&&st.player){
  // The game can rebuild its services in a running session; a stale quest service lists stale quests.
  const uint64_t now=clock_ms();if(st.enc!=E_IDLE||st.promise||now<st.recheck)return true;st.recheck=now+kRecheckMs;
  Root current=resolve("Niantic.Holoholo.Quests","IQuestService");
  if(!current||current.get()==st.quests.get())return true;
  // Services are looked up again; the quests already requested stay recorded (ids are per account).
  st.quests.reset();st.rpc.reset();st.player.reset();st.i18n.reset();st.pokemon_bag.reset();st.send_reflection.reset();
  st.send_method=nullptr;st.retry=0;st.next_scan=0;status("ready","Quest service changed; services looked up again");return false;
 }
 const uint64_t now=clock_ms();if(now<st.retry)return false;st.retry=now+kRetryMs;
 st.quests=resolve("Niantic.Holoholo.Quests","IQuestService");st.rpc=resolve("Niantic.Holoholo.Quests","IQuestRpcService");
 st.player=resolve("Niantic.Holoholo","IPlayerService");st.i18n=resolve("Niantic.Holoholo.Internationalization","II18n");
 if(!st.quests||!st.rpc||!st.player)return false;
 require(class_name(st.quests.get())=="QuestService"&&class_name(st.rpc.get())=="QuestRpcService","Quest service identity");
 st.account=text(call(st.player.get(),"get_PlayerId"));require(!st.account.empty(),"Account identity unavailable");
 return true;
}
std::string localized(const std::string &key){
 if(!st.i18n||key.empty())return key;Root k=managed_string(key);void *args[]={k.get()};
 try{return text(call(st.i18n.get(),"Text",1,args));}catch(const Failure&){return key;}
}
// The encounter species is best effort (a random encounter has none); a reward list that cannot be
// read is retried next scan. The species sits in a protobuf oneof, so it is read through its getter.
std::string reward_label(void *quest,bool &encounter){
 Root list(object_field(quest,"QuestRewards","Niantic.Protobuf.Collections.RepeatedField<Holoholo.Rpc.QuestRewardProto>"));if(!list)return "";
 int n=count(list.get(),16);std::string out;
 for(int i=0;i<n;i++){
  Root r=item(list.get(),i);if(!r)continue;void *k=nullptr;
  const int type=enum_field(r.get(),"Type","Holoholo.Rpc.QuestRewardProto.Types.Type",&k);std::string name=literal_name(k,type);
  if(name=="PokemonEncounter"){
   std::string pokemon;
   try{Root e(call(r.get(),"get_PokemonEncounter"));int species=unbox<int>(call(e.get(),"get_PokemonId"));
    void *args[]={&species};if(species>0&&st.i18n)pokemon=text(call(st.i18n.get(),"PokemonName",1,args));}
   catch(const Failure&){}
   name=pokemon.empty()?"Pokémon encounter":"Encounter: "+pokemon;encounter=true;
  }else if(name=="MegaResource")name="Mega Energy";
  if(!out.empty())out+=", ";out+=name;
 }
 return out;
}
// "<category>task text|reward · tag", cached per template once fully localized; until then retried
// each scan. The game's tag (e.g. Event) tells apart tasks that share their text. Category: 'v' for a
// task the game tags (any tag but Sponsored), else 'e' for a Pokemon encounter reward, else 'i' item.
std::string label(void *quest,const std::string &id,const std::string &templ,int target){
 auto it=st.labels.find(templ);if(it!=st.labels.end())return it->second;
 std::string task=templ,tags;bool complete=false,tagged=false,encounter=false;
 try{Root qid=managed_string(id);void *args[]={qid.get()};Root display(call(st.quests.get(),"GetQuestDisplayProto",1,args));
  if(display){const std::string key=text(object_field(display.get(),"Description","System.String"));task=localized(key);
   complete=!key.empty()&&task!=key;
   const auto at=task.find("{0}");if(at!=std::string::npos)task.replace(at,3,std::to_string(target));
   try{const std::string key=text(object_field(display.get(),"TagString","System.String"));const std::string tag=localized(key);
    if(!key.empty()&&tag!=key)tags=tag;
    std::string lower=key;for(char &c:lower)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    tagged=!key.empty()&&lower.find("sponsor")==std::string::npos;}catch(const Failure&){}}}
 catch(const Failure&){}
 std::string reward;try{reward=reward_label(quest,encounter);}catch(const Failure&){complete=false;}
 if(!tags.empty())reward+=(reward.empty()?"":" · ")+tags;
 const std::string out=std::string(1,tagged?'v':encounter?'e':'i')+task+"|"+reward;
 if(complete){if(st.labels.size()>=pgr_research::kMaxTemplates)st.labels.clear();st.labels[templ]=out;}
 return out;
}
// Active field research, read from the game's own quest cache.
std::vector<pgr_research::Quest> field_research(){
 Root qp(call(st.quests.get(),"get_QuestsProto"));require(bool(qp)&&class_name(qp.get())=="QuestsProto","Quest list identity");
 Root list(object_field(qp.get(),"Quest","Niantic.Protobuf.Collections.RepeatedField<Holoholo.Rpc.QuestProto>"));require(bool(list),"Quest list");
 const int n=count(list.get(),kMaxQuests);std::vector<pgr_research::Quest> out;std::string catalog;
 for(int i=0;i<n;i++){
  Root q=item(list.get(),i);require(q&&class_name(q.get())=="QuestProto","Quest identity");
  void *context_class=nullptr,*status_class=nullptr;pgr_research::Quest f;
  f.context=enum_field(q.get(),"QuestContext","Holoholo.Rpc.QuestProto.Types.Context",&context_class);
  if(st.challenge<0)st.challenge=literal(context_class,"ChallengeQuest");
  if(f.context!=st.challenge)continue;
  f.status=enum_field(q.get(),"Status","Holoholo.Rpc.QuestProto.Types.Status",&status_class);
  if(st.active<0)st.active=literal(status_class,"Active");
  f.id=text(object_field(q.get(),"QuestId","System.String"));f.templ=text(object_field(q.get(),"TemplateId","System.String"));
  f.progress=field<int>(q.get(),"Progress","System.Int32");f.bonus=field<uint8_t>(q.get(),"IsBonusChallenge","System.Boolean")!=0;
  if(void *goal=object_field(q.get(),"Goal","Holoholo.Rpc.QuestGoalProto")){Root g(goal);f.target=field<int>(g.get(),"Target","System.Int32");}
  bool valid=!f.templ.empty()&&f.templ.size()<=pgr_research::kMaxTemplateBytes;for(char c:f.templ)valid=valid&&pgr_research::template_char(c);
  if(!valid)continue;
  if(!f.bonus){const std::string l=label(q.get(),f.id,f.templ,f.target);const auto bar=l.find('|');
   catalog+=(catalog.empty()?"":",")+std::string("{\"t\":")+quote(f.templ)+",\"d\":"+quote(l.substr(1,bar-1))+",\"r\":"+quote(l.substr(bar+1))+",\"c\":\""+l[0]+"\"}";}
  out.push_back(f);
 }
 if(catalog!=st.catalog){st.catalog=catalog;event("{\"kind\":\"research\",\"phase\":\"catalog\",\"tasks\":["+catalog+"]}");}
 return out;
}
void submit(const pgr_research::Quest &q,pgr_research::Action action){
 Root id=managed_string(q.id);st.handled.insert(q.id);
 st.pending=action;st.pending_templ=q.templ;st.pending_id=q.id;st.since=clock_ms();++st.actions;
 if(action==pgr_research::CLAIM){
  // The game's own claim path: no sub quest, no branch choice, a full stamp card is not bypassed.
  Root sub=managed_string("");bool exclusive=false,bypass=false;struct {uint8_t has;int32_t value;} choice{0,0};uint64_t pokemon=0;
  void *args[]={id.get(),sub.get(),&exclusive,&choice,&bypass,&pokemon};
  st.promise=Root(call(st.quests.get(),"CompleteQuest",6,args));
 }else{void *args[]={id.get()};st.promise=Root(call(st.rpc.get(),"RemoveQuest",1,args));}
 require(bool(st.promise),"Request outcome unknown; not retried");
}
void finish(bool ok,const std::string &message){
 const bool claim=st.pending==pgr_research::CLAIM;std::string msg=message;
 if(!ok&&st.pending==pgr_research::REMOVE&&!st.pending_id.empty()){
  if(const uint64_t delay=pgr_research::remove_retry_ms(++st.failed[st.pending_id])){
   st.handled.erase(st.pending_id);st.not_before[st.pending_id]=clock_ms()+delay;
   const auto cut=msg.find("; not retried");if(cut!=std::string::npos)msg.erase(cut);
   msg+="; retrying in "+std::to_string(delay/1000)+" s";
  }
 }
 status(ok?(claim?"claimed":"removed"):"error",msg,st.pending_templ);
 st.promise.reset();st.pending=pgr_research::NONE;st.pending_templ.clear();st.pending_id.clear();
 st.next_action=clock_ms()+kGapMs;st.next_scan=0;
}
void poll(uint64_t tid){
 auto api=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));require(api,"Promise observer unavailable");
 PgrVgpResultsResult out{};const int rc=api->poll(st.promise.get(),1,tid,&out);Root result;result.h=out.value_handle;
 const bool claim=st.pending==pgr_research::CLAIM;
 if(!rc){if(clock_ms()-st.since>=kTimeoutMs)finish(false,"Request outcome unknown; not retried");return;}
 if(rc!=PGR_VR_DONE||!result){finish(false,claim?"Claim failed; not retried":"Removal failed; not retried");return;}
 require(class_name(result.get())==(claim?"CompleteQuestOutProto":"RemoveQuestOutProto"),"Quest response identity");
 void *k=nullptr;const int value=enum_field(result.get(),"Status",claim?"Holoholo.Rpc.CompleteQuestOutProto.Types.Status":"Holoholo.Rpc.RemoveQuestOutProto.Types.Status",&k);
 if(value==literal(k,"Success"))finish(true,claim?"Claimed field research reward":"Removed unwanted field research");
 else finish(false,(claim?"Claim refused: ":"Removal refused: ")+literal_name(k,value));
}
// The waiting research reward encounters, as the game's quest cache lists them.
Root waiting_encounters(){
 Root qp(call(st.quests.get(),"get_QuestsProto"));require(bool(qp),"Quest list identity");
 return Root(object_field(qp.get(),"QuestPokemonEncounter","Niantic.Protobuf.Collections.RepeatedField<Holoholo.Rpc.QuestPokemonEncounterProto>"));
}
void enc_done(const std::string &message,bool ok){
 st.promise.reset();st.encounter.reset();st.catch_response.reset();st.enc=E_IDLE;st.throws=0;
 status(ok?"caught":"error",message);st.next_action=clock_ms()+kGapMs;st.next_scan=0;
}
// The VGP+ ball when the bag has one, else the next regular ball; 0 when none are left.
int available_ball(int preferred){
 Root bag=resolve("Niantic.Holoholo","IItemBag");require(bool(bag),"item bag unavailable");
 for(int ball:{preferred,1,2,3}){int item=ball;void *args[]={&item};if(unbox<int>(call(bag.get(),"GetItemCount",1,args))>0)return ball;}
 return 0;
}
// Starts one waiting field research encounter through the game's quest RPC service.
bool begin_encounter(){
 Root list=waiting_encounters();if(!list)return false;const int n=count(list.get(),50);
 for(int i=0;i<n;i++){
  Root e=item(list.get(),i);require(e&&class_name(e.get())=="QuestPokemonEncounterProto","Encounter identity");
  const std::string id=text(object_field(e.get(),"QuestId","System.String"));if(id.empty()||st.enc_handled.count(id))continue;
  void *k=nullptr;const int type=enum_field(e.get(),"EncounterType","Holoholo.Rpc.EncounterType",&k);
  if(type!=literal(k,"ChallengeQuest"))continue;  // field research rewards only; weekly/party and other encounters stay manual
  Root pokemon(object_field(e.get(),"Pokemon","Holoholo.Rpc.PokemonProto"));require(bool(pokemon),"Encounter Pokemon");
  st.ball=throw_settings();if(!available_ball(st.ball.ball)){status("error","No Poké, Great or Ultra Balls left; encounters wait");st.next_action=clock_ms()+kRetryMs*6;return false;}
  st.enc_handled.insert(id);st.enc_id=id;st.enc_pokemon=field<uint64_t>(pokemon.get(),PGR_GAME_SHARED_METADATA_CPP_H_DET_ID_NAME,"System.UInt64");require(st.enc_pokemon!=0,"Encounter id");
  st.encounter=std::move(e);st.throws=0;
  void *args[]={st.encounter.get()};st.promise=Root(call(st.rpc.get(),"BeginQuestEncounter",1,args));require(bool(st.promise),"Encounter outcome unknown; not retried");
  st.enc=E_BEGIN;st.since=clock_ms();++st.actions;return true;
 }
 return false;
}
// The game's typed RPC handler, closed over CatchPokemonProto -> CatchPokemonOutProto once.
void *catch_sender(Root &rpc){
 Root bag=resolve("Niantic.Holoholo","IItemBag");require(bool(bag),"item bag unavailable");
 rpc=Root(field<void*>(bag.get(),PGR_GAME_GYM_RUNTIME_CPP_ERHP_NAME,"Niantic.Holoholo.Rpc.IRpcHandler"));require(bool(rpc),"RPC unavailable");
 if(st.send_method)return st.send_method;
 void *open=method(rpc.get(),"SendRpc",5);require(fn<bool(*)(void*)>(boot.meta.method_is_generic)(open),"generic RPC");
 Root reflected(fn<void*(*)(void*,void*)>(exp("method_get_object"))(open,klass(rpc.get())));
 Root types(fn<void*(*)(void*,uintptr_t)>(boot.runtime.array_new)(cls("System","Type"),2));
 void *ks[]={cls("Holoholo.Rpc","CatchPokemonProto"),cls("Holoholo.Rpc","CatchPokemonOutProto")};
 for(int i=0;i<2;i++){Root t=type_object(ks[i]);void **slot=reinterpret_cast<void**>(static_cast<char*>(types.get())+32+i*8);fn<void(*)(void*,void**,void*)>(exp("gc_wbarrier_set_field"))(types.get(),slot,t.get());}
 void *args[]={types.get()};st.send_reflection=Root(call(reflected.get(),"MakeGenericMethod",1,args));
 void *closed=fn<void*(*)(void*)>(exp("method_get_from_reflection"))(st.send_reflection.get());require(closed&&fn<bool(*)(void*)>(boot.meta.method_is_inflated)(closed),"closed RPC");
 return st.send_method=closed;
}
// One throw with the VGP+ ball and rating. Ratings are requested, not guaranteed outcomes.
void throw_ball(){
 const int ball=available_ball(st.ball.ball);if(!ball){enc_done("Out of balls after "+std::to_string(st.throws)+" throws",false);return;}
 Root rpc;void *send=catch_sender(rpc);
 Root request(fn<void*(*)(void*)>(boot.runtime.object_new)(cls("Holoholo.Rpc","CatchPokemonProto")));require(bool(request),"request allocation");call(request.get(),".ctor");
 const double reticle=st.ball.grade==3?1.95:st.ball.grade==2?1.5:st.ball.grade==1?1.15:1.0;
 put<uint64_t>(request.get(),"EncounterId","System.UInt64",st.enc_pokemon);
 put<int>(request.get(),"Pokeball","System.Int32",ball);  // protocol item ids, as VGP+ stores them: 1 Poke, 2 Great, 3 Ultra
 put<double>(request.get(),"NormalizedReticleSize","System.Double",reticle);
 Root spawn=managed_string(st.enc_id);put_object(request.get(),"SpawnPointGuid","System.String",spawn.get());
 put<bool>(request.get(),"HitPokemon","System.Boolean",true);
 put<double>(request.get(),"SpinModifier","System.Double",st.ball.curve?0.85:0.0);
 put<double>(request.get(),"NormalizedHitPosition","System.Double",1.0);
 int id=literal(cls("Holoholo.Rpc","Method"),"CatchPokemon"),zero=0,timeout=10000;void *args[]={&id,request.get(),&zero,&timeout,&zero};
 ++st.throws;st.promise=Root(invoke(send,rpc.get(),args));require(bool(st.promise),"Catch outcome unknown; not retried");
 st.enc=E_CATCH;st.since=clock_ms();
}
// Hands a caught Pokemon to the VGP transfer path once the bag lists it; Keep All is honored.
void transfer_caught(uint64_t tid){
 if(!st.pokemon_bag)st.pokemon_bag=resolve("Niantic.Holoholo","IPokemonBag");
 auto aux=static_cast<const PgrVgpAuxApi*>(pgrazor_vgp_aux_api(1));
 if(!aux||!st.pokemon_bag){enc_done("Caught; transfer unavailable",true);return;}
 PgrVgpAuxTransfer out{};out.version=1;out.size=sizeof(out);
 const int rc=aux->transfer(st.catch_response.get(),st.pokemon_bag.get(),tid,2500,&out);
 if(rc==PGR_VX_OK){if(clock_ms()-st.since>=kTransferMs)enc_done("Caught; transfer inventory update timed out",true);return;}
 if(rc!=PGR_VX_SUCCESS){enc_done("Caught; transfer skipped",true);return;}
 Root response(st.catch_response.get()),bag(st.pokemon_bag.get());
 if(pgr_research_transfer_caught(out.caught_id,response.h,bag.h,out.pokemon_handle,out.gm_handle,tid,keep_all()?1:0)){
  response.h=0;bag.h=0;enc_done("Caught research encounter; checking transfer rules",true);return;}
 fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(out.pokemon_handle);fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(out.gm_handle);
 if(clock_ms()-st.since>=kTransferMs)enc_done("Caught; transfer busy",true);
}
void poll_encounter(uint64_t tid){
 if(st.enc==E_TRANSFER){transfer_caught(tid);return;}
 auto api=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));require(api,"Promise observer unavailable");
 PgrVgpResultsResult out{};const int rc=api->poll(st.promise.get(),1,tid,&out);Root result;result.h=out.value_handle;
 if(!rc){if(clock_ms()-st.since>=kTimeoutMs)enc_done("Encounter outcome unknown; not retried",false);return;}
 if(rc!=PGR_VR_DONE||!result){enc_done(st.enc==E_BEGIN?"Encounter failed; not retried":"Catch failed; not retried",false);return;}
 st.promise.reset();void *k=nullptr;
 if(st.enc==E_BEGIN){
  require(class_name(result.get())=="QuestEncounterOutProto","Encounter response identity");
  const int value=enum_field(result.get(),"Result","Holoholo.Rpc.QuestEncounterOutProto.Types.Result",&k);
  if(value!=literal(k,"QuestEncounterSuccess")){enc_done("Encounter refused: "+literal_name(k,value),false);return;}
  throw_ball();return;
 }
 require(class_name(result.get())=="CatchPokemonOutProto","Catch response identity");
 const int value=enum_field(result.get(),"Status","Holoholo.Rpc.CatchPokemonOutProto.Types.Status",&k);const std::string name=literal_name(k,value);
 if(name=="CatchSuccess"){st.catch_response=std::move(result);st.enc=E_TRANSFER;st.since=clock_ms();transfer_caught(tid);return;}
 if((name=="CatchEscape"||name=="CatchMissed")&&st.throws<kMaxThrows){throw_ball();return;}
 enc_done(name=="CatchEscape"||name=="CatchMissed"?"Stopped after "+std::to_string(st.throws)+" throws":"Catch refused: "+name,false);
}

void step(uint64_t tid){
 if(!ready){auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(md&&api&&api->snapshot(&boot)==0,"Runtime unavailable");ready=true;}
 if(!services())return;
 if(st.enc!=E_IDLE){poll_encounter(tid);return;}
 if(st.promise){poll(tid);return;}
 // A switched account rebuilds every service and forgets its quests.
 if(text(call(st.player.get(),"get_PlayerId"))!=st.account){st=State{};return;}
 const uint64_t now=clock_ms();if(now<st.next_scan)return;st.next_scan=now+kScanMs;
 const auto c=config();const auto quests=field_research();
 if(!c.valid||(!c.claim&&!c.remove&&!c.catch_encounters)||now<st.next_action)return;
 if(now-st.window>=kWindowMs){st.window=now;st.actions=0;}
 if(st.actions>=kMaxActionsPerWindow)return;
 // Claims first; one request at a time, each quest instance at most once.
 for(auto wanted:{pgr_research::CLAIM,pgr_research::REMOVE})
  for(const auto &q:quests)
   if(!st.handled.count(q.id)&&!(st.not_before.count(q.id)&&now<st.not_before[q.id])&&pgr_research::decide(q,c,st.challenge,st.active)==wanted){
    if(wanted==pgr_research::CLAIM&&unbox<bool>(call(st.quests.get(),"get_ExclusiveQuestCompletionInProgress")))return;
    submit(q,wanted);return;
   }
 // Then the waiting research encounters, one at a time. VGP may run alongside: the caught
 // Pokemon waits for the shared transfer slot to be free.
 if(c.catch_encounters)begin_encounter();
}
}  // namespace
void pgr_research_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid))return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 owner=tid;md=metadata;
 try{step(tid);}
 catch(const Failure &e){
  if(st.enc!=E_IDLE)enc_done(e.where,false);else if(st.pending!=pgr_research::NONE)finish(false,e.where);else status("error",e.where);
  st.next_scan=clock_ms()+kRetryMs;
 }catch(...){st.promise.reset();st.pending=pgr_research::NONE;st.enc=E_IDLE;st.encounter.reset();st.catch_response.reset();status("error","Research stopped: native failure");st.next_scan=clock_ms()+kRetryMs;}
}
void pgr_research_stop(){if(owner&&owner!=uint64_t(gettid()))return;if(!busy){st.promise.reset();st.pending=pgr_research::NONE;st.enc=E_IDLE;st.encounter.reset();st.catch_response.reset();}}

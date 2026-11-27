#include "smart_battle.h"
#include "battle_counter.h"
#include "counter_engine.h"
#include "smart_battle_party.h"
#include "pokemon_storage.h"
#include "combat_host.h"
#include "game_bindings.h"
#include "hook_engine.h"
#include "rocket_combat.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include "shared_metadata.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// Before battle: BattlePartyService.ConstructPartyAgainst (gyms, raids) and FindGruntParty (Rocket) are
// the game's own "which team" answers, so their result is swapped for one built from battle_counter.h.
// Typed Rocket grunts reveal their type through the InvasionCharacter name; other grunts and leaders
// keep the game's team because nothing about their line-up is known in advance.
//
// In battle: every 250 ms the opponent and active Pokemon are read. When either changes, the choice is
// made once after a short settle, and a swap goes through the game's own swap menu (Open, then the
// button's Swap, exactly what tapping does). Gyms use the older BattleFrame path, Rocket the VNext
// BattleState path. Raids are left alone in battle because the boss never changes.
//
// All game data (types, base stats, moves) comes from the game master at runtime.
namespace {
using namespace pgr_counter;
using namespace pgr_shared_meta;
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
PgrBootBinding boot{};const PgrSharedMetadataOutput *md=nullptr;
bool busy=false,initialized=false,hooks_tried=false;
std::atomic<bool> enabled{false},hooks_ready{false};std::atomic<uint64_t> owner{0};
constexpr uint64_t kStepMs=250,kSettleMs=1200,kOpenMs=350,kPressRetryMs=500,kPressGiveUpMs=4500,kTableRetryMs=5000;
constexpr int kMaxCandidates=60;

uint64_t clock_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
template<class F>F fn(const void *p){require(p,"missing runtime export");return reinterpret_cast<F>(const_cast<void*>(p));}
void *exp(const char *name){std::string n="il2cpp_";n+=name;void *p=dlsym(boot.handle,n.c_str());require(p,name);return p;}
struct Root {
 uintptr_t h=0;
 Root()=default;explicit Root(void *v){if(v){h=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(v,0);require(h,"GC root");}}
 Root(const Root&)=delete;Root& operator=(const Root&)=delete;
 Root(Root &&r) noexcept:h(r.h){r.h=0;}
 Root& operator=(Root &&r) noexcept {if(this!=&r){reset();h=r.h;r.h=0;}return *this;}
 ~Root(){reset();}
 void reset(){if(h){fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);h=0;}}
 void *get()const{return h?fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h):nullptr;}
 explicit operator bool()const{return get()!=nullptr;}
};
void *klass(void *o){require(o,"null object");return fn<void*(*)(void*)>(boot.runtime.object_get_class)(o);}
void *cls(const char *ns,const char *name){
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api,"metadata API");
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult out{};require(api->resolve_class(&q,&out)==0,name);return const_cast<void*>(out.klass);
}
std::string typename_of(void *type){char *p=fn<char*(*)(void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
void *method(void *k,const char *name,int count){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,count);require(m,name);return m;}
// Which managed call threw, and the game's own message for it, so a field failure can be diagnosed from the log.
[[noreturn]] void managed_failure(void *m,void *ex){
 static std::string text;text="managed call failed: ";
 if(auto name=reinterpret_cast<const char*(*)(void*)>(dlsym(boot.handle,"il2cpp_method_get_name")))text+=name(m);
 if(auto format=reinterpret_cast<void(*)(void*,char*,int)>(dlsym(boot.handle,"il2cpp_format_exception"))){
  char detail[160]{};format(ex,detail,sizeof(detail)-1);text+=": ";
  for(const char *c=detail;*c;++c)text+=(*c=='"'||*c=='\\'||static_cast<unsigned char>(*c)<32)?' ':*c;
 }
 throw Failure{text.c_str()};
}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);if(ex)managed_failure(m,ex);return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(klass(o),name,n),o,args);}
template<class T>T unbox(void *o){require(o,"null result");T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");std::memcpy(&v,p,sizeof(v));return v;}
template<class T>T at(void *o,size_t off){T v{};std::memcpy(&v,static_cast<char*>(o)+off,sizeof(v));return v;}
void *h(unsigned id){return const_cast<void*>(md->opt_handles[id]);}
size_t field_offset(void *f){const size_t off=fn<size_t(*)(void*)>(exp("field_get_offset"))(f);require(off>=0x10&&off<0x1000,"field offset");return off;}
size_t off(void *k,const char *name){
 void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);
 require(!(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10),name);return field_offset(f);
}
// The instance field whose type name contains text; obfuscated field names change with each game update.
size_t typed(void *k,const char *text){
 void *iter=nullptr;
 while(void *f=fn<void*(*)(void*,void**)>(exp("class_get_fields"))(k,&iter)){
  if(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10)continue;
  if(typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f)).find(text)!=std::string::npos)return field_offset(f);
 }
 throw Failure{text};
}
Root type_object(void *k){return Root(fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k)));}
Root resolve(void *contract){
 Root type=type_object(h(H_IL_MAP));void *args[]={type.get()};Root map(invoke(h(H_IL_FIND_M),nullptr,args));if(!map)return {};
 Root container(invoke(h(H_IL_CONTAINER_M),map.get()));if(!container)return {};
 Root target=type_object(contract);void *resolve_args[]={target.get()};return Root(invoke(h(H_IL_RESOLVE_M),container.get(),resolve_args));
}

const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
void event(const std::string &json){auto tr=transport();if(!tr)return;PgrRuntimeTransportString s[]={{"smart-battle",12},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,s,2);}
void status(const char *phase,const std::string &message){event(std::string("{\"phase\":\"")+phase+"\",\"message\":\""+message+"\"}");}

// F2|smart|raidCounters|raidTeamSize|raidShowCard (slot 21). F1|enabled remains valid for old backups.
bool raid_counter_enabled=true,raid_counter_show_card=true;int raid_team_size=6;
bool configured(){
 auto tr=transport();PgrRuntimeTransportSample a{};char s[64]{};uint32_t n=0;
 if(!tr||tr->sample(&a)||!a.snapshot_fresh||a.fault||tr->get_string(21,s,sizeof(s)-1,&n))return false;
 s[n]=0;bool on=false;
 if(n>=4&&!std::memcmp(s,"F1|",3))on=s[3]=='1';
 else if(n>=4&&!std::memcmp(s,"F2|",3)){
  int smart=0,raid=1,card=1;
  if(std::sscanf(s,"F2|%d|%d|%d|%d",&smart,&raid,&raid_team_size,&card)!=4)return false;
  on=smart==1;raid_counter_enabled=raid==1;raid_counter_show_card=card!=0;
  if(raid_team_size<1||raid_team_size>6)raid_team_size=6;
 }else return false;
 if(!on)return false;
 int active=0,ready=0;tr->get_bool(3,&active);tr->get_bool(16,&ready);return active&&ready;
}
pgr_smart_party::HealingPolicy healing_policy(){
 auto tr=transport();PgrRuntimeTransportSample a{};char s[64]{};uint32_t n=0;
 if(!tr||tr->sample(&a)||!a.snapshot_fresh||a.fault||tr->get_string(18,s,sizeof(s)-1,&n)||n>=sizeof(s))return {};
 int active=0,ready=0;
 if(tr->get_bool(3,&active)||tr->get_bool(16,&ready)||!active||!ready)return {};
 s[n]=0;return pgr_smart_party::healing_policy(s);
}
bool gate(){
 const uint64_t tid=owner.load(std::memory_order_acquire);
 return enabled.load(std::memory_order_acquire)&&tid&&tid==uint64_t(gettid());
}

// ---- Game master tables ----------------------------------------------------------------------
struct Species {int type[2];int attack,defense,stamina;};
std::unordered_map<int,Move> moves;
std::unordered_map<uint64_t,Species> species;
uint64_t next_table_try=0;
bool tables_ready(){return moves.size()>100&&species.size()>500;}
uint64_t species_key(int id,int form){return (uint64_t(uint32_t(id))<<32)|uint32_t(form);}

// Dictionary<K,V> with reference V and a key of at most 8 bytes: { int hash; int next; K key; V value }.
// A wrapper type (the game's DefaultDictionary) may hold the real dictionary in a field.
void *field_named(void *k,const char *a,const char *b){
 void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,a);
 return f?f:fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,b);
}
template<class F>void each_entry(void *dict,F &&visit){
 void *k=klass(dict);
 if(!field_named(k,"_entries","entries")){dict=at<void*>(dict,typed(k,"Dictionary"));if(!dict)return;k=klass(dict);}
 void *entries_field=field_named(k,"_entries","entries"),*count_field=field_named(k,"_count","count");
 require(entries_field&&count_field,"dictionary fields");
 void *entries=at<void*>(dict,field_offset(entries_field));if(!entries)return;
 const int count=at<int32_t>(dict,field_offset(count_field));
 require(count>=0&&uint64_t(count)<=at<uint64_t>(entries,0x18),"dictionary size");
 for(int i=0;i<count;++i){
  const size_t e=0x20+size_t(i)*24;
  if(at<int32_t>(entries,e)<0||at<int32_t>(entries,e+4)<-1)continue;
  uint64_t key=at<uint64_t>(entries,e+8);void *value=at<void*>(entries,e+16);
  if(value)visit(key,value);
 }
}

// Move and species settings live either as raw templates (group -> id -> template) or in the typed
// store (message type -> key -> message), depending on the data; both are read.
int table_reports=0;
void load_tables(){
 Root gm=resolve(cls("","IGameMasterData"));require(bool(gm),"game master unavailable");
 void *tk=cls("Holoholo.Rpc","GameMasterClientTemplateProto");void *get_move=method(tk,"get_Move",0),*get_pokemon=method(tk,"get_Pokemon",0);
 void *mk=cls("Holoholo.Rpc","MoveSettingsProto"),*pk=cls("Holoholo.Rpc","PokemonSettingsProto"),*sk=cls("Holoholo.Rpc","PokemonStatsAttributesProto");
 const size_t m_id=off(mk,"UniqueId"),m_type=off(mk,"Type"),m_power=off(mk,"Power"),m_ms=off(mk,"DurationMs"),m_energy=off(mk,"EnergyDelta");
 const size_t p_id=off(pk,"uniqueId_"),p_t1=off(pk,"type1_"),p_t2=off(pk,"type2_"),p_stats=off(pk,"stats_"),p_form=off(pk,"form_");
 const size_t s_sta=off(sk,"BaseStamina"),s_atk=off(sk,"BaseAttack"),s_def=off(sk,"BaseDefense");
 std::unordered_map<int,Move> new_moves;std::unordered_map<uint64_t,Species> new_species;
 auto add_move=[&](void *ms){
  Move m;m.type=at<int32_t>(ms,m_type);m.power=at<float>(ms,m_power);m.seconds=at<int32_t>(ms,m_ms)/1000.0;m.energy=std::abs(at<int32_t>(ms,m_energy));m.known=m.seconds>0;
  new_moves[at<int32_t>(ms,m_id)]=m;
 };
 auto add_pokemon=[&](void *ps){
  void *stats=at<void*>(ps,p_stats);if(!stats)return;
  Species sp{{at<int32_t>(ps,p_t1),at<int32_t>(ps,p_t2)},at<int32_t>(stats,s_atk),at<int32_t>(stats,s_def),at<int32_t>(stats,s_sta)};
  const int id=at<int32_t>(ps,p_id);if(!id||sp.attack<=0)return;
  new_species[species_key(id,at<int32_t>(ps,p_form))]=sp;
  new_species.emplace(species_key(id,0),sp);  // Forms with no entry of their own fall back to this.
 };
 int templates=0,messages=0,stores=0;void *iter=nullptr;void *gk=klass(gm.get());
 while(void *f=fn<void*(*)(void*,void**)>(exp("class_get_fields"))(gk,&iter)){
  if(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10)continue;
  const std::string name=typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));
  if(name.find("Dictionary")==std::string::npos||name.find("Func")!=std::string::npos)continue;
  const bool raw=name.find("GameMasterClientTemplateProto")!=std::string::npos;
  const bool typed_store=name.find("System.Type")!=std::string::npos&&name.find("System.Object")!=std::string::npos&&name.find("IMessage")!=std::string::npos;
  if(!raw&&!typed_store)continue;
  void *store=at<void*>(gm.get(),field_offset(f));if(!store)continue;
  ++stores;
  each_entry(store,[&](uint64_t,void *group){
   each_entry(group,[&](uint64_t,void *item){
    if(raw){
     ++templates;
     if(void *ms=invoke(get_move,item))add_move(ms);else if(void *ps=invoke(get_pokemon,item))add_pokemon(ps);
    }else{
     ++messages;
     void *ik=klass(item);if(ik==mk)add_move(item);else if(ik==pk)add_pokemon(item);
    }
   });
  });
 }
 moves.swap(new_moves);species.swap(new_species);
 if(table_reports<4){++table_reports;event("{\"phase\":\"tables\",\"stores\":"+std::to_string(stores)+",\"templates\":"+std::to_string(templates)+",\"messages\":"+std::to_string(messages)+",\"moves\":"+std::to_string(moves.size())+",\"species\":"+std::to_string(species.size())+"}");}
 if(tables_ready())status("ready","Smart battling ready");
}

const Species *species_of(int id,int form){
 auto it=species.find(species_key(id,form));if(it==species.end())it=species.find(species_key(id,0));
 return it==species.end()?nullptr:&it->second;
}
Move move_of(int id){auto it=moves.find(id);return it==moves.end()?Move{}:it->second;}

struct ProtoFields {void *k=nullptr;size_t id=0,species=0,hp=0,max_hp=0,move1=0,move2=0,move3=0,iva=0,ivd=0,ivs=0,cpm=0,extra=0,egg=0,fort=0,station=0,display=0;};
ProtoFields pf;size_t display_form=0,display_align=0;
void load_proto_fields(){
 if(pf.k)return;
 ProtoFields f;f.k=cls("Holoholo.Rpc","PokemonProto");
 f.id=off(f.k,"id_");f.species=off(f.k,"pokemonId_");f.hp=off(f.k,"stamina_");f.move1=off(f.k,"move1_");f.move2=off(f.k,"move2_");f.move3=off(f.k,"move3_");
 f.max_hp=off(f.k,"maxStamina_");
 f.iva=off(f.k,"individualAttack_");f.ivd=off(f.k,"individualDefense_");f.ivs=off(f.k,"individualStamina_");f.cpm=off(f.k,"cpMultiplier_");f.extra=off(f.k,"additionalCpMultiplier_");
 f.egg=off(f.k,"isEgg_");f.fort=off(f.k,"deployedFortId_");f.station=off(f.k,"deployedStationId_");f.display=off(f.k,"pokemonDisplay_");
 void *d=cls("Holoholo.Rpc","PokemonDisplayProto");display_form=off(d,"Form");display_align=off(d,"Alignment");
 pf=f;
}

struct Stats {const Species *sp=nullptr;double attack=0,defense=0;};
Stats stats_of(void *p){
 Stats out;void *display=at<void*>(p,pf.display);
 const int form=display?at<int32_t>(display,display_form):0;
 out.sp=species_of(at<int32_t>(p,pf.species),form);if(!out.sp)return out;
 const double cpm=double(at<float>(p,pf.cpm))+double(at<float>(p,pf.extra));
 out.attack=(out.sp->attack+at<int32_t>(p,pf.iva))*cpm;out.defense=(out.sp->defense+at<int32_t>(p,pf.ivd))*cpm;
 if(display&&at<int32_t>(display,display_align)==1){out.attack*=1.2;out.defense/=1.2;}  // Shadow
 return out;
}
bool fighter_from(const PgrOwnedPokemon &o,Fighter &f){
 const Species *sp=species_of(o.species,o.form);if(!sp||sp->attack<=0)return false;
 f.type[0]=sp->type[0];f.type[1]=sp->type[1];
 f.attack=(sp->attack+o.atk)*o.cpm;f.defense=(sp->defense+o.def)*o.cpm;
 if(o.alignment==1){f.attack*=1.2;f.defense/=1.2;}  // Shadow
 f.hp=o.hp;f.fast=move_of(o.move1);f.charged[0]=move_of(o.move2);f.charged[1]=move_of(o.move3);
 return f.attack>0;
}
// hp<0 means use the HP stored on the Pokemon.
bool fighter_of(void *p,Fighter &f,int hp=-1){
 const Stats s=stats_of(p);if(!s.sp||s.attack<=0)return false;
 f.type[0]=s.sp->type[0];f.type[1]=s.sp->type[1];f.attack=s.attack;f.defense=s.defense;
 f.hp=hp>=0?hp:at<int32_t>(p,pf.hp);
 f.fast=move_of(at<int32_t>(p,pf.move1));f.charged[0]=move_of(at<int32_t>(p,pf.move2));f.charged[1]=move_of(at<int32_t>(p,pf.move3));
 return true;
}
bool foe_of(void *p,Foe &e){
 const Stats s=stats_of(p);if(!s.sp||s.attack<=0)return false;
 e.type[0]=s.sp->type[0];e.type[1]=s.sp->type[1];e.attack=s.attack;e.defense=s.defense;
 e.fast=move_of(at<int32_t>(p,pf.move1));e.charged=move_of(at<int32_t>(p,pf.move2));
 return true;
}

std::vector<void*> storage(Root &hold){
 Root bag=resolve(cls("Niantic.Holoholo","IPokemonBag"));require(bool(bag),"Pokemon storage unavailable");
 Root all(call(bag.get(),"get_AllPokemon"));require(bool(all),"Pokemon storage list unavailable");
 void *k=klass(all.get());
 const int n=unbox<int32_t>(invoke(method(k,"get_Count",0),all.get()));require(n>=0&&n<=30000,"Pokemon storage size");
 hold=Root(fn<void*(*)(void*,uintptr_t)>(exp("array_new"))(pf.k,uintptr_t(n)));require(bool(hold),"storage copy");
 int zero=0;void *args[]={hold.get(),&zero};invoke(method(k,"CopyTo",2),all.get(),args);
 std::vector<void*> out;out.reserve(size_t(n));
 for(int i=0;i<n;++i){void *p=at<void*>(hold.get(),0x20+size_t(i)*8);if(p&&klass(p)==pf.k)out.push_back(p);}
 return out;
}

std::unordered_map<uint64_t,void*> storage_index(Root &hold);
std::mutex counters_mutex;std::string counters_text;  // "key\nspecies,form,cp,shadow,nickname\n..." for Java
static std::string card_field(const std::string &value){std::string out;for(char c:value)out+=(c==','||c=='\n'||c=='\r')?' ':c;if(out.size()>40)out.resize(40);return out;}
void publish_counters(const std::string &key,const std::vector<std::vector<Foe>> &slots){
 std::vector<PgrOwnedPokemon> owned;if(!pgr_storage_snapshot(&owned))return;
 const auto healing=healing_policy();
 std::vector<const PgrOwnedPokemon*> who;std::vector<std::vector<double>> scores;
 for(const PgrOwnedPokemon &o:owned){
  if(o.egg||o.deployed||o.hp<=0)continue;
  Fighter f;if(!fighter_from(o,f))continue;
  f.hp=pgr_smart_party::selection_hp(o.hp,o.max_hp,healing);
  std::vector<double> s;bool any=false;for(const auto &g:slots){s.push_back(group_matchup(f,g));any=any||s.back()>0;}
  if(any){who.push_back(&o);scores.push_back(std::move(s));}
 }
 std::string text=key+"\n";
 const int card_size=key=="raid"&&raid_counter_enabled?raid_team_size:3;
 for(int i:pick_team(scores,int(slots.size()),card_size)){const PgrOwnedPokemon &o=*who[size_t(i)];text+=std::to_string(o.species)+","+std::to_string(o.form)+","+std::to_string(o.cp)+","+(o.alignment==1?"1":"0")+","+card_field(o.nickname)+"\n";}
 std::lock_guard<std::mutex> lock(counters_mutex);counters_text=text;
}
// ---- Before battle ---------------------------------------------------------------------------
// The party to use instead of original, or null to keep it. filter is the game's own eligibility check.
void *counter_party(const std::vector<std::vector<Foe>> &foes,void *original,void *filter,bool rocket=false,int size_override=0){
 if(foes.empty()||!original)return nullptr;
 void *party_class=klass(original);const size_t team_at=off(party_class,"TeamProtos");
 void *old_team=at<void*>(original,team_at);
 const uint64_t old_length=old_team?at<uint64_t>(old_team,0x18):0;if(old_length>6)return nullptr;
 const int old_size=int(old_length),size=size_override>0?size_override:pgr_smart_party::team_size(old_size,rocket);if(!size)return nullptr;
  // Shared engine scores the plain storage snapshot; game objects are looked up only for candidates.
  std::vector<PgrOwnedPokemon> owned;if(!pgr_storage_snapshot(&owned))return nullptr;
  const auto healing=healing_policy();
  std::vector<pgr_counter_engine::Entry> entries;
  for(const PgrOwnedPokemon &o:owned){
   if(o.egg||o.deployed||o.hp<=0)continue;
   pgr_counter_engine::Entry e;e.id=o.id;e.hp=o.hp;e.max_hp=o.max_hp;
   if(!fighter_from(o,e.fighter))continue;
   entries.push_back(std::move(e));
  }
  pgr_counter_engine::Options opts;opts.team_size=size;opts.healing=healing;
  const auto ranked=pgr_counter_engine::rank(entries,foes,opts);
  struct Rated {void *p;uint64_t id;std::vector<double> scores;double best;};
  std::vector<int> order;order.reserve(entries.size());
  for(size_t i=0;i<entries.size();++i){
   double best=0;for(const auto &row:ranked.scores[i])best=std::max(best,row);
   if(best>0)order.push_back(int(i));
  }
  std::sort(order.begin(),order.end(),[&](int a,int b){
   double x=0,y=0;for(double v:ranked.scores[a])x=std::max(x,v);for(double v:ranked.scores[b])y=std::max(y,v);return x>y;});
  Root hold;const auto bag=storage_index(hold);
  std::vector<Rated> eligible;void *invoke_method=filter?method(klass(filter),"Invoke",1):nullptr;
  for(int i:order){
   if(int(eligible.size())>=kMaxCandidates)break;
   auto found=bag.find(entries[i].id);if(found==bag.end())continue;
   void *obj=found->second;
   if(filter){void *args[]={obj};if(!unbox<bool>(invoke(invoke_method,filter,args)))continue;}
   double best=0;for(double v:ranked.scores[i])best=std::max(best,v);
   eligible.push_back(Rated{obj,entries[i].id,ranked.scores[i],best});
  }
  std::vector<std::vector<double>> scores;for(const Rated &r:eligible)scores.push_back(r.scores);
  const std::vector<int> picks=pick_team(scores,int(foes.size()),size);
if(picks.empty())return nullptr;
 Root array(fn<void*(*)(void*,uintptr_t)>(exp("array_new"))(pf.k,uintptr_t(size)));require(bool(array),"team array");
 std::vector<void*> team;for(int i:picks)team.push_back(eligible[size_t(i)].p);
 // Slots we could not fill keep the game's own choices.
 for(int i=0;i<old_size&&int(team.size())<size;++i){
  void *p=at<void*>(old_team,0x20+size_t(i)*8);if(!p)continue;
  const uint64_t id=at<uint64_t>(p,pf.id);bool dup=false;for(void *t:team)dup=dup||at<uint64_t>(t,pf.id)==id;
  if(!dup)team.push_back(p);
 }
 auto *slots=reinterpret_cast<void**>(static_cast<char*>(array.get())+0x20);
 for(size_t i=0;i<team.size();++i)slots[i]=team[i];
 Root out(fn<void*(*)(void*)>(exp("object_new"))(party_class));require(bool(out),"new party");
 int number=at<int32_t>(original,off(party_class,"<TeamNumber>k__BackingField"));
 void *args[]={&number,at<void*>(original,off(party_class,"<TeamName>k__BackingField")),array.get(),at<void*>(original,off(party_class,"LeagueId"))};
 invoke(method(party_class,".ctor",4),out.get(),args);
 event("{\"phase\":\"team\",\"foes\":"+std::to_string(foes.size())+",\"size\":"+std::to_string(picks.size())+",\"candidates\":"+std::to_string(eligible.size())+"}");
 return out.get();
}

using Construct=void*(*)(void*,void*,int,void*,float,void*,const void*);
std::atomic<Construct> construct_original{nullptr};
void *hook_construct(void *self,void *enemies,int number,void *conditions,float multiplier,void *filter,const void *m){
 void *result=construct_original.load(std::memory_order_acquire)(self,enemies,number,conditions,multiplier,filter,m);
 event("{\"phase\":\"construct-call\",\"gate\":"+std::string(gate()?"true":"false")+",\"tables\":"+(tables_ready()?"true":"false")+"}");
 if(!result||!enemies||!gate()||!tables_ready())return result;
 try{
  void *enumerable=cls("System.Collections","IEnumerable"),*enumerator=cls("System.Collections","IEnumerator");
  void *virtual_of=exp("object_get_virtual_method");
  auto dispatch=[&](void *o,void *iface_method){void *v=fn<void*(*)(void*,void*)>(virtual_of)(o,iface_method);require(v,"interface dispatch");return v;};
  Root it(invoke(dispatch(enemies,method(enumerable,"GetEnumerator",0)),enemies));require(bool(it),"enemy list");
  void *move_next=dispatch(it.get(),method(enumerator,"MoveNext",0)),*current=dispatch(it.get(),method(enumerator,"get_Current",0));
  std::vector<Foe> foes;
  // A struct enumerator (List<T>'s) comes back boxed, but its methods take a pointer to the struct itself.
  void *receiver=it.get();
  if(fn<bool(*)(void*)>(exp("class_is_valuetype"))(klass(receiver)))receiver=fn<void*(*)(void*)>(boot.runtime.object_unbox)(receiver);
  while(foes.size()<12&&unbox<bool>(invoke(move_next,receiver))){void *p=invoke(current,receiver);Foe e;if(p&&klass(p)==pf.k&&foe_of(p,e))foes.push_back(e);}
  std::vector<std::vector<Foe>> groups;for(const Foe &e:foes)groups.push_back({e});
  const bool raid=groups.size()==1;
  const int override=raid&&raid_counter_enabled?raid_team_size:0;
  if(raid&&raid_counter_show_card)publish_counters("raid",groups);
  if(void *party=counter_party(groups,result,filter,false,override))return party;
 }catch(const Failure &f){status("error",std::string("Team pick skipped: ")+f.where);}
 catch(...){status("error","Team pick skipped: native failure");}
 return result;
}

// InvasionCharacter value -> the type its managed name announces (CharacterFireGruntMale).
std::unordered_map<int,int> grunt_types;
std::unordered_map<int,std::string> grunt_keys;  // InvasionCharacter value -> line-up key (smart_battle_party.h)
// Current line-ups from Java (RocketLineups): key -> three slots of candidates {type1,type2,dex}.
struct LineupMon {int type[2];int dex;};
std::mutex lineup_mutex;std::unordered_map<std::string,std::vector<std::vector<LineupMon>>> lineups;
void load_grunt_types(void *hook_method){
 void *type=const_cast<void*>(fn<const void*(*)(void*,uint32_t)>(exp("method_get_param"))(hook_method,0));require(type,"grunt parameter");
 void *k=fn<void*(*)(void*)>(exp("class_from_type"))(type);require(k,"grunt enum");
 void *iter=nullptr;
 while(void *f=fn<void*(*)(void*,void**)>(exp("class_get_fields"))(k,&iter)){
  if((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)!=0x8050)continue;
  const char *name=fn<const char*(*)(void*)>(exp("field_get_name"))(f);if(!name)continue;
  int value=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&value);
  const int type_id=pgr_smart_party::grunt_type(name);if(type_id)grunt_types[value]=type_id;
  grunt_keys[value]=pgr_smart_party::lineup_key(name);
 }
 require(!grunt_types.empty(),"No typed Rocket grunts recognized");
 event("{\"phase\":\"grunt-types\",\"count\":"+std::to_string(grunt_types.size())+"}");
}
using FindGrunt=void*(*)(void*,int,const void*);
std::atomic<FindGrunt> grunt_original{nullptr};
void *hook_grunt(void *self,int character,const void *m){
 void *result=grunt_original.load(std::memory_order_acquire)(self,character,m);
 event("{\"phase\":\"grunt-call\",\"character\":"+std::to_string(character)+",\"gate\":"+(gate()?"true":"false")+",\"tables\":"+(tables_ready()?"true":"false")+"}");
 if(!gate()||!tables_ready())return result;
 try{
  auto it=grunt_types.find(character);
  const auto key=grunt_keys.find(character);
  // The known line-up first: counters against every candidate of each slot.
  std::vector<std::vector<Foe>> slots;
  if(key!=grunt_keys.end()){
   std::lock_guard<std::mutex> lock(lineup_mutex);
   auto found=lineups.find(key->second);
   if(found!=lineups.end())for(const auto &slot:found->second){
    std::vector<Foe> group;
    for(const LineupMon &m:slot){
     Foe e;e.type[0]=m.type[0];e.type[1]=m.type[1];
     if(const Species *sp=m.dex?species_of(m.dex,0):nullptr){e.attack=(sp->attack+15)*0.7;e.defense=(sp->defense+15)*0.7;}  // as vnext_foe
     group.push_back(e);
    }
    if(!group.empty())slots.push_back(std::move(group));
   }
  }
  event("{\"phase\":\"grunt-party\",\"character\":"+std::to_string(character)+",\"key\":\""+(key==grunt_keys.end()?std::string():key->second)+"\",\"lineup\":"+std::to_string(slots.size())+",\"type\":"+std::to_string(it==grunt_types.end()?0:it->second)+",\"saved\":"+(result?"true":"false")+"}");
  if(slots.empty()&&it!=grunt_types.end()){Foe e;e.type[0]=it->second;slots.push_back({e});}
  if(!slots.empty())publish_counters(key==grunt_keys.end()?std::string():key->second,slots);
  if(!result||slots.empty())return result;
  if(void *party=counter_party(slots,result,nullptr,true))return party;
 }catch(const Failure &f){status("error",std::string("Rocket team pick skipped: ")+f.where);}
 catch(...){status("error","Rocket team pick skipped: native failure");}
 return result;
}

// ---- In battle -------------------------------------------------------------------------------
using Start=void(*)(void*,const void*);
std::atomic<Start> gym_start_original{nullptr},vnext_start_original{nullptr};
Root gym_ui,vnext_ui;
void hook_gym_start(void *self,const void *m){
 gym_start_original.load(std::memory_order_acquire)(self,m);
 if(self&&owner.load(std::memory_order_acquire)==uint64_t(gettid())){try{gym_ui=Root(self);}catch(...){}}
}
void hook_vnext_start(void *self,const void *m){
 vnext_start_original.load(std::memory_order_acquire)(self,m);
 if(self&&owner.load(std::memory_order_acquire)==uint64_t(gettid())){try{vnext_ui=Root(self);}catch(...){}}
}

struct Watch {uint64_t foe=0,active=0,due=0;bool seen=false;};
Watch gym_watch,rocket_watch;
// A press in progress: the menu has been opened and the target's button comes next.
struct Press {bool on=false,vnext=false;int presses=0;uint64_t target=0,since=0,next=0;}press;
uint64_t next_step=0;

void note(Watch &w,uint64_t foe,uint64_t active,uint64_t now){
 if(w.seen&&w.foe==foe&&w.active==active)return;
 w.seen=true;w.foe=foe;w.active=active;w.due=now+kSettleMs;
}
void begin_press(void *ui,bool vnext,uint64_t target,uint64_t now){
 if(unbox<bool>(call(ui,"get_IsOpen")))return;  // The player is using the menu: leave it alone.
 call(ui,"Open");press=Press{};press.on=true;press.vnext=vnext;press.target=target;press.since=now;press.next=now+kOpenMs;
}
void end_press(void *ui){if(ui&&unbox<bool>(call(ui,"get_IsOpen")))call(ui,"Dismiss");press=Press{};}

// The Pokemon currently out, read fresh from the battle; 0 if it cannot be read right now.
uint64_t gym_active_id(void *ui){
 void *service=at<void*>(ui,typed(klass(ui),"IBattleFrameObservableService"));if(!service)return 0;
 void *frame=at<void*>(service,typed(klass(service),"Niantic.Holoholo.Battle.BattleFrame"));if(!frame)return 0;
 void *player=at<void*>(frame,off(klass(frame),"LocalPlayer"));if(!player)return 0;
 void *active=call(player,"get_ActivePokemon");if(!active)return 0;
 void *p=call(active,"get_Pokemon");return p?at<uint64_t>(p,pf.id):0;
}
uint64_t vnext_active_id(void *ui);

// The game's Swap quietly does nothing while the active Pokemon is busy (an attack or dodge still
// playing), and leaves the menu open. Attacks are paused while the menu is open, so keep pressing
// until the Pokemon actually changes or the time runs out.
void continue_press(uint64_t now){
 if(now<press.next)return;
 void *ui=(press.vnext?vnext_ui:gym_ui).get();
 if(!ui){press=Press{};return;}
 const char *mode=press.vnext?"rocket":"gym";
 if((press.vnext?vnext_active_id(ui):gym_active_id(ui))==press.target){
  event(std::string("{\"phase\":\"swapped\",\"mode\":\"")+mode+"\",\"presses\":"+std::to_string(press.presses)+"}");
  press=Press{};return;
 }
 const bool timed_out=now-press.since>kPressGiveUpMs;
 if(!unbox<bool>(call(ui,"get_IsOpen"))){
  if(timed_out){if(press.presses)event(std::string("{\"phase\":\"swap-refused\",\"mode\":\"")+mode+"\",\"presses\":"+std::to_string(press.presses)+"}");press=Press{};}
  else press.next=now+kOpenMs;
  return;
 }
 if(timed_out){
  event(std::string("{\"phase\":\"swap-refused\",\"mode\":\"")+mode+"\",\"presses\":"+std::to_string(press.presses)+"}");
  end_press(ui);return;
 }
 press.next=now+kPressRetryMs;
 void *list=at<void*>(ui,typed(klass(ui),press.vnext?"SwapButtonVNext>":".SwapButton>"));
 if(!list)return;
 const int count=unbox<int>(call(list,"get_Count"));
 void *get_item=method(klass(list),"get_Item",1);
 void *selectable=cls("UnityEngine.UI","Selectable");
 for(int i=0;i<count&&i<16;++i){
  void *args[]={&i};void *button=invoke(get_item,list,args);if(!button)continue;
  void *bk=klass(button);uint64_t id=0;
  if(press.vnext)id=at<uint64_t>(button,typed(bk,"System.UInt64"));
  else{void *p=at<void*>(button,typed(bk,"Holoholo.Rpc.PokemonProto"));id=p?at<uint64_t>(p,pf.id):0;}
  if(id!=press.target)continue;
  void *ui_button=at<void*>(button,typed(bk,"UnityEngine.UI.Button"));
  if(ui_button&&unbox<bool>(invoke(method(selectable,"IsInteractable",0),ui_button))){call(button,"Swap");++press.presses;}
  return;
 }
}

void step_gym(uint64_t now){
 void *ui=gym_ui.get();if(!ui)return;
 void *service=at<void*>(ui,typed(klass(ui),"IBattleFrameObservableService"));if(!service)return;
 void *frame=at<void*>(service,typed(klass(service),"Niantic.Holoholo.Battle.BattleFrame"));if(!frame)return;
 void *fk=klass(frame);
 void *player=at<void*>(frame,off(fk,"LocalPlayer")),*enemy_frame=at<void*>(frame,off(fk,"EnemyPokemon"));
 if(!player||!enemy_frame)return;
 void *pk=klass(player);void *team=at<void*>(player,off(pk,"Pokemon"));if(!team)return;
 const int active=at<int32_t>(player,off(pk,"ActivePokemonIndex"));
 void *enemy=call(enemy_frame,"get_Pokemon");if(!enemy)return;
 Foe foe;if(!foe_of(enemy,foe))return;
 const int count=unbox<int>(call(team,"get_Count"));if(count<=0||count>6)return;
 void *get_item=method(klass(team),"get_Item",1);
 std::vector<double> scores;std::vector<uint64_t> ids;uint64_t active_id=0;
 for(int i=0;i<count;++i){
  void *args[]={&i};void *pokemon_frame=invoke(get_item,team,args);double s=0;uint64_t id=0;
  if(pokemon_frame){
   void *p=call(pokemon_frame,"get_Pokemon");
   if(p){
    id=at<uint64_t>(p,pf.id);
    const int health=unbox<int32_t>(call(pokemon_frame,"get_Health"));
    Fighter f;if(health>0&&!unbox<bool>(call(pokemon_frame,"get_IsFainted"))&&fighter_of(p,f,health))s=matchup(f,foe);
   }
  }
  scores.push_back(s);ids.push_back(id);if(i==active)active_id=id;
 }
 note(gym_watch,at<uint64_t>(enemy,pf.id),active_id,now);
 if(!gym_watch.due||now<gym_watch.due)return;
 if(unbox<bool>(call(player,"get_IsSwapping")))return;
 gym_watch.due=0;
 const int target=choose_swap(scores,active);
 if(target>=0&&ids[size_t(target)])begin_press(ui,false,ids[size_t(target)],now);
}

// The BattleState reachable from a VNext swap control, through its battle director.
void *battle_state_of(void *control){
 void *director=at<void*>(control,typed(klass(control),"IBattleDirector"));if(!director)return nullptr;
 return call(director,"get_CurrentState");
}
void *dict_first(void *dict){void *found=nullptr;each_entry(dict,[&](uint64_t,void *v){if(!found)found=v;});return found;}
void *state_for(void *dict,uint64_t id){void *found=nullptr;each_entry(dict,[&](uint64_t key,void *v){if(key==id)found=v;});return found;}
uint64_t active_pokemon_of(void *actor){
 void *proto=at<void*>(actor,off(klass(actor),"<ActorProto>k__BackingField"));if(!proto)return 0;
 return at<uint64_t>(proto,off(klass(proto),"ActivePokemonId"));
}
uint64_t vnext_active_id(void *ui){
 void *state=battle_state_of(ui);if(!state)return 0;
 void *actor=call(state,"get_LocalActor");return actor?active_pokemon_of(actor):0;
}
// The opponent's types, from its battle record. Line-up moves are not exposed, so moves stay unknown.
bool vnext_foe(void *state,Foe &e,uint64_t &foe_id){
 void *sk=klass(state);
 void *opponents=at<void*>(state,off(sk,"ActiveOpponents"));if(!opponents)return false;
 void *actor=dict_first(opponents);if(!actor)return false;
 foe_id=active_pokemon_of(actor);if(!foe_id)return false;
 void *list=at<void*>(state,off(sk,"PokemonList"));if(!list)return false;
 void *foe_state=state_for(list,foe_id);if(!foe_state)return false;
 void *proto=at<void*>(foe_state,off(klass(foe_state),"<Pokemon>k__BackingField"));if(!proto)return false;
 void *pk=klass(proto);void *display=at<void*>(proto,off(pk,"Display"));
 const Species *sp=species_of(at<int32_t>(proto,off(pk,"PokedexId")),display?at<int32_t>(display,display_form):0);if(!sp)return false;
 e.type[0]=sp->type[0];e.type[1]=sp->type[1];e.attack=(sp->attack+15)*0.7;e.defense=(sp->defense+15)*0.7;
 return true;
}
std::unordered_map<uint64_t,void*> storage_index(Root &hold){
 std::unordered_map<uint64_t,void*> out;for(void *p:storage(hold))out[at<uint64_t>(p,pf.id)]=p;return out;
}
double vnext_score(const std::unordered_map<uint64_t,void*> &bag,void *pokemon_state,uint64_t id,const Foe &foe){
 auto it=bag.find(id);if(it==bag.end()||!pokemon_state)return 0;
 const int health=at<int32_t>(pokemon_state,off(klass(pokemon_state),"CurrentHealth"));
 Fighter f;return health>0&&fighter_of(it->second,f,health)?matchup(f,foe):0;
}

void step_rocket(uint64_t now){
 void *ui=vnext_ui.get();if(!ui)return;
 void *state=battle_state_of(ui);if(!state)return;
 void *sk=klass(state);if(!at<bool>(state,off(sk,"HasReceivedStateForBattle")))return;
 Foe foe;uint64_t foe_id=0;if(!vnext_foe(state,foe,foe_id))return;
 void *actor=call(state,"get_LocalActor");if(!actor)return;
 const uint64_t active_id=active_pokemon_of(actor);if(!active_id)return;
 note(rocket_watch,foe_id,active_id,now);
 if(!rocket_watch.due||now<rocket_watch.due)return;
 rocket_watch.due=0;
 void *roster=at<void*>(actor,off(klass(actor),"Roster"));if(!roster)return;
 Root hold;const auto bag=storage_index(hold);
 std::vector<double> scores;std::vector<uint64_t> ids;int active=-1;
 each_entry(roster,[&](uint64_t id,void *pokemon_state){if(id==active_id)active=int(ids.size());scores.push_back(vnext_score(bag,pokemon_state,id,foe));ids.push_back(id);});
 const int target=choose_swap(scores,active);
 if(target>=0)begin_press(ui,true,ids[size_t(target)],now);
}

bool gym_battle(){
 auto api=static_cast<const PgrCombatHostApi*>(pgrazor_combat_host_api(1));if(!api||!api->sample)return false;
 PgrCombatHostSample s{};s.version=1;s.size=sizeof(s);
 return api->sample(&s)==0&&s.battle_scope_active==1&&s.battle_mode==1;
}
bool rocket_battle(){
 auto api=static_cast<const pgr_rocket_combat_api_v1*>(pgrazor_rocket_combat_api(1));if(!api||!api->sample)return false;
 pgr_rocket_combat_sample_v1 s{};s.version=PGR_RC_API_VERSION;s.size=sizeof(s);
 return api->sample(&s)==0&&s.scope_active==1;
}

void battle_step(){
 const uint64_t now=clock_ms();if(now<next_step)return;next_step=now+kStepMs;
 if(press.on){continue_press(now);return;}
 if(gym_battle())step_gym(now);else gym_watch=Watch{};
 if(rocket_battle())step_rocket(now);else rocket_watch=Watch{};
}

// ---- Install ---------------------------------------------------------------------------------
template<class F>void install_hook(void *k,const char *name,int params,const char *ret,uint64_t rva,const uint8_t *prologue,F hook,std::atomic<F> &slot,void **method_out=nullptr){
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api&&api->validate_method,"metadata API");
 PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=k;q.name=name;q.return_type=ret;q.return_match=PGR_MD_MATCH_EXACT;q.param_count=params;
 q.expected_code=static_cast<const char*>(boot.module_base)+rva;q.prologue=prologue;q.prologue_len=24;
 PgrMetaMethodResult r{};require(api->validate_method(&q,&r)==0&&r.code,name);
 if(method_out)*method_out=method(k,name,params);
 void *o=nullptr;require(pgrazor_hook(const_cast<void*>(r.code),reinterpret_cast<void*>(hook),&o)==0&&o,name);
 slot.store(reinterpret_cast<F>(o),std::memory_order_release);
}
void install(){
 static const uint8_t construct[]=PGR_GAME_SMART_BATTLE_CPP_CONSTRUCT_PARTY_FINGERPRINT,grunt[]=PGR_GAME_SMART_BATTLE_CPP_FIND_GRUNT_PARTY_FINGERPRINT;
 static const uint8_t gym[]=PGR_GAME_SMART_BATTLE_CPP_SWAP_UI_START_FINGERPRINT,vnext[]=PGR_GAME_SMART_BATTLE_CPP_SWAP_UI_VNEXT_START_FINGERPRINT;
 static_assert(sizeof(construct)==24&&sizeof(grunt)==24&&sizeof(gym)==24&&sizeof(vnext)==24,"24-byte prologues");
 require(boot.module_base,"il2cpp module");
 load_proto_fields();
 void *service=cls("Niantic.Holoholo.Battle","BattlePartyService");
 void *find_grunt=nullptr;
 install_hook<Construct>(service,"ConstructPartyAgainst",5,"Niantic.Holoholo.Battle.BattleParty",PGR_GAME_SMART_BATTLE_CPP_CONSTRUCT_PARTY_RVA,construct,hook_construct,construct_original);
 install_hook<FindGrunt>(service,"FindGruntParty",1,"Niantic.Holoholo.Battle.BattleParty",PGR_GAME_SMART_BATTLE_CPP_FIND_GRUNT_PARTY_RVA,grunt,hook_grunt,grunt_original,&find_grunt);
 load_grunt_types(find_grunt);
 install_hook<Start>(cls("Niantic.Holoholo.Battle","SwapUI"),"Start",0,"System.Void",PGR_GAME_SMART_BATTLE_CPP_SWAP_UI_START_RVA,gym,hook_gym_start,gym_start_original);
 install_hook<Start>(cls("Niantic.Holoholo.VNext","SwapUIVNext"),"Start",0,"System.Void",PGR_GAME_SMART_BATTLE_CPP_SWAP_UI_VNEXT_START_RVA,vnext,hook_vnext_start,vnext_start_original);
 hooks_ready.store(true,std::memory_order_release);
}
}  // namespace

bool pgr_smart_battle_enabled(){return enabled.load(std::memory_order_acquire);}

std::string pgr_smart_battle_counters(){std::lock_guard<std::mutex> lock(counters_mutex);std::string out;out.swap(counters_text);return out;}

void pgr_smart_battle_lineups(const char *text){
 std::unordered_map<std::string,std::vector<std::vector<LineupMon>>> next;
 const std::string all=text?text:"";
 size_t start=0;
 while(start<all.size()){
  size_t end=all.find('\n',start);if(end==std::string::npos)end=all.size();
  const std::string line=all.substr(start,end-start);start=end+1;
  std::vector<std::string> parts;size_t p=0;
  while(true){size_t bar=line.find('|',p);parts.push_back(line.substr(p,bar==std::string::npos?std::string::npos:bar-p));if(bar==std::string::npos)break;p=bar+1;}
  if(parts.size()!=4||parts[0].empty())continue;
  std::vector<std::vector<LineupMon>> slots;
  for(size_t s=1;s<4;++s){
   std::vector<LineupMon> slot;size_t q=0;const std::string &list=parts[s];
   while(q<list.size()){
    size_t semi=list.find(';',q);if(semi==std::string::npos)semi=list.size();
    LineupMon m{};if(std::sscanf(list.substr(q,semi-q).c_str(),"%d,%d,%d",&m.type[0],&m.type[1],&m.dex)==3&&m.type[0]>=1&&m.type[0]<=18)slot.push_back(m);
    q=semi+1;
   }
   slots.push_back(std::move(slot));
  }
  next[parts[0]]=std::move(slots);
 }
 std::lock_guard<std::mutex> lock(lineup_mutex);lineups.swap(next);
}

int pgr_smart_battle_pick_prompt(void *const *buttons,int count){
 if(!gate()||!tables_ready()||!pf.k||count<=0||!buttons[0])return -1;
 try{
  void *state=battle_state_of(buttons[0]);if(!state)return -1;
  Foe foe;uint64_t foe_id=0;if(!vnext_foe(state,foe,foe_id))return -1;
  void *list=at<void*>(state,off(klass(state),"PokemonList"));if(!list)return -1;
  Root hold;const auto bag=storage_index(hold);
  std::vector<double> scores;
  for(int i=0;i<count;++i){
   const uint64_t id=at<uint64_t>(buttons[i],typed(klass(buttons[i]),"System.UInt64"));
   scores.push_back(vnext_score(bag,state_for(list,id),id,foe));
  }
  return choose_swap(scores,-1);
 }catch(const Failure &f){status("error",std::string("Counter pick skipped: ")+f.where);}
 catch(...){status("error","Counter pick skipped: native failure");}
 return -1;
}

void pgr_smart_battle_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||!tid||tid!=uint64_t(gettid()))return;
 const uint64_t prior=owner.load(std::memory_order_acquire);if(prior&&prior!=tid)return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 owner.store(tid,std::memory_order_release);md=metadata;
 const bool on=configured();enabled.store(on,std::memory_order_release);
 if(!on){press=Press{};gym_watch=Watch{};rocket_watch=Watch{};return;}
 try{
  if(!initialized){auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(api&&api->snapshot(&boot)==0,"Runtime unavailable");initialized=true;}
  if(!hooks_tried){hooks_tried=true;install();}
  if(!hooks_ready.load(std::memory_order_acquire))return;
  if(!tables_ready()){const uint64_t now=clock_ms();if(now>=next_table_try){next_table_try=now+kTableRetryMs;load_tables();}return;}
  battle_step();
 }catch(const Failure &f){press=Press{};status("error",std::string("Smart battling paused: ")+f.where);}
 catch(...){press=Press{};status("error","Smart battling paused: native failure");}
}
void pgr_smart_battle_stop(uint64_t){enabled.store(false,std::memory_order_release);press=Press{};}

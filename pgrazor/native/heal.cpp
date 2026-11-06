#include "heal.h"
#include "game_bindings.h"
#include "shared_metadata.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include "vgp_results.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <set>
#include <string>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Item choice. Every candidate mix of potions (and, for a fainted Pokemon, a Revive or Max Revive
// first) is scored, and the cheapest wins:
//   score = sum of item values + 10 per item used + 10 per HP left missing
// Item values rank how much each item is worth keeping: Potion 20, Super Potion 50, Hyper Potion 200
// (their HP), Max Potion 400, Revive 300, Max Revive 1000. So a Pokemon missing 5 HP gets a Potion,
// not a Max Potion; a Max Potion is used only when it beats the potions on hand (and is never spent
// on less than ~40 missing HP); a Revive plus potions beats a Max Revive whenever potions are on hand.
// Heal amounts are the game's long-standing ones: 20 / 50 / 200 HP and full; Revive to half HP.
namespace pgr_heal {
namespace {
constexpr long kValue[KIND_COUNT]={20,50,200,400,300,1000};
constexpr long kPerItem=10,kPerMissingHp=10;
constexpr int kHeal[3]={20,50,200};
long item_cost(int kind,int n){return n*(kValue[kind]+kPerItem);}
Plan potions(int missing,const Stock &s){
 Plan best;best.cost=kPerMissingHp*long(missing);  // use nothing
 long best_over=0;int best_items=0;
 // Equal scores: the one that overheals least (wastes the least), then the one with fewer items.
 auto consider=[&](const Plan &p,long over){
  int items=0;for(int c:p.count)items+=c;
  if(p.cost<best.cost||(p.cost==best.cost&&(over<best_over||(over==best_over&&items<best_items)))){best=p;best_over=over;best_items=items;}
 };
 if(s.count[MAX_POTION]>0){Plan p;p.count[MAX_POTION]=1;p.healed=missing;p.cost=item_cost(MAX_POTION,1);consider(p,0);}
 const int hmax=std::min(s.count[HYPER_POTION],(missing+199)/200),smax=std::min(s.count[SUPER_POTION],(missing+49)/50),pmax=std::min(s.count[POTION],(missing+19)/20);
 for(int h=0;h<=hmax;++h)for(int su=0;su<=smax;++su)for(int po=0;po<=pmax;++po){
  Plan p;p.count[HYPER_POTION]=h;p.count[SUPER_POTION]=su;p.count[POTION]=po;
  const int heal=h*kHeal[2]+su*kHeal[1]+po*kHeal[0];
  p.healed=heal<missing?heal:missing;
  p.cost=item_cost(HYPER_POTION,h)+item_cost(SUPER_POTION,su)+item_cost(POTION,po)+kPerMissingHp*long(missing-p.healed);
  consider(p,heal>missing?heal-missing:0);
 }
 return best;
}
}  // namespace
Plan plan(int hp,int max_hp,const Stock &s,bool revive){
 if(max_hp<=0||hp<0||hp>=max_hp)return {};
 if(hp>0){Plan p=potions(max_hp-hp,s);return p.any()?p:Plan{};}
 if(!revive)return {};
 Plan best;best.cost=kPerMissingHp*long(max_hp);  // leave it fainted
 if(s.count[MAX_REVIVE]>0){Plan p;p.count[MAX_REVIVE]=1;p.healed=max_hp;p.cost=item_cost(MAX_REVIVE,1);if(p.cost<best.cost)best=p;}
 if(s.count[REVIVE]>0){
  const int after=max_hp/2>0?max_hp/2:1;
  Plan p=potions(max_hp-after,s);p.count[REVIVE]=1;p.healed+=after;p.cost+=item_cost(REVIVE,1);
  if(p.cost<best.cost)best=p;
 }
 return best.any()?best:Plan{};
}
int first(const Plan &p){
 static const int order[]={REVIVE,MAX_REVIVE,MAX_POTION,HYPER_POTION,SUPER_POTION,POTION};
 for(int k:order)if(p.count[k]>0)return k;
 return -1;
}
}  // namespace pgr_heal

// ---------------------------------------------------------------------------------------------
// Runtime. Every ~4 s on the map the Pokemon storage is read (IPokemonBag.get_AllPokemon):
//  * After battle: a Pokemon whose HP dropped since the last read has just fought (battles only
//    write HP back to storage when they end) and is queued if it is below the threshold.
//  * Periodically: every 2 minutes every stored Pokemon below the threshold is queued.
// Queued Pokemon are healed one item at a time through the game's own IItemBag.UseItemOnPokemon (the
// call the potion picker makes), re-planned from fresh HP and item counts before every item, until
// full or nothing worth using is left. Only while the game is active, the account ready and the map
// showing, so never during a battle or an encounter. Eggs and gym or station defenders are skipped.
namespace {
using namespace pgr_shared_meta;
using namespace pgr_heal;
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
PgrBootBinding boot{};const PgrSharedMetadataOutput *md=nullptr;
bool initialized=false,busy=false,faulted=false;uint64_t owner=0;
uint64_t clock_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
template<class F>F fn(const void *p){require(p,"missing runtime export");return reinterpret_cast<F>(const_cast<void*>(p));}
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
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api,"metadata API");
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult out{};require(api->resolve_class(&q,&out)==0,name);return const_cast<void*>(out.klass);
}
std::string typename_of(void *type){char *p=fn<char*(*)(void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
void *method(void *k,const char *name,int count){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,count);require(m,name);return m;}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(klass(o),name,n),o,args);}
template<class T>T unbox(void *o){require(o,"null result");T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");memcpy(&v,p,sizeof(v));return v;}
void *h(unsigned id){return const_cast<void*>(md->opt_handles[id]);}
Root type_object(void *k){return Root(fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k)));}
Root resolve(void *contract){
 Root type=type_object(h(H_IL_MAP));void *args[]={type.get()};Root map(invoke(h(H_IL_FIND_M),nullptr,args));if(!map)return {};
 Root container(invoke(h(H_IL_CONTAINER_M),map.get()));if(!container)return {};
 Root target=type_object(contract);void *resolve_args[]={target.get()};return Root(invoke(h(H_IL_RESOLVE_M),container.get(),resolve_args));
}
int literal(void *k,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);require((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)==0x8050,name);int n=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&n);return n;}
size_t offset_of(void *k,const char *name,const char *type){
 void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);
 require(typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))==type,name);
 require(!(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10),name);
 const size_t at=fn<size_t(*)(void*)>(exp("field_get_offset"))(f);require(at>=0x10&&at<0x1000,name);return at;
}
template<class T>T at(void *o,size_t off){T v{};memcpy(&v,static_cast<char*>(o)+off,sizeof(v));return v;}
bool has_text(void *o,size_t off){void *s=at<void*>(o,off);return s&&at<int32_t>(s,0x10)>0;}  // System.String length

const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
std::string escape(const std::string &s){std::string out;for(unsigned char c:s){if(c=='"'||c=='\\')out+='\\';if(c>=32)out+=char(c);}return out;}
void event(const std::string &json){auto tr=transport();if(!tr)return;PgrRuntimeTransportString s[]={{"smart-heal",10},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,s,2);}
void status(const char *phase,const std::string &message){event(std::string("{\"phase\":\"")+phase+"\",\"message\":\""+escape(message)+"\"}");}

struct Config {bool on=false;int when=0,threshold=50;bool revive=true;};  // when: 0 after battle, 1 periodic, 2 both
// "H1|enabled|when|threshold|revive"; anything else (or a stale snapshot) means off.
Config configured(bool &on_map){
 Config c;on_map=false;auto tr=transport();PgrRuntimeTransportSample a{};char s[64]{};uint32_t n=0;
 if(!tr||tr->sample(&a)||!a.snapshot_fresh||a.fault||tr->get_string(18,s,sizeof(s)-1,&n)||n>=sizeof(s))return c;
 s[n]=0;int e=0,w=0,t=0,r=0;
 if(std::sscanf(s,"H1|%d|%d|%d|%d",&e,&w,&t,&r)!=4||(e!=0&&e!=1)||w<0||w>2||(t!=25&&t!=50&&t!=75&&t!=100)||(r!=0&&r!=1))return c;
 int active=0,ready=0,map=0;tr->get_bool(3,&active);tr->get_bool(16,&ready);tr->get_bool(38,&map);
 c.on=e==1&&active&&ready;c.when=w;c.threshold=t;c.revive=r==1;on_map=map!=0;return c;
}

constexpr uint64_t kScanMs=4000,kSweepMs=120000,kFirstSweepMs=10000,kGapMs=400,kResponseMs=15000,kConfirmMs=5000,kRetryMs=600000;
const char *const kItemNames[KIND_COUNT]={"Potion","Super Potion","Hyper Potion","Max Potion","Revive","Max Revive"};
const char *const kItemFields[KIND_COUNT]={"Potion","SuperPotion","HyperPotion","MaxPotion","Revive","MaxRevive"};
struct Fields {size_t id=0,hp=0,max=0,egg=0,fort=0,station=0,species=0;};
struct State {
 Root bag,items;void *pokemon_class=nullptr;Fields f;int item_id[KIND_COUNT]={};
 std::unordered_map<uint64_t,int> last_hp;bool baseline=false;
 std::vector<uint64_t> queue;std::set<uint64_t> queued;std::map<uint64_t,uint64_t> skip_until;
 uint64_t next_scan=0,next_sweep=0,next_action=0;
 // The one item in use: its promise, Pokemon, HP before, and when it was sent / answered.
 Root promise;uint64_t use_id=0,use_sent=0,use_done=0;int use_kind=-1,use_hp=0,use_species=0;bool use_rejected=false;
 int used[KIND_COUNT]={};
} st;

void reset_queue(){st.queue.clear();st.queued.clear();}
void prepare(){
 if(st.bag&&st.items)return;
 st.bag=resolve(cls("Niantic.Holoholo","IPokemonBag"));st.items=resolve(cls("Niantic.Holoholo","IItemBag"));
 require(st.bag&&st.items,"Pokemon storage or item bag unavailable");
 if(!st.pokemon_class){
  void *k=cls("Holoholo.Rpc","PokemonProto");Fields f;
  f.id=offset_of(k,"id_","System.UInt64");f.hp=offset_of(k,"stamina_","System.Int32");f.max=offset_of(k,"maxStamina_","System.Int32");
  f.egg=offset_of(k,"isEgg_","System.Boolean");f.fort=offset_of(k,"deployedFortId_","System.String");f.station=offset_of(k,"deployedStationId_","System.String");
  f.species=offset_of(k,"pokemonId_","System.Int32");
  void *item=cls("Holoholo.Rpc","Item");for(int i=0;i<KIND_COUNT;++i){st.item_id[i]=literal(item,kItemFields[i]);require(st.item_id[i]>0,kItemFields[i]);}
  st.f=f;st.pokemon_class=k;
 }
}
Stock stock(){
 Stock s;for(int i=0;i<KIND_COUNT;++i){int id=st.item_id[i];void *a[]={&id};s.count[i]=unbox<int32_t>(call(st.items.get(),"GetItemCount",1,a));if(s.count[i]<0)s.count[i]=0;}
 return s;
}
bool usable(void *p){return p&&!at<bool>(p,st.f.egg)&&!has_text(p,st.f.fort)&&!has_text(p,st.f.station);}
bool below(int hp,int max,const Config &c){
 if(max<=0||hp>=max)return false;
 if(hp==0)return c.revive;
 return c.threshold>=100||long(hp)*100<long(c.threshold)*max;
}
void enqueue(uint64_t id){if(st.queued.insert(id).second)st.queue.push_back(id);}

// Reads every stored Pokemon once: queues the ones that just fought (HP dropped) or, on a periodic
// sweep, every one below the threshold.
void scan(const Config &c,uint64_t now){
 Root all(call(st.bag.get(),"get_AllPokemon"));require(bool(all),"Pokemon storage list unavailable");
 void *k=klass(all.get());
 const int n=unbox<int32_t>(invoke(method(k,"get_Count",0),all.get()));require(n>=0&&n<=30000,"Pokemon storage size");
 Root array(fn<void*(*)(void*,uintptr_t)>(exp("array_new"))(st.pokemon_class,uintptr_t(n)));require(bool(array),"storage copy");
 int zero=0;void *args[]={array.get(),&zero};invoke(method(k,"CopyTo",2),all.get(),args);
 const bool after=c.when!=1&&st.baseline,sweep=c.when!=0&&now>=st.next_sweep;
 std::unordered_map<uint64_t,int> hp_now;hp_now.reserve(size_t(n));
 for(int i=0;i<n;++i){
  void *p=at<void*>(array.get(),0x20+size_t(i)*8);if(!p||klass(p)!=st.pokemon_class)continue;
  const uint64_t id=at<uint64_t>(p,st.f.id);const int hp=at<int32_t>(p,st.f.hp),max=at<int32_t>(p,st.f.max);
  if(!id||at<bool>(p,st.f.egg))continue;
  hp_now[id]=hp;
  if(!usable(p)||!below(hp,max,c))continue;
  auto skip=st.skip_until.find(id);if(skip!=st.skip_until.end()&&skip->second>now)continue;
  bool fought=false;
  if(after){auto was=st.last_hp.find(id);fought=was!=st.last_hp.end()&&hp<was->second;}
  if(fought||sweep)enqueue(id);
 }
 st.last_hp.swap(hp_now);st.baseline=true;
 if(sweep)st.next_sweep=now+kSweepMs;
}

// Uses the next item on the first queued Pokemon, or drops it when it is full or nothing is worth using.
void heal_next(const Config &c,uint64_t now){
 while(!st.queue.empty()){
  const uint64_t id=st.queue.front();
  uint64_t key=id;void *a[]={&key};Root p(call(st.bag.get(),"GetPokemon",1,a));
  if(!p||klass(p.get())!=st.pokemon_class||!usable(p.get())){st.queued.erase(id);st.queue.erase(st.queue.begin());continue;}
  const int hp=at<int32_t>(p.get(),st.f.hp),max=at<int32_t>(p.get(),st.f.max);
  if(hp==0&&!c.revive){st.queued.erase(id);st.queue.erase(st.queue.begin());continue;}
  const Plan plan=pgr_heal::plan(hp,max,stock(),c.revive);const int kind=first(plan);
  if(kind<0){
   // Full, or nothing on hand is worth using on it: look again only after a while.
   if(hp<max)st.skip_until[id]=now+kRetryMs;
   st.queued.erase(id);st.queue.erase(st.queue.begin());continue;
  }
  int item=st.item_id[kind];uint64_t pid=id;void *args[]={&item,&pid};
  st.use_id=id;st.use_kind=kind;st.use_hp=hp;st.use_species=at<int32_t>(p.get(),st.f.species);st.use_sent=now;st.use_done=0;st.use_rejected=false;
  st.promise=Root(call(st.items.get(),"UseItemOnPokemon",2,args));
  require(bool(st.promise),"heal request was not sent");
  return;
 }
}
void finish_use(bool ok,const std::string &why){
 if(ok){
  ++st.used[st.use_kind];
  event("{\"phase\":\"healed\",\"item\":\""+std::string(kItemNames[st.use_kind])+"\",\"species\":"+std::to_string(st.use_species)+",\"used\":["+
   std::to_string(st.used[0])+","+std::to_string(st.used[1])+","+std::to_string(st.used[2])+","+std::to_string(st.used[3])+","+std::to_string(st.used[4])+","+std::to_string(st.used[5])+"]}");
 }else{
  // Not retried soon: a declined or unconfirmed use must not repeat on the same Pokemon.
  st.skip_until[st.use_id]=clock_ms()+kRetryMs;st.queued.erase(st.use_id);
  for(auto it=st.queue.begin();it!=st.queue.end();++it)if(*it==st.use_id){st.queue.erase(it);break;}
  status("warning",std::string(kItemNames[st.use_kind])+" not used: "+why);
 }
 st.promise.reset();st.use_id=0;st.use_kind=-1;st.next_action=clock_ms()+kGapMs;
}
// Waits for the game's answer, then for storage to show the new HP before the next item.
void poll_use(uint64_t now){
 if(!st.use_done){
  auto vr=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));require(vr,"response polling unavailable");
  PgrVgpResultsResult out{};const int rc=vr->poll(st.promise.get(),PGR_VR_PHASE_ENCOUNTER,owner,&out);
  if(out.value_handle)fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(uintptr_t(out.value_handle));
  if(rc==PGR_VR_PENDING&&now-st.use_sent<=kResponseMs)return;
  // Answered (or no answer in time): storage decides. A higher HP means the item was used.
  st.use_done=now;st.use_rejected=rc!=PGR_VR_DONE;
 }
 uint64_t key=st.use_id;void *a[]={&key};Root p(call(st.bag.get(),"GetPokemon",1,a));
 if(p&&at<int32_t>(p.get(),st.f.hp)>st.use_hp){finish_use(true,"");return;}
 if(now-st.use_done>kConfirmMs)finish_use(false,st.use_rejected?"declined by the game":"HP did not change");
}
}  // namespace

void pgr_heal_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||faulted||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid))return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 bool on_map=false;const Config c=configured(on_map);const uint64_t now=clock_ms();
 try{
  if(!c.on&&!st.promise){if(st.baseline||!st.queue.empty()){st.last_hp.clear();st.baseline=false;reset_queue();}return;}
  if(!initialized){
   auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);
   require(api&&api->snapshot(&boot)==0,"runtime unavailable");md=metadata;require(md,"shared metadata");owner=tid;initialized=true;
   st.next_sweep=now+kFirstSweepMs;status("ready","Smart healing ready");
  }
  require(fn<void*(*)()>(boot.runtime.thread_current)()!=nullptr,"owner thread not attached");
  if(st.promise){poll_use(now);return;}  // an item in use is always followed to its answer
  if(!c.on||!on_map)return;
  prepare();
  if(now>=st.next_scan){st.next_scan=now+kScanMs;scan(c,now);}
  if(now>=st.next_action)heal_next(c,now);
 }catch(const Failure &f){
  st.promise.reset();st.use_id=0;reset_queue();
  st.bag.reset();st.items.reset();st.next_scan=now+kScanMs*4;  // services are resolved again later
  status("error",std::string("Smart healing paused: ")+f.where);
 }catch(...){faulted=true;st.promise.reset();status("error","Smart healing stopped after an unexpected error");}
}
void pgr_heal_stop(uint64_t tid){
 if(!initialized||tid!=owner||tid!=uint64_t(gettid()))return;
 st.promise.reset();st.use_id=0;reset_queue();st.last_hp.clear();st.baseline=false;st.bag.reset();st.items.reset();
}

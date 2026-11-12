// Exercise the actual healing runtime with deterministic storage, item promises and clock.
#include <cassert>
#include <cstdlib>
#include <memory>
#include <functional>
#include <thread>
#include <ctime>
extern "C" void *heal_fixture_symbol(void *,const char *);
extern "C" int heal_fixture_clock(clockid_t,struct timespec *);
#define dlsym heal_fixture_symbol
#define clock_gettime heal_fixture_clock
#include "heal.cpp"
#undef dlsym
#undef clock_gettime

namespace fixture {
struct Class {std::string name;};
struct Field {std::string name,type;size_t offset;};
struct Object {Class *type;char bytes[512]{};};
std::map<std::string,std::unique_ptr<Class>> classes;
std::map<std::string,std::unique_ptr<Field>> fields;
std::map<std::string,std::unique_ptr<std::string>> methods;
std::vector<std::unique_ptr<Object>> objects;
std::map<uintptr_t,void*> roots;uintptr_t next_root=1;
uint64_t now=10000;bool enabled=true,fresh=true,active=true,ready=true,on_map=false,permission=true;
int when=0,threshold=50;bool revive=true,full=true;
int stock_count[6]={10,10,10,10,10,10};int sends=0,polls=0,scans=0;
int sent_item=0;uint64_t sent_id=0;bool answered=false;
std::function<void()> lookup_callback;
PgrBootBinding binding{};PgrBootApi boot_api{};PgrMetaApi meta_api{};
PgrRuntimeTransportApi transport_api{};PgrVgpResultsApi results_api{};PgrSharedMetadataOutput metadata{};
Object *map_obj,*container_obj,*bag_obj,*items_obj,*list_obj,*promise_obj;
std::vector<Object*> pokemon;
Class *type(const std::string &s){auto &p=classes[s];if(!p)p.reset(new Class{s});return p.get();}
Object *object(const std::string &s){auto p=std::make_unique<Object>();p->type=type(s);auto *v=p.get();objects.push_back(std::move(p));return v;}
template<class T>void put(void *o,size_t off,T value){memcpy(static_cast<char*>(o)+off,&value,sizeof(value));}
Object *box(int value){auto *o=object("Box");put(o,16,value);return o;}
Object *mon(uint64_t id,int hp,int maximum=100){auto *o=object("Holoholo.Rpc.PokemonProto");put(o,16,id);put(o,24,hp);put(o,28,maximum);put(o,56,25);pokemon.push_back(o);return o;}
Object *find(uint64_t id){for(auto *p:pokemon)if(at<uint64_t>(p,16)==id)return p;return nullptr;}
void *method_lookup(void *,const char *name,int){auto &p=methods[name];if(!p)p.reset(new std::string(name));return p.get();}
void *field_lookup(void *,const char *name){auto &f=fields[name];if(!f){
 size_t off=0;std::string t="System.Int32",n=name;
 if(n=="id_"){off=16;t="System.UInt64";}else if(n=="stamina_")off=24;else if(n=="maxStamina_")off=28;
 else if(n=="isEgg_"){off=32;t="System.Boolean";}else if(n=="deployedFortId_"){off=40;t="System.String";}
 else if(n=="deployedStationId_"){off=48;t="System.String";}else if(n=="pokemonId_")off=56;
 f.reset(new Field{n,t,off});}return f.get();}
int field_flags(void *p){return static_cast<Field*>(p)->offset?0:0x8050;}
void static_value(void *p,void *out){for(int i=0;i<6;i++)if(static_cast<Field*>(p)->name==kItemFields[i]){*static_cast<int*>(out)=i+1;return;}assert(false);}
void *field_type(void *p){return type(static_cast<Field*>(p)->type);}
char *type_name(void *p){return strdup(static_cast<Class*>(p)->name.c_str());}
size_t field_offset(void *p){return static_cast<Field*>(p)->offset;}
void *object_class(void *p){return static_cast<Object*>(p)->type;}
void *unbox_value(void *p){return static_cast<char*>(p)+16;}
uintptr_t root_new(void *p,int){auto id=next_root++;roots[id]=p;return id;}
void *root_get(uintptr_t id){return roots.at(id);}
void root_free(uintptr_t id){assert(roots.erase(id)==1);}
void *current_thread(){return &metadata;}
void *class_type(void *p){return p;}
void *type_object(void *p){return p;}
void *array_new(void *,uintptr_t n){assert(n<50);return object("Array");}
int resolve_class(const PgrMetaClassQuery *q,PgrMetaClassResult *r){*r={};r->klass=type(std::string(q->name_space)+"."+q->name);return 0;}
int boot_snapshot(PgrBootBinding *out){*out=binding;return 0;}
void *invoke(void *m,void *,void **args,void **exception){
 *exception=nullptr;const std::string n=*static_cast<std::string*>(m);
 if(n=="find")return map_obj;if(n=="container")return container_obj;
 if(n=="resolve")return static_cast<Class*>(args[0])->name=="Niantic.Holoholo.IPokemonBag"?bag_obj:items_obj;
 if(n=="get_AllPokemon"){++scans;return list_obj;}
 if(n=="get_Count")return box(int(pokemon.size()));
 if(n=="CopyTo"){for(size_t i=0;i<pokemon.size();i++)put(args[0],32+8*i,pokemon[i]);return nullptr;}
 if(n=="GetPokemon"){if(lookup_callback){auto cb=std::move(lookup_callback);lookup_callback={};cb();}return find(*static_cast<uint64_t*>(args[0]));}
 if(n=="GetItemCount")return box(stock_count[*static_cast<int*>(args[0])-1]);
 if(n=="UseItemOnPokemon"){assert(permission&&enabled&&fresh&&active&&ready&&(when!=1||on_map));++sends;sent_item=*static_cast<int*>(args[0]);sent_id=*static_cast<uint64_t*>(args[1]);assert(stock_count[sent_item-1]>0);--stock_count[sent_item-1];answered=false;return promise_obj;}
 assert(false);return nullptr;
}
int sample(PgrRuntimeTransportSample *out){*out={};out->snapshot_fresh=fresh;out->snapshot_seq=1;return 0;}
int get_bool(uint32_t id,int *out){*out=id==3?active:id==16?ready:id==38?on_map:0;return 0;}
int get_string(uint32_t id,char *out,uint32_t cap,uint32_t *length){assert(id==18);std::string s="H2|"+std::to_string(enabled)+"|"+std::to_string(when)+"|"+std::to_string(threshold)+"|"+std::to_string(revive)+"|"+std::to_string(full);assert(s.size()<cap);memcpy(out,s.data(),s.size());*length=s.size();return 0;}
int emit(uint32_t,const int64_t*,uint32_t,const PgrRuntimeTransportString*,uint32_t){return 0;}
int poll(void *p,uint32_t,uint64_t,PgrVgpResultsResult *out){assert(p==promise_obj);*out={};++polls;return answered?PGR_VR_DONE:PGR_VR_PENDING;}
bool allowed(){return permission;}
uint64_t tid(){return uint64_t(gettid());}
int tick(uint64_t token=1){return pgr_heal_gym_tick(tid(),&metadata,token,allowed);}
void baseline(){pgr_heal_gym_baseline(tid(),&metadata);assert(sends==0);}
void answer(int hp){assert(st.promise);put(find(sent_id),24,hp);answered=true;now+=500;}
void reset(){
 if(initialized)pgr_heal_stop(tid());st=State{};gym_reset();initialized=faulted=busy=false;owner=0;md=nullptr;assert(roots.empty());
 objects.clear();pokemon.clear();now+=100000;enabled=fresh=active=ready=permission=true;on_map=false;when=0;threshold=50;revive=full=true;
 for(int &v:stock_count)v=10;sends=polls=scans=0;sent_item=0;sent_id=0;answered=false;lookup_callback={};
 map_obj=object("Map");container_obj=object("Container");bag_obj=object("Bag");items_obj=object("Items");list_obj=object("List");promise_obj=object("Promise");
 binding={};binding.handle=reinterpret_cast<void*>(1);
 auto &r=binding.runtime;r.gchandle_new=reinterpret_cast<void*>(root_new);r.gchandle_get_target=reinterpret_cast<void*>(root_get);r.gchandle_free=reinterpret_cast<void*>(root_free);r.object_get_class=reinterpret_cast<void*>(object_class);r.object_unbox=reinterpret_cast<void*>(unbox_value);r.runtime_invoke=reinterpret_cast<void*>(invoke);r.field_static_get_value=reinterpret_cast<void*>(static_value);r.thread_current=reinterpret_cast<void*>(current_thread);
 auto &m=binding.meta;m.class_get_type=reinterpret_cast<void*>(class_type);m.class_get_method_from_name=reinterpret_cast<void*>(method_lookup);m.class_get_field_from_name=reinterpret_cast<void*>(field_lookup);m.field_get_flags=reinterpret_cast<void*>(field_flags);m.field_get_type=reinterpret_cast<void*>(field_type);m.type_get_name=reinterpret_cast<void*>(type_name);m.mem_free=reinterpret_cast<void*>(free);
 metadata={};metadata.opt_handles[H_IL_MAP]=type("Map");metadata.opt_handles[H_IL_FIND_M]=method_lookup(nullptr,"find",1);metadata.opt_handles[H_IL_CONTAINER_M]=method_lookup(nullptr,"container",0);metadata.opt_handles[H_IL_RESOLVE_M]=method_lookup(nullptr,"resolve",1);
 boot_api.snapshot=boot_snapshot;meta_api.resolve_class=resolve_class;transport_api.sample=sample;transport_api.get_bool=get_bool;transport_api.get_string=get_string;transport_api.emit=emit;results_api.poll=poll;
}
void tests(){
 reset();auto *a=mon(1,100);baseline();put(a,24,20);pgr_heal_tick(tid(),&metadata);assert(sends==0);assert(tick()==1&&sends==1);assert(tick()==1&&sends==1);
 answer(100);assert(tick()==1);now+=4000;assert(tick()==0&&sends==1);int count=scans;assert(tick()==0&&scans==count);
 // A second round with less damage must compare against HEALED HP, not old damaged HP.
 put(a,24,40);assert(tick(2)==1&&sends==2);answer(100);tick(2);now+=4000;assert(tick(2)==0);
 // A delayed storage write, and a second participant after the first healed.
 reset();a=mon(1,100);auto *b=mon(2,100);baseline();assert(tick()==1&&sends==0);now+=1000;put(a,24,20);assert(tick()==1&&sends==1);answer(100);tick();now+=500;put(b,24,30);assert(tick()==1&&sends==2);answer(100);tick();now+=4000;assert(tick()==0);
 // A map tick may detect the damage first; beginning a gym window preserves its queue.
 reset();a=mon(1,100);baseline();put(a,24,20);Config c;c.on=true;c.threshold=50;scan(c,now);assert(!st.queue.empty());assert(tick()==1&&sends==1);
 // No baseline never guesses that all injured storage participated.
 reset();mon(1,20);assert(tick()==0&&sends==0);
 for(int mode=0;mode<5;mode++){
  reset();a=mon(1,100);baseline();put(a,24,20);
  if(mode==0)enabled=false;if(mode==1)when=1;if(mode==2)fresh=false;if(mode==3)permission=false;if(mode==4)active=false;
  assert(tick()==0&&sends==0);
 }
 // Periodic-only baseline call doesn't destroy ordinary periodic state/queue.
 reset();a=mon(1,100);baseline();enqueue(1);when=1;baseline();assert(st.baseline&&!st.queue.empty()&&!gym.armed);
 // HP exactly at threshold is ineligible; revive preference and stock are honoured.
 reset();a=mon(1,100);baseline();put(a,24,50);tick();now+=4000;assert(tick()==0&&sends==0);
 reset();a=mon(1,100);revive=false;baseline();put(a,24,0);tick();now+=4000;assert(tick()==0&&sends==0);
 reset();a=mon(1,100);baseline();put(a,24,0);assert(tick()==1&&sends==1&&(sent_item==5||sent_item==6));
 reset();a=mon(1,100);baseline();put(a,24,20);for(int &v:stock_count)v=0;tick();now+=4000;assert(tick()==0&&sends==0);
 // Disabling either permission during a managed lookup must prevent the item side effect.
 for(int i=0;i<2;i++){reset();a=mon(1,100);baseline();put(a,24,20);lookup_callback=[i](){if(i)enabled=false;else permission=false;};tick();assert(sends==0);}
 reset();a=mon(1,100);when=2;baseline();put(a,24,20);assert(tick()==1&&sends==1);
 // A successful response without updated HP still blocks and never sends a duplicate item.
 reset();a=mon(1,100);baseline();put(a,24,20);tick();answered=true;now+=500;assert(tick()==1&&sends==1);now+=1000;assert(tick()==1&&sends==1);answer(100);tick();now+=4000;assert(tick()==0&&sends==1);
 // Item conservation vs Always heal fully retains the existing user choice.
 for(int f=0;f<2;f++){reset();a=mon(1,100);threshold=100;full=f;for(int &v:stock_count)v=0;stock_count[MAX_POTION]=1;baseline();put(a,24,90);tick();assert(sends==f);}
 // Ordinary on-map periodic healing remains operational.
 reset();mon(1,20);when=1;on_map=true;pgr_heal_tick(tid(),&metadata);assert(sends==0);now+=10001;pgr_heal_tick(tid(),&metadata);assert(sends==1);
 // Owner and callback are mandatory. Stop releases all persistent roots and the token.
 reset();mon(1,100);baseline();assert(pgr_heal_gym_tick(tid(),&metadata,1,nullptr)==-1);int wrong=0;std::thread t([&](){wrong=tick();});t.join();assert(wrong==-1);pgr_heal_stop(tid());assert(roots.empty()&&!gym.armed);assert(tick()==0);
 // Existing item planner still selects revival first and never spends missing stock.
 pgr_heal::Stock stock{};stock.count[pgr_heal::POTION]=2;auto p=pgr_heal::plan(20,100,stock,true,true);assert(p.count[pgr_heal::POTION]==2&&p.healed==40);
 stock.count[pgr_heal::MAX_REVIVE]=1;p=pgr_heal::plan(0,100,stock,true,true);assert(pgr_heal::first(p)==pgr_heal::MAX_REVIVE);
 reset();puts("heal runtime: PASS (off-map permission, delayed HP, multiple rounds, pending items, preferences, cancellation, roots)");
}
}
const void *pgrazor_runtime_bootstrap_api(uint32_t){return &fixture::boot_api;}
const void *pgrazor_runtime_metadata_api(uint32_t){return &fixture::meta_api;}
const void *pgrazor_runtime_transport_api(uint32_t){return &fixture::transport_api;}
const void *pgrazor_vgp_results_api(uint32_t){return &fixture::results_api;}
extern "C" void *heal_fixture_symbol(void *,const char *n){
 std::string s=n;if(s=="il2cpp_type_get_object")return reinterpret_cast<void*>(fixture::type_object);
 if(s=="il2cpp_array_new")return reinterpret_cast<void*>(fixture::array_new);
 if(s=="il2cpp_field_get_offset")return reinterpret_cast<void*>(fixture::field_offset);
 assert(false);return nullptr;
}
extern "C" int heal_fixture_clock(clockid_t,struct timespec *t){t->tv_sec=fixture::now/1000;t->tv_nsec=(fixture::now%1000)*1000000;return 0;}
int main(){fixture::tests();}

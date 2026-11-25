#include "auto_boost.h"
#include "shared_metadata.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <time.h>
#include <unistd.h>


// Runtime. Every 10 s on the map the player's active buffs (IPlayerBuffService.ActiveBuffs, the list
// the map's buff icons come from) are read. A wanted boost that is not running is used the way the item
// bag does it: IPlayerBuffService.SelectBuff(ItemSettingsProto, SelectionSource.MainMenu, null) opens the
// item's screen, and its Use (PlayerBuffSelectionGuiController.PressUse, the tap on the item) is pressed
// once that screen is open. Then nothing more for 30 s while the game applies it. A use that does not
// start the boost counts as a miss; after three misses in a row that boost waits 10 minutes.
namespace {
using namespace pgr_shared_meta;
using namespace pgr_auto_boost;
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
void *method(void *k,const char *name,int count){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,count);require(m,name);return m;}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(klass(o),name,n),o,args);}
template<class T>T unbox(void *o){require(o,"null result");T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");memcpy(&v,p,sizeof(v));return v;}
template<class T>T at(void *o,size_t off){T v{};memcpy(&v,static_cast<char*>(o)+off,sizeof(v));return v;}
void *h(unsigned id){return const_cast<void*>(md->opt_handles[id]);}
Root type_object(void *k){return Root(fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k)));}
Root resolve(void *contract){
 Root type=type_object(h(H_IL_MAP));void *args[]={type.get()};Root map(invoke(h(H_IL_FIND_M),nullptr,args));if(!map)return {};
 Root container(invoke(h(H_IL_CONTAINER_M),map.get()));if(!container)return {};
 Root target=type_object(contract);void *resolve_args[]={target.get()};return Root(invoke(h(H_IL_RESOLVE_M),container.get(),resolve_args));
}
int literal(void *k,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);require((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)==0x8050,name);int n=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&n);return n;}

const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
void event(const std::string &json){auto tr=transport();if(!tr)return;PgrRuntimeTransportString s[]={{"auto-boost",10},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,s,2);}
void status(const char *phase,const std::string &message){event(std::string("{\"phase\":\"")+phase+"\",\"message\":\""+message+"\"}");}

// Slot 26 settings, honoured only with a fresh snapshot while the game is active, the account ready
// and the map showing (the same gates Smart healing uses).
Config configured(bool &on_map){
 Config c;on_map=false;auto tr=transport();PgrRuntimeTransportSample a{};char s[32]{};uint32_t n=0;
 if(!tr||tr->sample(&a)||!a.snapshot_fresh||a.fault||tr->get_string(26,s,sizeof(s)-1,&n)||n>=sizeof(s))return {};
 s[n]=0;c=parse(s);if(!c.valid)return {};
 int active=0,ready=0,map=0;tr->get_bool(3,&active);tr->get_bool(16,&ready);tr->get_bool(38,&map);
 if(!active||!ready){c.egg=c.star=false;}
 on_map=map!=0;return c;
}

constexpr uint64_t kPollMs=10000,kSettleMs=30000,kBackoffMs=600000;constexpr int kMaxMisses=3;
struct Boost {
 const char *name;const char *item_field;void *klass=nullptr;int item=0;
 uint64_t not_before=0;int misses=0;bool pending=false;  // pending: used, not yet seen active
};
struct State {
 Root buffs_service,items,master;void *buff_iface=nullptr,*buff_base=nullptr,*is_active=nullptr;int main_menu=0;
 void *screen_class=nullptr,*state_field=nullptr,*find_all=nullptr,*alive=nullptr;
 int confirm=-1;uint64_t confirm_until=0,next_confirm=0;  // the boost whose screen we opened, and until when
 Boost boost[2]={{"Lucky Egg","LuckyEgg"},{"Star Piece","StarPiece"}};
 uint64_t next_poll=0;
} st;

void prepare(){
 if(st.buffs_service&&st.items&&st.master)return;
 st.buffs_service=resolve(cls("Niantic.Holoholo.Items","IPlayerBuffService"));
 st.items=resolve(cls("Niantic.Holoholo","IItemBag"));
 st.master=resolve(cls("","IGameMasterData"));
 require(st.buffs_service&&st.items&&st.master,"buff service, item bag or game data unavailable");
 if(!st.buff_iface){
  st.buff_iface=cls("Niantic.Holoholo.Items","IPlayerBuff");
  st.buff_base=cls("Niantic.Holoholo.Items","ItemBuffBase");
  st.is_active=method(st.buff_base,"get_IsActive",0);
  // SelectionSource is nested in PlayerBuffService: take it from SelectBuff's second parameter.
  void *select=method(klass(st.buffs_service.get()),"SelectBuff",3);
  void *source_type=const_cast<void*>(fn<const void*(*)(void*,uint32_t)>(boot.meta.method_get_param)(select,1));require(source_type,"SelectBuff source type");
  void *source_class=fn<void*(*)(const void*)>(exp("class_from_type"))(source_type);require(source_class,"SelectionSource");
  st.main_menu=literal(source_class,"MainMenu");
  st.screen_class=cls("Niantic.Holoholo.Items","PlayerBuffSelectionGuiController");
  st.state_field=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(cls("Niantic.Holoholo.NUI","GuiController"),"currentState");require(st.state_field,"GuiController.currentState");
  st.find_all=method(cls("UnityEngine","Resources"),"FindObjectsOfTypeAll",1);
  st.alive=method(cls("UnityEngine","Object"),"op_Implicit",1);
  void *item=cls("Holoholo.Rpc","Item");
  st.boost[0].klass=cls("Niantic.Holoholo.Items","LuckyEgg");st.boost[1].klass=cls("Niantic.Holoholo.Items","StarPiece");
  for(auto &b:st.boost){b.item=literal(item,b.item_field);require(b.item>0,b.item_field);}
 }
}
// Which of the two boosts is running: an active LuckyEgg / StarPiece object in the buff list.
void running(bool active[2]){
 active[0]=active[1]=false;
 Root list(call(st.buffs_service.get(),"get_ActiveBuffs"));if(!list)return;
 void *k=klass(list.get());
 const int n=unbox<int32_t>(invoke(method(k,"get_Count",0),list.get()));require(n>=0&&n<=256,"buff list size");
 if(!n)return;
 Root array(fn<void*(*)(void*,uintptr_t)>(exp("array_new"))(st.buff_iface,uintptr_t(n)));require(bool(array),"buff list copy");
 int zero=0;void *args[]={array.get(),&zero};invoke(method(k,"CopyTo",2),list.get(),args);
 for(int i=0;i<n;++i){
  void *b=at<void*>(array.get(),0x20+size_t(i)*8);if(!b)continue;
  void *bk=klass(b);
  for(int j=0;j<2;++j)if(bk==st.boost[j].klass&&unbox<bool>(invoke(st.is_active,b)))active[j]=true;
 }
}
// The open buff screen (GuiController.currentState 2), if any.
Root open_screen(){
 Root type=type_object(st.screen_class);void *args[]={type.get()};Root arr(invoke(st.find_all,nullptr,args));if(!arr)return {};
 const size_t n=fn<size_t(*)(void*)>(exp("array_length"))(arr.get());
 for(size_t i=0;i<n&&i<8;++i){
  void *o=at<void*>(arr.get(),0x20+i*8);if(!o)continue;
  void *a[]={o};if(!unbox<bool>(invoke(st.alive,nullptr,a)))continue;
  int32_t state=0;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,st.state_field,&state);
  if(state==2)return Root(o);
 }
 return {};
}
int stock(const Boost &b){int id=b.item;void *a[]={&id};const int n=unbox<int32_t>(call(st.items.get(),"GetItemCount",1,a));return n>0?n:0;}
constexpr uint64_t kConfirmMs=8000,kConfirmPollMs=300;
// Opens the item's screen (as tapping it in the bag); confirm() presses its Use once it is open.
void use(int j,uint64_t now){
 Boost &b=st.boost[j];
 int id=b.item;void *a[]={&id};Root settings(call(st.master.get(),"GetItemSettings",1,a));require(bool(settings),"item settings unavailable");
 int source=st.main_menu;void *args[]={settings.get(),&source,nullptr};
 call(st.buffs_service.get(),"SelectBuff",3,args);
 b.pending=true;b.not_before=now+kSettleMs;st.confirm=j;st.confirm_until=now+kConfirmMs;st.next_confirm=now;
}
// Presses Use on the screen use() opened. Returns once that is done or given up.
void confirm(uint64_t now){
 Boost &b=st.boost[st.confirm];
 if(Root screen=open_screen()){
  call(screen.get(),"PressUse");st.confirm=-1;b.not_before=now+kSettleMs;
  status("used",std::string(b.name)+" used");return;
 }
 if(now>=st.confirm_until){st.confirm=-1;status("warning",std::string(b.name)+" screen did not open");}
}
void ensure_init(const PgrSharedMetadataOutput *metadata,uint64_t tid){
 if(initialized)return;
 auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);
 require(api&&api->snapshot(&boot)==0,"runtime unavailable");md=metadata;require(md,"shared metadata");owner=tid;initialized=true;
}
void tick(const Config &c,uint64_t now){
 const bool want[2]={c.egg,c.star};
 prepare();
 bool active[2];running(active);
 for(int j=0;j<2;++j){
  Boost &b=st.boost[j];
  if(active[j]){b.pending=false;b.misses=0;continue;}
  if(b.pending&&now>=b.not_before){  // used, settle time over, still not running
   b.pending=false;
   if(++b.misses>=kMaxMisses){b.misses=0;b.not_before=now+kBackoffMs;status("warning",std::string(b.name)+" did not start; trying again in 10 minutes");continue;}
  }
  if(!want[j]||b.pending)continue;
  if(should_use(true,false,stock(b),now,b.not_before)){
   if(open_screen())return;  // a buff screen is already open (the player's): leave it alone
   use(j,now);return;        // one item per tick
  }
 }
}
}  // namespace

void pgr_auto_boost_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||faulted||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid))return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 bool on_map=false;const Config c=configured(on_map);const uint64_t now=clock_ms();
 try{
  if(st.confirm>=0){  // the screen opened by use() leaves the map: follow it regardless of the map gate
   if(now<st.next_confirm)return;st.next_confirm=now+kConfirmPollMs;
   require(fn<void*(*)()>(boot.runtime.thread_current)()!=nullptr,"owner thread not attached");
   confirm(now);return;
  }
  if((!c.egg&&!c.star)||!on_map||now<st.next_poll)return;
  st.next_poll=now+kPollMs;
  ensure_init(metadata,tid);
  require(fn<void*(*)()>(boot.runtime.thread_current)()!=nullptr,"owner thread not attached");
  tick(c,now);
 }catch(const Failure &f){
  st.buffs_service.reset();st.items.reset();st.master.reset();st.confirm=-1;st.next_poll=now+kPollMs*6;  // resolved again later
  status("error",std::string("Auto boosts paused: ")+f.where);
 }catch(...){faulted=true;status("error","Auto boosts stopped after an unexpected error");}
}
void pgr_auto_boost_stop(uint64_t tid){
 if(!initialized||tid!=owner||tid!=uint64_t(gettid()))return;
 st.buffs_service.reset();st.items.reset();st.master.reset();st.confirm=-1;
 for(auto &b:st.boost){b.pending=false;b.misses=0;b.not_before=0;}
}

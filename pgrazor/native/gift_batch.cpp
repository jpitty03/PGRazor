#include "gift_batch.h"
#include <vector>
#include <map>
#include <string>
#include <limits>


#include "shared_metadata.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "vgp_results.h"
#include "remote_gym_policy.h"
#include "outgoing_gifts.h"
#include "incoming_gifts.h"
#include <dlfcn.h>
#include <unistd.h>
#include <time.h>
#include <cstring>
#include <cmath>
#include <set>
#include <algorithm>
#include <utility>
namespace {
using namespace pgr_shared_meta;

struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
template<class F>F fn(void *p){require(p,"missing runtime export");return reinterpret_cast<F>(p);}
PgrBootBinding boot{};const PgrSharedMetadataOutput *md=nullptr;
uint64_t owner=0,finished_token=0;
uint64_t clock_ms(clockid_t c=CLOCK_BOOTTIME){timespec t{};clock_gettime(c,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
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
std::string typename_of(void *type){char *p=fn<char*(*)(void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
void *field_info(void *k,const char *name,const char *type){
 void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);
 auto ft=fn<void*(*)(void*)>(boot.meta.field_get_type)(f);require(typename_of(ft)==type,name);
 require(!(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10),"instance field required");return f;
}
template<class T>T field(void *o,const char *name,const char *type){T value{};auto f=field_info(klass(o),name,type);fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&value);return value;}
template<class T>void put(void *o,const char *name,const char *type,T value){auto f=field_info(klass(o),name,type);fn<void(*)(void*,void*,void*)>(boot.runtime.field_set_value)(o,f,&value);}

void *method(void *k,const char *name,int count){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,count);require(m,name);require(fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(m)==unsigned(count),name);return m;}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(klass(o),name,n),o,args);}
template<class T>T boxed(void *o,const char *type){require(o&&typename_of(fn<void*(*)(void*)>(boot.meta.class_get_type)(klass(o)))==type,type);T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");memcpy(&v,p,sizeof(v));return v;}
std::string string(void *o){if(!o)return {};require(klass(o)==cls("System","String"),"string identity");int n=fn<int(*)(void*)>(exp("string_length"))(o);require(n>=0&&n<=512,"string bound");auto p=fn<const uint16_t*(*)(void*)>(boot.runtime.string_chars)(o);std::string s;for(int i=0;i<n;i++){if(p[i]>127)return "<non-ascii>";s+=char(p[i]);}return s;}
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
void *virtual_call(Root &o,void *m,void **args=nullptr){void *v=fn<void*(*)(void*,void*)>(exp("object_get_virtual_method"))(o.get(),m);require(v,"virtual method");return invoke(v,o.get(),args);}
struct Command {uint64_t token=0;bool enabled=false,full_bag=false;};
bool parse(const char *s,size_t n,Command &out){
 uint64_t v[3]{};if(n<8||memcmp(s,"G1|",3)||!pgr_remote_gym::detail::fields(s+3,n-3,v,3,INT64_MAX)||!v[0]||v[1]>1||v[2]>1)return false;
 out={v[0],v[1]!=0,v[2]!=0};return true;
}
struct Permission {Command command;bool valid=false,allowed=false;};
Permission permission(){
 Permission p;auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));if(!tr)return p;
 PgrRuntimeTransportSample a{},z{};if(tr->sample(&a)||!a.snapshot_fresh||a.fault)return p;
 char s[96]{};uint32_t n=0;int ready=0;
 if(tr->get_string(12,s,sizeof(s),&n)||n>=sizeof(s)||!parse(s,n,p.command))return p;
 tr->get_bool(12,&ready);
 if(tr->sample(&z)||!z.snapshot_fresh||z.fault||a.snapshot_seq!=z.snapshot_seq)return p;
 p.valid=true;p.allowed=ready==1;return p;
}
struct Friend {std::string id,nia;};
struct Gift {uint64_t id=0;std::string sender,nia;};
enum Stage {LIST,DISCOVER,SEND_CHECK,SEND,OPEN,DONE};
struct Session {
 Command command;bool active=false,canceled=false;std::string account;
 Root player,friends,gifting,rpc,promise,enumerator;
 std::vector<Friend> recipients;std::vector<uint64_t> sendable;std::vector<Gift> received;
 std::set<std::string> seen_friends;std::set<uint64_t> seen_gifts;
 size_t friend_cursor=0,gift_cursor=0,open_cursor=0;unsigned scanned=0,sent=0,opened=0,skipped=0;
 Stage stage=LIST;uint64_t since=0,next=0,started=0;bool send_authorized=false;
};
Session session;bool busy=false;
std::string quote(const std::string &s){std::string out="\"";for(unsigned char c:s){if(c=='"'||c=='\\')out+='\\';if(c>=32)out+=char(c);}return out+'"';}
void event(const char *phase,const std::string &message){
 auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));if(!tr)return;
 std::string json="{\"kind\":\"gift-batch\",\"token\":"+quote(std::to_string(session.command.token))+",\"phase\":"+quote(phase)+",\"message\":"+quote(message)+",\"sent\":"+std::to_string(session.sent)+",\"opened\":"+std::to_string(session.opened)+",\"skipped\":"+std::to_string(session.skipped)+"}";
 PgrRuntimeTransportString strings[]={{"gift-batch",10},{json.c_str(),uint32_t(json.size())}};
 if(tr->emit(19,nullptr,0,strings,2))session.canceled=true;
}
void finish(const char *phase,const std::string &message){
 event(phase,message);finished_token=session.command.token;
 if(session.enumerator){try{virtual_call(session.enumerator,h(H_INC_M_DISPOSE));}catch(...){}}
 session=Session{};
}
Root enumerate(Root &items){return Root(virtual_call(items,method(cls("System.Collections","IEnumerable"),"GetEnumerator",0)));}
bool move(Root &it){return boxed<bool>(virtual_call(it,h(H_INC_M_MOVE)),"System.Boolean");}
Root current(Root &it){return Root(virtual_call(it,h(H_INC_M_CURRENT)));}
Root reference(void *o,const char *name){
 auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name);require(f,name);
 auto k=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));
 require(k&&!fn<bool(*)(void*)>(boot.meta.class_is_valuetype)(k),"reference field");
 void *v=nullptr;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return Root(v);
}
void check_permission(){
 auto p=permission();require(p.valid&&p.allowed&&p.command.enabled&&p.command.token==session.command.token&&!session.canceled,"Batch stopped before request");
 require(string(call(session.player.get(),"get_PlayerId"))==session.account,"Account changed; batch stopped");
}
void submit(Stage stage,void *o,const char *name,int n=0,void **args=nullptr){
 check_permission();session.stage=stage;session.since=clock_ms();
 session.promise=Root(call(o,name,n,args));require(bool(session.promise),"Request outcome unknown; not retried");
}
// Services maintain their normal inventory/friend callbacks; no raw RPC or UI clicks.
void inventory(){
 Root rows(call(session.gifting.get(),"get_SendableGiftsInInventory"));require(bool(rows),"Sendable gifts unavailable");
 Root it=enumerate(rows);unsigned n=0;
 try{while(move(it)){
  require(++n<=1000,"Gift inventory exceeded bound");Root g=current(it);
  require(g&&klass(g.get())==cls("Holoholo.Rpc","GiftBoxProto"),"Gift inventory identity");
  uint64_t id=field<uint64_t>(g.get(),"GiftboxId","System.UInt64");
  if(id&&session.seen_gifts.insert(id).second)session.sendable.push_back(id);
 }virtual_call(it,h(H_INC_M_DISPOSE));}catch(...){try{virtual_call(it,h(H_INC_M_DISPOSE));}catch(...){}throw;}
}
void received_from(const Friend &f){
 Root id(fn<void*(*)(const char*)>(boot.runtime.string_new)(f.id.c_str()));void *args[]={id.get()};
 Root rows(call(session.friends.get(),"GetGiftboxDetails",1,args));if(!rows)return;
 Root it=enumerate(rows);unsigned n=0;
 try{while(move(it)){
  require(++n<=32&&session.received.size()<2000,"Received gift bound");Root g=current(it);
  require(g&&klass(g.get())==cls("Holoholo.Rpc","GiftBoxDetailsProto"),"Received gift identity");
  Gift gift;gift.id=field<uint64_t>(g.get(),"GiftboxId","System.UInt64");
  gift.sender=string(field<void*>(g.get(),"SenderId","System.String"));
  gift.nia=string(field<void*>(g.get(),"SenderNiaAccountId","System.String"));
  require(gift.sender==f.id,"Gift sender mismatch");
  if(gift.nia.empty())gift.nia=f.nia;
  if(gift.id&&session.seen_gifts.insert(gift.id).second)session.received.push_back(gift);
 }virtual_call(it,h(H_INC_M_DISPOSE));}catch(...){try{virtual_call(it,h(H_INC_M_DISPOSE));}catch(...){}throw;}
}

void discover(){
 for(unsigned i=0;i<8;i++){
  if(!move(session.enumerator)){
   virtual_call(session.enumerator,h(H_INC_M_DISPOSE));session.enumerator.reset();inventory();session.stage=SEND_CHECK;
   event("running","Found "+std::to_string(session.recipients.size())+" friends; sending gifts");return;
  }
  require(++session.scanned<=2000,"Friend list exceeded bound");Root f=current(session.enumerator);require(bool(f),"Friend identity");
  require(std::string(fn<const char*(*)(void*)>(exp("class_get_name"))(klass(f.get())))=="FriendProto","Friend identity");
  Friend next{string(field<void*>(f.get(),"playerId_","System.String")),string(field<void*>(f.get(),"niaAccountId_","System.String"))};
  if(!next.id.empty()&&next.id!=session.account&&session.seen_friends.insert(next.id).second){session.recipients.push_back(next);received_from(next);}
 }
}
std::string result_name(void *o){
 auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),"Result");require(f,"Result");
 auto k=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));
 int value=result_value(o);void *iter=nullptr;
 while(auto ef=fn<void*(*)(void*,void**)>(exp("class_get_fields"))(k,&iter)){
  if((fn<int(*)(void*)>(boot.meta.field_get_flags)(ef)&0x8050)!=0x8050)continue;
  int n=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(ef,&n);
  if(n==value)return fn<const char*(*)(void*)>(exp("field_get_name"))(ef);
 }return "Unknown result "+std::to_string(value);
}
void poll(){
 auto api=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));require(api,"Promise observer unavailable");
 PgrVgpResultsResult out{};int rc=api->poll(session.promise.get(),1,owner,&out);Root result;result.h=out.value_handle;
 if(!rc){if(clock_ms()-session.since>=20000)finish("error","Request outcome unknown; not retried");return;}
 session.promise.reset();require(rc==PGR_VR_DONE&&result,"Request failed; batch stopped without retry");
 if(session.stage==LIST){
  if(session.canceled){finish("stopped","Batch stopped");return;}
  session.enumerator=enumerate(result);require(bool(session.enumerator),"Friend enumeration unavailable");session.stage=DISCOVER;return;
 }
 const char *type=session.stage==SEND_CHECK?"CheckSendGiftOutProto":session.stage==SEND?"SendGiftOutProto":"OpenGiftOutProto";
 require(klass(result.get())==cls("Holoholo.Rpc",type),"Gift response identity mismatch");
 bool ok=success(result.get());std::string reason=ok?"Success":result_name(result.get());
 if(session.stage==SEND_CHECK){
  if(ok){session.send_authorized=true;session.stage=SEND;}
  else{++session.skipped;++session.friend_cursor;}
 }else if(session.stage==SEND){
  if(ok){++session.sent;++session.gift_cursor;}else{++session.skipped;if(reason=="ErrorGiftDoesNotExist")++session.gift_cursor;}
  ++session.friend_cursor;session.send_authorized=false;session.stage=SEND_CHECK;
 }else{
  if(ok)++session.opened;else ++session.skipped;
  ++session.open_cursor;
  if(reason=="ErrorPlayerLimitReached"||reason=="ErrorPlayerBagFull"){
   finish("done",reason=="ErrorPlayerLimitReached"?"Daily opening limit reached":"Opening stopped: item bag full");return;
  }
 }
 session.next=clock_ms()+700;
 event("running",ok?"Gift action confirmed":"Skipped: "+reason);
 if(session.canceled)finish("stopped","Batch stopped");
}
bool manual_request_pending(){
 auto og=static_cast<const PgrOutgoingGiftsApi*>(pgrazor_outgoing_gifts_api(1));PgrOutgoingGiftsSample a{};
 if(og&&!og->sample(&a)&&a.active)return true;
 auto ig=static_cast<const PgrIncomingGiftsApi*>(pgrazor_incoming_gifts_api(1));PgrIncomingGiftsSample b{};
 return ig&&!ig->sample(&b)&&b.owned&&b.attempted;
}
void advance(){
 if(session.stage==DISCOVER){discover();return;}
 if(session.stage==SEND_CHECK||session.stage==SEND){
  if(session.friend_cursor>=session.recipients.size()||session.gift_cursor>=session.sendable.size()){session.stage=OPEN;event("running","Opening received gifts");return;}
  auto &f=session.recipients[session.friend_cursor];Root id(fn<void*(*)(const char*)>(boot.runtime.string_new)(f.id.c_str()));
  Root nia(fn<void*(*)(const char*)>(boot.runtime.string_new)((f.nia.empty()?f.id:f.nia).c_str()));
  if(session.stage==SEND_CHECK){void *args[]={nia.get()};submit(SEND_CHECK,session.rpc.get(),"CheckGiftingStatus",1,args);}
  else{require(session.send_authorized,"Missing gift eligibility");bool override_stamp=false;uint64_t gift=session.sendable[session.gift_cursor];void *args[]={&gift,id.get(),nia.get(),nullptr,&override_stamp};submit(SEND,session.rpc.get(),"SendGift",5,args);}
  return;
 }
 if(session.stage==OPEN){
  if(session.open_cursor>=session.received.size()){finish("done","Gift batch complete");return;}
  if(!session.command.full_bag){Root bag=resolve("Niantic.Holoholo.Services","IItemBagService");require(bool(bag),"Item capacity unavailable");if(boxed<bool>(call(bag.get(),"get_IsFull"),"System.Boolean")){finish("done","Opening stopped: item bag full");return;}}
  auto &g=session.received[session.open_cursor];Root id(fn<void*(*)(const char*)>(boot.runtime.string_new)(g.sender.c_str()));
  Root nia(fn<void*(*)(const char*)>(boot.runtime.string_new)((g.nia.empty()?g.sender:g.nia).c_str()));
  void *args[]={&g.id,id.get(),nia.get()};submit(OPEN,session.rpc.get(),"OpenGift",3,args);
 }
}
}
void pgr_gift_batch_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid))return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 try{
  auto p=permission();
  if(!session.active){
   if(!p.valid||p.command.token==finished_token)return;
   session.command=p.command;
   if(!p.command.enabled||!p.allowed){finish("stopped","Batch stopped before starting");return;}
   if(manual_request_pending())return;
   owner=tid;md=metadata;require(md,"Shared metadata unavailable");
   auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(api&&api->snapshot(&boot)==0,"Runtime unavailable");
   session.active=true;session.started=clock_ms();
   session.player=resolve("Niantic.Holoholo","IPlayerService");require(bool(session.player),"Player service unavailable");
   session.account=string(call(session.player.get(),"get_PlayerId"));require(!session.account.empty(),"Account identity unavailable");
   session.friends=resolve("Niantic.Holoholo.Social","IFriendsRpcService");session.gifting=resolve("Niantic.Holoholo.Social.Gifting","IGiftingService");session.rpc=resolve("Niantic.Holoholo.Social.Gifting","IGiftingRpcService");
   require(session.friends&&session.gifting&&session.rpc,"Gift services unavailable");
   submit(LIST,session.friends.get(),"GetFriendsListWithCacheRefresh");event("running","Loading friends");return;
  }
  if(!p.valid||!p.allowed||!p.command.enabled||p.command.token!=session.command.token||clock_ms()-session.started>=1800000)session.canceled=true;
  require(string(call(session.player.get(),"get_PlayerId"))==session.account,"Account changed; batch stopped");
  if(session.promise){poll();return;}
  if(session.canceled){finish("stopped","Batch stopped");return;}
  if(clock_ms()<session.next)return;
  advance();
 }catch(const Failure &e){finish("error",e.where);}catch(...){finish("error","Gift batch stopped: native failure");}
}
void pgr_gift_batch_stop(){
 if(owner&&owner!=uint64_t(gettid()))return;
 session.canceled=true;if(!busy&&session.active)finish("stopped","Gift batch stopped");
}

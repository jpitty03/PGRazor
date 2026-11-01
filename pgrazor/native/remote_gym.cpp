#include "remote_gym.h"
#include "remote_gym_policy.h"
#include "gym_automation.h"
#include "gym_feed_binding.h"
#include "shared_metadata.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "vgp_results.h"
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
using namespace pgr_remote_gym;
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
std::string decline_reason(void *o){
 auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),"Result");require(f,"Result");
 auto k=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));int result=result_value(o);
 const char *names[]={"ErrorCannotUse","ErrorNotInRange","ErrorPokemonNotThere","ErrorPokemonFull","ErrorNoBerriesLeft","ErrorWrongTeam","ErrorWrongCount","ErrorTooFast","ErrorTooFrequent","ErrorGymBusy","ErrorRaidActive","ErrorGymClosed"};
 const char *messages[]={"Berry cannot be used","Remote feeding unavailable","Defender no longer there","Defender is full","No berries left","Wrong team","Inventory changed","Feeding too fast","Feeding limit reached","Gym busy","Active raid","Gym closed"};
 for(unsigned i=0;i<12;i++)if(result==literal(k,names[i]))return messages[i];
 return "result "+std::to_string(result);
}
int count(Root &list,int cap){int n=boxed<int>(call(list.get(),"get_Count"),"System.Int32");require(n>=0&&n<=cap,"collection bound");return n;}
Root item(Root &list,int index){void *args[]={&index};return Root(call(list.get(),"get_Item",1,args));}
void *virtual_call(Root &o,void *m,void **args=nullptr){void *v=fn<void*(*)(void*,void*)>(exp("object_get_virtual_method"))(o.get(),m);require(v,"virtual method");return invoke(v,o.get(),args);}
struct Permission {Command command;double lat=0,lon=0;bool allowed=false,valid=false;};
Permission permission(){
 Permission p;auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));if(!tr)return p;
 PgrRuntimeTransportSample before{},after{};if(tr->sample(&before)||!before.snapshot_fresh||before.fault)return p;
 char text[97]{};uint32_t len=0;int active=0,ready=0,has=0;
 if(tr->get_string(12,text,sizeof(text),&len)||!parse(text,len,p.command))return p;
 tr->get_bool(3,&active);tr->get_bool(16,&ready);
 bool coords=tr->get_double(0,&p.lat,&has)==0&&has;has=0;coords=tr->get_double(1,&p.lon,&has)==0&&has&&coords;
 if(tr->sample(&after)||!after.snapshot_fresh||after.fault||before.snapshot_seq!=after.snapshot_seq)return p;
 p.valid=true;p.allowed=active&&ready&&coords&&std::isfinite(p.lat)&&std::isfinite(p.lon)&&std::abs(p.lat)<=90&&std::abs(p.lon)<=180;return p;
}
struct Target {std::string gym;uint64_t anchor=0,id=0;};
struct Session {
 Command command;std::string account;Root bag,enumerator,promise,reflection;
 std::vector<Target> targets;std::set<std::string> visited;
 size_t cursor=0;unsigned scanned=0,berries=0,declined=0;int64_t dust=0;
 std::string last_decline;
 uint64_t since=0,next=0,started=0;int berry=0,stock=0;
 bool active=false,canceled=false,await_stock=false,uncertain=false,stock_seen=false;
};
Session session;
std::string quote(const std::string &s){std::string out="\"";for(unsigned char c:s){if(c=='"'||c=='\\')out+='\\';if(c>=32)out+=char(c);}return out+'"';}
void event(const char *phase,const std::string &message){
 auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));if(!tr)return;
 std::string json="{\"kind\":\"remote-gym\",\"token\":"+quote(std::to_string(session.command.token))+",\"phase\":"+quote(phase)+",\"message\":"+quote(message)+",\"berries\":"+std::to_string(session.berries)+",\"dust\":"+std::to_string(session.dust)+",\"gyms\":"+std::to_string(session.visited.size())+"}";
 PgrRuntimeTransportString strings[]={{"remote-gym",10},{json.c_str(),uint32_t(json.size())}};
 if(tr->emit(19,nullptr,0,strings,2)!=0)session.canceled=true;
}
void finish(const char *phase,const std::string &message){
 if(session.uncertain&&session.cursor<session.targets.size()){
  auto history=pgr_gym_remote_history(session.account);
  if(history)history->refuse(session.targets[session.cursor].id,clock_ms());
 }
 event(phase,message+" ("+std::to_string(session.berries)+" berries, "+std::to_string(session.dust)+" dust)");
 finished_token=session.command.token;
 if(session.enumerator){try{virtual_call(session.enumerator,h(H_INC_M_DISPOSE));}catch(...){}}
 session=Session{};pgr_gym_remote_hold(false);
}
std::string player(){Root service=resolve("Niantic.Holoholo","IPlayerService");require(bool(service),"Player service unavailable");return string(call(service.get(),"get_PlayerId"));}
int stock(int berry){Root bag=resolve("Niantic.Holoholo","IItemBag");require(bool(bag),"Item bag unavailable");void *args[]={&berry};return boxed<int>(call(bag.get(),"GetItemCount",1,args),"System.Int32");}
bool still_deployed(const Target &target){
 void *args[]={const_cast<uint64_t*>(&target.anchor)};Root pokemon(call(session.bag.get(),"GetPokemon",1,args));
 return pokemon&&klass(pokemon.get())==cls("Holoholo.Rpc","PokemonProto")&&string(field<void*>(pokemon.get(),"deployedFortId_","System.String"))==target.gym;
}
void send(const Permission &,pgr_gym::History &history){
 const Target &target=session.targets[session.cursor];
 Root bag=resolve("Niantic.Holoholo","IItemBag"),id(fn<void*(*)(const char*)>(boot.runtime.string_new)(target.gym.c_str()));
 require(bool(bag),"Item bag unavailable");
 void *feed=pgr_gym_feed::resolve(boot,bag.get());require(feed,"Gym item-bag feeding contract unavailable");
 auto final=permission();require(final.allowed&&final.command.mode==1&&final.command.token==session.command.token,"Session changed before feed");
 require(player()==session.account&&still_deployed(target),"Defender ownership changed");
 session.stock=stock(session.berry);require(session.stock>reserve_for(session.command,session.berry),"Berry reserve reached");
 require(history.feed_attempt(target.id,clock_ms()),"Feed limit reached");
 session.uncertain=true;session.stock_seen=false;session.since=clock_ms();uint64_t pokemon=target.id;
 void *args[]={&session.berry,&pokemon,id.get()};
 session.promise=Root(invoke(feed,bag.get(),args));require(bool(session.promise),"Feed outcome unknown; batch stopped");
 try{pgr_gym_feed::observe_stock(session.stock_seen,session.stock,stock(session.berry));}catch(...){}
 event("running","Feeding remote gym");
}
void discover(){
 // Enumerate incrementally; never block Unity on a large inventory.
 for(unsigned i=0;i<64;i++){
  if(!boxed<bool>(virtual_call(session.enumerator,h(H_INC_M_MOVE)),"System.Boolean")){
   virtual_call(session.enumerator,h(H_INC_M_DISPOSE));session.enumerator.reset();
   event("running","Found "+std::to_string(session.targets.size())+" deployed defenders");return;
  }
  require(++session.scanned<=50000,"Pokemon inventory exceeded bound");
  Root pokemon(virtual_call(session.enumerator,h(H_INC_M_CURRENT)));if(!pokemon)continue;
  require(klass(pokemon.get())==cls("Holoholo.Rpc","PokemonProto"),"Pokemon inventory identity");
  std::string gym=string(field<void*>(pokemon.get(),"deployedFortId_","System.String"));if(gym.empty())continue;
  uint64_t id=field<uint64_t>(pokemon.get(),"id_","System.UInt64");if(!id)continue;
  require(session.targets.size()<100,"Deployed defender bound exceeded");session.targets.push_back({gym,id,id});
 }
}
void expand_defenders(void *response,pgr_gym::History &history){
 Target anchor=session.targets[session.cursor];
 Root details(field<void*>(response,"GymStatusAndDefenders","Holoholo.Rpc.GymStatusAndDefendersProto"));
 if(!details)return;
 void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(details.get()),"GymDefender");require(f,"Gym defender list");
 void *ft=fn<void*(*)(void*)>(boot.meta.field_get_type)(f),*k=fn<void*(*)(void*)>(boot.meta.class_from_type)(ft);
 require(k&&!fn<bool(*)(void*)>(boot.meta.class_is_valuetype)(k)&&typename_of(ft).find("Niantic.Protobuf.Collections.RepeatedField")==0,"Gym defender collection identity");
 void *value=nullptr;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(details.get(),f,&value);Root rows(value);if(!rows)return;
 int n=count(rows,6);
 for(int i=0;i<n;i++){
  Root row=item(rows,i);require(row&&klass(row.get())==cls("Holoholo.Rpc","GymDefenderProto"),"Defender identity");
  Root motivated(field<void*>(row.get(),"MotivatedPokemon","Holoholo.Rpc.MotivatedPokemonProto"));if(!motivated)continue;
  Root pokemon(field<void*>(motivated.get(),"Pokemon","Holoholo.Rpc.PokemonProto"));if(!pokemon)continue;
  uint64_t id=field<uint64_t>(pokemon.get(),"id_","System.UInt64");if(!id)continue;
  if(field<int64_t>(motivated.get(),"FeedCooldownDurationMillis","System.Int64")>0)history.refuse(id,clock_ms());
  bool found=false;for(const auto &target:session.targets)if(target.id==id){found=true;break;}
  if(!found){require(session.targets.size()<256,"Defender batch bound");session.targets.push_back({anchor.gym,anchor.anchor,id});}
 }
}
void poll(pgr_gym::History &history){
 auto now=clock_ms();auto vr=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));require(vr,"Promise observer unavailable");
 PgrVgpResultsResult out{};int rc=vr->poll(session.promise.get(),1,owner,&out);
 if(!rc){if(now-session.since>15000){history.refuse(session.targets[session.cursor].id,now);finish("error","Feed outcome unknown; not retried");}return;}
 Root response;if(out.value_handle)response.h=out.value_handle;
 session.promise.reset();session.reflection.reset();
 if(rc!=PGR_VR_DONE){history.refuse(session.targets[session.cursor].id,now);finish("error","Feed request rejected; batch stopped");return;}
 require(response&&klass(response.get())==cls("Holoholo.Rpc","GymFeedPokemonOutProto"),"Feed response identity mismatch");
 if(success(response.get())){
  int dust=field<int>(response.get(),"StardustAwarded","System.Int32");require(dust>=0&&dust<=100000,"Invalid dust reward");
  ++session.berries;session.dust+=dust;session.visited.insert(session.targets[session.cursor].gym);
  session.await_stock=!session.stock_seen;session.since=now;session.next=now+1500;
  int64_t deadline=field<int64_t>(response.get(),"CooldownComplete","System.Int64"),wall=int64_t(clock_ms(CLOCK_REALTIME));
  if(deadline>wall){require(deadline-wall<=1800000,"Invalid feed deadline");session.next=std::max(session.next,now+uint64_t(deadline-wall));}
  event("running","Berry accepted: "+std::to_string(session.berries)+" berries, "+std::to_string(session.dust)+" dust");
 }else{
  // A definite rejection consumed no berry. Skip it for this batch, while
  // retaining the attempted-request count. A new manual batch may try again
  // after a temporary raid/closure ends; uncertain outcomes stay quarantined.
  session.uncertain=false;++session.declined;session.last_decline=decline_reason(response.get());
  ++session.cursor;session.next=now+1500;event("running","Skipped: "+session.last_decline);
 }
 session.uncertain=false;
 // Accepted responses supply the other defenders in this
 // owned gym. Keep the player's deployed anchor as the permission for every feed.
 if(success(response.get()))expand_defenders(response.get(),history);
 if(session.canceled)finish("stopped","Batch stopped");
}
}
void pgr_remote_gym_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid))return;
 try{
  auto p=permission();
  if(!session.active){
   if(!p.valid||!p.allowed||p.command.mode!=1||p.command.token==finished_token)return;
   owner=tid;md=metadata;require(md,"Shared metadata unavailable");
   auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(api&&api->snapshot(&boot)==0,"Runtime unavailable");
   session.active=true;session.command=p.command;session.started=clock_ms();session.account=player();require(!session.account.empty(),"Account identity unavailable");
   session.bag=resolve("Niantic.Holoholo","IPokemonBag");require(bool(session.bag),"Pokemon bag unavailable");
   Root all(call(session.bag.get(),"get_AllPokemon"));require(bool(all),"Pokemon collection unavailable");
   session.enumerator=Root(virtual_call(all,h(H_INC_M_ENUMERATE)));require(bool(session.enumerator),"Pokemon enumerator unavailable");event("running","Finding deployed defenders");
  }
  pgr_gym_remote_hold(true);
  if(!p.valid||!p.allowed||p.command.mode!=1||p.command.token!=session.command.token||clock_ms()-session.started>=1800000)session.canceled=true;
  if(player()!=session.account){finish("stopped","Account changed; any submitted feed is not retried");return;}
  auto history=pgr_gym_remote_history(session.account);if(!history)return;
  if(session.promise){poll(*history);return;}
  if(session.canceled){finish("stopped","Batch stopped");return;}
  if(session.enumerator){discover();return;}
  if(session.await_stock){
   if(stock(session.berry)>=session.stock){if(clock_ms()-session.since>15000)finish("error","Waiting for inventory update; batch stopped");return;}
   session.await_stock=false;
  }
  if(clock_ms()<session.next)return;
  while(session.cursor<session.targets.size()&&(!history->feed_allowed(session.targets[session.cursor].id,clock_ms())||!still_deployed(session.targets[session.cursor])))++session.cursor;
  if(session.cursor==session.targets.size()){
   finish("done",session.targets.empty()?"No deployed defenders found":session.declined?"Batch complete; "+std::to_string(session.declined)+" skipped. Last: "+session.last_decline:"Batch complete; defender limits reached");return;
  }
  session.berry=select_berry(session.command,stock(703),stock(701),session.command.pinap_enabled?stock(705):0);
  if(!session.berry){finish("done","Berry reserves reached");return;}
  send(p,*history);
 }catch(const Failure &e){if(!session.command.token)session.command=permission().command;finish("error",e.where);}catch(...){finish("error","Remote feeding failed; batch stopped");}
}
void pgr_remote_gym_stop(uint64_t tid){if(owner&&owner==tid&&tid==uint64_t(gettid()))finish("stopped","Runtime stopped; submitted requests are not retried");}

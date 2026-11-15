#include "shiny_scan.h"
#include "shiny_scan_policy.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include "vgp_marker.h"
#include "vgp_results.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>
// Shiny Scanner (see shiny_scan.h). Every call goes through the game's own objects on the Unity thread:
// WildMapPokemon.SendEncounterRequest for the check and the encounter response the game keeps on the map
// Pokemon for the answer. Only while the game is active and the map is showing.
namespace {
using namespace pgr_shiny_scan;
using namespace pgr_shared_meta;
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
 static std::map<std::string,void*> cache;const std::string key=std::string(ns)+"."+name;auto it=cache.find(key);if(it!=cache.end())return it->second;
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api,"metadata API");
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult out{};require(api->resolve_class(&q,&out)==0,name);return cache[key]=const_cast<void*>(out.klass);
}
std::string typename_of(void *type){char *p=fn<char*(*)(void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
void *method(void *k,const char *name,int count){
 void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,count);require(m,name);
 require(fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(m)==unsigned(count),name);return m;
}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(klass(o),name,n),o,args);}
template<class T>T unbox(void *o){require(o,"null result");T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");std::memcpy(&v,p,sizeof(v));return v;}
template<class T>T at(void *o,size_t off){T v{};std::memcpy(&v,static_cast<char*>(o)+off,sizeof(v));return v;}
void *field_info(void *o,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name);require(f,name);require(!(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10),name);return f;}
template<class T>T field(void *o,const char *name,const char *type){
 void *f=field_info(o,name);require(typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))==type,name);
 T v{};fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;
}
// The name of an enum field's value, read from the game's own enum.
std::string enum_name(void *o,const char *name){
 void *f=field_info(o,name);void *k=fn<void*(*)(void*)>(boot.meta.class_from_type)(fn<void*(*)(void*)>(boot.meta.field_get_type)(f));
 require(k&&fn<bool(*)(void*)>(exp("class_is_enum"))(k),name);int value=0;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&value);
 void *iter=nullptr;
 while(auto e=fn<void*(*)(void*,void**)>(exp("class_get_fields"))(k,&iter)){
  if((fn<int(*)(void*)>(boot.meta.field_get_flags)(e)&0x8050)!=0x8050)continue;int n=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(e,&n);
  if(n==value)return fn<const char*(*)(void*)>(exp("field_get_name"))(e);}
 return "Unknown "+std::to_string(value);
}
Root type_object(void *k){return Root(fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k)));}
// The offset of the one instance field of this type on WildMapPokemon (its names change with the game).
size_t offset_by_type(void *wild_class,const char *type){
 void *iter=nullptr;int found=0;size_t offset=0;
 while(auto f=fn<void*(*)(void*,void**)>(exp("class_get_fields"))(wild_class,&iter)){
  if(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10)continue;
  if(typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))!=type)continue;
  offset=fn<size_t(*)(void*)>(exp("field_get_offset"))(f);++found;
 }
 require(found==1&&offset>=0x10&&offset<0x1000,type);return offset;
}
std::string quote(const std::string &s){std::string out="\"";for(unsigned char c:s){if(c=='"'||c=='\\')out+='\\';if(c>=32)out+=char(c);}return out+'"';}
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}

struct Config {Command command;bool valid=false,active=false,map=false;double lat=0,lon=0;};
Config configured(){
 Config c;auto tr=transport();PgrRuntimeTransportSample a{};static char s[4200];uint32_t n=0;
 if(!tr||tr->sample(&a)||!a.snapshot_fresh||a.fault||tr->get_string(23,s,sizeof(s)-1,&n)||n>=sizeof(s))return c;
 if(!parse_command(s,n,c.command))return c;
 int active=0,ready=0,map=0,has_lat=0,has_lon=0;tr->get_bool(3,&active);tr->get_bool(16,&ready);tr->get_bool(38,&map);
 if(tr->get_double(0,&c.lat,&has_lat)||tr->get_double(1,&c.lon,&has_lon)||!has_lat||!has_lon)c.lat=c.lon=0;
 c.valid=true;c.active=active&&ready;c.map=map!=0;return c;
}

constexpr uint64_t kScanMs=700,kAnswerMs=15000,kStatusMs=2000;
constexpr unsigned kFreeReadsPerScan=12,kCountsPerScan=40;
constexpr uint64_t kMarkMs=100,kBerryMs=10000,kThrowMs=15000;
constexpr int kMaxThrows=10;
enum CatchStep {C_NONE,C_BERRY,C_BERRY_WAIT,C_THROW_WAIT};
struct Pending {uint64_t id=0,at=0;Root wild,promise;};
struct State {
 std::vector<Pending> pending;Seen seen,counted;Pace pace;
 // The latest shiny found, kept so its place on the screen can be followed until it leaves the map.
 Root shiny;uint64_t next_mark=0;bool marked=false;
 // Its open encounter: the id, and what a throw needs once the map Pokemon is gone.
 // opened: the player's Catch it tapped it; expires_ms: when it is due to leave the map (0 = unknown).
 bool held=false,gone_told=false,opened=false;int64_t expires_ms=0;uint64_t shiny_id=0,catch_seq=0,catch_since=0,catch_next=0;int species=0,throws=0;CatchStep step=C_NONE;
 Root rpc,spawn,promise,items,berry_reflection,catch_reflection;void *berry_method=nullptr,*catch_method=nullptr;
 uint64_t session=0,next_scan=0,status_at=0,stopped_session=0;std::string status;
 void *wild_class=nullptr,*get_id=nullptr,*finder=nullptr;size_t response_offset=0,proto_offset=0;
} st;

void event(const std::string &body){
 auto tr=transport();if(!tr)return;
 const std::string json="{\"session\":"+std::to_string(st.session)+","+body+"}";
 PgrRuntimeTransportString s[]={{"shiny-scan",10},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,s,2);
}
void status(const std::string &message,uint64_t now,bool force=false){
 if(message==st.status||(!force&&now-st.status_at<kStatusMs))return;
 st.status=message;st.status_at=now;event("\"phase\":\"status\",\"message\":"+quote(message));
}
std::string sent_text(bool sent){return sent?",\"sent\":true":",\"sent\":false";}
void failed(uint64_t id,bool sent,const std::string &message){
 event("\"phase\":\"failed\",\"id\":\""+std::to_string(id)+"\""+sent_text(sent)+",\"message\":"+quote(message));
}
// Reads the answer the game keeps on the map Pokemon. False when the game refused the encounter.
void show_mark(bool visible,double x,double y){
 auto tr=transport();if(!tr)return;
 int64_t scalars[3]={visible?1:0,0,0};std::memcpy(&scalars[1],&x,8);std::memcpy(&scalars[2],&y,8);
 tr->emit(59,scalars,3,nullptr,0);st.marked=visible;
}
void clear_mark(){st.shiny.reset();if(st.marked)show_mark(false,0,0);}
// Lets the held shiny go: nothing more is thrown at it and checking may go on.
void release(){clear_mark();st.held=st.gone_told=st.opened=false;st.expires_ms=0;st.shiny_id=0;st.step=C_NONE;st.throws=0;st.promise.reset();st.rpc.reset();st.spawn.reset();}
struct Vec3 {float x,y,z;};
struct Bounds {Vec3 center,extents;};
// The top of the Pokemon's tappable volume on the screen (0..1 each way, y down), so the symbol sits just
// above its head at any zoom. Checked against the VGP icon's own projection of the same Pokemon (base_x,
// base_y): when the two disagree this is not the camera the map is drawn with, and the answer is refused.
bool head_point(void *wild,double base_x,double base_y,double &x,double &y){
 try{
  Root type=type_object(cls("UnityEngine","Collider"));void *args[]={type.get()};
  Root collider(call(wild,"GetComponentInChildren",1,args));if(!collider)return false;
  const Bounds b=unbox<Bounds>(call(collider.get(),"get_bounds"));
  Root camera(invoke(method(cls("UnityEngine","Camera"),"get_main",0),nullptr));if(!camera)return false;
  const int w=unbox<int32_t>(call(camera.get(),"get_pixelWidth")),h=unbox<int32_t>(call(camera.get(),"get_pixelHeight"));
  if(w<16||h<16)return false;
  auto project=[&](Vec3 p,double &px,double &py){
   void *a[]={&p};const Vec3 s=unbox<Vec3>(call(camera.get(),"WorldToScreenPoint",1,a));
   if(!(s.z>0)||!std::isfinite(s.x)||!std::isfinite(s.y))return false;
   px=s.x/w;py=1.0-s.y/h;return true;
  };
  Root transform(call(wild,"get_transform"));if(!transform)return false;
  const Vec3 base=unbox<Vec3>(call(transform.get(),"get_position"));
  double bx=0,by=0;if(!project(base,bx,by)||std::fabs(bx-base_x)>0.03||std::fabs(by-base_y)>0.03)return false;
  const float top=b.center.y+b.extents.y;if(!std::isfinite(top)||top<base.y)return false;
  return project(Vec3{base.x,top,base.z},x,y)&&y<=by+0.001&&by-y<0.6;
 }catch(const Failure&){return false;}
}
// Where the found shiny is on the screen, through the same projection the VGP target icon uses.
void mark(uint64_t now){
 if(!st.shiny.h||now<st.next_mark)return;
 st.next_mark=now+kMarkMs;
 void *wild=st.shiny.get();
 if(!wild||unbox<bool>(call(wild,"get_Destroyed"))){
  if(!st.held){clear_mark();return;}
  // Gone after the player opened it, or well before it was due to leave: it was caught or it fled, so
  // there is nothing left to hold. Otherwise it left the map on time and its encounter is still open.
  timespec wall{};clock_gettime(CLOCK_REALTIME,&wall);const int64_t wall_ms=int64_t(wall.tv_sec)*1000+wall.tv_nsec/1000000;
  if(st.opened||(st.expires_ms&&wall_ms+15000<st.expires_ms)){release();event("\"phase\":\"closed\"");return;}
  clear_mark();
  if(!st.gone_told){st.gone_told=true;event("\"phase\":\"gone\",\"id\":\""+std::to_string(st.shiny_id)+"\"");}
  return;
 }
 const auto *api=static_cast<const PgrVgpMarkerApi*>(pgrazor_vgp_marker_api(1));
 PgrVgpMarkerResult r{};r.version=1;r.size=sizeof(r);
 const bool visible=api&&api->project&&api->project(wild,2500,owner,&r)==1&&r.visible&&std::isfinite(r.x)&&std::isfinite(r.y);
 if(!visible){if(st.marked)show_mark(false,0,0);return;}
 // Without a head point the symbol goes a little above the Pokemon's feet.
 double x=r.x,y=r.y-0.035;if(!head_point(wild,r.x,r.y,x,y)){x=r.x;y=r.y-0.035;}
 show_mark(true,x,y<0?0:y);
}
bool read_answer(void *wild,uint64_t id,bool sent,uint64_t took_ms){
 Root response(at<void*>(wild,st.response_offset));require(bool(response),"encounter response missing");
 const std::string name=enum_name(response.get(),"Status");
 if(name!="EncounterSuccess"){failed(id,sent,"Encounter refused: "+name);return false;}
 Root proto(field<void*>(response.get(),"Pokemon","Holoholo.Rpc.WildPokemonProto"));require(bool(proto),"encounter Pokemon");
 Root pokemon(field<void*>(proto.get(),"Pokemon","Holoholo.Rpc.PokemonProto"));require(bool(pokemon),"encounter Pokemon");
 Root display(call(pokemon.get(),"get_PokemonDisplay"));require(bool(display),"Pokemon display");
 const bool shiny=field<uint8_t>(display.get(),"Shiny","System.Boolean")!=0;
 const int species=unbox<int32_t>(call(pokemon.get(),"get_PokemonId")),cp=unbox<int32_t>(call(pokemon.get(),"get_Cp"));
 const int a=unbox<int32_t>(call(pokemon.get(),"get_IndividualAttack")),d=unbox<int32_t>(call(pokemon.get(),"get_IndividualDefense")),s=unbox<int32_t>(call(pokemon.get(),"get_IndividualStamina"));
 require(species>0&&cp>0,"encounter Pokemon not filled in");
 char place[96]{};std::snprintf(place,sizeof(place),",\"lat\":%.7f,\"lon\":%.7f",field<double>(proto.get(),"Latitude","System.Double"),field<double>(proto.get(),"Longitude","System.Double"));
 event("\"phase\":\"checked\",\"id\":\""+std::to_string(id)+"\""+sent_text(sent)+",\"shiny\":"+(shiny?"true":"false")+",\"species\":"+std::to_string(species)+
  ",\"cp\":"+std::to_string(cp)+",\"iv\":["+std::to_string(a)+","+std::to_string(d)+","+std::to_string(s)+"]"+place+",\"ms\":"+std::to_string(took_ms));
 if(shiny&&st.step==C_NONE){
  release();st.shiny=Root(wild);st.next_mark=0;st.shiny_id=id;st.species=species;st.held=true;
  // Kept for a throw after the map Pokemon is gone; without them it can only be caught while on the map.
  try{st.rpc=Root(field<void*>(wild,"rpcHandler","Niantic.Holoholo.Rpc.IRpcHandler"));st.spawn=Root(call(wild,"get_SpawnPointId"));}
  catch(const Failure&){st.rpc.reset();st.spawn.reset();}
  try{
   const int64_t modified=field<int64_t>(proto.get(),"LastModifiedMs","System.Int64");const int left=field<int32_t>(proto.get(),"TimeTillHiddenMs","System.Int32");
   st.expires_ms=modified>0&&left>0&&left<3600000?modified+left:0;
  }catch(const Failure&){st.expires_ms=0;}
 }
 return true;
}
// ---- Catching the held shiny (the same calls Shundo Sniper makes) ----
void *h(unsigned id){return const_cast<void*>(md->opt_handles[id]);}
Root resolve(void *contract){
 Root type=type_object(h(H_IL_MAP));void *args[]={type.get()};Root map(invoke(h(H_IL_FIND_M),nullptr,args));if(!map)return {};
 Root container(invoke(h(H_IL_CONTAINER_M),map.get()));if(!container)return {};
 Root target=type_object(contract);void *resolve_args[]={target.get()};return Root(invoke(h(H_IL_RESOLVE_M),container.get(),resolve_args));
}
int literal(void *k,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f,name);require((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)==0x8050,name);int n=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&n);return n;}
std::string field_type(void *o,const char *name){return typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(field_info(o,name)));}
template<class T>void put(void *o,const char *name,const char *type,T value){require(field_type(o,name)==type,name);fn<void(*)(void*,void*,void*)>(boot.runtime.field_set_value)(o,field_info(o,name),&value);}
void put_object(void *o,const char *name,const char *type,void *value){require(field_type(o,name)==type,name);fn<void(*)(void*,void*,void*)>(exp("field_set_value_object"))(o,field_info(o,name),value);}
void cooldown(const char *action){auto tr=transport();if(!tr)return;PgrRuntimeTransportString s[]={{action,uint32_t(std::strlen(action))}};tr->emit(17,nullptr,0,s,1);}
void catch_event(const char *result,const std::string &message){
 event(std::string("\"phase\":\"catch\",\"result\":\"")+result+"\",\"species\":"+std::to_string(st.species)+",\"throws\":"+std::to_string(st.throws)+",\"message\":"+quote(message));
}
// The game's typed RPC handler closed over one request/response pair, made once per pair.
void *rpc_sender(const char *request,const char *response,Root &reflection,void *&closed_method){
 if(closed_method)return closed_method;
 require(bool(st.rpc)&&bool(st.spawn),"it has left the map and cannot be reached");
 void *open=method(klass(st.rpc.get()),"SendRpc",5);require(fn<bool(*)(void*)>(boot.meta.method_is_generic)(open),"generic RPC");
 Root reflected(fn<void*(*)(void*,void*)>(exp("method_get_object"))(open,klass(st.rpc.get())));
 Root types(fn<void*(*)(void*,uintptr_t)>(boot.runtime.array_new)(cls("System","Type"),2));require(reflected&&types,"RPC reflection");
 void *ks[]={cls("Holoholo.Rpc",request),cls("Holoholo.Rpc",response)};
 for(int i=0;i<2;i++){Root t=type_object(ks[i]);void **slot=reinterpret_cast<void**>(static_cast<char*>(types.get())+32+i*8);fn<void(*)(void*,void**,void*)>(exp("gc_wbarrier_set_field"))(types.get(),slot,t.get());}
 void *args[]={types.get()};reflection=Root(call(reflected.get(),"MakeGenericMethod",1,args));
 void *closed=fn<void*(*)(void*)>(exp("method_get_from_reflection"))(reflection.get());require(closed&&fn<bool(*)(void*)>(boot.meta.method_is_inflated)(closed),"closed RPC");
 return closed_method=closed;
}
Root new_request(const char *type){Root r(fn<void*(*)(void*)>(boot.runtime.object_new)(cls("Holoholo.Rpc",type)));require(bool(r),"request allocation");call(r.get(),".ctor");return r;}
Root send(void *sender,const char *method_name,void *request){
 int id=literal(cls("Holoholo.Rpc","Method"),method_name),zero=0,timeout=10000;void *args[]={&id,request,&zero,&timeout,&zero};
 return Root(invoke(sender,st.rpc.get(),args));
}
int count(int item){void *a[]={&item};const int n=unbox<int32_t>(call(st.items.get(),"GetItemCount",1,a));return n>0?n:0;}
int item_id(const char *name){return literal(cls("Holoholo.Rpc","Item"),name);}
bool on_map(){void *wild=st.shiny.get();return wild&&!unbox<bool>(call(wild,"get_Destroyed"));}
// One excellent curveball with the best ball on hand: Ultra, Great, then Poke Ball (never a Master Ball).
void throw_ball(uint64_t now){
 int ball=0;for(const char *name:{"UltraBall","GreatBall","PokeBall"}){const int id=item_id(name);if(count(id)){ball=id;break;}}
 if(!ball){catch_event("failed","No Poké, Great or Ultra Balls left");st.step=C_NONE;return;}
 if(on_map()){
  uint8_t pitch[12]{};const float reticle=0.1f;std::memcpy(pitch,&ball,4);std::memcpy(pitch+4,&reticle,4);pitch[8]=1;pitch[9]=1;
  void *args[]={pitch,nullptr};st.promise=Root(call(st.shiny.get(),"TryCapture",2,args));
 }else{
  void *sender=rpc_sender("CatchPokemonProto","CatchPokemonOutProto",st.catch_reflection,st.catch_method);
  Root request=new_request("CatchPokemonProto");
  put<uint64_t>(request.get(),"EncounterId","System.UInt64",st.shiny_id);
  put<int>(request.get(),"Pokeball","System.Int32",ball);
  put<double>(request.get(),"NormalizedReticleSize","System.Double",1.95);
  put_object(request.get(),"SpawnPointGuid","System.String",st.spawn.get());
  put<bool>(request.get(),"HitPokemon","System.Boolean",true);
  put<double>(request.get(),"SpinModifier","System.Double",0.85);
  put<double>(request.get(),"NormalizedHitPosition","System.Double",1.0);
  st.promise=send(sender,"CatchPokemon",request.get());
 }
 require(bool(st.promise),"throw not sent");
 cooldown("Shiny Scanner throw");++st.throws;st.step=C_THROW_WAIT;st.catch_since=now;
}
// A Golden Razz Berry, else a Razz Berry, before each throw; none on hand goes straight to the throw.
void feed_berry(uint64_t now){
 int berry=item_id("GoldenRazzBerry");if(!count(berry)){berry=item_id("RazzBerry");if(!count(berry))berry=0;}
 if(!berry||!st.rpc||!st.spawn){throw_ball(now);return;}
 void *sender=rpc_sender("UseItemEncounterProto","UseItemEncounterOutProto",st.berry_reflection,st.berry_method);
 Root request=new_request("UseItemEncounterProto");
 put<int>(request.get(),"Item","Holoholo.Rpc.Item",berry);put<uint64_t>(request.get(),"EncounterId","System.UInt64",st.shiny_id);
 put_object(request.get(),"SpawnPointGuid","System.String",st.spawn.get());
 st.promise=send(sender,"UseItemEncounter",request.get());require(bool(st.promise),"berry not sent");
 cooldown("Shiny Scanner berry");st.step=C_BERRY_WAIT;st.catch_since=now;
}
void read_catch(Root &response,uint64_t now){
 const std::string name=enum_name(response.get(),"Status");
 const std::string throws=std::to_string(st.throws)+(st.throws==1?" throw":" throws");
 if(name=="CatchSuccess"){catch_event("caught","Caught after "+throws);release();return;}
 if((name=="CatchEscape"||name=="CatchMissed")&&st.throws<kMaxThrows){st.step=C_BERRY;st.catch_next=now+1200;return;}
 if(name=="CatchFlee"){catch_event("fled","Fled after "+throws);release();return;}
 if(name=="CatchEscape"||name=="CatchMissed"){catch_event("failed","Still free after "+throws+"; Catch tries again");st.step=C_NONE;st.throws=0;return;}
 catch_event("failed","Catch refused: "+name);st.step=C_NONE;st.throws=0;
}
// One step of a catch the player asked for. A throw already sent is always followed to its answer.
void catch_step(const Config &c,uint64_t now){
 if(st.step==C_NONE){
  if(!c.command.catch_seq||c.command.catch_seq==st.catch_seq)return;
  st.catch_seq=c.command.catch_seq;
  if(!st.held){catch_event("failed","There is no shiny to catch any more");return;}
  // On the map: the game's own tap, so the encounter opens (or Auto throw catches it) as if the player
  // had tapped it. The normal tap body ignores its sender and event arguments.
  if(on_map()){
   if(!c.active||!c.map){catch_event("failed","Go back to the map first");return;}
   void *tap=method(cls("Niantic.Holoholo.Map","MapPokemon"),"OnTap",2);void *args[]={nullptr,nullptr};
   invoke(tap,st.shiny.get(),args);st.opened=true;catch_event("opened","Opened");return;
  }
  st.throws=0;st.step=C_BERRY;st.catch_next=now;catch_event("throwing","Catching");
 }
 if(st.step==C_BERRY_WAIT||st.step==C_THROW_WAIT){
  auto vr=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));require(vr,"response polling unavailable");
  PgrVgpResultsResult out{};const int rc=vr->poll(st.promise.get(),PGR_VR_PHASE_ENCOUNTER,owner,&out);Root value;value.h=out.value_handle;
  if(rc==PGR_VR_PENDING&&now-st.catch_since<=(st.step==C_BERRY_WAIT?kBerryMs:kThrowMs))return;
  st.promise.reset();
  if(st.step==C_BERRY_WAIT){throw_ball(now);return;}  // the throw goes ahead either way
  if(rc!=PGR_VR_DONE||!value){catch_event("failed",rc==PGR_VR_PENDING?"No answer to the throw":"Throw failed");st.step=C_NONE;st.throws=0;return;}
  read_catch(value,now);return;
 }
 if(st.step==C_BERRY&&now>=st.catch_next&&c.active){
  if(!st.items)st.items=resolve(cls("Niantic.Holoholo","IItemBag"));require(bool(st.items),"item bag unavailable");
  feed_berry(now);
 }
}
void note_failure(uint64_t now){if(st.pace.failed(now))status("Paused 30 s: no answers",now,true);}
// Follows the requests already sent to their answers.
void follow(uint64_t now){
 for(size_t i=0;i<st.pending.size();){
  Pending &p=st.pending[i];void *wild=p.wild.get();bool done=true;
  if(!wild){failed(p.id,true,"Left the map before it answered");}
  else if(at<void*>(wild,st.response_offset)){if(read_answer(wild,p.id,true,now-p.at))st.pace.answered();else note_failure(now);}
  else if(now-p.at>kAnswerMs){failed(p.id,true,"No answer");note_failure(now);}
  else done=false;
  if(done)st.pending.erase(st.pending.begin()+long(i));else ++i;
 }
}
bool is_pending(uint64_t id){for(const auto &p:st.pending)if(p.id==id)return true;return false;}
void scan(const Config &c,uint64_t now){
 if(!st.wild_class){
  st.wild_class=cls("Niantic.Holoholo.Map","WildMapPokemon");st.get_id=method(st.wild_class,"get_EncounterId",0);
  st.finder=method(cls("UnityEngine","Object"),"FindObjectsOfType",2);
  st.response_offset=offset_by_type(st.wild_class,"Holoholo.Rpc.EncounterOutProto");
  st.proto_offset=offset_by_type(st.wild_class,"Holoholo.Rpc.WildPokemonProto");
 }
 Root type=type_object(st.wild_class);bool inactive=false;void *args[]={type.get(),&inactive};
 Root list(invoke(st.finder,nullptr,args));if(!list)return;
 const uintptr_t n=at<uintptr_t>(list.get(),0x18);require(n<=5000,"map Pokemon count");
 unsigned nearby=0,waiting=0,free_reads=0,counts=0;
 for(uintptr_t i=0;i<n;++i){
  void *p=at<void*>(list.get(),0x20+i*8);if(!p||klass(p)!=st.wild_class)continue;
  const uint64_t id=unbox<uint64_t>(invoke(st.get_id,p));if(!id)continue;
  // The encounter counter: an answer on the map Pokemon means the game encountered it, whoever asked.
  if(c.command.count&&counts<kCountsPerScan&&at<void*>(p,st.response_offset)&&!st.counted.has(id,now)){
   st.counted.add(id,now);++counts;event("\"phase\":\"encounter\",\"id\":\""+std::to_string(id)+"\"");
  }
  if(!c.command.on||is_pending(id))continue;
  void *proto=at<void*>(p,st.proto_offset);if(!proto)continue;
  if(!in_range(c.lat,c.lon,field<double>(proto,"Latitude","System.Double"),field<double>(proto,"Longitude","System.Double")))continue;
  ++nearby;if(st.seen.has(id,now))continue;
  // The game already has this one's answer (the player or VGP+ opened it): reading it costs nothing.
  if(at<void*>(p,st.response_offset)){
   if(free_reads++>=kFreeReadsPerScan){++waiting;continue;}
   st.seen.add(id,now);read_answer(p,id,false,0);continue;
  }
  if(!c.command.species.empty()){
   void *pokemon=field<void*>(proto,"Pokemon","Holoholo.Rpc.PokemonProto");
   if(!pokemon||!wanted(c.command,unbox<int32_t>(call(pokemon,"get_PokemonId"))))continue;
  }
  // A held shiny keeps its encounter open: nothing else is asked about until it is caught or let go.
  if(st.step!=C_NONE||(c.command.hold&&st.held))continue;
  if(!st.pace.may_send(c.command,now,st.pending.size())){++waiting;continue;}
  if(unbox<bool>(call(p,"get_Destroyed")))continue;
  // Reserved before sending, so a request that goes wrong is never repeated for this Pokemon.
  st.seen.add(id,now);
  Pending request;request.id=id;request.at=now;request.wild=Root(p);
  request.promise=Root(call(p,"SendEncounterRequest"));require(bool(request.promise),"encounter not sent");
  st.pending.push_back(std::move(request));st.pace.sent(c.command,now);
 }
 if(!c.command.on||now<st.pace.paused_until)return;
 if(st.step!=C_NONE){status("Catching the shiny",now);return;}
 if(c.command.hold&&st.held){status("Holding the shiny",now);return;}
 if(!c.command.budget)status("24 h encounter limit reached",now);
 else if(!nearby)status("No wild Pokémon in range",now);
 else if(waiting||!st.pending.empty())status("Checking "+std::to_string(waiting+unsigned(st.pending.size()))+" of "+std::to_string(nearby)+" nearby",now);
 else status("All "+std::to_string(nearby)+" nearby checked",now);
}
void drop_pending(){for(const auto &p:st.pending)failed(p.id,true,"Stopped before it answered");st.pending.clear();}
}  // namespace

void pgr_shiny_scan_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||faulted||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid))return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 const Config c=configured();const uint64_t now=clock_ms();
 try{
  // Switching the scanner off lets a held shiny go, but never abandons a throw that is already out.
  if(!c.valid||!c.command.on){if(initialized&&!st.pending.empty())drop_pending();if(initialized&&st.step==C_NONE)release();st.status.clear();}
  if(!c.valid||(!c.command.on&&!c.command.count&&st.step==C_NONE))return;
  if(c.command.session==st.stopped_session)return;
  if(!initialized){
   auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);
   require(api&&api->snapshot(&boot)==0,"runtime unavailable");md=metadata;owner=tid;initialized=true;
  }
  require(fn<void*(*)()>(boot.runtime.thread_current)()!=nullptr,"owner thread not attached");
  // A new session (the player dismissed the shiny, or switched the scanner off and on) lets it go.
  if(c.command.session!=st.session&&st.step==C_NONE)release();
  st.session=c.command.session;
  require(md!=nullptr,"shared metadata");
  catch_step(c,now);
  // A request already sent is followed to its answer; nothing new starts off the map or in the background.
  if(!st.pending.empty())follow(now);
  if(c.active&&c.map)mark(now);else if(st.marked)show_mark(false,0,0);
  if(!c.active||!c.map||now<st.next_scan)return;
  st.next_scan=now+kScanMs;scan(c,now);
 }catch(const Failure &f){
  // A failed catch step is reported and leaves the scanner running; anything else stops it for this
  // session only (switching the scanner off and on tries again).
  if(st.step!=C_NONE){st.step=C_NONE;st.throws=0;st.promise.reset();catch_event("failed",std::string("Catch failed: ")+f.where);return;}
  st.stopped_session=c.command.session;st.pending.clear();st.shiny.reset();st.marked=false;st.held=false;
  event("\"phase\":\"error\",\"message\":"+quote(std::string("Shiny Scanner stopped: ")+f.where));
 }catch(...){faulted=true;st.pending.clear();event("\"phase\":\"error\",\"message\":\"Shiny Scanner stopped after an unexpected error\"");}
}
void pgr_shiny_scan_stop(uint64_t tid){
 if(!initialized||tid!=owner||tid!=uint64_t(gettid()))return;
 st.pending.clear();st.marked=false;release();st.items.reset();st.berry_reflection.reset();st.catch_reflection.reset();st.berry_method=st.catch_method=nullptr;st.seen.clear();st.counted.clear();st.pace=Pace{};st.status.clear();st.next_scan=0;
}

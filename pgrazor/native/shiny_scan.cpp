#include "shiny_scan.h"
#include "shiny_scan_policy.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
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
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
PgrBootBinding boot{};
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
struct Pending {uint64_t id=0,at=0;Root wild,promise;};
struct State {
 std::vector<Pending> pending;Seen seen,counted;Pace pace;
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
 return true;
}
void note_failure(uint64_t now){if(st.pace.failed(now))status("Paused for 30 s: the game is not answering checks",now,true);}
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
  if(!st.pace.may_send(c.command,now,st.pending.size())){++waiting;continue;}
  if(unbox<bool>(call(p,"get_Destroyed")))continue;
  // Reserved before sending, so a request that goes wrong is never repeated for this Pokemon.
  st.seen.add(id,now);
  Pending request;request.id=id;request.at=now;request.wild=Root(p);
  request.promise=Root(call(p,"SendEncounterRequest"));require(bool(request.promise),"encounter not sent");
  st.pending.push_back(std::move(request));st.pace.sent(c.command,now);
 }
 if(!c.command.on||now<st.pace.paused_until)return;
 if(!c.command.budget)status("Encounter limit for the last 24 hours reached",now);
 else if(!nearby)status("No wild Pokémon in range",now);
 else if(waiting||!st.pending.empty())status("Checking "+std::to_string(waiting+unsigned(st.pending.size()))+" of "+std::to_string(nearby)+" nearby",now);
 else status("All "+std::to_string(nearby)+" nearby checked",now);
}
void drop_pending(){for(const auto &p:st.pending)failed(p.id,true,"Stopped before it answered");st.pending.clear();}
}  // namespace

void pgr_shiny_scan_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 (void)metadata;
 if(busy||faulted||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid))return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 const Config c=configured();const uint64_t now=clock_ms();
 try{
  if(!c.valid||!c.command.on){if(initialized&&!st.pending.empty())drop_pending();st.status.clear();}
  if(!c.valid||(!c.command.on&&!c.command.count))return;
  if(c.command.session==st.stopped_session)return;
  if(!initialized){
   auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);
   require(api&&api->snapshot(&boot)==0,"runtime unavailable");owner=tid;initialized=true;
  }
  require(fn<void*(*)()>(boot.runtime.thread_current)()!=nullptr,"owner thread not attached");
  st.session=c.command.session;
  // A request already sent is followed to its answer; nothing new starts off the map or in the background.
  if(!st.pending.empty())follow(now);
  if(!c.active||!c.map||now<st.next_scan)return;
  st.next_scan=now+kScanMs;scan(c,now);
 }catch(const Failure &f){
  // Stops for this session only; switching the scanner off and on tries again.
  st.stopped_session=c.command.session;st.pending.clear();
  event("\"phase\":\"error\",\"message\":"+quote(std::string("Shiny Scanner stopped: ")+f.where));
 }catch(...){faulted=true;st.pending.clear();event("\"phase\":\"error\",\"message\":\"Shiny Scanner stopped after an unexpected error\"");}
}
void pgr_shiny_scan_stop(uint64_t tid){
 if(!initialized||tid!=owner||tid!=uint64_t(gettid()))return;
 st.pending.clear();st.seen.clear();st.counted.clear();st.pace=Pace{};st.status.clear();st.next_scan=0;
}

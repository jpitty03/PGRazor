// Standalone host/NDK fixture: compile this TU alone with -ldl -rdynamic.
// -rdynamic exposes the fake IL2CPP string export used by production dlsym.
// Never link or package this fixture into libpgrazor.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <map>
#include "gift_batch.cpp"

namespace {
std::map<uintptr_t,void*> roots;
uintptr_t next_root=1;
unsigned allocated=0,freed=0,polls=0,invocations=0,bootstraps=0;
int promise_object,response_object,player_object,string_object;
int response_class,enum_class,string_class,player_class,enum_type;
int result_field,success_field,bag_field,limit_field,other_field,player_method;
int result_code=1,poll_code=PGR_VR_DONE;
bool fresh=true,ready=true,changing=false;
unsigned samples=0;
std::string config="G1|42|1|0",account="account-a";
std::vector<std::string> events;
uintptr_t root_new(void *p,int pin){assert(p&&!pin);auto h=next_root++;roots[h]=p;++allocated;return h;}
void *root_get(uintptr_t h){assert(roots.count(h));return roots.at(h);}
void root_free(uintptr_t h){assert(roots.erase(h)==1);++freed;}
void *object_class(void *p){
 if(p==&response_object)return &response_class;
 if(p==&player_object)return &player_class;
 if(p==&string_object)return &string_class;
 assert(false&&"unexpected managed object");return nullptr;
}
void *lookup_field(void *k,const char *name){
 if(k==&response_class&&!strcmp(name,"Result"))return &result_field;
 if(k==&enum_class&&!strcmp(name,"Success"))return &success_field;
 assert(false&&"unexpected field lookup");return nullptr;
}
void *field_type(void *f){assert(f==&result_field);return &enum_type;}
void *type_class(void *p){assert(p==&enum_type);return &enum_class;}
int flags(void *f){return f==&result_field?0:0x8050;}
void static_value(void *f,void *out){
 *static_cast<int*>(out)=f==&success_field?1:f==&bag_field?2:f==&limit_field?3:4;
}
void read_value(void *o,void *f,void *out){assert(o==&response_object&&f==&result_field);*static_cast<int*>(out)=result_code;}
void *enum_fields(void *k,void **iter){
 assert(k==&enum_class);uintptr_t i=reinterpret_cast<uintptr_t>(*iter);
 void *fields[]={&success_field,&bag_field,&limit_field,&other_field};
 if(i==4)return nullptr;*iter=reinterpret_cast<void*>(i+1);return fields[i];
}
const char *field_name(void *f){return f==&success_field?"Success":f==&bag_field?"ErrorPlayerBagFull":f==&limit_field?"ErrorPlayerLimitReached":"ErrorGiftDoesNotExist";}
int resolve_class(const PgrMetaClassQuery *q,PgrMetaClassResult *out){
 if(!strcmp(q->name,"String"))out->klass=&string_class;
 else if(!strcmp(q->name,"SendGiftOutProto")||!strcmp(q->name,"OpenGiftOutProto")||!strcmp(q->name,"CheckSendGiftOutProto"))out->klass=&response_class;
 else {assert(false&&"unexpected class resolution");return -1;}
 return 0;
}
void *lookup_method(void *k,const char *name,int n){assert(k==&player_class&&!strcmp(name,"get_PlayerId")&&!n);return &player_method;}
unsigned param_count(void *m){assert(m==&player_method);return 0;}
void *invoke_method(void *m,void *o,void **,void **ex){assert(m==&player_method&&o==&player_object);*ex=nullptr;++invocations;return &string_object;}
const uint16_t *chars(void *o){assert(o==&string_object);static uint16_t buffer[64];assert(account.size()<64);for(size_t i=0;i<account.size();i++)buffer[i]=account[i];return buffer;}
int sample(PgrRuntimeTransportSample *out){*out={};out->snapshot_fresh=fresh;out->snapshot_seq=100+(changing?samples:0);++samples;return 0;}
int get_string(uint32_t slot,char *out,uint32_t cap,uint32_t *len){assert(slot==12&&config.size()<cap);*len=config.size();memcpy(out,config.data(),*len);return 0;}
int get_bool(uint32_t slot,int *out){assert(slot==12);*out=ready;return 0;}
int emit(uint32_t op,const int64_t*,uint32_t n,const PgrRuntimeTransportString *s,uint32_t count){assert(op==19&&!n&&count==2);assert(std::string(s[0].ptr,s[0].len)=="gift-batch");events.emplace_back(s[1].ptr,s[1].len);return 0;}
int poll_result(void *p,uint32_t phase,uint64_t tid,PgrVgpResultsResult *out){assert(p==&promise_object&&phase==1&&tid==owner);++polls;*out={};if(poll_code==PGR_VR_DONE)out->value_handle=root_new(&response_object,0);return poll_code;}
int bootstrap(PgrBootBinding*){++bootstraps;return -1;}
PgrRuntimeTransportApi transport{};PgrVgpResultsApi results{};PgrMetaApi metadata{};PgrBootApi bootstrap_api{};
void clean(){session=Session{};assert(roots.empty()&&allocated==freed);}
void fixture(Stage stage=SEND){
 clean();events.clear();polls=invocations=0;fresh=ready=true;changing=false;samples=0;
 config="G1|42|1|0";account="account-a";result_code=1;poll_code=PGR_VR_DONE;
 owner=gettid();finished_token=0;session.active=true;session.command={42,true,false};session.account=account;
 session.stage=stage;session.started=session.since=clock_ms();session.promise=Root(&promise_object);session.player=Root(&player_object);
}
}
extern "C" int il2cpp_string_length(void *o){assert(o==&string_object);return account.size();}
extern "C" void *il2cpp_class_get_fields(void *k,void **iter){return enum_fields(k,iter);}
extern "C" const char *il2cpp_field_get_name(void *f){return field_name(f);}
extern "C" const void *pgrazor_runtime_transport_api(uint32_t v){assert(v==1);return &transport;}
extern "C" const void *pgrazor_vgp_results_api(uint32_t v){assert(v==1);return &results;}
extern "C" const void *pgrazor_runtime_metadata_api(uint32_t v){assert(v==1);return &metadata;}
extern "C" const void *pgrazor_runtime_bootstrap_api(uint32_t v){assert(v==1);return &bootstrap_api;}
extern "C" const void *pgrazor_incoming_gifts_api(uint32_t){return nullptr;}
extern "C" const void *pgrazor_outgoing_gifts_api(uint32_t){return nullptr;}
int main(){
 transport.sample=sample;transport.get_string=get_string;transport.get_bool=get_bool;transport.emit=emit;
 results.poll=poll_result;metadata.resolve_class=resolve_class;bootstrap_api.snapshot=bootstrap;
 boot.runtime.gchandle_new=reinterpret_cast<void*>(root_new);boot.runtime.gchandle_get_target=reinterpret_cast<void*>(root_get);boot.runtime.gchandle_free=reinterpret_cast<void*>(root_free);
 boot.runtime.object_get_class=reinterpret_cast<void*>(object_class);boot.runtime.field_get_value=reinterpret_cast<void*>(read_value);boot.runtime.field_static_get_value=reinterpret_cast<void*>(static_value);
 boot.runtime.runtime_invoke=reinterpret_cast<void*>(invoke_method);boot.runtime.string_chars=reinterpret_cast<void*>(chars);
 boot.meta.class_get_field_from_name=reinterpret_cast<void*>(lookup_field);boot.meta.field_get_type=reinterpret_cast<void*>(field_type);boot.meta.class_from_type=reinterpret_cast<void*>(type_class);boot.meta.field_get_flags=reinterpret_cast<void*>(flags);
 boot.meta.class_get_method_from_name=reinterpret_cast<void*>(lookup_method);boot.meta.method_get_param_count=reinterpret_cast<void*>(param_count);
 Command c;assert(parse("G1|42|1|0",9,c)&&c.token==42&&c.enabled&&!c.full_bag);
 for(const char *s:{"G1|0|1|0","G1|42|2|0","G1|42|1|2","G1|42|1|0|1","G1|-1|1|0","G1|9223372036854775808|1|0","G1|42|1","G2|42|1|0"})assert(!parse(s,strlen(s),c));
 assert(permission().valid&&permission().allowed);fresh=false;assert(!permission().valid);fresh=true;changing=true;assert(!permission().valid);changing=false;ready=false;assert(permission().valid&&!permission().allowed);ready=true;
 fixture();poll_code=PGR_VR_PENDING;session.canceled=true;poll();assert(session.promise&&session.sent==0&&polls==1&&events.empty());
 session.since=clock_ms()-20001;poll();assert(!session.active&&finished_token==42&&polls==2);assert(events.back().find("not retried")!=std::string::npos);
 pgr_gift_batch_tick(owner,nullptr);assert(polls==2&&bootstraps==0);clean();
 fixture();poll();assert(session.sent==1&&session.opened==0&&session.friend_cursor==1&&session.gift_cursor==1&&!session.promise);clean();
 fixture();result_code=4;poll();assert(session.sent==0&&session.skipped==1&&session.gift_cursor==1&&session.friend_cursor==1);clean();
 fixture(SEND_CHECK);poll();assert(session.send_authorized&&session.stage==SEND&&session.sent==0&&session.friend_cursor==0);clean();
 fixture(OPEN);poll();assert(session.opened==1&&session.sent==0&&session.open_cursor==1);clean();
 for(int code:{2,3}){fixture(OPEN);result_code=code;poll();assert(!session.active&&finished_token==42);assert(events.back().find("\"opened\":0,\"skipped\":1")!=std::string::npos);clean();}
 fixture();session.canceled=true;poll();assert(!session.active&&events.back().find("\"sent\":1")!=std::string::npos);clean();
 fixture();poll_code=PGR_VR_REJECTED;pgr_gift_batch_tick(owner,nullptr);assert(!session.active&&polls==1&&events.back().find("without retry")!=std::string::npos);clean();
 fixture();account="account-b";pgr_gift_batch_tick(owner,nullptr);assert(!session.active&&polls==0&&events.back().find("Account changed")!=std::string::npos);clean();
 fixture();session.promise.reset();session.canceled=true;pgr_gift_batch_tick(owner,nullptr);assert(!session.active&&polls==0);clean();
 // Exercise the actual submission guard: cancellation and command replacement must
 // fail before any managed request (or even account lookup) is invoked.
 fixture();session.promise.reset();session.canceled=true;
 bool blocked=false;try{submit(SEND,&response_object,"SendGift",5);}catch(const Failure&){blocked=true;}
 assert(blocked&&invocations==0&&!session.promise);clean();
 fixture();session.promise.reset();config="G1|99|1|0";blocked=false;
 try{submit(OPEN,&response_object,"OpenGift",3);}catch(const Failure&){blocked=true;}
 assert(blocked&&invocations==0&&!session.promise);clean();
 config="G1|43|0|0";events.clear();pgr_gift_batch_tick(owner,nullptr);assert(finished_token==43&&events.size()==1&&bootstraps==0);pgr_gift_batch_tick(owner,nullptr);assert(events.size()==1);clean();
 std::puts("PASS: gift batch parsing, permission, cancellation, account change, outcome counters, limits and no replay");
}

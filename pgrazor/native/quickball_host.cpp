#include "quickball_host.h"
#include "always_hit.h"
#include "ar_throw.h"
#include "catch_animation.h"
#include "catch_observers.h"
#include "iv_display.h"
#include "map_tap.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <time.h>
#include <unistd.h>

namespace {
using namespace pgr_quickball_schema;
std::mutex config_mutex,options_mutex,event_mutex;
std::atomic<bool> configured{false},stopping{false},busy{false},meta_ready{false};
std::atomic<uint64_t> owner{0},ticks{0};
std::atomic<uint64_t> policy_revision{1};
std::atomic<uint32_t> capabilities{0},sites{0},owned_count{0};
std::atomic<uint32_t> optional_faults{0};
std::atomic<int> fault{0},meta_error{0};
PgrQuickballMetaConfig mc{};PgrQuickballExtra extra{};PgrMetaExports me{};
PgrQuickballHostRuntime rt{};PgrQuickballMetaOutput md{};
uint32_t flags=0;bool bootstrap=false,binding_ready=false,install_attempted=false;
PgrQuickballHostOptions options{};uint64_t option_deadline=0,metadata_retry=0;
PgrQuickballHostEvent events[128]{};uint32_t event_head=0,event_count=0;uint64_t event_seq=0;
uint64_t obs_cursor=0,throws_cursor=0,ar_cursor=0,hit_cursor=0,hit_normal_cursor=0,hit_error_cursor=0;
uint32_t speed_cursor=0;uint64_t catch_cursor=0,owned_cursor=0,position_cursor=0,encounter_cursor=0;
bool ready_emitted=false,terminal_emitted=false,control_configured=false;uint32_t previous_ready_bits=UINT32_MAX;
const PgrSpeedApi *speed(){return static_cast<const PgrSpeedApi*>(pgrazor_catch_animation_api(1));}
const PgrObsApi *obs(){return static_cast<const PgrObsApi*>(pgrazor_catch_observers_api(1));}
const PgrThrowApi *throws(){return static_cast<const PgrThrowApi*>(pgrazor_throws_api(1));}
const PgrAlwaysHitApi *hit(){return static_cast<const PgrAlwaysHitApi*>(pgrazor_always_hit_api(1));}
const PgrArThrowApi *ar(){return static_cast<const PgrArThrowApi*>(pgrazor_ar_throw_api(1));}
const PgrMapTapApi *map(){return static_cast<const PgrMapTapApi*>(pgrazor_map_tap_api(1));}
const PgrIvDisplayApi *iv(){return static_cast<const PgrIvDisplayApi*>(pgrazor_iv_display_api(1));}
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
bool now(uint64_t *v){timespec t{};if(clock_gettime(CLOCK_BOOTTIME,&t)||t.tv_sec<0)return false;*v=uint64_t(t.tv_sec)*1000+uint64_t(t.tv_nsec)/1000000;return true;}
void *mut(const void *p){return const_cast<void*>(p);}
void *klass(unsigned i){return mut(md.classes[i]);}
void *method(unsigned i){return mut(md.methods[i]);}
void *code(unsigned i){return mut(md.method_code[i]);}
void *field(unsigned i){return mut(md.fields[i]);}
bool group(unsigned i){return md.groups[i].available==1;}
bool has(uint32_t b){return (capabilities.load(std::memory_order_acquire)&b)!=0;}
bool attached(uint64_t tid){return tid&&tid==uint64_t(gettid())&&mc.thread_current&&reinterpret_cast<void*(*)()>(mc.thread_current)()!=nullptr;}
void revoke(){
 const uint32_t c=capabilities.load(std::memory_order_acquire);
 if(c&PGR_QBH_SPEED)speed()->update(0,0,0);
 if(c&PGR_QBH_OBSERVERS)obs()->stop(0);
 if(c&PGR_QBH_THROWS)throws()->update(0,0,0,0,0);
 if(c&PGR_QBH_ALWAYS_HIT)hit()->update(0,0,0,0);
 if(c&PGR_QBH_AR)ar()->update(0,0,0,0);
 if(c&PGR_QBH_MAP)map()->disable();
 if(c&PGR_QBH_IV)iv()->disable();
}
void fail(int error){int expected=0;fault.compare_exchange_strong(expected,error);stopping.store(true,std::memory_order_release);revoke();}
std::string quote(const char *s){
 std::string out="\"";if(s)for(const unsigned char *p=reinterpret_cast<const unsigned char*>(s);*p;++p){
  if(*p=='"'||*p=='\\'){out+='\\';out+=char(*p);}else if(*p<32){char b[7];std::snprintf(b,sizeof(b),"\\u%04x",unsigned(*p));out+=b;}else out+=char(*p);
 }out+='"';return out;
}
int emit_action(uint32_t op,const int64_t *scalar,uint32_t ns,const PgrRuntimeTransportString *text,uint32_t nt){
 if(!(flags&PGR_QBH_TRANSPORT))return 0;
 int rc=transport()->emit(op,scalar,ns,text,nt);if(rc<0)fail(PGR_QBH_OVERFLOW);return rc;
}
void bool_action(uint32_t op,bool value){int64_t v=value?1:0;emit_action(op,&v,1,nullptr,0);}
void string_action(uint32_t op,const char *value){PgrRuntimeTransportString s{value,uint32_t(std::strlen(value))};emit_action(op,nullptr,0,&s,1);}
void ready_action(bool value,const char *reason){int64_t v=value?1:0;PgrRuntimeTransportString s{reason,uint32_t(std::strlen(reason))};emit_action(43,&v,1,&s,1);}
void event(const char *kind,const std::string &body="",uint64_t sampled_at=UINT64_MAX){
 uint64_t time=0;if(sampled_at==UINT64_MAX)now(&time);else time=sampled_at;
 std::string json="{\"kind\":"+quote(kind)+",\"backend\":\"native\",\"boottimeMs\":"+std::to_string(time);
 if(!body.empty()){json+=',';json+=body;}json+='}';
 if(json.size()>=sizeof(events[0].json)||std::strlen(kind)>=sizeof(events[0].kind)){fail(PGR_QBH_OVERFLOW);return;}
 if(flags&PGR_QBH_TRANSPORT){
  const char *channel=std::strncmp(kind,"outcome-",8)==0?"quick-ball-outcome":"quick-ball";
  PgrRuntimeTransportString args[]={{channel,uint32_t(std::strlen(channel))},{json.data(),uint32_t(json.size())}};
  emit_action(19,nullptr,0,args,2);return;
 }
 bool overflow=false;
 {std::lock_guard<std::mutex> lock(event_mutex);if(event_count==128)overflow=true;else{
   auto &e=events[(event_head+event_count)%128];e={};e.version=1;e.size=sizeof(e);e.sequence=++event_seq;
   std::memcpy(e.kind,kind,std::strlen(kind)+1);std::memcpy(e.json,json.c_str(),json.size()+1);++event_count;
 }}if(overflow)fail(PGR_QBH_OVERFLOW);
}
void *symbol(void *handle,const char *suffix){std::string n="il2cpp_";n+=suffix;return dlsym(handle,n.c_str());}
int bootstrap_binding(){
 PgrBootBinding b{};const auto *api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));
 if(!api||api->snapshot(&b)!=0)return PGR_QBH_NOT_READY;
 me=b.meta;rt={b.runtime.runtime_invoke,b.runtime.gchandle_new,b.runtime.gchandle_get_target,b.runtime.gchandle_free,
  b.runtime.object_get_class,b.runtime.object_unbox,b.runtime.field_get_value,symbol(b.handle,"class_is_assignable_from"),
  b.runtime.object_new,b.runtime.field_set_value,b.runtime.string_new,symbol(b.handle,"string_length"),b.runtime.string_chars,symbol(b.handle,"type_get_object")};
 extra={};extra.version=2;extra.size=sizeof(extra);extra.field_static_get_value=b.runtime.field_static_get_value;
 extra.type_is_byref=symbol(b.handle,"type_is_byref");extra.class_get_fields=symbol(b.handle,"class_get_fields");
 extra.gchandle_new=rt.gchandle_new;extra.gchandle_get_target=rt.gchandle_get_target;extra.gchandle_free=rt.gchandle_free;
 if(rt.object_new)extra.present|=PGR_QB_EXP_OBJECT_NEW;
 if(rt.field_set_value)extra.present|=PGR_QB_EXP_FIELD_SET_VALUE;
 if(symbol(b.handle,"object_get_virtual_method"))extra.present|=PGR_QB_EXP_OBJECT_GET_VIRTUAL;
 if(rt.string_new)extra.present|=PGR_QB_EXP_STRING_NEW;
 if(rt.string_length)extra.present|=PGR_QB_EXP_STRING_LENGTH;
 if(rt.string_chars)extra.present|=PGR_QB_EXP_STRING_CHARS;
 if(rt.type_get_object)extra.present|=PGR_QB_EXP_TYPE_GET_OBJECT;
 if(extra.type_is_byref)extra.present|=PGR_QB_EXP_TYPE_IS_BYREF;
 if(extra.class_get_fields)extra.present|=PGR_QB_EXP_CLASS_GET_FIELDS;
 mc={};mc.version=2;mc.size=sizeof(mc);mc.binding.version=1;mc.binding.size=sizeof(mc.binding);
 mc.binding.exports=&me;mc.binding.module_base=b.module_base;mc.binding.module_size=b.module_size;
 mc.thread_current=b.runtime.thread_current;mc.extra=&extra;
 binding_ready=true;return 0;
}
bool required_runtime(){return rt.runtime_invoke&&rt.gchandle_new&&rt.gchandle_get_target&&rt.gchandle_free&&rt.object_get_class&&rt.object_unbox&&rt.field_get_value;}

// All metadata positions are ABI2 declaration order, independently matched to the
// original adapters. No host-authored object/field/method choices cross this boundary.
void optional_failure(uint32_t bit,const char *kind,int code);
int install_leaves(){
 if(!required_runtime())return PGR_QBH_BAD_CONFIG;
 PgrSpeedConfig sc{1,sizeof(sc),rt.runtime_invoke,rt.gchandle_new,rt.gchandle_get_target,rt.gchandle_free,
  rt.object_get_class,rt.object_unbox,method(2),method(3),method(4),klass(C_SINGLE),klass(C_BOOL),klass(C_ANIMATOR)};
 int rc=speed()->install(&sc);if(rc)return PGR_QBH_LEAF;capabilities.fetch_or(PGR_QBH_SPEED);
 if(!group(GROUP_OUTCOMES))return PGR_QBH_METADATA; // cannot silently retire outcome behavior
 PgrObsConfig oc{};oc.version=1;oc.size=sizeof(oc);oc.object_get_class=rt.object_get_class;oc.field_get_value=rt.field_get_value;
 oc.gchandle_new=rt.gchandle_new;oc.gchandle_get_target=rt.gchandle_get_target;oc.gchandle_free=rt.gchandle_free;
 oc.code_btxu=code(1);oc.code_caught=code(6);oc.code_escaped=code(0);oc.class_ball=klass(C_BALL);oc.class_response=klass(C_RESPONSE);oc.class_animator=klass(C_ANIMATOR);
 oc.field_bread=field(1);oc.field_animator=field(0);oc.field_status=field(2);oc.speed_api=mut(speed());oc.thread_current=mc.thread_current;
 oc.status_error=md.outcomes_values[0];oc.status_success=md.outcomes_values[1];oc.status_escape=md.outcomes_values[2];
 oc.status_flee=md.outcomes_values[3];oc.status_missed=md.outcomes_values[4];oc.status_item_replacement=md.outcomes_values[5];
 rc=obs()->install(&oc);if(rc)return PGR_QBH_LEAF;capabilities.fetch_or(PGR_QBH_OBSERVERS);
 if(group(GROUP_THROWS)){
  PgrThrowConfig c{1,sizeof(c),code(14),code(15),klass(C_BALL),klass(C_ENCOUNTER),rt.object_get_class,rt.class_is_assignable_from,rt.field_get_value,field(1)};
  rc=throws()->install(&c);if(rc)optional_failure(PGR_QBH_THROWS,"throw-unavailable",rc);else capabilities.fetch_or(PGR_QBH_THROWS);
 }
 if(group(GROUP_ALWAYSHIT)){
  PgrAlwaysHitConfig c{1,sizeof(c),code(19),code(20),code(17),code(18),code(21),klass(C_BALL),klass(C_SCHEDULER),klass(C_ACTION),
   rt.object_get_class,rt.field_get_value,rt.runtime_invoke,method(16),rt.gchandle_new,rt.gchandle_get_target,rt.gchandle_free,
   field(1),field(36),field(31),mut(md.alwayshit_current),field(32),field(33),field(35),field(34)};
  rc=hit()->install(&c);if(rc)optional_failure(PGR_QBH_ALWAYS_HIT,"always-hit-unavailable",rc);else capabilities.fetch_or(PGR_QBH_ALWAYS_HIT);
 }
 if(group(GROUP_ARTHROWS)){
  PgrArThrowConfig c{1,sizeof(c),code(22),klass(C_WILDMAP),klass(C_ARVALUES),rt.object_get_class,rt.class_is_assignable_from,
   rt.object_new,rt.gchandle_new,rt.gchandle_get_target,rt.gchandle_free,rt.field_set_value,field(37),field(38),field(39)};
  rc=ar()->install(&c);if(rc)optional_failure(PGR_QBH_AR,"ar-throw-unavailable",rc);else capabilities.fetch_or(PGR_QBH_AR);
 }
 if(group(GROUP_MAPTAP)){
  PgrMapTapConfig c{1,sizeof(c),rt.gchandle_new,rt.gchandle_get_target,rt.gchandle_free,rt.object_get_class,rt.runtime_invoke,
   rt.object_unbox,code(23),code(24),method(25),klass(C_GROUND),klass(C_MAPVIEW),klass(C_LATLNG)};
  rc=map()->install(&c);if(rc)optional_failure(PGR_QBH_MAP,"map-tap-unavailable",rc);else capabilities.fetch_or(PGR_QBH_MAP);
 }
 if(group(GROUP_IV)&&group(GROUP_IVTEXT)&&group(GROUP_IVOWNED)){
  PgrIvDisplayConfig c{};c.version=1;c.size=sizeof(c);const bool outline=group(GROUP_IVOUTLINE);
  void *p[]={rt.gchandle_new,rt.gchandle_get_target,rt.gchandle_free,rt.object_get_class,rt.object_unbox,rt.runtime_invoke,
   rt.field_get_value,rt.class_is_assignable_from,outline?me.class_get_type:nullptr,outline?rt.type_get_object:nullptr,
   rt.string_new,rt.string_length,rt.string_chars,method(27),method(28),method(29),method(30),method(32),method(33),
   method(34),method(35),method(31),method(36),method(37),method(38),method(39),method(40),method(4),
   outline?method(42):nullptr,outline?method(43):nullptr,outline?method(44):nullptr,outline?method(45):nullptr,
   outline?method(46):nullptr,outline?method(47):nullptr,outline?method(48):nullptr,outline?method(49):nullptr,outline?method(50):nullptr,
   code(26),code(41),klass(C_NAMEPLATE),klass(C_MAPPK),klass(C_WILDMAP),klass(C_PK),klass(C_UITEXT),klass(C_ENCOUNTEROUT),klass(C_WILDPROTO),
   klass(C_TAPPABLE),klass(C_PANEL),klass(C_STRING),klass(C_INT32),klass(C_BOOL),klass(C_SINGLE),klass(C_HWRAP),klass(C_TEXTANCHOR),
   outline?klass(C_OUTLINE):nullptr,outline?klass(C_COLOR):nullptr,outline?klass(C_VECTOR2):nullptr,
   field(51),field(52),field(53),field(54),field(55),field(56),field(57),field(58),field(59),field(60),field(61),
   field(45),field(46),field(47),field(48),field(49),field(50),field(62),field(63),field(64)};
  static_assert(sizeof(p)==77*sizeof(void*),"IV pointer layout");std::memcpy(&c.gchandle_new,p,sizeof(p));
  c.overflow_value=md.iv_overflow_value;c.alignment_value=md.iv_alignment_value;c.external_catch_observer=1;
  rc=iv()->install(&c);if(rc)optional_failure(PGR_QBH_IV,"iv-owned-unavailable",rc);else capabilities.fetch_or(PGR_QBH_IV);
 }
 return 0;
}

int configure(const PgrQuickballHostConfig *input){
 if(input&&(input->version!=1||input->size!=sizeof(*input)||input->reserved||(input->flags&~PGR_QBH_TRANSPORT)||
  !input->metadata||!input->runtime||!input->metadata->binding.exports||!input->metadata->extra))return PGR_QBH_BAD_CONFIG;
 std::lock_guard<std::mutex> lock(config_mutex);
 if(configured.load())return PGR_QBH_RECONFIG;
 if(input){mc=*input->metadata;extra=*mc.extra;me=*mc.binding.exports;rt=*input->runtime;
  mc.extra=&extra;mc.binding.exports=&me;flags=input->flags;binding_ready=true;
 }else{bootstrap=true;flags=PGR_QBH_TRANSPORT;}
 configured.store(true,std::memory_order_release);return 0;
}
bool valid_options(const PgrQuickballHostOptions &p){
 return p.version==1&&p.size==sizeof(p)&&!p.reserved&&p.quick_allowed<=1&&p.throw_allowed<=1&&p.ar_allowed<=1&&p.map_allowed<=1&&p.iv_allowed<=1&&
 p.grade<=3&&p.curve<=1&&p.always_hit<=1&&p.ar_enabled<=1&&p.map_mode<=3&&p.iv_catch<=1&&p.iv_owned<=1&&p.lease_ms<=2500&&
 p.map_epoch<=((flags&PGR_QBH_TRANSPORT)?uint64_t(INT32_MAX):9007199254740991ULL);
}
int set_options(const PgrQuickballHostOptions *p){
 if(!configured.load(std::memory_order_acquire))return PGR_QBH_BAD_CONFIG;
 if(!p||!valid_options(*p)){{std::lock_guard<std::mutex> lock(options_mutex);option_deadline=0;policy_revision.fetch_add(1);}revoke();return PGR_QBH_BAD_CONFIG;}
 if(stopping.load())return PGR_QBH_STOPPED;
 uint64_t t=0;if(!now(&t)){fail(PGR_QBH_CLOCK);return PGR_QBH_CLOCK;}
 bool changed=false;
 {std::lock_guard<std::mutex> lock(options_mutex);auto a=options,b=*p;a.lease_ms=b.lease_ms=0;
  changed=std::memcmp(&a,&b,sizeof(a))!=0;options=*p;option_deadline=t+p->lease_ms;policy_revision.fetch_add(1);}
 if(changed)revoke();return 0;
}
int shared_sites(uint32_t mask){if(mask&~3u)return PGR_QBH_BAD_CONFIG;uint32_t old=sites.exchange(mask);policy_revision.fetch_add(1);if((old&mask)!=old)revoke();return 0;}

void poll_options(){
 if(!(flags&PGR_QBH_TRANSPORT))return;
 const auto *tr=transport();PgrRuntimeTransportSample before{},after{};
 PgrQuickballHostOptions p{};p.version=1;p.size=sizeof(p);p.lease_ms=100;
 auto b=[&](uint32_t i){int v=0;return tr->get_bool(i,&v)==0&&v==1;};
 auto n=[&](uint32_t i){int32_t v=0;return tr->get_int(i,&v)==0?v:0;};
 if(tr->sample(&before)||!before.snapshot_fresh){p.lease_ms=0;set_options(&p);return;}
 // Reader positions are the immutable52 typed settings schema.
 p.quick_allowed=b(23);p.throw_allowed=b(29);p.ar_allowed=p.throw_allowed&&b(33);
 p.map_allowed=b(22);p.iv_allowed=b(19);p.grade=uint32_t(n(7));p.curve=b(34);
 p.always_hit=b(30)&&b(31);p.ar_enabled=b(32);p.map_mode=uint32_t(n(5));p.map_epoch=uint64_t(n(4));
 p.iv_catch=b(18);p.iv_owned=b(20);
 if(tr->sample(&after)||!after.snapshot_fresh||after.snapshot_seq!=before.snapshot_seq)p.lease_ms=0;
 set_options(&p);
}

void publish_readiness(){
 const uint32_t c=stopping.load()?0:(capabilities.load()&~optional_faults.load()),s=sites.load();
 const bool complete=meta_ready.load()&&!stopping.load()&&s==3&&has(PGR_QBH_SPEED)&&has(PGR_QBH_OBSERVERS);
 const uint32_t bits=(complete?1:0)|(c<<1)|(s<<10);
 if(bits==previous_ready_bits)return;previous_ready_bits=bits;
 ready_action(complete,complete?"Ready: 20x catch and break-free animation":stopping.load()?"Cleanup pending; resume the game to restore animation speed":"Waiting for compiled shared observers");
 bool_action(52,(c&PGR_QBH_THROWS)&&(s&PGR_QBH_SUBMIT));bool_action(51,(c&PGR_QBH_ALWAYS_HIT)!=0);
 bool_action(50,(c&PGR_QBH_AR)!=0);bool_action(42,(c&PGR_QBH_MAP)!=0);bool_action(40,(c&PGR_QBH_IV)&&(s&PGR_QBH_NAMEPLATE));
 if(!ready_emitted&&complete){ready_emitted=true;
  event("metadata-ready","\"successValue\":"+std::to_string(md.success)+",\"multiplier\":20,\"leaseMs\":2500,\"timeoutMs\":8000,\"throws\":{\"available\":"+std::string(has(PGR_QBH_THROWS)?"true":"false")+"},\"alwaysHit\":{\"available\":"+(has(PGR_QBH_ALWAYS_HIT)?"true":"false")+"},\"arThrows\":{\"available\":"+(has(PGR_QBH_AR)?"true":"false")+"},\"mapTap\":{\"available\":"+(has(PGR_QBH_MAP)?"true":"false")+"},\"ivOwned\":{\"available\":"+(has(PGR_QBH_IV)?"true":"false")+"}");
 }
}

void optional_failure(uint32_t bit,const char *kind,int code){
 if(optional_faults.fetch_or(bit)&bit)return;
 if(bit==PGR_QBH_THROWS)throws()->update(0,0,0,0,0);
 if(bit==PGR_QBH_ALWAYS_HIT)hit()->update(0,0,0,0);
 if(bit==PGR_QBH_AR)ar()->update(0,0,0,0);
 if(bit==PGR_QBH_MAP)map()->disable();
 if(bit==PGR_QBH_IV)iv()->disable();
 event(kind,"\"reason\":\"native-leaf\",\"code\":"+std::to_string(code));
}

const char *outcome(uint32_t reason){return reason==10?"caught":reason==11?"broke-free":reason==12?"fled":nullptr;}
void drain_observers(){
 PgrObsSample s{};if(obs()->sample(&s))return;
 if(s.event_head<obs_cursor)obs_cursor=s.event_head;
 if(s.event_head-obs_cursor>PGR_OBS_EVENT_RING){event("outcome-dropped","\"count\":"+std::to_string(s.event_head-obs_cursor-PGR_OBS_EVENT_RING));obs_cursor=s.event_head-PGR_OBS_EVENT_RING;}
 for(uint64_t seq=obs_cursor+1;seq<=s.event_head;++seq){const auto &e=s.events[seq%PGR_OBS_EVENT_RING];if(e.seq!=seq)continue;
  std::string body="\"status\":"+std::to_string(e.status)+",\"tid\":"+std::to_string(e.tid);
  const char *o=outcome(e.reason);
  if(e.action==1){body+=",\"outcome\":"+quote(o);event("outcome-confirmed",body);if(o&&!stopping.load())string_action(22,o);}
  else if(e.action==2)event("outcome-suppressed",body+",\"reason\":"+quote(e.reason==1?"ambiguous-status":e.reason==2?"not-confirmed":"unknown-status"));
  else if(e.action==3)event("outcome-unavailable",body);
  else if(e.action==4)event("skip",body+",\"reason\":"+quote(e.reason==20?"concurrent-attempt":e.reason==21?"special-ball":"revoked"));
  else if(e.action==5)event("catch-observer-apply",body);
  else if(e.action==6)event("catch-observer-complete",body+",\"outcome\":"+quote(o));
 }obs_cursor=s.event_head;if(s.fault)fail(PGR_QBH_LEAF);
}
void drain_speed(){
 PgrSpeedSample s{};if(speed()->sample(&s))return;
 if(s.serial!=speed_cursor){speed_cursor=s.serial;const char *names[]={"","speed-applied","speed-restored","speed-external-change","speed-target-destroyed","cleanup-failed","speed-skip"};
  const char *reasons[]={"","stop","off","lease-expired","timeout","normal-caught","normal-broke-free","clock-failed","apply-readback","destroyed-animator","nonstandard-speed","concurrent-attempt","root-failed"};
  if(s.action<7&&s.action){std::string values="\"originalSpeed\":"+(std::isfinite(s.original)?std::to_string(s.original):"null")+",\"readback\":"+(std::isfinite(s.readback)?std::to_string(s.readback):"null")+",\"fault\":"+(s.fault?"true":"false");
   event(names[s.action],values+",\"reason\":"+(s.reason<13&&s.reason?quote(reasons[s.reason]):"null"));}
 }if(s.fault)fail(PGR_QBH_LEAF);
}
const char *iv_reason(uint32_t reason,bool inventory){
 switch(reason){case 1:return "encounter-id-mismatch";case 2:return inventory?"stats-not-populated":"encounter-stats-not-populated";
 case 3:return "pokemon-class";case 4:return "text-class";case 5:return "response-class";case 6:return "string";case 7:return "managed-exception";
 case 8:return "gc-handle";case 9:return "boxed-class-mismatch";case 10:return "clock";case 11:return "owned-identity";case 12:return "outline-unavailable";case 13:return "render";default:return "native";}
}
void iv_notice(bool inventory,uint32_t notice,int32_t level,int32_t attack,int32_t defense,int32_t stamina,int32_t cp,uint32_t reason,const char *text){
 const char *name=inventory?(notice==1?"iv-owned-label":notice==2?"iv-owned-pending":"iv-owned-unavailable"):(notice==1?"iv-label":notice==2?"iv-pending":"iv-unavailable");
 if(notice==1)event(name,"\"text\":"+quote(text)+",\"level\":"+(level==PGR_IV_LEVEL_UNKNOWN?std::string("null"):std::to_string(level/2.0))+",\"attack\":"+std::to_string(attack)+",\"defense\":"+std::to_string(defense)+",\"stamina\":"+std::to_string(stamina)+",\"cp\":"+std::to_string(cp));
 else if(notice==2||notice==3)event(name,"\"reason\":"+quote(iv_reason(reason,inventory)));
}
int64_t double_bits(double value){int64_t b;std::memcpy(&b,&value,8);return b;}
void drain_iv(){
 PgrIvDisplaySample s{};if(iv()->sample(&s))return;
 if(s.encounter_serial!=encounter_cursor){encounter_cursor=s.encounter_serial;event("cooldown-encounter","\"source\":\"native\"");string_action(30,"native");}
 if(s.position_serial!=position_cursor){position_cursor=s.position_serial;
  std::string context;
  if(flags&PGR_QBH_TRANSPORT){
   auto coordinate=[](uint32_t i){double value=0;int present=0;return transport()->get_double(i,&value,&present)==0&&present&&std::isfinite(value)?std::to_string(value):std::string("null");};
   int booster=0;transport()->get_bool(28,&booster);
   context=",\"playerLatitude\":"+coordinate(0)+",\"playerLongitude\":"+coordinate(1)+",\"spawnBooster\":"+(booster==1?"true":"false");
  }
  event("encounter-position","\"latitude\":"+std::to_string(s.latitude)+",\"longitude\":"+std::to_string(s.longitude)+",\"encounterId\":"+quote(std::to_string(s.encounter_id).c_str())+context);
 }
 if(s.catch_serial!=catch_cursor){catch_cursor=s.catch_serial;iv_notice(false,s.catch_notice,s.catch_level_half,s.catch_attack,s.catch_defense,s.catch_stamina,s.catch_cp,s.catch_reason,s.catch_label);}
 if(s.owned_serial!=owned_cursor){owned_cursor=s.owned_serial;iv_notice(true,s.owned_notice,s.owned_level_half,s.owned_attack,s.owned_defense,s.owned_stamina,s.owned_cp,s.owned_reason,s.owned_label);}
}
void drain_map(const PgrQuickballHostOptions &p,uint64_t deadline){
 if(optional_faults.load()&PGR_QBH_MAP)return;
 PgrMapTapSample s{};int status=map()->sample(&s);if(status||s.fault){optional_failure(PGR_QBH_MAP,"map-tap-unavailable",status?status:int(s.fault));return;}
 for(unsigned i=0;i<16;++i){PgrMapTapEvent e{};int rc=map()->drain(&e);if(rc==PGR_MAP_TAP_EMPTY)break;if(rc){optional_failure(PGR_QBH_MAP,"map-tap-unavailable",rc);return;}
  uint64_t t=0;if(stopping.load()||!p.map_allowed||!now(&t)||t>=deadline||e.mode!=int32_t(p.map_mode)||e.epoch!=p.map_epoch)continue;
  event("map-tap","\"mode\":"+std::to_string(e.mode)+",\"epoch\":"+std::to_string(e.epoch)+",\"latitude\":"+std::to_string(e.latitude)+",\"longitude\":"+std::to_string(e.longitude),e.boottime_ms);
  int64_t args[]={double_bits(e.latitude),double_bits(e.longitude),e.mode,int64_t(e.epoch),double_bits(double(e.boottime_ms))};emit_action(10,args,5,nullptr,0);
 }
}
void drain_counters(){
 if(has(PGR_QBH_THROWS)){PgrThrowSample s{};if(!throws()->sample(&s)&&s.count!=throws_cursor){throws_cursor=s.count;
  const char *grades[]={"Off","Nice","Great","Excellent"};event("throw-applied","\"route\":"+quote(s.route?"hit":"submit")+",\"grade\":"+quote(s.grade<4?grades[s.grade]:"Off")+",\"curve\":"+(s.curve?"true":"false")+",\"reticle\":"+std::to_string(s.radius)+",\"count\":"+quote(std::to_string(s.count).c_str()));}}
 if(has(PGR_QBH_AR)){PgrArThrowSample s{};if(!ar()->sample(&s)&&s.applied!=ar_cursor){ar_cursor=s.applied;event("ar-throw-applied","\"applied\":"+quote(std::to_string(s.applied).c_str())+",\"errors\":"+quote(std::to_string(s.errors).c_str()));}}
 if(has(PGR_QBH_ALWAYS_HIT)){PgrAlwaysHitSample s{};if(!hit()->sample(&s)&&(s.applied!=hit_cursor||s.normalized!=hit_normal_cursor||s.errors!=hit_error_cursor)){
  hit_cursor=s.applied;hit_normal_cursor=s.normalized;hit_error_cursor=s.errors;event("always-hit-applied","\"applied\":"+quote(std::to_string(s.applied).c_str())+",\"normalized\":"+quote(std::to_string(s.normalized).c_str())+",\"errors\":"+quote(std::to_string(s.errors).c_str()));}}
}
void update_owned(){
 uint32_t count=0;PgrSpeedSample a{};PgrObsSample b{};PgrMapTapSample c{};PgrIvDisplaySample d{};
 if(has(PGR_QBH_SPEED)){if(speed()->sample(&a))++count;else count+=a.owned;}
 if(has(PGR_QBH_OBSERVERS)){if(obs()->sample(&b))++count;else count+=b.seen_count;}
 if(has(PGR_QBH_MAP)){if(map()->sample(&c))++count;else count+=c.owned_root_count;}
 if(has(PGR_QBH_IV)){if(iv()->sample(&d))++count;else count+=d.owned_root_count;}
 owned_count.store(count);
}

int tick(uint64_t tid){
 if(!configured.load(std::memory_order_acquire))return PGR_QBH_BAD_CONFIG;
 if(tid==0||tid!=uint64_t(gettid()))return PGR_QBH_OFF_OWNER;
 if(busy.exchange(true))return PGR_QBH_BUSY;
 struct Busy {~Busy(){busy.store(false,std::memory_order_release);}} guard;
 if(!binding_ready){if(!bootstrap||bootstrap_binding()!=0)return PGR_QBH_NOT_READY;}
 if(!attached(tid))return PGR_QBH_OFF_OWNER;
 uint64_t expected=0;owner.compare_exchange_strong(expected,tid);if(owner.load()!=tid)return PGR_QBH_OFF_OWNER;
 const uint64_t tick_count=ticks.fetch_add(1)+1;uint64_t time=0;if(!now(&time)){fail(PGR_QBH_CLOCK);return PGR_QBH_CLOCK;}
 if(!control_configured&&!stopping.load()){control_configured=true;bool_action(15,true);}
 if(!meta_ready.load()&&!stopping.load()){
  if(time<metadata_retry)return PGR_QBH_NOT_READY;metadata_retry=time+1000;mc.owner_tid=tid;
  const auto *api=static_cast<const PgrQuickballMetaApi*>(pgrazor_quickball_metadata_api(2));
  int rc=api?api->resolve(&mc,&md):PGR_QBH_METADATA;meta_error.store(rc);
  if(rc==PGR_QB_NOT_READY)return PGR_QBH_NOT_READY;
  if(rc){fail(PGR_QBH_METADATA);event("unavailable","\"reason\":\"native-metadata\",\"code\":"+std::to_string(rc));return PGR_QBH_METADATA;}
  if(stopping.load())return PGR_QBH_STOPPED;
  install_attempted=true;rc=install_leaves();if(rc){fail(rc);event("unavailable","\"reason\":\"native-leaf-install\",\"code\":"+std::to_string(rc));return rc;}
  meta_ready.store(true,std::memory_order_release);
 }
 if(!install_attempted)return stopping.load()?PGR_QBH_STOPPED:PGR_QBH_NOT_READY;
 if(!stopping.load())poll_options();
 PgrQuickballHostOptions p{};uint64_t deadline=0,revision=0;{std::lock_guard<std::mutex> lock(options_mutex);p=options;deadline=option_deadline;revision=policy_revision.load();}
 const bool active=!stopping.load()&&tick_count>=3&&time<deadline;
 const uint64_t lease=active?(deadline-time>2500?2500:deadline-time):0;const uint32_t shared=sites.load();
 int rc=0;
 if(has(PGR_QBH_SPEED))rc=speed()->update(active&&p.quick_allowed,active&&p.quick_allowed?lease:0,tid);
 if(has(PGR_QBH_OBSERVERS)){int r=obs()->update(active&&p.quick_allowed,active&&p.quick_allowed?lease:0,tid,
   reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mc.binding.module_base)+kCallerRva));if(r&&r!=PGR_OBS_ERR_LEASE)rc=r;}
 if(has(PGR_QBH_THROWS)&&!(optional_faults.load()&PGR_QBH_THROWS)){int r=throws()->update(p.grade,p.curve,active&&p.throw_allowed&&(shared&PGR_QBH_SUBMIT),active&&p.throw_allowed&&(shared&PGR_QBH_SUBMIT)?lease:0,tid);if(r)optional_failure(PGR_QBH_THROWS,"throw-unavailable",r);}
 if(has(PGR_QBH_ALWAYS_HIT)&&!(optional_faults.load()&PGR_QBH_ALWAYS_HIT)){int r=hit()->update(p.always_hit,active&&p.throw_allowed&&p.always_hit,active&&p.throw_allowed&&p.always_hit?lease:0,tid);if(r)optional_failure(PGR_QBH_ALWAYS_HIT,"always-hit-unavailable",r);}
 if(has(PGR_QBH_AR)&&!(optional_faults.load()&PGR_QBH_AR)){int r=ar()->update(p.ar_enabled,active&&p.ar_allowed&&p.ar_enabled,active&&p.ar_allowed&&p.ar_enabled?lease:0,tid);if(r)optional_failure(PGR_QBH_AR,"ar-throw-unavailable",r);}
 if(has(PGR_QBH_MAP)&&!(optional_faults.load()&PGR_QBH_MAP)){int r=map()->update(active&&p.map_allowed,active&&p.map_allowed?lease:0,tid,p.map_epoch,p.map_mode);if(r)optional_failure(PGR_QBH_MAP,"map-tap-unavailable",r);}
 if(has(PGR_QBH_IV)&&!(optional_faults.load()&PGR_QBH_IV)){int r=iv()->update(p.iv_catch,p.iv_owned,active&&p.iv_allowed&&(shared&PGR_QBH_NAMEPLATE)?lease:0,tid);if(r)optional_failure(PGR_QBH_IV,"iv-owned-unavailable",r);}
 if(rc)fail(PGR_QBH_LEAF);
 if(stopping.load()||revision!=policy_revision.load())revoke();
 // Cleanup always runs, including terminal failure, expired lease, and explicit stop.
 if(has(PGR_QBH_SPEED)){speed()->tick(tid);drain_speed();}
 if(has(PGR_QBH_OBSERVERS)){obs()->tick(tid);drain_observers();}
 if(has(PGR_QBH_ALWAYS_HIT))hit()->tick(tid);
 if(has(PGR_QBH_IV)){iv()->tick(tid,stopping.load()?1:0);drain_iv();}
 if(has(PGR_QBH_MAP))drain_map(p,deadline);
 drain_counters();update_owned();publish_readiness();
 if(stopping.load()&&!owned_count.load()&&!terminal_emitted){terminal_emitted=true;event("finished","\"status\":"+quote(fault.load()?"unavailable":"cancelled"));}
 return fault.load()?fault.load():0;
}
int stop(){stopping.store(true,std::memory_order_release);policy_revision.fetch_add(1);revoke();return 0;}
int sample(PgrQuickballHostSample *out){if(!out)return PGR_QBH_BAD_CONFIG;update_owned();*out={1,sizeof(*out),uint32_t(configured.load()),uint32_t(meta_ready.load()),
 uint32_t(meta_ready.load()&&!stopping.load()&&sites.load()==3),uint32_t(stopping.load()),capabilities.load(),sites.load(),owner.load(),ticks.load(),owned_count.load(),0,fault.load(),meta_error.load()};
 {std::lock_guard<std::mutex> lock(event_mutex);out->queued=event_count;}return 0;}
int next_event(PgrQuickballHostEvent *out){if(!out)return PGR_QBH_BAD_CONFIG;std::lock_guard<std::mutex> lock(event_mutex);if(!event_count)return PGR_QBH_EMPTY;
 *out=events[event_head];event_head=(event_head+1)%128;--event_count;return 0;}
int rewrite_submit(void *self,PgrThrowValue *value){
 if(!value)return PGR_QBH_BAD_CONFIG;if(stopping.load()||!has(PGR_QBH_THROWS)||(optional_faults.load()&PGR_QBH_THROWS)||!(sites.load()&PGR_QBH_SUBMIT))return PGR_QBH_NOT_READY;
 return throws()->rewrite_submit(self,value);
}
uint64_t begin_catch(void *self,void *pokemon,uint64_t tid,void **refreshed){
 if(stopping.load()||!has(PGR_QBH_IV)||(optional_faults.load()&PGR_QBH_IV)||!(sites.load()&PGR_QBH_NAMEPLATE)||tid!=owner.load()||!attached(tid))return 0;
 return iv()->begin_catch(self,pokemon,tid,refreshed);
}
int end_catch(uint64_t token,uint64_t tid){
 // Pairing is permitted after stop and shared-site revocation; the leaf owns frame roots.
 if(!has(PGR_QBH_IV)||tid!=owner.load()||!attached(tid))return PGR_QBH_OFF_OWNER;
 return iv()->end_catch(token,tid);
}
void on_pulse(void *,const PgrUnityPulse *pulse){if(pulse&&pulse->tid>0&&pulse->generation){
 const auto *api=static_cast<const PgrUnitySchedulerApi*>(pgrazor_unity_scheduler_api(2));if(api&&api->valid(pulse->generation)==1)tick(uint64_t(pulse->tid));}}
const PgrQuickballHostApi api{1,sizeof(PgrQuickballHostApi),configure,set_options,shared_sites,tick,stop,sample,next_event,rewrite_submit,begin_catch,end_catch,on_pulse};
}
extern "C" const void *pgrazor_quickball_host_api(uint32_t version){return version==1?&api:nullptr;}

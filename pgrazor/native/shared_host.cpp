#include "shared_host.h"
#include "shared_catch.h"
#include "summary_completion.h"
#include "details_completion.h"
#include "details_recovery.h"
#include "catch_discovery.h"
#include "catch_canvas.h"
#include "catch_completion.h"
#include "catch_presentation.h"
#include "transfer_execution.h"
#include "transfer_facts.h"
#include "transfer_observers.h"
#include "runtime_transport.h"
#include "runtime_bootstrap.h"
#include "interaction_observer.h"
#include <dlfcn.h>
#include "hook_engine.h"
#include <atomic>
#include <array>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <time.h>
#include <unistd.h>

namespace {
using namespace pgr_shared_meta;
template<class T>const T *table(const void *p){return static_cast<const T*>(p);}
const auto sh=table<PgrSharedCatchApi>(pgrazor_shared_catch_api(1));
const auto sc=table<PgrSummaryCompletionApi>(pgrazor_summary_completion_api(1));
const auto dc=table<PgrDetailsCompletionApi>(pgrazor_details_completion_api(1));
const auto dr=table<PgrDetailsRecoveryApi>(pgrazor_details_recovery_api(1));
const auto cd=table<PgrCatchDiscoveryApi>(pgrazor_catch_discovery_api(1));
const auto cv=table<PgrCatchCanvasApi>(pgrazor_catch_canvas_api(1));
const auto tx=table<PgrTransferExecApi>(pgrazor_transfer_execution_api(1));
const auto tf=table<PgrTransferFactsApi>(pgrazor_transfer_facts_api(1));
const auto cc=table<PgrCatchCompletionApi>(pgrazor_catch_completion_api(1));
const auto bridge=table<PgrCatchApi>(pgrazor_catch_presentation_api(1));
const auto transport=table<PgrRuntimeTransportApi>(pgrazor_runtime_transport_api(1));
const auto transfer_policy=table<PgrTransferApi>(pgrazor_transfer_policy_api(1));
PgrSharedHostConfig cfg{},accepted_config{};
PgrMetaExports exports{};
PgrSharedMetaExtraExports extra{};
PgrSharedMetadataOutput md{};
std::mutex setup_mutex,options_mutex,event_mutex;
std::atomic<bool> ready{false},stopping{false},failed{false};
std::atomic<uint64_t> owner{0},frames{0},ticks{0};
std::atomic<uint32_t> capabilities{0},root_count{0},record_count{0};
PgrSharedHostOptions options{};
std::array<int32_t,2000> keep{};
uint64_t deadline=0,serial=0,next_key=0,last_scene=0,hit_window=0;
uint32_t hook_hits=0;
thread_local uint64_t pulse_generation=0;
uint64_t transport_sequence=0,transport_generation=0;
bool transport_keep_valid=true;
std::array<PgrSharedHostEvent,256> events{};
size_t event_head=0,event_count=0;
std::array<uint64_t,64> external_tokens{};
std::array<PgrTransferExecEvent,128> external_events{};
size_t external_head=0,external_count=0;
bool external_submitting=false;
bool installed=false,busy=false,invoking=false;
void *invoking_gui=nullptr;
uint32_t opener_depth=0;
using New=void *(*)(void*,int);using Get=void *(*)(void*);using Free=void(*)(void*);
uint64_t now(){timespec t{};if(clock_gettime(CLOCK_BOOTTIME,&t)||t.tv_sec<0)return 0;return uint64_t(t.tv_sec)*1000+uint64_t(t.tv_nsec)/1000000;}
uint64_t tid(){return uint64_t(gettid());}
bool owned(){return owner.load()==tid()&&cfg.metadata.thread_current&&reinterpret_cast<void *(*)()>(cfg.metadata.thread_current)();}
void *target(void *h){return h?reinterpret_cast<Get>(extra.gchandle_get_target)(h):nullptr;}
void *hold(void *o,int pinned=0){if(!o)return nullptr;void *h=reinterpret_cast<New>(extra.gchandle_new)(o,pinned);if(h)root_count.fetch_add(1);return h;}
void free_root(void *&h){if(h){void *old=h;h=nullptr;reinterpret_cast<Free>(extra.gchandle_free)(old);root_count.fetch_sub(1);}}
bool exact(void *o,const void *k){return o&&k&&reinterpret_cast<void *(*)(void*)>(cfg.object_get_class)(o)==k;}
template<class T>T field(void *o,const void *f){T out{};if(o&&f)reinterpret_cast<void(*)(void*,const void*,void*)>(cfg.field_get_value)(o,f,&out);return out;}
void *h(int i){return const_cast<void*>(md.opt_handles[i]);}
void *k(int i){return const_cast<void*>(md.opt_classes[i]);}
void *c(int i){return const_cast<void*>(md.classes[i]);}
void *f(int i){return const_cast<void*>(md.fields[i]);}
void *m(int i){return const_cast<void*>(md.methods[i]);}
void transport_event(uint32_t,uint64_t,int,uint32_t,int,int,int);
void emit(uint32_t kind,uint64_t slot=0,int status=0,uint32_t flags=0,int before=0,int after=0,int reason=0){
 if(cfg.transport_flags&1){transport_event(kind,slot,status,flags,before,after,reason);return;}
 std::lock_guard<std::mutex> lock(event_mutex);
 if(event_count==events.size()){failed=true;stopping=true;return;}
 events[(event_head+event_count++)%events.size()]={1,sizeof(PgrSharedHostEvent),kind,flags,status,before,after,reason,++serial,slot,tid()};
}
void fault(int rc){failed=true;stopping=true;emit(8,0,rc);}
struct Gate {uint32_t flags,protect,count;uint64_t epoch,remaining;std::array<int32_t,2000> ids;};
Gate gate(){Gate g{};std::lock_guard<std::mutex> lock(options_mutex);g.flags=options.flags;g.protect=options.protect;g.epoch=options.epoch;g.count=options.keep_count;std::copy_n(keep.begin(),g.count,g.ids.begin());const auto n=now();g.remaining=!stopping&&!failed&&(g.flags&PGR_SHH_ALLOWED)&&n&&n<deadline?std::min<uint64_t>(2500,deadline-n):0;return g;}
bool scheduler_current(){return !pulse_generation||table<PgrUnitySchedulerApi>(pgrazor_unity_scheduler_api(2))->valid(pulse_generation)==1;}
bool transport_current(){if(!(cfg.transport_flags&1))return true;PgrRuntimeTransportSample s{};return transport->sample(&s)==0&&s.snapshot_fresh&&!s.fault&&s.snapshot_seq==transport_sequence&&s.generation==transport_generation;}
bool refresh_transport_current();
bool current(uint64_t epoch){if(!owned()||!scheduler_current()||!refresh_transport_current())return false;const auto g=gate();return g.remaining&&g.epoch==epoch;}
const char *keep_reason(int code){static const char *reasons[]={"none","keep-all","invalid-keep-list","unknown-species","custom-keep-list","unknown-iv","unknown-category","unknown-size","favorite-or-egg","iv-at-least-90","protected-rare","protected-shiny","protected-event","protected-background","protected-xxs","protected-xxl","invalid-native-input"};return code>=0&&code<17?reasons[code]:"native-transfer-blocked";}
bool publish(uint32_t opcode,const char *text=nullptr,int boolean=-1){PgrRuntimeTransportString s{text,text?uint32_t(std::strlen(text)):0};const int64_t value=boolean;const int rc=transport->emit(opcode,boolean<0?nullptr:&value,boolean<0?0:1,text?&s:nullptr,text?1:0);if(rc!=0&&rc!=PGR_TRANSPORT_STOPPED){failed=true;stopping=true;return false;}return true;}
void transport_event(uint32_t kind,uint64_t slot,int status,uint32_t flags,int before,int after,int reason){
 const char *name="native-shared-event",*why="";
 switch(kind){case 1:name="candidate-initialized";break;case 2:name="candidate-cancelled";break;case 3:name="summary-dismiss-invoked";break;case 4:name="details-dismiss-invoked";break;case 5:name="transfer-requested";break;
 case 6:name=status==PGR_TX_EVENT_COMPLETE&&(flags&1)?"transfer-confirmed":"transfer-error";why=status==PGR_TX_EVENT_TIMEOUT?"response-timeout":"server-result";break;
 case 7:name="transfer-kept";why=keep_reason(status);break;case 8:name="fault";break;case 9:name="metadata-ready";break;case 10:name="direct-catch-fallback";break;case 11:name="direct-catch-returned";break;case 12:name="candidate-armed";break;case 13:name="stopped";break;}
 char json[768];const int n=std::snprintf(json,sizeof(json),"{\"kind\":\"%s\",\"backend\":\"native\",\"boottimeMs\":%llu,\"tid\":%llu,\"slot\":%llu,\"status\":%d,\"invoked\":%s,\"stateBefore\":%d,\"stateAfter\":%d,\"guiGone\":%s,\"advanced\":%s,\"reason\":\"%s\",\"reasonCode\":%d,\"transferAvailable\":%s}",name,(unsigned long long)now(),(unsigned long long)tid(),(unsigned long long)slot,status,(flags&1)?"true":"false",before,after,(flags&2)?"true":"false",((flags&2)||((flags&4)&&after!=2))?"true":"false",why,reason,(capabilities&PGR_SHH_TRANSFER)?"true":"false");
 if(n<=0||size_t(n)>=sizeof(json)){failed=true;stopping=true;return;}
 PgrRuntimeTransportString strings[]={{"quick-close",11},{json,uint32_t(n)}};
 const int rc=transport->emit(19,nullptr,0,strings,2);if(rc!=0&&rc!=PGR_TRANSPORT_STOPPED){failed=true;stopping=true;return;}
 if(kind==9){publish(16,nullptr,1);publish(45,"Ready: Quick Catch",1);publish(23,(capabilities&PGR_SHH_TRANSFER)?"Ready: new catches only; IV below 90%":"Unavailable: native transfer prerequisites",(capabilities&PGR_SHH_TRANSFER)?1:0);}
 else if(kind==5)publish(56,"Transfer requested; waiting for bag update");
 else if(kind==6){if(status==PGR_TX_EVENT_COMPLETE&&(flags&1)){publish(56,"Transferred newly caught Pokemon");publish(22,"transferred");}else{char message[128];std::snprintf(message,sizeof(message),"Transfer Error: %s",why);publish(56,message);publish(22,"transfer-error");}}
 else if(kind==7){char message[128];std::snprintf(message,sizeof(message),"Kept: %s",why);publish(56,message);}
 else if(kind==8||kind==13){publish(45,kind==8?"Unavailable: native shared host fault":"Stopped",0);publish(23,"Unavailable: shared host stopped",0);}
}
bool parse_keep(const char *text,uint32_t bytes,std::array<int32_t,2000>&ids,uint32_t &count){count=0;if(bytes>10000)return false;std::array<bool,2001> seen{};uint32_t at=0;while(at<bytes){if(text[at]<'1'||text[at]>'9')return false;int value=0;unsigned digits=0;while(at<bytes&&text[at]>='0'&&text[at]<='9'){if(++digits>4)return false;value=value*10+text[at++]-'0';}if(value>2000||seen[value]||count==2000)return false;seen[value]=true;ids[count++]=value;if(at==bytes)break;if(text[at++]!=','||at==bytes)return false;}std::sort(ids.begin(),ids.begin()+count);return true;}
int update(const PgrSharedHostOptions*);
void poll_transport(){if(!(cfg.transport_flags&1))return;PgrRuntimeTransportSample start{},end{};int dismiss=0,allow_transfer=0,keep_all=0;int32_t protect=63;char text[10000];uint32_t len=0,count=0;std::array<int32_t,2000> ids{};
 bool fresh=transport->sample(&start)==0&&start.snapshot_fresh&&!start.fault;
 if(fresh)fresh=transport->get_bool(25,&dismiss)==0&&transport->get_bool(5,&allow_transfer)==0&&transport->get_bool(35,&keep_all)==0&&transport->get_int(8,&protect)==0;
 bool valid_keep=true;if(fresh){const int rc=transport->get_string(4,text,sizeof(text),&len);if(rc==PGR_TRANSPORT_ERR_RANGE){valid_keep=false;len=0;}else if(rc)fresh=false;else valid_keep=parse_keep(text,len,ids,count);}
 if(fresh)fresh=transport->sample(&end)==0&&end.snapshot_fresh&&!end.fault&&end.snapshot_seq==start.snapshot_seq&&end.generation==start.generation;
 if(!fresh){dismiss=allow_transfer=keep_all=0;count=0;protect=63;valid_keep=true;transport_sequence=transport_generation=0;}else {transport_sequence=start.snapshot_seq;transport_generation=start.generation;}
 if(protect<0||protect>63)protect=63;
 const bool enabled=allow_transfer&&valid_keep&&!keep_all&&(capabilities&PGR_SHH_TRANSFER);
 const uint32_t flags=(dismiss||allow_transfer?PGR_SHH_ALLOWED:0)|(dismiss?PGR_SHH_DISMISS:0)|(enabled?PGR_SHH_TRANSFER_ENABLED:0)|(keep_all?PGR_SHH_KEEP_ALL:0);
 uint64_t epoch=1;{std::lock_guard<std::mutex> lock(options_mutex);epoch=options.epoch?options.epoch:1;const bool changed=((options.flags^flags)&(PGR_SHH_ALLOWED|PGR_SHH_DISMISS|PGR_SHH_TRANSFER_ENABLED|PGR_SHH_KEEP_ALL))||options.protect!=uint32_t(protect)||options.keep_count!=count||!std::equal(ids.begin(),ids.begin()+count,keep.begin())||transport_keep_valid!=valid_keep;
  if(changed||!(flags&PGR_SHH_ALLOWED)||now()>=deadline)++epoch;}
 transport_keep_valid=valid_keep;PgrSharedHostOptions next{1,sizeof(next),flags,uint32_t(protect),epoch,fresh?2500u:0u,ids.data(),count,0};if(update(&next)!=0){failed=true;stopping=true;}
}
struct Record {
 uint64_t key=0,slot=0,epoch=0,id=0,canvas=0,transfer_token=0;
 void *gui=nullptr,*state=nullptr,*proto=nullptr,*response=nullptr,*dismissed=nullptr,*data=nullptr,*pokemon=nullptr;
 bool details=false,cancelled=false,invoked=false,transfer_checked=false,transfer_complete=false,transfer_failed=false;
};
std::array<Record,4> records{};
struct Pending {void *response=nullptr,*layer=nullptr;uint64_t id=0,epoch=0,expires=0;bool returned=false;} pending;
void release_pending(){free_root(pending.response);free_root(pending.layer);pending={};}
void release(Record &r){Record old=r;r={};if(old.slot)record_count.fetch_sub(1);if(old.canvas&&(capabilities&PGR_SHH_CANVAS))cv->release(old.canvas,tid());free_root(old.gui);free_root(old.state);free_root(old.proto);free_root(old.response);free_root(old.dismissed);free_root(old.data);free_root(old.pokemon);}

Record *by_slot(uint64_t slot){for(auto &r:records)if(r.slot==slot&&slot)return &r;return nullptr;}
Record *by_gui(void *gui){if(!gui)return nullptr;for(auto &r:records)if(r.slot&&target(r.gui)==gui)return &r;return nullptr;}
void drain(){PgrSharedCatchEvent e{};while(sh->next_event(&e)==1){auto r=by_slot(e.slot);if(r&&!r->invoked){emit(2,r->slot,0,0,0,0,e.reason);release(*r);}}}
void cancel(Record &r,int reason=8){if(!r.slot||r.cancelled||r.invoked)return;r.cancelled=true;sh->cancel(r.slot,reason,tid());}
Record *create(void *gui,bool details=false){if(gui)if(auto r=by_gui(gui))return r;auto g=gate();if(!g.remaining)return nullptr;
 const uint64_t key=++next_key;uint64_t slot=0;const int rc=sh->upsert(key,details?1:0,tid(),&slot);drain();if(rc<0){fault(rc);return nullptr;}if(!slot)return nullptr;
 for(auto &r:records)if(!r.slot){r.key=key;r.slot=slot;r.epoch=g.epoch;r.details=details;record_count.fetch_add(1);if(gui){r.gui=hold(gui);if(!r.gui){cancel(r);drain();return nullptr;}}return &r;}
 sh->cancel(slot,8,tid());drain();fault(-13);return nullptr;
}
bool assign(void *&dst,void *object){void *fresh=hold(object);if(object&&!fresh)return false;free_root(dst);dst=fresh;return true;}
void gates(const Gate &g){const int enabled=g.remaining?1:0;sh->update(enabled,g.remaining,g.epoch,tid());sc->update(enabled,g.remaining,g.epoch,tid());
 if(capabilities&PGR_SHH_DETAILS)dc->update(enabled,g.remaining,g.epoch,tid());
 if(capabilities&PGR_SHH_CANVAS)cv->update(enabled&&(g.flags&PGR_SHH_DISMISS),g.remaining,g.epoch,tid());
 if(capabilities&PGR_SHH_TRANSFER)tx->update(enabled&&(g.flags&PGR_SHH_TRANSFER_ENABLED),g.remaining,g.epoch,tid());
 if(capabilities&PGR_SHH_DIRECT)cc->update(enabled&&(g.flags&PGR_SHH_DISMISS),g.remaining,g.epoch,tid());
}
// A Java snapshot may advance while an original constructs its UI. Refresh the
// complete policy coherently, rather than dropping an unchanged-policy capture.
// A changed/expired/revoked policy still advances the epoch and invalidates the
// HookFrame. Only the proven Unity owner can refresh; no managed calls occur here.
bool refresh_transport_current(){
 if(transport_current())return true;
 if(!owned()||!(cfg.transport_flags&1))return false;
 poll_transport();gates(gate());
 return transport_current();
}
PgrSummaryCompletionRequest summary_request(const Record &r){PgrSummaryCompletionRequest q{1,sizeof(q)};std::memcpy(q.expected_key,&r.id,8);q.epoch=r.epoch;q.tid=tid();q.gui=r.gui;q.state=r.state;q.proto=r.proto;q.response=r.response;q.dismissed=r.dismissed;return q;}
PgrDetailsCompletionRequest details_request(const Record &r){PgrDetailsCompletionRequest q{1,sizeof(q)};std::memcpy(q.expected_key,&r.id,8);q.epoch=r.epoch;q.tid=tid();q.gui=r.gui;q.view=r.state;q.response=r.response;q.data=r.data;q.pokemon=r.pokemon;q.dismissed=r.dismissed;return q;}

// Hooks retain pinned arguments across originals; records use independent roots.
struct HookFrame {
 uint64_t epoch=0;void *a=nullptr,*b=nullptr;bool observe=false;
 HookFrame(void *self,void *value=nullptr){frames.fetch_add(1);if(!ready||!owned())return;++opener_depth;observe=true;refresh_transport_current();auto g=gate();epoch=g.epoch;if(!g.remaining||!scheduler_current())return;const auto n=now();if(n-hit_window>=1000){hit_window=n;hook_hits=0;}if(++hook_hits>1000){fault(-21);return;}a=hold(self,1);b=hold(value,1);if((self&&!a)||(value&&!b)){fault(-12);}}
 bool active()const{return observe&&a&&current(epoch);}
 ~HookFrame(){free_root(b);free_root(a);if(observe)--opener_depth;frames.fetch_sub(1);}
};
std::array<std::atomic<void*>,10> originals{};
void *original(unsigned i){auto p=originals[i].load();if(!p){std::lock_guard<std::mutex> lock(setup_mutex);p=originals[i].load();}if(!p)std::abort();return p;}
using One=void(*)(void*,const void*);using Two=void(*)(void*,void*,const void*);using Scores=void(*)(void*,void*,int32_t,const void*);
void hook_initialize(void *self,void *value,const void *info){auto fn=reinterpret_cast<Two>(original(0));HookFrame frame(self,value);if(frame.active()){sh->cancel_all(7,tid());}
 try{fn(frame.a?target(frame.a):self,frame.b?target(frame.b):value,info);}catch(...){if(frame.observe)fault(-11);throw;}
 if(!frame.active()||!exact(target(frame.a),c(2))||!exact(target(frame.b),c(3)))return;
 if(field<int32_t>(target(frame.b),f(5))!=md.success)return;auto gui=field<void*>(target(frame.a),f(3));if(!exact(gui,c(1)))return;
 Record *r=create(gui);if(!r)return;if(!assign(r->state,target(frame.a))||!assign(r->proto,target(frame.b))){fault(-12);return;}
 if(field<void*>(target(r->state),f(4))!=target(r->proto)||(r->response&&target(r->response)!=target(r->proto))){cancel(*r);return;}
 if(!assign(r->dismissed,field<void*>(target(r->gui),f(2)))){fault(-12);return;}
 r->id=field<uint64_t>(target(r->proto),f(0));sh->mark(r->slot,PGR_SH_STAGE_INIT,1|(r->id?2:0),tid());emit(1,r->slot);
}
void hook_scores(void *self,void *value,int32_t berry,const void *info){auto fn=reinterpret_cast<Scores>(original(1));HookFrame frame(self,value);try{fn(frame.a?target(frame.a):self,frame.b?target(frame.b):value,berry,info);}catch(...){if(frame.observe)fault(-11);throw;}
 if(!frame.active()||!exact(target(frame.a),c(1))||!exact(target(frame.b),c(3)))return;Record *r=create(target(frame.a));if(!r)return;if(!assign(r->response,target(frame.b))){fault(-12);return;}if(r->proto&&target(r->proto)!=target(r->response)){cancel(*r);return;}sh->mark(r->slot,PGR_SH_STAGE_SET_SCORES,0,tid());
}
void hook_open(void *self,const void *info){auto fn=reinterpret_cast<One>(original(2));HookFrame frame(self);
 pgr_interaction_owner::Phase phase(15);
 pgr_interaction_owner::Pins pins(phase.wanted(),extra.gchandle_new,extra.gchandle_get_target,extra.gchandle_free,frame.a?target(frame.a):self);
 pgr_interaction_owner::AbortBeforeRoots abort{phase};
 if(pins.valid)phase.enter(pins.at(0));
 try{fn(pins.at(0),info);}catch(...){phase.abort();if(frame.observe)fault(-11);throw;}
 phase.leave();
 if(frame.active()&&exact(target(frame.a),c(1)))if(auto r=create(target(frame.a)))sh->mark(r->slot,PGR_SH_STAGE_OPEN_COMPLETE,0,tid());}
void hook_dismiss(void *self,const void *info){auto fn=reinterpret_cast<One>(original(3));if(ready&&owned()&&!(invoking&&self==invoking_gui))if(auto r=by_gui(self))cancel(*r);fn(self,info);}
void hook_exit(void *self,const void *info){auto fn=reinterpret_cast<One>(original(4));
 pgr_interaction_owner::Phase phase(18);
 pgr_interaction_owner::Pins pins(phase.wanted(),extra.gchandle_new,extra.gchandle_get_target,extra.gchandle_free,self);
 pgr_interaction_owner::AbortBeforeRoots abort{phase};
 if(ready&&owned())for(auto &r:records)if(r.slot&&target(r.state)==pins.at(0))cancel(r);
 if(pins.valid)phase.enter(pins.at(0));
 fn(pins.at(0),info);phase.leave();}

// Optional details hooks and leaf configuration follow below.
template<class T,size_t N>void pointers(T &out,const std::array<void*,N>&p){out={};out.version=1;out.size=sizeof(out);static_assert(sizeof(T)>=8+N*8,"pointer config bounds");std::memcpy(reinterpret_cast<char*>(&out)+8,p.data(),N*8);}
int install_leaves(){
 PgrSharedCatchConfig policy{1,sizeof(policy),4,8000,2500,64,0,0};int rc=sh->install(&policy);if(rc)return rc;
 PgrSummaryCompletionConfig summary{};
 pointers(summary,std::array<void*,19>{cfg.object_get_class,cfg.field_get_value,cfg.runtime_invoke,cfg.object_unbox,c(1),c(2),c(3),c(6),f(3),f(4),f(5),f(0),f(1),f(2),m(5),m(1),extra.gchandle_new,extra.gchandle_get_target,extra.gchandle_free});summary.success_literal=md.success;summary.state_open=2;
 if((rc=sc->install(&summary)))return rc;capabilities=PGR_SHH_SUMMARY;
 if(md.optional[OPT_DETAILS]==PGR_SHARED_OPT_AVAILABLE){
  PgrDetailsCompletionConfig detail{};
  pointers(detail,std::array<void*,32>{cfg.object_get_class,cfg.field_get_value,cfg.runtime_invoke,cfg.object_unbox,extra.gchandle_new,extra.gchandle_get_target,extra.gchandle_free,k(OC_DGUI),k(OC_VIEW),c(3),k(OC_DATA),k(OC_PK),k(OC_ACTION),c(6),k(OC_INT),h(H_DET_LIST_CLASS),m(5),h(H_DET_CLICK),h(H_DET_COUNT),h(H_DET_ITEM),f(1),f(5),f(0),h(H_DET_VIEW_RESP),h(H_DET_ID),h(H_DET_DELEGATE_TARGET),h(H_DET_DATA),h(H_DET_CALLBACK),h(H_DET_ON_DISMISSED),h(H_DET_POST),h(H_DET_INDEX),h(H_DET_LIST)});
  detail.success_literal=md.success;detail.state_open=2;
  if((rc=dc->install(&detail)))return rc;if((rc=dr->install(&detail)))return rc;capabilities.fetch_or(PGR_SHH_DETAILS|PGR_SHH_RECOVERY);
  if(md.optional[OPT_CATCH_CANVAS]==PGR_SHARED_OPT_AVAILABLE){
   PgrCatchCanvasConfig canvas{};pointers(canvas,std::array<void*,14>{cfg.object_get_class,cfg.field_get_value,cfg.runtime_invoke,cfg.object_unbox,extra.gchandle_new,extra.gchandle_get_target,extra.gchandle_free,c(1),k(OC_DGUI),c(6),f(1),m(5),h(H_CC_METHOD),h(H_CC_CODE)});canvas.state_open=2;
   if((rc=cv->install(&canvas)))return rc;capabilities.fetch_or(PGR_SHH_CANVAS);
  }
  if(md.optional[OPT_CATCH_DISCOVERY]==PGR_SHARED_OPT_AVAILABLE&&md.optional[OPT_CATCH_SCENE]==PGR_SHARED_OPT_AVAILABLE){
   PgrCatchDiscoveryConfig discovery{};pointers(discovery,std::array<void*,12>{cfg.object_get_class,cfg.runtime_invoke,extra.gchandle_new,extra.gchandle_get_target,extra.gchandle_free,exports.class_get_type,extra.type_get_object,extra.array_length,h(H_CD_CLASS),k(OC_DGUI),h(H_CD_TOP),h(H_CS_FIND)});
   if((rc=cd->install(&discovery)))return rc;capabilities.fetch_or(PGR_SHH_DISCOVERY);
  }
  if(md.optional[OPT_TRANSFER]==PGR_SHARED_OPT_AVAILABLE&&cfg.string_length){
   PgrTransferFactsConfig facts{};pointers(facts,std::array<void*,34>{extra.gchandle_new,extra.gchandle_get_target,extra.gchandle_free,cfg.object_get_class,cfg.runtime_invoke,cfg.object_unbox,cfg.field_get_value,cfg.string_length,k(OC_PK),k(OC_DISPLAY),c(6),k(OC_INT),h(H_TR_EVENT_CLASS),k(OC_CHAR_SIZE),k(OC_STRING),h(H_TR_SIZE_M),h(H_TR_RARE_M),h(H_TR_SHINY_M),h(H_TR_EVENT_COUNT_M),h(H_DET_ID),h(H_TR_SPECIES),h(H_TR_DISPLAY),h(H_TR_EVENTS),h(H_TR_LIMITED),h(H_TR_ATTACK),h(H_TR_DEFENSE),h(H_TR_STAMINA),h(H_TR_FAVORITE),h(H_TR_EGG),h(H_TR_COSTUME),h(H_TR_ORIG_COSTUME),h(H_TR_BACKGROUND),h(H_TR_ART),h(H_TR_ART_ASSET)});
   facts.xxs=md.opt_ints[I_TR_SIZE_XXS];facts.xxl=md.opt_ints[I_TR_SIZE_XXL];facts.costume_none=md.opt_ints[I_TR_COSTUME_NONE];facts.art_none=md.opt_ints[I_TR_ART_NONE];facts.asset_none=md.opt_ints[I_TR_ASSET_NONE];std::copy_n(md.opt_ints+I_TR_SIZE_XXS,5,facts.sizes);
   if((rc=tf->install(&facts)))return rc;
   PgrTransferExecConfig transfer{};pointers(transfer,std::array<void*,16>{extra.gchandle_new,extra.gchandle_get_target,extra.gchandle_free,cfg.object_get_class,cfg.runtime_invoke,cfg.field_get_value,h(H_TR_GET_M),h(H_TR_RELEASE_M),k(OC_BAG),k(OC_PK),h(H_TR_CB_CLASS),k(OC_RELEASE),h(H_DET_ID),h(H_TR_CB_POKEMON),h(H_TR_CB_BAG),h(H_TR_RESULT_STATUS)});transfer.success_literal=md.opt_ints[I_TR_RESULT_SUCCESS];
   if((rc=tx->install(&transfer)))return rc;
   PgrTransferObserversConfig observers{1,sizeof(observers),h(H_TR_RESULT_CODE),h(H_TR_ERROR_CODE),cfg.metadata.thread_current,extra.gchandle_new,extra.gchandle_get_target,extra.gchandle_free};
   if((rc=table<PgrTransferObserversApi>(pgrazor_transfer_observers_api(1))->install(&observers)))return rc;capabilities.fetch_or(PGR_SHH_TRANSFER);
  }
  if(md.optional[OPT_DIRECT_PRESENTATION]==PGR_SHARED_OPT_AVAILABLE){
   PgrCatchCompletionConfig completion{};pointers(completion,std::array<void*,14>{cfg.object_get_class,cfg.field_get_value,cfg.runtime_invoke,k(OC_VIEW),c(3),k(OC_PK),k(OC_ACTION),h(H_DP_ACTIVE),h(H_DET_DELEGATE_TARGET),h(H_DET_VIEW_RESP),f(5),f(0),h(H_DET_ID),h(H_DP_CALLBACK)});completion.success_literal=md.success;
   if((rc=cc->install(&completion)))return rc;
   PgrCatchConfig capture{1,sizeof(capture),h(H_DP_SHOW_CODE),reinterpret_cast<New>(extra.gchandle_new),reinterpret_cast<Free>(extra.gchandle_free),reinterpret_cast<Get>(extra.gchandle_get_target),reinterpret_cast<void *(*)(void*)>(cfg.object_get_class),k(OC_VIEW),k(OC_ACTION),k(OC_PK)};
   if((rc=bridge->install(&capture)))return rc;capabilities.fetch_or(PGR_SHH_DIRECT);
  }
 }
 return 0;
}
void hide_opening(Record &r){if(!(capabilities&PGR_SHH_CANVAS)||!r.gui||!current(r.epoch)||(gate().flags&PGR_SHH_DISMISS)==0)return;auto req=details_request(r);if(dc->validate(&req)!=1)return;uint64_t token=0;const int rc=cv->hide_opening(r.gui,r.epoch,tid(),&token);if(rc==1)r.canvas=token;}
Record *recover(void *gui){if(!(capabilities&PGR_SHH_RECOVERY)||!pending.response||!pending.returned||!current(pending.epoch)||now()>=pending.expires||invoking||!exact(gui,k(OC_DGUI)))return nullptr;
 if(auto r=by_gui(gui))return r;void *gui_root=hold(gui);if(!gui_root)return nullptr;
 PgrDetailsRecoveryRequest q{1,sizeof(q)};std::memcpy(q.expected_key,&pending.id,8);q.epoch=pending.epoch;q.tid=tid();const auto remaining_now=now();q.remaining_ms=remaining_now<pending.expires?std::min(gate().remaining,pending.expires-remaining_now):0;q.gui=gui_root;q.response=pending.response;
 PgrDetailsRecoveryOutput out{};const int rc=dr->recover(&q,&out);free_root(gui_root);if(rc!=1){dr->release(&out);return nullptr;}
 Record *r=current(q.epoch)?create(nullptr,true):nullptr;if(!r){dr->release(&out);return nullptr;}
 r->id=pending.id;r->gui=out.gui;r->state=out.state;r->response=out.response;r->data=out.data;r->pokemon=out.pokemon;r->dismissed=out.dismissed;root_count.fetch_add(6);out.gui=out.state=out.response=out.data=out.pokemon=out.dismissed=nullptr;dr->release(&out);
 sh->mark(r->slot,PGR_SH_STAGE_INIT,0,tid());sh->mark(r->slot,PGR_SH_STAGE_PRESENT,0,tid());if(field<int32_t>(target(r->gui),f(1))==2)sh->mark(r->slot,PGR_SH_STAGE_OPEN_COMPLETE,0,tid());release_pending();hide_opening(*r);return r;
}
using Present=void(*)(void*,void*,void*,bool,const void*);
void hook_present(void *self,void *pokemon,void *awards,bool flag,const void *info){auto fn=reinterpret_cast<Present>(original(5));HookFrame frame(self,pokemon);void *awards_root=frame.observe?hold(awards,1):nullptr;uint64_t slot=0;
 if(frame.active()&&exact(target(frame.a),k(OC_VIEW))&&exact(target(frame.b),k(OC_PK))){
  for(auto &r:records)if(r.slot&&r.details)cancel(r);
  void *response=field<void*>(target(frame.a),h(H_DET_VIEW_RESP));const uint64_t id=exact(response,c(3))?field<uint64_t>(response,f(0)):0;
  if(exact(response,c(3))&&field<int32_t>(response,f(5))==md.success&&id&&field<uint64_t>(target(frame.b),h(H_DET_ID))==id){
   void *response_root=hold(response);if(response_root){auto r=create(nullptr,true);if(r){r->id=id;r->response=response_root;response_root=nullptr;if(!assign(r->state,target(frame.a))||!assign(r->pokemon,target(frame.b)))fault(-12);slot=r->slot;if(target(pending.response)==target(r->response))release_pending();}free_root(response_root);}
  }
 }
 try{fn(frame.a?target(frame.a):self,frame.b?target(frame.b):pokemon,awards_root?target(awards_root):awards,flag,info);}catch(...){free_root(awards_root);if(frame.observe)fault(-11);throw;}free_root(awards_root);
 if(frame.active())if(auto r=by_slot(slot))sh->mark(r->slot,PGR_SH_STAGE_PRESENT,0,tid());
}
void hook_details_init(void *self,void *data,const void *info){auto fn=reinterpret_cast<Two>(original(6));HookFrame frame(self,data);uint64_t slot=0;
 if(frame.active()&&exact(target(frame.a),k(OC_DGUI))&&exact(target(frame.b),k(OC_DATA))){if(auto old=by_gui(target(frame.a)))cancel(*old);auto callback=field<void*>(target(frame.b),h(H_DET_ON_DISMISSED));auto view=exact(callback,k(OC_ACTION))?field<void*>(callback,h(H_DET_DELEGATE_TARGET)):nullptr;
  for(auto &r:records)if(r.slot&&r.details&&!r.cancelled&&!r.gui&&target(r.state)==view){if(!assign(r.gui,target(frame.a))||!assign(r.data,target(frame.b)))fault(-12);slot=r.slot;break;}}
 try{fn(frame.a?target(frame.a):self,frame.b?target(frame.b):data,info);}catch(...){if(frame.observe)fault(-11);throw;}
 if(frame.active())if(auto r=by_slot(slot)){auto callback=field<void*>(target(r->data),h(H_DET_ON_DISMISSED));if(!callback||field<void*>(target(r->gui),h(H_DET_CALLBACK))!=callback||field<void*>(target(r->gui),h(H_DET_DATA))!=target(r->data)){cancel(*r);return;}
 if(!assign(r->dismissed,callback)){fault(-12);return;}sh->mark(r->slot,PGR_SH_STAGE_INIT,0,tid());hide_opening(*r);}
}
void hook_details_open(void *self,const void *info){auto fn=reinterpret_cast<One>(original(7));HookFrame frame(self);uint64_t slot=0;if(frame.active()){auto r=by_gui(target(frame.a));if(!r)r=recover(target(frame.a));if(r&&r->details){slot=r->slot;hide_opening(*r);}}
 try{fn(frame.a?target(frame.a):self,info);}catch(...){if(frame.observe)fault(-11);throw;}if(frame.active())if(auto r=by_slot(slot)){hide_opening(*r);sh->mark(r->slot,PGR_SH_STAGE_OPEN_COMPLETE,0,tid());}}
void hook_details_exit(void *self,const void *info){auto fn=reinterpret_cast<One>(original(8));if(ready&&owned())for(auto &r:records)if(r.slot&&r.details&&target(r.state)==self)cancel(r);fn(self,info);}
void hook_details_click(void *self,const void *info){auto fn=reinterpret_cast<One>(original(9));if(ready&&owned()&&!invoking)if(auto r=by_gui(self))cancel(*r);fn(self,info);}
int install_hooks(){
 const void *addresses[]={md.method_code[2],md.method_code[3],md.method_code[4],md.method_code[1],md.method_code[0],h(H_DET_PRESENT_CODE),h(H_DET_INIT_CODE),h(H_DET_OPEN_CODE),h(H_DET_EXIT_CODE),h(H_DET_CLICK_CODE)};
 void *replacements[]={reinterpret_cast<void*>(hook_initialize),reinterpret_cast<void*>(hook_scores),reinterpret_cast<void*>(hook_open),reinterpret_cast<void*>(hook_dismiss),reinterpret_cast<void*>(hook_exit),reinterpret_cast<void*>(hook_present),reinterpret_cast<void*>(hook_details_init),reinterpret_cast<void*>(hook_details_open),reinterpret_cast<void*>(hook_details_exit),reinterpret_cast<void*>(hook_details_click)};
 const unsigned count=(capabilities&PGR_SHH_DETAILS)?10:5;
 for(unsigned i=0;i<count;++i){void *trampoline=nullptr;const int rc=pgrazor_hook(const_cast<void*>(addresses[i]),replacements[i],&trampoline);if(trampoline)originals[i]=trampoline;if(rc||!trampoline)return -20;}
 return 0;
}
struct Root {void *value=nullptr;explicit Root(void *object):value(hold(object)){}~Root(){free_root(value);}void *get()const{return target(value);}};
void *invoke(const void *method,void *receiver,void **args,bool *ok){void *exception=nullptr;void *result=reinterpret_cast<void *(*)(const void*,void*,void**,void**)>(cfg.runtime_invoke)(method,receiver,args,&exception);*ok=exception==nullptr;return result;}
void transfer_drain(){if(!(capabilities&PGR_SHH_TRANSFER))return;PgrTransferExecEvent e{};while(tx->next_event(&e)==1){
 auto external=std::find(external_tokens.begin(),external_tokens.end(),e.token);
 if(e.token&&external!=external_tokens.end()){
  if(external_count==external_events.size()){fault(-30);return;}
  external_events[(external_head+external_count++)%external_events.size()]=e;
  if(e.kind!=PGR_TX_EVENT_REQUEST)*external=0;
  continue;
 }
 Record *matched=nullptr;for(auto &r:records)if(r.slot&&r.transfer_token==e.token){matched=&r;break;}
 if(matched&&e.kind!=PGR_TX_EVENT_REQUEST){matched->transfer_complete=e.kind==PGR_TX_EVENT_COMPLETE&&(e.aux&1);matched->transfer_failed=!matched->transfer_complete;uint64_t slot=0;sh->transfer_resolve(matched->key,tid(),&slot);}
 emit(e.kind==PGR_TX_EVENT_REQUEST?5:6,matched?matched->slot:0,e.kind,uint32_t(e.aux));}}
int transfer_ready(){return ready&&!stopping&&!failed&&(capabilities&PGR_SHH_TRANSFER)?1:0;}
int external_submit(const PgrTransferExecRequest *input,uint64_t *token){
 if(token)*token=0;
 if(!input||!token||input->version!=1||input->size!=sizeof(*input))return -1;
 if(!transfer_ready()||!owned()||busy||external_submitting)return -2;
 auto g=gate();if(!current(g.epoch)||!(g.flags&PGR_SHH_TRANSFER_ENABLED))return 0;
 auto slot=std::find(external_tokens.begin(),external_tokens.end(),uint64_t(0));
 const size_t active=std::count_if(external_tokens.begin(),external_tokens.end(),[](uint64_t v){return v!=0;});
 if(slot==external_tokens.end()||external_count+2*(active+1)>external_events.size())return -3;
 PgrTransferExecRequest request=*input;request.epoch=g.epoch;request.tid=tid();
 request.input.protect=g.protect;request.input.keep_all=(g.flags&PGR_SHH_KEEP_ALL)?1:0;
 request.input.keep_count=g.count;request.input.keep_ids=g.ids.data();
 if(transfer_policy->decide(&request.input)!=0)return 0;
 external_submitting=true;
 struct Clear {~Clear(){external_submitting=false;}} clear;
 int rc=0;try{rc=tx->submit(&request,token);}catch(...){if(*token)*slot=*token;transfer_drain();throw;}
 if(*token)*slot=*token;
 transfer_drain();return rc;
}
int external_next_event(PgrTransferExecEvent *out){
 if(!out)return -1;if(!owned())return -2;
 if(!external_count){*out={};return 0;}
 *out=external_events[external_head];external_head=(external_head+1)%external_events.size();--external_count;return 1;
}
bool transfer(Record &r,bool direct=false){auto g=gate();if(!(capabilities&PGR_SHH_TRANSFER)||!(g.flags&PGR_SHH_TRANSFER_ENABLED))return false;
 if(r.transfer_token)return !r.transfer_complete&&!r.transfer_failed&&!(g.flags&PGR_SHH_DISMISS);
 if(r.transfer_checked)return false;r.transfer_checked=true;
 if(!current(r.epoch)||!r.pokemon||!r.state||!r.id)return false;
 Root bag(field<void*>(target(r.state),h(H_TR_BAG)));if(!exact(bag.get(),k(OC_BAG)))return false;
 void *args[]={&r.id};bool ok=false;void *found=invoke(h(H_TR_GET_M),bag.get(),args,&ok);if(!ok||!found)return false;
 Root pokemon(found);if(!exact(pokemon.get(),k(OC_PK))||field<uint64_t>(pokemon.get(),h(H_DET_ID))!=r.id)return false;
 const int32_t species=field<int32_t>(pokemon.get(),h(H_TR_SPECIES));
 if(g.flags&PGR_SHH_KEEP_ALL||std::find(g.ids.begin(),g.ids.begin()+g.count,species)!=g.ids.begin()+g.count){emit(7,r.slot,(g.flags&PGR_SHH_KEEP_ALL)?1:4);return false;}
 Root gm(field<void*>(target(r.state),h(H_TR_GM)));if(!gm.value)return false;
 PgrTransferFactsRequest req{1,sizeof(req),tid(),g.remaining,r.id,pokemon.get(),gm.get(),g.protect,0};PgrTransferFactsOut facts{};
 if(tf->inspect(&req,&facts)!=1||facts.species!=species)return false;
 g=gate();if(!current(r.epoch)||!(g.flags&PGR_SHH_TRANSFER_ENABLED))return false;
 if(direct){PgrCatchCompletionRequest q{1,sizeof(q),r.id,r.epoch,tid(),target(r.state),target(r.response),target(r.pokemon),target(r.dismissed)};if(cc->validate(&q)!=1)return false;}
 else {auto q=details_request(r);if(dc->validate(&q)!=1)return false;}
 if(!current(r.epoch))return false;
 PgrTransferExecRequest request{1,sizeof(request),r.id,r.epoch,tid(),bag.get(),pokemon.get(),{}};
 request.input={1,sizeof(PgrTransferInput),facts.attack,facts.defense,facts.stamina,facts.species,g.protect,facts.known,facts.flags,uint32_t((g.flags&PGR_SHH_KEEP_ALL)!=0),g.count,0,g.ids.data()};
 const int policy=transfer_policy->decide(&request.input);if(policy){emit(7,r.slot,policy);return false;}
 if(r.slot)sh->transfer_bind(r.slot,r.key,tid());
 // No tick/event drain can reenter this owner while submit calls managed code.
 // The compiled observer can synchronously enqueue COMPLETE; correlate before draining.
 const int result=tx->submit(&request,&r.transfer_token);
 if(result!=1&&!r.transfer_token){if(r.slot){uint64_t slot=0;sh->transfer_resolve(r.key,tid(),&slot);}r.transfer_failed=true;emit(7,r.slot,17);}
 transfer_drain();return result==1&&!r.transfer_complete&&!r.transfer_failed&&!(g.flags&PGR_SHH_DISMISS);
}
void discover(){if(!(capabilities&PGR_SHH_DISCOVERY)||!pending.response||!pending.returned||invoking||!current(pending.epoch))return;
 PgrCatchDiscoveryRequest q{1,sizeof(q),tid(),gate().remaining,pending.layer};PgrCatchDiscoveryOutput out{};
 if(pending.layer){if(cd->top(&q,&out)==1&&out.count)recover(target(out.handles[0]));cd->release(&out);}
 const auto n=now();if(!pending.response||n-last_scene<250)return;last_scene=n;
 q.layer=nullptr;if(cd->scene(&q,&out)==1){for(uint32_t i=0;i<out.count&&pending.response;++i)recover(target(out.handles[i]));}cd->release(&out);
}
bool replay(PgrCatchCapture &capture){int32_t values[]={int32_t(capture.confirm),int32_t(capture.persist),capture.open_hyper_training,int32_t(capture.hide_ar),int32_t(capture.post),int32_t(capture.suppress)};
 void *args[]={target(capture.pokemon),&values[0],&values[1],&values[2],target(capture.callback),nullptr,nullptr,&values[3],&values[4],&values[5]};bool ok=false;invoke(h(H_DP_SHOW),target(capture.service),args,&ok);emit(10,0,ok?0:-11);return ok;}
void complete_direct(PgrCatchCapture &capture,bool advanced){if(!capture.service)return;root_count.fetch_add((capture.service?1:0)+(capture.pokemon?1:0)+(capture.callback?1:0));
 Record r{};r.epoch=gate().epoch;r.pokemon=capture.pokemon;capture.pokemon=nullptr;r.dismissed=capture.callback;capture.callback=nullptr;
 auto fallback=[&](){capture.pokemon=r.pokemon;capture.callback=r.dismissed;replay(capture);capture.pokemon=capture.callback=nullptr;};
 if(!pending.response){fallback();release(r);free_root(capture.service);return;}
 r.id=pending.id;r.state=hold(field<void*>(target(r.dismissed),h(H_DET_DELEGATE_TARGET)));r.response=hold(target(pending.response));
 auto request=[&](){return PgrCatchCompletionRequest{1,sizeof(PgrCatchCompletionRequest),r.id,r.epoch,tid(),target(r.state),target(r.response),target(r.pokemon),target(r.dismissed)};};
 auto q=request();const auto g=gate();
 if(!advanced||!(g.flags&PGR_SHH_DISMISS)||!current(r.epoch)||cc->validate(&q)!=1){fallback();release(r);free_root(capture.service);return;}
 transfer(r,true);q=request();uint64_t token=0;cc->acquire(r.epoch,tid(),&token);const int rc=token?cc->complete(&q,token):0;
 if(rc==1){release_pending();emit(11);}else {q=request();if(cc->validate(&q)==1)fallback();}
 release(r);free_root(capture.service);
}
void hide_after(Record &r,uint32_t flags,int after,int result){if((capabilities&PGR_SHH_CANVAS)&&result==1&&(flags&4)&&after!=2&&(gate().flags&PGR_SHH_DISMISS)&&current(r.epoch))cv->hide_dismissed(r.gui,r.epoch,tid(),after);}
void complete_summary(Record &r){auto q=summary_request(r);if(sc->validate(&q)!=1){cancel(r);return;}if(!current(r.epoch))return;
 if(capabilities&PGR_SHH_DETAILS){release_pending();pending.response=hold(target(r.proto));if(!pending.response){fault(-12);return;}pending.id=r.id;pending.epoch=r.epoch;pending.expires=now()+8000;
  if(capabilities&PGR_SHH_DISCOVERY){void *layer=field<void*>(target(r.gui),h(H_CD_LAYER));if(exact(layer,h(H_CD_CLASS)))pending.layer=hold(layer);}}
 bool opened=false;PgrCatchCapture capture{};if((capabilities&PGR_SHH_DIRECT)&&(gate().flags&PGR_SHH_DISMISS)&&pending.response)opened=bridge->begin(target(pending.response),int32_t(tid()))==0;
 r.invoked=true;invoking=true;invoking_gui=target(r.gui);uint64_t token=0;sc->acquire(r.epoch,tid(),&token);PgrSummaryCompletionOutput out{};int rc=0;
 try{if(token)rc=sc->complete(&q,token,&out);}catch(...){if(opened)bridge->end(&capture);invoking=false;invoking_gui=nullptr;complete_direct(capture,false);fault(-11);throw;}
 if(opened)bridge->end(&capture);invoking=false;invoking_gui=nullptr;
 const bool advanced=(out.flags&2)||((out.flags&4)&&out.after!=2);
 complete_direct(capture,rc==1&&advanced);
 if(pending.response){if(rc==1&&advanced){pending.returned=true;discover();}else release_pending();}
 hide_after(r,out.flags,out.after,rc);emit(3,r.slot,rc,out.flags,out.before,out.after);sh->finish(r.slot,advanced?1:0,tid());release(r);
}
void complete_details(Record &r){auto q=details_request(r);if(dc->validate(&q)!=1){cancel(r);return;}if(field<int32_t>(target(r.gui),f(1))!=2){cancel(r);return;}
 if(!current(r.epoch)||opener_depth)return;if(transfer(r))return;
 if(!(gate().flags&PGR_SHH_DISMISS)&&!r.transfer_complete){cancel(r);return;}
 if(!current(r.epoch))return;r.invoked=true;invoking=true;invoking_gui=target(r.gui);uint64_t token=0;dc->acquire(r.epoch,tid(),&token);PgrDetailsCompletionOutput out{};int rc=0;
 try{if(token)rc=dc->complete(&q,token,&out);}catch(...){invoking=false;invoking_gui=nullptr;fault(-11);throw;}invoking=false;invoking_gui=nullptr;
 const bool advanced=(out.flags&2)||((out.flags&4)&&out.after!=2);hide_after(r,out.flags,out.after,rc);emit(4,r.slot,rc,out.flags,out.before,out.after);sh->finish(r.slot,advanced?1:0,tid());release(r);
}
int tick(uint64_t proven,uint64_t sequence){if(!ready)return -1;if(proven!=tid()||!owned())return -2;if(busy)return 0;
 struct Busy {Busy(){busy=true;}~Busy(){busy=false;}} scope;
 ticks.fetch_add(1);poll_transport();const auto g=gate();gates(g);
 if(capabilities&PGR_SHH_TRANSFER){tx->tick(tid(),stopping?1:0);transfer_drain();}
 if(capabilities&PGR_SHH_CANVAS)cv->tick(tid());
 if(pending.response&&(!current(pending.epoch)||now()>=pending.expires))release_pending();
 PgrSharedCatchDecision decision{};sh->select(0,tid(),&decision);drain();
 if(stopping){if(!opener_depth){for(auto &r:records)release(r);release_pending();}return 0;}
 discover();if(!g.remaining||opener_depth||!sequence)return 0;
 const int rc=sh->select(sequence,tid(),&decision);drain();if(rc<0){fault(rc);return rc;}if(decision.action==PGR_SH_ACT_NONE)return 0;
 auto r=by_slot(decision.slot);if(!r||r->cancelled)return 0;if(decision.action==PGR_SH_ACT_ARM){emit(12,r->slot);return 0;}
 if(decision.action!=PGR_SH_ACT_DISMISS)return -3;if(r->details)complete_details(*r);else complete_summary(*r);drain();return 0;
}
int install_leaves();
int install_hooks();
int tick(uint64_t,uint64_t);
int stop(){const bool was=stopping.exchange(true);sh->stop(owner);sc->stop(owner);dc->stop(owner);cc->stop(owner);cv->stop();tx->update(0,0,0,0);if(!was)emit(13);return 0;}
int configure(const PgrSharedHostConfig *input);
// Pull only from54. The manager owns discovery and Unity-thread scheduling; this
// path never probes, attaches a thread, or accepts an unvalidated address.
std::atomic_flag default_busy=ATOMIC_FLAG_INIT;
PgrSharedHostConfig default_config{};
PgrMetaExports default_exports{};
PgrSharedMetaExtraExports default_extra{};
bool default_built=false;
int configure_default(){
 if(default_busy.test_and_set(std::memory_order_acquire))return 1;
 struct Clear{~Clear(){default_busy.clear(std::memory_order_release);}} clear;
 if(!default_built){
  PgrBootBinding b{};const auto boot=table<PgrBootApi>(pgrazor_runtime_bootstrap_api(1));
  const int rc=boot->snapshot(&b);if(rc)return rc>0?1:rc;
  if(!b.handle||!b.module_base||!b.module_size||!b.runtime.thread_current)return -7;
  if(!reinterpret_cast<void *(*)()>(b.runtime.thread_current)())return 1;
  const auto metadata=table<PgrMetaApi>(pgrazor_runtime_metadata_api(1));
  int export_error=0;
  auto symbol=[&](const char *name){char full[80];std::snprintf(full,sizeof(full),"il2cpp_%s",name);void *address=dlsym(b.handle,full);
   if(address){PgrMetaCodeQuery q{1,sizeof(q),address,4,nullptr,0,0};if(metadata->validate_code(&q)!=0){export_error=-7;return static_cast<void*>(nullptr);}}
   return address;};
  PgrSharedMetaExtraExports e{};
  e.field_static_get_value=b.runtime.field_static_get_value;e.gchandle_new=b.runtime.gchandle_new;e.gchandle_get_target=b.runtime.gchandle_get_target;e.gchandle_free=b.runtime.gchandle_free;
  e.class_get_fields=symbol("class_get_fields");e.field_get_name=symbol("field_get_name");e.type_get_object=symbol("type_get_object");e.array_length=symbol("array_length");
  e.method_get_object=symbol("method_get_object");e.runtime_class_init=symbol("runtime_class_init");e.class_get_parent=symbol("class_get_parent");e.array_new=b.runtime.array_new;
  e.gc_wbarrier_set_field=symbol("gc_wbarrier_set_field");e.method_get_from_reflection=symbol("method_get_from_reflection");
  PgrSharedHostConfig c{};c.version=1;c.size=sizeof(c);c.metadata.version=2;c.metadata.size=sizeof(c.metadata);
  c.metadata.binding.version=1;c.metadata.binding.size=sizeof(c.metadata.binding);c.metadata.binding.module_base=b.module_base;c.metadata.binding.module_size=b.module_size;
  c.metadata.thread_current=b.runtime.thread_current;c.object_get_class=b.runtime.object_get_class;c.field_get_value=b.runtime.field_get_value;c.runtime_invoke=b.runtime.runtime_invoke;c.object_unbox=b.runtime.object_unbox;c.string_length=symbol("string_length");
  if(export_error)return export_error;
  default_exports=b.meta;default_extra=e;c.metadata.binding.exports=&default_exports;c.metadata.extra=&default_extra;c.exclusive_hooks=1;c.transport_flags=1;
  default_config=c;default_built=true;
 }
 if(!reinterpret_cast<void *(*)()>(default_config.metadata.thread_current)())return 1;
 default_config.metadata.owner_tid=tid();return configure(&default_config);
}
int configure(const PgrSharedHostConfig *input){
 if(!input)return configure_default();
 if(!input||input->version!=1||input->size!=sizeof(*input)||input->exclusive_hooks!=1||(input->transport_flags&~1u)||!input->object_get_class||!input->field_get_value||!input->runtime_invoke||!input->object_unbox||!input->metadata.binding.exports||!input->metadata.extra)return -1;
 if(input->transport_flags&1){PgrRuntimeTransportSample snapshot{};if(transport->sample(&snapshot)!=0||snapshot.fault)return -6;}
 if(input->metadata.owner_tid!=tid())return -2;
 std::lock_guard<std::mutex> lock(setup_mutex);if(failed)return -3;if(ready)return std::memcmp(input,&accepted_config,sizeof(*input))==0?0:-4;
 cfg=*input;exports=*input->metadata.binding.exports;extra=*input->metadata.extra;cfg.metadata.binding.exports=&exports;cfg.metadata.extra=&extra;
 int rc=table<PgrSharedMetadataApi>(pgrazor_shared_metadata_api(2))->resolve(&cfg.metadata,&md);if(rc)return rc;
 owner=tid();rc=install_leaves();if(rc){failed=true;return rc;}rc=install_hooks();if(rc){failed=true;return rc;}installed=true;accepted_config=*input;ready=true;emit(9);return 0;
}
int update(const PgrSharedHostOptions *input){if(!input||input->version!=1||input->size!=sizeof(*input)||input->flags&~15u||input->protect&~63u||input->reserved||input->keep_count>keep.size()||(input->keep_count&&!input->keep_ids)||!input->epoch)return -1;
 for(uint32_t i=0;i<input->keep_count;++i)if(input->keep_ids[i]<1||input->keep_ids[i]>2000)return -1;
 {std::lock_guard<std::mutex> lock(options_mutex);if(input->epoch<options.epoch)return -2;options=*input;options.keep_ids=nullptr;if(input->keep_count)std::copy_n(input->keep_ids,input->keep_count,keep.begin());const auto n=now();deadline=n&&(input->flags&1)?n+std::min<uint64_t>(2500,input->remaining_ms):0;}
 if(!(input->flags&1)){sh->update(0,0,input->epoch,0);sc->update(0,0,input->epoch,0);dc->update(0,0,input->epoch,0);cv->update(0,0,input->epoch,0);tx->update(0,0,input->epoch,0);cc->update(0,0,input->epoch,0);}return 0;
}
int sample(PgrSharedHostSample *out){if(!out)return -1;*out={1,sizeof(*out),uint32_t(ready.load()),capabilities.load(),uint32_t(stopping.load()),uint32_t(failed.load()),record_count.load(),root_count.load(),owner.load(),ticks.load(),frames.load()};return 0;}
int next_event(PgrSharedHostEvent *out){if(!out)return -1;std::lock_guard<std::mutex> lock(event_mutex);if(!event_count){*out={};return 0;}*out=events[event_head];event_head=(event_head+1)%events.size();--event_count;return 1;}
const PgrSharedMetadataOutput *metadata(){return ready?&md:nullptr;}
void pulse(void*,const PgrUnityPulse *p){if(!p||p->reserved||p->tid<=0)return;auto scheduler=table<PgrUnitySchedulerApi>(pgrazor_unity_scheduler_api(2));if(!scheduler||scheduler->valid(p->generation)!=1)return;const auto previous=pulse_generation;pulse_generation=p->generation;try{tick(uint64_t(p->tid),p->sequence);}catch(...){fault(-11);}pulse_generation=previous;}
const PgrSharedHostApi api{1,sizeof(api),configure,update,tick,stop,sample,next_event,metadata,pulse};
const PgrSharedHostTransferApi transfer_api{2,sizeof(transfer_api),transfer_ready,external_submit,external_next_event};
}
extern "C" const void *pgrazor_shared_host_api(uint32_t version){if(version==1)return &api;if(version==2)return &transfer_api;return nullptr;}

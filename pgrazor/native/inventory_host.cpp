#include "inventory_host.h"
#include "shared_host.h"
#include "hook_engine.h"
#include "catch_canvas.h"
#include "items.h"
#include "buddy_feeding.h"
#include "incubation.h"
#include "incoming_gifts.h"
#include "outgoing_gifts.h"
#include "runtime_transport.h"
#include <atomic>
#include <mutex>
#include <string>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
namespace {
using namespace pgr_shared_meta;
std::mutex config_lock,policy_lock,event_lock;
std::atomic<bool> configured{false},stopping{false},busy{false},force_poll{false},cleanup_poll{false};
std::atomic<uint32_t> caps{0},faults{0},roots{0};std::atomic<uint64_t> owner{0},ticks{0};
PgrSharedMetadataOutput md{};PgrInventoryRuntime rt{};bool binding=false;std::atomic<bool> installed{false};uint32_t flags=0;
bool permission_seen=false;
PgrInventoryHostOptions policy{};std::string rule_text;uint64_t deadline=0,revision=0,rule_generation=0,rule_epoch=0;
pgrazor_items_rules_v1 rules{};std::string event_queue[64];uint32_t event_head=0,event_count=0;uint64_t event_seq=0;
pgrazor_items_sample_v1 old_items{};pgrazor_buddy_sample_v1 old_buddy{};pgrazor_incubation_sample_v1 old_inc{};
const pgrazor_items_api_v1 *items(){return static_cast<const pgrazor_items_api_v1*>(pgrazor_items_api(1));}
const pgrazor_buddy_api_v1 *buddy(){return static_cast<const pgrazor_buddy_api_v1*>(pgrazor_buddy_feeding_api(1));}
const pgrazor_incubation_api_v1 *inc(){return static_cast<const pgrazor_incubation_api_v1*>(pgrazor_incubation_api(1));}
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
void *h(unsigned n){return const_cast<void*>(md.opt_handles[n]);}void *c(unsigned n){return const_cast<void*>(md.opt_classes[n]);}
void *core_m(unsigned n){return const_cast<void*>(md.methods[n]);}void *core_c(unsigned n){return const_cast<void*>(md.classes[n]);}
bool available(unsigned n){return md.optional[n]==PGR_SHARED_OPT_AVAILABLE;}
uint64_t clock_ms(){timespec t{};return clock_gettime(CLOCK_BOOTTIME,&t)==0?uint64_t(t.tv_sec)*1000+uint64_t(t.tv_nsec)/1000000:0;}
std::string quote(const std::string &s){std::string out="\"";for(unsigned char ch:s){if(ch=='"'||ch=='\\'){out+='\\';out+=char(ch);}else if(ch<32){char b[7];snprintf(b,sizeof(b),"\\u%04x",unsigned(ch));out+=b;}else out+=char(ch);}return out+'"';}
int action(uint32_t op,const int64_t *v,uint32_t nv,const PgrRuntimeTransportString *s,uint32_t ns){if(!(flags&1))return 0;int r=transport()->emit(op,v,nv,s,ns);if(r<0)stopping.store(true);return r;}
void text_action(uint32_t op,const std::string &value){PgrRuntimeTransportString s{value.data(),uint32_t(value.size())};action(op,nullptr,0,&s,1);}
void ready_action(uint32_t op,bool yes,const char *message){int64_t v=yes?1:0;PgrRuntimeTransportString s{message,uint32_t(strlen(message))};action(op,&v,1,&s,1);}
void event(const char *kind,const std::string &body=""){
 std::string json="{\"kind\":"+quote(kind)+",\"backend\":\"native\",\"boottimeMs\":"+std::to_string(clock_ms())+(body.empty()?"":","+body)+"}";
 if(json.size()>=65536){stopping.store(true);return;}
 if(flags&1){const char *channel="quick-close";PgrRuntimeTransportString v[]={{channel,uint32_t(strlen(channel))},{json.data(),uint32_t(json.size())}};action(19,nullptr,0,v,2);return;}
 std::lock_guard<std::mutex> lock(event_lock);if(event_count==64){stopping.store(true);return;}event_queue[(event_head+event_count++)%64]=json;
}
void status(const char *kind,uint32_t op,const std::string &message){event(kind,"\"message\":"+quote(message));text_action(op,message);}
void feature_ready(uint32_t bit,const char *kind,uint32_t op,int rc){if(!rc)caps.fetch_or(bit);else faults.fetch_or(bit);event(kind,"\"available\":"+std::string(rc?"false":"true")+",\"reason\":"+quote(rc?"native-metadata-or-install":""));
 const char *ready=bit==PGR_IH_ITEMS?"Waiting for item bag":bit==PGR_IH_BUDDY?"Ready: choose a berry":bit==PGR_IH_SOCIAL?"Ready: tap a friend with a gift":"Ready";
 if(op)ready_action(op,!rc,rc?"Unavailable: native-metadata-or-install":ready);
}
bool attached(uint64_t tid){return tid&&tid==uint64_t(gettid())&&rt.boot.thread_current&&reinterpret_cast<void*(*)()>(rt.boot.thread_current)();}
int bind_default(){
 const auto *sh=static_cast<const PgrSharedHostApi*>(pgrazor_shared_host_api(1));const auto *metadata=sh?sh->metadata():nullptr;if(!metadata)return 1;
 PgrBootBinding b{};const auto *boot=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));if(!boot||boot->snapshot(&b))return 1;
 md=*metadata;rt.boot=b.runtime;auto exp=[&](const char *name){return dlsym(b.handle,(std::string("il2cpp_")+name).c_str());};
 rt.string_length=exp("string_length");rt.object_get_virtual_method=exp("object_get_virtual_method");rt.class_get_type=b.meta.class_get_type;
 rt.type_get_object=exp("type_get_object");rt.runtime_class_init=exp("runtime_class_init");rt.method_get_object=exp("method_get_object");binding=true;return 0;
}
void install_inventory(){
 auto &r=rt.boot;
 if(available(OPT_ITEMS)){
  pgrazor_items_config_v1 cfg{};cfg.version=1;cfg.size=sizeof(cfg);cfg.has_lookup=available(OPT_ITEMS_LOOKUP);
  void *p[]={r.runtime_invoke,r.gchandle_new,r.gchandle_get_target,r.gchandle_free,r.object_get_class,r.object_unbox,r.field_get_value,
   h(H_IT_LIST_M),h(H_IT_COUNT_M),h(H_IT_GET_M),h(H_IT_QUANTITY_M),h(H_IT_EXPIRING_M),h(H_IT_RECYCLE_M),h(H_IT_PROTO_M),
   h(H_IT_INIT_CODE),h(H_IT_LIST_CODE),h(H_IT_RECYCLE_CODE),h(H_IT_RESULT_CODE),h(H_IT_ERROR_CODE),c(OC_ISERVICE),c(OC_IBAG),h(H_IT_DK),h(H_IT_CB),c(OC_IRESPONSE),c(OC_INT),
   h(H_IT_BAG),h(H_IT_ID),h(H_IT_COUNT_F),h(H_IT_RECYCLABLE),h(H_IT_CB_BAG),h(H_IT_PREDICTION),h(H_IT_PROTO_ID),h(H_IT_STATUS)};
  static_assert(sizeof(p)==33*8,"items core");memcpy(&cfg.runtime_invoke,p,sizeof(p));
  if(cfg.has_lookup){void *q[]={rt.class_get_type,rt.type_get_object,r.field_static_get_value,rt.runtime_class_init,rt.method_get_object,
   h(H_IL_FIND_M),h(H_IL_CONTAINER_M),h(H_IL_RESOLVE_M),h(H_IL_CREATE_DEL_M),h(H_IL_USABLE_M),h(H_IL_SHOW_M),h(H_IL_MAP),h(H_IL_CONTRACT),h(H_IL_GUI_CONTRACT),h(H_IL_GUI),h(H_IL_SINGLETON),h(H_IL_INSTANCE),h(H_IL_FUNC_TYPE)};
   static_assert(sizeof(q)==18*8,"items lookup");memcpy(reinterpret_cast<char*>(&cfg)+280,q,sizeof(q));}
  cfg.success=md.opt_ints[I_ITEMS_SUCCESS];feature_ready(PGR_IH_ITEMS,"item-bag-ready",38,items()->install(&cfg));
 }else feature_ready(PGR_IH_ITEMS,"item-bag-ready",38,-1);
 if(available(OPT_BUDDY)&&available(OPT_ITEMS)&&available(OPT_ITEMS_LOOKUP)){
  pgrazor_buddy_config_v1 cfg{};cfg.version=1;cfg.size=sizeof(cfg);
  void *p[]={r.runtime_invoke,r.gchandle_new,r.gchandle_get_target,r.gchandle_free,r.object_get_class,r.object_unbox,rt.class_get_type,rt.type_get_object,r.field_get_value,
   h(H_IL_FIND_M),h(H_IL_CONTAINER_M),h(H_IL_RESOLVE_M),h(H_BD_M_HAS),h(H_BD_M_POKEMON),h(H_BD_M_STATS),h(H_BD_M_REMAINING),h(H_BD_M_FULLNESS),h(H_BD_M_COOLDOWN),h(H_IT_QUANTITY_M),h(H_BD_M_FEED),core_m(5),
   h(H_BD_M_FEED_CODE),h(H_BD_M_RESULT_CODE),h(H_IL_MAP),h(H_BD_CONTRACT),c(OC_BUDDY_7769),c(OC_BUDDY_8109),c(OC_BUDDY_7768),c(OC_BUDDY_PK_PROTO),c(OC_BUDDY_OBS),c(OC_IBAG),core_c(6),c(OC_INT),c(OC_SINGLE),h(H_BD_CALLBACK),c(OC_BUDDY_FEED_OUT),
   h(H_BD_F_RPC),h(H_BD_F_POKEMON),h(H_BD_F_BAG),h(H_BD_F_CB_RPC),h(H_BD_F_RESULT),h(H_BD_F_BUDDY_ID)};
  static_assert(sizeof(p)==42*8,"buddy config");memcpy(&cfg.runtime_invoke,p,sizeof(p));cfg.success=md.opt_ints[I_BUDDY_SUCCESS];feature_ready(PGR_IH_BUDDY,"buddy-ready",27,buddy()->install(&cfg));
 }else feature_ready(PGR_IH_BUDDY,"buddy-ready",27,-1);
 if(available(OPT_INCUBATOR)&&available(OPT_ITEMS)&&available(OPT_ITEMS_LOOKUP)){
  pgrazor_incubation_config_v1 cfg{};cfg.version=1;cfg.size=sizeof(cfg);
  void *p[]={r.runtime_invoke,r.gchandle_new,r.gchandle_get_target,r.gchandle_free,r.object_get_class,r.object_unbox,rt.string_length,r.string_chars,rt.object_get_virtual_method,rt.class_get_type,rt.type_get_object,r.field_get_value,
   h(H_IL_FIND_M),h(H_IL_CONTAINER_M),h(H_IL_RESOLVE_M),h(H_INC_M_INCUBATORS),h(H_INC_M_EGGS),h(H_INC_M_PUT),h(H_INC_M_ENUMERATE),h(H_INC_M_MOVE),h(H_INC_M_CURRENT),h(H_INC_M_DISPOSE),
   h(H_IL_MAP),c(OC_VGP_PLAYER_CONTRACT),c(OC_VGP_PLAYER),c(OC_VGP_BAG_CONTRACT),c(OC_BAG),c(OC_INC_PROTO),c(OC_PK),c(OC_INC_CONTRACT),c(OC_INC_SERVICE),
   h(H_INC_F_ITEM),h(H_INC_F_INC_ID),h(H_INC_F_PK_ID),h(H_INC_F_EXPIRY),h(H_INC_F_EGG),h(H_INC_F_EGG_TYPE),h(H_INC_F_ASSIGNMENT),h(H_INC_F_ID),h(H_INC_F_TARGET),h(H_INC_F_START)};
  static_assert(sizeof(p)==41*8,"inc config");memcpy(&cfg.runtime_invoke,p,sizeof(p));cfg.unlimited=md.opt_ints[I_INC_UNLIMITED];cfg.special=md.opt_ints[I_INC_SPECIAL];cfg.class_boolean=core_c(6);cfg.class_string=c(OC_STRING);
  feature_ready(PGR_IH_INCUBATION,"incubator-ready",34,inc()->install(&cfg));
 }else feature_ready(PGR_IH_INCUBATION,"incubator-ready",34,-1);
 if(caps.load()&PGR_IH_ITEMS)items()->sample(&old_items);if(caps.load()&PGR_IH_BUDDY)buddy()->sample(&old_buddy);if(caps.load()&PGR_IH_INCUBATION)inc()->sample(&old_inc);
}
bool parse_rules(const std::string &text,pgrazor_items_rules_v1 &out){
 out={};out.version=1;out.size=sizeof(out);size_t pos=0;while(pos<text.size()){
  size_t end=text.find(';',pos);if(end==std::string::npos)end=text.size();if(end==pos){pos=end+1;continue;}
  size_t colon=text.find(':',pos);if(colon==std::string::npos||colon>=end||colon==pos||colon-pos>7||end-colon<2||end-colon>7)return false;
  int32_t id=0,cap=0;for(size_t i=pos;i<end;++i){if(i==colon)continue;if(text[i]<'0'||text[i]>'9')return false;auto &v=i<colon?id:cap;v=v*10+(text[i]-'0');}
  if(id<=0||cap>100000||out.count==512)return false;for(uint32_t i=0;i<out.count;++i)if(out.entries[i].id==id)return false;
  out.entries[out.count++]={id,cap};pos=end+1;
 }
 std::sort(out.entries,out.entries+out.count,[](auto a,auto b){return a.id<b.id;});return true;
}
int update(const PgrInventoryHostOptions *p){
 if(!configured.load(std::memory_order_acquire))return -1;
 if(!p||p->version!=1||p->size!=sizeof(*p)||p->reserved||p->item_enabled>1||p->item_allowed>1||p->item_catalog>2||p->buddy_allowed>1||p->incubator_enabled>1||p->incubator_longest>1||p->incubator_allowed>1||p->gift_mask>7||p->gift_allowed>1||p->lease_ms>2500){std::lock_guard<std::mutex> lock(policy_lock);deadline=0;++revision;return -2;}
 if(stopping.load())return -3;
 size_t len=p->item_rules?strnlen(p->item_rules,8193):0;if(len>8192){std::lock_guard<std::mutex> lock(policy_lock);deadline=0;++revision;return -2;}
 std::string text=p->item_rules?std::string(p->item_rules,len):"";pgrazor_items_rules_v1 next{};bool valid=parse_rules(text,next);if(!valid){next={};next.version=1;next.size=sizeof(next);}
 next.enabled=p->item_enabled&&valid;uint64_t time=clock_ms();if(!time){stopping.store(true);return -4;}
 bool permission_changed=false;
 {std::lock_guard<std::mutex> lock(policy_lock);
 permission_changed=!permission_seen||policy.gift_mask!=p->gift_mask||policy.gift_allowed!=p->gift_allowed;permission_seen=true;
 bool changed=next.enabled!=rules.enabled||next.count!=rules.count||memcmp(next.entries,rules.entries,sizeof(next.entries));
 if(changed)++rule_generation;if(changed||!p->item_allowed||time>=deadline)++rule_epoch;
 next.generation=rule_generation;next.epoch=rule_epoch;rules=next;rule_text=text;policy=*p;policy.item_rules=nullptr;deadline=time+p->lease_ms;++revision;}
 if(permission_changed)event("gift-permission","\"mask\":"+std::to_string(p->gift_mask)+",\"allowed\":"+(p->gift_allowed?"true":"false"));return 0;
}
void poll_policy(){
 if(!(flags&1))return;auto tr=transport();PgrRuntimeTransportSample a{},z{};PgrInventoryHostOptions p{};p.version=1;p.size=sizeof(p);p.lease_ms=100;
 auto b=[&](unsigned i){int v=0;return tr->get_bool(i,&v)==0&&v==1;};auto n=[&](unsigned i){int32_t v=0;return tr->get_int(i,&v)==0?v:0;};
 if(tr->sample(&a)||!a.snapshot_fresh){p.lease_ms=0;update(&p);return;}
 p.item_enabled=b(17);p.item_allowed=b(16);int mode=n(3);p.item_catalog=mode>=0&&mode<=2?mode:0;p.buddy_allowed=b(8);p.buddy_berry=n(1);
 p.incubator_enabled=b(4);p.incubator_longest=b(13);p.incubator_allowed=b(14);int mask=n(2);p.gift_mask=mask>=0&&mask<=7?mask:0;p.gift_allowed=b(12);
 char text[8193]{};uint32_t length=0;if(tr->get_string(3,text,sizeof(text),&length)||length>8192){p.item_enabled=0;text[0]=0;}p.item_rules=text;
 if(tr->sample(&z)||!z.snapshot_fresh||a.snapshot_seq!=z.snapshot_seq)p.lease_ms=0;update(&p);
}
std::string number(double value){char text[32];snprintf(text,sizeof(text),"%.15g",value);return text;}
void drain_inventory(int32_t berry){
 if(caps.load()&PGR_IH_ITEMS){pgrazor_items_sample_v1 v{};if(!items()->sample(&v)){
  if(v.bag_ready_serial!=old_items.bag_ready_serial)event("item-bag-ready");
  if(v.catalog_serial!=old_items.catalog_serial){pgrazor_item_row row[1024]{};uint32_t n=0;if(!items()->catalog(row,1024,&n)&&n<=1024){std::string rows;
   for(uint32_t i=0;i<n;++i){std::string name="Item "+std::to_string(row[i].id);for(uint32_t k=0;k<md.item_name_count&&k<kItemNameMax;++k)if(md.item_names[k].value==row[i].id){name=md.item_names[k].name;break;}
    if(i)rows+='\n';rows+=std::to_string(row[i].id)+"|"+name+"|"+std::to_string(row[i].count);}
   event("item-catalog","\"rows\":"+quote(rows));text_action(37,rows);
  }}
  if(v.events_lost!=old_items.events_lost){event("item-transfer-error","\"reason\":\"Native item event queue overflow; recycling paused\"");text_action(39,"Item transfer: Native item event queue overflow; recycling paused");}old_items=v;
 }
 pgrazor_item_event records[64]{};uint32_t n=0;if(!items()->drain(records,64,&n)&&n<=64)for(uint32_t i=0;i<n;++i){auto &v=records[i];std::string body="\"id\":"+std::to_string(v.id)+",\"amount\":"+std::to_string(v.amount);
  if(v.kind==1)event("item-transfer-requested",body+",\"keep\":"+std::to_string(v.aux));
  else if(v.kind==2){event("item-transfer-confirmed",body);text_action(39,"Transferred "+std::to_string(v.amount)+" items");}
  else if(v.kind==3){const char *reason=v.aux==1?"Timed out; item paused until app restart":v.aux==2?"Game rejected recycling; item paused until settings change":"native";event(v.aux==4?"item-lookup-error":"item-transfer-error",body+",\"reason\":"+quote(reason));if(v.aux!=4)text_action(39,"Item transfer: "+std::string(reason));}
 }
 }
 if(caps.load()&PGR_IH_BUDDY){pgrazor_buddy_sample_v1 v{};if(!buddy()->sample(&v)){
  const char *msg[]={"","Off","Waiting for the map","Choose a buddy first","Waiting for buddy hunger data","","Waiting for buddy feeding cooldown","Feeding paused after six berries; check your buddy","Selected berry is out of stock","Feeding buddy","Berry fed; checking buddy hunger","","Buddy feed timed out; no automatic retry","Hunger did not update; toggle Off to retry","Buddy feeding paused: native"};
  if(v.status_serial!=old_buddy.status_serial){std::string m=v.status<15?msg[v.status]:"";if(v.status==5)m="Buddy fed - "+std::to_string(v.detail)+" min remaining";if(v.status==11)m="Buddy feed failed (result "+std::to_string(v.detail)+"); toggle Off to retry";if(!m.empty())status("buddy-status",28,m);}
  if(v.service_serial!=old_buddy.service_serial)event("buddy-service-ready");
  if(v.feed_request_serial!=old_buddy.feed_request_serial)event("buddy-feed-request","\"berry\":"+std::to_string(berry)+",\"count\":1");
  if(v.result_serial!=old_buddy.result_serial)force_poll.store(true);
  if(v.fault_serial!=old_buddy.fault_serial)event("buddy-error","\"reason\":\"native\"");old_buddy=v;
 }}
 if(caps.load()&PGR_IH_INCUBATION){pgrazor_incubation_sample_v1 v{};if(!inc()->sample(&v)){
  const char *msg[]={"","Off","Waiting for the map","Egg incubating","Incubator already in use","Assignment unconfirmed; waiting for inventory","Confirming egg assignment","Incubation stopped; toggle off/on to retry","Unlimited incubator in use or unavailable","No eggs waiting","","Incubation unavailable; toggle off/on to retry"};
  if(v.status_serial!=old_inc.status_serial){std::string m=v.status<12?msg[v.status]:"";if(v.status==10)m="Starting "+number(v.distance)+" km egg";if(!m.empty())status("incubator-status",35,m);}
  if(v.request_serial!=old_inc.request_serial)event("incubator-request","\"distance\":"+(std::isfinite(v.distance)?std::to_string(v.distance):"null")+",\"priority\":"+quote(v.longest?"longest":"shortest"));
  if(v.fault_serial!=old_inc.fault_serial)event("incubator-error","\"reason\":\"native\"");old_inc=v;
 }}
}
const PgrIncomingGiftsApi *incoming(){return static_cast<const PgrIncomingGiftsApi*>(pgrazor_incoming_gifts_api(1));}
const PgrOutgoingGiftsApi *outgoing(){return static_cast<const PgrOutgoingGiftsApi*>(pgrazor_outgoing_gifts_api(1));}
std::atomic<uint32_t> social_roots{0},social_frames{0};std::atomic<bool> visibility_wired{false};
void *profile=nullptr,*open_gui=nullptr,*guest_open=nullptr;bool guest_fired=false,profile_checked=false,profile_failed=false;
std::string friend_id;uint64_t profile_next=0,profile_deadline=0,dialog_serial=0;uint32_t gift_mask=0;uint64_t gift_deadline=0;
void *object_class(void *p){return p?reinterpret_cast<void*(*)(void*)>(rt.boot.object_get_class)(p):nullptr;}
bool exact(void *p,void *klass){return p&&object_class(p)==klass;}
void *hold(void *p){if(!p)return nullptr;void *r=reinterpret_cast<void*(*)(void*,int)>(rt.boot.gchandle_new)(p,0);if(r)social_roots.fetch_add(1);return r;}
void *target(void *r){return r?reinterpret_cast<void*(*)(void*)>(rt.boot.gchandle_get_target)(r):nullptr;}
void drop(void *&r){void *old=r;r=nullptr;if(old){reinterpret_cast<void(*)(void*)>(rt.boot.gchandle_free)(old);social_roots.fetch_sub(1);}}
struct Root {void *r;explicit Root(void *p):r(hold(p)){}~Root(){drop(r);}void *get(){return target(r);}};
void *invoke(void *method,void *self,void **args=nullptr){if(!method)return nullptr;void *exception=nullptr;void *result=reinterpret_cast<void*(*)(void*,void*,void**,void**)>(rt.boot.runtime_invoke)(method,self,args,&exception);if(exception){profile_failed=true;event("gift-error","\"reason\":\"native-managed-exception\"");text_action(32,"Gift automation paused: native-managed-exception");return nullptr;}return result;}
void *read_ref(void *p,void *field){void *out=nullptr;if(p&&field)reinterpret_cast<void(*)(void*,void*,void*)>(rt.boot.field_get_value)(p,field,&out);return out;}
int32_t read_int(void *p,void *field){int32_t out=0;if(p&&field)reinterpret_cast<void(*)(void*,void*,void*)>(rt.boot.field_get_value)(p,field,&out);return out;}
bool boolean(void *method,void *self,void **args=nullptr){void *v=invoke(method,self,args);return exact(v,core_c(6))&&*reinterpret_cast<uint8_t*>(reinterpret_cast<void*(*)(void*)>(rt.boot.object_unbox)(v))!=0;}
bool alive(void *p){void *args[]={p};return p&&boolean(core_m(5),nullptr,args);}
bool is_open(void *root){void *p=target(root);return alive(p)&&read_int(target(root),const_cast<void*>(md.fields[1]))==2;}
std::string managed_text(void *p){if(!exact(p,c(OC_STRING)))return "";int n=reinterpret_cast<int(*)(void*)>(rt.string_length)(p);if(n<1||n>256)return "";auto chars=reinterpret_cast<const uint16_t*(*)(void*)>(rt.boot.string_chars)(p);if(!chars)return "";std::string result;for(int i=0;i<n;++i){uint32_t u=chars[i];if(u>=0xd800&&u<=0xdbff&&i+1<n&&chars[i+1]>=0xdc00&&chars[i+1]<=0xdfff){u=0x10000+((u-0xd800)<<10)+(chars[++i]-0xdc00);}if(u<128)result+=char(u);else if(u<2048){result+=char(0xc0|(u>>6));result+=char(0x80|(u&63));}else if(u<65536){result+=char(0xe0|(u>>12));result+=char(0x80|((u>>6)&63));result+=char(0x80|(u&63));}else{result+=char(0xf0|(u>>18));result+=char(0x80|((u>>12)&63));result+=char(0x80|((u>>6)&63));result+=char(0x80|(u&63));}}return result;}
std::string identity(void *f){if(!exact(f,c(OC_FRIEND_DETAILS)))return "";void *p=read_ref(f,h(H_GF_F_PLAYER));if(!exact(p,c(OC_PLAYER_SUMMARY)))return "";return managed_text(read_ref(p,h(H_GF_F_PLAYER_ID)));}
bool observation_allowed(){uint64_t tid=gettid();if(stopping.load()||tid!=owner.load()||!attached(tid)||ticks.load()<3)return false;
 if(busy.load())return true;auto api=static_cast<const PgrUnitySchedulerApi3*>(pgrazor_unity_scheduler_api(3));return api&&api->executing_generation(tid)!=0;}
void reset_profile(){if(profile&&(caps.load()&PGR_IH_OUTGOING))outgoing()->observe(0,target(profile),nullptr,gettid());drop(open_gui);drop(profile);friend_id.clear();profile_checked=false;profile_failed=false;profile_next=profile_deadline=0;}
void incoming_ready(void *gui){if(!exact(gui,c(OC_GIFT_6240)))return;if(caps.load()&PGR_IH_INCOMING)incoming()->observe(1,gui,gettid());if(profile){drop(open_gui);open_gui=hold(gui);}}
void social_observe(unsigned kind,void *self,void *arg=nullptr){
 if(kind==0){reset_profile();profile=hold(self);profile_next=clock_ms()+200;}
 else if(kind==1){if(target(profile)==self)reset_profile();}
 else if(kind==2)incoming_ready(self);
 else if(kind==3){if(caps.load()&PGR_IH_INCOMING)incoming()->observe(2,self,gettid());}
 else if(kind==4){if(caps.load()&PGR_IH_INCOMING)incoming()->observe(3,self,gettid());if(target(open_gui)==self)drop(open_gui);profile_checked=true;profile_deadline=0;profile_next=clock_ms()+600;}
 else if(kind==5){if(caps.load()&PGR_IH_INCOMING)incoming()->observe(4,self,gettid());}
 else if(kind==6||kind==7){if(caps.load()&PGR_IH_OUTGOING)outgoing()->observe(kind==6?1:2,self,arg,gettid());}
}
std::mutex hook_lock;void *originals[9]{};bool social_hooks=false,needs_visibility=false;
void *original(unsigned i){std::lock_guard<std::mutex> lock(hook_lock);return originals[i];}
struct HookFrame {HookFrame(){social_frames.fetch_add(1);}~HookFrame(){social_frames.fetch_sub(1);}};
template<unsigned I,bool AFTER> void hook_zero(void *self,void *info){
 auto orig=reinterpret_cast<void(*)(void*,void*)>(original(I));if(!observation_allowed()){orig(self,info);return;}HookFrame frame;Root root(self);
 if(!root.get()){orig(self,info);return;}if(!AFTER)social_observe(I,root.get());orig(root.get(),info);
 if(AFTER&&!stopping.load()&&root.get())social_observe(I,root.get());
}
void hook_result(void *self,void *arg,void *info){auto orig=reinterpret_cast<void(*)(void*,void*,void*)>(original(6));if(!observation_allowed()){orig(self,arg,info);return;}
 HookFrame frame;Root self_root(self),arg_root(arg);if(!self_root.get()||(arg&&!arg_root.get())){orig(self_root.get()?self_root.get():self,arg_root.get()?arg_root.get():arg,info);return;}
 social_observe(6,self_root.get(),arg_root.get());orig(self_root.get(),arg_root.get(),info);
}
uint8_t hook_sequence(void *self,void *info){auto orig=reinterpret_cast<uint8_t(*)(void*,void*)>(original(5));if(!observation_allowed())return orig(self,info);HookFrame frame;Root root(self);
 if(!root.get())return orig(self,info);social_observe(5,root.get());return orig(root.get(),info);
}
void hook_choose(void *self,void *info){auto orig=reinterpret_cast<void(*)(void*,void*)>(original(8));if(!observation_allowed()||!(caps.load()&PGR_IH_OUTGOING)){orig(self,info);return;}
 HookFrame frame;Root root(self);if(!root.get()){orig(self,info);return;}
 uint64_t t=clock_ms(),left=t&&t<gift_deadline?gift_deadline-t:0;outgoing()->update(gift_mask,left,gettid());int r=outgoing()->queue(root.get(),target(profile),gettid());if(r==0)orig(root.get(),info);
}
void visibility(void *gui,uint64_t tid){if(tid!=uint64_t(gettid())||!observation_allowed()||!needs_visibility||!exact(gui,c(OC_GIFT_6240)))return;if(target(guest_open)==gui)return;drop(guest_open);guest_open=hold(gui);guest_fired=false;}
int visibility_filter(void*,void *gui,uint64_t tid){return tid==uint64_t(gettid())&&observation_allowed()&&needs_visibility&&exact(gui,c(OC_GIFT_6240))?1:0;}
void visibility_enter(void*,void *gui,uint64_t tid){visibility(gui,tid);}
void bind_visibility(){if(!needs_visibility||visibility_wired.load())return;auto api=static_cast<const PgrCatchCanvasApi2*>(pgrazor_catch_canvas_api(2));
 if(api){PgrCatchCanvasObserver observer{1,sizeof(observer),visibility_filter,visibility_enter,nullptr};if(api->subscribe(&observer)==0)visibility_wired.store(true);}}
int visibility_site(uint32_t ready){if(ready>1)return -2;visibility_wired.store(ready!=0);return 0;}
void install_social(){
 if(!available(OPT_GIFTS)){feature_ready(PGR_IH_SOCIAL,"gift-ready",33,-1);return;}auto &r=rt.boot;
 PgrIncomingGiftsConfig in{};in.version=1;in.size=sizeof(in);
 void *a[]={r.gchandle_new,r.gchandle_get_target,r.gchandle_free,r.object_get_class,r.object_unbox,r.runtime_invoke,r.field_get_value,r.field_set_value,rt.string_length,r.string_chars,
 h(H_GF_M_OPEN),h(H_GF_M_ACTIVE),h(H_GF_M_INTERACTABLE),core_m(5),c(OC_GIFT_6240),c(OC_GIFT_BOX_DETAILS),c(OC_OPEN_GIFT_OUT),c(OC_GIFT_OPEN_SEQ),core_c(6),c(OC_STRING),const_cast<void*>(md.fields[1]),h(H_GF_F_OPEN_DETAILS),h(H_GF_F_SENDER),h(H_GF_F_OPEN_BTN),h(H_GF_F_SEQ_GUI),h(H_GF_F_SEQ_RESP),h(H_GF_F_SEQ_STATE),h(H_GF_F_OPEN_RESULT)};
 static_assert(sizeof(a)==28*8,"incoming");memcpy(&in.gchandle_new,a,sizeof(a));in.state_open=2;in.open_success=md.opt_ints[I_GIFT_OPEN_SUCCESS];feature_ready(PGR_IH_INCOMING,"native-incoming-gifts-ready",0,incoming()->install(&in));
 PgrOutgoingGiftsConfig out{};out.version=1;out.size=sizeof(out);
 void *b[]={r.gchandle_new,r.gchandle_get_target,r.gchandle_free,r.object_get_class,r.object_unbox,rt.object_get_virtual_method,r.runtime_invoke,r.field_get_value,r.field_set_value,rt.string_length,r.string_chars,
 h(H_GF_M_PANEL),h(H_GF_M_DISABLED),h(H_GF_M_ACTIVE),h(H_GF_M_INTERACTABLE),h(H_GF_M_SENDABLE),h(H_GF_M_GET_ENUM),h(H_GF_M_MOVE_GIFT),h(H_GF_M_CUR_GIFT),h(H_GF_M_DISP_GIFT),h(H_GF_M_NIA_ID),h(H_GF_M_INVENTORY),h(H_GF_M_STICKER_COUNT),h(H_GF_M_STICKER_ITEM),h(H_GF_M_QUANTITY),h(H_GF_M_RESERVE),h(H_GF_M_RETURN_STICKER),h(H_GF_M_RPC_SEND),core_m(5),
 c(OC_GIFT_5936),c(OC_FRIEND_DETAILS),c(OC_PLAYER_SUMMARY),c(OC_GIFT_6154),c(OC_GIFT_6149),c(OC_GIFT_6164),c(OC_GIFT_BOX_PROTO),c(OC_STICKER_PROTO),h(H_GF_STICKER_LIST_CLASS),c(OC_SEND_GIFT_OUT),h(H_GF_CB_CLASS),core_c(6),c(OC_INT),c(OC_STRING),
 h(H_GF_F_DATA),h(H_GF_F_PANEL_FRIEND),h(H_GF_F_PLAYER),h(H_GF_F_PLAYER_ID),h(H_GF_F_CHOOSE_BTN),h(H_GF_F_CHECK_PENDING),h(H_GF_F_CHECK_RESULT),h(H_GF_F_PANEL_GIFTING),h(H_GF_F_PANEL_RPC),h(H_GF_F_NIA_UTIL),h(H_GF_F_RPC_STICKERS),h(H_GF_F_GIFT_ID),h(H_GF_F_STICKER_ID),h(H_GF_F_SEND_RESULT),h(H_GF_F_CB_SVC),h(H_GF_F_CB_ID)};
 static_assert(sizeof(b)==59*8,"outgoing");memcpy(&out.gchandle_new,b,sizeof(b));out.check_success=md.opt_ints[I_GIFT_CHECK_SUCCESS];out.send_success=md.opt_ints[I_GIFT_SEND_SUCCESS];feature_ready(PGR_IH_OUTGOING,"native-outgoing-gifts-ready",0,outgoing()->install(&out));
 // A RET-only validated HandleOpenComplete uses the shared canvas visibility owner.
 uint32_t instruction=0;memcpy(&instruction,h(H_GF_M_OPEN_READY_CODE),4);needs_visibility=instruction==0xd65f03c0;
 void *addresses[]={h(H_GF_M_PROF_OPEN_CODE),h(H_GF_M_PROF_CLOSE_CODE),h(H_GF_M_OPEN_READY_CODE),h(H_GF_M_OPEN_CODE),h(H_GF_M_OPEN_CLOSE_CODE),h(H_GF_M_SEQ_CODE),h(H_GF_M_RPC_RESULT_CODE),h(H_GF_M_RPC_FINALLY_CODE),h(H_GF_M_CHOOSE_CODE)};
 void *hooks[]={reinterpret_cast<void*>(hook_zero<0,true>),reinterpret_cast<void*>(hook_zero<1,false>),reinterpret_cast<void*>(hook_zero<2,true>),reinterpret_cast<void*>(hook_zero<3,false>),reinterpret_cast<void*>(hook_zero<4,false>),reinterpret_cast<void*>(hook_sequence),reinterpret_cast<void*>(hook_result),reinterpret_cast<void*>(hook_zero<7,true>),reinterpret_cast<void*>(hook_choose)};
 std::lock_guard<std::mutex> lock(hook_lock);for(unsigned i=0;i<9;++i){if(i==2&&needs_visibility)continue;if(!addresses[i]||pgrazor_hook(addresses[i],hooks[i],&originals[i])!=0){faults.fetch_or(PGR_IH_SOCIAL);return;}}
 social_hooks=true;
}
void drain_social(){
 if(caps.load()&PGR_IH_INCOMING){PgrIncomingGiftsSample s{};if(!incoming()->sample(&s)&&s.dialog_serial!=dialog_serial){dialog_serial=s.dialog_serial;event("gift-dialog","\"source\":\"incoming\",\"profilePresent\":"+std::string(profile?"true":"false"));}
 PgrIncomingGiftsEvent e{};for(unsigned n=0;n<64&&incoming()->drain(&e)==1;++n){
  static const char *wait[]={"native-wait","Auto open is off","Waiting for foreground readiness","Waiting for dialog to finish opening","Waiting for incoming gift details","Waiting for Open button"};
  static const char *err[]={"native","native-open-exception","native-gc-handle","native-event-overflow"};
  if(e.kind==1){const char *reason=e.reason<6?wait[e.reason]:wait[0];event("gift-wait","\"reason\":"+quote(reason)+",\"mask\":"+std::to_string(gift_mask));text_action(32,reason);}
  else if(e.kind==2){profile_checked=true;status("gift-status",32,"Opening gift");}
  else if(e.kind==3){const char *reason=e.reason<4?err[e.reason]:err[0];event("gift-error","\"reason\":"+quote(reason));text_action(32,"Gift automation paused: "+std::string(reason));}
  else if(e.kind==4)event("gift-presentation-skipped","\"kindOfGift\":\"received\"");
 }}
 if(caps.load()&PGR_IH_OUTGOING){PgrOutgoingGiftsEvent e{};for(unsigned n=0;n<64&&outgoing()->next_event(&e)==1;++n){
  static const char *msg[]={"","Preparing gift","Gift sending is not available for this friend yet","No gifts available","Sending gift","Gift sent","Gift send failed; no automatic retry","","Gift result pending; no automatic retry","Gift result uncertain; no automatic retry","Gift send already pending"};
  static const char *err[]={"native-outgoing","gift-recipient-missing","gift-service-type","gift-inventory","gift-id-zero","gift-nia","gift-sticker-service","gift-sticker-id","gift-gc-handle","gift-invoke","sticker-release","gift-event-overflow"};
  if(e.kind==1){std::string m=e.id>=0&&e.id<11?msg[e.id]:"";if(e.id==7)m="Gift send failed (result "+std::to_string(e.aux)+")";if(!m.empty())status("gift-status",32,m);}
  else if(e.kind==2)event("gift-bundled-request","\"sticker\":"+std::string(e.aux==1?"true":"false"));
  else if(e.kind==3)event("gift-bundled-result","\"success\":"+std::string(e.aux&1?"true":"false")+",\"result\":"+((e.aux&2)?std::to_string(e.id):"null"));
  else if(e.kind==4){const char *reason=e.id>=0&&e.id<12?err[e.id]:err[0];event("gift-error","\"reason\":"+quote(reason));text_action(32,"Gift automation paused: "+std::string(reason));}
 }}
}
void social_tick(uint64_t time,uint64_t tid,uint64_t lease,const PgrInventoryHostOptions &p){
 bind_visibility();
 bool enabled=social_hooks&&(!needs_visibility||visibility_wired.load())&&!stopping.load();
 if(enabled&&!(caps.load()&PGR_IH_SOCIAL))feature_ready(PGR_IH_SOCIAL,"gift-ready",33,0);
 gift_mask=enabled?p.gift_mask:0;gift_deadline=time+(enabled&&p.gift_allowed?lease:0);
 if(guest_open){void *gui=target(guest_open);if(stopping.load()||!alive(gui)||(guest_fired&&read_int(target(guest_open),const_cast<void*>(md.fields[1]))!=2)){drop(guest_open);guest_fired=false;}
  else if(!guest_fired&&read_int(target(guest_open),const_cast<void*>(md.fields[1]))==2){guest_fired=true;incoming_ready(target(guest_open));}}
 if(caps.load()&PGR_IH_INCOMING){incoming()->update(gift_mask,enabled&&p.gift_allowed?lease:0);incoming()->tick(tid,stopping.load()?1:0);}
 if(caps.load()&PGR_IH_OUTGOING){outgoing()->update(gift_mask,enabled&&p.gift_allowed?lease:0,tid);outgoing()->tick(tid,stopping.load()?1:0);}
 drain_social();if(stopping.load()){drop(guest_open);reset_profile();return;}
 if(!enabled||!profile||!lease||!p.gift_allowed||!(gift_mask&3)||profile_failed||time<profile_next)return;profile_next=time+150;
 if(!alive(target(profile))){reset_profile();return;}Root panel(invoke(h(H_GF_M_PANEL),target(profile)));if(!exact(panel.get(),c(OC_GIFT_5936))||!alive(panel.get()))return;
 void *data=read_ref(panel.get(),h(H_GF_F_DATA));if(!data)return;std::string id=identity(read_ref(data,h(H_GF_F_PANEL_FRIEND)));if(id.empty())return;
 if(id!=friend_id){drop(open_gui);friend_id=id;profile_checked=false;profile_deadline=0;profile_next=time+600;return;}
 if(profile_deadline&&time>profile_deadline){profile_failed=true;drop(open_gui);status("gift-status",32,"Gift flow paused; finish manually or reopen this friend");return;}
 if(((caps.load()&PGR_IH_INCOMING)&&incoming()->is_open(tid)==1)||is_open(open_gui))return;
 if(!is_open(profile)||boolean(h(H_GF_M_DISABLED),target(profile)))return;
 if((gift_mask&1)&&!profile_checked){profile_checked=true;profile_next=time+1200;if(boolean(h(H_GF_M_CHECK),panel.get())){profile_deadline=time+30000;profile_next=time+500;}}
}
void refresh_roots(){uint32_t n=social_roots.load()+social_frames.load();
 if(caps.load()&PGR_IH_ITEMS){pgrazor_items_sample_v1 s{};n+=items()->sample(&s)?1:s.owned_roots;}
 if(caps.load()&PGR_IH_BUDDY){pgrazor_buddy_sample_v1 s{};n+=buddy()->sample(&s)?1:s.owned_roots;}
 if(caps.load()&PGR_IH_INCUBATION){pgrazor_incubation_sample_v1 s{};n+=inc()->sample(&s)?1:s.owned_roots;}
 if(caps.load()&PGR_IH_INCOMING){PgrIncomingGiftsSample s{};n+=incoming()->sample(&s)?1:s.root_count;}
 if(caps.load()&PGR_IH_OUTGOING){PgrOutgoingGiftsSample s{};n+=outgoing()->sample(&s)?1:s.roots;}roots.store(n);
}
int configure(const PgrInventoryHostConfig *cfg){
 if(cfg&&(cfg->version!=1||cfg->size!=sizeof(*cfg)||cfg->reserved||(cfg->flags&~1u)||!cfg->metadata||!cfg->runtime||cfg->metadata->version!=2||cfg->metadata->size!=sizeof(md)))return -2;
 std::lock_guard<std::mutex> lock(config_lock);if(configured.load())return -3;
 if(cfg){md=*cfg->metadata;rt=*cfg->runtime;flags=cfg->flags;binding=true;}else flags=1;configured.store(true,std::memory_order_release);return 0;
}
int tick(uint64_t tid){
 if(!configured.load(std::memory_order_acquire))return -1;if(tid!=uint64_t(gettid())||!tid)return -5;if(busy.exchange(true))return -6;struct Guard{~Guard(){busy.store(false);}} guard;
 if(!binding&&bind_default())return 1;if(!attached(tid))return -5;uint64_t zero=0;owner.compare_exchange_strong(zero,tid);if(owner.load()!=tid)return -5;uint64_t count=ticks.fetch_add(1)+1;
 if(!installed&&!stopping.load()){installed=true;install_inventory();install_social();}
 if(!installed)return stopping.load()?-3:1;
 if(!stopping.load())poll_policy();PgrInventoryHostOptions p{};pgrazor_items_rules_v1 local_rules{};uint64_t expires=0;
 {std::lock_guard<std::mutex> lock(policy_lock);p=policy;local_rules=rules;expires=deadline;}
 uint64_t time=clock_ms();uint64_t lease=time&&time<expires&&!stopping.load()&&count>=3?expires-time:0;
 if(caps.load()&PGR_IH_ITEMS){if(stopping.load())items()->clear(tid);else if(!(faults.load()&PGR_IH_ITEMS)){uint32_t force=(force_poll.exchange(false)?1:0)|(cleanup_poll.exchange(false)?9:0)|(p.item_catalog==2?2:p.item_catalog==1?4:0);if(items()->tick(&local_rules,force,p.item_allowed?lease:0,tid)){faults.fetch_or(PGR_IH_ITEMS);items()->clear(tid);}}}
 int32_t berry=(p.buddy_berry==701||p.buddy_berry==703||p.buddy_berry==705||p.buddy_berry==706||p.buddy_berry==707)?p.buddy_berry:0;
 if(caps.load()&PGR_IH_BUDDY){if(stopping.load())buddy()->clear(tid);else if(!(faults.load()&PGR_IH_BUDDY)&&buddy()->tick(berry,p.buddy_allowed?lease:0,tid)){faults.fetch_or(PGR_IH_BUDDY);buddy()->clear(tid);}}
 if(caps.load()&PGR_IH_INCUBATION){if(stopping.load())inc()->clear(tid);else if(!(faults.load()&PGR_IH_INCUBATION)&&inc()->tick(p.incubator_enabled,p.incubator_longest,p.incubator_allowed?lease:0,tid)){faults.fetch_or(PGR_IH_INCUBATION);inc()->clear(tid);}}
 drain_inventory(berry);social_tick(time,tid,lease,p);refresh_roots();return 0;
}
int stop(){stopping.store(true);{std::lock_guard<std::mutex> lock(policy_lock);deadline=0;++revision;}
 // Owner reentry must revoke the leaf currently inside a managed call, not wait
 // for the next host pulse. Leaves defer frees under their own active frames.
 const uint64_t tid=gettid();if(owner.load()==tid&&attached(tid)){
  if(caps.load()&PGR_IH_ITEMS)items()->clear(tid);
  if(caps.load()&PGR_IH_BUDDY)buddy()->clear(tid);
  if(caps.load()&PGR_IH_INCUBATION)inc()->clear(tid);
 }
 if(caps.load()&PGR_IH_INCOMING)incoming()->update(0,0);if(caps.load()&PGR_IH_OUTGOING)outgoing()->update(0,0,gettid());return 0;}
int sample(PgrInventoryHostSample *s){if(!s)return -2;refresh_roots();*s={1,sizeof(*s),uint32_t(configured.load()),uint32_t(installed.load()&&!stopping.load()&&caps.load()==63&&faults.load()==0),caps.load(),uint32_t(stopping.load()),roots.load(),faults.load(),owner.load(),ticks.load()};return 0;}
int next_event(PgrInventoryHostEvent *out){if(!out)return -2;std::lock_guard<std::mutex> lock(event_lock);if(!event_count)return 1;*out={};out->version=1;out->size=sizeof(*out);out->sequence=++event_seq;auto &s=event_queue[event_head];memcpy(out->json,s.c_str(),s.size()+1);s.clear();event_head=(event_head+1)%64;--event_count;return 0;}
void request_item_poll(){force_poll.store(true);}void *acquire_item_service(){return (caps.load()&PGR_IH_ITEMS)&&!stopping.load()&&owner.load()==uint64_t(gettid())?items()->service_acquire():nullptr;}
void on_pulse(void*,const PgrUnityPulse *pulse){auto api=static_cast<const PgrUnitySchedulerApi*>(pgrazor_unity_scheduler_api(2));if(pulse&&api&&api->valid(pulse->generation)==1)tick(pulse->tid);}
bool cleanup_available(){
 if(!configured.load()||!installed.load()||stopping.load()||!(caps.load()&PGR_IH_ITEMS)||(faults.load()&PGR_IH_ITEMS))return false;
 std::lock_guard<std::mutex> lock(policy_lock);
 return policy.item_enabled&&policy.item_allowed&&deadline>clock_ms();
}
uint64_t request_cleanup(){
 if(owner.load()!=uint64_t(gettid())||!cleanup_available())return 0;
 uint64_t ticket=pgrazor_items_cleanup_serial()+1;
 cleanup_poll.store(true);
 return ticket;
}
int cleanup_pending(uint64_t ticket){
 return ticket&&cleanup_available()&&pgrazor_items_cleanup_serial()<ticket?1:0;
}
const PgrInventoryCleanupApi cleanup_api{2,sizeof(PgrInventoryCleanupApi),request_cleanup,cleanup_pending};
const PgrInventoryHostApi api{1,sizeof(PgrInventoryHostApi),configure,update,tick,stop,sample,next_event,request_item_poll,acquire_item_service,on_pulse,visibility_site,visibility};
}
extern "C" const void *pgrazor_inventory_host_api(uint32_t version){return version==1?static_cast<const void*>(&api):version==2?static_cast<const void*>(&cleanup_api):nullptr;}

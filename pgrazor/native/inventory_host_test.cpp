// Executes ACTUAL production65 and real14/12/9 from dlopened core. Only complete56
// metadata output and managed world are synthetic, supplied via the native config.
#include "inventory_host.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <unistd.h>
#include <map>
#include <sys/wait.h>
using namespace pgr_shared_meta;
namespace {
int class_tokens[160]{},handle_tokens[PGR_OPT_HANDLE_COUNT]{};int calls=0,original_calls=0;bool is_attached=true;
struct Object{void *klass;unsigned char bytes[16];} boxed_bool{&class_tokens[6],{1}},map_type{&class_tokens[19],{}};
std::map<uintptr_t,void*> handles;uintptr_t next_root=10;
void *thread_current(){return is_attached?&class_tokens[0]:nullptr;}
void *gc_new(void *obj,int){if(!obj)return nullptr;handles[++next_root]=obj;return reinterpret_cast<void*>(next_root);}
void *gc_get(void *root){auto i=handles.find(reinterpret_cast<uintptr_t>(root));return i==handles.end()?nullptr:i->second;}
void gc_free(void *root){assert(handles.erase(reinterpret_cast<uintptr_t>(root))==1);}
void *object_class(void *obj){return obj?static_cast<Object*>(obj)->klass:nullptr;}void *unbox(void *o){return static_cast<Object*>(o)->bytes;}
Object item_service{},item_bag{},item_list{},item_row{},item_expiry{},item_callback{},item_prediction{},item_proto{},item_response{},item_box{};
bool item_world=false;int item_count=0,item_quantity=50,item_recycles=0,item_lists=0,item_gets=0;
void *invoke(void *method,void*,void**,void **exc){
 *exc=nullptr;++calls;if(method==&handle_tokens[H_IL_FIND_M])return nullptr;
 if(item_world){
  if(method==&handle_tokens[H_IT_LIST_M]){++item_lists;return &item_list;}
  if(method==&handle_tokens[H_IT_COUNT_M]||method==&handle_tokens[H_IT_QUANTITY_M]){int v=method==&handle_tokens[H_IT_COUNT_M]?item_count:item_quantity;memcpy(item_box.bytes,&v,4);return &item_box;}
  if(method==&handle_tokens[H_IT_GET_M]){++item_gets;return &item_row;}
  if(method==&handle_tokens[H_IT_EXPIRING_M])return &item_expiry;
  if(method==&handle_tokens[H_IT_RECYCLE_M]){++item_recycles;return nullptr;}
  if(method==&handle_tokens[H_IT_PROTO_M])return &item_proto;
 }
 assert(false&&"unexpected managed mutation");return nullptr;
}
void field_get(void*,void *field,void *out){
 if(item_world){
  if(field==&handle_tokens[H_IT_BAG]||field==&handle_tokens[H_IT_CB_BAG]){*static_cast<void**>(out)=&item_bag;return;}
  if(field==&handle_tokens[H_IT_PREDICTION]){*static_cast<void**>(out)=&item_prediction;return;}
  if(field==&handle_tokens[H_IT_RECYCLABLE]){*static_cast<uint8_t*>(out)=1;return;}
  int v=field==&handle_tokens[H_IT_ID]||field==&handle_tokens[H_IT_PROTO_ID]?701:field==&handle_tokens[H_IT_STATUS]?1:item_quantity;
  *static_cast<int*>(out)=v;return;
 }
 *static_cast<void**>(out)=nullptr;
}void field_set(void*,void*,void*){assert(false);}
void static_get(void*,void *out){*static_cast<void**>(out)=nullptr;}
void *type(void *p){return p;}void *type_obj(void*){return &map_type;}void class_init(void*){}void *reflection(void*,void*){return nullptr;}
void *virtual_method(void*,void *m){return m;}int string_length(void*){return 0;}const uint16_t *string_chars(void*){return nullptr;}
#define PAD asm volatile("nop; nop; nop; nop; nop; nop; nop; nop":::"memory")
__attribute__((noinline)) void init(void*,void*){PAD;++original_calls;}
__attribute__((noinline)) void *list(void*,void*,int32_t,void*,void*,void*,void*){PAD;++original_calls;return nullptr;}
__attribute__((noinline)) void *recycle(void*,void*,int32_t,void*,void*){PAD;++original_calls;return nullptr;}
__attribute__((noinline)) void result(void*,void*,void*){PAD;++original_calls;}
__attribute__((noinline)) void error(void*,void*,void*){PAD;++original_calls;asm volatile("nop");}
__attribute__((noinline)) void *feed(void*,int32_t,int32_t,void*){PAD;++original_calls;return nullptr;}
__attribute__((noinline)) void feed_result(void*,void*,void*){PAD;++original_calls;asm volatile("nop; nop");}
}
int inventory_run(int argc,char **argv){
 assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib)puts(dlerror());assert(lib);auto get=reinterpret_cast<const void*(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(get);
 auto core=static_cast<const pgrazor_native_api_v2*>(get(2));auto api=static_cast<const PgrInventoryHostApi*>(core->query_feature(65,1));assert(api&&api->size==96&&!core->query_feature(65,3));
 auto cleanup=static_cast<const PgrInventoryCleanupApi*>(core->query_feature(65,2));
 assert(cleanup&&cleanup->version==2&&cleanup->size==sizeof(*cleanup));
 assert(cleanup->request()==0&&!cleanup->pending(1));
 PgrSharedMetadataOutput md{};md.version=2;md.size=sizeof(md);for(unsigned i=0;i<kClassCount;++i)md.classes[i]=&class_tokens[i];for(unsigned i=0;i<kOptClassCount;++i)md.opt_classes[i]=&class_tokens[i+7];
 for(unsigned i=0;i<PGR_OPT_HANDLE_COUNT;++i)md.opt_handles[i]=&handle_tokens[i];
 md.optional[OPT_ITEMS]=md.optional[OPT_ITEMS_LOOKUP]=md.optional[OPT_BUDDY]=md.optional[OPT_INCUBATOR]=1;
 md.opt_handles[H_IT_INIT_CODE]=reinterpret_cast<void*>(init);md.opt_handles[H_IT_LIST_CODE]=reinterpret_cast<void*>(list);md.opt_handles[H_IT_RECYCLE_CODE]=reinterpret_cast<void*>(recycle);
 md.opt_handles[H_IT_RESULT_CODE]=reinterpret_cast<void*>(result);md.opt_handles[H_IT_ERROR_CODE]=reinterpret_cast<void*>(error);md.opt_handles[H_BD_M_FEED_CODE]=reinterpret_cast<void*>(feed);md.opt_handles[H_BD_M_RESULT_CODE]=reinterpret_cast<void*>(feed_result);
 md.methods[5]=&class_tokens[159];md.opt_ints[I_ITEMS_SUCCESS]=1;md.opt_ints[I_INC_UNLIMITED]=901;md.opt_ints[I_INC_SPECIAL]=1;
 PgrInventoryRuntime rt{};
 rt.boot.runtime_invoke=reinterpret_cast<void*>(invoke);rt.boot.thread_current=reinterpret_cast<void*>(thread_current);rt.boot.gchandle_new=reinterpret_cast<void*>(gc_new);rt.boot.gchandle_get_target=reinterpret_cast<void*>(gc_get);rt.boot.gchandle_free=reinterpret_cast<void*>(gc_free);
 rt.boot.object_get_class=reinterpret_cast<void*>(object_class);rt.boot.object_unbox=reinterpret_cast<void*>(unbox);rt.boot.field_get_value=reinterpret_cast<void*>(field_get);rt.boot.field_set_value=reinterpret_cast<void*>(field_set);rt.boot.field_static_get_value=reinterpret_cast<void*>(static_get);rt.boot.string_chars=reinterpret_cast<void*>(string_chars);
 rt.string_length=reinterpret_cast<void*>(string_length);rt.object_get_virtual_method=reinterpret_cast<void*>(virtual_method);rt.class_get_type=reinterpret_cast<void*>(type);rt.type_get_object=reinterpret_cast<void*>(type_obj);rt.runtime_class_init=reinterpret_cast<void*>(class_init);rt.method_get_object=reinterpret_cast<void*>(reflection);
 PgrInventoryHostConfig cfg{1,sizeof(cfg),0,0,&md,&rt};auto bad=cfg;bad.flags=2;assert(api->configure(&bad)==-2);assert(api->configure(&cfg)==0);
 PgrInventoryHostOptions p{};p.version=1;p.size=sizeof(p);p.lease_ms=1500;p.item_rules="701:10;705:20";assert(api->update(&p)==0);assert(cleanup->request()==0&&!cleanup->pending(1));
 assert(api->tick(gettid()+1)==-5);assert(api->tick(gettid())==0);assert(api->tick(gettid())==0);assert(api->tick(gettid())==0);
 PgrInventoryHostSample s{};assert(api->sample(&s)==0);assert((s.capabilities&7)==7&&!s.ready&&s.roots==0);assert(calls==0);
 // Real7 Dobby-installed leaf sites still chain their manual originals exactly once.
 decltype(&init) volatile init_call=&init;decltype(&feed) volatile feed_call=&feed;init_call(nullptr,nullptr);feed_call(nullptr,701,1,nullptr);assert(original_calls==2);
 // Real native incubation resolves the map via configured56 slots, reports waiting,
 // balances the temporary System.Type root, and issues no assignment without a map.
 p.incubator_enabled=1;p.incubator_allowed=1;assert(api->update(&p)==0);assert(api->tick(gettid())==0);assert(calls>0&&handles.empty());
 // The leaf leaves its prior status unchanged when FindObjectOfType returns null.
 // WAIT_MAP is specifically the zero-lease path, not an absent-map diagnostic.
 auto invalid=p;invalid.lease_ms=2501;assert(api->update(&invalid)==-2);assert(api->tick(gettid())==0);
 bool waiting=false;PgrInventoryHostEvent e{};while(api->next_event(&e)==0)if(strstr(e.json,"Waiting for the map"))waiting=true;assert(waiting);
 // Actual65 cleanup tickets drive actual14, including unchanged empty passes and pending RPCs.
 item_world=true;item_service.klass=const_cast<void*>(md.opt_classes[OC_ISERVICE]);item_bag.klass=const_cast<void*>(md.opt_classes[OC_IBAG]);
 item_row.klass=const_cast<void*>(md.opt_handles[H_IT_DK]);item_callback.klass=const_cast<void*>(md.opt_handles[H_IT_CB]);
 item_box.klass=const_cast<void*>(md.opt_classes[OC_INT]);item_response.klass=const_cast<void*>(md.opt_classes[OC_IRESPONSE]);
 p.incubator_enabled=p.incubator_allowed=0;p.item_enabled=p.item_allowed=1;p.lease_ms=2500;
 assert(api->update(&p)==0);
 decltype(&list) volatile list_call=&list;list_call(&item_service,nullptr,0,nullptr,nullptr,nullptr,nullptr);
 item_quantity=0;auto empty=cleanup->request();assert(empty&&cleanup->pending(empty));
 assert(api->tick(gettid())==0);assert(!cleanup->pending(empty)&&item_lists==0&&!item_recycles);
 // A second unchanged empty pass must independently satisfy its new ticket.
 auto empty_again=cleanup->request();assert(empty_again>empty&&cleanup->pending(empty_again));
 assert(api->tick(gettid())==0);assert(!cleanup->pending(empty_again)&&item_lists==0);
 uint64_t foreign_ticket=99;std::thread request_foreign([&]{foreign_ticket=cleanup->request();});request_foreign.join();assert(foreign_ticket==0);
 item_count=1;item_quantity=50;auto delayed=cleanup->request();assert(delayed&&api->tick(gettid())==0);
 assert(item_recycles==1&&cleanup->pending(delayed));
 for(int i=0;i<4;++i){assert(api->tick(gettid())==0);assert(cleanup->pending(delayed)&&item_recycles==1);}
 decltype(&result) volatile result_call=&result;item_quantity=10;result_call(&item_callback,&item_response,nullptr);
 api->request_item_poll();assert(api->tick(gettid())==0);assert(!cleanup->pending(delayed)&&item_recycles==1);
 // Automatic no-work cleanup checks only selected counts, regardless of total inventory size.
 item_count=257;int lists_before=item_lists,gets_before=item_gets;auto large=cleanup->request();
 assert(large&&cleanup->pending(large));assert(api->tick(gettid())==0);
 assert(!cleanup->pending(large)&&item_gets==gets_before&&item_lists==lists_before&&item_recycles==1);
 // The explicit inventory settings catalog still enumerates the full bag.
 p.item_catalog=2;assert(api->update(&p)==0);api->request_item_poll();
 for(int frame=0;frame<300&&item_gets-gets_before<257;++frame){int before=item_gets;assert(api->tick(gettid())==0);assert(item_gets-before<=64);}
 assert(item_gets-gets_before==257&&item_lists-lists_before==1);
 std::thread foreign([&]{assert(api->stop()==0);assert(api->tick(gettid())==-5);});foreign.join();assert(api->tick(gettid())==0);assert(api->sample(&s)==0&&s.stopping&&!s.roots);assert(handles.empty());
 puts("PASS actual65 + real14/12/9 installs/manual originals/disabled gates/native incubation missing-map/invalid policy/foreign stop/root cleanup. Synthetic56 metadata; social flow covered in child process.");return 0;
}
// Full gift orchestration in a fresh process, including the actual48 execution
// inspector and all nine65 Dobby-owned observer/replacement methods.
namespace social_fixture {
int classes[160]{},tokens[PGR_OPT_HANDLE_COUNT]{},current_state_token,alive_token,execute_token;
struct Obj {void *klass=nullptr;alignas(8) unsigned char value[8]{};std::map<int,uint64_t> fields;uint16_t chars[32]{};int length=0;};
Obj profile,panel,data,friend_obj,player,text,button,open_gui,details,response,sequence,service,rpc,sticker_service,gift,sticker,sticker_text,sticker_list,iterable,enumerator,nia,callback,send_response,bool_box,int_box,promise;
std::map<uintptr_t,Obj*> live;uintptr_t root_serial=100;int originals[9]{},open_calls=0,send_calls=0,reserves=0,returns=0,work=0,enum_moves=0;uint8_t sequence_return=0;
const PgrInventoryHostApi *host=nullptr;
void *k(unsigned n){return &classes[n+7];}void *h(unsigned n){return &tokens[n];}
void *thread(){return &classes[0];}void *new_root(void *o,int){if(!o)return nullptr;live[++root_serial]=static_cast<Obj*>(o);return reinterpret_cast<void*>(root_serial);}
void *get_root(void *r){auto i=live.find(reinterpret_cast<uintptr_t>(r));return i==live.end()?nullptr:i->second;}void free_root(void *r){assert(live.erase(reinterpret_cast<uintptr_t>(r))==1);}
void *object_class(void *o){return o?static_cast<Obj*>(o)->klass:nullptr;}void *unbox(void *o){return static_cast<Obj*>(o)->value;}
int field_id(void *f){return f==&current_state_token?-1:int(static_cast<int*>(f)-tokens);}
size_t field_size(int id){return id==H_GF_F_CHECK_PENDING?1:id==-1||id==H_GF_F_CHECK_RESULT||id==H_GF_F_OPEN_RESULT||id==H_GF_F_SEQ_STATE||id==H_GF_F_SEND_RESULT?4:8;}
void read(void *o,void *f,void *out){assert(o);int id=field_id(f);auto &fields=static_cast<Obj*>(o)->fields;assert(fields.count(id));uint64_t v=fields[id];memcpy(out,&v,field_size(id));}
void write(void *o,void *f,void *value){int id=field_id(f);assert(o&&id==H_GF_F_SEQ_STATE);uint64_t v=0;memcpy(&v,value,field_size(id));static_cast<Obj*>(o)->fields[id]=v;}
void ref(Obj &o,int id,Obj &v){o.fields[id]=reinterpret_cast<uintptr_t>(&v);}void scalar(Obj &o,int id,uint64_t v){o.fields[id]=v;}
void string(Obj &o,const char *v){o.klass=k(OC_STRING);o.length=int(strlen(v));for(int i=0;i<o.length;++i)o.chars[i]=uint16_t(v[i]);}
int length(void *o){return static_cast<Obj*>(o)->length;}const uint16_t *chars(void *o){return static_cast<Obj*>(o)->chars;}
void *box(bool v){bool_box.klass=&classes[6];bool_box.value[0]=v?1:0;return &bool_box;}void *integer(int32_t v){int_box.klass=k(OC_INT);memcpy(int_box.value,&v,4);return &int_box;}
void *virtual_method(void*,void *m){return m;}
#define SPAD asm volatile("nop; nop; nop; nop; nop; nop; nop; nop":::"memory")
__attribute__((noinline)) void prof_open(void*,void*){SPAD;++originals[0];}
__attribute__((noinline)) void prof_close(void*,void*){SPAD;++originals[1];}
__attribute__((noinline)) void ready(void*,void*){SPAD;++originals[2];}
__attribute__((noinline)) void manual_open(void*,void*){SPAD;++originals[3];}
__attribute__((noinline)) void close(void*,void*){SPAD;++originals[4];}
__attribute__((noinline)) uint8_t seq(void*,void*){SPAD;++originals[5];return 1;}
__attribute__((noinline)) void result(void*,void*,void*){SPAD;++originals[6];}
__attribute__((noinline)) void final(void*,void*){SPAD;++originals[7];}
__attribute__((noinline)) void choose(void*,void*){SPAD;++originals[8];}
decltype(&prof_open) volatile call_prof_open=prof_open;decltype(&prof_close) volatile call_prof_close=prof_close;decltype(&ready) volatile call_ready=ready;
decltype(&manual_open) volatile call_open=manual_open;decltype(&close) volatile call_close=close;decltype(&seq) volatile call_seq=seq;
decltype(&result) volatile call_result=result;decltype(&final) volatile call_final=final;decltype(&choose) volatile call_choose=choose;
__attribute__((noinline)) void *invoke(const void *method,void *self,void **args,void **exception){SPAD;*exception=nullptr;
 if(method==&execute_token){int action=work;work=0;
  if(action==1)call_prof_open(&profile,nullptr);if(action==2)call_ready(&open_gui,nullptr);
  if(action==3)sequence_return=call_seq(&sequence,nullptr);if(action==4)call_close(&open_gui,nullptr);
  if(action==5)call_choose(&panel,nullptr);if(action==6)call_prof_close(&profile,nullptr);if(action==7)call_open(&open_gui,nullptr);
  return nullptr;
 }
 if(method==&alive_token)return box(true);
 if(method==h(H_GF_M_PANEL)){assert(self==&profile);return &panel;}
 if(method==h(H_GF_M_DISABLED))return box(false);
 if(method==h(H_GF_M_ACTIVE)||method==h(H_GF_M_INTERACTABLE))return box(true);
 if(method==h(H_GF_M_CHECK))return box(false);
 if(method==h(H_GF_M_OPEN)){assert(self==&open_gui);++open_calls;call_open(&open_gui,nullptr);return nullptr;}
 if(method==h(H_GF_M_SENDABLE))return &iterable;
 if(method==h(H_GF_M_GET_ENUM)){enum_moves=0;return &enumerator;}
 if(method==h(H_GF_M_MOVE_GIFT))return box(enum_moves++==0);
 if(method==h(H_GF_M_CUR_GIFT))return &gift;
 if(method==h(H_GF_M_DISP_GIFT))return nullptr;
 if(method==h(H_GF_M_NIA_ID))return &text;
 if(method==h(H_GF_M_INVENTORY))return &sticker_list;
 if(method==h(H_GF_M_STICKER_COUNT)||method==h(H_GF_M_QUANTITY))return integer(1);
 if(method==h(H_GF_M_STICKER_ITEM))return &sticker;
 if(method==h(H_GF_M_RESERVE)){++reserves;return nullptr;}
 if(method==h(H_GF_M_RETURN_STICKER)){++returns;return nullptr;}
 if(method==h(H_GF_M_RPC_SEND)){assert(self==&rpc&&args);++send_calls;call_result(&callback,&send_response,nullptr);call_final(&callback,nullptr);return &promise;}
 fprintf(stderr,"Unexpected social managed method %p self %p\n",method,self);assert(false);return nullptr;
}
__attribute__((noinline)) void *convert(const void *m,void *self,void **args,int,void **exc){SPAD;return invoke(m,self,args,exc);}
decltype(&invoke) volatile call_invoke=invoke;
int verify(const PgrUnitySchedulerConfig *cfg){return cfg->execute_tasks==&execute_token?1:0;}
void pulse(int action=0){work=action;void *exc=nullptr;call_invoke(&execute_token,nullptr,nullptr,&exc);assert(!exc);}
void pause_ms(unsigned ms){usleep(ms*1000);}
void initialize(){
 profile.klass=k(OC_GIFT_5916);panel.klass=k(OC_GIFT_5936);friend_obj.klass=k(OC_FRIEND_DETAILS);player.klass=k(OC_PLAYER_SUMMARY);open_gui.klass=k(OC_GIFT_6240);details.klass=k(OC_GIFT_BOX_DETAILS);response.klass=k(OC_OPEN_GIFT_OUT);sequence.klass=k(OC_GIFT_OPEN_SEQ);
 service.klass=k(OC_GIFT_6154);rpc.klass=k(OC_GIFT_6149);sticker_service.klass=k(OC_GIFT_6164);gift.klass=k(OC_GIFT_BOX_PROTO);sticker.klass=k(OC_STICKER_PROTO);sticker_list.klass=h(H_GF_STICKER_LIST_CLASS);callback.klass=h(H_GF_CB_CLASS);send_response.klass=k(OC_SEND_GIFT_OUT);
 string(text,"fixture-player");string(sticker_text,"fixture-sticker");scalar(profile,-1,2);scalar(open_gui,-1,2);
 ref(panel,H_GF_F_DATA,data);ref(data,H_GF_F_PANEL_FRIEND,friend_obj);ref(friend_obj,H_GF_F_PLAYER,player);ref(player,H_GF_F_PLAYER_ID,text);
 ref(panel,H_GF_F_CHOOSE_BTN,button);scalar(panel,H_GF_F_CHECK_PENDING,0);scalar(panel,H_GF_F_CHECK_RESULT,7);ref(panel,H_GF_F_PANEL_GIFTING,service);ref(panel,H_GF_F_PANEL_RPC,rpc);ref(panel,H_GF_F_NIA_UTIL,nia);
 ref(rpc,H_GF_F_RPC_STICKERS,sticker_service);scalar(gift,H_GF_F_GIFT_ID,99);ref(sticker,H_GF_F_STICKER_ID,sticker_text);ref(callback,H_GF_F_CB_SVC,rpc);ref(callback,H_GF_F_CB_ID,text);scalar(send_response,H_GF_F_SEND_RESULT,1);
 ref(open_gui,H_GF_F_OPEN_DETAILS,details);ref(details,H_GF_F_SENDER,text);ref(open_gui,H_GF_F_OPEN_BTN,button);ref(sequence,H_GF_F_SEQ_GUI,open_gui);ref(sequence,H_GF_F_SEQ_RESP,response);scalar(sequence,H_GF_F_SEQ_STATE,0);scalar(response,H_GF_F_OPEN_RESULT,5);
}
int run(const char *library){
 void *lib=dlopen(library,RTLD_NOW|RTLD_LOCAL);assert(lib);auto get=reinterpret_cast<const void*(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(get);auto core=static_cast<const pgrazor_native_api_v2*>(get(2));host=static_cast<const PgrInventoryHostApi*>(core->query_feature(65,1));assert(host);
 initialize();PgrSharedMetadataOutput md{};md.version=2;md.size=sizeof(md);for(unsigned i=0;i<kClassCount;++i)md.classes[i]=&classes[i];for(unsigned i=0;i<kOptClassCount;++i)md.opt_classes[i]=k(i);for(unsigned i=0;i<PGR_OPT_HANDLE_COUNT;++i)md.opt_handles[i]=h(i);md.fields[1]=&current_state_token;md.methods[5]=&alive_token;md.optional[OPT_GIFTS]=1;
 md.opt_ints[I_GIFT_OPEN_SUCCESS]=5;md.opt_ints[I_GIFT_SEND_SUCCESS]=1;md.opt_ints[I_GIFT_CHECK_SUCCESS]=7;
 md.opt_handles[H_GF_M_PROF_OPEN_CODE]=reinterpret_cast<void*>(prof_open);md.opt_handles[H_GF_M_PROF_CLOSE_CODE]=reinterpret_cast<void*>(prof_close);md.opt_handles[H_GF_M_OPEN_READY_CODE]=reinterpret_cast<void*>(ready);md.opt_handles[H_GF_M_OPEN_CODE]=reinterpret_cast<void*>(manual_open);md.opt_handles[H_GF_M_OPEN_CLOSE_CODE]=reinterpret_cast<void*>(close);md.opt_handles[H_GF_M_SEQ_CODE]=reinterpret_cast<void*>(seq);md.opt_handles[H_GF_M_RPC_RESULT_CODE]=reinterpret_cast<void*>(result);md.opt_handles[H_GF_M_RPC_FINALLY_CODE]=reinterpret_cast<void*>(final);md.opt_handles[H_GF_M_CHOOSE_CODE]=reinterpret_cast<void*>(choose);
 PgrInventoryRuntime rt{};rt.boot.runtime_invoke=reinterpret_cast<void*>(invoke);rt.boot.thread_current=reinterpret_cast<void*>(thread);rt.boot.gchandle_new=reinterpret_cast<void*>(new_root);rt.boot.gchandle_get_target=reinterpret_cast<void*>(get_root);rt.boot.gchandle_free=reinterpret_cast<void*>(free_root);rt.boot.object_get_class=reinterpret_cast<void*>(object_class);rt.boot.object_unbox=reinterpret_cast<void*>(unbox);rt.boot.field_get_value=reinterpret_cast<void*>(read);rt.boot.field_set_value=reinterpret_cast<void*>(write);rt.boot.string_chars=reinterpret_cast<void*>(chars);rt.string_length=reinterpret_cast<void*>(length);rt.object_get_virtual_method=reinterpret_cast<void*>(virtual_method);
 PgrInventoryHostConfig config{1,sizeof(config),0,0,&md,&rt};assert(host->configure(&config)==0);
 PgrInventoryHostOptions p{};p.version=1;p.size=sizeof(p);p.gift_mask=7;p.gift_allowed=1;p.lease_ms=2500;assert(host->update(&p)==0);assert(host->update(&p)==0);
 const auto *scheduler=static_cast<const PgrUnitySchedulerApi*>(core->query_feature(48,2));assert(scheduler);
 PgrUnitySchedulerConfig cfg{};cfg.version=2;cfg.size=sizeof(cfg);cfg.runtime_invoke=reinterpret_cast<void*>(invoke);cfg.runtime_invoke_convert_args=reinterpret_cast<void*>(convert);cfg.execute_tasks=&execute_token;cfg.thread_current=thread;cfg.verify_metadata=verify;cfg.expected_owner_tid=gettid();cfg.exclusive_owner=1;cfg.subscriber_count=1;cfg.subscribers[0].end=host->on_unity_pulse;
 assert(scheduler->install(&cfg)==0);assert(scheduler->activate());pulse();pulse();pulse();PgrInventoryHostSample gauge{};assert(host->sample(&gauge)==0&&(gauge.capabilities&56)==56);
 pulse(1);assert(originals[0]==1);pulse(2);assert(originals[2]==1);pause_ms(120);pulse();assert(open_calls==1&&originals[3]==1);
 pulse(3);assert(sequence_return==1&&originals[5]==1&&sequence.fields[H_GF_F_SEQ_STATE]==3);
 pulse(4);assert(originals[4]==1);pulse(5);assert(originals[8]==0);pulse();assert(send_calls==1&&reserves==1&&returns==1&&originals[6]==1&&originals[7]==1);
 unsigned permissions=0;bool issued=false,confirmed=false,skipped=false;PgrInventoryHostEvent event{};while(host->next_event(&event)==0){if(strstr(event.json,"gift-permission")){++permissions;assert(strstr(event.json,"\"mask\":7")&&strstr(event.json,"\"allowed\":true"));}if(strstr(event.json,"gift-bundled-request"))issued=true;if(strstr(event.json,"gift-bundled-result")&&strstr(event.json,"\"success\":true"))confirmed=true;if(strstr(event.json,"gift-presentation-skipped"))skipped=true;}assert(issued&&confirmed&&skipped&&permissions==1);
 pulse(6);assert(originals[1]==1);assert(host->stop()==0);pulse();assert(host->sample(&gauge)==0&&gauge.roots==0&&live.empty());
 // Stopped shared replacements preserve manual originals without new transactions.
 pulse(5);assert(originals[8]==1&&send_calls==1);
 puts("PASS actual65 social: actual48 owner inspector + all9 Dobby sites;17 incoming open and sequence skip;22 bundled send/sticker reserve/result/finally; telemetry, stopped originals and root balance. Synthetic56/managed world.");return 0;
}
}

int main(int argc,char **argv){
 if(argc==3&&!strcmp(argv[2],"social"))return social_fixture::run(argv[1]);
 assert(argc==2);pid_t child=fork();assert(child>=0);
 if(child==0){execl(argv[0],argv[0],argv[1],"social",static_cast<char*>(nullptr));_exit(127);}
 int status=0;assert(waitpid(child,&status,0)==child);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
 return inventory_run(argc,argv);
}

// Test translation unit contains UNCHANGED production61 source. Only56 metadata
// resolution is substituted; every gameplay leaf comes from the actual core DLL,
// and observer hooks use real Dobby on executable fixture functions.56's separate
// actual-library fixture owns metadata/prologue proof; this is not a game ABI test.
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
#include "pgrazor_native_api.h"
#include "dobby.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <vector>
#include <string>

PgrSharedCatchApi fixture_sh{};PgrSummaryCompletionApi fixture_sc{};PgrDetailsCompletionApi fixture_dc{};
PgrDetailsRecoveryApi fixture_dr{};PgrCatchDiscoveryApi fixture_cd{};PgrCatchCanvasApi fixture_cv{};
PgrCatchCompletionApi fixture_cc{};PgrCatchApi fixture_bridge{};PgrTransferExecApi fixture_tx{};
PgrTransferFactsApi fixture_tf{};PgrTransferObserversApi fixture_to{};PgrUnitySchedulerApi fixture_scheduler{};
PgrSharedMetadataApi fixture_md_api{};PgrBootApi fixture_boot{};PgrMetaApi fixture_meta{};
PgrRuntimeTransportApi fixture_transport{};PgrTransferApi fixture_transfer_policy{};
const void *fixture_interaction_table=nullptr;
extern "C" const void *fixture_interaction_api(uint32_t version){return version==2?fixture_interaction_table:nullptr;}
#define SHIM(NAME,TYPE,VALUE) const void *NAME(uint32_t){return &VALUE;}
SHIM(fixture_shared_api,PgrSharedCatchApi,fixture_sh)
SHIM(fixture_summary_api,PgrSummaryCompletionApi,fixture_sc)
SHIM(fixture_details_api,PgrDetailsCompletionApi,fixture_dc)
SHIM(fixture_recovery_api,PgrDetailsRecoveryApi,fixture_dr)
SHIM(fixture_discovery_api,PgrCatchDiscoveryApi,fixture_cd)
SHIM(fixture_canvas_api,PgrCatchCanvasApi,fixture_cv)
SHIM(fixture_completion_api,PgrCatchCompletionApi,fixture_cc)
SHIM(fixture_presentation_api,PgrCatchApi,fixture_bridge)
SHIM(fixture_transfer_api,PgrTransferExecApi,fixture_tx)
SHIM(fixture_facts_api,PgrTransferFactsApi,fixture_tf)
SHIM(fixture_observers_api,PgrTransferObserversApi,fixture_to)
SHIM(fixture_scheduler_api,PgrUnitySchedulerApi,fixture_scheduler)
SHIM(fixture_metadata_api,PgrSharedMetadataApi,fixture_md_api)
SHIM(fixture_transport_api,PgrRuntimeTransportApi,fixture_transport)
SHIM(fixture_transfer_policy_api,PgrTransferApi,fixture_transfer_policy)
SHIM(fixture_boot_api,PgrBootApi,fixture_boot)
SHIM(fixture_meta_api,PgrMetaApi,fixture_meta)
#undef SHIM
int fixture_hook(void *address,void *replacement,void **original){return DobbyHook(address,replacement,original);}
#define pgrazor_shared_catch_api fixture_shared_api
#define pgrazor_summary_completion_api fixture_summary_api
#define pgrazor_details_completion_api fixture_details_api
#define pgrazor_details_recovery_api fixture_recovery_api
#define pgrazor_catch_discovery_api fixture_discovery_api
#define pgrazor_catch_canvas_api fixture_canvas_api
#define pgrazor_catch_completion_api fixture_completion_api
#define pgrazor_catch_presentation_api fixture_presentation_api
#define pgrazor_transfer_execution_api fixture_transfer_api
#define pgrazor_transfer_facts_api fixture_facts_api
#define pgrazor_transfer_observers_api fixture_observers_api
#define pgrazor_unity_scheduler_api fixture_scheduler_api
#define pgrazor_shared_metadata_api fixture_metadata_api
#define pgrazor_runtime_transport_api fixture_transport_api
#define pgrazor_transfer_policy_api fixture_transfer_policy_api
#define pgrazor_runtime_bootstrap_api fixture_boot_api
#define pgrazor_runtime_metadata_api fixture_meta_api
#define pgrazor_interaction_host_api fixture_interaction_api
#define pgrazor_shared_host_api fixture_host_api
#define pgrazor_hook fixture_hook
#include "shared_host.cpp"

namespace test {
int klasses[40],field_tokens[48],method_tokens[24];
struct Object{void *klass;uint64_t padding[3]{};void *target;Object(void *k,void *t=nullptr):klass(k),target(t){}};
struct Response{void *klass;uint64_t padding[3]{};uint64_t id;int32_t status;Response(void *k,uint64_t i,int32_t s):klass(k),id(i),status(s){}};
struct Gui{void *klass;int32_t state;Object *dismissed;Object *layer=nullptr;bool visible=true;};
struct State{void *klass;Gui *gui;Response *response;};
struct Bool{void *klass;int32_t value;};
struct Pokemon {void *klass;uint64_t padding[3]{};uint64_t id;int species,attack,defense,stamina;uint8_t favorite,egg;void *display,*events,*limited;Pokemon(void *k,uint64_t i,int sp,int a,int d,int s,uint8_t f,uint8_t e,void *di,void *ev,void *li):klass(k),id(i),species(sp),attack(a),defense(d),stamina(s),favorite(f),egg(e),display(di),events(ev),limited(li){}};
struct Display {void *klass;int costume,original,art,asset;void *background;};
struct List {void *klass;Pokemon *pokemon;};
struct Data {void *klass;List *list;int index;uint8_t post;Object *callback;};
struct View {void *klass;uint8_t padding[216]{};Response *response;void *bag,*gm;uint8_t active=1;View(void *k,Response *r,void *b,void *g):klass(k),response(r),bag(b),gm(g){}};
struct DetailsGui {void *klass;int32_t state;Object *dismissed;Data *data;Object *callback;bool visible=true;};
struct Callback {void *klass;Pokemon *pokemon;void *bag;};
struct ReleaseOut {void *klass;int32_t status;};
struct Handle{void *object;int pinned;};
Object action{&klasses[7]};Gui gui{&klasses[1],2,&action};Response response{&klasses[3],0xfedcba9876543210ULL,7};State state{&klasses[2],&gui,&response};Bool boolean{&klasses[6],1};
Display display{&klasses[16],0,0,0,0,nullptr};Object origins{&klasses[17]},bag_object{&klasses[18]},gm_object{&klasses[19]};
Pokemon pokemon{&klasses[13],response.id,25,1,2,3,0,0,&display,&origins,nullptr};List list{&klasses[15],&pokemon};View view{&klasses[11],&response,&bag_object,&gm_object};Object detail_action{&klasses[7],&view};Data data{&klasses[12],&list,0,1,&detail_action};DetailsGui details_gui{&klasses[10],2,&detail_action,&data,&detail_action};
Bool integer{&klasses[14],1},false_boolean{&klasses[6],0},zero_integer{&klasses[14],0},size_value{&klasses[20],2};
Callback callback{&klasses[21],&pokemon,&bag_object};ReleaseOut released{&klasses[22],9};
Object layer{&klasses[24]},type_object{&klasses[25]},service{&klasses[26]};
struct Array {void *klass=&klasses[27];uint64_t padding[3]{};void *values[1]={&details_gui};} scene_array;
static_assert(offsetof(Object,target)==0x20&&offsetof(View,response)==0xe0&&offsetof(Pokemon,id)==0x20&&offsetof(Response,id)==0x20,"actual feature3 fixed-layout proof");
unsigned detail_clicks=0,release_calls=0,result_calls=0;bool synchronous_transfer=true;
unsigned canvas_calls=0,direct_calls=0,show_calls=0;bool direct_mode=false,direct_exception=false,recovery_mode=false,scene_only=false;
unsigned handles=0,freed=0,allocated=0,dismissals=0,initializations=0,scores_calls=0,open_calls=0;
bool init_throws=false,reentrant=false,revoke_during_dismiss=false;
void (*during_init)()=nullptr;
bool observer_enabled=false,observer_move=false,open_throws=false,exit_throws=false,gc_fail=false;
unsigned observer_enters=0,observer_leaves=0,observer_aborts=0,exit_calls=0;
void *last_open_self=nullptr,*last_exit_self=nullptr;const void *last_open_info=nullptr,*last_exit_info=nullptr;
Gui relocated_gui=gui;State relocated_state=state;
std::vector<Handle*> live_handles;
int observer_wants(uint32_t id){return observer_enabled&&(id==15||id==18);}
uint64_t observer_enter(uint32_t id,void *self,void *arg){assert(!arg&&(id==15||id==18));bool pinned=false;for(auto h:live_handles)if(h->object==self&&h->pinned==1)pinned=true;assert(pinned);
 if(observer_move)for(auto h:live_handles)if(h->object==self)h->object=id==15?static_cast<void*>(&relocated_gui):static_cast<void*>(&relocated_state);
 return ++observer_enters;
}
void observer_leave(uint64_t token,void *result){assert(token&&!result&&!live_handles.empty());++observer_leaves;}
void observer_abort(uint64_t token){assert(token&&!live_handles.empty());++observer_aborts;}
const pgr_interaction_owner::Api observer_api{2,sizeof(pgr_interaction_owner::Api),observer_wants,observer_enter,observer_leave,observer_abort};
const PgrSharedHostApi *host=nullptr;
void *gc_new(void *o,int pinned){if(!o||gc_fail)return nullptr;++handles;++allocated;auto h=new Handle{o,pinned};live_handles.push_back(h);return h;}
void *gc_get(void *h){return h?static_cast<Handle*>(h)->object:nullptr;}
void gc_free(void *h){if(h){auto found=std::find(live_handles.begin(),live_handles.end(),static_cast<Handle*>(h));assert(found!=live_handles.end());live_handles.erase(found);delete static_cast<Handle*>(h);assert(handles);--handles;++freed;}}
void *object_class(void *o){return o?static_cast<Object*>(o)->klass:nullptr;}
void *unbox(void *o){return &static_cast<Bool*>(o)->value;}
void *attached(){return &state;}
void field_read(void *o,const void *f,void *out){const ptrdiff_t index=static_cast<const int*>(f)-field_tokens;switch(index){
 case 0:std::memcpy(out,&static_cast<Response*>(o)->id,8);break;
 case 1:std::memcpy(out,&static_cast<Gui*>(o)->state,4);break;
 case 2:std::memcpy(out,&static_cast<Gui*>(o)->dismissed,8);break;
 case 3:std::memcpy(out,&static_cast<State*>(o)->gui,8);break;
 case 4:std::memcpy(out,&static_cast<State*>(o)->response,8);break;
 case 5:std::memcpy(out,&static_cast<Response*>(o)->status,4);break;
 case 6:std::memcpy(out,&static_cast<Object*>(o)->target,8);break;
 case 7:std::memcpy(out,&static_cast<View*>(o)->response,8);break;
 case 8:std::memcpy(out,&static_cast<DetailsGui*>(o)->data,8);break;
 case 9:std::memcpy(out,&static_cast<DetailsGui*>(o)->callback,8);break;
 case 10:std::memcpy(out,&static_cast<Data*>(o)->callback,8);break;
 case 11:std::memcpy(out,&static_cast<Data*>(o)->list,8);break;
 case 12:std::memcpy(out,&static_cast<Data*>(o)->index,4);break;
 case 13:std::memcpy(out,&static_cast<Data*>(o)->post,1);break;
 case 14:std::memcpy(out,&static_cast<Pokemon*>(o)->id,8);break;
 case 15:std::memcpy(out,&static_cast<View*>(o)->bag,8);break;
 case 16:std::memcpy(out,&static_cast<View*>(o)->gm,8);break;
 case 17:std::memcpy(out,&static_cast<Pokemon*>(o)->species,4);break;
 case 18:std::memcpy(out,&static_cast<Pokemon*>(o)->attack,4);break;
 case 19:std::memcpy(out,&static_cast<Pokemon*>(o)->defense,4);break;
 case 20:std::memcpy(out,&static_cast<Pokemon*>(o)->stamina,4);break;
 case 21:std::memcpy(out,&static_cast<Pokemon*>(o)->favorite,1);break;
 case 22:std::memcpy(out,&static_cast<Pokemon*>(o)->egg,1);break;
 case 23:std::memcpy(out,&static_cast<Pokemon*>(o)->display,8);break;
 case 24:std::memcpy(out,&static_cast<Pokemon*>(o)->events,8);break;
 case 25:std::memcpy(out,&static_cast<Pokemon*>(o)->limited,8);break;
 case 26:std::memcpy(out,&static_cast<Display*>(o)->costume,4);break;
 case 27:std::memcpy(out,&static_cast<Display*>(o)->original,4);break;
 case 28:std::memcpy(out,&static_cast<Display*>(o)->background,8);break;
 case 29:std::memcpy(out,&static_cast<Display*>(o)->art,4);break;
 case 30:std::memcpy(out,&static_cast<Display*>(o)->asset,4);break;
 case 31:std::memcpy(out,&static_cast<Callback*>(o)->pokemon,8);break;
 case 32:std::memcpy(out,&static_cast<Callback*>(o)->bag,8);break;
 case 33:std::memcpy(out,&static_cast<ReleaseOut*>(o)->status,4);break;
 case 34:std::memcpy(out,&static_cast<Gui*>(o)->layer,8);break;
 case 35:std::memcpy(out,&static_cast<View*>(o)->active,1);break;
 default:std::abort();}}
#define FIXTURE_FUNCTION __attribute__((noinline,optnone))
#define PAD asm volatile(".rept 32\n nop\n .endr\n")
FIXTURE_FUNCTION void scores_fn(void*,void*,int32_t,const void*){PAD;++scores_calls;}
FIXTURE_FUNCTION void open_fn(void *o,const void *info){PAD;++open_calls;last_open_self=o;last_open_info=info;if(open_throws)throw std::runtime_error("original-open");static_cast<Gui*>(o)->state=2;}
FIXTURE_FUNCTION void init_fn(void *o,void *value,const void*){PAD;++initializations;if(init_throws)throw std::runtime_error("original-init");auto s=static_cast<State*>(o);s->response=static_cast<Response*>(value);if(during_init)during_init();Scores volatile scores=&scores_fn;scores(s->gui,value,701,nullptr);One volatile open=&open_fn;open(s->gui,nullptr);if(during_init)during_init();}
void show_fn(void*,void*,uint64_t,uint64_t,uint64_t,void*,void*,void*,uint64_t,uint64_t,uint64_t,void*);
FIXTURE_FUNCTION void dismiss_fn(void *o,const void*){PAD;++dismissals;if(reentrant)assert(host->tick(uint64_t(gettid()),900)==0);if(revoke_during_dismiss){PgrSharedHostOptions off{1,sizeof(off),0,63,2,0,nullptr,0,0};assert(host->update(&off)==0);}static_cast<Gui*>(o)->state=3;if(direct_mode){using Show=decltype(&show_fn);Show volatile show=&show_fn;show(&service,&pokemon,1,1,0,&detail_action,nullptr,nullptr,1,1,0,nullptr);}else if(recovery_mode){details_gui.state=2;details_gui.visible=true;}}
FIXTURE_FUNCTION void exit_fn(void *o,const void *info){PAD;++exit_calls;last_exit_self=o;last_exit_info=info;if(exit_throws)throw std::runtime_error("original-exit");}
FIXTURE_FUNCTION void detail_init_fn(void *o,void *value,const void*){PAD;auto g=static_cast<DetailsGui*>(o);g->data=static_cast<Data*>(value);g->callback=g->data->callback;g->state=1;}
FIXTURE_FUNCTION void detail_open_fn(void *o,const void*){PAD;static_cast<DetailsGui*>(o)->state=2;}
FIXTURE_FUNCTION void detail_present_fn(void*,void*,void*,bool,const void*){PAD;Two volatile init=&detail_init_fn;init(&details_gui,&data,nullptr);One volatile open=&detail_open_fn;open(&details_gui,nullptr);}
FIXTURE_FUNCTION void detail_exit_fn(void*,const void*){PAD;}
FIXTURE_FUNCTION void detail_click_fn(void *o,const void*){PAD;++detail_clicks;static_cast<DetailsGui*>(o)->state=3;}
FIXTURE_FUNCTION void result_fn(void*,void*,const void*){PAD;++result_calls;}
FIXTURE_FUNCTION void error_fn(void*,void*,const void*){PAD;}
FIXTURE_FUNCTION void canvas_fn(void *o,bool value,const void*){PAD;++canvas_calls;if(object_class(o)==&klasses[10])static_cast<DetailsGui*>(o)->visible=value;else static_cast<Gui*>(o)->visible=value;}
FIXTURE_FUNCTION void show_fn(void*,void *pk,uint64_t,uint64_t,uint64_t,void*,void*,void*,uint64_t,uint64_t,uint64_t,void*){PAD;++show_calls;Present volatile present=&detail_present_fn;present(&view,pk,nullptr,false,nullptr);}
void *class_type(const void *klass){return const_cast<void*>(klass);}void *type_get_object(const void*){return &type_object;}uintptr_t array_length(void*){return 1;}
int32_t string_length(void*){return 0;}
void *runtime(const void *method,void *o,void **args,void **exception){*exception=nullptr;if(method==&method_tokens[5])return &boolean;if(method==&method_tokens[1]){One volatile fn=&dismiss_fn;fn(o,nullptr);return nullptr;}
 if(method==&method_tokens[7]){One volatile fn=&detail_click_fn;fn(o,nullptr);return nullptr;}
 if(method==&method_tokens[8])return &integer;if(method==&method_tokens[9])return &pokemon;
 if(method==&method_tokens[10])return &size_value;if(method==&method_tokens[11]||method==&method_tokens[12])return &false_boolean;
 if(method==&method_tokens[13])return &zero_integer;if(method==&method_tokens[14])return &pokemon;
 if(method==&method_tokens[15]){++release_calls;if(synchronous_transfer){Two volatile fn=&result_fn;fn(&callback,&released,nullptr);}return &bag_object;}
 if(method==&method_tokens[16]){auto volatile set=&canvas_fn;set(o,*static_cast<uint8_t*>(args[0])!=0,nullptr);return nullptr;}
 if(method==&method_tokens[17])return scene_only?nullptr:&details_gui;if(method==&method_tokens[18])return &scene_array;
 if(method==&method_tokens[19]){++direct_calls;if(direct_exception)*exception=&gm_object;else view.active=0;return nullptr;}
 if(method==&method_tokens[20]){using Show=decltype(&show_fn);Show volatile show=&show_fn;show(o,args[0],*static_cast<uint8_t*>(args[1]),*static_cast<uint8_t*>(args[2]),*static_cast<int32_t*>(args[3]),args[4],args[5],args[6],*static_cast<uint8_t*>(args[7]),*static_cast<uint8_t*>(args[8]),*static_cast<uint8_t*>(args[9]),nullptr);return nullptr;}
 std::abort();}
PgrSharedMetadataOutput prepared{};
int resolve(const PgrSharedMetadataConfig*,PgrSharedMetadataOutput *out){*out=prepared;return 0;}
void prepare(){using namespace pgr_shared_meta;prepared.version=2;prepared.size=sizeof(prepared);for(unsigned i=0;i<7;++i){prepared.classes[i]=&klasses[i];prepared.methods[i]=&method_tokens[i];}for(unsigned i=0;i<6;++i)prepared.fields[i]=&field_tokens[i];for(auto &o:prepared.optional)o=PGR_SHARED_OPT_UNAVAILABLE;prepared.success=7;prepared.caught_id_size=8;prepared.method_code[0]=reinterpret_cast<void*>(exit_fn);prepared.method_code[1]=reinterpret_cast<void*>(dismiss_fn);prepared.method_code[2]=reinterpret_cast<void*>(init_fn);prepared.method_code[3]=reinterpret_cast<void*>(scores_fn);prepared.method_code[4]=reinterpret_cast<void*>(open_fn);}
void prepare_details(){using namespace pgr_shared_meta;prepared.optional[OPT_DETAILS]=PGR_SHARED_OPT_AVAILABLE;prepared.optional[OPT_TRANSFER]=PGR_SHARED_OPT_AVAILABLE;
 const int oc[]={OC_DGUI,OC_VIEW,OC_DATA,OC_PK,OC_ACTION,OC_INT,OC_DISPLAY,OC_BAG,OC_CHAR_SIZE,OC_STRING,OC_RELEASE};const int kc[]={10,11,12,13,7,14,16,18,20,23,22};for(unsigned i=0;i<sizeof(oc)/sizeof(*oc);++i)prepared.opt_classes[oc[i]]=&klasses[kc[i]];
 const int df[]={H_DET_DELEGATE_TARGET,H_DET_VIEW_RESP,H_DET_DATA,H_DET_CALLBACK,H_DET_ON_DISMISSED,H_DET_LIST,H_DET_INDEX,H_DET_POST,H_DET_ID};for(unsigned i=0;i<9;++i)prepared.opt_handles[df[i]]=&field_tokens[6+i];prepared.opt_handles[H_DET_LIST_CLASS]=&klasses[15];
 prepared.opt_handles[H_DET_PRESENT_CODE]=reinterpret_cast<void*>(detail_present_fn);prepared.opt_handles[H_DET_INIT_CODE]=reinterpret_cast<void*>(detail_init_fn);prepared.opt_handles[H_DET_OPEN_CODE]=reinterpret_cast<void*>(detail_open_fn);prepared.opt_handles[H_DET_EXIT_CODE]=reinterpret_cast<void*>(detail_exit_fn);prepared.opt_handles[H_DET_CLICK_CODE]=reinterpret_cast<void*>(detail_click_fn);
 prepared.opt_handles[H_DET_CLICK]=&method_tokens[7];prepared.opt_handles[H_DET_COUNT]=&method_tokens[8];prepared.opt_handles[H_DET_ITEM]=&method_tokens[9];
 const int trf[]={H_TR_BAG,H_TR_GM,H_TR_SPECIES,H_TR_ATTACK,H_TR_DEFENSE,H_TR_STAMINA,H_TR_FAVORITE,H_TR_EGG,H_TR_DISPLAY,H_TR_EVENTS,H_TR_LIMITED,H_TR_COSTUME,H_TR_ORIG_COSTUME,H_TR_BACKGROUND,H_TR_ART,H_TR_ART_ASSET,H_TR_CB_POKEMON,H_TR_CB_BAG,H_TR_RESULT_STATUS};for(unsigned i=0;i<19;++i)prepared.opt_handles[trf[i]]=&field_tokens[15+i];
 prepared.opt_handles[H_TR_EVENT_CLASS]=&klasses[17];prepared.opt_handles[H_TR_CB_CLASS]=&klasses[21];const int trm[]={H_TR_SIZE_M,H_TR_RARE_M,H_TR_SHINY_M,H_TR_EVENT_COUNT_M,H_TR_GET_M,H_TR_RELEASE_M};for(unsigned i=0;i<6;++i)prepared.opt_handles[trm[i]]=&method_tokens[10+i];
 prepared.opt_handles[H_TR_RESULT_CODE]=reinterpret_cast<void*>(result_fn);prepared.opt_handles[H_TR_ERROR_CODE]=reinterpret_cast<void*>(error_fn);for(int i=0;i<5;++i)prepared.opt_ints[I_TR_SIZE_XXS+i]=i;prepared.opt_ints[I_TR_RESULT_SUCCESS]=9;
 prepared.optional[OPT_CATCH_CANVAS]=PGR_SHARED_OPT_AVAILABLE;prepared.opt_handles[H_CC_METHOD]=&method_tokens[16];prepared.opt_handles[H_CC_CODE]=reinterpret_cast<void*>(canvas_fn);
}
void prepare_recovery(){using namespace pgr_shared_meta;recovery_mode=true;gui.layer=&layer;prepared.optional[OPT_CATCH_DISCOVERY]=prepared.optional[OPT_CATCH_SCENE]=PGR_SHARED_OPT_AVAILABLE;prepared.opt_handles[H_CD_CLASS]=&klasses[24];prepared.opt_handles[H_CD_LAYER]=&field_tokens[34];prepared.opt_handles[H_CD_TOP]=&method_tokens[17];prepared.opt_handles[H_CS_FIND]=&method_tokens[18];}
void prepare_direct(){using namespace pgr_shared_meta;direct_mode=true;prepared.optional[OPT_DIRECT_PRESENTATION]=PGR_SHARED_OPT_AVAILABLE;prepared.opt_handles[H_DP_ACTIVE]=&field_tokens[35];prepared.opt_handles[H_DP_CALLBACK]=&method_tokens[19];prepared.opt_handles[H_DP_SHOW]=&method_tokens[20];prepared.opt_handles[H_DP_SHOW_CODE]=reinterpret_cast<void*>(show_fn);}
void options(uint64_t epoch=1){PgrSharedHostOptions o{1,sizeof(o),PGR_SHH_ALLOWED|PGR_SHH_DISMISS,63,epoch,2500,nullptr,0,0};assert(host->update(&o)==0);}
void initialize(){Two volatile fn=&init_fn;fn(&state,&response,nullptr);}
PgrSharedHostSample snapshot(){PgrSharedHostSample s{};assert(host->sample(&s)==0);return s;}
void check_clean(){auto s=snapshot();assert(s.roots==0&&s.records==0&&s.hook_frames==0);assert(handles==0&&allocated==freed);}
struct WireEvent{uint32_t opcode;std::vector<int64_t> scalars;std::vector<std::string> strings;};std::vector<WireEvent> wire_events;
uint32_t wire32(const uint8_t *p){uint32_t value;std::memcpy(&value,p,4);return value;}
void wire_put(std::vector<uint8_t>&bytes,size_t at,uint32_t value){std::memcpy(bytes.data()+at,&value,4);}
void wire_decode(const std::vector<uint8_t>&bytes,uint32_t length){assert(length>=32);const uint32_t count=wire32(bytes.data()+12);size_t at=32;for(uint32_t i=0;i<count;++i){WireEvent e{};e.opcode=wire32(bytes.data()+at);const auto ns=wire32(bytes.data()+at+4),nt=wire32(bytes.data()+at+8);at+=12;for(uint32_t j=0;j<ns;++j){int64_t value;std::memcpy(&value,bytes.data()+at,8);at+=8;e.scalars.push_back(value);}for(uint32_t j=0;j<nt;++j){uint32_t n=wire32(bytes.data()+at);at+=4;e.strings.emplace_back(reinterpret_cast<const char*>(bytes.data()+at),n);at+=n;}wire_events.push_back(e);}assert(at==length);}
void wire_exchange(const std::vector<uint8_t>&frame={}){std::vector<uint8_t> out(65536);uint32_t length=0;assert(fixture_transport.exchange(frame.empty()?nullptr:frame.data(),frame.size(),out.data(),out.size(),&length)==0);wire_decode(out,length);}
void wire_snapshot(bool dismiss,bool transfer,const std::string &keep_list="",bool keep_all=false,int protect=63){static uint64_t sequence=0;++sequence;std::vector<uint8_t> bytes(PGR_TRANSPORT_SNAPSHOT_FIXED+7*4+keep_list.size(),0);wire_put(bytes,0,PGR_TRANSPORT_SNAPSHOT_MAGIC);wire_put(bytes,4,1);wire_put(bytes,8,bytes.size());std::memcpy(bytes.data()+16,&sequence,8);bytes[32+25]=dismiss;bytes[32+5]=transfer;bytes[32+35]=keep_all;wire_put(bytes,72+8*4,uint32_t(protect));size_t at=152;for(unsigned i=0;i<7;++i){uint32_t n=i==4?keep_list.size():0;wire_put(bytes,at,n);at+=4;if(n){std::memcpy(bytes.data()+at,keep_list.data(),n);at+=n;}}wire_exchange(bytes);}
bool wire_has(uint32_t opcode,const char *text=nullptr){for(auto &e:wire_events)if(e.opcode==opcode&&(!text||(!e.strings.empty()&&e.strings[0]==text)))return true;return false;}
}
int main(int argc,char **argv){assert(argc==2||argc==3);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);auto get=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(get);auto core=static_cast<const pgrazor_native_api_v2*>(get(2));assert(core);
#define LOAD(VAR,TYPE,ID,VER) {auto value=static_cast<const TYPE*>(core->query_feature(ID,VER));assert(value);VAR=*value;}
 LOAD(fixture_sh,PgrSharedCatchApi,51,1) LOAD(fixture_sc,PgrSummaryCompletionApi,36,1) LOAD(fixture_dc,PgrDetailsCompletionApi,37,1)
 LOAD(fixture_dr,PgrDetailsRecoveryApi,38,1) LOAD(fixture_cd,PgrCatchDiscoveryApi,40,1) LOAD(fixture_cv,PgrCatchCanvasApi,39,1)
 LOAD(fixture_cc,PgrCatchCompletionApi,24,1) LOAD(fixture_bridge,PgrCatchApi,3,1) LOAD(fixture_tx,PgrTransferExecApi,23,1)
 LOAD(fixture_tf,PgrTransferFactsApi,35,1) LOAD(fixture_to,PgrTransferObserversApi,57,1) LOAD(fixture_scheduler,PgrUnitySchedulerApi,48,2)
 LOAD(fixture_boot,PgrBootApi,54,1) LOAD(fixture_meta,PgrMetaApi,47,1)
 LOAD(fixture_transport,PgrRuntimeTransportApi,52,1) LOAD(fixture_transfer_policy,PgrTransferApi,4,1)
#undef LOAD
 fixture_interaction_table=core->query_feature(69,2);assert(fixture_interaction_table);
 using namespace test;prepare();if(argc==3){prepare_details();if(std::strcmp(argv[2],"recovery")==0||std::strcmp(argv[2],"scene")==0){prepare_recovery();scene_only=std::strcmp(argv[2],"scene")==0;}if(std::strcmp(argv[2],"direct")==0)prepare_direct();}fixture_md_api={2,sizeof(fixture_md_api),sizeof(PgrSharedMetadataConfig),sizeof(PgrSharedMetadataOutput),resolve};host=static_cast<const PgrSharedHostApi*>(fixture_host_api(1));
 assert(host->configure(nullptr)==1);assert(!snapshot().ready&&!snapshot().fault);
 PgrMetaExports exp{};exp.class_get_type=reinterpret_cast<void*>(class_type);PgrSharedMetaExtraExports ex{};ex.type_get_object=reinterpret_cast<void*>(type_get_object);ex.array_length=reinterpret_cast<void*>(array_length);ex.gchandle_new=reinterpret_cast<void*>(gc_new);ex.gchandle_get_target=reinterpret_cast<void*>(gc_get);ex.gchandle_free=reinterpret_cast<void*>(gc_free);
 PgrSharedHostConfig config{1,sizeof(config)};config.metadata.version=2;config.metadata.size=sizeof(config.metadata);config.metadata.binding.exports=&exp;config.metadata.extra=&ex;config.metadata.owner_tid=gettid();config.metadata.thread_current=reinterpret_cast<void*>(attached);config.object_get_class=reinterpret_cast<void*>(object_class);config.field_get_value=reinterpret_cast<void*>(field_read);config.runtime_invoke=reinterpret_cast<void*>(runtime);config.object_unbox=reinterpret_cast<void*>(unbox);config.exclusive_hooks=1;
 config.string_length=reinterpret_cast<void*>(string_length);const bool transport_mode=argc==3&&std::strcmp(argv[2],"transport")==0;
 if(transport_mode){PgrRuntimeTransportConfig transport_config{1,sizeof(transport_config),1000,256,10000,65536,0,0};assert(fixture_transport.install(&transport_config)==0);config.transport_flags=1;}
 if(transport_mode){
  assert(host->configure(&config)==0);wire_snapshot(true,false);assert(wire_has(16)&&wire_has(45,"Ready: Quick Catch")&&wire_has(23,"Ready: new catches only; IV below 90%"));host->tick(gettid(),1);
  auto present=[](){Present volatile fn=&detail_present_fn;fn(&view,&pokemon,nullptr,false,nullptr);};
  present();host->tick(gettid(),2);host->tick(gettid(),3);assert(detail_clicks==1);check_clean();wire_exchange();assert(wire_has(19,"quick-close"));PgrSharedHostEvent empty{};assert(host->next_event(&empty)==0);
  // Native parser rejects duplicate, leading-zero, whitespace and oversize IDs.
  for(const char *bad:{"25,25","025","25, ","2001"}){wire_snapshot(false,true,bad);host->tick(gettid(),4);present();host->tick(gettid(),5);host->tick(gettid(),6);host->tick(gettid(),7);assert(!release_calls&&detail_clicks==1);check_clean();}
  // Canonical sorting makes reordering the same keep set a non-policy change.
  wire_snapshot(false,true,"1,2");host->tick(gettid(),8);present();host->tick(gettid(),9);wire_snapshot(false,true,"2,1");host->tick(gettid(),10);assert(release_calls==1&&detail_clicks==2);host->tick(gettid(),11);check_clean();wire_exchange();assert(wire_has(56,"Transferred newly caught Pokemon")&&wire_has(22,"transferred"));
  response.id++;pokemon.id=response.id;wire_snapshot(false,true,"",true);host->tick(gettid(),12);present();host->tick(gettid(),13);host->tick(gettid(),14);host->tick(gettid(),15);assert(release_calls==1);check_clean();
  // A new policy cancels an already-armed candidate, never applies it retroactively.
  wire_snapshot(false,true);host->tick(gettid(),16);present();host->tick(gettid(),17);wire_snapshot(false,true,"25");host->tick(gettid(),18);assert(release_calls==1);check_clean();
  // Snapshot expiry revokes at the next Unity tick, even though its previous host lease was2500ms.
  wire_snapshot(true,false);host->tick(gettid(),19);present();usleep(1100000);host->tick(gettid(),20);assert(detail_clicks==2);check_clean();
  wire_snapshot(true,false);host->tick(gettid(),21);present();host->tick(gettid(),22);host->tick(gettid(),23);assert(detail_clicks==3);check_clean();
  // Actual native52 snapshots advance INSIDE the real-Dobby original, both
  // before nested scores/open and immediately before initialize returns.
  uint64_t pulse=30;
  wire_snapshot(true,false);host->tick(gettid(),pulse++);
  during_init=+[](){wire_snapshot(true,false);};initialize();during_init=nullptr;
  assert(snapshot().records==1);host->tick(gettid(),pulse++);host->tick(gettid(),pulse++);
  assert(dismissals==1);present();host->tick(gettid(),pulse++);host->tick(gettid(),pulse++);check_clean();
  // Real policy changes during that same original invalidate its capture.
  // In particular DISMISS off must invalidate even while TRANSFER remains on.
  for(int mode=0;mode<4;++mode){
   response.id++;pokemon.id=response.id;gui.state=2;
   wire_snapshot(true,mode==2);host->tick(gettid(),pulse++);
   if(mode==0)during_init=+[](){wire_snapshot(false,false);};
   if(mode==1)during_init=+[](){wire_snapshot(true,false,"25",false,31);};
   if(mode==2)during_init=+[](){wire_snapshot(false,true);};
   if(mode==3)during_init=+[](){usleep(1100000);};
   initialize();during_init=nullptr;host->tick(gettid(),pulse++);host->tick(gettid(),pulse++);
   assert(dismissals==1&&release_calls==1);
   wire_snapshot(false,false);host->tick(gettid(),pulse++);check_clean();
  }
  host->stop();host->tick(gettid(),pulse++);wire_exchange();assert(wire_has(45,"Stopped"));puts("PASS61 direct52 typed frames/settings parser/canonical-policy epochs/stale revoke+resume/readiness+transfer/status events without JS update/drain");return 0;
 }
 assert(host->configure(&config)==0);assert(host->configure(&config)==0);assert(host->metadata()->success==7);test::options();assert(host->tick(gettid(),1)==0);
 if(argc==3&&std::strcmp(argv[2],"observer")==0){
  fixture_interaction_table=&observer_api;PgrSharedHostOptions off{1,sizeof(off),0,63,2,0,nullptr,0,0};assert(host->update(&off)==0);host->tick(gettid(),2);check_clean();
  One volatile open=&open_fn;One volatile leave=&exit_fn;const void *info=reinterpret_cast<void*>(0x11223344);
  unsigned before=allocated;open(&gui,info);leave(&state,info);assert(allocated==before&&observer_enters==0&&open_calls==1&&exit_calls==1);
  observer_enabled=true;observer_move=true;open(&gui,info);assert(last_open_self==&relocated_gui&&last_open_info==info&&observer_enters==1&&observer_leaves==1);check_clean();
  leave(&state,info);assert(last_exit_self==&relocated_state&&last_exit_info==info&&observer_enters==2&&observer_leaves==2);check_clean();
  gc_fail=true;open(&gui,info);gc_fail=false;assert(observer_enters==2&&last_open_self==&gui&&open_calls==3);check_clean();
  exit_throws=true;bool caught=false;try{leave(&state,info);}catch(const std::runtime_error&e){caught=std::strcmp(e.what(),"original-exit")==0;}assert(caught&&exit_calls==3&&observer_aborts==1&&observer_leaves==2);exit_throws=false;check_clean();
  open_throws=true;caught=false;try{open(&gui,info);}catch(const std::runtime_error&e){caught=std::strcmp(e.what(),"original-open")==0;}assert(caught&&open_calls==4&&observer_aborts==2&&observer_leaves==2);check_clean();
  puts("PASS61 observer69 API2 targets15/18: inactive no-GC, pin before observer, moved receiver refresh, exact MethodInfo/original once, pin failure passthrough, abort before root cleanup on throw");return 0;
 }

 if(recovery_mode){
  initialize();host->tick(gettid(),2);host->tick(gettid(),3);assert(dismissals==1&&detail_clicks==0&&snapshot().records==1);assert(!details_gui.visible);
  host->tick(gettid(),4);host->tick(gettid(),5);assert(detail_clicks==1&&canvas_calls>0);check_clean();
  Present volatile present=&detail_present_fn;present(&view,&pokemon,nullptr,false,nullptr);assert(!details_gui.visible);
  PgrSharedHostOptions off{1,sizeof(off),0,63,2,0,nullptr,0,0};assert(host->update(&off)==0);host->tick(gettid(),6);assert(details_gui.visible&&detail_clicks==1);check_clean();
  puts(scene_only?"PASS61 actual40 scene discovery +38 recovery +39 canvas restoration and OFF cleanup":"PASS61 actual40 top-layer discovery +38 recovery +39 canvas restoration and OFF cleanup");return 0;
 }
 if(direct_mode){
  initialize();host->tick(gettid(),2);host->tick(gettid(),3);assert(dismissals==1&&direct_calls==1&&show_calls==0&&detail_clicks==0);check_clean();
  view.active=1;response.id++;pokemon.id=response.id;direct_exception=true;test::options(2);host->tick(gettid(),4);initialize();host->tick(gettid(),5);host->tick(gettid(),6);
  assert(direct_calls==2&&show_calls==1&&snapshot().records==1);host->tick(gettid(),7);host->tick(gettid(),8);assert(detail_clicks==1);check_clean();
  view.active=1;response.id++;pokemon.id=response.id;direct_exception=false;PgrSharedHostOptions transfer{1,sizeof(transfer),7,63,3,2500,nullptr,0,0};assert(host->update(&transfer)==0);host->tick(gettid(),9);initialize();host->tick(gettid(),10);host->tick(gettid(),11);assert(direct_calls==3&&release_calls==1&&show_calls==1);check_clean();
  puts("PASS61 actual3 presentation capture +24 direct completion, managed callback exception replay, direct synchronous transfer, balanced roots");return 0;
 }
 if(argc==3&&std::strcmp(argv[2],"broker")==0){
  auto broker=static_cast<const PgrSharedHostTransferApi*>(fixture_host_api(2));assert(broker&&broker->size==32&&broker->ready()==1);
  int32_t kept=25;PgrSharedHostOptions opt{1,sizeof(opt),PGR_SHH_ALLOWED|PGR_SHH_TRANSFER_ENABLED,63,2,2500,&kept,1,0};assert(host->update(&opt)==0);host->tick(gettid(),2);
  PgrTransferFactsRequest facts_request{1,sizeof(facts_request),uint64_t(gettid()),2500,response.id,&pokemon,&gm_object,63,0};PgrTransferFactsOut facts{};assert(fixture_tf.inspect(&facts_request,&facts)==1);
  PgrTransferExecRequest req{1,sizeof(req),response.id,999,uint64_t(gettid()),&bag_object,&pokemon,{}};
  req.input={1,sizeof(PgrTransferInput),facts.attack,facts.defense,facts.stamina,facts.species,0,facts.known,facts.flags,0,0,0,nullptr};
  uint64_t token=0;assert(broker->submit(&req,&token)==0&&!token&&!release_calls); // authoritative keep list
  opt.epoch=3;opt.keep_ids=nullptr;opt.keep_count=0;assert(host->update(&opt)==0);host->tick(gettid(),3);
  assert(broker->submit(&req,&token)==1&&token&&release_calls==1);PgrTransferExecEvent e{};unsigned requests=0,completed=0;
  while(broker->next_event(&e)==1){assert(e.token==token);requests+=e.kind==PGR_TX_EVENT_REQUEST;completed+=e.kind==PGR_TX_EVENT_COMPLETE&&(e.aux&1);}
  assert(requests==1&&completed==1);host->tick(gettid(),4);check_clean();assert(broker->next_event(&e)==0);
  // Ordinary catch transfer still receives its own event after the external FIFO is drained.
  response.id++;pokemon.id=response.id;Present volatile present=&detail_present_fn;present(&view,&pokemon,nullptr,false,nullptr);host->tick(gettid(),5);host->tick(gettid(),6);host->tick(gettid(),7);
  assert(release_calls==2&&detail_clicks==1);check_clean();assert(broker->next_event(&e)==0);
  host->stop();host->tick(gettid(),8);assert(!broker->ready());token=123;assert(broker->submit(&req,&token)<0&&!token);
  puts("PASS61 transfer broker + actual23/35/57: authoritative policy/epoch, synchronous external routing, ordinary catch isolation, stop");return 0;
 }
 if(argc==3){
  auto present=[](){Present volatile fn=&detail_present_fn;fn(&view,&pokemon,nullptr,false,nullptr);};
  // Normal details initialization/presentation preserves the strictly-later tick.
  present();assert(snapshot().records==1);host->tick(gettid(),2);assert(detail_clicks==0);host->tick(gettid(),3);assert(detail_clicks==1);check_clean();
  // Protected species remain open with Quick Catch off; no release or click.
  int32_t kept=25;PgrSharedHostOptions opt{1,sizeof(opt),PGR_SHH_ALLOWED|PGR_SHH_TRANSFER_ENABLED,63,2,2500,&kept,1,0};assert(host->update(&opt)==0);host->tick(gettid(),4);present();host->tick(gettid(),5);host->tick(gettid(),6);host->tick(gettid(),7);assert(!release_calls&&detail_clicks==1);check_clean();
  // A synchronous callback settles before submit returns; correlate its queued event once.
  opt.epoch=3;opt.keep_ids=nullptr;opt.keep_count=0;assert(host->update(&opt)==0);host->tick(gettid(),8);present();host->tick(gettid(),9);host->tick(gettid(),10);assert(release_calls==1&&result_calls==1&&detail_clicks==2);host->tick(gettid(),11);check_clean();
  // A second caught id waits for delayed confirmation with Quick Catch off.
  response.id++;pokemon.id=response.id;synchronous_transfer=false;opt.epoch=4;assert(host->update(&opt)==0);host->tick(gettid(),12);present();host->tick(gettid(),13);host->tick(gettid(),14);assert(release_calls==2&&detail_clicks==2&&snapshot().records==1);
  Two volatile result=&result_fn;result(&callback,&released,nullptr);host->tick(gettid(),15);assert(detail_clicks==3&&result_calls==2);host->tick(gettid(),16);check_clean();
  // A duplicate callback cannot revive/release a completed candidate.
  result(&callback,&released,nullptr);host->tick(gettid(),17);assert(release_calls==2&&detail_clicks==3);check_clean();
  puts("PASS compiled61 details + actual37/38/23/35/57 leaves: initialization, later-tick close, keep-species, synchronous and delayed transfer callbacks, duplicate callback, balanced roots");return 0;
 }
 initialize();assert(snapshot().records==1);assert(host->tick(gettid(),2)==0&&dismissals==0);assert(host->tick(gettid(),3)==0&&dismissals==1);check_clean();
 // User dismissal cancels pending work; its original executes once, no auto replay.
 initialize();One volatile dismiss=&dismiss_fn;dismiss(&gui,nullptr);assert(host->tick(gettid(),4)==0&&dismissals==2);check_clean();
 // Original callback reentry is ignored while the outer tick owns the transaction.
 initialize();reentrant=true;host->tick(gettid(),5);host->tick(gettid(),6);assert(dismissals==3);reentrant=false;check_clean();
 // Revocation from inside an invoked original cannot cause another invocation.
 initialize();revoke_during_dismiss=true;host->tick(gettid(),7);host->tick(gettid(),8);assert(dismissals==4);revoke_during_dismiss=false;check_clean();
 // OFF drains existing candidate roots even though no action is permitted.
 test::options(3);host->tick(gettid(),9);initialize();PgrSharedHostOptions off{1,sizeof(off),0,63,4,0,nullptr,0,0};assert(host->update(&off)==0);host->tick(gettid(),10);assert(dismissals==4);check_clean();
 // Original exception propagates and temporary pins unwind; no ready stage is published.
 test::options(5);host->tick(gettid(),11);init_throws=true;bool thrown=false;try{initialize();}catch(const std::runtime_error &e){thrown=std::strcmp(e.what(),"original-init")==0;}assert(thrown);host->tick(gettid(),12);assert(snapshot().fault&&snapshot().stopping);check_clean();
 assert(initializations==6&&scores_calls==5&&open_calls==5);puts("PASS compiled61 source + actual51/36 leaves + realDobby: summary roots, later-tick arming, natural-dismiss cancellation, reentrant tick, in-original revocation, OFF cleanup, original exception");
 for(const char *mode:{"details","transport","recovery","scene","direct","observer","broker"}){pid_t child=fork();assert(child>=0);if(child==0){execl(argv[0],argv[0],argv[1],mode,static_cast<char*>(nullptr));_exit(127);}int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}
}

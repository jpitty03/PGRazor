// Standalone fixture for the real smart_catch.cpp; no managed game or device required.
#include "smart_catch.h"
#include "runtime_transport.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unistd.h>
namespace {
void *tag(uintptr_t n){return reinterpret_cast<void*>(n);}
struct Obj {void *cls;uint64_t value=0;};
Obj wild{tag(1)},pokemon{tag(2)},display{tag(3)},bag{tag(4)},rpc{tag(5)},request{tag(6)},response{tag(7)},boxed_int{tag(9)},boxed_id{tag(11)},spawn{tag(12)},reflection{tag(20)},closed{tag(20)},promise{tag(21)},type{tag(8)};
struct Array {uint64_t header[4];void *items[2];} array_obj{};
std::unordered_map<uintptr_t,void*> roots;
uintptr_t next_root=1;int sends=0,stock[800]={},atk=0,def=0,sta=0,cp=100,species=25;
bool shiny=false,fresh=true,allowed=true,visible=true,revoke_reflection=false,throw_send=false;
uint64_t encounter=0xfedcba9876543210ULL, sent_encounter=0;
int req_item=0;uint64_t req_encounter=0;void *req_spawn=nullptr;
std::string config="1;1;1;0;1;1500;30;3;1;";
int sample(PgrRuntimeTransportSample *out){out->snapshot_fresh=fresh;out->fault=0;return 0;}
int get_bool(uint32_t field,int *out){assert(field==36||field==38);*out=field==36?allowed:visible;return 0;}
int get_string(uint32_t field,char *out,uint32_t cap,uint32_t *len){assert(field==7);*len=config.size();if(cap<config.size())return -1;memcpy(out,config.data(),config.size());return 0;}
PgrRuntimeTransportApi transport{};
uintptr_t gc_new(void *o,int){auto h=next_root++;roots[h]=o;return h;}
void *gc_get(uintptr_t h){auto i=roots.find(h);return i==roots.end()?nullptr:i->second;}
void gc_free(uintptr_t h){assert(roots.erase(h)==1);}
void *object_class(void *o){return static_cast<Obj*>(o)->cls;}
void *object_new(void *cls){assert(cls==request.cls);req_item=0;req_encounter=0;req_spawn=nullptr;return &request;}
void *unbox(void *o){return &static_cast<Obj*>(o)->value;}
enum Fields {MAP=101,SPECIES,CP,ATK,DEF,STA,MULT,ADD,DISPLAY,SHINY,ITEM,ENCID,SPAWN,STATUS,RPC};
enum Methods {ENCOUNTER=201,SPAWN_ID,COUNT,CTOR,OPEN,MAKE,CLOSED};
void field_get(void *o,void *field,void *out){
 switch(uintptr_t(field)){
 case MAP:assert(o==&wild);*static_cast<void**>(out)=&pokemon;break;
 case SPECIES:*static_cast<int*>(out)=species;break;case CP:*static_cast<int*>(out)=cp;break;
 case ATK:*static_cast<int*>(out)=atk;break;case DEF:*static_cast<int*>(out)=def;break;case STA:*static_cast<int*>(out)=sta;break;
 case MULT:case ADD:*static_cast<float*>(out)=0;break;
 case DISPLAY:*static_cast<void**>(out)=&display;break;case SHINY:*static_cast<uint8_t*>(out)=shiny;break;
 case ITEM:*static_cast<int*>(out)=req_item;break;case ENCID:*static_cast<uint64_t*>(out)=req_encounter;break;
 case SPAWN:*static_cast<void**>(out)=req_spawn;break;case STATUS:*static_cast<int*>(out)=int(response.value);break;
 case RPC:assert(o==&bag);*static_cast<void**>(out)=&rpc;break;default:assert(false);
 }
}
void field_set(void *o,void *field,void *value){assert(o==&request);if(uintptr_t(field)==ITEM)req_item=*static_cast<int*>(value);else{assert(uintptr_t(field)==ENCID);req_encounter=*static_cast<uint64_t*>(value);}}
void set_ref(void *o,void *field,void *value){assert(o==&request&&uintptr_t(field)==SPAWN);req_spawn=value;}
void *invoke(void *method,void *o,void **args,void **exception){
 *exception=nullptr;
 switch(uintptr_t(method)){
 case COUNT:{assert(o==&bag);int item=*static_cast<int*>(args[0]);assert(item>=1&&item<800&&item!=4);boxed_int.value=stock[item];return &boxed_int;}
 case ENCOUNTER:assert(o==&wild);boxed_id.value=encounter;return &boxed_id;
 case SPAWN_ID:return &spawn;
 case CTOR:return nullptr;
 case MAKE:if(revoke_reflection)allowed=false;return &closed;
 case CLOSED:
  assert(o==&rpc);assert(*static_cast<int*>(args[0])==42);assert(args[1]==&request);
  assert(*static_cast<int*>(args[2])==0&&*static_cast<int*>(args[3])==10000&&*static_cast<int*>(args[4])==0);
  assert(req_spawn==&spawn);sent_encounter=req_encounter;++sends;
  if(throw_send){*exception=tag(999);return nullptr;}return &promise;
 default:assert(false);return nullptr;
 }
}
void *method_name(void *,const char *name,int count){if(!strcmp(name,"SendRpc")){assert(count==5);return tag(OPEN);}assert(!strcmp(name,"MakeGenericMethod")&&count==1);return tag(MAKE);}
bool is_generic(void *m){return m==tag(OPEN);}
unsigned param_count(void *m){assert(m==tag(OPEN)||m==tag(CLOSED));return 5;}
void *method_object(void *,void *){return &reflection;}
void *class_type(void *c){return c;}
void *type_object(void *){return &type;}
void barrier(void *,void **slot,void *value){*slot=value;}
void *from_reflection(void *o){assert(o==&closed);return tag(CLOSED);}
bool inflated(void *m){return m==tag(CLOSED);}
void *array_new(void *,uintptr_t size){assert(size==2);return &array_obj;}
PgrSmartBinding binding(){
 PgrSmartBinding b{};b.version=1;b.size=sizeof(b);
 b.gc_new=(void*)gc_new;b.gc_get=(void*)gc_get;b.gc_free=(void*)gc_free;b.object_get_class=(void*)object_class;b.object_new=(void*)object_new;b.object_unbox=(void*)unbox;b.runtime_invoke=(void*)invoke;
 b.field_get_value=(void*)field_get;b.field_set_value=(void*)field_set;b.field_set_value_object=(void*)set_ref;b.array_new=(void*)array_new;
 b.class_get_method_from_name=(void*)method_name;b.method_is_generic=(void*)is_generic;b.method_get_param_count=(void*)param_count;b.method_get_object=(void*)method_object;b.class_get_type=(void*)class_type;b.type_get_object=(void*)type_object;b.gc_wbarrier_set_field=(void*)barrier;b.method_get_from_reflection=(void*)from_reflection;b.method_is_inflated=(void*)inflated;
 b.class_wildmap=wild.cls;b.class_pokemon_proto=pokemon.cls;b.class_display_proto=display.cls;b.class_item_bag=bag.cls;b.class_rpc=rpc.cls;b.class_use_item_req=request.cls;b.class_use_item_out=response.cls;b.class_type=type.cls;b.class_int=boxed_int.cls;b.class_bool=tag(10);b.class_ulong=boxed_id.cls;b.class_string=spawn.cls;
 void **fields[]={&b.f_map_pokemon,&b.f_proto_id,&b.f_proto_cp,&b.f_proto_atk,&b.f_proto_def,&b.f_proto_sta,&b.f_proto_cpmul,&b.f_proto_addcpmul,&b.f_proto_display,&b.f_display_shiny,&b.f_req_item,&b.f_req_encid,&b.f_req_spawn,&b.f_out_status,&b.f_bag_rpc};
 for(unsigned i=0;i<15;i++)*fields[i]=tag(MAP+i);
 b.m_get_encounter_id=tag(ENCOUNTER);b.m_get_spawn_id=tag(SPAWN_ID);b.m_item_count=tag(COUNT);b.m_req_ctor=tag(CTOR);b.rpc_id=42;b.status_success=1;return b;
}
PgrSmartSettings parse(const std::string &s){PgrSmartSettings out{};assert(pgr_smart_parse_settings(s.data(),s.size(),&out)==0);return out;}
void pure_tests(){
 assert(pgr_smart_parse_settings(nullptr,0,nullptr)==PGR_SMART_ERR_NULL_ARG);
 auto s=parse("1;0;1;0;1;1500;30;3;1;");assert(!s.enabled&&s.ball==1);
 for(const std::string &bad:{"","2;1;1;0;1;1500;30;3;1;","1;1;4;0;1;1500;30;3;1;","1;1;1;707;1;1500;30;3;1;","1;1;1;0;2;1500;30;3;1;","1;1;1;0;1;10001;30;3;1;","1;1;1;0;1;1500;51;3;1;","1;1;1;0;1;1500;30;4;1;","1;1;1;0;1;1500;30;3;2;","1;1;1;0;1;1500;30;3;1;0","1;1;1;0;1;1500;30;3;1;2001","1;1;1;0;1;1500;30;3;1;1,","1;1;1;0;1;1500;30;3;1;1;;"})assert(!parse(bad).enabled);
 assert(!parse(std::string(4097,'x')).enabled);
 std::string ids;for(int i=1;i<=200;i++){if(i>1)ids+=',';ids+=std::to_string(i);}assert(parse("1;1;1;0;1;1500;30;3;1;"+ids).species_count==200);assert(!parse("1;1;1;0;1;1500;30;3;1;"+ids+",201").enabled);
 s=parse("1;1;2;701;1;1500;30;2;0;25,1,25");assert(s.species_count==2&&s.species[0]==1&&s.species[1]==25);
 PgrSmartDecision d{};
 pgr_smart_decide(&s,true,0,false,0,-1,25,&d);assert(d.ball==3&&d.berry==706); // premium beats candy
 pgr_smart_decide(&s,false,41,true,0,-1,25,&d);assert(d.ball==3&&d.berry==706);
 pgr_smart_decide(&s,false,40,true,1499,59,25,&d);assert(d.ball==2&&d.berry==705&&d.grade==2&&!d.curve);
 pgr_smart_decide(&s,false,45,false,1500,-1,99,&d);assert(d.ball==3&&d.berry==701); // invalid IV never implies premium
 pgr_smart_decide(&s,false,0,true,0,60,99,&d);assert(d.ball==3);
 s.high_cp=0;s.high_level=0;s.premium=0;pgr_smart_decide(&s,true,45,true,10000,100,25,&d);assert(d.ball==2&&d.berry==705);
 s.enabled=0;pgr_smart_decide(&s,true,45,true,10000,100,25,&d);assert(!d.enabled&&d.ball==0);
}
}
extern "C" const void *pgrazor_runtime_transport_api(uint32_t version){return version==1?&transport:nullptr;}
int main(){
 pure_tests();transport.version=1;transport.size=sizeof(transport);transport.sample=sample;transport.get_bool=get_bool;transport.get_string=get_string;
 auto b=binding();assert(pgr_smart_bind(nullptr)==PGR_SMART_ERR_NULL_ARG);auto bad=b;bad.gc_new=nullptr;assert(pgr_smart_bind(&bad)==PGR_SMART_ERR_MISSING_PTR);assert(pgr_smart_bind(&b)==0);assert(pgr_smart_bind(&b)==0);bad=b;bad.rpc_id++;assert(pgr_smart_bind(&bad)==PGR_SMART_ERR_RECONFIG);
 auto tid=uint64_t(gettid());PgrSmartDecision d{};stock[1]=10;stock[4]=999;stock[705]=5;
 config="1;1;3;705;1;1500;30;3;1;25";
 assert(pgr_smart_plan(&wild,&bag,tid,&d)==0&&d.ball==1&&d.berry==705);assert(roots.empty());
 stock[705]=0;assert(pgr_smart_plan(&wild,&bag,tid,&d)==0&&d.berry==0);assert(roots.empty());
 stock[1]=0;assert(pgr_smart_plan(&wild,&bag,tid,&d)==PGR_SMART_NO_BALLS);assert(roots.empty()); // Master stock cannot rescue
 stock[1]=10;assert(pgr_smart_plan(&wild,&bag,tid+1,&d)==0&&!d.enabled);assert(roots.empty());
 fresh=false;assert(pgr_smart_plan(&wild,&bag,tid,&d)==0&&!d.enabled);fresh=true;
 uint64_t ph=99,rh=99;
 assert(pgr_smart_berry(&wild,&bag,705,tid,2500,&ph,&rh)==0&&ph==0&&rh==0&&sends==0);assert(roots.empty());
 stock[705]=5;assert(pgr_smart_berry(&wild,&bag,705,tid,2500,&ph,&rh)==1);assert(sends==1&&sent_encounter==encounter&&req_item==705&&roots.size()==2);gc_free(ph);gc_free(rh);assert(roots.empty());
 revoke_reflection=true;assert(pgr_smart_berry(&wild,&bag,705,tid,2500,&ph,&rh)==0&&ph==0&&rh==0&&sends==1);assert(roots.empty());revoke_reflection=false;allowed=true;
 visible=false;assert(pgr_smart_berry(&wild,&bag,705,tid,2500,&ph,&rh)==1&&sends==2);gc_free(ph);gc_free(rh);assert(roots.empty());visible=true;
 assert(pgr_smart_berry(&wild,&bag,4,tid,2500,&ph,&rh)==PGR_SMART_ERR_SHAPE&&sends==2);assert(roots.empty());
 throw_send=true;assert(pgr_smart_berry(&wild,&bag,705,tid,2500,&ph,&rh)==PGR_SMART_ERR_MANAGED&&sends==3&&ph==0&&rh==0);assert(roots.empty());
 response.value=1;assert(pgr_smart_berry_success(&response,tid)==1);response.value=0;assert(pgr_smart_berry_success(&response,tid)==PGR_SMART_NOT_SUCCESS);assert(pgr_smart_berry_success(&spawn,tid)==PGR_SMART_ERR_IDENTITY);
 std::cout<<"smart_catch parser, policy, stock, RPC identity, revocation and GC fixture passed\n";
}

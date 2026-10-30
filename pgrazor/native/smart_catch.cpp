#include "smart_catch.h"
#include "runtime_transport.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <time.h>
#include <unistd.h>

namespace {
PgrSmartBinding b{};
std::mutex bind_lock;
std::atomic<bool> bound{false},busy{false};
std::atomic<uint64_t> owner_tid{0};
thread_local bool (*admission_guard)(void *)=nullptr;
thread_local void *admission_context=nullptr;
using Invoke=void*(*)(void*,void*,void**,void**);
uint64_t now(){timespec t{};return clock_gettime(CLOCK_BOOTTIME,&t)==0?uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000:0;}
void *klass(void *o){return o?reinterpret_cast<void*(*)(void*)>(b.object_get_class)(o):nullptr;}
bool exact(void *o,void *k){return o&&k&&klass(o)==k;}
struct Root {
 uint64_t h=0;
 explicit Root(void *o=nullptr){if(o)h=reinterpret_cast<uintptr_t(*)(void*,int)>(b.gc_new)(o,1);}
 ~Root(){if(h)reinterpret_cast<void(*)(uintptr_t)>(b.gc_free)(h);}
 Root(const Root&)=delete;Root&operator=(const Root&)=delete;
 void *get()const{return h?reinterpret_cast<void*(*)(uintptr_t)>(b.gc_get)(h):nullptr;}
 uint64_t take(){auto n=h;h=0;return n;}
};
struct Operation {
 bool ok=false;
 explicit Operation(uint64_t owner){
  if(!bound.load()||!owner||owner!=uint64_t(gettid()))return;
  uint64_t unset=0;owner_tid.compare_exchange_strong(unset,owner);
  if(owner_tid.load()!=owner)return;
  bool clear=false;ok=busy.compare_exchange_strong(clear,true);
 }
 ~Operation(){if(ok)busy.store(false);}
};
template<class T>T field(void *o,void *f){T v{};reinterpret_cast<void(*)(void*,void*,void*)>(b.field_get_value)(o,f,&v);return v;}
void set_field(void *o,void *f,void *v){reinterpret_cast<void(*)(void*,void*,void*)>(b.field_set_value)(o,f,v);}
void *invoke(void *m,void *o,void **args,int &error){void *ex=nullptr;void *v=reinterpret_cast<Invoke>(b.runtime_invoke)(m,o,args,&ex);if(ex)error=PGR_SMART_ERR_MANAGED;return ex?nullptr:v;}
template<class T>bool unbox(void *o,void *k,T &value){if(!exact(o,k))return false;void *p=reinterpret_cast<void*(*)(void*)>(b.object_unbox)(o);if(!p)return false;std::memcpy(&value,p,sizeof(T));return true;}
bool berry_valid(int n){return n==701||n==703||n==705||n==706||n==708;}
bool number(const std::string &s,int lo,int hi,int &v){
 if(s.empty()||s.size()>6)return false;int n=0;
 for(char c:s){if(c<'0'||c>'9')return false;n=n*10+c-'0';}
 if(n<lo||n>hi)return false;v=n;return true;
}
bool read_config(std::string &text,PgrSmartSettings &settings){
 const auto *t=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));
 PgrRuntimeTransportSample sample{};sample.version=1;sample.size=sizeof(sample);
 char buf[PGR_SMART_CONFIG_MAX+1];uint32_t len=0;int allowed=0;
 if(!t||t->sample(&sample)!=0||!sample.snapshot_fresh||sample.fault||
    t->get_bool(36,&allowed)!=0||!allowed||t->get_string(7,buf,sizeof(buf),&len)!=0||len>PGR_SMART_CONFIG_MAX)return false;
 text.assign(buf,len);pgr_smart_parse_settings(text.data(),text.size(),&settings);return settings.enabled!=0;
}
bool gate(uint64_t deadline,const std::string &expected){
 if(admission_guard&&!admission_guard(admission_context))return false;
 uint64_t time=now();if(!time||time>=deadline)return false;
 std::string current;PgrSmartSettings settings{};
 if(!read_config(current,settings)||current!=expected)return false;
 // Foreground/runtime authorization is checked by read_config. Map visibility
 // only controls the marker; storage and other menus need not stop catching.
 return true;
}
int quantity(Root &bag,int item,int &count){
 int err=0;void *args[]={&item};void *v=invoke(b.m_item_count,bag.get(),args,err);
 if(err)return err;if(!unbox(v,b.class_int,count)||count<0)return PGR_SMART_ERR_SHAPE;return 0;
}
void *method(void *k,const char *name,int argc){return reinterpret_cast<void*(*)(void*,const char*,int)>(b.class_get_method_from_name)(k,name,argc);}
bool generic(void *m){return m&&reinterpret_cast<bool(*)(void*)>(b.method_is_generic)(m);}
unsigned params(void *m){return reinterpret_cast<unsigned(*)(void*)>(b.method_get_param_count)(m);}
}

void pgr_smart_set_admission_guard(bool (*guard)(void *),void *context){admission_guard=guard;admission_context=context;}
extern "C" int pgr_smart_parse_settings(const char *text,size_t len,PgrSmartSettings *out){
 if(!out)return PGR_SMART_ERR_NULL_ARG;
 *out={};out->ball=1;out->premium=1;out->high_cp=1500;out->high_level=30;out->grade=3;out->curve=1;
 if(!text||len>PGR_SMART_CONFIG_MAX)return 0;
 std::vector<std::string> parts;size_t start=0;
 for(size_t i=0;i<=len;++i)if(i==len||text[i]==';'){parts.emplace_back(text+start,i-start);start=i+1;if(parts.size()>10)return 0;}
 if(parts.size()!=10||parts[0]!="1")return 0;
 PgrSmartSettings s{};
 if(!number(parts[1],0,1,s.enabled)||!number(parts[2],1,3,s.ball)||!number(parts[3],0,708,s.berry)||
    (s.berry&&!berry_valid(s.berry))||!number(parts[4],0,1,s.premium)||!number(parts[5],0,10000,s.high_cp)||
    !number(parts[6],0,50,s.high_level)||!number(parts[7],0,3,s.grade)||!number(parts[8],0,1,s.curve))return 0;
 if(!parts[9].empty()){
  size_t pos=0;while(pos<=parts[9].size()){
   size_t end=parts[9].find(',',pos);if(end==std::string::npos)end=parts[9].size();int id=0;
   if(s.species_count==PGR_SMART_MAX_SPECIES||!number(parts[9].substr(pos,end-pos),1,2000,id))return 0;
   s.species[s.species_count++]=id;if(end==parts[9].size())break;pos=end+1;
  }
  std::sort(s.species,s.species+s.species_count);
  s.species_count=std::unique(s.species,s.species+s.species_count)-s.species;
 }
 *out=s;return 0;
}
extern "C" void pgr_smart_decide(const PgrSmartSettings *s,bool shiny,int iv_sum,bool iv_valid,int cp,int level_half,int species,PgrSmartDecision *out){
 if(!out)return;*out={};if(!s||!s->enabled)return;
 out->enabled=true;out->ball=s->ball;out->berry=s->berry;out->grade=s->grade;out->curve=s->curve!=0;
 if(s->premium&&(shiny||(iv_valid&&iv_sum>=41&&iv_sum<=45))){out->ball=3;out->berry=706;return;}
 if((s->high_cp>0&&cp>=s->high_cp)||(s->high_level>0&&level_half>=2*s->high_level))out->ball=3;
 if(std::binary_search(s->species,s->species+s->species_count,species))out->berry=705;
}
extern "C" int pgr_smart_bind(const PgrSmartBinding *value){
 if(!value)return PGR_SMART_ERR_NULL_ARG;
 if(value->version!=PGR_SMART_BINDING_VERSION)return PGR_SMART_ERR_BAD_VERSION;
 if(value->size!=sizeof(*value))return PGR_SMART_ERR_BAD_SIZE;
 const auto *slots=reinterpret_cast<void*const*>(&value->gc_new);
 size_t count=(offsetof(PgrSmartBinding,class_gm)-offsetof(PgrSmartBinding,gc_new))/sizeof(void*);
 for(size_t i=0;i<count;++i)if(!slots[i])return PGR_SMART_ERR_MISSING_PTR;
 if(value->rpc_id<=0)return PGR_SMART_ERR_SHAPE;
 const void *optional[]={value->class_gm,value->class_single,value->f_bag_gm,value->m_gm_level,value->m_gm_mult};
 unsigned present=0;for(auto p:optional)if(p)++present;if(present&&present!=5)return PGR_SMART_ERR_MISSING_PTR;
 std::lock_guard<std::mutex> lock(bind_lock);
 if(bound.load())return std::memcmp(&b,value,sizeof(b))==0?0:PGR_SMART_ERR_RECONFIG;
 b=*value;bound.store(true);return 0;
}
extern "C" int pgr_smart_plan(void *target,void *itemBag,uint64_t owner,PgrSmartDecision *out){
 if(!out)return PGR_SMART_ERR_NULL_ARG;*out={};Operation op(owner);if(!op.ok)return 0;
 PgrSmartSettings settings{};std::string config;if(!read_config(config,settings))return 0;
 Root map(target),bag(itemBag);if(!map.h||!bag.h)return PGR_SMART_ERR_GCHANDLE;
 if(!exact(map.get(),b.class_wildmap)||!exact(bag.get(),b.class_item_bag))return PGR_SMART_ERR_IDENTITY;
 Root pokemon(field<void*>(map.get(),b.f_map_pokemon));
 if(!pokemon.h)return PGR_SMART_ERR_GCHANDLE;
 if(!exact(pokemon.get(),b.class_pokemon_proto))return PGR_SMART_ERR_IDENTITY;
 const int atk=field<int>(pokemon.get(),b.f_proto_atk),def=field<int>(pokemon.get(),b.f_proto_def),sta=field<int>(pokemon.get(),b.f_proto_sta);
 const int cp=field<int>(pokemon.get(),b.f_proto_cp),species=field<int>(pokemon.get(),b.f_proto_id);
 const float multiplier=field<float>(pokemon.get(),b.f_proto_cpmul)+field<float>(pokemon.get(),b.f_proto_addcpmul);
 bool shiny=false;void *display=field<void*>(pokemon.get(),b.f_proto_display);
 if(display){if(!exact(display,b.class_display_proto))return PGR_SMART_ERR_IDENTITY;shiny=field<uint8_t>(display,b.f_display_shiny)!=0;}
 int level_half=-1;
 if(b.class_gm&&std::isfinite(multiplier)&&multiplier>0){
  Root gm(field<void*>(bag.get(),b.f_bag_gm));if(gm.h&&exact(gm.get(),b.class_gm)){
   int err=0,level=0;float value=multiplier;void *args[]={gm.get(),&value};
   void *box=invoke(b.m_gm_level,nullptr,args,err);if(err)return err;
   if(!unbox(box,b.class_int,level))return PGR_SMART_ERR_SHAPE;
   if(level>=1&&level<=50){void *pa[]={gm.get(),&level};float base=0;box=invoke(b.m_gm_mult,nullptr,pa,err);if(err)return err;
    if(!unbox(box,b.class_single,base)||!std::isfinite(base))return PGR_SMART_ERR_SHAPE;
    level_half=level*2+(multiplier>base+0.00001f?1:0);
   }
  }
 }
 pgr_smart_decide(&settings,shiny,atk+def+sta,atk>=0&&atk<=15&&def>=0&&def<=15&&sta>=0&&sta<=15,cp,level_half,species,out);
 int stocks[4]={};for(int item=1;item<=3;++item){int rc=quantity(bag,item,stocks[item]);if(rc)return rc;}
 if(stocks[out->ball]<1){out->ball=0;for(int item=1;item<=3;++item)if(stocks[item]>0){out->ball=item;break;}}
 if(!out->ball)return PGR_SMART_NO_BALLS;
 if(out->berry){int count=0;int rc=quantity(bag,out->berry,count);if(rc)return rc;if(!count)out->berry=0;}
 return 0;
}

extern "C" int pgr_smart_berry(void *target,void *itemBag,int32_t item,uint64_t owner,uint64_t lease,uint64_t *promiseHandle,uint64_t *reflectionHandle){
 if(promiseHandle)*promiseHandle=0;if(reflectionHandle)*reflectionHandle=0;
 if(!promiseHandle||!reflectionHandle)return PGR_SMART_ERR_NULL_ARG;
 if(!berry_valid(item))return PGR_SMART_ERR_SHAPE;
 Operation op(owner);if(!op.ok||!lease)return 0;
 uint64_t start=now();if(!start)return PGR_SMART_ERR_CLOCK_FAILURE;
 const uint64_t deadline=start+std::min<uint64_t>(lease,PGR_SMART_MAX_LEASE_MS);
 PgrSmartSettings settings{};std::string config;if(!read_config(config,settings)||!gate(deadline,config))return 0;
 Root map(target),bag(itemBag);if(!map.h||!bag.h)return PGR_SMART_ERR_GCHANDLE;
 if(!exact(map.get(),b.class_wildmap)||!exact(bag.get(),b.class_item_bag))return PGR_SMART_ERR_IDENTITY;
 int stock=0,err=quantity(bag,item,stock);if(err)return err;if(stock<1)return 0;
 Root rpc(field<void*>(bag.get(),b.f_bag_rpc));if(!rpc.h)return PGR_SMART_ERR_GCHANDLE;
 if(!exact(rpc.get(),b.class_rpc))return PGR_SMART_ERR_IDENTITY;
 void *open=method(b.class_rpc,"SendRpc",5);
 if(!generic(open)||params(open)!=5)return PGR_SMART_ERR_REFLECT;
 Root reflected(reinterpret_cast<void*(*)(void*,void*)>(b.method_get_object)(open,b.class_rpc));
 Root types(reinterpret_cast<void*(*)(void*,uintptr_t)>(b.array_new)(b.class_type,2));
 if(!reflected.h||!types.h)return PGR_SMART_ERR_GCHANDLE;
 void *args_types[]={b.class_use_item_req,b.class_use_item_out};
 for(unsigned i=0;i<2;++i){
  void *type=reinterpret_cast<void*(*)(void*)>(b.class_get_type)(args_types[i]);
  Root obj(type?reinterpret_cast<void*(*)(void*)>(b.type_get_object)(type):nullptr);
  if(!obj.h)return PGR_SMART_ERR_REFLECT;
  void **slot=reinterpret_cast<void**>(static_cast<char*>(types.get())+PGR_SMART_ARRAY_DATA_OFF+i*sizeof(void*));
  reinterpret_cast<void(*)(void*,void**,void*)>(b.gc_wbarrier_set_field)(types.get(),slot,obj.get());
 }
 void *make=method(klass(reflected.get()),"MakeGenericMethod",1);if(!make)return PGR_SMART_ERR_REFLECT;
 void *params_make[]={types.get()};Root closed(invoke(make,reflected.get(),params_make,err));
 if(err)return err;if(!closed.h)return PGR_SMART_ERR_GCHANDLE;
 void *send=reinterpret_cast<void*(*)(void*)>(b.method_get_from_reflection)(closed.get());
 if(!send||!reinterpret_cast<bool(*)(void*)>(b.method_is_inflated)(send)||params(send)!=5)return PGR_SMART_ERR_REFLECT;
 Root request(reinterpret_cast<void*(*)(void*)>(b.object_new)(b.class_use_item_req));if(!request.h)return PGR_SMART_ERR_GCHANDLE;
 invoke(b.m_req_ctor,request.get(),nullptr,err);if(err)return err;
 uint64_t encounter=0;void *box=invoke(b.m_get_encounter_id,map.get(),nullptr,err);if(err)return err;
 if(!unbox(box,b.class_ulong,encounter)||!encounter)return PGR_SMART_ERR_SHAPE;
 Root spawn(invoke(b.m_get_spawn_id,map.get(),nullptr,err));if(err)return err;
 if(!spawn.h||!exact(spawn.get(),b.class_string))return PGR_SMART_ERR_IDENTITY;
 set_field(request.get(),b.f_req_item,&item);set_field(request.get(),b.f_req_encid,&encounter);
 reinterpret_cast<void(*)(void*,void*,void*)>(b.field_set_value_object)(request.get(),b.f_req_spawn,spawn.get());
 if(field<uint64_t>(request.get(),b.f_req_encid)!=encounter||field<int>(request.get(),b.f_req_item)!=item||field<void*>(request.get(),b.f_req_spawn)!=spawn.get())return PGR_SMART_ERR_SHAPE;
 if(!gate(deadline,config))return 0;
 int rpc_id=b.rpc_id,zero=0,timeout=10000;void *args[]={&rpc_id,request.get(),&zero,&timeout,&zero};
 Root promise(invoke(send,rpc.get(),args,err));if(err)return err;
 if(!promise.h)return PGR_SMART_ERR_GCHANDLE; // Issued but uncertain: caller must not retry.
 *promiseHandle=promise.take();*reflectionHandle=closed.take();return PGR_SMART_ISSUED;
}
extern "C" int pgr_smart_berry_success(void *response,uint64_t owner){
 Operation op(owner);if(!op.ok)return 0;
 if(!exact(response,b.class_use_item_out))return PGR_SMART_ERR_IDENTITY;
 return field<int>(response,b.f_out_status)==b.status_success?PGR_SMART_ISSUED:PGR_SMART_NOT_SUCCESS;
}

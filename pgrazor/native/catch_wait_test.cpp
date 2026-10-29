// Actual core query58 + real47 validators + real Dobby. Fake managed universe;
// explicit native segment/target seam only. No live game/Houdini claim.
#define main included_metadata_fixture_main
#include "runtime_metadata_test.cpp"
#undef main
#include "catch_wait.h"
#include "catch_wait_schema.h"
#include <cmath>
#include <map>
#include <functional>
namespace cw {
using namespace pgr_catch_wait_schema;
const PgrCatchWaitApi *api;
PgrBootBinding binding{};PgrCatchWaitConfig config{};
FClass klass[5]{};FType types[5]{};
FType int_type{"System.Int32",nullptr},bool_type{"System.Boolean",nullptr},float_type{"System.Single",nullptr},void_type{"System.Void",nullptr};
FField field[7],literal;FMethod methods[4];
unsigned char *module;constexpr size_t module_size=0xe130000;
struct Object {alignas(8) unsigned char bytes[256]{};};
Object coroutine,capture,response,wait_object;
std::map<uintptr_t,void *> roots;uintptr_t next_root=1;unsigned makes=0,frees=0,original_calls=0;
thread_local bool attached=true;
int thread_token,method_token;void *seen_self;const void *seen_method;
bool throw_original=false,nested=false,null_target=false;uint8_t original_result=1;
struct OriginalFailure {int marker;};
using Move=uint8_t(*)(void *,const void *);
extern "C" __attribute__((noinline)) uint8_t cw_move(void *,const void *);
Move volatile call=cw_move;
void *current(){return attached?&thread_token:nullptr;}
uintptr_t make(void *o,int pinned){assert(o&&pinned==1);++makes;auto h=next_root++;roots[h]=o;return h;}
void *target(uintptr_t h){assert(roots.count(h));return null_target?nullptr:roots[h];}
void free_root(uintptr_t h){assert(roots.erase(h)==1);++frees;}
void *object_class(void *o){return o?*static_cast<void **>(o):nullptr;}
size_t width(const FField *f){return f->type->name=="System.Boolean"?1:(f->type->name=="System.Single"||f->type->name=="System.Int32"||f->type->name==kStatusEnum?4:8);}
void field_get(void *o,const void *f,void *out){auto v=static_cast<const FField *>(f);std::memcpy(out,static_cast<unsigned char *>(o)+v->offset,width(v));}
void field_set(void *o,const void *f,void *in){auto v=static_cast<const FField *>(f);std::memcpy(static_cast<unsigned char *>(o)+v->offset,in,width(v));}
void static_get(const void *f,void *out){assert(f==&literal);*static_cast<int32_t *>(out)=73;}
const char *class_name(const void *k){return k==&klass[0]?"ebk":"other";}
void *nested_class(const void *k,void **it){if(k!=&klass[1]||*it)return nullptr;*it=&klass[0];return &klass[0];}
const void *declaring(const void *k){return k==&klass[0]?&klass[1]:nullptr;}
int segment(void *,const void *p,uint64_t n,PgrMetaSegment *out){*out={};auto a=reinterpret_cast<uintptr_t>(p),b=reinterpret_cast<uintptr_t>(module);
 if(a>=b&&a-b<=module_size&&n<=module_size-(a-b)){out->found=out->readable=out->host_exec=1;out->seg_base=module;out->seg_size=module_size;}return 0;}
template<class T>void put(Object &o,size_t off,T value){std::memcpy(o.bytes+off,&value,sizeof(value));}
float duration(){float out;std::memcpy(&out,wait_object.bytes+0x10,4);return out;}
void reset_objects(){coroutine={};capture={};response={};wait_object={};
 put(coroutine,0,&klass[0]);put(capture,0,&klass[1]);put(response,0,&klass[2]);put(wait_object,0,&klass[3]);
 put(coroutine,0x10,int32_t(7));put(coroutine,0x18,&wait_object);put(coroutine,0x20,&capture);put(coroutine,0x28,&response);put(coroutine,0x60,uint8_t(1));put(response,0x30,int32_t(73));put(wait_object,0x10,5.0f);}
extern "C" __attribute__((noinline)) uint8_t cw_move(void *self,const void *method){
 asm volatile(".rept 32; nop; .endr" ::: "memory");++original_calls;seen_self=self;seen_method=method;
 if(nested){nested=false;assert(call(self,method)==original_result);}
 if(throw_original)throw OriginalFailure{99};return original_result;
}
void setup(){
 module=static_cast<unsigned char *>(mmap(nullptr,module_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));assert(module!=MAP_FAILED);
 g_classes.clear();g_fields.clear();g_methods.clear();g_asms[0]=IMGA;g_asms[1]=IMGB;g_asm_count=2;
 types[0]={"Niantic.Holoholo.Encounter.EncounterCaptureState.ebk",&klass[0]};klass[0].type=&types[0];
 for(const auto &s:kClasses){types[s.owner]={s.canonical,&klass[s.owner]};klass[s.owner].type=&types[s.owner];g_classes.push_back({IMGA,s.name_space,s.name,&klass[s.owner]});}
 types[4]={kStatusEnum,&klass[4]};klass[4]={&types[4],true,true,&int_type,4,4};
 for(unsigned i=0;i<7;i++){const auto &s=kFields[i];FType *t=nullptr;
  if(i==0)t=&int_type;else if(i==4)t=&bool_type;else if(i==5)t=&float_type;else if(i==6)t=&types[4];else if(i==2)t=&types[1];else if(i==3)t=&types[2];else t=new FType{s.type_name,nullptr};
  field[i]={s.name,&klass[s.owner],t,0,s.offset_is_exact?s.exact_offset:0x30};g_fields.push_back(&field[i]);}
 literal={kSuccessLiteral,&klass[4],&types[4],0x8050,0};g_fields.push_back(&literal);
 for(unsigned i=0;i<4;i++){const auto &s=kMethods[i];methods[i]={};auto &m=methods[i];m.name=s.name;m.klass=&klass[s.owner];m.ret=i==0?&bool_type:(i==1?&float_type:&void_type);m.code=module+s.rva;g_methods.push_back(&m);std::memcpy(module+s.rva,s.prologue,s.prologue_size);}
 for(const auto &s:{kYieldTailSite,kYieldReturnSite,kStepSite})std::memcpy(module+s.rva,s.bytes,s.size);
 // Independent source-known constants guard accidental descriptor drift.
 assert(field[0].name=="drny"&&field[0].offset==0x10&&field[4].offset==0x60&&methods[0].code==module+0xacf24fc);
 binding.version=1;binding.size=sizeof(binding);binding.module_base=module;binding.module_size=module_size;binding.domain=&g_domain_tok;binding.meta=make_exports();
 binding.meta.class_get_nested_types=reinterpret_cast<void *>(nested_class);binding.meta.class_get_declaring_type=reinterpret_cast<void *>(declaring);
 binding.runtime.thread_current=reinterpret_cast<void *>(current);binding.runtime.object_get_class=reinterpret_cast<void *>(object_class);
 binding.runtime.gchandle_new=reinterpret_cast<void *>(make);binding.runtime.gchandle_get_target=reinterpret_cast<void *>(target);binding.runtime.gchandle_free=reinterpret_cast<void *>(free_root);
 binding.runtime.field_get_value=reinterpret_cast<void *>(field_get);binding.runtime.field_set_value=reinterpret_cast<void *>(field_set);binding.runtime.field_static_get_value=reinterpret_cast<void *>(static_get);
 config={1,sizeof(config),&binding,reinterpret_cast<void *>(class_name),segment,nullptr,reinterpret_cast<void *>(cw_move)};reset_objects();
}
PgrCatchWaitSample sample(){PgrCatchWaitSample s{};assert(api->sample(&s)==0);return s;}
void invoke(){unsigned before=original_calls;assert(call(&coroutine,&method_token)==original_result);assert(original_calls==before+1&&seen_self==&coroutine&&seen_method==&method_token);assert(roots.empty()&&makes==frees);}
void install(){assert(api->install(&config)==0);assert(sample().ready==1);}
void run(const char *name,const std::function<void()> &fn){pid_t p=fork();assert(p>=0);if(!p){setup();fn();std::_Exit(0);}int status;assert(waitpid(p,&status,0)==p);if(!WIFEXITED(status)||WEXITSTATUS(status)){std::fprintf(stderr,"FAIL catch_wait %s\n",name);std::abort();}}
}
int main(int argc,char **argv){using namespace cw;assert(argc==2);auto lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);auto get=reinterpret_cast<GetApiFn>(dlsym(lib,"pgrazor_native_api"));assert(get);auto core=static_cast<const pgrazor_native_api_v2 *>(get(2));assert(core);assert(!core->query_feature(58,2));api=static_cast<const PgrCatchWaitApi *>(core->query_feature(58,1));assert(api&&api->size==48);
 run("bootstrap not ready",[]{assert(api->install(nullptr)==1&&!sample().ready);});
 run("bad config",[]{auto bad=config;bad.size--;assert(api->install(&bad)<0);});
 run("field offset",[]{field[0].offset++;assert(api->install(&config)<0);});
 run("field type",[]{field[1].type=&int_type;assert(api->install(&config)<0);});
 run("field flags",[]{field[2].flags=0x10;assert(api->install(&config)<0);});
 run("method static",[]{methods[0].flags=0x10;assert(api->install(&config)<0);});
 run("enum ABI",[]{klass[4].align=8;assert(api->install(&config)<0);});
 run("status offset bound",[]{field[6].offset=4097;assert(api->install(&config)<0);});
 run("generic method",[]{methods[0].generic=true;assert(api->install(&config)<0);});
 run("method return",[]{methods[0].ret=&void_type;assert(api->install(&config)<0);});
 run("code fingerprint",[]{module[0xacf4c84]^=1;assert(api->install(&config)<0);});
 run("success off on stop",[]{install();invoke();assert(duration()==5.0f&&sample().observed==1&&sample().shortened==0);assert(api->update(1,2500)==0);invoke();assert(std::fabs(duration()-0.1f)<1e-6&&sample().shortened==1);PgrCatchWaitEvent e{};assert(api->next_event(&e)==1&&e.kind==1&&e.status==73);assert(api->stop()==0);reset_objects();invoke();assert(duration()==5.0f&&api->update(1,2500)<0);});
 run("failed status",[]{install();api->update(1,2500);put(response,0x30,int32_t(74));invoke();assert(duration()==5.0f&&sample().observed==0);});
 run("non yield",[]{install();api->update(1,2500);original_result=0;invoke();assert(sample().observed==0);original_result=1;put(coroutine,0x10,int32_t(6));invoke();assert(sample().observed==0);});
 run("bad branch",[]{install();api->update(1,2500);put(coroutine,0x60,uint8_t(2));invoke();assert(sample().fault&&duration()==5.0f);});
 run("bad duration",[]{install();api->update(1,2500);put(wait_object,0x10,31.0f);invoke();assert(sample().fault&&duration()==31.0f);});
 run("nan duration",[]{install();put(wait_object,0x10,std::nanf(""));invoke();assert(sample().fault&&sample().observed==0);});
 run("below minimum duration",[]{install();api->update(1,2500);put(wait_object,0x10,0.05f);invoke();assert(sample().observed==1&&sample().shortened==0);});
 run("wrong wait type",[]{install();put(wait_object,0,&klass[1]);invoke();assert(sample().fault&&sample().observed==0);});
 run("expiry",[]{install();api->update(1,1);usleep(20000);invoke();assert(duration()==5.0f&&sample().observed==1&&sample().shortened==0);});
 run("nested",[]{install();api->update(1,2500);nested=true;assert(call(&coroutine,&method_token)==1);assert(original_calls==2&&sample().observed==1&&sample().shortened==1&&roots.empty()&&makes==frees);});
 run("original exception",[]{install();throw_original=true;bool caught=false;try{call(&coroutine,&method_token);}catch(const OriginalFailure &e){caught=e.marker==99;}assert(caught&&original_calls==1&&sample().observed==0&&roots.empty()&&makes==frees);});
 run("unattached",[]{install();attached=false;invoke();assert(sample().fault&&makes==0);});
 run("null pinned target preserves original receiver",[]{install();null_target=true;invoke();assert(sample().fault==-26&&sample().observed==0&&makes==1&&frees==1);});
 run("foreign owner",[]{install();invoke();unsigned before=makes;std::thread t([]{invoke();});t.join();assert(sample().fault&&makes==before);});
 run("queue before mutation",[]{install();api->update(1,2500);for(unsigned i=0;i<64;i++){reset_objects();invoke();}assert(sample().queued==128&&sample().shortened==64);reset_objects();invoke();assert(sample().fault&&duration()==5.0f&&sample().shortened==64);});
 puts("PASS catch wait actual-library: schema validation, Dobby ABI, gates, pins, exception, queue and stop; fake IL2CPP not live Houdini");return 0;
}

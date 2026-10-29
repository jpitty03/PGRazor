// Independent adversarial lifetime/lease regressions. Real exported feature46
// from dlopen production library; Dobby patches real fixture originals. No copied
// observer implementation, fake hook engine, or wrapper-only result assertions.
#include "routes_observers.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <memory>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {
int cls_service,cls_progress,cls_play,cls_shared,cls_string,cls_reward,cls_rating,cls_arg,cls_retired;
int method_current,method_dismiss,field_route,field_id,thread_token,method_init,method_bjan;
struct alignas(8) Object {unsigned char bytes[512]{};};
struct Entity {void *klass;Object *current;};
struct Root {size_t entity;bool live,pinned;};
std::vector<std::unique_ptr<Object>> allocations;
std::vector<Entity> entities;
std::vector<Root> roots;
const PgrRoutesObserversApi *api=nullptr;
size_t service,progress,play,shared,string_id,reward,rating,arg;
uint64_t borrowed=0;
bool move_on_new=false,move_on_invoke=false,move_on_free=false;
bool revoke_current=false,revoke_free=false,revoke_dismiss=false,expire_dismiss=false;
unsigned init_calls=0,rating_calls=0,bjan_calls=0,dismiss_calls=0;

void put_pointer(Object *obj,size_t offset,void *value){std::memcpy(obj->bytes+offset,&value,sizeof(value));}
void put_i32(Object *obj,size_t offset,int32_t value){std::memcpy(obj->bytes+offset,&value,sizeof(value));}
void *pointer(Object *obj,size_t offset){void *value;std::memcpy(&value,obj->bytes+offset,sizeof(value));return value;}
int32_t i32(Object *obj,size_t offset){int32_t value;std::memcpy(&value,obj->bytes+offset,sizeof(value));return value;}
Object *at(size_t id){assert(id<entities.size());return entities[id].current;}
size_t current_id(void *obj){for(size_t i=0;i<entities.size();++i)if(at(i)==obj)return i;assert(!"stale or unknown managed pointer");std::abort();}
size_t create(void *klass){allocations.emplace_back(new Object);Object *obj=allocations.back().get();put_pointer(obj,0,klass);entities.push_back({klass,obj});return entities.size()-1;}
bool pinned(size_t id){for(const auto &r:roots)if(r.live&&r.entity==id&&r.pinned)return true;return false;}
bool rooted(size_t id){for(const auto &r:roots)if(r.live&&r.entity==id)return true;return false;}
void repair_references(){put_pointer(at(play),0x18,at(shared));put_pointer(at(shared),0x20,at(string_id));}
void collect(bool rooted_only){
  for(size_t i=0;i<entities.size();++i){
    if(pinned(i)||(rooted_only&&!rooted(i)))continue;
    Object *old=at(i);allocations.emplace_back(new Object);Object *fresh=allocations.back().get();
    std::memcpy(fresh,old,sizeof(*fresh));entities[i].current=fresh;
    put_pointer(old,0,&cls_retired); // retired storage remains allocated, never silently reused
  }
  repair_references();
}
uintptr_t gc_new(void *obj,int pin){
  if(!obj)return 0;size_t id=current_id(obj);roots.push_back({id,true,pin!=0});
  uintptr_t handle=roots.size();if(move_on_new)collect(true);return handle;
}
void *gc_target(uintptr_t handle){assert(handle&&handle<=roots.size());const auto &r=roots[handle-1];return r.live?at(r.entity):nullptr;}
void gc_free(uintptr_t handle){
  assert(handle&&handle<=roots.size()&&roots[handle-1].live);
  assert(handle!=borrowed&&"feature45 service handle is borrowed, never owned here");
  roots[handle-1].live=false;
  if(move_on_free)collect(false);
  if(revoke_free){revoke_free=false;api->revoke();}
}
void *object_class(void *obj){return obj?pointer(static_cast<Object *>(obj),0):nullptr;}
void field_get(void *obj,void *field,void *out){
  current_id(obj);assert(field==&field_route||field==&field_id);
  void *value=pointer(static_cast<Object *>(obj),field==&field_route?0x18:0x20);std::memcpy(out,&value,sizeof(value));
}
void *thread_current(){return &thread_token;}
int32_t string_length(void *obj){current_id(obj);return i32(static_cast<Object *>(obj),0x10);}
uint16_t *string_chars(void *obj){current_id(obj);return reinterpret_cast<uint16_t *>(static_cast<Object *>(obj)->bytes+0x18);}
void *runtime_invoke(const void *method,void *obj,void **params,void **exception){
  assert(params==nullptr);if(exception)*exception=nullptr;
  size_t id=current_id(obj);
  if(move_on_invoke)collect(false); // managed call may relocate every non-pinned object
  if(method==&method_current){assert(id==service);if(revoke_current){revoke_current=false;api->revoke();}return at(play);}
  assert(method==&method_dismiss&&(id==reward||id==rating));++dismiss_calls;
  if(revoke_dismiss){revoke_dismiss=false;api->revoke();}
  if(expire_dismiss){expire_dismiss=false;usleep(550000);}
  put_i32(at(id),PGR_RO_GUI_STATE_OFFSET,0);return nullptr;
}
using Hook=void (*)(void *,void *,const void *);
extern "C" __attribute__((noinline)) void reward_original(void *self,void *argument,const void *method){
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  assert(self==at(reward)&&argument==at(arg)&&method==&method_init);++init_calls;
}
extern "C" __attribute__((noinline)) void rating_original(void *self,void *argument,const void *method){
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  assert(self==at(rating)&&argument==at(arg)&&method==&method_init);++rating_calls;
}
extern "C" __attribute__((noinline)) void bjan_original(void *self,void *argument,const void *method){
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  assert(self==at(service)&&argument==at(progress)&&method==&method_bjan);++bjan_calls;
}
Hook volatile reward_call=reward_original,rating_call=rating_original,bjan_call=bjan_original;
void initialize(const std::string &route){
  service=create(&cls_service);progress=create(&cls_progress);play=create(&cls_play);shared=create(&cls_shared);
  string_id=create(&cls_string);reward=create(&cls_reward);rating=create(&cls_rating);arg=create(&cls_arg);
  assert(route.size()<=200);put_i32(at(string_id),0x10,route.size());
  auto *chars=reinterpret_cast<uint16_t *>(at(string_id)->bytes+0x18);
  for(size_t i=0;i<route.size();++i)chars[i]=static_cast<unsigned char>(route[i]);
  put_i32(at(progress),PGR_RO_PROGRESS_STATUS_OFFSET,PGR_RO_COMPLETION_SUCCESS);
  put_i32(at(reward),PGR_RO_GUI_STATE_OFFSET,PGR_RO_GUI_STATE_DISMISSED);
  put_i32(at(rating),PGR_RO_GUI_STATE_OFFSET,PGR_RO_GUI_STATE_DISMISSED);repair_references();
  borrowed=gc_new(at(service),0);
}
PgrRoutesObserversRequest request(uint64_t residual=PGR_RO_MAX_LEASE_MS){
  PgrRoutesObserversRequest r{};r.version=1;r.size=sizeof(r);r.tid=gettid();r.residual_ms=residual;r.generation=api->generation();return r;
}
PgrRoutesObserversOutput poll(uint64_t residual=PGR_RO_MAX_LEASE_MS){
  auto r=request(residual);PgrRoutesObserversOutput out{};int rc=api->poll(&r,&out);assert(rc>=0&&rc==out.status);return out;
}
bool event(const PgrRoutesObserversOutput &out,uint32_t type){for(uint32_t i=0;i<out.event_count;i++)if(out.events[i].type==type)return true;return false;}
void arm(const std::string &id){
  assert(id.size()<=PGR_RO_ID_MAX);PgrRoutesObserversCompleting c{};c.version=1;c.size=sizeof(c);c.tid=gettid();
  c.residual_ms=PGR_RO_MAX_LEASE_MS;c.generation=api->generation();c.consumption_handle=borrowed;c.seq=77;c.id_length=id.size();
  for(size_t i=0;i<id.size();i++)c.id[i]=static_cast<unsigned char>(id[i]);assert(api->set_completing(&c)==PGR_RO_OK);
}
void invoke_bjan(){unsigned previous=bjan_calls;bjan_call(at(service),at(progress),&method_bjan);assert(bjan_calls==previous+1);}
void confirm(){arm("route-a");invoke_bjan();auto out=poll();assert(event(out,PGR_RO_EV_REWARD_CONFIRMED));}
void queue_both(){reward_call(at(reward),at(arg),&method_init);rating_call(at(rating),at(arg),&method_init);assert(init_calls==1&&rating_calls==1);}
void load(const char *path){
  void *library=dlopen(path,RTLD_NOW|RTLD_LOCAL);assert(library);
  auto get=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(library,"pgrazor_native_api"));assert(get);
  auto *core=static_cast<const pgrazor_native_api_v2 *>(get(2));assert(core&&core->query_feature);
  api=static_cast<const PgrRoutesObserversApi *>(core->query_feature(46,1));assert(api&&api->size==sizeof(*api));
  PgrRoutesObserversConfig c{};c.version=1;c.size=sizeof(c);
  c.runtime_invoke=reinterpret_cast<void *>(runtime_invoke);c.object_get_class=reinterpret_cast<void *>(object_class);
  c.field_get_value=reinterpret_cast<void *>(field_get);c.gchandle_new=reinterpret_cast<void *>(gc_new);
  c.gchandle_get_target=reinterpret_cast<void *>(gc_target);c.gchandle_free=reinterpret_cast<void *>(gc_free);
  c.thread_current=reinterpret_cast<void *>(thread_current);c.string_length=reinterpret_cast<void *>(string_length);
  c.string_chars=reinterpret_cast<void *>(string_chars);c.class_consumption=&cls_service;c.class_progress=&cls_progress;
  c.class_play=&cls_play;c.class_shared=&cls_shared;c.class_reward_gui=&cls_reward;c.class_rating_gui=&cls_rating;
  c.m_current_route=&method_current;c.m_dismiss_gui=&method_dismiss;c.f_play_route=&field_route;c.f_shared_id=&field_id;
  c.addr_reward_init=reinterpret_cast<void *>(reward_original);c.addr_rating_init=reinterpret_cast<void *>(rating_original);
  c.addr_completion_resp=reinterpret_cast<void *>(bjan_original);assert(api->install(&c)==0);
}
const char *names[]={"control","Initialize-moving-roots","bjan-moving-invoke-and-free","long-id-prefix-collision",
  "revoke-current-read","revoke-cleanup","dismiss-revokes-second-ui","dismiss-expired-second-ui","poll-stale-generation"};
void scenario(const char *library,unsigned index){
  load(library);initialize(index==3?std::string(PGR_RO_ID_MAX,'x')+"z":"route-a");assert(poll().status==PGR_RO_OK);
  if(index==0){confirm();queue_both();usleep(110000);auto out=poll();assert(dismiss_calls==2&&event(out,PGR_RO_EV_RESULT_DISMISSED));}
  else if(index==1){confirm();move_on_new=true;reward_call(at(reward),at(arg),&method_init);assert(init_calls==1);}
  else if(index==2){arm("route-a");move_on_new=move_on_invoke=move_on_free=true;invoke_bjan();auto out=poll();assert(event(out,PGR_RO_EV_REWARD_CONFIRMED));}
  else if(index==3){arm(std::string(PGR_RO_ID_MAX,'x'));invoke_bjan();auto out=poll();assert(!event(out,PGR_RO_EV_REWARD_CONFIRMED)&&!(out.flags&PGR_RO_FLAG_COMPLETION_READY));}
  else if(index==4||index==5){arm("route-a");if(index==4)revoke_current=true;else revoke_free=true;
    invoke_bjan();auto out=poll();assert(!event(out,PGR_RO_EV_REWARD_CONFIRMED)&&!(out.flags&PGR_RO_FLAG_COMPLETION_READY));}
  else if(index==6||index==7){confirm();queue_both();usleep(110000);
    if(index==6)revoke_dismiss=true;else expire_dismiss=true;
    auto r=request(index==7?500:PGR_RO_MAX_LEASE_MS);PgrRoutesObserversOutput out{};api->poll(&r,&out);
    assert(dismiss_calls==1); // fresh generation+lease proof required before second managed UI call
    if(index==6){assert(!(out.flags&PGR_RO_FLAG_COMPLETION_READY));assert(!event(out,PGR_RO_EV_REWARD_CONFIRMED));
      assert(out.status!=PGR_RO_OK||out.generation==r.generation);}
  }else if(index==8){confirm();queue_both();usleep((PGR_RO_COMPLETION_MS+50)*1000);
    revoke_free=true;auto r=request();PgrRoutesObserversOutput out{};api->poll(&r,&out);
    assert(!(out.flags&PGR_RO_FLAG_COMPLETION_READY));
    assert(out.status!=PGR_RO_OK||out.generation==r.generation);
    assert(!event(out,PGR_RO_EV_REWARD_CONFIRMED));
  }else std::abort();
  std::printf("PASS Routes observer regression %s\n",names[index]);std::fflush(stdout);
  // Hooks/trampolines and rooted fixture storage intentionally live until process exit.
}
}
int main(int argc,char **argv){
  assert(argc==2);unsigned failed=0;
  // Isolated children ensure one draft assertion cannot hide independent regressions.
  for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++){
    pid_t child=fork();assert(child>=0);if(!child){scenario(argv[1],i);_exit(0);}
    int status=0;assert(waitpid(child,&status,0)==child);
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0){++failed;std::fprintf(stderr,"FAIL Routes observer regression %s status=%d\n",names[i],status);}
  }
  if(failed){std::fprintf(stderr,"%u Routes observer regressions failed\n",failed);return 1;}
  std::puts("PASS all9 independent actual-library/Dobby Routes observer regressions");return 0;
}

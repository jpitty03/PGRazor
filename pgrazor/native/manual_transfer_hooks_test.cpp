// Isolated production wrapper fixture. No game hooks, dialog presses or transfers.
#include "manual_transfer.cpp"
#include <cassert>
#include <thread>
#include <unordered_map>
namespace fixture {
struct Object {void *klass;bool favorite=false,egg=false;Object *fort=nullptr;int length=0;};
int pk_class=0;
std::atomic<unsigned> calls{0},roots{0};
std::atomic<bool> permission{true},fresh{true};
bool result=false,stop_inside=false,nested=false;
std::unordered_map<uintptr_t,Object*> handles;uintptr_t next_handle=uintptr_t(1)<<40;
void *current(){return &pk_class;}
uintptr_t gc_new(void *p,int pinned){assert(p&&pinned==1);handles[++next_handle]=static_cast<Object*>(p);++roots;return next_handle;}
void *gc_get(uintptr_t h){return handles[h];}
void gc_free(uintptr_t h){assert(h&&handles[h]);handles[h]=nullptr;--roots;}
void *get_class(void *p){return static_cast<Object*>(p)->klass;}
void get_field(void *p,const void *f,void *value){auto o=static_cast<Object*>(p);if(f==reinterpret_cast<void*>(1))*static_cast<uint8_t*>(value)=o->favorite;
 else if(f==reinterpret_cast<void*>(2))*static_cast<uint8_t*>(value)=o->egg;
 else {assert(f==reinterpret_cast<void*>(3));*static_cast<void**>(value)=o->fort;}}
int length(void *p){return static_cast<Object*>(p)->length;}
int sample(PgrRuntimeTransportSample *out){*out={};out->snapshot_fresh=fresh;out->snapshot_seq=7;out->generation=1;return 0;}
int get_bool(uint32_t field,int *out){assert(field==43);*out=permission?1:0;return 0;}
int emit(uint32_t,const int64_t*,uint32_t,const PgrRuntimeTransportString*,uint32_t){return 0;}
bool unlock(void*,void*,bool,const void*){++calls;if(stop_inside)pgr_manual_transfer_stop();return result;}
bool check(void*,void*,const void*){++calls;if(stop_inside)pgr_manual_transfer_stop();return result;}
bool selection_original(void*,void*,const void*){++calls;return result;}
bool nested_multi(void *self,void *proto,const void *info){++calls;return selection_replacement(proto,self,info);}
PgrRuntimeTransportApi transport{};
void setup(){
 owner=uint64_t(gettid());ready=true;paused=stopped=false;busy=false;error=false;
 boot.runtime.thread_current=reinterpret_cast<void*>(current);boot.runtime.gchandle_new=reinterpret_cast<void*>(gc_new);
 boot.runtime.gchandle_get_target=reinterpret_cast<void*>(gc_get);boot.runtime.gchandle_free=reinterpret_cast<void*>(gc_free);
 boot.runtime.object_get_class=reinterpret_cast<void*>(get_class);boot.runtime.field_get_value=reinterpret_cast<void*>(get_field);
 pokemon_class=&pk_class;favorite=reinterpret_cast<void*>(1);egg=reinterpret_cast<void*>(2);fort=reinterpret_cast<void*>(3);string_length=reinterpret_cast<void*>(length);
 transport.sample=sample;transport.get_bool=get_bool;transport.emit=emit;tr=&transport;
 original_unlock=unlock;original_multi=check;original_selection=selection_original;revoke();
}
}
extern "C" const void *pgrazor_runtime_bootstrap_api(uint32_t){return nullptr;}
extern "C" const void *pgrazor_runtime_metadata_api(uint32_t){return nullptr;}
extern "C" const void *pgrazor_runtime_transport_api(uint32_t){return &fixture::transport;}
int pgrazor_hook(void*,void*,void**){std::abort();}
int pgrazor_unhook(void*){std::abort();}
int pgrazor_instrument(void*,dobby_instrument_callback_t){std::abort();}
int main(){using namespace fixture;setup();Object self{&pk_class},pk{&pk_class},deployed{nullptr,false,false,nullptr,10};
 auto one=[&](auto call,bool expected){unsigned n=calls;assert(call()==expected);assert(calls==n+1);assert(roots==0);};
 result=false;one([&]{return unlock_replacement(&self,&pk,false,nullptr);},true);
 one([&]{return unlock_replacement(&self,&pk,true,nullptr);},false);
 pk.favorite=true;one([&]{return unlock_replacement(&self,&pk,false,nullptr);},false);pk.favorite=false;
 pk.egg=true;one([&]{return unlock_replacement(&self,&pk,false,nullptr);},false);pk.egg=false;
 pk.fort=&deployed;one([&]{return unlock_replacement(&self,&pk,false,nullptr);},false);pk.fort=nullptr;
 result=true;one([&]{return multi_replacement(&self,&pk,nullptr);},false);
 pk.favorite=true;one([&]{return multi_replacement(&self,&pk,nullptr);},true);pk.favorite=false;
 revoke();
 // Before any transfer click, a validated multi-select predicate establishes intent.
 one([&]{return selection_replacement(&pk,&self,nullptr);},true);
 one([&]{return multi_replacement(&self,&pk,nullptr);},false);
 assert(selection_window.recent(now(),30000));
 assert(!window.recent(now(),20000)&&!window.capture(now()));
 one([&]{return selection_replacement(&pk,&self,nullptr);},false);
 revoke();one([&]{return selection_replacement(&pk,&self,nullptr);},true);
 permission=false;one([&]{return multi_replacement(&self,&pk,nullptr);},true);permission=true;
 fresh=false;one([&]{return multi_replacement(&self,&pk,nullptr);},true);fresh=true;
 result=false;std::thread worker([&]{one([&]{return unlock_replacement(&self,&pk,false,nullptr);},false);});worker.join();
 stop_inside=true;one([&]{return unlock_replacement(&self,&pk,false,nullptr);},false);stop_inside=false;
 one([&]{return unlock_replacement(&self,&pk,false,nullptr);},false);
 assert(!window.pending);
 setup();warning_ready=false;type_object=nullptr;assert(!prepare_warning());assert(ready);
 result=false;one([&]{return unlock_replacement(&self,&pk,false,nullptr);},true);
 assert(next_handle>(uintptr_t(1)<<40)&&roots==0);
 revoke();result=true;original_multi=nested_multi;unsigned before=calls;
 assert(!multi_replacement(&self,&pk,nullptr));assert(calls==before+2&&roots==0&&!busy&&!selection_busy);std::puts("PASS production manual wrappers: original once, gates, favorites/eggs/gym, explicit intent, stop in original and balanced roots");
}

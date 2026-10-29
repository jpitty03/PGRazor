#include "vgp_marker.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

namespace {
PgrVgpMarkerConfig cfg{};
std::mutex installMutex;
std::atomic<bool> installed{false},busy{false};
std::atomic<uint64_t> ownerTid{0};
using New=uintptr_t(*)(void*,int);
using Get=void*(*)(uintptr_t);
using Free=void(*)(uintptr_t);
using Class=void*(*)(void*);
using Invoke=void*(*)(void*,void*,void**,void**);
using Assign=bool(*)(void*,void*);
uint64_t now(){timespec t{};if(clock_gettime(CLOCK_BOOTTIME,&t)||t.tv_sec<0)return 0;return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
struct Root {
 uintptr_t h=0;
 Root()=default;Root(const Root&)=delete;Root&operator=(const Root&)=delete;
 ~Root(){if(h)reinterpret_cast<Free>(cfg.gc_free)(h);}
 bool set(void *o){if(!o)return false;h=reinterpret_cast<New>(cfg.gc_new)(o,0);return h!=0;}
 void *get()const{return h?reinterpret_cast<Get>(cfg.gc_get)(h):nullptr;}
};
bool exact(const Root &r,void *klass){void *o=r.get();return o&&reinterpret_cast<Class>(cfg.object_class)(o)==klass;}
bool is(const Root &r,void *klass){void *o=r.get();return o&&reinterpret_cast<Assign>(cfg.assignable)(klass,reinterpret_cast<Class>(cfg.object_class)(o));}
// Root a returned reference immediately, before inspecting or issuing another managed call.
bool call(void *method,Root *receiver,void **args,Root &out){
 void *exception=nullptr;void *o=receiver?receiver->get():nullptr;
 if(receiver&&!o)return false;
 void *value=reinterpret_cast<Invoke>(cfg.invoke)(method,o,args,&exception);
 return !exception&&out.set(value);
}
void *unbox(const Root &r){void *o=r.get();return o?reinterpret_cast<Class>(cfg.unbox)(o):nullptr;}
bool live(Root &r){
 void *o=r.get();if(!o)return false;void *args[]={o};Root result;
 if(!call(cfg.alive,nullptr,args,result)||!exact(result,cfg.bool_class))return false;
 void *v=unbox(result);return v&&*static_cast<uint8_t*>(v)!=0;
}
struct Vector {float x,y,z;};
bool vector(const Root &r,Vector &v){if(!exact(r,cfg.vector_class))return false;void *p=unbox(r);if(!p)return false;memcpy(&v,p,sizeof(v));return true;}
bool integer(const Root &r,int32_t &v){if(!exact(r,cfg.int_class))return false;void *p=unbox(r);if(!p)return false;memcpy(&v,p,sizeof(v));return true;}
int install(const PgrVgpMarkerConfig *c){
 if(!c||c->version!=1||c->size!=sizeof(*c))return -1;
 void *pointers[21];memcpy(pointers,reinterpret_cast<const char*>(c)+8,sizeof(pointers));
 for(void *p:pointers)if(!p)return -1;
 std::lock_guard<std::mutex> guard(installMutex);
 if(installed.load())return memcmp(c,&cfg,sizeof(cfg))==0?0:-2;
 cfg=*c;installed.store(true,std::memory_order_release);return 0;
}
int project(void *target,uint64_t lease,uint64_t owner,PgrVgpMarkerResult *out){
 if(!out)return -1;
 *out={1,sizeof(*out),0,0,0,0};
 if(!installed.load(std::memory_order_acquire))return -2;
 if(!target||!lease||!owner||owner!=static_cast<uint64_t>(gettid()))return 0;
 uint64_t expected=0;ownerTid.compare_exchange_strong(expected,owner);
 if(ownerTid.load()!=owner)return 0;
 const uint64_t start=now();if(!start)return -3;
 const uint64_t deadline=start+(lease>2500?2500:lease);
 bool idle=false;if(!busy.compare_exchange_strong(idle,true))return 0;
 // Clear busy after roots destruct, including cleanup reentry.
 struct BusyExit{~BusyExit(){busy.store(false);}} exit;
 Root object,camera,transform,position,screen,width,height;
 if(!object.set(target))return -4;
 if(!(is(object,cfg.wild_class)||is(object,cfg.stop_class))||!live(object))return 0;
 if(!call(cfg.camera,nullptr,nullptr,camera)||!is(camera,cfg.camera_class)||!live(camera))return 0;
 if(!call(cfg.transform,&object,nullptr,transform)||!is(transform,cfg.transform_class)||!live(transform))return 0;
 Vector point{},projected{};
 if(!call(cfg.position,&transform,nullptr,position)||!vector(position,point))return -5;
 if(!std::isfinite(point.x)||!std::isfinite(point.y)||!std::isfinite(point.z))return 0;
 void *args[]={&point};
 if(!call(cfg.project,&camera,args,screen)||!vector(screen,projected))return -5;
 int32_t w=0,h=0;
 if(!call(cfg.width,nullptr,nullptr,width)||!integer(width,w)||!call(cfg.height,nullptr,nullptr,height)||!integer(height,h))return -5;
 if(w<=0||h<=0||!std::isfinite(projected.x)||!std::isfinite(projected.y)||!std::isfinite(projected.z)||projected.z<=0)return 0;
 if(!live(object)||!live(camera))return 0;
 const uint64_t end=now();if(!end||end>=deadline)return 0;
 out->x=double(projected.x)/w;out->y=1-double(projected.y)/h;
 out->visible=out->x>=0&&out->x<=1&&out->y>=0&&out->y<=1;
 return 1;
}
const PgrVgpMarkerApi api={1,sizeof(PgrVgpMarkerApi),install,project};
}
extern "C" const void *pgrazor_vgp_marker_api(uint32_t version){return version==1?&api:nullptr;}

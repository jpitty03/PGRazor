#include "max_orbs.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <time.h>

namespace {
struct Failure {};
void require(bool ok){if(!ok)throw Failure{};}
PgrBootBinding boot{};
bool ready=false,reported=false;
uint64_t next_prepare=0;
void report(const char *json){
 auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));
 if(tr&&tr->emit){PgrRuntimeTransportString args[]={{"max-orbs",8},{json,uint32_t(std::strlen(json))}};tr->emit(19,nullptr,0,args,2);}
}
void unavailable(){if(!reported){reported=true;report("{\"phase\":\"unavailable\",\"message\":\"Max orb position unavailable\"}");}}
template<class F>F fn(const void *p){require(p);return reinterpret_cast<F>(const_cast<void*>(p));}
void *exp(const char *name){auto p=dlsym(boot.handle,name);require(p);return p;}
struct Root {
 uintptr_t h=0;
 explicit Root(void *o){if(o){h=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(o,0);require(h);}}
 Root(const Root&)=delete;
 ~Root(){if(h)fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);}
 void *get()const{return h?fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h):nullptr;}
};
void *klass(void *o){require(o);return fn<void*(*)(void*)>(boot.runtime.object_get_class)(o);}
void *cls(const char *ns,const char *name){
 auto api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api);
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult r{};require(api->resolve_class(&q,&r)==0&&r.klass);return const_cast<void*>(r.klass);
}
void *method(void *k,const char *name,int n){auto m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,n);require(m);return m;}
void *field(void *k,const char *name){auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(k,name);require(f);return f;}
template<class T>T read(void *o,void *f){require(o&&f);T v{};fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;}
template<class T>T at(void *o,size_t offset){require(o);T v{};std::memcpy(&v,static_cast<char*>(o)+offset,sizeof(v));return v;}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;auto v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex);return v;}
template<class T>T unbox(void *o){require(o);auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p);T v{};std::memcpy(&v,p,sizeof(v));return v;}
void *return_class(void *m){return fn<void*(*)(void*)>(exp("il2cpp_class_from_type"))(fn<void*(*)(void*)>(exp("il2cpp_method_get_return_type"))(m));}
int literal(void *k,const char *name){auto f=field(k,name);require((fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x8050)==0x8050);int v=0;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(f,&v);return v;}

struct Position {int32_t x,y;};
struct Binding {
 void *state_class=nullptr,*field_class=nullptr,*local=nullptr,*orb_type=nullptr,*orb_state=nullptr,*prediction=nullptr;
 void *mode=nullptr,*fields=nullptr,*position=nullptr;
 int normal=0,collectible=0,idle=0,left=0,right=0;
} b;
void prepare(void *dodge){
 if(ready)return;
 auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(api&&api->snapshot(&boot)==0);
 Binding next;
 next.state_class=cls("Niantic.Holoholo.VNext","BattleState");
 next.field_class=cls("Niantic.Holoholo.VNext","BattleFieldActorState");
 next.local=method(next.state_class,"get_LocalActor",0);
 next.mode=field(next.state_class,"<UIMode>k__BackingField");
 next.fields=field(next.state_class,"FieldActors");
 next.normal=literal(return_class(method(next.state_class,"get_UIMode",0)),"Default");
 void *character=cls("Niantic.Holoholo.VNext","BattleCharacterState");
 next.position=field(character,"Position");
 // Position is an inline pair of int32s, never an object reference.
 void *position_class=cls("Niantic.Holoholo.VNext","BattlePosition");uint32_t align=0;
 require(fn<int32_t(*)(void*,uint32_t*)>(exp("il2cpp_class_value_size"))(position_class,&align)==sizeof(Position));
 next.prediction=method(character,"get_HasPredictedPosition",0);
 next.orb_type=method(next.field_class,"get_Type",0);
 next.collectible=literal(return_class(next.orb_type),"CollectibleOrb");
 next.orb_state=method(cls("Niantic.Holoholo.VNext","BattleCharacterStateExt"),"GetOrbAnimationState",1);
 next.idle=literal(return_class(next.orb_state),"OrbStateIdle");
 void *direction_type=fn<void*(*)(void*,uint32_t)>(exp("il2cpp_method_get_param"))(dodge,0);
 void *direction_class=fn<void*(*)(void*)>(exp("il2cpp_class_from_type"))(direction_type);
 next.left=literal(direction_class,"Left");next.right=literal(direction_class,"Right");
 require(next.left&&next.right&&next.left!=next.right);
 b=next;ready=true;
}

// No references survive this call. The caller arbitrates this with attacks,
// manual menus, scope changes, STOP and the live policy lease before moving.
int probe(void *state){
 Root s(state);if(!s.get()||klass(s.get())!=b.state_class||read<int32_t>(s.get(),b.mode)!=b.normal)return 0;
 Root actor(invoke(b.local,s.get()));if(!actor.get())return 0;
 if(unbox<bool>(invoke(b.prediction,actor.get())))return 0;
 const Position player=read<Position>(actor.get(),b.position);
 Root dict(read<void*>(s.get(),b.fields));if(!dict.get())return 0;
 void *dk=klass(dict.get());
 const int count=read<int32_t>(dict.get(),field(dk,"_count"));
 Root entries(read<void*>(dict.get(),field(dk,"_entries")));if(!entries.get())return 0;
 const uint64_t length=at<uint64_t>(entries.get(),0x18);
 require(count>=0&&count<=128&&uint64_t(count)<=length);
 // Dictionary<string,BattleFieldActorState>.Entry: hash,next,key,value (24 B).
 for(int i=0;i<count;++i){
  const size_t offset=0x20+size_t(i)*24;
  if(at<int32_t>(entries.get(),offset)<0||at<int32_t>(entries.get(),offset+4)<-1)continue;
  Root orb(at<void*>(entries.get(),offset+16));if(!orb.get()||klass(orb.get())!=b.field_class)continue;
  if(unbox<int32_t>(invoke(b.orb_type,orb.get()))!=b.collectible)continue;
  void *args[]={orb.get()};
  if(unbox<int32_t>(invoke(b.orb_state,nullptr,args))!=b.idle)continue;
  const Position target=read<Position>(orb.get(),b.position);
  // Match BreadOrb's tap handler: it compares X, not Y, and requires a
  // one-position difference before submitting the normal directional dodge.
  const int64_t delta=int64_t(target.x)-player.x;
  if(delta==-1)return b.left;
  if(delta==1)return b.right;
 }
 return 0;
}
}

bool pgr_max_orbs_prepare(void *dodge_method){
 if(ready)return true;
 timespec t{};if(clock_gettime(CLOCK_BOOTTIME,&t)!=0)return false;
 const uint64_t now=uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;if(now<next_prepare)return false;next_prepare=now+5000;
 try{prepare(dodge_method);reported=false;report("{\"phase\":\"ready\",\"message\":\"Automatic Max orb movement ready\"}");return true;}
 catch(...){unavailable();return false;}
}
int pgr_max_orb_direction(void *state,void *dodge_method){
 if(!pgr_max_orbs_prepare(dodge_method))return 0;
 try{return probe(state);}catch(...){unavailable();return 0;}
}

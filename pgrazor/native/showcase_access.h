#pragma once
#include "showcase_bindings.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "shared_metadata.h"
#include <dlfcn.h>
#include <cstring>
#include <string>
#include <initializer_list>

namespace pgr_showcase {
struct AccessFailure {const char* text;};
inline void need(bool ok,const char* text){if(!ok)throw AccessFailure{text};}
// Short-lived managed access context. It never owns a mutex or extends its
// absolute lease. The optional permission callback is native-only.
struct Access {
 ScoreRequest lease;ScoreBindings b{};PgrBootBinding boot{};const PgrMetaApi* md=nullptr;
 bool (*permitted)(void*)=nullptr;void* permission=nullptr;
 Access(const ScoreRequest& l,bool(*check)(void*)=nullptr,void* data=nullptr):lease(l),permitted(check),permission(data){
  need(resolve_live_score_bindings(lease,b),"Showcase bindings unavailable");
  auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));
  need(api&&api->snapshot(&boot)==0,"Showcase runtime unavailable");
  md=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));need(md,"Showcase metadata unavailable");gate();
 }
 void gate()const {const auto now=b.boottime_ms();need(now&&now<lease.deadline_ms&&lease.deadline_ms-now<=2500&&
   b.owner_valid(lease.owner,lease.generation)&&(!permitted||permitted(permission)),"Showcase session or lease changed");}
 template<class F>F fn(void* p)const {need(p,"Missing Showcase runtime export");return reinterpret_cast<F>(p);}
 template<class F>F symbol(const char* name)const {gate();return fn<F>(dlsym(boot.handle,name));}
 void* get(uintptr_t h)const {gate();void* p=h?b.target(h):nullptr;need(p,"Showcase object unavailable");return p;}
 void* klass(uintptr_t h)const{return b.object_class(get(h));}
 std::string type(void* t)const {
  gate();char* name=fn<char*(*)(void*)>(boot.meta.type_get_name)(t);need(name,"Missing managed type");
  std::string result;try{result=name;}catch(...){fn<void(*)(void*)>(boot.meta.mem_free)(name);throw;}
  fn<void(*)(void*)>(boot.meta.mem_free)(name);
  for(size_t at=0;(at=result.find("`1<",at))!=std::string::npos;)result.erase(at,2);
  return result;
 }
 void* cls(const char* ns,const char* name)const {
  gate();PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
  PgrMetaClassResult out{};need(md->resolve_class(&q,&out)==0,name);return const_cast<void*>(out.klass);
 }
 void exact(uintptr_t h,void* c)const{need(klass(h)==c,"Unexpected Showcase object class");}
 void* method(void* c,const char* name,const char* result,std::initializer_list<const char*> params={})const {
  gate();auto candidate=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(c,name,int(params.size()));need(candidate,name);
  auto declaring=fn<void*(*)(void*)>(boot.meta.method_get_class)(candidate);
  need(declaring&&fn<bool(*)(void*,void*)>(boot.meta.class_is_assignable_from)(declaring,c),"Showcase method owner mismatch");
  PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=declaring;q.name=name;q.return_type=result;q.param_count=unsigned(params.size());q.allow_inflated=1;
  unsigned i=0;for(auto p:params)q.param_types[i++]=p;
  PgrMetaMethodResult out{};need(md->validate_method(&q,&out)==0,name);return const_cast<void*>(out.method);
 }
 void* invoke(const void* m,uintptr_t self,void** args=nullptr)const {
  gate();void* receiver=self?get(self):nullptr;void* error=nullptr;void* result=b.invoke(m,receiver,args,&error);
  need(!error,"Showcase managed call failed");return result;
 }
 void* call(uintptr_t self,const char* name,const char* result,std::initializer_list<const char*> params={},void** args=nullptr)const {
  auto m=method(klass(self),name,result,params);return invoke(m,self,args);
 }
 void* field(uintptr_t h,const char* name,const char* expected)const {
  gate();auto f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(h),name);need(f,name);
  need(!(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x50),"Showcase field is not an instance field");
  need(type(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))==expected,name);return f;
 }
 template<class T>T value(uintptr_t h,const char* name,const char* expected)const {
  auto f=field(h,name,expected);T out{};b.field_get(get(h),f,&out);return out;
 }
 bool boolean(uintptr_t h,const char* name)const {auto v=value<uint8_t>(h,name,"System.Boolean");need(v<=1,"Invalid Showcase Boolean");return v==1;}
 std::string text(uintptr_t h)const {
  exact(h,cls("System","String"));int length=symbol<int(*)(void*)>("il2cpp_string_length")(get(h));
  need(length>0&&length<=256,"Invalid Showcase account identity");
  auto p=fn<const uint16_t*(*)(void*)>(boot.runtime.string_chars)(get(h));need(p,"Missing Showcase identity text");
  std::string out;for(int i=0;i<length;++i){need(p[i]>=32&&p[i]<127,"Unsupported Showcase account identity");out+=char(p[i]);}return out;
 }
};
struct AccessRoot {
 const Access* a=nullptr;uintptr_t h=0;
 AccessRoot()=default;
 AccessRoot(const Access& ctx,void* p):a(&ctx){if(p){ctx.gate();h=ctx.b.root(p,0);need(h,"Showcase GC root failed");}}
 AccessRoot(const AccessRoot&)=delete;AccessRoot& operator=(const AccessRoot&)=delete;
 AccessRoot(AccessRoot&& r)noexcept:a(r.a),h(r.h){r.h=0;}
 AccessRoot& operator=(AccessRoot&& r)noexcept{if(this!=&r){reset();a=r.a;h=r.h;r.h=0;}return *this;}
 ~AccessRoot(){reset();}
 void reset(){if(h){a->b.release(h);h=0;}}
 void* get()const{return h?a->get(h):nullptr;}
 explicit operator bool()const{return get()!=nullptr;}
 uintptr_t release(){auto value=h;h=0;return value;}
};
inline AccessRoot reference(const Access& a,uintptr_t object,const char* name,const char* type){
 return AccessRoot(a,a.value<void*>(object,name,type));
}
template<class T>T boxed(const Access& a,void* object,const char* ns,const char* name){
 AccessRoot root(a,object);a.exact(root.h,a.cls(ns,name));T value{};void* p=a.b.unbox(root.get());need(p,"Invalid Showcase boxed value");std::memcpy(&value,p,sizeof(value));return value;
}
inline AccessRoot new_instance(const Access& a,void* klass){
 a.gate();AccessRoot root(a,a.fn<void*(*)(void*)>(a.boot.runtime.object_new)(klass));need(bool(root),"Showcase allocation failed");
 a.call(root.h,".ctor","System.Void");return root;
}
inline AccessRoot resolve_service(const Access& a,const PgrSharedMetadataOutput& metadata,const char* ns,const char* name){
 using namespace pgr_shared_meta;
 auto h=[&](unsigned id){auto p=metadata.opt_handles[id];need(p,"Showcase service resolver unavailable");return p;};
 auto object_type=a.symbol<void*(*)(void*)>("il2cpp_type_get_object");
 auto class_type=a.fn<void*(*)(void*)>(a.boot.meta.class_get_type);
 a.gate();AccessRoot map_type(a,object_type(class_type(const_cast<void*>(h(H_IL_MAP)))));
 void* args[]={map_type.get()};AccessRoot map(a,a.invoke(h(H_IL_FIND_M),0,args));need(bool(map),"Map services unavailable");
 AccessRoot container(a,a.invoke(h(H_IL_CONTAINER_M),map.h));need(bool(container),"Game services unavailable");
 auto contract=a.cls(ns,name);a.gate();AccessRoot wanted(a,object_type(class_type(contract)));
 void* resolve_args[]={wanted.get()};AccessRoot service(a,a.invoke(h(H_IL_RESOLVE_M),container.h,resolve_args));
 need(service&&a.fn<bool(*)(void*,void*)>(a.boot.meta.class_is_assignable_from)(contract,a.klass(service.h)),"Showcase service identity mismatch");
 return service;
}
}

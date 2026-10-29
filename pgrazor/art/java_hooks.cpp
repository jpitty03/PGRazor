#include <jni.h>
#include <lsplant.hpp>
#include <dobby.h>
#include <mutex>
#include <cstddef>
#include "art_symbols.h"
namespace {
bool initialized=false;
PgrArtSymbols symbols;
std::mutex hooks;
using CurrentThread = void *(*)();
using SuspendCtor = void (*)(void *, const char *, bool);
using ScopeDtor = void (*)(void *);
using GCCtor = void (*)(void *, void *, int, int);
CurrentThread current_thread = nullptr;
SuspendCtor suspend_ctor = nullptr;
ScopeDtor suspend_dtor = nullptr;
GCCtor gc_ctor = nullptr;
ScopeDtor gc_dtor = nullptr;
// Match the pinned LSPlant scope ABI: GC has three pointer-sized members;
// ScopedSuspendAll has no data members. Extra aligned storage is conservative.
struct PublicationScope {
 alignas(std::max_align_t) unsigned char gc[8 * sizeof(void *)]{};
 alignas(std::max_align_t) unsigned char suspend[8 * sizeof(void *)]{};
 bool active = false;
};
thread_local PublicationScope publication_scope;
bool load_publication_scopes() {
 current_thread = reinterpret_cast<CurrentThread>(symbols.find("_ZN3art6Thread14CurrentFromGdbEv"));
 suspend_ctor = reinterpret_cast<SuspendCtor>(symbols.find("_ZN3art16ScopedSuspendAllC2EPKcb"));
 suspend_dtor = reinterpret_cast<ScopeDtor>(symbols.find("_ZN3art16ScopedSuspendAllD2Ev"));
 gc_ctor = reinterpret_cast<GCCtor>(symbols.find("_ZN3art2gc23ScopedGCCriticalSectionC2EPNS_6ThreadENS0_7GcCauseENS0_13CollectorTypeE"));
 gc_dtor = reinterpret_cast<ScopeDtor>(symbols.find("_ZN3art2gc23ScopedGCCriticalSectionD2Ev"));
 return current_thread && suspend_ctor && suspend_dtor && gc_ctor && gc_dtor;
}
jboolean ready(JNIEnv *,jclass){return initialized?JNI_TRUE:JNI_FALSE;}
jobject install(JNIEnv *env,jclass,jobject target,jobject context,jobject callback){
 if(!initialized||!target||!context||!callback)return nullptr;
 try{
  std::lock_guard<std::mutex> guard(hooks);
  if(lsplant::IsHooked(env,target))return nullptr;
  return lsplant::Hook(env,target,context,callback);
 }catch(...){
  if(!env->ExceptionCheck()){
   jclass cls=env->FindClass("java/lang/IllegalStateException");
   if(cls){env->ThrowNew(cls,"ART hook installation failed");env->DeleteLocalRef(cls);}
  }
  return nullptr;
 }
}
jboolean deopt(JNIEnv *env,jclass,jobject method){
 if(!initialized||!method)return JNI_FALSE;
 try{
  std::lock_guard<std::mutex> guard(hooks);
  return lsplant::Deoptimize(env,method)?JNI_TRUE:JNI_FALSE;
 }catch(...){return JNI_FALSE;}
}
JNINativeMethod methods[]={
 {const_cast<char *>("ready"),const_cast<char *>("()Z"),reinterpret_cast<void *>(ready)},
 {const_cast<char *>("hook"),const_cast<char *>("(Ljava/lang/reflect/Method;Ljava/lang/Object;Ljava/lang/reflect/Method;)Ljava/lang/reflect/Method;"),reinterpret_cast<void *>(install)},
 {const_cast<char *>("deoptimize"),const_cast<char *>("(Ljava/lang/reflect/Method;)Z"),reinterpret_cast<void *>(deopt)}
};
}
// Called by the generated, build-local LSPlant HookHandler guard. Its scope
// covers native code activation AND LSPlant's backup-pointer assignment. Do not
// wrap InitJNI/InitNative wholesale: JNI/class creation must run unsuspended.
extern "C" __attribute__((visibility("hidden"))) bool pgrazor_art_publication_begin() noexcept {
 if (!current_thread || !suspend_ctor || !suspend_dtor || !gc_ctor || !gc_dtor ||
     publication_scope.active) return false;
 void *self = current_thread();
 if (!self) return false;
 // Same order and enum values as pinned LSPlant DoHook: debugger GC exclusion,
 // then suspend all mutators. JNI_OnLoad executes with the calling thread native.
 gc_ctor(publication_scope.gc, self, 10 /* kGcCauseDebugger */, 9 /* kCollectorTypeDebugger */);
 suspend_ctor(publication_scope.suspend, "PGRazor native hook publication", false);
 publication_scope.active = true;
 return true;
}
extern "C" __attribute__((visibility("hidden"))) void pgrazor_art_publication_end() noexcept {
 if (!publication_scope.active) return;
 suspend_dtor(publication_scope.suspend);
 gc_dtor(publication_scope.gc);
 publication_scope.active = false;
}
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm,void *){
 JNIEnv *env=nullptr;if(vm->GetEnv(reinterpret_cast<void **>(&env),JNI_VERSION_1_6)!=JNI_OK)return JNI_ERR;
 try{
  if(symbols.load() && load_publication_scopes()){
   lsplant::InitInfo info;
   info.inline_hooker=[](void *target,void *hooker)->void *{void *original=nullptr;return DobbyHook(target,hooker,&original)==0?original:nullptr;};
   info.inline_unhooker=[](void *target){return DobbyDestroy(target)==0;};
   info.art_symbol_resolver=[](std::string_view name){return symbols.find(name);};
   info.art_symbol_prefix_resolver=[](std::string_view prefix){return symbols.find(prefix,true);};
   info.generated_class_name="PgrMethod_";info.generated_source_name="";
   initialized=lsplant::Init(env,info);
  }
 }catch(...){initialized=false;}
 if(env->ExceptionCheck())return JNI_ERR;
 jclass cls=env->FindClass("dev/pgrazor/NativeJavaHooks");if(!cls)return JNI_ERR;
 const int status=env->RegisterNatives(cls,methods,sizeof(methods)/sizeof(methods[0]));env->DeleteLocalRef(cls);
 return status==0?JNI_VERSION_1_6:JNI_ERR;
}

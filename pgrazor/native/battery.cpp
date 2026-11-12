#include "battery.h"
#include "game_bindings.h"
#include "hook_engine.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include <algorithm>
#include <atomic>
#include <vector>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unordered_map>
#include <time.h>
#include <unistd.h>
// Battery usage (see battery.h). Unity's public graphics API (static getters and setters on Application,
// QualitySettings, RenderSettings and GraphicsSettings, and the active URP asset's properties) is set from
// the Unity owner thread in the shared tick. Low quality also skips three of the game's own render
// steps through hooks that are inert (call straight through) while the setting is off.
namespace {
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
PgrBootBinding boot{};
bool initialized=false,busy=false,faulted=false,hooks_tried=false;uint64_t owner=0,next_apply=0;
std::string hook_note;
constexpr uint64_t kApplyEveryMs=2000;
constexpr float kRenderScale=0.5f,kLodBias=0.5f;
constexpr int kFrameRate=20;
// While PGRazor's black screen covers the game nothing it draws is seen, so it draws at a quarter of the
// resolution. The frame rate is left to the user's settings: the black screen is for an unattended Shundo
// Sniper, and a lower one was not shown to leave its map searches unaffected.
constexpr float kDarkRenderScale=0.25f;
uint64_t clock_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
template<class F>F fn(const void *p){require(p,"missing runtime export");return reinterpret_cast<F>(const_cast<void*>(p));}
void *klass(void *o){require(o,"null object");return fn<void*(*)(void*)>(boot.runtime.object_get_class)(o);}
const PgrMetaApi *metadata(){const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api&&api->resolve_class&&api->validate_method,"metadata API");return api;}
void *cls(const char *ns,const char *name){
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult out{};require(metadata()->resolve_class(&q,&out)==0,name);return const_cast<void*>(out.klass);
}
void *method(void *k,const char *name,int count){
 void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(k,name,count);require(m,name);
 require(fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(m)==unsigned(count),name);return m;
}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
template<class T>T unbox(void *o){require(o,"null result");T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");std::memcpy(&v,p,sizeof(v));return v;}
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
void event(const std::string &message){
 auto tr=transport();if(!tr)return;
 PgrRuntimeTransportString s[]={{"battery",7},{message.c_str(),uint32_t(message.size())}};tr->emit(19,nullptr,0,s,2);
}

struct Config {bool res=false,fps=false,quality=false,dark=false;};
// "B2|r|f|q|d"; anything malformed or stale means everything off (the game's values come back).
Config configured(){
 Config c;auto tr=transport();PgrRuntimeTransportSample a{};char s[16]{};uint32_t n=0;
 if(!tr||tr->sample(&a)||!a.snapshot_fresh||a.fault||tr->get_string(20,s,sizeof(s)-1,&n)||n!=10)return c;
 if(std::memcmp(s,"B2|",3)||s[4]!='|'||s[6]!='|'||s[8]!='|')return c;
 for(int i:{3,5,7,9})if(s[i]!='0'&&s[i]!='1')return c;
 c.res=s[3]=='1';c.fps=s[5]=='1';c.quality=s[7]=='1';c.dark=s[9]=='1';return c;
}

// ---- The game's own render steps, skipped while low quality is on ----------------------------------
// Each replacement only reads an atomic flag and otherwise calls the original, so a hook left installed
// with the setting off behaves exactly like the game.
enum Step {S_SHADOWS,S_WATER,S_GROUND,S_COUNT};
const char *const kStepNames[S_COUNT]={"shadows","water reflections","encounter ground cover"};
std::atomic<bool> skip{false};
std::atomic<void*> original[S_COUNT]{};
std::atomic<bool> hooked[S_COUNT]{};
// AngledGradientShadowScriptableRenderFeature.AddRenderPasses(renderer, ref renderingData): the pass that
// draws the trainer's and Pokemon's long ground shadows. Not adding it removes the whole pass.
void hook_shadows(void *self,void *renderer,void *data,const void *m){
 if(skip.load(std::memory_order_relaxed))return;
 reinterpret_cast<void(*)(void*,void*,void*,const void*)>(original[S_SHADOWS].load(std::memory_order_acquire))(self,renderer,data,m);
}
// Vista.WaterReflection.UpdateRenderTexture(): re-renders the scene into the water's reflection texture.
void hook_water(void *self,const void *m){
 if(skip.load(std::memory_order_relaxed))return;
 reinterpret_cast<void(*)(void*,const void*)>(original[S_WATER].load(std::memory_order_acquire))(self,m);
}
// Encounter.DenseGroundCoverRenderer.RenderDenseGroundCover(): the instanced grass in encounters.
void hook_ground(void *self,const void *m){
 if(skip.load(std::memory_order_relaxed))return;
 reinterpret_cast<void(*)(void*,const void*)>(original[S_GROUND].load(std::memory_order_acquire))(self,m);
}
struct Target {Step step;const char *ns,*owner,*name;unsigned params;uint64_t rva;const uint8_t *prologue;void *replacement;};
// Each hook is validated (owner class, name, void return, arity, address and 24-byte prologue) before it
// is installed; one that does not match is left out and named in the log, the rest still work.
void install_hooks(){
 hooks_tried=true;
 static const uint8_t p_shadows[]=PGR_GAME_BATTERY_CPP_SHADOW_PASS_FINGERPRINT,p_water[]=PGR_GAME_BATTERY_CPP_WATER_REFLECTION_FINGERPRINT,
  p_ground[]=PGR_GAME_BATTERY_CPP_GROUND_COVER_FINGERPRINT;
 static_assert(sizeof(p_shadows)==24&&sizeof(p_water)==24&&sizeof(p_ground)==24,"24-byte prologues");
 const Target targets[]={
  {S_SHADOWS,"Niantic.Holoholo.Rendering","AngledGradientShadowScriptableRenderFeature","AddRenderPasses",2,PGR_GAME_BATTERY_CPP_SHADOW_PASS_RVA,p_shadows,reinterpret_cast<void*>(hook_shadows)},
  {S_WATER,"Niantic.Holoholo.Vista","WaterReflection","UpdateRenderTexture",0,PGR_GAME_BATTERY_CPP_WATER_REFLECTION_RVA,p_water,reinterpret_cast<void*>(hook_water)},
  {S_GROUND,"Niantic.Holoholo.Encounter","DenseGroundCoverRenderer","RenderDenseGroundCover",0,PGR_GAME_BATTERY_CPP_GROUND_COVER_RVA,p_ground,reinterpret_cast<void*>(hook_ground)},
 };
 std::string missing;
 for(const Target &t:targets){
  try{
   require(boot.module_base,"il2cpp module");
   PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=cls(t.ns,t.owner);q.name=t.name;
   q.return_type="System.Void";q.return_match=PGR_MD_MATCH_EXACT;q.param_count=t.params;
   q.expected_code=static_cast<const char*>(boot.module_base)+t.rva;q.prologue=t.prologue;q.prologue_len=24;
   PgrMetaMethodResult r{};const int rc=metadata()->validate_method(&q,&r);
   if(rc!=0||!r.code){missing+=std::string(missing.empty()?"":", ")+kStepNames[t.step]+" (code "+std::to_string(rc)+")";continue;}
   void *o=nullptr;
   if(pgrazor_hook(const_cast<void*>(r.code),t.replacement,&o)!=0||!o){missing+=std::string(missing.empty()?"":", ")+kStepNames[t.step]+" (hook)";continue;}
   original[t.step].store(o,std::memory_order_release);hooked[t.step].store(true,std::memory_order_release);
  }catch(const Failure &f){missing+=std::string(missing.empty()?"":", ")+kStepNames[t.step]+" ("+f.where+")";}
 }
 hook_note=missing.empty()?"":" (not skipped: "+missing+")";
}

// ---- Unity settings ------------------------------------------------------------------------------
// Resolved once per process.
void *app_get_fps=nullptr,*app_set_fps=nullptr,*quality_get_lod=nullptr,*quality_set_lod=nullptr,*graphics_pipeline=nullptr,
     *fog_get=nullptr,*fog_set=nullptr;
void *urp_class=nullptr,*urp_get_scale=nullptr,*urp_set_scale=nullptr,*urp_get_msaa=nullptr,*urp_set_msaa=nullptr,
     *urp_get_main=nullptr,*urp_set_main=nullptr,*urp_get_extra=nullptr,*urp_set_extra=nullptr,
     *urp_get_distance=nullptr,*urp_set_distance=nullptr,*urp_get_hdr=nullptr,*urp_set_hdr=nullptr,
     *urp_get_opaque=nullptr,*urp_set_opaque=nullptr;
void bind(){
 void *app=cls("UnityEngine","Application"),*quality=cls("UnityEngine","QualitySettings"),*graphics=cls("UnityEngine.Rendering","GraphicsSettings"),
      *render=cls("UnityEngine","RenderSettings");
 app_get_fps=method(app,"get_targetFrameRate",0);app_set_fps=method(app,"set_targetFrameRate",1);
 quality_get_lod=method(quality,"get_lodBias",0);quality_set_lod=method(quality,"set_lodBias",1);
 graphics_pipeline=method(graphics,"get_currentRenderPipeline",0);
 fog_get=method(render,"get_fog",0);fog_set=method(render,"set_fog",1);
 urp_class=cls("UnityEngine.Rendering.Universal","UniversalRenderPipelineAsset");
 urp_get_scale=method(urp_class,"get_renderScale",0);urp_set_scale=method(urp_class,"set_renderScale",1);
 urp_get_msaa=method(urp_class,"get_msaaSampleCount",0);urp_set_msaa=method(urp_class,"set_msaaSampleCount",1);
 urp_get_main=method(urp_class,"get_supportsMainLightShadows",0);urp_set_main=method(urp_class,"set_supportsMainLightShadows",1);
 urp_get_extra=method(urp_class,"get_supportsAdditionalLightShadows",0);urp_set_extra=method(urp_class,"set_supportsAdditionalLightShadows",1);
 urp_get_distance=method(urp_class,"get_shadowDistance",0);urp_set_distance=method(urp_class,"set_shadowDistance",1);
 urp_get_hdr=method(urp_class,"get_supportsHDR",0);urp_set_hdr=method(urp_class,"set_supportsHDR",1);
 urp_get_opaque=method(urp_class,"get_supportsCameraOpaqueTexture",0);urp_set_opaque=method(urp_class,"set_supportsCameraOpaqueTexture",1);
}
// A GC handle, so a managed object survives the allocating calls (boxed getter results) around it.
struct Root {
 uintptr_t h=0;
 explicit Root(void *v){if(v){h=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(v,0);require(h,"GC root");}}
 Root(const Root&)=delete;Root& operator=(const Root&)=delete;
 ~Root(){if(h)fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);}
 void *get()const{return h?fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h):nullptr;}
};
// The active URP asset, or null when the game is not using one right now.
void *pipeline(){void *p=invoke(graphics_pipeline,nullptr);return p&&klass(p)==urp_class?p:nullptr;}

// One managed value PGRazor may lower: the game's value is kept in `original` while ours is applied.
template<class T>struct Knob {
 bool applied=false;T original{},ours{};
 // Lowers it if needed, keeping the game's value. If the game has changed it since, that becomes the
 // value to put back. Returns true when something was set.
 template<class Get,class Set,class Lower>bool on(Get get,Set set,Lower lower){
  const T now=get();
  if(applied&&now==ours){  // still ours: follow a change in how low it should be (e.g. black screen)
   const T want=lower(original);if(want==ours)return false;
   ours=want;set(want);return true;
  }
  const T want=lower(now);
  if(want==now){if(applied){applied=false;}return false;}  // already as low as ours: nothing to change
  original=now;ours=want;set(want);applied=true;return true;
 }
 template<class Set>bool off(Set set){if(!applied)return false;applied=false;set(original);return true;}
};
Knob<int> fps,msaa;Knob<float> scale,lod,shadow_distance;Knob<bool> main_shadows,extra_shadows,fog,hdr,opaque;
bool held(){return fps.applied||msaa.applied||scale.applied||lod.applied||shadow_distance.applied||main_shadows.applied||
 extra_shadows.applied||fog.applied||hdr.applied||opaque.applied||skip.load();}

int get_int(void *m,void *o=nullptr){return unbox<int>(invoke(m,o));}
float get_float(void *m,void *o=nullptr){return unbox<float>(invoke(m,o));}
bool get_bool(void *m,void *o=nullptr){return unbox<uint8_t>(invoke(m,o))!=0;}
void set_int(void *m,int v,void *o=nullptr){void *a[]={&v};invoke(m,o,a);}
void set_float(void *m,float v,void *o=nullptr){void *a[]={&v};invoke(m,o,a);}
void set_bool(void *m,bool v,void *o=nullptr){uint8_t b=v;void *a[]={&b};invoke(m,o,a);}
// A boolean the low setting turns off.
template<class Obj>bool lower_bool(Knob<bool> &k,bool on,void *get,void *set,Obj obj){
 if(on)return k.on([&]{return get_bool(get,obj());},[&](bool v){set_bool(set,v,obj());},[](bool){return false;});
 return k.off([&](bool v){set_bool(set,v,obj());});
}

// ---- Encounter plants ----------------------------------------------------------------------------
// An encounter's grass tufts, clovers and standing flowers are hundreds of separate small objects, and
// every rock, bush and tree has a flat blob shadow (PropDecalShadow) under it. While low quality is on,
// their renderers are switched off in each encounter, in three passes about 2 s apart while the scene
// fills in. The game reuses these objects from one encounter to the next, so every renderer switched off
// is remembered (pruned of destroyed ones at each new encounter) and switched back on when the setting
// is turned off or the module stops.
constexpr int kPlantPasses=3;
void *o_find=nullptr,*o_name=nullptr,*o_alive=nullptr,*r_get_enabled=nullptr,*r_set_enabled=nullptr,*renderer_class=nullptr,*mesh_renderer_class=nullptr,
     *decorator_class=nullptr,*type_object=nullptr,*array_length=nullptr,*string_length=nullptr;
// Renderer -> GC handle. il2cpp's collector does not move objects, so a held object's address is its identity.
uintptr_t encounter=0;int passes=0;std::unordered_map<void*,uintptr_t> hidden;
void *exp(const char *name){std::string n="il2cpp_";n+=name;void *p=dlsym(boot.handle,n.c_str());require(p,name);return p;}
void bind_plants(){
 void *obj=cls("UnityEngine","Object");o_find=method(obj,"FindObjectsOfType",1);o_name=method(obj,"get_name",0);o_alive=method(obj,"op_Implicit",1);
 renderer_class=cls("UnityEngine","Renderer");mesh_renderer_class=cls("UnityEngine","MeshRenderer");
 r_get_enabled=method(renderer_class,"get_enabled",0);r_set_enabled=method(renderer_class,"set_enabled",1);
 decorator_class=cls("Niantic.Holoholo.Encounter","EncounterDecorator");
 type_object=exp("type_get_object");array_length=exp("array_length");string_length=exp("string_length");
}
void *target(uintptr_t h){return fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h);}
// FindObjectsOfType(type): the active objects of that type, kept alive by the returned (rooted) array.
Root find_all(void *k){
 Root type(fn<void*(*)(void*)>(type_object)(fn<void*(*)(void*)>(boot.meta.class_get_type)(k)));
 void *a[]={type.get()};require(a[0],"type object");return Root(invoke(o_find,nullptr,a));
}
uintptr_t length(void *array){return array?fn<uintptr_t(*)(void*)>(array_length)(array):0;}
void *element(void *array,uintptr_t i){void *e=nullptr;std::memcpy(&e,static_cast<char*>(array)+0x20+i*sizeof(void*),sizeof(void*));return e;}
// Case-insensitive prefix match on a managed string.
bool starts(void *s,const char *prefix){
 if(!s)return false;
 const int n=fn<int(*)(void*)>(string_length)(s);const uint16_t *c=static_cast<const uint16_t*>(fn<void*(*)(void*)>(boot.runtime.string_chars)(s));
 for(int i=0;prefix[i];++i){if(i>=n)return false;uint16_t ch=c[i];if(ch>='A'&&ch<='Z')ch+=32;if(ch!=uint8_t(prefix[i]))return false;}
 return true;
}
bool plant(void *renderer){
 void *name=invoke(o_name,renderer);
 for(const char *p:{"grass","clover","flower","propdecalshadow"})if(starts(name,p))return true;
 return false;
}
int hide_pass(){
 Root all=find_all(renderer_class);int n=0;
 const uintptr_t count=length(all.get());
 for(uintptr_t i=0;i<count;++i){
  void *r=element(all.get(),i);
  if(!r||klass(r)!=mesh_renderer_class||!plant(r)||!get_bool(r_get_enabled,r))continue;
  if(!hidden.count(r)){const uintptr_t h=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(r,0);require(h,"GC root");hidden.emplace(r,h);}
  set_bool(r_set_enabled,false,r);++n;
 }
 return n;
}
// Lets the hidden renderers go, switching them back on first when `show`. One the game has destroyed
// since throws, and is skipped.
void release(bool show){
 for(const auto &[r,h]:hidden){
  if(show){uint8_t on=1;void *a[]={&on},*ex=nullptr;fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(r_set_enabled,r,a,&ex);}
  fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);
 }
 hidden.clear();
}
// Forgets the renderers the game has destroyed since (UnityEngine.Object's bool conversion is false).
void prune(){
 for(auto it=hidden.begin();it!=hidden.end();){void *a[]={it->first};
  if(unbox<uint8_t>(invoke(o_alive,nullptr,a))){++it;continue;}
  fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(it->second);it=hidden.erase(it);}
}
void drop_encounter(){if(encounter)fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(encounter);encounter=0;passes=0;}
void plants(bool on){
 if(!on){
  const size_t n=hidden.size();release(true);drop_encounter();
  if(n)event("encounter plants shown again ("+std::to_string(n)+")");
  return;
 }
 void *dec=nullptr;
 {Root all=find_all(decorator_class);if(length(all.get()))dec=element(all.get(),0);
  if(!dec){drop_encounter();return;}
  if(!encounter||target(encounter)!=dec){
   drop_encounter();prune();encounter=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(dec,0);require(encounter,"GC root");
  }}
 if(passes>=kPlantPasses)return;
 ++passes;const uint64_t t=clock_ms();const int n=hide_pass();
 if(n)event("encounter plants hidden: "+std::to_string(n)+" (pass "+std::to_string(passes)+", "+std::to_string(clock_ms()-t)+" ms)");
}

std::string last_logged;
void apply(const Config &c){
 if(c.quality&&!hooks_tried)install_hooks();
 const bool was_skipping=skip.exchange(c.quality);
 Root asset(pipeline());auto urp_obj=[&]{void *o=asset.get();require(o,"render pipeline asset");return o;};
 auto no_obj=[]()->void*{return nullptr;};
 const bool urp=asset.get()!=nullptr;
 bool changed=was_skipping!=c.quality;
 const float max_scale=c.dark?kDarkRenderScale:kRenderScale;
 if(c.fps)changed|=fps.on([]{return get_int(app_get_fps);},[](int v){set_int(app_set_fps,v);},[](int now){return now>0&&now<=kFrameRate?now:kFrameRate;});
 else changed|=fps.off([](int v){set_int(app_set_fps,v);});
 if(c.quality)changed|=lod.on([]{return get_float(quality_get_lod);},[](float v){set_float(quality_set_lod,v);},[](float now){return now>kLodBias?kLodBias:now;});
 else changed|=lod.off([](float v){set_float(quality_set_lod,v);});
 changed|=lower_bool(fog,c.quality,fog_get,fog_set,no_obj);
 if(urp){
  if(c.res||c.dark)changed|=scale.on([&]{return get_float(urp_get_scale,urp_obj());},[&](float v){set_float(urp_set_scale,v,urp_obj());},[&](float now){return now>max_scale?max_scale:now;});
  else changed|=scale.off([&](float v){set_float(urp_set_scale,v,urp_obj());});
  if(c.quality){
   changed|=msaa.on([&]{return get_int(urp_get_msaa,urp_obj());},[&](int v){set_int(urp_set_msaa,v,urp_obj());},[](int){return 1;});
   // A zero shadow distance stops URP drawing real-time shadows whatever the other shadow settings say.
   changed|=shadow_distance.on([&]{return get_float(urp_get_distance,urp_obj());},[&](float v){set_float(urp_set_distance,v,urp_obj());},[](float){return 0.f;});
  }else{
   changed|=msaa.off([&](int v){set_int(urp_set_msaa,v,urp_obj());});
   changed|=shadow_distance.off([&](float v){set_float(urp_set_distance,v,urp_obj());});
  }
  changed|=lower_bool(main_shadows,c.quality,urp_get_main,urp_set_main,urp_obj);
  changed|=lower_bool(extra_shadows,c.quality,urp_get_extra,urp_set_extra,urp_obj);
  // HDR off halves the colour buffers' size; the opaque texture is a full-screen copy of the scene.
  changed|=lower_bool(hdr,c.quality,urp_get_hdr,urp_set_hdr,urp_obj);
  changed|=lower_bool(opaque,c.quality,urp_get_opaque,urp_set_opaque,urp_obj);
 }
 plants(c.quality);
 if(!changed)return;
 char text[320];
 std::snprintf(text,sizeof(text),"fps %d, render scale %.2f, lod bias %.2f, msaa %d, shadows %s/%s, shadow distance %.0f, fog %s, hdr %s, opaque texture %s, game render steps %s%s",
  get_int(app_get_fps),urp?get_float(urp_get_scale,urp_obj()):-1.f,get_float(quality_get_lod),urp?get_int(urp_get_msaa,urp_obj()):-1,
  urp?(get_bool(urp_get_main,urp_obj())?"on":"off"):"?",urp?(get_bool(urp_get_extra,urp_obj())?"on":"off"):"?",urp?get_float(urp_get_distance,urp_obj()):-1.f,
  get_bool(fog_get)?"on":"off",urp?(get_bool(urp_get_hdr,urp_obj())?"on":"off"):"?",urp?(get_bool(urp_get_opaque,urp_obj())?"on":"off"):"?",
  c.quality?"skipped":"drawn",c.quality?hook_note.c_str():"");
 if(last_logged!=text){last_logged=text;event(text);}
}

}

void pgr_battery_tick(uint64_t tid,const PgrSharedMetadataOutput *){
 if(busy||faulted||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid))return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 const uint64_t now=clock_ms();if(now<next_apply)return;next_apply=now+kApplyEveryMs;
 const Config c=configured();
 if(!(c.res||c.fps||c.quality||c.dark)&&!held()&&hidden.empty()&&!encounter)return;
 try{
  if(!initialized){
   auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);
   require(api&&api->snapshot(&boot)==0,"runtime unavailable");bind();bind_plants();owner=tid;initialized=true;
  }
  require(fn<void*(*)()>(boot.runtime.thread_current)()!=nullptr,"owner thread not attached");
  apply(c);
 }catch(const Failure &f){faulted=true;skip.store(false);event(std::string("Battery usage stopped: ")+f.where);}
 catch(...){faulted=true;skip.store(false);event("Battery usage stopped after an unexpected error");}
}
void pgr_battery_stop(uint64_t tid){
 skip.store(false);
 if(!initialized||faulted||tid!=owner||tid!=uint64_t(gettid()))return;
 try{apply(Config{});}catch(...){}
}

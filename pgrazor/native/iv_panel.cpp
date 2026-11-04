#include "iv_display.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
// Encounter and Pokemon-details IVs in the storage list's style. iv_display.cpp decides when an
// IV overlay is applied or restored; this draws it. Instead of re-texting the game's CP label,
// a soft off-white panel named PGRazorIvLine is added next to the label (under it in encounters,
// above it on the Pokemon details screen, where the level arc runs just below the CP) and holds a
// smaller copy of it reading "Lv 25 · IV 91.1% · 15/14/12", or the chosen part of that, with the
// percent colored by tier. Restoring hides the panel;
// the game's label is never changed. The panel is a child of the CP label, so it follows the
// label through layout and animation. Any managed failure stops the panel for the process and
// iv_display falls back to re-texting the label.
namespace {
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
template<class F>F fn(void *p){require(p,"missing runtime export");return reinterpret_cast<F>(p);}
PgrBootBinding boot{};
bool ready=false,faulted=false,busy=false,announced=false;
void *instantiate=nullptr,*get_component=nullptr,*get_components=nullptr,*find_child=nullptr,*new_game_object=nullptr,*set_parent=nullptr;
const char *kName="PGRazorIvLine";

void *exp(const char *name){std::string n="il2cpp_";n+=name;void *p=dlsym(boot.handle,n.c_str());require(p,name);return p;}
void *klass(void *o){require(o,"null object");return fn<void*(*)(void*)>(boot.runtime.object_get_class)(o);}
std::string typename_of(const void *type){char *p=fn<char*(*)(const void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
void *method(void *o,const char *name,int count){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(klass(o),name,count);require(m,name);return m;}
void *optional_method(void *o,const char *name,int count){return fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(klass(o),name,count);}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(o,name,n),o,args);}
template<class T>T unbox(void *o){require(o,"boxed value");T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");std::memcpy(&v,p,sizeof(v));return v;}
void *managed_string(const std::string &s){void *v=fn<void*(*)(const char*)>(boot.runtime.string_new)(s.c_str());require(v,"string");return v;}
std::string class_name(void *o){return fn<const char*(*)(void*)>(exp("class_get_name"))(klass(o));}
void *class_of(const char *ns,const char *name){
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api,"metadata API");
 PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);q.name_space=ns;q.name=name;q.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult r{};require(api->resolve_class(&q,&r)==0,name);return const_cast<void*>(r.klass);
}
// The non-generic overload of ns.owner.name whose parameters are exactly p0[, p1].
void *overload(const char *ns,const char *owner,const char *name,const char *p0,const char *p1=nullptr){
 void *k=class_of(ns,owner);const unsigned count=p1?2:1;void *iter=nullptr;
 while(void *m=fn<void*(*)(const void*,void**)>(exp("class_get_methods"))(k,&iter)){
  if(std::strcmp(fn<const char*(*)(void*)>(exp("method_get_name"))(m),name)!=0)continue;
  if(fn<bool(*)(void*)>(boot.meta.method_is_generic)(m)||fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(m)!=count)continue;
  auto param=[&](unsigned i){return typename_of(fn<const void*(*)(void*,unsigned)>(exp("method_get_param"))(m,i));};
  if(param(0)==p0&&(!p1||param(1)==p1))return m;
 }
 require(false,name);return nullptr;
}
void *class_type(const char *ns,const char *name){
 return fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(class_of(ns,name)));
}
void *type_object(void *o){return fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(klass(o)));}
void set_vector(void *o,const char *setter,float x,float y){float v[2]={x,y};void *a[]={v};call(o,setter,1,a);}
void set_bool(void *o,const char *setter,bool value){void *a[]={&value};call(o,setter,1,a);}
void set_color(void *graphic,float r,float g,float b,float a){float c[4]={r,g,b,a};void *args[]={c};call(graphic,"set_color",1,args);}

const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
void status(const char *phase,const std::string &message){
 auto tr=transport();if(!tr)return;
 const std::string json=std::string("{\"phase\":\"")+phase+"\",\"message\":\""+message+"\"}";
 PgrRuntimeTransportString strings[]={{"iv-panel",8},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,strings,2);
}

void init(){
 if(ready)return;
 auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);
 require(api&&api->snapshot(&boot)==0&&boot.handle,"Runtime unavailable");
 instantiate=overload("UnityEngine","Object","Instantiate","UnityEngine.Object","UnityEngine.Transform");
 get_component=overload("UnityEngine","Component","GetComponent","System.Type");
 get_components=overload("UnityEngine","Component","GetComponents","System.Type");
 new_game_object=overload("UnityEngine","GameObject",".ctor","System.String","System.Type[]");
 set_parent=overload("UnityEngine","Transform","SetParent","UnityEngine.Transform","System.Boolean");
 find_child=overload("UnityEngine","Transform","Find","System.String");
 ready=true;
}

// The storage list's IV tiers (storage_iv.cpp tier_color): darker shades for a light backing.
std::string tier_color(int sum){
 const double percent=sum/45.0*100.0;
 auto shade=[](const int a[3],const int b[3],double t){
  char out[8];int c[3];for(int i=0;i<3;i++)c[i]=int(a[i]+(b[i]-a[i])*t+0.5);
  std::snprintf(out,sizeof(out),"#%02x%02x%02x",c[0]&0xff,c[1]&0xff,c[2]&0xff);return std::string(out);};
 static const int g90a[3]={125,70,215},g90b[3]={160,95,245},g80a[3]={35,140,75},g80b[3]={55,175,95};
 if(sum==45)return "#e53935";
 if(percent>=90.0)return shade(g90a,g90b,(percent-90.0)/10.0);
 if(percent>=80.0)return shade(g80a,g80b,(percent-80.0)/10.0);
 return "";
}
// The format chosen for this screen from "S2|<storage>|<encounter>|<details>": 1 level and
// attack/defense/stamina, 2 level and percent, 3 all three. Level and percent when unreadable.
int format(bool owned){
 auto tr=transport();char s[16];uint32_t n=0;PgrRuntimeTransportSample a{};
 if(!tr||tr->sample(&a)||!a.snapshot_fresh||a.fault)return 2;
 if(tr->get_string(14,s,sizeof(s),&n)||n!=8||std::memcmp(s,"S2|",3)!=0)return 2;
 const char c=s[owned?7:5];return c>='1'&&c<='3'?c-'0':2;
}
// "Lv 25 · IV 91.1% · 15/14/12", the same numbers iv_display used on the label (one decimal, '--'
// unknown), without the stats for format 2 and without the percent for format 1. The percent, or
// the stats when they stand alone, takes the tier color.
std::string line(int32_t level_half,int atk,int def,int sta,int fmt){
 std::string lv="--";
 if(level_half!=PGR_IV_LEVEL_UNKNOWN&&level_half>0)lv=std::to_string(level_half/2)+(level_half%2?".5":"");
 const bool valid=atk>=0&&atk<=15&&def>=0&&def<=15&&sta>=0&&sta<=15;
 std::string iv="IV --";
 if(valid){
  const int sum=atk+def+sta;const long tenths=long(sum/45.0*1000.0+0.5);
  char pct[16];if(tenths%10==0)std::snprintf(pct,sizeof(pct),"%ld%%",tenths/10);else std::snprintf(pct,sizeof(pct),"%ld.%ld%%",tenths/10,tenths%10);
  iv=std::string("IV ")+pct;
 }
 const std::string stats=valid?std::to_string(atk)+"/"+std::to_string(def)+"/"+std::to_string(sta):"--/--/--";
 const std::string color=valid?tier_color(atk+def+sta):"";
 auto paint=[&](const std::string &s){return color.empty()?s:"<color="+color+">"+s+"</color>";};
 const char *dot=" \xc2\xb7 ";
 return "Lv "+lv+dot+(fmt==1?paint(stats):fmt==2?paint(iv):paint(iv)+dot+stats);
}

struct Line {void *panel=nullptr,*text=nullptr;};
// The panel under this CP label, or null. Its first child is the text copy.
Line find_line(void *cp_text){
 Line out;void *args[]={managed_string(kName)};
 void *found=invoke(find_child,call(cp_text,"get_transform"),args);if(!found)return out;
 require(unbox<int>(call(found,"get_childCount"))>=1,"IV line text");int first=0;void *index[]={&first};
 void *component_args[]={type_object(cp_text)};
 out.text=invoke(get_component,call(found,"GetChild",1,index),component_args);require(out.text,"IV line component");
 out.panel=call(found,"get_gameObject");return out;
}
// A new UI GameObject with a RectTransform, an Image and a LayoutElement, not yet parented.
void *new_panel(){
 void *types=fn<void*(*)(void*,uintptr_t)>(boot.runtime.array_new)(class_of("System","Type"),3);require(types,"IV panel types");
 void *components[]={class_type("UnityEngine","RectTransform"),class_type("UnityEngine.UI","Image"),class_type("UnityEngine.UI","LayoutElement")};
 for(int i=0;i<3;i++){void **slot=reinterpret_cast<void**>(static_cast<char*>(types)+32+i*8);fn<void(*)(void*,void**,void*)>(exp("gc_wbarrier_set_field"))(types,slot,components[i]);}
 void *panel=fn<void*(*)(void*)>(boot.runtime.object_new)(class_of("UnityEngine","GameObject"));require(panel,"IV panel");
 void *ctor_args[]={managed_string(kName),types};invoke(new_game_object,panel,ctor_args);
 return panel;
}
Line create_line(void *cp_text){
 Line out;out.panel=new_panel();void *panel_transform=call(out.panel,"get_transform");
 void *image_type[]={class_type("UnityEngine.UI","Image")};void *image=invoke(get_component,panel_transform,image_type);require(image,"IV panel image");
 // The storage cells' off-white, a little see-through.
 set_color(image,247/255.f,254/255.f,244/255.f,0.85f);set_bool(image,"set_raycastTarget",false);
 void *layout_type[]={class_type("UnityEngine.UI","LayoutElement")};
 if(void *layout=invoke(get_component,panel_transform,layout_type))set_bool(layout,"set_ignoreLayout",true);
 // Copy the label before the panel joins it, so the copy never contains the panel.
 void *copy_args[]={cp_text,panel_transform};void *text=invoke(instantiate,nullptr,copy_args);require(text&&klass(text)==klass(cp_text),"IV line copy");
 out.text=text;set_bool(text,"set_raycastTarget",false);
 void *copy_transform=call(text,"get_transform");
 set_vector(copy_transform,"set_anchorMin",0,0);set_vector(copy_transform,"set_anchorMax",1,1);
 set_vector(copy_transform,"set_pivot",0.5f,0.5f);set_vector(copy_transform,"set_sizeDelta",0,0);set_vector(copy_transform,"set_anchoredPosition",0,0);
 // The label's own children (a separate small "CP" prefix) are not part of the line.
 const int children=unbox<int>(call(copy_transform,"get_childCount"));require(children>=0&&children<=16,"IV line children");
 for(int i=0;i<children;i++){void *index[]={&i};void *child=call(copy_transform,"GetChild",1,index);bool off=false;void *active[]={&off};call(call(child,"get_gameObject"),"SetActive",1,active);}
 // Outlines and shadows that keep white text readable over the scene are not wanted on the panel.
 void *effect_type[]={class_type("UnityEngine.UI","BaseMeshEffect")};
 if(void *effects=invoke(get_components,copy_transform,effect_type)){
  const uintptr_t count=*reinterpret_cast<uintptr_t*>(static_cast<char*>(effects)+24);require(count<=16,"IV line effects");
  for(uintptr_t i=0;i<count;i++){void *effect=*reinterpret_cast<void**>(static_cast<char*>(effects)+32+i*8);if(effect)set_bool(effect,"set_enabled",false);}
 }
 // Dark text, one centered line however long, the percent colored by tier.
 set_color(text,52/255.f,64/255.f,72/255.f,1.f);
 if(void *m=optional_method(text,"set_supportRichText",1)){bool rich=true;void *a[]={&rich};invoke(m,text,a);}
 if(void *m=optional_method(text,"set_resizeTextForBestFit",1)){bool fit=false;void *a[]={&fit};invoke(m,text,a);}
 if(void *m=optional_method(text,"set_horizontalOverflow",1)){int overflow=1;void *a[]={&overflow};invoke(m,text,a);}
 if(void *m=optional_method(text,"set_verticalOverflow",1)){int overflow=1;void *a[]={&overflow};invoke(m,text,a);}
 if(class_name(text)=="Text"||optional_method(text,"set_alignment",1)){int middle_center=4;void *a[]={&middle_center};call(text,"set_alignment",1,a);}
 bool keep_world=false;void *parent_args[]={call(cp_text,"get_transform"),&keep_world};invoke(set_parent,panel_transform,parent_args);
 return out;
}
// Half the label's font, a one-line panel hugging the text, just below the label's glyphs in an
// encounter and just above them on the details screen. The label's alignment says where its glyphs
// sit relative to its rectangle (the rectangle can be zero wide when the text overflows), so the
// panel is anchored at that point.
void place(void *cp_text,const Line &l,bool owned){
 const int label_font=unbox<int>(call(cp_text,"get_fontSize"));
 int font=int(label_font*(owned?0.45f:0.5f));if(font<10)font=10;if(font>40)font=40;
 {void *a[]={&font};call(l.text,"set_fontSize",1,a);}
 const float glyph_w=unbox<float>(call(cp_text,"get_preferredWidth")),glyph_h=unbox<float>(call(cp_text,"get_preferredHeight"));
 int alignment=4;if(void *m=optional_method(cp_text,"get_alignment",0))alignment=unbox<int>(invoke(m,cp_text));
 if(alignment<0||alignment>8)alignment=4;
 const int column=alignment%3,row=alignment/3;  // TextAnchor: Upper/Middle/Lower x Left/Center/Right
 const float ax=column*0.5f,ay=row==0?1.f:row==1?0.5f:0.f;
 const float x=column==0?glyph_w*0.5f:column==2?-glyph_w*0.5f:0.f;
 const float glyph_bottom=row==0?-glyph_h:row==1?-glyph_h*0.5f:0.f,glyph_top=glyph_bottom+glyph_h;
 const float height=font*1.3f,width=unbox<float>(call(l.text,"get_preferredWidth"))+font*0.9f;
 void *panel_transform=call(l.panel,"get_transform");
 set_vector(panel_transform,"set_anchorMin",ax,ay);set_vector(panel_transform,"set_anchorMax",ax,ay);
 set_vector(panel_transform,"set_pivot",0.5f,owned?0.f:1.f);set_vector(panel_transform,"set_sizeDelta",width,height);
 set_vector(panel_transform,"set_anchoredPosition",x,owned?glyph_top+font*0.1f:glyph_bottom-font*0.2f);
 float one[3]={1,1,1};void *scale_args[]={one};call(panel_transform,"set_localScale",1,scale_args);
}

bool show(bool owned,void *cp_text,int32_t level_half,int atk,int def,int sta){
 if(faulted||busy||!cp_text)return false;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 try{
  init();
  Line l=find_line(cp_text);
  if(!l.panel)l=create_line(cp_text);
  void *args[]={managed_string(line(level_half,atk,def,sta,format(owned)))};call(l.text,"set_text",1,args);
  place(cp_text,l,owned);
  bool on=true;void *active[]={&on};call(l.panel,"SetActive",1,active);
  if(!announced){announced=true;status("ready",std::string("IV panel shown (")+class_name(cp_text)+")");}
  return true;
 }catch(const Failure &f){faulted=true;status("error",std::string("IV panel stopped: ")+f.where);}
 catch(...){faulted=true;status("error","IV panel stopped: native failure");}
 return false;
}
void hide(void *cp_text){
 if(!ready||busy||!cp_text)return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 try{
  const Line l=find_line(cp_text);
  if(l.panel){bool off=false;void *active[]={&off};call(l.panel,"SetActive",1,active);}
 }catch(const Failure &f){faulted=true;status("error",std::string("IV panel stopped: ")+f.where);}
 catch(...){faulted=true;status("error","IV panel stopped: native failure");}
}
bool available(){
 if(faulted||busy)return false;
 try{init();return true;}
 catch(const Failure &f){faulted=true;status("error",std::string("IV panel unavailable: ")+f.where);}
 catch(...){faulted=true;status("error","IV panel unavailable: native failure");}
 return false;
}
const PgrIvPanelHooks kHooks={&available,&show,&hide};
__attribute__((constructor)) void register_panel(){pgr_iv_display_set_panel_hooks(&kHooks);}
}  // namespace

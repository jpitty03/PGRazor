#include "iv_display.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include "pvp_icons.h"
#include "pvp_keep.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
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
bool ready=false,faulted=false,busy=false,announced=false,announced_owned=false;
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
// The non-generic overload of ns.owner.name whose parameters are exactly p0[, p1[, p2]].
void *overload(const char *ns,const char *owner,const char *name,const char *p0,const char *p1=nullptr,const char *p2=nullptr){
 void *k=class_of(ns,owner);const unsigned count=p2?3:p1?2:1;void *iter=nullptr;
 while(void *m=fn<void*(*)(const void*,void**)>(exp("class_get_methods"))(k,&iter)){
  if(std::strcmp(fn<const char*(*)(void*)>(exp("method_get_name"))(m),name)!=0)continue;
  if(fn<bool(*)(void*)>(boot.meta.method_is_generic)(m)||fn<unsigned(*)(void*)>(boot.meta.method_get_param_count)(m)!=count)continue;
  auto param=[&](unsigned i){return typename_of(fn<const void*(*)(void*,unsigned)>(exp("method_get_param"))(m,i));};
  if(param(0)==p0&&(!p1||param(1)==p1)&&(!p2||param(2)==p2))return m;
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
 const char c=s[owned?7:5];return c>='1'&&c<='4'?c-'0':2;
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
 // Format 4 is format 3 with a second row of PvP crests and ranks, drawn by separate pieces.
 return "Lv "+lv+dot+(fmt==1?paint(stats):fmt==2?paint(iv):paint(iv)+dot+stats);
}

// PvP pieces of a format-4 line, per league (Great, Ultra, Master): the crest (an Image) and the rank
// (a further text copy), children of the panel after the main text.
const char *const kRankNames[]={"PGRazorPvpRank0","PGRazorPvpRank1","PGRazorPvpRank2"};
const char *const kIconNames[]={"PGRazorPvpIcon0","PGRazorPvpIcon1","PGRazorPvpIcon2"};
const char *const kLeagueColor[]={"#2a63c4","#a87405","#6a3fb5"};
struct Line {void *panel=nullptr,*text=nullptr,*rank[3]={},*icon[3]={};};
void *child_named(void *transform,const char *name){void *args[]={managed_string(name)};return invoke(find_child,transform,args);}
// On the Pokemon details screen the panel sits just above the Pokemon's name (PokemonInfoPanel.nameText),
// below the model and clear of the status bar; null when that label cannot be found, and the panel then
// stays above the CP label.
void *name_label(void *cp_text){
 static void *in_parent=nullptr,*info_class=nullptr,*name_field=nullptr;static bool tried=false;
 try{
 if(!tried){
  tried=true;
  in_parent=overload("UnityEngine","Component","GetComponentInParent","System.Type");
  info_class=class_of("Niantic.Holoholo.UI.Pokemon","PokemonInfoPanel");
  void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(info_class,"nameText");
  if(f&&!(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10)&&typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))=="UnityEngine.UI.Text")name_field=f;
 }
 if(!name_field)return nullptr;
 void *type_args[]={fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(info_class))};
 void *info=invoke(in_parent,cp_text,type_args);if(!info||klass(info)!=info_class)return nullptr;
 void *name=nullptr;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(info,name_field,&name);
 return name&&klass(name)==klass(cp_text)?name:nullptr;
 }catch(...){name_field=nullptr;return nullptr;}
}
// What the panel is attached to: the name label on the details screen when found, else the CP label.
void *anchor_of(bool owned,void *cp_text){void *name=owned?name_label(cp_text):nullptr;return name?name:cp_text;}
// The panel under this label (the CP label, or the name label it is attached to), or null. Its first
// child is the text copy.
Line find_line(void *anchor,void *cp_text){
 Line out;void *found=child_named(call(anchor,"get_transform"),kName);if(!found)return out;
 require(unbox<int>(call(found,"get_childCount"))>=1,"IV line text");int first=0;void *index[]={&first};
 void *component_args[]={type_object(cp_text)};
 out.text=invoke(get_component,call(found,"GetChild",1,index),component_args);require(out.text,"IV line component");
 for(int i=0;i<3;i++){
  if(void *rank=child_named(found,kRankNames[i]))out.rank[i]=invoke(get_component,rank,component_args);
  if(void *icon=child_named(found,kIconNames[i])){void *image_type[]={class_type("UnityEngine.UI","Image")};out.icon[i]=invoke(get_component,icon,image_type);}
 }
 out.panel=call(found,"get_gameObject");return out;
}

// League crest sprites (Great, Ultra, Master), made once from the bundled PNGs and kept for the process.
uintptr_t sprites[3]={0,0,0};
void *sprite(int crest){
 require(crest>=0&&crest<3,"PvP crest");
 if(!sprites[crest]){
  void *texture_class=class_of("UnityEngine","Texture2D");
  void *texture=fn<void*(*)(void*)>(boot.runtime.object_new)(texture_class);require(texture,"PvP texture");
  int two=2;void *size[]={&two,&two};invoke(overload("UnityEngine","Texture2D",".ctor","System.Int32","System.Int32"),texture,size);
  const unsigned length=pgr_pvp_icons::size[crest];
  void *bytes=fn<void*(*)(void*,uintptr_t)>(boot.runtime.array_new)(class_of("System","Byte"),length);require(bytes,"PvP crest bytes");
  std::memcpy(static_cast<char*>(bytes)+32,pgr_pvp_icons::png[crest],length);
  void *load[]={texture,bytes};
  require(unbox<bool>(invoke(overload("UnityEngine","ImageConversion","LoadImage","UnityEngine.Texture2D","System.Byte[]"),nullptr,load)),"PvP crest decode");
  const float w=float(unbox<int>(call(texture,"get_width"))),h=float(unbox<int>(call(texture,"get_height")));
  float rect[4]={0,0,w,h},pivot[2]={0.5f,0.5f};void *create[]={texture,rect,pivot};
  void *made=invoke(overload("UnityEngine","Sprite","Create","UnityEngine.Texture2D","UnityEngine.Rect","UnityEngine.Vector2"),nullptr,create);
  require(made,"PvP crest sprite");
  sprites[crest]=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(made,0);require(sprites[crest],"PvP crest root");
 }
 void *value=fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(sprites[crest]);require(value,"PvP crest sprite");return value;
}
// Adds one league's rank text and crest image to a panel that lacks them.
void add_pvp_pieces(Line &l,int league){
 void *panel_transform=call(l.panel,"get_transform");
 if(!l.rank[league]){
  void *copy_args[]={l.text,panel_transform};void *rank=invoke(instantiate,nullptr,copy_args);require(rank&&klass(rank)==klass(l.text),"PvP rank copy");
  void *name[]={managed_string(kRankNames[league])};call(call(rank,"get_gameObject"),"set_name",1,name);l.rank[league]=rank;
 }
 if(!l.icon[league]){
  void *types=fn<void*(*)(void*,uintptr_t)>(boot.runtime.array_new)(class_of("System","Type"),2);require(types,"PvP icon types");
  void *components[]={class_type("UnityEngine","RectTransform"),class_type("UnityEngine.UI","Image")};
  for(int i=0;i<2;i++){void **slot=reinterpret_cast<void**>(static_cast<char*>(types)+32+i*8);fn<void(*)(void*,void**,void*)>(exp("gc_wbarrier_set_field"))(types,slot,components[i]);}
  void *icon=fn<void*(*)(void*)>(boot.runtime.object_new)(class_of("UnityEngine","GameObject"));require(icon,"PvP icon");
  void *ctor_args[]={managed_string(kIconNames[league]),types};invoke(new_game_object,icon,ctor_args);
  void *icon_transform=call(icon,"get_transform");
  bool keep_world=false;void *parent_args[]={panel_transform,&keep_world};invoke(set_parent,icon_transform,parent_args);
  void *image_type[]={class_type("UnityEngine.UI","Image")};l.icon[league]=invoke(get_component,icon_transform,image_type);require(l.icon[league],"PvP icon image");
  set_bool(l.icon[league],"set_raycastTarget",false);set_bool(l.icon[league],"set_preserveAspect",true);
  void *sprite_args[]={sprite(league)};call(l.icon[league],"set_sprite",1,sprite_args);
 }
}
void set_active(void *component,bool on){if(component){void *a[]={&on};call(call(component,"get_gameObject"),"SetActive",1,a);}}
// A new UI GameObject with a RectTransform, an Image and a LayoutElement, not yet parented.
void *new_panel(){
 void *types=fn<void*(*)(void*,uintptr_t)>(boot.runtime.array_new)(class_of("System","Type"),3);require(types,"IV panel types");
 void *components[]={class_type("UnityEngine","RectTransform"),class_type("UnityEngine.UI","Image"),class_type("UnityEngine.UI","LayoutElement")};
 for(int i=0;i<3;i++){void **slot=reinterpret_cast<void**>(static_cast<char*>(types)+32+i*8);fn<void(*)(void*,void**,void*)>(exp("gc_wbarrier_set_field"))(types,slot,components[i]);}
 void *panel=fn<void*(*)(void*)>(boot.runtime.object_new)(class_of("UnityEngine","GameObject"));require(panel,"IV panel");
 void *ctor_args[]={managed_string(kName),types};invoke(new_game_object,panel,ctor_args);
 return panel;
}
Line create_line(void *cp_text,void *anchor){
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
 bool keep_world=false;void *parent_args[]={call(anchor,"get_transform"),&keep_world};invoke(set_parent,panel_transform,parent_args);
 return out;
}
// Half the label's font, a one-line panel hugging the text, just below the label's glyphs in an
// encounter and just above them on the details screen. The label's alignment says where its glyphs
// sit relative to its rectangle (the rectangle can be zero wide when the text overflows), so the
// panel is anchored at that point.
// A rect centered in the panel at x with the given size.
void put(void *component,float x,float y,float w,float h){
 void *t=call(component,"get_transform");
 set_vector(t,"set_anchorMin",0.5f,0.5f);set_vector(t,"set_anchorMax",0.5f,0.5f);set_vector(t,"set_pivot",0.5f,0.5f);
 set_vector(t,"set_sizeDelta",w,h);set_vector(t,"set_anchoredPosition",x,y);
 float one[3]={1,1,1};void *scale_args[]={one};call(t,"set_localScale",1,scale_args);
}
// shown[league]: that league's crest and rank go on the second row.
// The font follows the CP label; the position follows the anchor's glyphs.
void place(void *cp_text,void *anchor,const Line &l,bool owned,const bool shown[3]){
 const int label_font=unbox<int>(call(cp_text,"get_fontSize"));
 int font=int(label_font*(owned?0.45f:0.5f));if(font<10)font=10;if(font>40)font=40;
 {void *a[]={&font};call(l.text,"set_fontSize",1,a);}
 const bool pvp=shown[0]||shown[1]||shown[2];
 for(int i=0;i<3;i++)if(shown[i]){void *a[]={&font};call(l.rank[i],"set_fontSize",1,a);}
 const float glyph_w=unbox<float>(call(anchor,"get_preferredWidth"));float glyph_h=unbox<float>(call(anchor,"get_preferredHeight"));
 if(anchor!=cp_text){
  // The name label shrinks long names to fit (best fit); its preferred height is for the nominal size,
  // which would leave the panel far above the visible name and in front of tall models' area.
  const int nominal=unbox<int>(call(anchor,"get_fontSize"));int used=nominal;bool fit=false;
  if(void *m=optional_method(anchor,"get_resizeTextForBestFit",0))fit=unbox<bool>(invoke(m,anchor));
  if(fit)if(void *m=optional_method(anchor,"get_cachedTextGenerator",0))if(void *generator=invoke(m,anchor))
   if(void *u=optional_method(generator,"get_fontSizeUsedForBestFit",0))used=unbox<int>(invoke(u,generator));
  if(fit&&used>0&&nominal>0&&used<nominal)glyph_h*=float(used)/float(nominal);
 }
 int alignment=4;if(void *m=optional_method(anchor,"get_alignment",0))alignment=unbox<int>(invoke(m,anchor));
 if(alignment<0||alignment>8)alignment=4;
 const int column=alignment%3,row=alignment/3;  // TextAnchor: Upper/Middle/Lower x Left/Center/Right
 const float ax=column*0.5f,ay=row==0?1.f:row==1?0.5f:0.f;
 const float x=column==0?glyph_w*0.5f:column==2?-glyph_w*0.5f:0.f;
 const float glyph_bottom=row==0?-glyph_h:row==1?-glyph_h*0.5f:0.f,glyph_top=glyph_bottom+glyph_h;
 const float line_h=font*1.3f,text_w=unbox<float>(call(l.text,"get_preferredWidth"));
 float width=text_w+font*0.9f,height=line_h;
 void *text_transform=call(l.text,"get_transform");
 if(pvp){
  // Second row: [crest] [rank] per league, centered under the IVs.
  const float icon=font*1.25f,gap=font*0.25f,spacing=font*0.7f;
  float rank_w[3]={0,0,0},row_w=0;int count=0;
  for(int i=0;i<3;i++)if(shown[i]){rank_w[i]=unbox<float>(call(l.rank[i],"get_preferredWidth"));row_w+=(count?spacing:0.f)+icon+gap+rank_w[i];++count;}
  if(row_w+font*0.9f>width)width=row_w+font*0.9f;
  height=line_h*2;
  put(l.text,0,line_h*0.5f,text_w+2,line_h);
  float at=-row_w*0.5f;count=0;
  for(int i=0;i<3;i++)if(shown[i]){
   if(count++)at+=spacing;
   put(l.icon[i],at+icon*0.5f,-line_h*0.5f,icon,icon);at+=icon+gap;
   put(l.rank[i],at+rank_w[i]*0.5f,-line_h*0.5f,rank_w[i]+2,line_h);at+=rank_w[i];
  }
 }else{
  // The text fills the panel.
  set_vector(text_transform,"set_anchorMin",0,0);set_vector(text_transform,"set_anchorMax",1,1);
  set_vector(text_transform,"set_pivot",0.5f,0.5f);set_vector(text_transform,"set_sizeDelta",0,0);set_vector(text_transform,"set_anchoredPosition",0,0);
 }
 void *panel_transform=call(l.panel,"get_transform");
 set_vector(panel_transform,"set_anchorMin",ax,ay);set_vector(panel_transform,"set_anchorMax",ax,ay);
 set_vector(panel_transform,"set_pivot",0.5f,owned?0.f:1.f);set_vector(panel_transform,"set_sizeDelta",width,height);
 set_vector(panel_transform,"set_anchoredPosition",x,owned?glyph_top+font*0.1f:glyph_bottom-font*0.2f);
 float one[3]={1,1,1};void *scale_args[]={one};call(panel_transform,"set_localScale",1,scale_args);
}

// The details screen shows the 3D Pokemon as a RawImage (a render texture) later in the same canvas
// as the card, so it covers anything on the card above the name. A nested canvas with override
// sorting one above the screen's canvas draws the panel after it. Added once per panel.
void draw_in_front(const Line &l,void *anchor){
 void *canvas_type[]={class_type("UnityEngine","Canvas")};
 void *panel_transform=call(l.panel,"get_transform");
 // A canvas is drawn by the cameras whose culling mask has its GameObject's layer; the panel is a new
 // object on the Default layer, which the UI camera does not draw. Use the name label's layer.
 const int layer=unbox<int>(call(call(anchor,"get_gameObject"),"get_layer"));
 void *panel_object=call(panel_transform,"get_gameObject");
 if(unbox<int>(call(panel_object,"get_layer"))!=layer){
  void *layer_args[]={const_cast<int*>(&layer)};call(panel_object,"set_layer",1,layer_args);
  const int children=unbox<int>(call(panel_transform,"get_childCount"));
  for(int i=0;i<children&&i<16;i++){void *index[]={&i};void *child=call(call(panel_transform,"GetChild",1,index),"get_gameObject");call(child,"set_layer",1,layer_args);}
 }
 if(!invoke(get_component,panel_transform,canvas_type)){
  static void *in_parent=nullptr,*add=nullptr;
  if(!in_parent)in_parent=overload("UnityEngine","Component","GetComponentInParent","System.Type");
  if(!add)add=overload("UnityEngine","GameObject","AddComponent","System.Type");
  void *screen=invoke(in_parent,anchor,canvas_type);if(!screen)return;
  const int order=unbox<int>(call(screen,"get_sortingOrder"));
  void *canvas=invoke(add,l.panel,canvas_type);require(canvas,"IV panel canvas");
  bool on=true;void *override_args[]={&on};call(canvas,"set_overrideSorting",1,override_args);
  int above=order+1;void *order_args[]={&above};call(canvas,"set_sortingOrder",1,order_args);
 }
 // The scroll view's RectMask2D cannot clip into a nested canvas and culls it instead, so the panel's
 // graphics opt out of masking (they stay on the card, above the name).
 void *graphic_type[]={class_type("UnityEngine.UI","MaskableGraphic")};
 static void *in_children=nullptr;
 if(!in_children)in_children=overload("UnityEngine","Component","GetComponentsInChildren","System.Type","System.Boolean");
 bool inactive=true;void *children_args[]={graphic_type[0],&inactive};
 if(void *list=invoke(in_children,panel_transform,children_args)){
  const uintptr_t n=*reinterpret_cast<uintptr_t*>(static_cast<char*>(list)+24);require(n<=32,"IV panel graphics");
  for(uintptr_t i=0;i<n;i++)if(void *g=*reinterpret_cast<void**>(static_cast<char*>(list)+32+i*8))set_bool(g,"set_maskable",false);
 }
}

bool show(bool owned,void *cp_text,int32_t level_half,int atk,int def,int sta,const PgrPvpBest *pvp){
 if(faulted||busy||!cp_text)return false;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 try{
  init();
  void *anchor=anchor_of(owned,cp_text);
  // A panel left on the CP label (an earlier placement) is hidden once the name label holds it.
  if(anchor!=cp_text){const Line old=find_line(cp_text,cp_text);if(old.panel){bool off=false;void *a[]={&off};call(old.panel,"SetActive",1,a);}}
  Line l=find_line(anchor,cp_text);
  if(!l.panel)l=create_line(cp_text,anchor);
  const int fmt=format(owned);
  bool shown[3]={false,false,false};
  for(int i=0;i<3;i++){
   shown[i]=fmt==4&&pvp&&pvp->rank[i]>0;
   if(!shown[i]){set_active(l.rank[i],false);set_active(l.icon[i],false);continue;}
   add_pvp_pieces(l,i);
   std::string rank="#"+std::to_string(pvp->rank[i]);
   if(pvp->species[i][0])rank+=std::string(" ")+pvp->species[i];
   void *rank_args[]={managed_string("<color="+std::string(kLeagueColor[i])+">"+rank+"</color>")};call(l.rank[i],"set_text",1,rank_args);
   set_active(l.rank[i],true);set_active(l.icon[i],true);
  }
  void *args[]={managed_string(line(level_half,atk,def,sta,fmt))};call(l.text,"set_text",1,args);
  place(cp_text,anchor,l,owned,shown);
  if(owned&&anchor!=cp_text)draw_in_front(l,anchor);
  bool on=true;void *active[]={&on};call(l.panel,"SetActive",1,active);
  if(!announced||(owned&&!announced_owned)){announced=true;if(owned)announced_owned=true;
   status("ready",std::string("IV panel shown (")+class_name(cp_text)+(owned?anchor!=cp_text?", above name":", above CP":"")+")");}
  return true;
 }catch(const Failure &f){faulted=true;status("error",std::string("IV panel stopped: ")+f.where);}
 catch(...){faulted=true;status("error","IV panel stopped: native failure");}
 return false;
}
void hide(void *cp_text){
 if(!ready||busy||!cp_text)return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;
 try{
  for(void *anchor:{cp_text,name_label(cp_text)}){
   if(!anchor)continue;
   const Line l=find_line(anchor,cp_text);
   if(l.panel){bool off=false;void *active[]={&off};call(l.panel,"SetActive",1,active);}
  }
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

// The Great (0), Ultra (1) or Master (2) League crest sprite for other displays (the storage list), or
// null when it cannot be made. Unity owner only.
extern "C" void *pgr_pvp_crest(int crest){
 if(faulted)return nullptr;
 try{init();return sprite(crest);}
 catch(const Failure &f){status("error",std::string("PvP crest unavailable: ")+f.where);}
 catch(...){status("error","PvP crest unavailable: native failure");}
 return nullptr;
}

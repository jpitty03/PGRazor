#include "storage_iv.h"
#include "game_bindings.h"
#include "hook_engine.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include "shared_metadata.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>
// Pokemon storage list IVs. PokemonInventoryCellView.Initialize fills one storage cell each time it
// shows a Pokemon (cells are reused while scrolling). After the game's own Initialize, a copy of the
// cell's CP label, on a soft off-white panel named PGRazorIv, is placed just under it and shows the
// Pokemon's level and IVs, and the CP header is drawn in front of the Pokemon image. Turning the setting off
// hides the panel; the game's own label text is never changed. The hook is installed the first time
// the setting is on and stays for the process, like every other code hook.
namespace {
struct Failure {const char *where;};
void require(bool ok,const char *where){if(!ok)throw Failure{where};}
template<class F>F fn(void *p){require(p,"missing runtime export");return reinterpret_cast<F>(p);}
PgrBootBinding boot{};uint64_t owner=0;bool ready=false,busy=false;
std::atomic<int> mode{0};
std::atomic<bool> installed{false},faulted{false};
bool pokemon_arg=false;  // Initialize(Initializer) confirmed; the Pokemon is Initializer.PokemonProto
using Init=void(*)(void*,void*,const void*);
std::atomic<Init> original{nullptr};
void *instantiate=nullptr,*get_component=nullptr,*find_child=nullptr,*new_game_object=nullptr,*set_parent=nullptr;  // exact non-generic overloads
void *exp(const char *name){std::string n="il2cpp_";n+=name;void *p=dlsym(boot.handle,n.c_str());require(p,name);return p;}
void *klass(void *o){require(o,"null object");return fn<void*(*)(void*)>(boot.runtime.object_get_class)(o);}
std::string typename_of(const void *type){char *p=fn<char*(*)(const void*)>(boot.meta.type_get_name)(type);require(p,"type name");std::string s=p;fn<void(*)(void*)>(boot.meta.mem_free)(p);return s;}
void *field_info(void *o,const char *name){void *f=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(klass(o),name);require(f,name);require(!(fn<int(*)(void*)>(boot.meta.field_get_flags)(f)&0x10),name);return f;}
template<class T>T field(void *o,const char *name,const char *type){
 void *f=field_info(o,name);require(typename_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))==type,name);
 T v{};fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;}
// An object reference field of any class type (the CP label is a UI text component).
void *reference_field(void *o,const char *name){
 void *f=field_info(o,name);require(fn<int(*)(const void*)>(exp("type_get_type"))(fn<void*(*)(void*)>(boot.meta.field_get_type)(f))==0x12,name);
 void *v=nullptr;fn<void(*)(void*,void*,void*)>(boot.runtime.field_get_value)(o,f,&v);return v;}
void *method(void *o,const char *name,int count){void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(klass(o),name,count);require(m,name);return m;}
void *optional_method(void *o,const char *name,int count){return fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(klass(o),name,count);}
void *invoke(void *m,void *o,void **args=nullptr){void *ex=nullptr;void *v=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(m,o,args,&ex);require(!ex,"managed call failed");return v;}
void *call(void *o,const char *name,int n=0,void **args=nullptr){return invoke(method(o,name,n),o,args);}
template<class T>T unbox(void *o){require(o,"boxed value");T v{};auto p=fn<void*(*)(void*)>(boot.runtime.object_unbox)(o);require(p,"boxed value");std::memcpy(&v,p,sizeof(v));return v;}
void *managed_string(const std::string &s){void *v=fn<void*(*)(const char*)>(boot.runtime.string_new)(s.c_str());require(v,"string");return v;}
std::string class_name(void *o){return fn<const char*(*)(void*)>(exp("class_get_name"))(klass(o));}
const PgrRuntimeTransportApi *transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
void event(const std::string &json){auto tr=transport();if(!tr)return;PgrRuntimeTransportString strings[]={{"storage-iv",10},{json.c_str(),uint32_t(json.size())}};tr->emit(19,nullptr,0,strings,2);}
void status(const char *phase,const std::string &message){event(std::string("{\"phase\":\"")+phase+"\",\"message\":\""+message+"\"}");}

// The storage mode of "S2|<storage>|<encounter>|<details>" from the current snapshot (0 off,
// 1 level and attack/defense/stamina, 2 level and percent, 3 all three); anything else means off.
int configured_mode(){
 auto tr=transport();char s[16];uint32_t n=0;PgrRuntimeTransportSample a{};
 if(!tr||tr->sample(&a)||!a.snapshot_fresh||a.fault)return 0;
 if(tr->get_string(14,s,sizeof(s),&n)||n!=8||std::memcmp(s,"S2|",3)!=0||s[3]<'0'||s[3]>'3')return 0;
 return s[3]-'0';
}

// Arguments that travel in general registers: references, enums and integers up to 8 bytes. The
// replacement forwards x1..x4 unchanged, so floating point or struct arguments are refused.
bool register_sized(const void *type){
 const int kind=fn<int(*)(const void*)>(exp("type_get_type"))(type);
 if(fn<bool(*)(const void*)>(exp("type_is_byref"))(type))return true;
 if((kind>=0x02&&kind<=0x0b)||kind==0x0e||kind==0x12||kind==0x14||kind==0x18||kind==0x19||kind==0x1c||kind==0x1d)return true;
 void *k=fn<void*(*)(const void*)>(boot.meta.class_from_type)(type);if(!k)return false;
 if(kind==0x11)return fn<bool(*)(void*)>(exp("class_is_enum"))(k);
 if(kind==0x15)return !fn<bool(*)(void*)>(exp("class_is_valuetype"))(k);
 return false;
}
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
void *type_object(void *o){return fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(klass(o)));}

// Levels. The cells show no level, so it comes from the Pokemon's CP multiplier and the game
// master's multiplier per whole level (GameMasterDataExt_PlayerLevelSettings.CpMultiplierAtLevel),
// read once on the owner thread when the game master is ready. A half level's multiplier is the
// quadratic mean of its neighbors, with the same tolerance as the encounter overlay. Without the
// table the line simply has no level; a failure here never stops the IVs.
std::vector<float> cpm;  // cpm[i] = multiplier at level i+1, strictly increasing
const PgrSharedMetadataOutput *md=nullptr;
uint64_t next_table_try=0;int table_tries=0;
constexpr int kLevelUnknown=-1,kTableTries=60;
uint64_t clock_ms(){timespec t{};clock_gettime(CLOCK_BOOTTIME,&t);return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
struct Root {
 uintptr_t h=0;
 Root()=default;explicit Root(void *v){if(v){h=fn<uintptr_t(*)(void*,int)>(boot.runtime.gchandle_new)(v,0);require(h,"GC root");}}
 Root(const Root&)=delete;Root& operator=(const Root&)=delete;
 ~Root(){if(h)fn<void(*)(uintptr_t)>(boot.runtime.gchandle_free)(h);}
 void *get()const{return h?fn<void*(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h):nullptr;}
};
void *handle(unsigned id){require(md,"metadata");void *p=const_cast<void*>(md->opt_handles[id]);require(p,"game services");return p;}
void *class_type_object(void *k){return fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(k));}
// IGameMasterData from the game's service container, as ar_values.cpp resolves it.
void *game_master(){
 using namespace pgr_shared_meta;
 Root type(class_type_object(handle(H_IL_MAP)));void *args[]={type.get()};
 Root map(invoke(handle(H_IL_FIND_M),nullptr,args));if(!map.get())return nullptr;
 Root container(invoke(handle(H_IL_CONTAINER_M),map.get()));if(!container.get())return nullptr;
 Root target(class_type_object(class_of("","IGameMasterData")));void *resolve_args[]={target.get()};
 return invoke(handle(H_IL_RESOLVE_M),container.get(),resolve_args);
}
// True once the table is read; false to try again later.
bool load_levels(){
 Root gm(game_master());if(!gm.get())return false;
 if(!unbox<bool>(call(gm.get(),"get_IsReady")))return false;
 void *levels=class_of("Niantic.Holoholo","GameMasterDataExt_PlayerLevelSettings");
 void *at=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(levels,"CpMultiplierAtLevel",2);require(at,"CpMultiplierAtLevel");
 std::vector<float> table;
 for(int level=1;level<=100;level++){
  void *args[]={gm.get(),&level};void *ex=nullptr;
  void *boxed=fn<void*(*)(void*,void*,void**,void**)>(boot.runtime.runtime_invoke)(at,nullptr,args,&ex);
  if(ex||!boxed)break;  // past the last level
  const float v=unbox<float>(boxed);
  if(!std::isfinite(v)||v<=0||(!table.empty()&&v<=table.back()))break;
  table.push_back(v);
 }
 require(table.size()>=40,"level table");
 cpm.swap(table);return true;
}
int level_half(float multiplier){
 if(cpm.empty()||!std::isfinite(multiplier)||multiplier<=0)return kLevelUnknown;
 const double m=multiplier;
 for(size_t i=0;i<cpm.size();i++){
  if(std::fabs(cpm[i]-m)<0.00002)return int(i+1)*2;
  if(i+1<cpm.size()&&std::fabs(std::sqrt((double(cpm[i])*cpm[i]+double(cpm[i+1])*cpm[i+1])/2.0)-m)<0.00002)return int(i+1)*2+1;
 }
 return kLevelUnknown;
}
void tick_levels(){
 if(!cpm.empty()||table_tries>=kTableTries)return;
 const uint64_t now=clock_ms();if(now<next_table_try)return;next_table_try=now+5000;
 std::string why="game master not ready";
 try{if(load_levels()){status("levels",std::to_string(cpm.size())+" levels");return;}}
 catch(const Failure &f){why=f.where;}
 catch(...){why="native failure";}
 if(++table_tries>=kTableTries)status("levels","No storage list levels: "+why);
}

// The encounter overlay's IV tiers (iv_display.cpp iv_color): 100% red, 90%+ a purple and 80%+ a
// green that brighten toward the next tier, below that the label's own color. The shades here are
// darker than the overlay's, which is drawn on a dark plate; storage cells are white.
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
// "Lv 25 · 91% · 15/14/12" for mode 3, without the percent for 1 and without the stats for 2. The
// percent (or the stats, when they stand alone) takes the tier color. No level when it is unknown.
std::string label(int atk,int def,int sta,int m,int half){
 const std::string stats=std::to_string(atk)+"/"+std::to_string(def)+"/"+std::to_string(sta);
 const std::string pct=std::to_string(((atk+def+sta)*100+22)/45)+"%";
 const std::string color=tier_color(atk+def+sta);
 auto paint=[&](const std::string &s){return color.empty()?s:"<color="+color+">"+s+"</color>";};
 const std::string lv=half>0?"Lv "+std::to_string(half/2)+(half%2?".5":"")+" · ":"";
 return lv+(m==1?paint(stats):m==2?paint(pct):paint(pct)+" · "+stats);
}
void *layout_element_type(){
 return fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(class_of("UnityEngine.UI","LayoutElement")));
}
void *class_type(const char *ns,const char *name){
 return fn<void*(*)(void*)>(exp("type_get_object"))(fn<void*(*)(void*)>(boot.meta.class_get_type)(class_of(ns,name)));
}
void set_vector(void *o,const char *setter,float x,float y){float v[2]={x,y};void *a[]={v};call(o,setter,1,a);}
// A cell's IV label: the PGRazorIv panel (a soft off-white backing, in the CP header's layout) and
// the copy of the CP label inside it that shows the text.
struct Label {void *panel=nullptr,*text=nullptr;};
// A new UI GameObject with a RectTransform, an Image and a LayoutElement, under parent.
void *new_panel(void *parent){
 void *types=fn<void*(*)(void*,uintptr_t)>(boot.runtime.array_new)(class_of("System","Type"),3);require(types,"IV panel types");
 void *components[]={class_type("UnityEngine","RectTransform"),class_type("UnityEngine.UI","Image"),layout_element_type()};
 for(int i=0;i<3;i++){void **slot=reinterpret_cast<void**>(static_cast<char*>(types)+32+i*8);fn<void(*)(void*,void**,void*)>(exp("gc_wbarrier_set_field"))(types,slot,components[i]);}
 void *panel=fn<void*(*)(void*)>(boot.runtime.object_new)(class_of("UnityEngine","GameObject"));require(panel,"IV panel");
 void *ctor_args[]={managed_string("PGRazorIv"),types};invoke(new_game_object,panel,ctor_args);
 bool keep_world=false;void *parent_args[]={parent,&keep_world};invoke(set_parent,call(panel,"get_transform"),parent_args);
 return panel;
}
// The PGRazorIv label of a cell: found under the CP label's parent, or built from the CP label.
Label iv_label(void *cp_text,bool create){
 void *cp_transform=call(cp_text,"get_transform");void *parent=call(cp_transform,"get_parent");require(parent,"CP label parent");
 Label out;
 void *find_args[]={managed_string("PGRazorIv")};
 if(void *found=invoke(find_child,parent,find_args)){
  require(unbox<int>(call(found,"get_childCount"))>=1,"IV label text");int first=0;void *index[]={&first};
  void *component_args[]={type_object(cp_text)};out.text=invoke(get_component,call(found,"GetChild",1,index),component_args);require(out.text,"IV label component");
  out.panel=call(found,"get_gameObject");return out;}
 if(!create)return out;
 out.panel=new_panel(parent);void *panel_transform=call(out.panel,"get_transform");
 void *image_type[]={class_type("UnityEngine.UI","Image")};void *image=invoke(get_component,panel_transform,image_type);require(image,"IV panel image");
 // Soft: the cells' own off-white, a little see-through so the Pokemon still shows behind it.
 float color[4]={247/255.f,254/255.f,244/255.f,0.85f};void *color_args[]={color};call(image,"set_color",1,color_args);
 bool hit=false;void *hit_args[]={&hit};call(image,"set_raycastTarget",1,hit_args);
 // Copying the text component copies its GameObject and returns the new component.
 void *copy_args[]={cp_text,panel_transform};void *text=invoke(instantiate,nullptr,copy_args);require(text&&klass(text)==klass(cp_text),"IV label copy");
 out.text=text;call(text,"set_raycastTarget",1,hit_args);
 void *copy_transform=call(text,"get_transform");
 set_vector(copy_transform,"set_anchorMin",0,0);set_vector(copy_transform,"set_anchorMax",1,1);
 set_vector(copy_transform,"set_pivot",0.5f,0.5f);set_vector(copy_transform,"set_sizeDelta",0,0);set_vector(copy_transform,"set_anchoredPosition",0,0);
 // The CP label's own children (its small "CP" prefix) are localized labels of their own: hide them.
 const int children=unbox<int>(call(copy_transform,"get_childCount"));require(children>=0&&children<=16,"IV label children");
 for(int i=0;i<children;i++){void *index[]={&i};void *child=call(copy_transform,"GetChild",1,index);
  bool off=false;void *active[]={&off};call(call(child,"get_gameObject"),"SetActive",1,active);}
 // One centered line, however long the text, with the percent colored by tier.
 if(void *m=optional_method(text,"set_supportRichText",1)){bool rich=true;void *a[]={&rich};invoke(m,text,a);}
 else if(void *m=optional_method(text,"set_richText",1)){bool rich=true;void *a[]={&rich};invoke(m,text,a);}
 if(void *m=optional_method(text,"set_horizontalOverflow",1)){int overflow=1;void *a[]={&overflow};invoke(m,text,a);}
 if(void *m=optional_method(text,"set_verticalOverflow",1)){int overflow=1;void *a[]={&overflow};invoke(m,text,a);}
 if(void *m=optional_method(text,"set_enableWordWrapping",1)){bool wrap=false;void *a[]={&wrap};invoke(m,text,a);}
 if(class_name(text)=="Text"){int middle_center=4;void *a[]={&middle_center};call(text,"set_alignment",1,a);}
 else if(void *m=optional_method(text,"set_alignment",1)){int center=0x202;void *a[]={&center};invoke(m,text,a);}
 return out;
}
// A smaller font than the CP label (smallest for the longest format), shrunk further when the line
// would be wider than the cell. The panel hugs the text: one line tall, so the Pokemon image stays
// close below, and a little wider.
void size_label(void *cell,void *cp_text,const Label &l,int m){
 const float scale=m==3?0.42f:0.55f;void *size=call(cp_text,"get_fontSize");const std::string size_type=class_name(size);
 float font=0;
 auto set_font=[&](float f){
  if(size_type=="Int32"){int v=int(f);if(v<8)v=8;font=float(v);void *a[]={&v};call(l.text,"set_fontSize",1,a);}
  else if(size_type=="Single"){float v=f<8?8:f;font=v;void *a[]={&v};call(l.text,"set_fontSize",1,a);}};
 if(size_type=="Int32")set_font(unbox<int>(size)*scale);else if(size_type=="Single")set_font(unbox<float>(size)*scale);
 if(font<=0)return;
 struct Rect{float x,y,w,h;};
 const float cell_width=unbox<Rect>(call(call(cell,"get_transform"),"get_rect")).w;
 float text_width=unbox<float>(call(l.text,"get_preferredWidth"));
 if(cell_width>0&&text_width+font*0.8f>cell_width*0.96f&&font>8){
  set_font(font*(cell_width*0.96f)/(text_width+font*0.8f));text_width=unbox<float>(call(l.text,"get_preferredWidth"));}
 float line=font*1.25f,width=text_width+font*0.8f;
 set_vector(call(l.panel,"get_transform"),"set_sizeDelta",width,line);
 void *layout_type[]={layout_element_type()};void *layout=invoke(get_component,call(l.panel,"get_transform"),layout_type);require(layout,"IV label layout");
 void *line_args[]={&line},*width_args[]={&width};
 call(layout,"set_preferredHeight",1,line_args);call(layout,"set_minHeight",1,line_args);
 call(layout,"set_preferredWidth",1,width_args);call(layout,"set_minWidth",1,width_args);
}
// The cell draws its children in order, and its CP header (WidgetHeader) comes before the Pokemon
// image and the badges drawn over it (ModifiersAbove), so they cover the IV line. (The shiny
// sparkles are in ModifiersBelow, already under the header.) The cell root has no layout group, so moving the header after them changes only what is
// drawn on top. The header stays before the cell's tap target, which is drawn last.
void draw_over_icon(void *cell,void *cp_text){
 void *root=call(cell,"get_transform");void *header=call(call(cp_text,"get_transform"),"get_parent");
 if(!header||call(header,"get_parent")!=root)return;
 int to=-1;
 for(const char *name:{"PokemonIcon","ModifiersAbove"}){
  void *find_args[]={managed_string(name)};
  if(void *child=invoke(find_child,root,find_args)){const int i=unbox<int>(call(child,"GetSiblingIndex"));if(i>to)to=i;}
 }
 if(to>=0&&unbox<int>(call(header,"GetSiblingIndex"))<to){void *a[]={&to};call(header,"SetSiblingIndex",1,a);}
}
void render(void *cell,void *pokemon){
 const int m=mode.load(std::memory_order_acquire);
 void *cp_text=reference_field(cell,"cpText");require(cp_text,"CP label");
 const Label l=iv_label(cp_text,m!=0&&pokemon);
 if(!l.panel)return;
 bool show=m!=0&&pokemon;void *active[]={&show};call(l.panel,"SetActive",1,active);
 if(!show)return;
 const int atk=field<int>(pokemon,PGR_GAME_QUICKBALL_METADATA_H_INDIVIDUALATTACK__NAME,"System.Int32");
 const int def=field<int>(pokemon,PGR_GAME_QUICKBALL_METADATA_H_INDIVIDUALDEFENSE__NAME,"System.Int32");
 const int sta=field<int>(pokemon,PGR_GAME_QUICKBALL_METADATA_H_INDIVIDUALSTAMINA__NAME,"System.Int32");
 require(atk>=0&&atk<=15&&def>=0&&def<=15&&sta>=0&&sta<=15,"IV range");
 const float multiplier=field<float>(pokemon,PGR_GAME_QUICKBALL_METADATA_H_CPMULTIPLIER__NAME,"System.Single")+
                        field<float>(pokemon,PGR_GAME_QUICKBALL_METADATA_H_ADDITIONALCPMULTIPLIER__NAME,"System.Single");
 void *args[]={managed_string(label(atk,def,sta,m,level_half(multiplier)))};call(l.text,"set_text",1,args);
 size_label(cell,cp_text,l,m);
 draw_over_icon(cell,cp_text);
}
void hook_initialize(void *self,void *initializer,const void *info){
 original.load(std::memory_order_acquire)(self,initializer,info);
 if(faulted.load(std::memory_order_acquire)||!self||!pokemon_arg)return;
 try{render(self,initializer?field<void*>(initializer,"PokemonProto","Holoholo.Rpc.PokemonProto"):nullptr);}
 catch(const Failure &f){faulted.store(true,std::memory_order_release);status("error",std::string("Storage IVs stopped: ")+f.where);}
 catch(...){faulted.store(true,std::memory_order_release);status("error","Storage IVs stopped: native failure");}
}

void install(){
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));require(api&&api->resolve_class&&api->validate_method,"metadata API");
 require(boot.module_base,"il2cpp module");
 PgrMetaClassQuery cq{};cq.version=1;cq.size=sizeof(cq);cq.name_space="Niantic.Holoholo.UI";cq.name="PokemonInventoryCellView";cq.canonical_match=PGR_MD_MATCH_EXACT;
 PgrMetaClassResult cr{};require(api->resolve_class(&cq,&cr)==0,"PokemonInventoryCellView");
 static const uint8_t prologue[]=PGR_GAME_STORAGE_IV_CPP_POKEMONINVENTORYCELLVIEW_INITIALIZE_FINGERPRINT;
 PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=cr.klass;q.name="Initialize";q.return_type="System.Void";q.return_match=PGR_MD_MATCH_EXACT;q.param_count=1;
 q.expected_code=static_cast<const char*>(boot.module_base)+PGR_GAME_STORAGE_IV_CPP_POKEMONINVENTORYCELLVIEW_INITIALIZE_RVA;q.prologue=prologue;q.prologue_len=sizeof(prologue);
 PgrMetaMethodResult r{};const int rc_validate=api->validate_method(&q,&r);
 if(rc_validate!=0||!r.code){
  // Name the signature the game has, so a changed method can be told from a moved one.
  static std::string why;why="Initialize signature (code "+std::to_string(rc_validate)+")";
  if(void *m=fn<void*(*)(void*,const char*,int)>(boot.meta.class_get_method_from_name)(const_cast<void*>(cr.klass),"Initialize",1))
   why+=" "+typename_of(fn<const void*(*)(void*)>(exp("method_get_return_type"))(m))+"("+typename_of(fn<const void*(*)(void*,unsigned)>(exp("method_get_param"))(m,0))+")";
  require(false,why.c_str());
 }
 const void *param=fn<const void*(*)(const void*,unsigned)>(exp("method_get_param"))(r.method,0);require(param,"Initialize parameter");
 const std::string types=typename_of(param);
 static std::string unexpected;unexpected="Initialize takes "+types;
 require(types=="Niantic.Holoholo.UI.PokemonInventoryCellView.Initializer"&&register_sized(param),unexpected.c_str());
 pokemon_arg=true;
 instantiate=overload("UnityEngine","Object","Instantiate","UnityEngine.Object","UnityEngine.Transform");
 get_component=overload("UnityEngine","Component","GetComponent","System.Type");
 new_game_object=overload("UnityEngine","GameObject",".ctor","System.String","System.Type[]");
 set_parent=overload("UnityEngine","Transform","SetParent","UnityEngine.Transform","System.Boolean");
 find_child=overload("UnityEngine","Transform","Find","System.String");
 void *orig=nullptr;const int rc=pgrazor_hook(const_cast<void*>(r.code),reinterpret_cast<void*>(hook_initialize),&orig);
 if(orig)original.store(reinterpret_cast<Init>(orig),std::memory_order_release);
 require(rc==0&&orig,"Initialize hook");
 installed.store(true,std::memory_order_release);status("ready","Storage IVs ready ("+types+")");
}
}  // namespace

void pgr_storage_iv_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata){
 if(busy||!tid||tid!=uint64_t(gettid())||(owner&&owner!=tid))return;
 struct Guard{Guard(){busy=true;}~Guard(){busy=false;}}guard;owner=tid;md=metadata;
 const int m=configured_mode();mode.store(m,std::memory_order_release);
 if(m&&installed.load(std::memory_order_acquire)&&!faulted.load(std::memory_order_acquire))tick_levels();
 if(!m||installed.load(std::memory_order_acquire)||faulted.load(std::memory_order_acquire))return;
 try{
  if(!ready){auto api=static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));boot.version=1;boot.size=sizeof(boot);require(api&&api->snapshot(&boot)==0,"Runtime unavailable");ready=true;}
  install();
 }catch(const Failure &f){faulted.store(true,std::memory_order_release);status("error",std::string("Storage IVs unavailable: ")+f.where);}
 catch(...){faulted.store(true,std::memory_order_release);status("error","Storage IVs unavailable: native failure");}
}
void pgr_storage_iv_stop(){mode.store(0,std::memory_order_release);}

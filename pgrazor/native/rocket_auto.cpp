#include "rocket_auto.h"
#include "rocket_auto_policy.h"
#include "rocket_auto_receiver.h"
#include "rocket_auto_buttons.h"
#include "runtime_transport.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
namespace pgr_rocket_auto {
namespace {
PgrBootBinding b{};pgr_rocket_combat_config_v1 rc{};
const PgrMetaApi *md=nullptr;
const auto tr=static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));
std::atomic<uint64_t> owner{0},spin_until{0};std::atomic<uint32_t> owned{0};std::atomic<bool> stopping{false};
bool installed=false,ready=false,busy=false,fault=false,capture_ready=false;uint64_t command=0,last_status=0;unsigned last_mask=0;
Policy policy;std::string error,last_message;
template<class T>T fn(void*p){return reinterpret_cast<T>(p);}
uint64_t now_ms(){timespec t{};return clock_gettime(CLOCK_BOOTTIME,&t)?0:uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
void emit(const char*kind,const char*text){char json[512];std::snprintf(json,sizeof(json),"{\"kind\":\"%s\",\"backend\":\"native\",\"reason\":\"%s\",\"boottimeMs\":%llu}",kind,text,(unsigned long long)now_ms());const char channel[]="rocket-auto";PgrRuntimeTransportString args[]={{channel,sizeof(channel)-1},{json,uint32_t(std::strlen(json))}};tr->emit(19,nullptr,0,args,2);}
void status(const char*text){if(last_message==text)return;last_message=text;PgrRuntimeTransportString s{text,uint32_t(std::strlen(text))};tr->emit(47,nullptr,0,&s,1);emit("status",text);}
struct Root {
 uintptr_t h=0;Root()=default;explicit Root(void*p){if(p){h=fn<uintptr_t(*)(void*,int)>(b.runtime.gchandle_new)(p,1);if(h)++owned;else{fault=true;error="gchandle-create-failed";}}}
 Root(const Root&)=delete;Root&operator=(const Root&)=delete;Root(Root&&v):h(std::exchange(v.h,0)){};Root&operator=(Root&&v){if(this!=&v){reset();h=std::exchange(v.h,0);}return *this;}
 void reset(){auto old=std::exchange(h,0);if(old){fn<void(*)(uintptr_t)>(b.runtime.gchandle_free)(old);--owned;}}
 ~Root(){reset();}void*get()const{if(!h)return nullptr;void*p=fn<void*(*)(uintptr_t)>(b.runtime.gchandle_get_target)(h);if(!p){fault=true;error="gchandle-target-null";}return p;}uint64_t key()const{return reinterpret_cast<uintptr_t>(get());}
};
struct Meta {
 void *explore=nullptr,*stop=nullptr,*dialog=nullptr,*npc=nullptr,*party=nullptr,*result=nullptr,*intro=nullptr,*spin=nullptr,*encounter=nullptr,*interaction=nullptr,*button=nullptr,*gui=nullptr,*modal=nullptr;
 void *active=nullptr,*gui_state=nullptr,*component=nullptr,*game_object=nullptr,*dialog_containers[2]{};
 const void *find=nullptr,*enabled=nullptr,*alive=nullptr,*visible=nullptr,*incident=nullptr,*character=nullptr,*selected=nullptr,*dialog_next=nullptr,*npc_next=nullptr,*submit=nullptr,*rescue=nullptr,*done=nullptr,*challenge=nullptr,*exit=nullptr,*in_play=nullptr,*ball=nullptr,*ball_type=nullptr,*select=nullptr,*kids=nullptr,*go_kids=nullptr,*on_click=nullptr,*fire=nullptr,*obj_name=nullptr;
 void *type_object=nullptr,*array_length=nullptr,*next_field=nullptr,*field_name=nullptr,*static_value=nullptr;
} m;
std::unordered_map<int,std::string> characters;
void *klass(const char*canonical){std::string full=canonical;auto split=full.rfind('.');if(split==std::string::npos)return nullptr;auto ns=full.substr(0,split),name=full.substr(split+1);PgrMetaClassQuery q{1,sizeof(q),ns.c_str(),name.c_str(),canonical,PGR_MD_MATCH_EXACT,0};PgrMetaClassResult r{};return md->resolve_class(&q,&r)?nullptr:const_cast<void*>(r.klass);}
std::string type_name(void*t){if(!t)return {};char*s=fn<char*(*)(void*)>(b.meta.type_get_name)(t);std::string name=s?s:"";if(s)fn<void(*)(void*)>(b.meta.mem_free)(s);return name;}
void *field(void*k,const char*n,const char*type){if(!k)return nullptr;auto*f=fn<void*(*)(void*,const char*)>(b.meta.class_get_field_from_name)(k,n);if(!f||fn<uint32_t(*)(void*)>(b.meta.field_get_flags)(f)&0x50)return nullptr;return type_name(fn<void*(*)(void*)>(b.meta.field_get_type)(f))==type?f:nullptr;}
const void *method(void*k,const char*n,const char*ret,unsigned count=0,const char*p0=nullptr,const char*p1=nullptr,const char*p2=nullptr,bool is_static=false){if(!k)return nullptr;PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=k;q.name=n;q.return_type=ret;q.return_match=PGR_MD_MATCH_EXACT;q.param_count=count;q.param_types[0]=p0;q.param_types[1]=p1;q.param_types[2]=p2;for(unsigned i=0;i<count;i++)q.param_match[i]=q.param_types[i]?PGR_MD_MATCH_EXACT:PGR_MD_MATCH_ANY;q.is_static=is_static?1:0;
 // A ButtonClickedEvent inherits Invoke from UnityEvent; capture helpers can
 // also be inherited. Validate the actual declaring class and its relation.
 auto*raw=fn<void*(*)(void*,const char*,int)>(b.meta.class_get_method_from_name)(k,n,int(count));
 if(raw){auto*parent=fn<void*(*)(void*)>(b.meta.method_get_class)(raw);if(parent&&parent!=k&&fn<bool(*)(void*,void*)>(b.meta.class_is_assignable_from)(parent,k))q.owner=parent;}
 PgrMetaMethodResult r{};return md->validate_method(&q,&r)?nullptr:r.method;}
const void *children_method(void*k){if(!k)return nullptr;void*it=nullptr;for(unsigned i=0;i<512;i++){auto*mi=fn<void*(*)(void*,void**)>(b.meta.class_get_methods)(k,&it);if(!mi)break;auto*n=fn<const char*(*)(void*)>(b.meta.method_get_name)(mi);if(!n||std::strcmp(n,"GetComponentsInChildren")||fn<bool(*)(void*)>(b.meta.method_is_generic)(mi)||fn<bool(*)(void*)>(b.meta.method_is_inflated)(mi)||fn<uint32_t(*)(void*)>(b.meta.method_get_param_count)(mi)!=2||fn<void*(*)(void*)>(b.meta.method_get_class)(mi)!=k)continue;
 uint32_t iflags=0;if(fn<uint32_t(*)(void*,uint32_t*)>(b.meta.method_get_flags)(mi,&iflags)&0x10)continue;
 auto param=fn<void*(*)(void*,uint32_t)>(b.meta.method_get_param);if(type_name(param(mi,0))!="System.Type"||type_name(param(mi,1))!="System.Boolean"||type_name(fn<void*(*)(void*)>(b.meta.method_get_return_type)(mi))!="UnityEngine.Component[]")continue;
 void*info[3]{};if(!fn<bool(*)(void*,void*)>(b.meta.debug_get_method_info)(mi,info)||!info[0])continue;PgrMetaCodeQuery q{1,sizeof(q),info[0],4,nullptr,0,0};if(!md->validate_code(&q))return mi;
 }return nullptr;}
void *exported(const char*n){void*p=dlsym(b.handle,n);PgrMetaCodeQuery q{1,sizeof(q),p,4,nullptr,0,0};return p&&!md->validate_code(&q)?p:nullptr;}
struct Config {uint64_t command=0;unsigned mask=0;bool valid=false;};
Config config(){Config c{};if(!ready||stopping)return c;PgrRuntimeTransportSample a{},z{};int active=0,allowed=0;int32_t raw=0;char text[128]{};uint32_t len=0;unsigned long long token=0;unsigned mask=0,on=0;int end=0;
 if(tr->sample(&a)||!a.snapshot_fresh||a.fault||tr->get_bool(3,&active)||!active||tr->get_bool(26,&allowed)||!allowed||tr->get_int(6,&raw)||raw<0||raw>63||!(raw&8)||tr->get_string(13,text,sizeof(text)-1,&len)||len>=sizeof(text))return c;
 text[len]=0;if(std::sscanf(text,"v1:%llu:%u:%u%n",&token,&mask,&on,&end)!=3||end!=int(len)||mask!=unsigned(raw)||on!=1||token>INT64_MAX)return c;
 if(tr->sample(&z)||!z.snapshot_fresh||z.fault||a.snapshot_seq!=z.snapshot_seq||a.generation!=z.generation)return c;
 return {uint64_t(token),mask,true};}
bool authorized(const Config &c){auto fresh=config();return !fault&&c.valid&&fresh.valid&&c.command==fresh.command&&c.mask==fresh.mask&&!stopping;}
Root invoke(const void*method,const Root*o,void**params=nullptr){if(!method){fault=true;error="missing-method";return Root();}void*exc=nullptr;void*self=o?o->get():nullptr;if(o&&!self){fault=true;error="invoke-target-null";return Root();}if(o&&!receiver_matches(method,self,fn<MethodClass>(b.meta.method_get_class),fn<ObjectClass>(b.runtime.object_get_class),fn<Assignable>(b.meta.class_is_assignable_from))){fault=true;error="receiver-type-mismatch:";auto*n=fn<const char*(*)(const void*)>(b.meta.method_get_name)(method);if(n)error+=std::string(n).substr(0,80);return Root();}void*p=fn<void*(*)(const void*,void*,void**,void**)>(b.runtime.runtime_invoke)(method,self,params,&exc);if(exc){fault=true;auto*n=fn<const char*(*)(const void*)>(b.meta.method_get_name)(method);error="managed-exception:";if(n)error+=std::string(n).substr(0,80);return Root();}return Root(p);}
bool boolean(const void*method,const Root*o,void**params=nullptr){auto box=invoke(method,o,params);if(!box.get()||fn<void*(*)(void*)>(b.runtime.object_get_class)(box.get())!=rc.class_bool){if(!fault){fault=true;error="invalid-boolean";}return false;}auto*p=fn<void*(*)(void*)>(b.runtime.object_unbox)(box.get());if(!p){fault=true;return false;}return *static_cast<uint8_t*>(p)==1;}
template<class T>T read(const Root&o,void*f){T result{};if(o.get()&&f)fn<void(*)(void*,void*,void*)>(b.runtime.field_get_value)(o.get(),f,&result);return result;}
bool exact(const Root&o,void*k){return o.get()&&k&&fn<void*(*)(void*)>(b.runtime.object_get_class)(o.get())==k;}
bool alive(const Root&o){void*value=o.get();void*args[]={value};return value&&boolean(m.alive,nullptr,args);}
bool showing(const Root&o){if(!alive(o)||!boolean(m.enabled,&o))return false;auto*k=fn<void*(*)(void*)>(b.runtime.object_get_class)(o.get());if(m.gui&&m.gui_state&&fn<bool(*)(void*,void*)>(b.meta.class_is_assignable_from)(m.gui,k))return read<int32_t>(o,m.gui_state)==2;return true;}
template<class F>Root scan(void*k,F accept){if(!k||fault)return {};Root type(fn<void*(*)(void*)>(m.type_object)(fn<void*(*)(void*)>(b.meta.class_get_type)(k)));if(!type.get())return {};void*t=type.get();void*args[]={t};auto array=invoke(m.find,nullptr,args);if(!array.get())return {};auto count=fn<uintptr_t(*)(void*)>(m.array_length)(array.get());if(count>4096){fault=true;error="object-scan-limit";return {};}
 for(uintptr_t i=0;i<count&&!fault;i++){void*p=nullptr;std::memcpy(&p,static_cast<char*>(array.get())+32+i*8,8);Root candidate(p);if(exact(candidate,k)&&alive(candidate)&&accept(candidate))return candidate;}return {};}
Root visible(void*k){return scan(k,[](const Root&o){return showing(o);});}
std::string object_name(const Root&o){auto value=invoke(m.obj_name,&o);if(!value.get())return {};int n=0;std::memcpy(&n,static_cast<char*>(value.get())+16,4);if(n<0||n>256)return {};std::string text;for(int i=0;i<n;i++){uint16_t u=0;std::memcpy(&u,static_cast<char*>(value.get())+20+2*i,2);if(u<128)text.push_back(char(u));}return text;}
std::vector<Root> children(const Root&o,const void*kids){std::vector<Root> out;if(!kids||!m.button)return out;Root type(fn<void*(*)(void*)>(m.type_object)(fn<void*(*)(void*)>(b.meta.class_get_type)(m.button)));uint8_t inactive=0;void*t=type.get();void*args[]={t,&inactive};auto array=invoke(kids,&o,args);if(!array.get())return out;auto count=fn<uintptr_t(*)(void*)>(m.array_length)(array.get());if(count>32){error="button-limit";return out;}out.reserve(count);for(uintptr_t i=0;i<count;i++){void*p=nullptr;std::memcpy(&p,static_cast<char*>(array.get())+32+i*8,8);Root button(p);if(button.get()&&fn<bool(*)(void*,void*)>(b.meta.class_is_assignable_from)(m.button,fn<void*(*)(void*)>(b.runtime.object_get_class)(button.get()))&&showing(button)&&boolean(rc.m_interactable,&button))out.push_back(std::move(button));}return out;}
Root named_battle(const std::vector<Root>&buttons){
 BattleChoice choice;
 for(auto &button:buttons)choice.add(button.key(),battle_label(object_name(button).c_str()));
 if(choice.selected())for(auto &button:buttons)if(button.key()==choice.selected())return Root(button.get());
 return {};
}
Root screen_battle(const Root&screen){
 if(!m.kids||!m.obj_name)return {};
 auto buttons=children(screen,m.kids);auto selected=named_battle(buttons);
 static uint64_t report=0;auto now=now_ms();
 if(!selected.get()&&now>=report){report=now+30000;emit("battle-button-wait",buttons.empty()?"no-active-interactable-buttons-in-rocket-screen":"no-unique-battle-button-in-rocket-screen");}
 return selected;
}
Root battle_button(const Root&dialog){
 std::vector<Root> all;
 for(auto*f:m.dialog_containers){if(!f)continue;Root holder(read<void*>(dialog,f));if(!holder.get())continue;auto*k=fn<void*(*)(void*)>(b.runtime.object_get_class)(holder.get());
  const void*kids=m.component&&fn<bool(*)(void*,void*)>(b.meta.class_is_assignable_from)(m.component,k)?m.kids:m.go_kids;
  auto buttons=children(holder,kids);for(auto &button:buttons){bool duplicate=false;for(auto &old:all)if(old.get()==button.get())duplicate=true;if(!duplicate)all.push_back(std::move(button));}}
 if(all.size()==1)return std::move(all[0]);
 auto selected=named_battle(all);if(selected.get())return selected;
 // Only the known Rocket screen's descendants, never unrelated screen buttons.
 return screen_battle(dialog);
}
bool leader_name(std::string name){for(char&c:name)if(c>='a'&&c<='z')c-=32;for(const char*term:{"EXECUTIVE","GIOVANNI","LEADER","BOSS","CLIFF","ARLO","SIERRA","UNSET"})if(name.find(term)!=std::string::npos)return true;return false;}
bool stop_allowed(const Root&o,unsigned mask,bool &leader){leader=true;if(!m.character)return (mask&16)!=0;auto box=invoke(m.character,&o);if(!box.get())return false;auto*p=fn<void*(*)(void*)>(b.runtime.object_unbox)(box.get());if(!p)return false;int value=0;std::memcpy(&value,p,4);auto it=characters.find(value);if(it==characters.end())return (mask&16)!=0;leader=leader_name(it->second);return (mask&16)||!leader;}
bool setup(){
 md=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));if(!md)return false;
 m.explore=klass("Niantic.Holoholo.Map.MapExploreState");m.stop=klass("Niantic.Holoholo.Pokestop.MapPokestop");m.dialog=klass("Niantic.Holoholo.Incident.IncidentInvasionDialogGui");m.npc=klass("Niantic.Holoholo.UI.Conversations.NpcDialogueGuiController");m.party=klass("Niantic.Holoholo.Combat.PokemonPartyState.CombatPokemonPartyWidget");m.result=klass("Niantic.Holoholo.Incident.IncidentCombatResolveGuiController");m.intro=klass("Niantic.Holoholo.Combat.NpcTrainers.NpcTrainerInterstitialGui");m.spin=klass("Niantic.Holoholo.Pokestop.PokestopSpinnerInteractionGuiController");m.encounter=klass("Niantic.Holoholo.Encounter.IncidentEncounter");m.interaction=klass("Niantic.Holoholo.Encounter.EncounterInteractionState");m.button=klass("UnityEngine.UI.Button");m.gui=klass("Niantic.Holoholo.NUI.GuiController");m.modal=klass("Niantic.Holoholo.UI.ModalDialog");
 auto*object=klass("UnityEngine.Object"),*behaviour=klass("UnityEngine.Behaviour"),*state=klass("Niantic.Zeppelin.GameStates.GameState"),*ball=klass("Niantic.Holoholo.Encounter.Pokeball"),*component=klass("UnityEngine.Component"),*go=klass("UnityEngine.GameObject");
 m.component=component;m.game_object=go;
 m.active=field(state,"<IsActiveState>k__BackingField","System.Boolean");
 if(!state||!m.explore||!fn<bool(*)(void*,void*)>(b.meta.class_is_assignable_from)(state,m.explore))m.active=nullptr;
 // Resolve the GUI enum by metadata, never by a guessed object offset.
 if(m.gui){auto*f=fn<void*(*)(void*,const char*)>(b.meta.class_get_field_from_name)(m.gui,"currentState");if(f){auto*k=fn<void*(*)(void*)>(b.meta.class_from_type)(fn<void*(*)(void*)>(b.meta.field_get_type)(f));int align=0;if(k&&fn<bool(*)(void*)>(b.meta.class_is_enum)(k)&&fn<int(*)(void*,int*)>(b.meta.class_value_size)(k,&align)==4)m.gui_state=f;}}
 m.find=method(object,"FindObjectsOfType","UnityEngine.Object[]",1,"System.Type",nullptr,nullptr,true);m.alive=rc.m_alive;m.enabled=method(behaviour,"get_isActiveAndEnabled","System.Boolean");
 m.visible=method(m.explore,"get_IsActiveAndVisible","System.Boolean");m.incident=method(m.stop,"get_IsIncidentActiveAndInRange","System.Boolean");
 m.character=method(m.stop,"get_NPCCharacter",nullptr);if(m.character){auto*type=fn<void*(*)(const void*)>(b.meta.method_get_return_type)(m.character);auto*k=fn<void*(*)(void*)>(b.meta.class_from_type)(type);if(!k||!fn<bool(*)(void*)>(b.meta.class_is_enum)(k)||type_name(fn<void*(*)(void*)>(b.meta.class_enum_basetype)(k))!="System.Int32")m.character=nullptr;}
 const char*stop_type="Niantic.Holoholo.Pokestop.IMapPokestop";
 m.selected=method(m.explore,"PokestopSelected","System.Void",3,stop_type,nullptr,"System.Boolean");if(!m.selected)m.selected=method(m.explore,"PokestopSelected","System.Void",3,"Niantic.Holoholo.Pokestop.MapPokestop",nullptr,"System.Boolean");
 if(m.selected&&type_name(fn<void*(*)(const void*,uint32_t)>(b.meta.method_get_param)(m.selected,1)).rfind("System.Action",0)!=0)m.selected=nullptr;
 m.dialog_next=method(m.dialog,"ClickedToContinue","System.Void");m.npc_next=method(m.npc,"ClickedToContinue","System.Void");m.submit=method(m.party,"SubmitButtonPressed","System.Void");m.rescue=method(m.result,"StartEncounterButtonClicked","System.Void");m.done=method(m.result,"DoneButtonClicked","System.Void");m.challenge=method(m.intro,"PressChallenge","System.Void");m.exit=method(m.spin,"ClickExit","System.Void");
 m.in_play=method(m.interaction,"get_PokeballInPlay","System.Boolean");m.ball=method(m.interaction,"get_Pokeball",nullptr);m.ball_type=method(ball,"get_BallType","Holoholo.Rpc.Item");m.select=method(m.interaction,"SelectPokeball","System.Void",1,"Holoholo.Rpc.Item");
 // The getter may declare an interface/base class rather than concrete Pokeball.
 // Validate reference compatibility, then validate each returned object's receiver
 // before invoking get_BallType. Never unbox an arbitrary return as a ball.
 if(m.ball){auto*t=fn<void*(*)(const void*)>(b.meta.method_get_return_type)(m.ball);auto*k=t?fn<void*(*)(void*)>(b.meta.class_from_type)(t):nullptr;
  auto name=type_name(t);emit("capture-ball-return",name.c_str());
  bool compatible=reference_return_matches(k,ball,fn<ValueType>(b.meta.class_is_valuetype),fn<Assignable>(b.meta.class_is_assignable_from));
  if(!compatible){m.ball=nullptr;emit("capture-unavailable","get_Pokeball-incompatible-reference-return");}}
 if(m.ball_type){auto*k=fn<void*(*)(void*)>(b.meta.class_from_type)(fn<void*(*)(const void*)>(b.meta.method_get_return_type)(m.ball_type));if(!k||!fn<bool(*)(void*)>(b.meta.class_is_enum)(k)||type_name(fn<void*(*)(void*)>(b.meta.class_enum_basetype)(k))!="System.Int32")m.ball_type=nullptr;}
 m.type_object=exported("il2cpp_type_get_object");m.array_length=exported("il2cpp_array_length");m.next_field=exported("il2cpp_class_get_fields");m.field_name=exported("il2cpp_field_get_name");m.static_value=exported("il2cpp_field_static_get_value");
 if(m.character&&m.next_field&&m.field_name&&m.static_value){auto*k=fn<void*(*)(void*)>(b.meta.class_from_type)(fn<void*(*)(const void*)>(b.meta.method_get_return_type)(m.character));void*it=nullptr;for(unsigned i=0;i<4096;i++){auto*f=fn<void*(*)(void*,void**)>(m.next_field)(k,&it);if(!f)break;if((fn<uint32_t(*)(void*)>(b.meta.field_get_flags)(f)&0x8050)!=0x8050)continue;int value=0;fn<void(*)(void*,void*)>(m.static_value)(f,&value);auto*n=fn<const char*(*)(void*)>(m.field_name)(f);if(n&&std::strlen(n)<256)characters[value]=n;}}
 m.kids=children_method(component);m.go_kids=children_method(go);m.obj_name=method(object,"get_name","System.String");m.on_click=method(m.button,"get_onClick",nullptr);if(m.on_click){auto*k=fn<void*(*)(void*)>(b.meta.class_from_type)(fn<void*(*)(const void*)>(b.meta.method_get_return_type)(m.on_click));m.fire=method(k,"Invoke","System.Void");}
 for(int i=0;i<2;i++){auto*f=m.dialog?fn<void*(*)(void*,const char*)>(b.meta.class_get_field_from_name)(m.dialog,i?"buttonContainer":"pgoButtonContainer"):nullptr;if(!f||(fn<uint32_t(*)(void*)>(b.meta.field_get_flags)(f)&0x50))continue;auto*k=fn<void*(*)(void*)>(b.meta.class_from_type)(fn<void*(*)(void*)>(b.meta.field_get_type)(f));if(k&&((component&&fn<bool(*)(void*,void*)>(b.meta.class_is_assignable_from)(component,k))||(go&&fn<bool(*)(void*,void*)>(b.meta.class_is_assignable_from)(go,k))))m.dialog_containers[i]=f;}
 capture_ready=m.encounter&&m.interaction&&m.in_play&&m.ball&&m.ball_type&&m.select&&state&&fn<bool(*)(void*,void*)>(b.meta.class_is_assignable_from)(state,m.interaction);
 const char*capture_missing=!m.encounter?"IncidentEncounter":!m.interaction?"EncounterInteractionState":!m.in_play?"PokeballInPlay":!m.ball?"Pokeball":!m.ball_type?"BallType-Item-enum":!m.select?"SelectPokeball-Item-enum":!capture_ready?"interaction-state-inheritance":nullptr;
 if(capture_missing)emit("capture-unavailable",capture_missing);else emit("capture-ready","Premier-Ball-screen-swipe");
 const char*missing=!m.explore?"MapExploreState":!m.stop?"MapPokestop":!m.dialog?"IncidentInvasionDialogGui":!m.party?"CombatPokemonPartyWidget":!m.result?"IncidentCombatResolveGuiController":!m.active?"active-state-field":!m.find?"FindObjectsOfType":!m.enabled?"Behaviour.isActiveAndEnabled":!m.alive?"UnityEngine.Object.op_Implicit":!m.type_object||!m.array_length?"array-type-exports":!m.visible||!m.incident||!m.selected?"map-contract":!m.dialog_next||!m.submit||!m.rescue||!m.done?"flow-contract":nullptr;
 if(missing){error=missing;emit("unavailable",missing);return false;}emit("ready",capture_ready?"flow-and-capture":"flow-ready-capture-unavailable");return true;
}
void call_action(const Config&c,const void*method,const Root&o,void**args=nullptr){if(!authorized(c)||!o.get())return;auto*n=fn<const char*(*)(const void*)>(b.meta.method_get_name)(method);if(n)emit("action-before",n);auto result=invoke(method,&o,args);if(n)emit(fault?"action-failed":"action-after",n);}
}
void install(const PgrBootBinding &binding,const pgr_rocket_combat_config_v1 &config){if(installed)return;b=binding;rc=config;installed=true;ready=setup();}
void tick(uint64_t tid,bool rocket_scope,bool rocket_battle,bool terminal){if(!installed||!tid||uint64_t(gettid())!=tid)return;uint64_t zero=0;owner.compare_exchange_strong(zero,tid);if(owner!=tid||busy)return;busy=true;struct Guard{~Guard(){busy=false;}} guard;
 if(terminal||stopping){policy.suspend();spin_until=0;stopping=true;return;}
 if(!ready){int32_t raw=0;if(!tr->get_int(6,&raw)&&(raw&8)){std::string text="Full auto unavailable: "+error;status(text.c_str());}return;}
 auto c=config();if(!c.valid){int32_t raw=0;if(!tr->get_int(6,&raw)&&raw>=0&&raw<=63&&!(raw&8)){policy.suspend();spin_until=0;last_mask=unsigned(raw);}return;}
 // A transient snapshot change or opening the panel revokes actions immediately
 // without losing an already-owned rescue context. Off/mask changes reset flow.
 if(c.command!=command||c.mask!=last_mask){bool restart=c.mask!=last_mask||fault;command=c.command;last_mask=c.mask;if(restart){fault=false;policy.suspend();}}
 if(fault){status("Full auto paused: managed call failed; toggle Off/On");return;}
 auto now=now_ms();if(now<policy.next)return;
 if(!policy.flow(now)&&!rocket_scope&&now<policy.map_next)return;

 Frame f{};f.now=now;f.mask=c.mask;f.authorized=true;f.rocket_scope=rocket_scope;f.rocket_battle=rocket_battle;f.capture_supported=capture_ready;
 Root map,stop,spin,intro,intro_button,dialog,button,npc,party,result,encounter,interaction,ball,modal,radar;
 dialog=!rocket_battle?visible(m.dialog):Root();if(dialog.get()&&(m.kids||m.go_kids)&&m.on_click&&m.fire)button=battle_button(dialog);
 intro=!rocket_battle&&!policy.combat_seen?visible(m.intro):Root();if(intro.get()&&!m.challenge&&m.on_click&&m.fire)intro_button=screen_battle(intro);spin=m.exit&&policy.exit_pending?visible(m.spin):Root();
 bool in_flow=policy.flow(now)||rocket_scope;
 if(in_flow&&!policy.combat_seen){npc=m.npc_next?visible(m.npc):Root();party=visible(m.party);}
 result=visible(m.result);
 if(in_flow&&!rocket_battle&&capture_ready){encounter=visible(m.encounter);if(encounter.get()){interaction=scan(m.interaction,[](const Root&o){return read<uint8_t>(o,m.active)==1;});if(interaction.get()){f.ball_in_play=boolean(m.in_play,&interaction);if(!f.ball_in_play){ball=invoke(m.ball,&interaction);if(ball.get()){auto value=invoke(m.ball_type,&ball);if(value.get()){auto*p=fn<void*(*)(void*)>(b.runtime.object_unbox)(value.get());if(p)std::memcpy(&f.ball_type,p,4);}}}}}}
 if(in_flow){static uint64_t capture_at=0;static unsigned last_capture=~0u;unsigned scene=(capture_ready?1:0)|(encounter.get()?2:0)|(interaction.get()?4:0)|(ball.get()?8:0)|(f.ball_in_play?16:0);
  if(scene!=last_capture||now>=capture_at){last_capture=scene;capture_at=now+30000;char text[180];std::snprintf(text,sizeof(text),"ready=%d encounter=%d interaction=%d ball=%d inPlay=%d ballType=%d",capture_ready?1:0,!!encounter.get(),!!interaction.get(),!!ball.get(),f.ball_in_play?1:0,f.ball_type);emit("capture-state",text);}}
 if(in_flow&&!rocket_battle&&(c.mask&16)&&m.modal&&m.kids&&m.on_click&&m.fire&&policy.battle_at&&now-policy.battle_at<15000){modal=visible(m.modal);if(modal.get()){auto buttons=children(modal,m.kids);unsigned defaults=0,alts=0;for(auto&item:buttons){auto name=object_name(item);if(name=="DefaultButton"){++defaults;radar=Root(item.get());}if(name=="AltButton")++alts;}if(defaults!=1||alts!=1)radar.reset();}}
 if(!in_flow&&now>=policy.map_next){map=scan(m.explore,[](const Root&o){return read<uint8_t>(o,m.active)==1&&boolean(m.visible,&o);});if(map.get())stop=scan(m.stop,[&](const Root&o){bool leader=true;if(policy.stop_tried(o.key(),now)||!boolean(m.incident,&o)||!stop_allowed(o,c.mask,leader))return false;f.leader=leader;return true;});}
 f.map=map.key();f.stop=stop.key();f.spin=spin.key();f.intro=(m.challenge||intro_button.get())?intro.key():0;f.dialog=dialog.key();f.dialog_button=button.key();f.npc=npc.key();f.party=party.key();f.result=result.key();f.encounter=encounter.key();f.interaction=interaction.key();f.ball=ball.key();f.radar=radar.key();
 static uint64_t scene_at=0;static unsigned last_scene=~0u;unsigned scene=(dialog.get()?1:0)|(button.get()?2:0)|(intro.get()?4:0)|(party.get()?8:0)|(m.challenge?16:0)|(intro_button.get()?32:0)|(rocket_scope?64:0)|(rocket_battle?128:0);
 if((in_flow||dialog.get())&&(scene!=last_scene||now>=scene_at)){last_scene=scene;scene_at=now+30000;char detail[240];std::snprintf(detail,sizeof(detail),"dialog=%d battleButton=%d intro=%d party=%d challengeMethod=%d introButton=%d rocketScope=%d combatUi=%d childrenMethod=%d clickMethod=%d invokeMethod=%d",!!dialog.get(),!!button.get(),!!intro.get(),!!party.get(),!!m.challenge,!!intro_button.get(),rocket_scope?1:0,rocket_battle?1:0,!!m.kids,!!m.on_click,!!m.fire);emit("screens",detail);}
 if(!authorized(c)){if(fault)emit("error",error.empty()?"managed-read":error.c_str());return;}auto plan=policy.step(f);if(rocket_battle)policy.next=now+1000;if(plan.action==None){if(now-last_status>=5000){last_status=now;status(rocket_battle?"Full auto: Rocket battle":policy.flow(now)?"Full auto: flow in progress":map.get()?"Full auto: watching invaded stops in range":"Full auto: waiting for the map");}return;}
 switch(plan.action){
 case OpenStop:{if(!map.get()||!stop.get()||!boolean(m.visible,&map)||!boolean(m.incident,&stop))break;bool leader=true;if(!stop_allowed(stop,c.mask,leader))break;void*o=stop.get();uint8_t no=0;void*args[]={o,nullptr,&no};spin_until=now+20000;call_action(c,m.selected,map,args);status("Full auto: opening an invaded PokeStop");break;}
 case ExitStop:if(showing(spin)){spin_until=now+3000;call_action(c,m.exit,spin);status("Full auto: leaving the stop without spinning");}break;
 case Challenge:if(showing(intro)){if(m.challenge)call_action(c,m.challenge,intro);else if(intro_button.get()&&showing(intro_button)&&boolean(rc.m_interactable,&intro_button)){auto on_click=invoke(m.on_click,&intro_button);call_action(c,m.fire,on_click);}status("Full auto: starting Rocket battle");}break;
 case DialogBattle:case Radar:{Root &target=plan.action==Radar?radar:button;if(showing(target)&&boolean(rc.m_interactable,&target)){auto on_click=invoke(m.on_click,&target);call_action(c,m.fire,on_click);status(plan.action==Radar?"Full auto: using Rocket Radar":"Full auto: pressing Battle");}break;}
 case DialogNext:if(showing(dialog)){call_action(c,m.dialog_next,dialog);status("Full auto: advancing Rocket dialogue");}break;
 case NpcNext:if(showing(npc)){call_action(c,m.npc_next,npc);status("Full auto: advancing NPC dialogue");}break;
 case Submit:if(showing(party)){call_action(c,m.submit,party);status("Full auto: submitting your selected party");}break;
 case Rescue:if(showing(result)){call_action(c,m.rescue,result);status("Full auto: rescuing Shadow Pokemon");}break;
 case Done:if(showing(result)){call_action(c,m.done,result);status("Full auto: returning to map");}break;
 case SelectPremier:{int id=5;void*args[]={&id};if(read<uint8_t>(interaction,m.active)==1&&!boolean(m.in_play,&interaction)){call_action(c,m.select,interaction,args);status("Full auto: selecting Premier Ball");}break;}
 case Swipe:{if(read<uint8_t>(interaction,m.active)==1&&!boolean(m.in_play,&interaction)&&authorized(c)){int64_t values[]={int64_t(c.command),int64_t(now_ms()+200)};if(tr->emit(59,values,2,nullptr,0)){fault=true;error="swipe-transport";}else status("Full auto: throwing Premier Ball");}break;}
 default:break;
 }
 if(fault){spin_until=0;emit("error",error.empty()?"managed-call":error.c_str());status("Full auto paused: managed call failed; toggle Off/On");}
}
void stop(uint64_t){stopping=true;spin_until=0;}
uint32_t roots(){return owned.load();}
bool spin_blocked(){return now_ms()<spin_until.load()&&!stopping;}
}

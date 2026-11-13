// Executes production selection and background probability parsing with a fake
// managed heap. It cannot establish live event/server acceptance.
#include <cassert>
#include <map>
#include <vector>
#include <limits>
extern "C" void *fake_symbol(void *,const char *);
#define dlsym fake_symbol
#include "catch_chance.cpp"
#undef dlsym

namespace {
struct Object {int kind;};
Object interaction{1},probability{2},types{3},values{4},ball{5},bag{6},lazy{7},encounter{8},map{9},display{10},response{11};
int display_class, strong_field, lazy_field;
union Box {int i;float f;bool b;} boxed;
std::map<uintptr_t,void*> roots;
uintptr_t next_handle=1;
std::vector<int> offered{1,2,3,7};
std::vector<float> rates{0.1f,0.2f,0.3f,0.8f};
std::map<int,int> stocks;
std::string settings;
std::vector<std::string> events;
bool mighty=true,eligible=true,in_play=false,fresh=true,broken=false;
int current_ball=1,selections=0;
uintptr_t root_new(void *p,int){auto h=next_handle++;roots[h]=p;return h;}
void *root_get(uintptr_t h){return roots.at(h);}
void root_free(uintptr_t h){assert(roots.erase(h)==1);}
void *object_class(void *o){return o==&display?static_cast<void*>(&display_class):o;}
void *object_unbox(void *o){return o;}
void *get_method(void*,const char *name,int){return const_cast<char*>(name);}
void *get_field(void*,const char *name){return const_cast<char*>(name);}
void *field_type(void*){return reinterpret_cast<void*>(0x12);}
int type_kind(const void*){return 0x12;}
bool is_enum(void*){return true;}
void field_read(void *o,void *f,void *out){
 if(f==&strong_field){*static_cast<uint8_t*>(out)=mighty?1:0;return;}
 if(f==&lazy_field){assert(o==&interaction);*static_cast<void**>(out)=&lazy;return;}
 const std::string name=static_cast<char*>(f);
 if(name=="CaptureProbability")*static_cast<void**>(out)=o==&response?static_cast<void*>(&probability):static_cast<void*>(&values);
 else if(name=="PokeballType")*static_cast<void**>(out)=&types;
 else assert(false);
}
void *managed_call(void *m,void *o,void **args,void **exc){
 *exc=nullptr;const std::string name=static_cast<char*>(m);
 if(broken&&name=="get_CanUseWildBallInEncounter"){*exc=&boxed;return nullptr;}
 if(name=="get_CaptureProbability")return &probability;
 if(name=="get_ActiveBerry")return nullptr;
 if(name=="get_Pokeball")return &ball;
 if(name=="get_BallType"){boxed.i=current_ball;return &boxed;}
 if(name=="get_PokeballInPlay"){boxed.b=in_play;return &boxed;}
 if(name=="get_Count"){boxed.i=int(o==&types?offered.size():rates.size());return &boxed;}
 if(name=="get_Item"){
  int index=*static_cast<int*>(args[0]);
  if(o==&types)boxed.i=offered.at(index);else boxed.f=rates.at(index);return &boxed;
 }
 if(name=="GetItemCount"){boxed.i=stocks[*static_cast<int*>(args[0])];return &boxed;}
 if(name=="SelectPokeball"){current_ball=*static_cast<int*>(args[0]);++selections;return nullptr;}
 if(name=="get_Value")return &encounter;
 if(name=="get_MapPokemon")return &map;
 if(name=="get_PokemonDisplay")return &display;
 if(name=="get_CanUseWildBallInEncounter"){boxed.b=eligible;return &boxed;}
 assert(false);return nullptr;
}
int sample(PgrRuntimeTransportSample *out){*out={};out->snapshot_fresh=fresh;return 0;}
int get_string(uint32_t slot,char *out,uint32_t cap,uint32_t *len){assert(slot==15&&cap>settings.size());*len=settings.size();std::memcpy(out,settings.data(),*len);return 0;}
int get_bool(uint32_t,int *out){*out=0;return 0;}
int get_int(uint32_t,int32_t *out){*out=0;return 0;}
int emit(uint32_t op,const int64_t*,uint32_t,const PgrRuntimeTransportString *s,uint32_t n){assert(op==19&&n==2);events.emplace_back(s[1].ptr,s[1].len);return 0;}
PgrRuntimeTransportApi transport_api{};
void reset(){
 settings="C2|1|1|75|0|1";stocks={{1,10},{2,10},{3,10},{7,5}};
 offered={1,2,3,7};rates={0.1f,0.2f,0.3f,0.8f};mighty=true;eligible=true;broken=false;fresh=true;in_play=false;
 current_ball=1;selections=0;events.clear();seen={};generation.fetch_add(1);
 safari={true,7,&display_class,&strong_field,&lazy_field};
}
bool logged(const char *needle){for(const auto &s:events)if(s.find(needle)!=std::string::npos)return true;return false;}
}
void *fake_symbol(void*,const char *name){
 if(std::strcmp(name,"il2cpp_type_get_type")==0)return reinterpret_cast<void*>(type_kind);
 if(std::strcmp(name,"il2cpp_class_is_enum")==0)return reinterpret_cast<void*>(is_enum);
 return nullptr;
}
extern "C" const void *pgrazor_runtime_transport_api(uint32_t){return &transport_api;}
extern "C" const void *pgrazor_runtime_bootstrap_api(uint32_t){return nullptr;}
extern "C" const void *pgrazor_runtime_metadata_api(uint32_t){return nullptr;}

int main(){
 assert(pgr_parse_catch_chance("C1|1|1|75").valid);
 assert(pgr_parse_catch_chance("C1|1|1|75|701").berry==701);
 assert(!pgr_parse_catch_chance("C1|1|1|75|0").safari);
 for(const char *bad:{"C2|1|1|75|0","C2|1|1|75|0|2","C2|1|1|75|0|1x","C2|1|1|0|0|1","C2|1|2|75|0|1","C1|1|1|75|","C9|1|1|75|0|1"})assert(!pgr_parse_catch_chance(bad).valid);
 boot.runtime.gchandle_new=reinterpret_cast<void*>(root_new);boot.runtime.gchandle_get_target=reinterpret_cast<void*>(root_get);boot.runtime.gchandle_free=reinterpret_cast<void*>(root_free);
 boot.runtime.object_get_class=reinterpret_cast<void*>(object_class);boot.runtime.object_unbox=reinterpret_cast<void*>(object_unbox);
 boot.runtime.runtime_invoke=reinterpret_cast<void*>(managed_call);boot.runtime.field_get_value=reinterpret_cast<void*>(field_read);
 boot.meta.class_get_method_from_name=reinterpret_cast<void*>(get_method);boot.meta.class_get_field_from_name=reinterpret_cast<void*>(get_field);boot.meta.field_get_type=reinterpret_cast<void*>(field_type);
 transport_api.sample=sample;transport_api.get_string=get_string;transport_api.get_bool=get_bool;transport_api.get_int=get_int;transport_api.emit=emit;
 ready=true;owner=gettid();select_method=const_cast<char*>("SelectPokeball");item_bag=Root(&bag);pgr_catch_chance_enter(&interaction);
 reset();step();assert(current_ball==7&&selections==1&&logged("safari-selected"));
 step();assert(selections==1);current_ball=2;step();assert(current_ball==2&&selections==1&&logged("manual-selection-kept"));
 in_play=true;step();in_play=false;step();assert(current_ball==7&&selections==2);
 reset();stocks[1]=stocks[2]=stocks[3]=0;step();assert(current_ball==7);
 reset();mighty=false;step();assert(current_ball==3&&logged("not-mighty"));
 reset();stocks[7]=0;step();assert(current_ball==3&&logged("no-safari-balls"));
 reset();eligible=false;step();assert(current_ball==3&&logged("encounter-ineligible"));
 reset();broken=true;step();assert(current_ball==3&&logged("eligibility-unavailable"));
 reset();safari.strong=nullptr;step();assert(current_ball==3&&logged("metadata-unavailable"));
 reset();settings="C2|1|0|75|0|1";step();assert(selections==0);
 reset();settings="C2|1|1|75|0|0";step();assert(current_ball==3&&!logged("safari-selected"));
 settings="C2|1|1|75|0|1";step();assert(current_ball==7&&selections==2);
 reset();settings="C3|1|1|75|0|1|1|0";step();assert(current_ball==7&&selections==1);
 assert(config().grade==0&&!config().curve); // Auto Throw does not override configured throw bonuses.
 for(const char* bad:{"C3|1|1|75|0|1|2|0","C3|1|1|75|0|1|1|-1","C3|1|1|75|0|1|1|7200001","C3|1|1|75|0|1|1|0x"})assert(!pgr_parse_catch_chance(bad).valid);
 reset();settings="C1|1|1|75|0";step();assert(current_ball==3);
 reset();fresh=false;step();assert(selections==0);
 reset();in_play=true;step();assert(selections==0);
 reset();assert(pgr_catch_chance_offers_ball(&response,7));
 rates[3]=0;assert(!pgr_catch_chance_offers_ball(&response,7));rates[3]=std::numeric_limits<float>::quiet_NaN();assert(!pgr_catch_chance_offers_ball(&response,7));
 rates[3]=1.01f;assert(!pgr_catch_chance_offers_ball(&response,7));rates.pop_back();assert(!pgr_catch_chance_offers_ball(&response,7));
 reset();assert(!pgr_catch_chance_offers_ball(&response,8));busy=true;assert(!pgr_catch_chance_offers_ball(&response,7));busy=false;
 assert(!pgr_is_inventory_safari_ball(7));pgr_safari_inventory_item.store(7);assert(pgr_is_inventory_safari_ball(7)&&!pgr_is_inventory_safari_ball(8)&&!pgr_is_inventory_safari_ball(4));
 pgr_catch_chance_stop();item_bag.reset();assert(roots.empty());
 std::puts("PASS catch chance Safari runtime: selection, manual override, breakout, stock, eligibility, defaults, malformed config, offered probabilities, roots");
}

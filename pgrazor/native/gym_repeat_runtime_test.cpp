// Actual gym_repeat.cpp adapter driven by a deterministic mock IL2CPP runtime.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <cstdlib>
#include <ctime>
static uint64_t fixture_now=1000;
extern "C" int fixture_clock(clockid_t, struct timespec*);

extern "C" void *fixture_symbol(void *, const char *);
#define dlsym fixture_symbol
#define clock_gettime fixture_clock
#include "gym_repeat.cpp"
#undef dlsym
#undef clock_gettime

using namespace pgr_gym_repeat;

namespace {

struct K { std::string name; };
struct M { std::string name; int count; };
struct F { std::string name; std::string type; };
struct O { K *type; std::map<std::string, uint64_t> data; std::u16string text; };

std::map<std::string, std::unique_ptr<K>> classes;
std::map<std::string, std::unique_ptr<M>> methods;
std::map<std::string, std::unique_ptr<F>> fields;
std::vector<std::unique_ptr<O>> objects;
std::map<uintptr_t, void *> handles;
uintptr_t handle_next = 0;
int live_handles = 0;

PgrSharedMetadataOutput fixture_md{};
PgrBootBinding fixture_boot{};

// Transport state.
int tr_enabled = 1, tr_active = 1, tr_battles = 1;
bool tr_fresh = true;
uint64_t tr_seq = 1, tr_generation=1;
int tr_map=0;
bool friendly=false, neutral=false, in_range=true, battle_disabled=false, button_ready=true;
bool sync_screens=true, delay_open=false;
std::string player_id="ACCOUNT_A";
std::function<void()> on_player_read;
int reads_until_callback=0;
int back_calls=0;
std::vector<std::string> statuses;

// Managed fixtures.
std::string g_fort_id = "GYM_A";
bool g_complete = true, g_fainted = false;
int g_heal = 0;
int gym_heal_result=0, gym_heal_calls=0, baseline_calls=0;
int click_calls = 0, battle_invoke_calls = 0, tapped_calls = 0;

O *g_map, *g_container, *g_root_ctrl, *player, *approach_ui, *prep_ui, *button_ui, *mapgym, *fortobj;
struct Array {char header[32]{}; void *items[1]{};} gym_array, badge_array;
O *badge_ui; bool badge_present=false, badge_closed=true; int badge_dismissals=0;

// Recorded hooks: code sentinel -> replacement.
std::map<void *, void *> hooks;
bool fail_validation=false;
int fail_hook_at=0;
int code_ao, code_ab, code_po, code_pb, code_pe, code_re; // address identity per method
K *cls_approach, *cls_prep, *cls_state;

K *klass_of(const std::string &n) {
    auto it = classes.find(n);
    if (it == classes.end()) it = classes.emplace(n, std::unique_ptr<K>(new K{n})).first;
    return it->second.get();
}
M *method_of(const std::string &n, int count) {
    std::string key = n + "#" + std::to_string(count);
    auto it = methods.find(key);
    if (it == methods.end()) it = methods.emplace(key, std::unique_ptr<M>(new M{n, count})).first;
    return it->second.get();
}
O *object(const std::string &n) { objects.emplace_back(new O{klass_of(n), {}, {}}); return objects.back().get(); }
O *box_bool(bool v) { O *o = object("System.Boolean"); o->data["value"] = v ? 1 : 0; return o; }
O *string_obj(const std::string &s) { O *o = object("System.String"); o->text.assign(s.begin(), s.end()); return o; }

// --- fake IL2CPP runtime ----------------------------------------------------------
uintptr_t gc_new(void *o, int) { if (!o) return 0; handles[++handle_next] = o; ++live_handles; return handle_next; }
void *gc_get(uintptr_t h) { auto it = handles.find(h); return it == handles.end() ? nullptr : it->second; }
void gc_free(uintptr_t h) { if (handles.erase(h)) --live_handles; }
void *obj_class(void *o) { return static_cast<O *>(o)->type; }
void *fx_unbox(void *o) { return &static_cast<O *>(o)->data["value"]; }
void field_read(void *o, void *f, void *out) {
    auto *ff = static_cast<F *>(f);
    uint64_t v = static_cast<O *>(o)->data[ff->name];
    size_t n = ff->type == "System.Boolean" ? 1 : ff->type == "Niantic.Holoholo.Battle.BattleStatus" ? 4 : 8;
    std::memcpy(out, &v, n);
}
const uint16_t *str_chars(void *o) { return reinterpret_cast<const uint16_t *>(static_cast<O *>(o)->text.data()); }
int g_thread = 1;
void *thread_current() { return &g_thread; }

void *fx_invoke(void *method, void *obj, void **args, void **ex) {
    *ex = nullptr; const std::string n = static_cast<M *>(method)->name;
    if(n=="find")return g_map;
    if(n=="container")return g_container;
    if(n=="resolve") {auto *k=static_cast<K*>(args[0]); return k->name=="Niantic.Holoholo.Gym.IGymRootController"?g_root_ctrl:k->name=="Niantic.Holoholo.IPlayerService"?player:nullptr;}
    if(n=="get_PlayerId"){if(on_player_read && --reads_until_callback<=0){auto f=std::move(on_player_read);on_player_read={};f();}return string_obj(player_id);}
    if(n=="get_FortId")return string_obj(g_fort_id);
    if(n=="get_BattleComplete")return box_bool(g_complete);
    if(n=="get_AllPlayerPokemonFainted")return box_bool(g_fainted);
    if(n=="get_Enabled")return box_bool(true);
    if(n=="get_IsBattleDisabled")return box_bool(battle_disabled);
    if(n=="get_IsPlayerInRange")return box_bool(in_range);
    if(n=="get_HasRaidInfo")return box_bool(false);
    if(n=="get_IsFriendly")return box_bool(friendly);
    if(n=="get_IsNeutral")return box_bool(neutral);
    if(n=="get_InGymState")return box_bool(tr_map==0);
    if(n=="get_IsOpen"||n=="get_IsTopGui")return box_bool(static_cast<O*>(obj)->data["open"]!=0);
    if(n=="get_interactable")return box_bool(button_ready);
    if(n=="op_Implicit")return box_bool(args[0]!=nullptr);
    if(n=="get_IsFullyDismissed")return box_bool(obj==badge_ui?badge_closed:false);
    if(n=="Dismiss"){assert(obj==badge_ui);++badge_dismissals;badge_ui->data["open"]=0;return nullptr;}
    if(n=="find_all")return static_cast<K*>(args[0])->name=="Niantic.Holoholo.GymBadges.GymBadgeAwardGuiController"? &badge_array:&gym_array;
    if(n=="fort")return fortobj;
    if(n=="OnBackButtonPressed"){++back_calls;hook_approach_back(obj,nullptr);static_cast<O*>(obj)->data["open"]=0;tr_map=1;return nullptr;}
    if(n=="OnTapped"){++tapped_calls;if(sync_screens){tr_map=0;approach_ui->data["open"]=!delay_open;hook_capture_root<HK_APPROACH_OPEN>(approach_ui,nullptr);}return nullptr;}
    if(n=="ClickBattle"){++click_calls;approach_ui->data["open"]=0;if(sync_screens){prep_ui->data["open"]=!delay_open;hook_capture_root<HK_PREP_OPEN>(prep_ui,nullptr);}return nullptr;}
    if(n=="Battle"){++battle_invoke_calls;hook_prep_battle(obj,nullptr);prep_ui->data["open"]=0;api_observe_scope(1,self_tid());return nullptr;}
    return nullptr;
}

// --- fake metadata ----------------------------------------------------------------
void *class_type(void *k) { return k; }
void *method_lookup(void *, const char *name, int count) { return method_of(name, count); }
unsigned method_count(void *m) { return static_cast<M *>(m)->count; }
void *field_lookup(void *, const char *name) {
    std::string n = name, t = "System.Int32";
    if (n == "FortId") t = "System.String";
    if (n == "Enabled") t = "System.Boolean";
    if (n == "Latitude" || n == "Longitude") t = "System.Double";
    if (n == "RaidInfo") t = "Holoholo.Rpc.RaidInfoProto";
    if(n=="dyka"||n=="dzdt")t="Niantic.Holoholo.Gym.IGymRootController";
    if(n=="battleButton")t="UnityEngine.UI.Button";
    if(n=="BattleStatus")t="Niantic.Holoholo.Battle.BattleStatus";
    auto it = fields.find(n);
    if (it == fields.end()) it = fields.emplace(n, std::unique_ptr<F>(new F{n, t})).first;
    return it->second.get();
}
int field_flags(void *p) {auto n=static_cast<F*>(p)->name;return n=="Victory"||n=="CONTINUE_BATTLE"||n=="LEAVE_GYM"||n=="LEAVE_BATTLE"?0x8050:0;}
void *field_type(void *p){return klass_of(static_cast<F*>(p)->type);}
char *type_name(void *p){return strdup(static_cast<K*>(p)->name.c_str());}
void static_value(void *p,void *out){auto n=static_cast<F*>(p)->name;*static_cast<int*>(out)=n=="LEAVE_GYM"?0:n=="LEAVE_BATTLE"?1:2;}

int resolve_class(const PgrMetaClassQuery *q, PgrMetaClassResult *out) {
    *out = {}; out->klass = klass_of(std::string(q->name_space) + "." + q->name); out->type = out->klass; return 0;
}
int validate_method(const PgrMetaMethodQuery *q, PgrMetaMethodResult *out) {
    *out = {};
    if(fail_validation)return -1;
    void *code = nullptr;
    const std::string name = q->name;
    const void *owner = q->owner;
    if (owner == cls_approach && name == "HandleOpen") code = &code_ao;
    else if (owner == cls_approach && name == "OnBackButtonPressed") code = &code_ab;
    else if (owner == cls_prep && name == "HandleOpen") code = &code_po;
    else if (owner == cls_prep && name == "Battle") code = &code_pb;
    else if (owner == cls_prep && name == "ExitPressed") code = &code_pe;
    else if (owner == cls_state && name == "ExitBattleResultsState") code = &code_re;
    else return -1;
    if(name=="ExitBattleResultsState") {assert(q->param_count==2);assert(q->param_types[1]&&std::string(q->param_types[1])=="Niantic.Holoholo.Gym.BattleResultsExit");}
    out->method = method_of(name, q->param_count);
    out->code = code;
    return 0;
}

// --- fake transport ---------------------------------------------------------------
int tr_sample(PgrRuntimeTransportSample *out) { *out = {}; out->snapshot_fresh = tr_fresh; out->fault = 0; out->snapshot_seq = tr_seq; out->generation=tr_generation; return 0; }
int tr_bool(uint32_t id, int *out) { *out = id == 40 ? tr_enabled : id == 3 ? tr_active : id == 6 ? tr_battles : id==38 ? tr_map : 0; return 0; }
int tr_double(uint32_t, double *out, int *has) { *out = 0; if (has) *has = 1; return 0; }
int tr_emit(uint32_t, const int64_t *, uint32_t, const PgrRuntimeTransportString *s, uint32_t n) {
    if (n) statuses.emplace_back(s[0].ptr, s[0].len);
    return 0;
}

PgrBootApi boot_api{};
PgrMetaApi meta_api{};
PgrRuntimeTransportApi transport_api{};

int boot_snapshot(PgrBootBinding *out) { *out = fixture_boot; return 0; }

// fixture exports resolved via dlsym.
void *type_get_object(void *k) { return k; }
int string_length(void *o) { return int(static_cast<O *>(o)->text.size()); }
void *object_get_virtual_method(void *, void *m) { return m; }
size_t array_length(void *o) { return o==&badge_array ? (badge_present?1:0) : 1; }

const PgrGymRepeatApi *api = nullptr;

void reset_world() {
    classes.clear(); methods.clear(); fields.clear(); objects.clear();
    handles.clear(); handle_next = 0; live_handles = 0;
    statuses.clear();
    g_fort_id = "GYM_A"; g_complete = true; g_fainted = false; g_heal = 0;
    click_calls = battle_invoke_calls = tapped_calls = 0;
    tr_enabled = tr_active = tr_battles = 1; tr_fresh = true; tr_seq = 1;

    cls_approach = klass_of("Niantic.Holoholo.Gym.Approach.GymApproachGuiController");
    cls_prep = klass_of("Niantic.Holoholo.Gym.GymBattlePrepUIController");
    cls_state = klass_of("Niantic.Holoholo.Gym.GymBattleState");
    g_map = object("Map"); g_container = object("Container"); g_root_ctrl = object("GymRootController");

    fixture_md = {};
    fixture_md.opt_handles[H_IL_MAP] = klass_of("Map");
    fixture_md.opt_handles[H_IL_FIND_M] = method_of("find", 1);
    fixture_md.opt_handles[H_IL_CONTAINER_M] = method_of("container", 0);
    fixture_md.opt_handles[H_IL_RESOLVE_M] = method_of("resolve", 1);
    fixture_md.methods[5] = method_of("op_Implicit", 1);
    fixture_md.opt_handles[H_VGP_M_FIND_ALL]=method_of("find_all",1);
    fixture_md.opt_handles[H_VGP_M_FORT]=method_of("fort",0);
    badge_ui=object("Niantic.Holoholo.GymBadges.GymBadgeAwardGuiController");badge_array.items[0]=badge_ui;
    player=object("Player");approach_ui=object("Niantic.Holoholo.Gym.Approach.GymApproachGuiController");prep_ui=object("Niantic.Holoholo.Gym.GymBattlePrepUIController");button_ui=object("UnityEngine.UI.Button");
    approach_ui->data["dzdt"]=reinterpret_cast<uintptr_t>(g_root_ctrl);prep_ui->data["dyka"]=reinterpret_cast<uintptr_t>(g_root_ctrl);prep_ui->data["battleButton"]=reinterpret_cast<uintptr_t>(button_ui);
    mapgym=object("Niantic.Holoholo.Gym.MapGym");fortobj=object("Holoholo.Rpc.PokemonFortProto");fortobj->data["FortId"]=reinterpret_cast<uintptr_t>(string_obj("GYM_A"));fortobj->data["Enabled"]=1;gym_array.items[0]=mapgym;

    fixture_boot = {};
    fixture_boot.module_base = reinterpret_cast<const void *>(0x1000);
    fixture_boot.handle = reinterpret_cast<void *>(0x2000);
    auto &r = fixture_boot.runtime;
    r.gchandle_new = reinterpret_cast<void *>(gc_new);
    r.gchandle_get_target = reinterpret_cast<void *>(gc_get);
    r.gchandle_free = reinterpret_cast<void *>(gc_free);
    r.object_get_class = reinterpret_cast<void *>(obj_class);
    r.object_unbox = reinterpret_cast<void *>(fx_unbox);
    r.runtime_invoke = reinterpret_cast<void *>(fx_invoke);
    r.field_get_value = reinterpret_cast<void *>(field_read);
    r.field_static_get_value=reinterpret_cast<void*>(static_value);
    r.string_chars = reinterpret_cast<void *>(str_chars);
    r.thread_current = reinterpret_cast<void *>(thread_current);
    auto &m = fixture_boot.meta;
    m.class_get_type = reinterpret_cast<void *>(class_type);
    m.class_get_method_from_name = reinterpret_cast<void *>(method_lookup);
    m.method_get_param_count = reinterpret_cast<void *>(method_count);
    m.class_get_field_from_name = reinterpret_cast<void *>(field_lookup);
    m.field_get_flags = reinterpret_cast<void *>(field_flags);
    m.field_get_type=reinterpret_cast<void*>(field_type); m.type_get_name=reinterpret_cast<void*>(type_name);m.mem_free=reinterpret_cast<void*>(free);

    boot_api.snapshot = boot_snapshot;
    meta_api.resolve_class = resolve_class;
    meta_api.validate_method = validate_method;
    transport_api.sample = tr_sample;
    transport_api.get_bool = tr_bool;
    transport_api.get_double = tr_double;
    transport_api.emit = tr_emit;

    hooks.clear();
}

void noop2(void *, void *) {}
void noop4(void *, void *, int32_t, void *) {}

void configure() {
    reset_world();
    PgrGymRepeatConfig cfg{};
    cfg.version = PGR_GYM_REPEAT_API_VERSION;
    cfg.size = sizeof(cfg);
    cfg.owner_tid = uint64_t(gettid());
    cfg.metadata = &fixture_md;
    assert(api->configure(&cfg) == PGR_GR_OK);
}

void fire2(int *code, void *self) {
    auto it = hooks.find(code);
    assert(it != hooks.end());
    reinterpret_cast<void (*)(void *, void *)>(it->second)(self, nullptr);
}
void fire_results(void *self,void *frame,int32_t exit=1){reinterpret_cast<FnResults>(hooks.at(&code_re))(self,frame,exit,nullptr);}
void tick(){assert(api->tick(self_tid())==PGR_GR_OK);}
int state(){PgrGymRepeatSample s{};api->sample(&s);return s.state;}
void reset_session(){
 api->stop(self_tid());assert(live_handles==0);fixture_now+=100000;tr_enabled=tr_active=tr_battles=1;tr_fresh=true;tr_map=0;friendly=neutral=battle_disabled=false;in_range=button_ready=true;sync_screens=true;delay_open=false;g_complete=true;g_fainted=false;g_heal=0;player_id="ACCOUNT_A";g_fort_id="GYM_A";on_player_read={};
 gym_heal_result=0;gym_heal_calls=baseline_calls=0;
 badge_present=false;badge_closed=true;badge_dismissals=0;badge_ui->data["open"]=0;
 approach_ui->data["open"]=prep_ui->data["open"]=0;click_calls=battle_invoke_calls=tapped_calls=back_calls=0;
}
void start_round(){prep_ui->data["open"]=1;fire2(&code_pb,prep_ui);api->observe_scope(1,self_tid());tick();assert(state()==ST_IN_ROUND);}
void win_round(){
 start_round();O *st=object("GymBattleState"),*frame=object("Niantic.Holoholo.Battle.BattleFrame");frame->data["BattleStatus"]=2;
 // Optional natural map return: no automatic Back is involved.
 fire_results(st,frame);api->observe_scope(0,self_tid());prep_ui->data["open"]=0;
 approach_ui->data["open"]=0;fire2(&code_ao,approach_ui);tr_map=1;
 tick();assert(back_calls==0&&policy.phase==PH_MAP_DWELL);
}
void won_approach(){
 start_round();O *st=object("GymBattleState"),*frame=object("Niantic.Holoholo.Battle.BattleFrame");frame->data["BattleStatus"]=2;
 fire_results(st,frame);api->observe_scope(0,self_tid());prep_ui->data["open"]=0;
 approach_ui->data["open"]=1;fire2(&code_ao,approach_ui);
}

} // namespace

// --- injected API accessors / globals the included module links against -----------
const void *pgrazor_runtime_bootstrap_api(uint32_t) { return &boot_api; }
const void *pgrazor_runtime_metadata_api(uint32_t) { return &meta_api; }
const void *pgrazor_runtime_transport_api(uint32_t) { return &transport_api; }
int pgrazor_hook(void *address, void *replacement, void **original) {
    hooks[address] = replacement;
    *original = address == &code_re ? reinterpret_cast<void*>(noop4) : reinterpret_cast<void*>(noop2);
    return fail_hook_at && int(hooks.size())==fail_hook_at ? -1 : 0;
}
int pgr_heal_busy() { return g_heal; }
void pgr_heal_gym_baseline(uint64_t tid,const PgrSharedMetadataOutput *metadata){assert(tid==self_tid()&&metadata==&fixture_md);++baseline_calls;}
int pgr_heal_gym_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata,uint64_t token,bool (*ready)()){
 assert(tid==self_tid()&&metadata==&fixture_md&&token==policy.won_at);assert(ready&&ready());++gym_heal_calls;return gym_heal_result;
}
void *fixture_symbol(void *, const char *n) {
    std::string s = n;
    if (s == "il2cpp_type_get_object") return reinterpret_cast<void *>(type_get_object);
    if (s == "il2cpp_string_length") return reinterpret_cast<void *>(string_length);
    if (s == "il2cpp_object_get_virtual_method") return reinterpret_cast<void *>(object_get_virtual_method);
    if (s == "il2cpp_array_length") return reinterpret_cast<void *>(array_length);
    return nullptr;
}

extern "C" int fixture_clock(clockid_t,struct timespec *t){t->tv_sec=fixture_now/1000;t->tv_nsec=(fixture_now%1000)*1000000;return 0;}
int main(){
 api=static_cast<const PgrGymRepeatApi*>(pgrazor_gym_repeat_api(1));
 reset_world();PgrGymRepeatConfig cfg{1,sizeof(cfg),self_tid(),&fixture_md};fail_validation=true;
 assert(api->configure(&cfg)==PGR_GR_ERR_BINDING&&hooks.empty()&&!g_ready.load());fail_validation=false;fail_hook_at=3;
 assert(api->configure(&cfg)==PGR_GR_ERR_BINDING&&!g_ready.load()&&hooks.size()==3);
 fire2(&code_ao,approach_ui);assert(live_handles==0&&state()==ST_IDLE); // partial hook calls only original
 for(auto &slot:g_orig)slot.store(nullptr);g_installed_hooks=0;fail_hook_at=0;configure();assert(hooks.size()==6);
 tick();fire2(&code_ao,approach_ui);tick();assert(state()==ST_IDLE&&live_handles==0);
 fire2(&code_ab,approach_ui);tick();assert(state()==ST_IDLE);
 // Re-shown gym screen starts another round directly, with no Back or map tap.
 reset_session();won_approach();g_heal=1;tick();assert(click_calls==1&&back_calls==0&&tapped_calls==0);
 tick();assert(battle_invoke_calls==1);tick();assert(state()==ST_IN_ROUND&&live_handles==0);
 // Explicit healing must finish before the next ClickBattle; active combat never calls it.
 assert(baseline_calls==1&&gym_heal_calls>0);int healed_calls=gym_heal_calls;tick();assert(gym_heal_calls==healed_calls);
 reset_session();won_approach();gym_heal_result=1;tick();tick();assert(click_calls==0&&battle_invoke_calls==0&&back_calls==0);
 gym_heal_result=0;tick();assert(click_calls==1);tick();assert(battle_invoke_calls==1);tick();assert(state()==ST_IN_ROUND);
 reset_session();won_approach();gym_heal_result=-1;tick();assert(state()==ST_IDLE&&g_last_end==END_ERROR&&click_calls==0);
 reset_session();won_approach();gym_heal_result=1;tick();tr_enabled=0;tick();assert(state()==ST_IDLE&&click_calls==0&&live_handles==0);
 // Cleared gym ends on its own screen without any navigation.
 reset_session();won_approach();neutral=true;tick();assert(g_last_end==END_DEFEATED&&back_calls==0&&click_calls==0&&tapped_calls==0&&live_handles==0);
 // Badge on the direct route must fully close before either Battle action.
 reset_session();won_approach();badge_present=true;badge_closed=false;badge_ui->data["open"]=1;
 tick();assert(badge_dismissals==1&&click_calls==0&&battle_invoke_calls==0);
 fixture_now+=250;tick();assert(badge_dismissals==1&&click_calls==0);
 badge_closed=true;fixture_now+=250;tick();assert(click_calls==1&&back_calls==0&&tapped_calls==0);
 // A second badge can open at prep inside the scan-cache interval.
 badge_closed=false;badge_ui->data["open"]=1;tick();assert(badge_dismissals==2&&battle_invoke_calls==0);
 badge_closed=true;fixture_now+=250;tick();assert(battle_invoke_calls==1);tick();assert(state()==ST_IN_ROUND&&live_handles==0);
 // Manual exit and toggle changes still cancel direct repeat.
 reset_session();won_approach();fire2(&code_ab,approach_ui);tick();assert(state()==ST_IDLE&&click_calls==0&&back_calls==0);
 reset_session();won_approach();tr_enabled=0;tick();assert(state()==ST_IDLE&&click_calls==0&&live_handles==0);
 reset_session();win_round();fixture_now+=4499;tick();assert(tapped_calls==0);g_heal=1;fixture_now+=1;tick();assert(tapped_calls==0);g_heal=0;
 delay_open=true;tick();assert(tapped_calls==1);assert(g_approach_root.h);tick();assert(click_calls==0&&g_approach_root.h);approach_ui->data["open"]=1;
 tick();assert(click_calls==1);tick();assert(battle_invoke_calls==0&&g_prep_root.h);prep_ui->data["open"]=1;button_ready=false;tick();assert(battle_invoke_calls==0);button_ready=true;tick();assert(battle_invoke_calls==1);tick();assert(state()==ST_IN_ROUND&&live_handles==0);
 fixture_now+=300000;tick();assert(state()==ST_IN_ROUND); // no 60-second active battle cutoff
 reset_session();start_round();O *st=object("State"),*fr=object("Niantic.Holoholo.Battle.BattleFrame");fr->data["BattleStatus"]=2;fire_results(st,fr,2);tick();assert(state()==ST_IN_ROUND&&back_calls==0);api->observe_scope(0,self_tid());tick();assert(state()==ST_IDLE&&g_last_end==END_MANUAL_CANCEL);
 reset_session();start_round();g_fainted=true;fire_results(st,fr,1);api->observe_scope(0,self_tid());tick();assert(g_last_end==END_LOSS&&live_handles==0);
 reset_session();start_round();fr->data["BattleStatus"]=4;fire_results(st,fr,1);api->observe_scope(0,self_tid());tick();assert(state()==ST_IDLE&&back_calls==0);fr->data["BattleStatus"]=2;
 reset_session();win_round();neutral=true;fixture_now+=4500;tick();assert(g_last_end==END_DEFEATED&&tapped_calls==0&&live_handles==0);
 reset_session();win_round();fixture_now+=4500;tr_enabled=0;tick();assert(state()==ST_IDLE&&tapped_calls==0);
 reset_session();win_round();fixture_now+=4500;tr_fresh=false;tick();assert(state()==ST_IDLE&&tapped_calls==0);
 reset_session();win_round();fixture_now+=4500;player_id="ACCOUNT_B";tick();assert(g_last_end==END_ACCOUNT&&tapped_calls==0);
 reset_session();win_round();fixture_now+=4500;++tr_generation;tick();assert(g_last_end==END_ACCOUNT&&tapped_calls==0);
 reset_session();win_round();fixture_now+=4500;reads_until_callback=2;on_player_read=[](){tr_enabled=0;};tick();assert(tapped_calls==0&&state()==ST_IDLE);
 reset_session();win_round();fixture_now+=4500;reads_until_callback=2;on_player_read=[](){api_observe_leave(self_tid());};tick();assert(tapped_calls==0&&state()==ST_IDLE);
 reset_session();win_round();fixture_now+=4500;reads_until_callback=2;on_player_read=[](){api->stop(self_tid());};tick();assert(tapped_calls==0&&state()==ST_IDLE&&live_handles==0);
 reset_session();win_round();sync_screens=false;fixture_now+=4500;tick();assert(tapped_calls==1);tick();tick();assert(tapped_calls==1);fixture_now+=60001;tick();assert(g_last_end==END_TIMEOUT&&live_handles==0);
 reset_session();win_round();fixture_now+=4500;tick();g_fort_id="GYM_B";tick();assert(click_calls==0&&state()==ST_IDLE);
 reset_session();win_round();std::thread stopper([](){api->stop(self_tid());});stopper.join();assert(g_cancel.load()&&g_approach_root.h);tick();assert(state()==ST_IDLE&&live_handles==0);
 reset_session();start_round();for(size_t i=0;i<kEvCap+1;i++){Event e{};e.type=EV_RESULTS;push_ev(e);}tick();assert(state()==ST_IDLE&&live_handles==0);
 // Badge popup blocks map readiness even when the map heartbeat is true.
 reset_session();win_round();badge_present=true;badge_closed=false;badge_ui->data["open"]=1;fixture_now+=4500;
 tick();assert(badge_dismissals==1&&tapped_calls==0&&g_badge_root.h);
 fixture_now+=250;tick();assert(badge_dismissals==1&&tapped_calls==0); // closing animation, no duplicate Dismiss
 badge_closed=true;fixture_now+=250;tick();assert(tapped_calls==1&&!g_badge_root.h);
 // Popup arrives after an entry attempt: only its observed dismissal permits a retry.
 reset_session();win_round();sync_screens=false;fixture_now+=4500;tick();assert(tapped_calls==1);
 badge_present=true;badge_closed=false;badge_ui->data["open"]=1;fixture_now+=250;tick();assert(badge_dismissals==1&&tapped_calls==1);
 badge_closed=true;fixture_now+=250;tick();assert(policy.phase==PH_MAP_DWELL&&tapped_calls==1);
 fixture_now+=4500;tick();assert(tapped_calls==2);tick();assert(tapped_calls==2);
 // Idle gym badge stays visible with no armed repeat session.
 reset_session();badge_present=true;badge_closed=false;badge_ui->data["open"]=1;tick();assert(badge_dismissals==0);
 // Toggle off while a popup is pending releases it without dismissal/navigation.
 reset_session();win_round();badge_present=true;badge_closed=false;fixture_now+=250;tick();assert(g_badge_root.h&&badge_dismissals==0);
 tr_enabled=0;fixture_now+=250;tick();assert(state()==ST_IDLE&&live_handles==0&&badge_dismissals==0);
 puts("gym_repeat runtime: PASS (real hooks/tick; direct repeat, natural map fallback, badge gating, enums, synchronous and delayed UI, healing, cancellation, identity, roots)");
}

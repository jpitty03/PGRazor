// Game 0.435.2 adapter. Same silent MapPokemon RPCs as VGP+, explicit tap ownership.
// Included inside auto_raid.cpp's anonymous namespace: headers must be included by that file, at
// global scope. Including vgp_marker.h here gave this file a private copy of its shared variable.
bool map_gate=false,map_hook_attempted=false,map_hook_ready=false,map_hook_frame=false;
using MapTapFn=void(*)(void*,void*,void*,void*);
MapTapFn map_original=nullptr;
void* map_wild_class=nullptr;

bool map_vgp_busy(){
    auto api=static_cast<const PgrVgpControllerApi*>(pgrazor_vgp_controller_api(1));
    if(!api||!api->sample)return true;
    PgrVgpControllerSample sample{};sample.version=1;sample.size=sizeof(sample);
    return api->sample(&sample)!=0||sample.phase!=PGR_VGC_PH_NONE;
}
bool map_command_free(){
    auto tr=transport();char command[1280]{};uint32_t n=0;
    return tr&&tr->get_string(12,command,sizeof(command),&n)==0&&n<sizeof(command)&&map_command_idle(command,n);
}
// Every tappable map Pokemon derives from MapPokemon: wild spawns, incense spawns, lure spawns and
// event spawn types. Each class is checked once. One is caught on the map only if it has the same
// encounter and capture calls as a wild spawn and exactly one stored encounter response whose
// success value is known; any other class keeps the game's normal encounter screen.
struct MapKind{void* klass=nullptr;bool ok=false;void *m_encounter=nullptr,*f_response=nullptr,*f_status=nullptr;int success=0;};
std::vector<MapKind> map_kinds;
void map_bind_kind(MapKind& k){
    void* base=resolve_class("Niantic.Holoholo.Map","MapPokemon");
    require(fn<bool(*)(void*,void*)>(boot.meta.class_is_assignable_from)(base,k.klass),"Not a map Pokemon");
    k.m_encounter=require_map_method(k.klass,"SendEncounterRequest",0,
        "Niantic.Promises.IPromise<Niantic.Holoholo.Map.PokemonEncounterResponse>");
    require_map_method(k.klass,"TryCapture",2,"Niantic.Promises.IPromise<Holoholo.Rpc.CatchPokemonOutProto>",
        "Niantic.Holoholo.Encounter.PokeballThrow","Holoholo.Rpc.ARPlusEncounterValuesProto");
    auto get_field=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name);
    auto field_type=fn<void*(*)(void*)>(boot.meta.field_get_type);
    if(k.klass==map_wild_class){
        k.f_response=find_field_by_name_checked(map_wild_class,PGR_GAME_SMART_CATCH_BINDING_CPP_EMBS_NAME,"Holoholo.Rpc.EncounterOutProto");
    }else{
        auto fields=fn<void*(*)(void*,void**)>(exp_sym("class_get_fields"));
        auto flags=fn<int(*)(void*)>(boot.meta.field_get_flags);
        auto parent=fn<void*(*)(void*)>(exp_sym("class_get_parent"));
        const std::string prefix="Holoholo.Rpc.",suffix="EncounterOutProto";int count=0;void* at=k.klass;
        for(int depth=0;at&&depth<12;++depth,at=parent(at)){
            void* iter=nullptr;
            while(void* f=fields(at,&iter)){
                if(flags(f)&0x10)continue;
                const std::string type=type_name_of(field_type(f));
                if(type.size()<=prefix.size()+suffix.size()||type.compare(0,prefix.size(),prefix)||
                   type.compare(type.size()-suffix.size(),suffix.size(),suffix))continue;
                k.f_response=f;++count;
            }
            if(at==base)break;
        }
        require(count==1,"Encounter response field missing or ambiguous");
    }
    void* response_class=fn<void*(*)(void*)>(boot.meta.class_from_type)(field_type(k.f_response));
    require(response_class!=nullptr,"Encounter response class unavailable");
    k.f_status=get_field(response_class,"Status");if(!k.f_status)k.f_status=get_field(response_class,"Result");
    require(k.f_status!=nullptr,"Encounter response has no status");
    void* status_class=fn<void*(*)(void*)>(boot.meta.class_from_type)(field_type(k.f_status));
    require(status_class!=nullptr,"Encounter status type unavailable");
    const char* found=nullptr;
    for(const char* name:{"EncounterSuccess","IncenseEncounterSuccess","Success"})if(get_field(status_class,name)){found=name;break;}
    require(found!=nullptr,"Encounter success value unknown");
    k.success=read_enum_literal(status_class,found);k.ok=true;
}
const MapKind& map_kind(void* klass){
    static const MapKind unsupported{};
    if(!klass)return unsupported;
    for(const auto& k:map_kinds)if(k.klass==klass)return k;
    if(map_kinds.size()>=16)return unsupported;
    MapKind k;k.klass=klass;std::string why;
    try{map_bind_kind(k);}catch(const Failure& f){k.ok=false;why=f.where;}catch(...){k.ok=false;why="native failure";}
    map_kinds.push_back(k);
    map_event(k.ok?"kind-ready":"kind-unsupported",klass_name_of(klass)+(k.ok?std::string():": "+why));
    return map_kinds.back();
}
bool map_real_alive(void* raw){
    Root target(raw);if(!target||!unity_obj_alive(target.get())||unbox_val<bool>(call_obj(target.get(),"get_Destroyed")))return false;
    // Incense and lure spawns have no VisibleOnMap; the checks below still apply to them.
    if(void* visible=find_method_in_hierarchy(obj_klass(target.get()),"get_VisibleOnMap",0))
        if(!unbox_val<bool>(invoke_method(visible,target.get())))return false;
    void* base=resolve_class("Niantic.Holoholo.Map","MapPokemon");
    if(!read_field_val<bool>(target.get(),find_field_by_name_checked(base,"_selectionEnabled","System.Boolean")))return false;
    Root map(read_field_val<void*>(target.get(),find_field_by_name_checked(base,"mapExploreState","Niantic.Holoholo.Map.IMapExploreState")));
    return map&&unbox_val<bool>(call_obj(map.get(),"get_IsActiveAndVisible"));
}
bool map_real_eligible(void* raw){
    if(!raw||!map_kind(obj_klass(raw)).ok||!map_real_alive(raw))return false;
    // A sleeping Snorlax has its own wake-up encounter; leave it to the game.
    void* asleep=find_method_in_hierarchy(obj_klass(raw),"get_IsSleepingSnorlax",0);
    return !asleep||!unbox_val<bool>(invoke_method(asleep,raw));
}
uint64_t map_real_id(void* raw){return unbox_val<uint64_t>(call_obj(raw,"get_EncounterId"));}
Root map_real_encounter(void* raw){
    Root target(raw);
    const MapKind& kind=map_kind(obj_klass(target.get()));require(kind.ok,"Unsupported map Pokemon");
    return Root(invoke_method(kind.m_encounter,target.get()));
}
int map_poll_response(void* promise,uint64_t tid,Root& response){
    Root hold(promise);auto api=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));
    require(api&&api->poll,"Promise observer unavailable");PgrVgpResultsResult out{};
    int rc=api->poll(hold.get(),PGR_VR_PHASE_ENCOUNTER,tid,&out);response.h=out.value_handle;return rc;
}
int map_real_poll_encounter(void* promise,uint64_t tid,Root& response){
    Root wrapper;int rc=map_poll_response(promise,tid,wrapper);if(rc!=PGR_VR_DONE)return rc;
    require(wrapper&&obj_class_name(wrapper.get())=="PokemonEncounterResponse","Unexpected encounter response");
    const MapKind& kind=map_kind(obj_klass(map_tap.target.get()));require(kind.ok,"Unsupported map Pokemon");
    response=Root(read_field_val<void*>(map_tap.target.get(),kind.f_response));require(bool(response),"Map encounter response missing");
    const int result=read_field_val<int>(response.get(),kind.f_status);
    if(result!=kind.success){
        map_event("encounter-refused","Server refused map encounter",result);require(false,"Encounter refused (range, storage, or unavailable spawn); no throw sent");
    }
    return rc;
}
int map_real_active_berry(void* response){
    if(!response)return 0;
    void* field=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(obj_klass(response),"ActiveItem");
    return field?read_field_val<int>(response,field):0;
}
int map_stock(int item){
    Root bag=resolve_di_service("Niantic.Holoholo","IItemBag");require(bool(bag),"Inventory unavailable");
    void* args[]={&item};return unbox_val<int>(call_obj(bag.get(),"GetItemCount",1,args));
}
Root map_real_berry(void* raw,int berry){
    Root target(raw);if(map_stock(berry)<=0)return Root();
    // A spawn type the berry call does not accept is thrown at without a berry.
    void* context=resolve_class_optional("Niantic.Holoholo.Map","IItemEncounterContext");
    if(!context||!fn<bool(*)(void*,void*)>(boot.meta.class_is_assignable_from)(context,obj_klass(target.get())))return Root();
    real_ensure_berry_ready();
    Root bag=resolve_di_service("Niantic.Holoholo","IItemBag");require(bool(bag),"Berry inventory unavailable");
    void* method=require_map_method(obj_klass(bag.get()),"UseItemForEncounter",2,
        "Niantic.Promises.IPromise<Holoholo.Rpc.UseItemEncounterOutProto>","Holoholo.Rpc.Item","Niantic.Holoholo.Map.IItemEncounterContext");
    void* args[]={&berry,target.get()};Root promise(invoke_method(method,bag.get(),args));
    require(bool(promise),"Berry returned no promise; outcome unknown");return promise;
}
int map_real_poll_berry(void* promise,uint64_t tid,Root& response,int& status){
    int rc=map_poll_response(promise,tid,response);if(rc!=PGR_VR_DONE)return rc;
    require(response&&obj_class_name(response.get())=="UseItemEncounterOutProto","Unexpected berry response");
    const int raw=read_field_val<int>(response.get(),find_field_by_name_checked(obj_klass(response.get()),"Status"));
    status=raw==g_berry_success||raw==g_berry_active_exists?1:raw==g_berry_no_stock?2:0;
    if(status==1)require(map_real_active_berry(response.get())>0,"Berry response omitted effect");return rc;
}
bool map_real_value(PgrThrowValue& value){return pgr_map_throw_value(&value);}
int map_real_ball(void* raw,void* raw_response,const PgrCatchChanceOptions& cfg,const PgrThrowValue& value){
    Root target(raw),response(raw_response);
    if(cfg.autoball&&cfg.safari){
        const int safari=item_literal_opt("WildBall");
        Root display(call_obj(target.get(),"get_PokemonDisplay"));
        if(display&&safari>5){
            bool mighty=read_field_val<bool>(display.get(),find_field_by_name_checked(obj_klass(display.get()),"IsStrongPokemon","System.Boolean"));
            if(mighty&&map_stock(safari)>0&&pgr_catch_chance_offers_ball(response.get(),safari))return safari;
        }
    }
    double base[4]{};const bool rates=pgr_catch_chance_base(response.get(),base);
    int best=0;double best_chance=-1;
    // Server probabilities after a berry already include its effect.
    const double mult=(value.bullseye?2.0-value.radius:1.0)*(value.spinning?1.7:1.0);
    for(int item=1;item<=3;item++){
        if(map_stock(item)<=0)continue;
        if(!cfg.autoball)return item;
        if(!rates||base[item]<=0)continue;
        const double chance=1-std::pow(1-base[item],mult);
        if(chance*100>=cfg.target)return item;
        if(chance>best_chance){best=item;best_chance=chance;}
    }
    return best;
}
Root map_real_capture(void* raw,const PgrThrowValue& value){
    Root target(raw);
    require(value.item==1||value.item==2||value.item==3||pgr_is_inventory_safari_ball(value.item),"Unsupported map catch ball");
    require(map_stock(value.item)>0,"Selected ball no longer available");
    void* method=require_map_method(obj_klass(target.get()),"TryCapture",2,
        "Niantic.Promises.IPromise<Holoholo.Rpc.CatchPokemonOutProto>",
        "Niantic.Holoholo.Encounter.PokeballThrow","Holoholo.Rpc.ARPlusEncounterValuesProto");
    return invoke_auto_throw_capture(method,target,value);
}
int map_real_poll_capture(void* promise,uint64_t tid,int& status){Root hold(promise),response;return poll_auto_throw(hold,tid,response,status);}
void map_real_remove(void* raw){
    Root target(raw);
    // Every type needs this, wild spawns included: without it a caught wild spawn was still standing
    // (and being tapped) 20 s after its map catch (MuMu, 2026-11-13, maptap-test5).
    void* destroy=find_method_in_hierarchy(obj_klass(target.get()),"Destroy",0);
    if(!destroy){map_event("left-on-map",klass_name_of(obj_klass(target.get()))+": no removal call found");return;}
    invoke_method(destroy,target.get());
    map_event("removed",klass_name_of(obj_klass(target.get()))+" taken off the map");
}
const MapTapAdapter& map_tap_adapter(){
    static const MapTapAdapter adapter{real_read_account_id,map_real_eligible,map_real_id,map_real_alive,map_real_encounter,
        map_real_poll_encounter,map_real_active_berry,map_real_berry,map_real_poll_berry,map_real_value,map_real_ball,
        map_real_capture,map_real_poll_capture,map_real_remove};return adapter;
}
void map_tap_hook(void* self,void* sender,void* event,void* method){
    if(owner==uint64_t(gettid())&&map_tap.target){map_event("busy","Map catch already in progress; tap ignored");return;}
    if(owner==uint64_t(gettid())&&!map_hook_frame&&!busy){
        struct Guard{Guard(){map_hook_frame=true;}~Guard(){map_hook_frame=false;}} guard;
        try{
            const bool vgp_busy=map_vgp_busy(),command_free=map_command_free();
            // A tap during a VGP+ request waits for it instead of opening the encounter.
            if(map_tap_consume(self,map_gate&&map_hook_ready&&command_free,vgp_busy))return;
            if(map_mode_on(map_permission()))map_event("fallback",!map_gate?"Another automation owns the game":!command_free?"Another automation command is active":"Unsupported Pokemon type, cooldown, or map not ready");
        }
        catch(const Failure& f){map_event("fallback",f.where);if(map_tap.target){map_block(f.where);return;}}
        catch(...){map_event("fallback","Map tap unavailable; normal encounter retained");if(map_tap.target){map_block("Map tap failed");return;}}
    }
    if(map_original)map_original(self,sender,event,method);
}
void map_install_hook(){
    if(map_hook_attempted)return;map_hook_attempted=true;
    map_wild_class=resolve_class("Niantic.Holoholo.Map","WildMapPokemon");
    // Validate both RPCs before intercepting the first tap.
    require_map_method(map_wild_class,"SendEncounterRequest",0,"Niantic.Promises.IPromise<Niantic.Holoholo.Map.PokemonEncounterResponse>");
    require_map_method(map_wild_class,"TryCapture",2,"Niantic.Promises.IPromise<Holoholo.Rpc.CatchPokemonOutProto>","Niantic.Holoholo.Encounter.PokeballThrow","Holoholo.Rpc.ARPlusEncounterValuesProto");
    void* base=resolve_class("Niantic.Holoholo.Map","MapPokemon");
    g_pgr_vgp_marker_map_pokemon.store(base,std::memory_order_release);
    void* method=require_map_method(base,"OnTap",2,"System.Void","System.Object","System.EventArgs");
    void* code=nullptr;std::memcpy(&code,method,sizeof(code));
    auto api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));
    PgrMetaCodeQuery query{1,sizeof(query),code,16,nullptr,0,0};require(api&&api->validate_code(&query)==0,"Map tap code validation failed");
    require(pgrazor_hook(code,reinterpret_cast<void*>(map_tap_hook),reinterpret_cast<void**>(&map_original))==0&&map_original,"Map tap hook unavailable");
    map_hook_ready=true;map_event("ready","Map tap catch hook installed");
}

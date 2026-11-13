// Game 0.435.2 catch adapter; verified signatures in catch-owner-summary.txt.
int32_t g_premier_ball=0, g_beast_ball=0;
int32_t g_berry_success=0, g_berry_no_stock=0, g_berry_active_exists=0;
// --- il2cpp method type-introspection (metadata reads only) ---------------------------------
void* catch_method_return_type(void* m) {
    return fn<void* (*)(void*)>(exp_sym("method_get_return_type"))(m);
}
void* catch_method_param_type(void* m, uint32_t idx) {
    return fn<void* (*)(void*, uint32_t)>(exp_sym("method_get_param"))(m, idx);
}

// Require a method by name+argc (walking the hierarchy) AND validate its exact return type name.
void* require_method_ret(void* k, const char* name, int argc, const char* ret_type) {
    void* m = require_method(k, name, argc);
    require(type_name_of(catch_method_return_type(m)) == ret_type, name);
    return m;
}

// Resolve a Holoholo.Rpc.Item enum literal, returning -1 when absent. Never throws (soft).
int32_t item_literal_opt(const char* name) {
    void* item_k = resolve_class_optional("Holoholo.Rpc", "Item");
    if (!item_k) return -1;
    void* f = fn<void* (*)(void*, const char*)>(boot.meta.class_get_field_from_name)(item_k, name);
    if (!f) return -1;
    if ((fn<int (*)(void*)>(boot.meta.field_get_flags)(f) & 0x8050) != 0x8050) return -1;
    int32_t v = 0;
    fn<void (*)(void*, void*)>(boot.runtime.field_static_get_value)(f, &v);
    return v;
}

// ============================================================================================
// real_validate_catch_bindings: fail-closed check of every catch-flow binding (called from map
// real_ensure_ready after boot snapshot, before a pass is spent). Validates declaring class,
// Check the return, parameter and field types used by the catch path.
// ============================================================================================
void real_validate_catch_bindings() {
    require(boot_ready, "boot not initialised before catch-binding validation");

    void* gui_k    = resolve_class("Niantic.Holoholo.Encounter", "EncounterGuiController");
    void* state_k  = resolve_class("Niantic.Holoholo.Encounter", "EncounterState");
    void* raidpk_k = resolve_class("Niantic.Holoholo.Map", "RaidMapPokemon");
    void* resp_k   = resolve_class("Holoholo.Rpc", "CatchPokemonOutProto");
    void* sgui_k   = resolve_class("Niantic.Holoholo.Encounter", "EncounterSummaryGuiController");

    // EncounterGuiController.btuk() -> IEncounterState : shared-GUI encounter-state accessor.
    require_method_ret(gui_k, "btuk", 0, "Niantic.Holoholo.Encounter.IEncounterState");

    // EncounterState ownership + capture-completion surface.
    require_method_ret(state_k, "get_IsRaidOrIncidentEncounter", 0, "System.Boolean");
    require_method_ret(state_k, "get_IsEncounteredPokemonShiny", 0, "System.Boolean");
    require_method_ret(state_k, "get_MapPokemon", 0, "Niantic.Holoholo.Map.MapPokemon");
    require_method_ret(state_k, "get_EncounterDefaultBall", 0, "Holoholo.Rpc.Item");

    // RaidMapPokemon raid-boss identity (ownership marker, distinct from wild/incident MapPokemon).
    require_method_ret(raidpk_k, "get_EncounterId", 0, "System.UInt64");

    // CatchPokemonOutProto.Status enum field (manual-read fallback behind the phase-2 classifier).
    void* f_status = (md && md->fields[kStatusFieldIndex])
                         ? const_cast<void*>(md->fields[kStatusFieldIndex])
                         : find_field_by_name_checked(resp_k, "Status");
    require(f_status != nullptr && md && md->fields[kStatusFieldIndex], "CatchPokemonOutProto.Status field");

    // Summary dismissal: shared GuiController.Dismiss (inherited by EncounterSummaryGuiController).
    void* dismiss = (md && md->methods[1])
                        ? const_cast<void*>(md->methods[1])
                        : require_method(sgui_k, "Dismiss", 0);
    require(dismiss != nullptr && md->methods[1], "EncounterSummaryGuiController.Dismiss");

    // Resolve bonus balls by metadata; never substitute an inventory ball.
    g_premier_ball = item_literal_opt("PremierBall");
    g_beast_ball = item_literal_opt("BeastBall");
    require(g_premier_ball>0 && g_beast_ball>0,"Raid bonus ball metadata unavailable");
    require_method_ret(state_k, "get_EncounterStrategyReady", 0, "System.Boolean");
    require_method_ret(state_k, "get_Strategy", 0, "Niantic.Holoholo.Encounter.IEncounterStrategy");
    require_method_ret(state_k, "get_RaidBallCount", 0, "System.Int32");
    void* thrown=require_method_ret(state_k, "NotifyPokeballThrown", 1, "System.Void");
    require(type_name_of(catch_method_param_type(thrown,0))=="Holoholo.Rpc.Item","NotifyPokeballThrown parameter");
    void* complete=require_method_ret(state_k, "EncounterStateComplete", 1, "System.Void");
    require(type_name_of(catch_method_param_type(complete,0))=="Niantic.Holoholo.Encounter.EncounterResult","EncounterStateComplete parameter");
    read_enum_literal(resolve_class("Niantic.Holoholo.Encounter","EncounterResult"),"CapturedPokemon");
    read_enum_literal(resolve_class("Niantic.Holoholo.Encounter","EncounterResult"),"PokemonFled");
    void* capture=require_method_ret(raidpk_k, "TryCapture", 2,
           "Niantic.Promises.IPromise<Holoholo.Rpc.CatchPokemonOutProto>");
    require(type_name_of(catch_method_param_type(capture,0))=="Niantic.Holoholo.Encounter.PokeballThrow", "TryCapture throw type");
    require(type_name_of(catch_method_param_type(capture,1))=="Holoholo.Rpc.ARPlusEncounterValuesProto", "TryCapture AR type");
    find_field_by_name_checked(raidpk_k,"eljb","Holoholo.Rpc.RaidEncounterProto");
    void* proto=resolve_class("Holoholo.Rpc","RaidEncounterProto");
    find_field_by_name_checked(proto,"FortId","System.String");
    find_field_by_name_checked(proto,"EncounterId","System.Int64");
    require_method_ret(sgui_k,"btim",0,"Niantic.Holoholo.Encounter.IEncounterState");
}

// ============================================================================================
// Resolve the live EncounterState through a real live Unity controller's metadata-backed
// accessor. The EncounterState itself is never located with FindObjectsOfType.
// ============================================================================================
Root resolve_live_encounter_state() {
    // Primary: the shared encounter GUI controller is a live MonoBehaviour during any encounter.
    if (void* gui_k = resolve_class_optional("Niantic.Holoholo.Encounter", "EncounterGuiController")) {
        if (Root gui = find_live_unity_object(gui_k)) {
            if (find_method_in_hierarchy(obj_klass(gui.get()), "btuk", 0)) {
                if (Root st = Root(call_obj(gui.get(), "btuk", 0))) {
                    if (obj_class_name(st.get()) == "EncounterState") return st;
                }
            }
        }
    }
    // Fallback: the Coordinated2 raid encounter view exposes the same IEncounterState accessor.
    if (void* coord_k = resolve_class_optional("Niantic.Holoholo.Encounter", "Coordinated2RaidEncounter")) {
        if (Root coord = find_live_unity_object(coord_k)) {
            if (find_method_in_hierarchy(obj_klass(coord.get()), "btzb", 0)) {
                if (Root st = Root(call_obj(coord.get(), "btzb", 0))) {
                    if (obj_class_name(st.get()) == "EncounterState") return st;
                }
            }
        }
    }
    return {};
}

// io_state is the session's EncounterState root; no global managed root survives stop.
bool real_poll_raid_encounter(Root& io_state, Root& io_pokemon, bool& shiny, int& ball) {
    shiny=false; ball=0;
    Root state=resolve_live_encounter_state();
    if(!state || !unbox_val<bool>(call_obj(state.get(),"get_IsRaidOrIncidentEncounter",0)) ||
       !unbox_val<bool>(call_obj(state.get(),"get_EncounterStrategyReady",0))) return false;
    Root pokemon(call_obj(state.get(),"get_MapPokemon",0));
    if(!pokemon || obj_class_name(pokemon.get())!="RaidMapPokemon" || !unity_obj_alive(pokemon.get())) return false;
    Root proto(read_field_val<void*>(pokemon.get(),find_field_by_name_checked(obj_klass(pokemon.get()),"eljb","Holoholo.Rpc.RaidEncounterProto")));
    require(bool(proto),"Raid catch identity unavailable");
    Root fort(read_field_val<void*>(proto.get(),find_field_by_name_checked(obj_klass(proto.get()),"FortId","System.String")));
    require(!session.target.fort_id.empty() && read_managed_string(fort.get())==session.target.fort_id,
            "Bonus encounter belongs to another gym; Auto-Raid stopped");
    const auto id=read_field_val<int64_t>(proto.get(),find_field_by_name_checked(obj_klass(proto.get()),"EncounterId","System.Int64"));
    require(id!=0 && static_cast<uint64_t>(id)==unbox_val<uint64_t>(call_obj(pokemon.get(),"get_EncounterId",0)),"Raid encounter identity mismatch");
    if(io_pokemon) require(io_pokemon.get()==pokemon.get(),"Raid encounter changed during catch");
    shiny=unbox_val<bool>(call_obj(state.get(),"get_IsEncounteredPokemonShiny",0));
    ball=unbox_val<int32_t>(call_obj(state.get(),"get_EncounterDefaultBall",0));
    require(ball>0 && (ball==g_premier_ball || ball==g_beast_ball),"Unsupported raid ball; no inventory ball used");
    io_state=std::move(state);io_pokemon=std::move(pokemon);
    return true;
}

Root real_submit_raid_throw(void* state, void* pokemon, int ball) {
    require(state && state==session.coord_encounter.get() && pokemon==session.raid_map_pokemon.get()
            && unity_obj_alive(pokemon),"Raid catch owner unavailable");
    require(ball>0 && (ball==g_premier_ball || ball==g_beast_ball),"Raid ball not authorized");
    require(unbox_val<int32_t>(call_obj(state,"get_RaidBallCount",0))>0,"No bonus balls remain; check encounter");
    Root promise=submit_auto_throw(session.coord_encounter,session.raid_map_pokemon,pgr_raid_throw_value(ball));
    emit_raw_event("{\"token\":"+json_quote(std::to_string(session.command.token))+
        ",\"phase\":\"throw\",\"throw\":"+std::to_string(session.throws_made)+
        ",\"ball\":"+std::to_string(ball)+",\"grade\":\"excellent\",\"curve\":true,\"alwaysHit\":true,\"missed\":false,\"reticle\":0.1}");
    return promise;
}

// Use the game's inventory service with the RAID encounter context, rather than
// constructing a wild-encounter berry RPC. Its own continuation updates inventory.
bool real_ensure_berry_ready() {
    void* gui=resolve_class("Niantic.Holoholo.Encounter","EncounterGuiController");
    require_map_method(gui,"btul",0,"Niantic.Holoholo.Encounter.IEncounterInteractionState");
    void* interaction=resolve_class("Niantic.Holoholo.Encounter","EncounterInteractionState");
    require_map_method(interaction,"get_ActiveBerry",0,"Holoholo.Rpc.Item");
    require_map_method(interaction,"set_ActiveBerry",1,"System.Void","Holoholo.Rpc.Item");
    require_map_method(interaction,"get_CanUseBerry",0,"System.Boolean");
    require_map_method(interaction,"get_IsInPhotoMode",0,"System.Boolean");
    require_map_method(interaction,"get_PokeballInPlay",0,"System.Boolean");
    require_map_method(interaction,"set_CaptureProbability",1,"System.Void","Holoholo.Rpc.CaptureProbabilityProto");
    void* bag=resolve_class("Niantic.Holoholo.Internal","ItemBagImpl");
    require_map_method(bag,"UseItemForEncounter",2,"Niantic.Promises.IPromise<Holoholo.Rpc.UseItemEncounterOutProto>",
                       "Holoholo.Rpc.Item","Niantic.Holoholo.Map.IItemEncounterContext");
    require_map_method(bag,"GetItemCount",1,"System.Int32","Holoholo.Rpc.Item");
    void* response=resolve_class("Holoholo.Rpc","UseItemEncounterOutProto");
    void* status=find_field_by_name_checked(response,"Status");
    void* status_class=fn<void*(*)(void*)>(exp_sym("class_from_type"))(
        fn<void*(*)(void*)>(boot.meta.field_get_type)(status));
    g_berry_success=read_enum_literal(status_class,"Success");
    g_berry_no_stock=read_enum_literal(status_class,"NoItemInInventory");
    g_berry_active_exists=read_enum_literal(status_class,"ActiveItemExists");
    find_field_by_name_checked(response,"ActiveItem","Holoholo.Rpc.Item");
    find_field_by_name_checked(response,"CaptureProbability","Holoholo.Rpc.CaptureProbabilityProto");
    require(item_literal_opt("RazzBerry")==701 && item_literal_opt("GoldenRazzBerry")==706,"Raid berry item metadata mismatch");
    require(fn<bool(*)(void*,void*)>(boot.meta.class_is_assignable_from)(
        resolve_class("Niantic.Holoholo.Map","IItemEncounterContext"),
        resolve_class("Niantic.Holoholo.Map","RaidMapPokemon")),"Raid berry context unsupported");
    return true;
}

Root owned_berry_interaction(void* state,void* pokemon) {
    require(state && state==session.coord_encounter.get() && pokemon && pokemon==session.raid_map_pokemon.get(),
            "Raid berry owner unavailable");
    Root gui=find_live_unity_object(resolve_class("Niantic.Holoholo.Encounter","EncounterGuiController"));
    if(!gui)return {};
    Root owner(call_obj(gui.get(),"btuk",0));
    require(owner && owner.get()==state,"Raid berry belongs to another encounter");
    Root interaction(call_obj(gui.get(),"btul",0));
    // This is a managed state, not a Unity scene object. The live GUI and its
    // exact EncounterState owner establish liveness; keep the state GC-rooted.
    if(!interaction)return {};
    require(obj_class_name(interaction.get())=="EncounterInteractionState","Unsupported raid berry interaction");
    return interaction;
}

bool real_inspect_raid_berry(void* state,void* pokemon,RaidBerrySnapshot& out) {
    out={};
    Root interaction=owned_berry_interaction(state,pokemon);
    if(!interaction)return false;
    session.berry_interaction=std::move(interaction);
    void* scope=session.berry_interaction.get();
    out.active=unbox_val<int32_t>(call_obj(scope,"get_ActiveBerry",0));
    out.can_feed=unbox_val<bool>(call_obj(scope,"get_CanUseBerry",0)) &&
                 !unbox_val<bool>(call_obj(scope,"get_IsInPhotoMode",0)) &&
                 !unbox_val<bool>(call_obj(scope,"get_PokeballInPlay",0));
    Root bag=resolve_di_service("Niantic.Holoholo","IItemBag");
    require(bool(bag),"Raid berry inventory unavailable");
    int32_t item=701;void* args[]={&item};
    out.razz=unbox_val<int32_t>(call_obj(bag.get(),"GetItemCount",1,args));
    item=706;out.golden=unbox_val<int32_t>(call_obj(bag.get(),"GetItemCount",1,args));
    require(out.razz>=0 && out.golden>=0,"Raid berry inventory count invalid");
    return true;
}

Root real_submit_raid_berry(void* state,void* pokemon,int item) {
    require(item==701 || item==706,"Unauthorized raid berry");
    Root interaction=owned_berry_interaction(state,pokemon);
    require(interaction && interaction.get()==session.berry_interaction.get(),"Raid berry interaction changed");
    Root bag=resolve_di_service("Niantic.Holoholo","IItemBag");
    require(bool(bag),"Raid berry inventory unavailable");
    void* method=require_map_method(obj_klass(bag.get()),"UseItemForEncounter",2,
        "Niantic.Promises.IPromise<Holoholo.Rpc.UseItemEncounterOutProto>","Holoholo.Rpc.Item","Niantic.Holoholo.Map.IItemEncounterContext");
    void* args[]={&item,pokemon};
    return Root(invoke_method(method,bag.get(),args));
}

int real_poll_berry_promise(void* promise,uint64_t tid,int& result) {
    result=BerryRefused;
    const auto* vr=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));
    require(vr && vr->poll,"Raid berry promise observer unavailable");
    PgrVgpResultsResult response{};
    const int rc=vr->poll(promise,PGR_VR_PHASE_ENCOUNTER,tid,&response);
    Root value;value.h=response.value_handle;
    if(rc!=PGR_VR_DONE)return rc;
    require(value && obj_class_name(value.get())=="UseItemEncounterOutProto","Unexpected raid berry response");
    void* k=obj_klass(value.get());
    const int status=read_field_val<int32_t>(value.get(),find_field_by_name_checked(k,"Status"));
    emit_raw_event("{\"token\":"+json_quote(std::to_string(session.command.token))+
        ",\"phase\":\"berry-result\",\"item\":"+std::to_string(session.berry_item)+",\"status\":"+std::to_string(status)+"}");
    if(status==g_berry_no_stock){result=BerryNoStock;return rc;}
    if(status!=g_berry_success && status!=g_berry_active_exists)return rc;
    Root current=owned_berry_interaction(session.coord_encounter.get(),session.raid_map_pokemon.get());
    require(current && current.get()==session.berry_interaction.get(),"Raid berry owner changed before result");
    // Mirror the confirmed-result property updates in EncounterInteractionState.dyo.bswy.
    // A pre-existing server effect is also sufficient; do not send another berry.
    int item=read_field_val<int32_t>(value.get(),find_field_by_name_checked(k,"ActiveItem","Holoholo.Rpc.Item"));
    if(status==g_berry_success)require(item>0,"Accepted raid berry omitted active effect");
    if(item>0){void* args[]={&item};call_obj(current.get(),"set_ActiveBerry",1,args);}
    Root probability(read_field_val<void*>(value.get(),find_field_by_name_checked(k,"CaptureProbability","Holoholo.Rpc.CaptureProbabilityProto")));
    if(probability){void* args[]={probability.get()};call_obj(current.get(),"set_CaptureProbability",1,args);}
    result=BerryAccepted;return rc;
}

void real_clear_raid_berry() {
    if(!session.berry_interaction)return;
    Root current=owned_berry_interaction(session.coord_encounter.get(),session.raid_map_pokemon.get());
    require(current && current.get()==session.berry_interaction.get(),"Raid berry owner changed after breakout");
    int32_t none=0;void* args[]={&none};call_obj(current.get(),"set_ActiveBerry",1,args);
}

void finish_owned_raid_encounter(bool caught){
    require(bool(session.coord_encounter),"Raid encounter owner lost");
    int32_t result=read_enum_literal(resolve_class("Niantic.Holoholo.Encounter","EncounterResult"),caught?"CapturedPokemon":"PokemonFled");
    void* args[]={&result};call_obj(session.coord_encounter.get(),"EncounterStateComplete",1,args);
}

int real_poll_catch_promise(void* promise, uint64_t tid, int& out_catch_status) {
    out_catch_status = 0;
    const auto* vr = static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));
    require(vr != nullptr && vr->poll != nullptr, "Promise observer API unavailable");

    // Phase 2 classifies an exact CatchPokemonOutProto and reads its Status via the installed
    // RESP.Status FieldInfo; falls back to the verified Status field if not classified.
    PgrVgpResultsResult res{};
    const int rc = vr->poll(promise, PGR_VR_PHASE_CATCH, tid, &res);
    if (rc == PGR_VR_PENDING) return PGR_VR_PENDING;

    Root val;
    val.h = res.value_handle; // adopt the single rooted completed value exactly once
    if (rc != PGR_VR_DONE || !val) return PGR_VR_REJECTED;

    require(obj_class_name(val.get())=="CatchPokemonOutProto","Unexpected raid catch response");
    out_catch_status=read_field_val<int32_t>(val.get(),const_cast<void*>(md->fields[kStatusFieldIndex]));
    require(bool(session.coord_encounter),"Raid catch owner lost before completion");
    const int balls=unbox_val<int32_t>(call_obj(session.coord_encounter.get(),"get_RaidBallCount",0));
    emit_raw_event("{\"token\":"+json_quote(std::to_string(session.command.token))+",\"phase\":\"catch-result\",\"throw\":"+std::to_string(session.throws_made)+",\"status\":"+std::to_string(out_catch_status)+",\"bonusBallsRemaining\":"+std::to_string(balls)+"}");
    if(out_catch_status==1){
        // Complete the owned encounter with the confirmed game result. CaptureCompleted
        // opens EncounterViewPokemonState (the Pokemon detail page), not the catch summary.
        // Auto-Raid returns to the map through the game's completion/cleanup path instead.
        finish_owned_raid_encounter(true);
    }else if(out_catch_status==3){
        finish_owned_raid_encounter(false);
    }else if((out_catch_status==2 || out_catch_status==4) &&
             balls==0){
        finish_owned_raid_encounter(false);out_catch_status=3;
    }
    return PGR_VR_DONE;
}

void real_close_encounter_summary(){
    Root gui=find_live_unity_object(resolve_class("Niantic.Holoholo.Encounter","EncounterSummaryGuiController"));
    if(!gui)return;
    Root owner(call_obj(gui.get(),"btim",0));
    if(!owner || owner.get()!=session.coord_encounter.get())return;
    invoke_method(const_cast<void*>(md->methods[1]),gui.get(),nullptr);
}

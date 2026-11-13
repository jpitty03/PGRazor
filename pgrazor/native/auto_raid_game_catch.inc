// Game 0.435.2 catch adapter; verified signatures in catch-owner-summary.txt.
int32_t g_premier_ball=0, g_beast_ball=0;
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
    Root strategy(call_obj(state,"get_Strategy",0));
    require(bool(strategy),"Raid encounter strategy unavailable");
    void* om=require_method_ret(obj_klass(strategy.get()),"get_OverridesAttemptCapture",0,"System.Boolean");
    bool overrides=unbox_val<bool>(invoke_method(om,strategy.get(),nullptr));
    void* capture=require_method_ret(overrides?obj_klass(strategy.get()):obj_klass(pokemon),
        overrides?"AttemptCapture":"TryCapture",overrides?1:2,
        "Niantic.Promises.IPromise<Holoholo.Rpc.CatchPokemonOutProto>");
    require(type_name_of(catch_method_param_type(capture,0))=="Niantic.Holoholo.Encounter.PokeballThrow","Raid throw ABI mismatch");
    alignas(8) PgrThrowValue tv{};
    tv.item=ball;tv.radius=0.1f;tv.bullseye=1;tv.spinning=1;
    // NotifyThrown decrements supplied balls. NotifyHit dispatches capture listeners;
    // do not send it as well as TryCapture and risk a duplicate request.
    void* thrown_args[]={&ball};call_obj(state,"NotifyPokeballThrown",1,thrown_args);
    void* args[]={&tv,nullptr};
    Root promise(invoke_method(capture,overrides?strategy.get():pokemon,args));
    require(bool(promise),"Raid capture returned no promise; check encounter (not retried)");
    return promise;
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

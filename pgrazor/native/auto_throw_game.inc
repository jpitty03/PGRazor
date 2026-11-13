Root invoke_auto_throw_capture(void* method,Root& target,const PgrThrowValue& requested) {
    alignas(8) PgrThrowValue value=requested;void* args[]={&value,nullptr};
    Root promise(invoke_method(method,target.get(),args));
    require(bool(promise),"Capture returned no promise; outcome unknown (not retried)");return promise;
}

// Shared capture primitive, included in the encounter automation translation unit.
// Callers own authorization, ball stock, encounter identity, berries and cadence.
// All method signatures are checked BEFORE NotifyPokeballThrown spends a ball.
Root submit_auto_throw(Root& state, Root& pokemon, const PgrThrowValue& requested) {
    int ball=requested.item;
    require(state && pokemon && ball>0,"Auto Throw owner unavailable");
    Root state_owner(state.get()),pokemon_owner(pokemon.get());
    Root strategy(call_obj(state_owner.get(),"get_Strategy"));
    require(bool(strategy),"Encounter strategy unavailable");
    void* overrides_method=require_map_method(obj_klass(strategy.get()),"get_OverridesAttemptCapture",0,"System.Boolean");
    const bool overrides=unbox_val<bool>(invoke_method(overrides_method,strategy.get()));
    void* capture=require_map_method(overrides?obj_klass(strategy.get()):obj_klass(pokemon_owner.get()),
        overrides?"AttemptCapture":"TryCapture",overrides?1:2,
        "Niantic.Promises.IPromise<Holoholo.Rpc.CatchPokemonOutProto>",
        "Niantic.Holoholo.Encounter.PokeballThrow",overrides?nullptr:"Holoholo.Rpc.ARPlusEncounterValuesProto");
    void* thrown=require_map_method(obj_klass(state_owner.get()),"NotifyPokeballThrown",1,"System.Void","Holoholo.Rpc.Item");
    alignas(8) PgrThrowValue value=requested;
    void* thrown_args[]={&ball};invoke_method(thrown,state_owner.get(),thrown_args);
    require(state.get()==state_owner.get() && pokemon.get()==pokemon_owner.get(),"Encounter exited while notifying throw; capture cancelled");
    // NotifyPokeballHit dispatches another capture request: never call it here.
    return invoke_auto_throw_capture(capture,overrides?strategy:pokemon_owner,value);
}

int poll_auto_throw(Root& promise,uint64_t tid,Root& value,int& status) {
    const auto* api=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));
    require(api && api->poll,"Capture promise observer unavailable");
    PgrVgpResultsResult result{};
    const int rc=api->poll(promise.get(),PGR_VR_PHASE_CATCH,tid,&result);
    value.h=result.value_handle;
    if(rc!=PGR_VR_DONE)return rc;
    require(value && obj_class_name(value.get())=="CatchPokemonOutProto","Unexpected capture response");
    status=read_field_val<int32_t>(value.get(),find_field_by_name_checked(obj_klass(value.get()),"Status"));
    return rc;
}

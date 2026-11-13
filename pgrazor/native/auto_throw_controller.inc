// Manual encounter controller. Shares rooted IL2CPP helpers and submit_auto_throw
// with Auto-Raid, but no raid command, pass, target, or session state.
struct ManualThrowSession {
    Root interaction,state,pokemon,promise;
    std::string account,last_message;
    int attempts=0,berry_item=0;
    bool blocked=false,uncertain=false,berry_prepared=false;
    int64_t next_ms=0,promise_ms=0;
};
ManualThrowSession manual_throw;
uint64_t manual_throw_generation=0;

void throw_event(const char* phase,const std::string& message,int value=0) {
    auto tr=transport();if(!tr||!tr->emit)return;
    const std::string json="{\"phase\":"+json_quote(phase)+",\"message\":"+json_quote(message)+
        ",\"throw\":"+std::to_string(manual_throw.attempts)+",\"value\":"+std::to_string(value)+"}";
    PgrRuntimeTransportString strings[]={{"auto-throw",10},{json.c_str(),uint32_t(json.size())}};
    tr->emit(19,nullptr,0,strings,2);
}
void throw_wait(const char* reason) {
    if(manual_throw.last_message!=reason){manual_throw.last_message=reason;throw_event("waiting",reason);}
}
void throw_block(const std::string& reason) {
    manual_throw.blocked=true;manual_throw.promise.reset();
    throw_event("stopped",reason);
}

struct ManualThrowPermission {PgrCatchChanceOptions config;bool foreground=false,coherent=false;};
ManualThrowPermission throw_permission() {
    ManualThrowPermission p;auto tr=transport();
    if(!tr||!tr->sample||!tr->get_string||!tr->get_bool)return p;
    PgrRuntimeTransportSample a{},z{};int foreground=0;char raw[96]{};uint32_t n=0;
    if(tr->sample(&a)||!a.snapshot_fresh||a.fault)return p;
    if(tr->get_string(15,raw,sizeof(raw),&n)||n>=sizeof(raw)||tr->get_bool(3,&foreground))return p;
    raw[n]=0;
    if(tr->sample(&z)||!z.snapshot_fresh||z.fault||a.snapshot_seq!=z.snapshot_seq)return p;
    p.config=pgr_parse_catch_chance(raw);p.coherent=p.config.valid;p.foreground=foreground==1;
    return p;
}

bool manual_throw_owner() {
    Root gui=find_live_unity_object(resolve_class("Niantic.Holoholo.Encounter","EncounterGuiController"));
    if(!gui)return false;
    Root interaction(call_obj(gui.get(),"btul"));
    if(!interaction||interaction.get()!=manual_throw.interaction.get())return false;
    Root state(call_obj(gui.get(),"btuk"));
    if(!state)return false;
    if(manual_throw.state && state.get()!=manual_throw.state.get())return false;
    if(!manual_throw.state)manual_throw.state=std::move(state);
    Root pokemon(call_obj(manual_throw.state.get(),"get_MapPokemon"));
    if(!pokemon||!unity_obj_alive(pokemon.get()))return false;
    if(manual_throw.pokemon && pokemon.get()!=manual_throw.pokemon.get())return false;
    if(!manual_throw.pokemon)manual_throw.pokemon=std::move(pokemon);
    const std::string account=real_read_account_id();
    if(manual_throw.account.empty())manual_throw.account=account;
    return manual_throw.account==account;
}

void finish_manual_throw(bool caught) {
    const int32_t result=read_enum_literal(resolve_class("Niantic.Holoholo.Encounter","EncounterResult"),caught?"CapturedPokemon":"PokemonFled");
    int32_t mutable_result=result;void* args[]={&mutable_result};
    // Completion can synchronously invoke ExitState. Hold an independent root.
    Root state(manual_throw.state.get());manual_throw.blocked=true;
    call_obj(state.get(),"EncounterStateComplete",1,args);
}

void poll_manual_throw(uint64_t tid,int64_t now) {
    const uint64_t generation=manual_throw_generation;
    Root response;int result=0;int rc=PGR_VR_PENDING;
    if(manual_throw.berry_item){
        const auto* api=static_cast<const PgrVgpResultsApi*>(pgrazor_vgp_results_api(1));
        require(api&&api->poll,"Berry promise observer unavailable");
        PgrVgpResultsResult out{};rc=api->poll(manual_throw.promise.get(),PGR_VR_PHASE_ENCOUNTER,tid,&out);response.h=out.value_handle;
    }else rc=poll_auto_throw(manual_throw.promise,tid,response,result);
    if(generation!=manual_throw_generation)return;
    if(rc==PGR_VR_PENDING){
        if(now-manual_throw.promise_ms>=kPromiseTimeoutMs)throw_block("Request timed out; outcome unknown, no retry in this encounter");
        return;
    }
    require(rc==PGR_VR_DONE && response,"Request rejected; no automatic retry");
    manual_throw.promise.reset();manual_throw.uncertain=false;
    if(manual_throw.berry_item){
        require(obj_class_name(response.get())=="UseItemEncounterOutProto","Unexpected berry response");
        const int status=read_field_val<int32_t>(response.get(),find_field_by_name_checked(obj_klass(response.get()),"Status"));
        manual_throw.berry_item=0;
        throw_event("berry-result","Berry response",status);
        require(status==g_berry_success||status==g_berry_active_exists||status==g_berry_no_stock,"Berry refused; check encounter");
        if(status!=g_berry_no_stock){
            int active=read_field_val<int32_t>(response.get(),find_field_by_name_checked(obj_klass(response.get()),"ActiveItem","Holoholo.Rpc.Item"));
            require(active>0,"Berry response omitted active effect");
            void* args[]={&active};call_obj(manual_throw.interaction.get(),"set_ActiveBerry",1,args);
            Root probability(read_field_val<void*>(response.get(),find_field_by_name_checked(obj_klass(response.get()),"CaptureProbability","Holoholo.Rpc.CaptureProbabilityProto")));
            if(probability){void* args2[]={probability.get()};call_obj(manual_throw.interaction.get(),"set_CaptureProbability",1,args2);}
        }
        manual_throw.berry_prepared=true;manual_throw.next_ms=now+500;return;
    }
    throw_event("catch-result","Confirmed capture response",result);
    if(result==1||result==3){finish_manual_throw(result==1);return;}
    require(result==2||result==4,"Unknown catch result; no automatic retry");
    if(unbox_val<bool>(call_obj(manual_throw.state.get(),"get_IsRaidOrIncidentEncounter")) &&
       unbox_val<int32_t>(call_obj(manual_throw.state.get(),"get_RaidBallCount"))<=0){finish_manual_throw(false);return;}
    manual_throw.berry_prepared=false;
    if(result==2){int none=0;void* args[]={&none};call_obj(manual_throw.interaction.get(),"set_ActiveBerry",1,args);}
    manual_throw.next_ms=now+kThrowCadenceMs;
    // Direct capture doesn't animate a ball in flight. Re-arm Auto Ball explicitly.
    pgr_catch_chance_enter(manual_throw.interaction.get());
}

void step_manual_throw(uint64_t tid,bool raid_owns_game) {
    const auto permission=throw_permission();
    if(!permission.coherent||!permission.foreground||raid_owns_game){
        if(manual_throw.uncertain&&!manual_throw.blocked)throw_block("Automation ownership or foreground lost while request pending; no retry");
        return;
    }
    if(!manual_throw.interaction||manual_throw.blocked)return;
    if(!permission.config.autothrow&&!manual_throw.promise)return;
    const int64_t now=clock_ms();if(now<manual_throw.next_ms)return;
    manual_throw.next_ms=now+250;
    if(!manual_throw_owner()){
        if(manual_throw.state)throw_block("Encounter or account changed; request ownership lost");
        else throw_wait("Waiting for encounter owner");
        return;
    }
    if(manual_throw.promise){poll_manual_throw(tid,now);return;}
    if(permission.config.cooldown_ms>0){throw_wait("Waiting for cooldown");return;}
    if(manual_throw.attempts>=100){throw_block("Throw limit reached; reopen encounter to continue");return;}
    if(!unbox_val<bool>(call_obj(manual_throw.state.get(),"get_EncounterStrategyReady"))||
       unbox_val<bool>(call_obj(manual_throw.interaction.get(),"get_IsInPhotoMode"))||
       unbox_val<bool>(call_obj(manual_throw.interaction.get(),"get_PokeballInPlay"))){throw_wait("Waiting for encounter controls or current throw");return;}
    Root ball(call_obj(manual_throw.interaction.get(),"get_Pokeball"));
    if(!ball){throw_wait("Waiting for selected ball");return;}
    int item=unbox_val<int32_t>(call_obj(ball.get(),"get_BallType"));
    const int premier=item_literal_opt("PremierBall"),beast=item_literal_opt("BeastBall"),safari=item_literal_opt("WildBall");
    const bool supplied=item==premier||item==beast;
    if(item<=0){throw_wait("Waiting for selected ball");return;}
    if(!supplied&&item!=1&&item!=2&&item!=3&&item!=safari){throw_wait("Selected ball is not supported for automatic throwing (Master Ball excluded)");return;}
    Root bag=resolve_di_service("Niantic.Holoholo","IItemBag");require(bool(bag),"Inventory unavailable");
    void* item_args[]={&item};
    const int stock=supplied?unbox_val<int32_t>(call_obj(manual_throw.state.get(),"get_RaidBallCount")):
        unbox_val<int32_t>(call_obj(bag.get(),"GetItemCount",1,item_args));
    if(stock<=0){throw_wait("No selected balls remain");return;}
    if(permission.config.berry&&!manual_throw.berry_prepared&&
       !unbox_val<int32_t>(call_obj(manual_throw.interaction.get(),"get_ActiveBerry"))){
        if(!unbox_val<bool>(call_obj(manual_throw.interaction.get(),"get_CanUseBerry"))){throw_wait("Waiting for berry controls");return;}
        int berry=permission.config.berry;void* count_args[]={&berry};
        if(unbox_val<int32_t>(call_obj(bag.get(),"GetItemCount",1,count_args))>0){
            real_ensure_berry_ready();
            void* use=require_map_method(obj_klass(bag.get()),"UseItemForEncounter",2,
                "Niantic.Promises.IPromise<Holoholo.Rpc.UseItemEncounterOutProto>","Holoholo.Rpc.Item","Niantic.Holoholo.Map.IItemEncounterContext");
            manual_throw.berry_item=berry;manual_throw.uncertain=true;manual_throw.promise_ms=now;
            void* args[]={&berry,manual_throw.pokemon.get()};manual_throw.promise=Root(invoke_method(use,bag.get(),args));
            require(bool(manual_throw.promise),"Berry request returned no promise; not retried");
            throw_event("berry","Applying selected berry",berry);return;
        }
    }
    PgrThrowValue value{};
    const auto* throws=static_cast<const PgrThrowApi*>(pgrazor_throws_api(1));
    require(pgr_manual_throw_value(throws,ball.get(),item,value),"Throw settings unavailable; no throw sent");
    manual_throw.uncertain=true;manual_throw.promise_ms=now;++manual_throw.attempts;
    const uint64_t generation=manual_throw_generation;
    Root promise=submit_auto_throw(manual_throw.state,manual_throw.pokemon,value);
    if(generation!=manual_throw_generation)return;
    manual_throw.promise=std::move(promise);
    PgrRuntimeTransportString action{"Ball throw",10};if(auto tr=transport())tr->emit(17,nullptr,0,&action,1);
    manual_throw.last_message.clear();throw_event("throw",std::string("Always Hit; radius=")+std::to_string(value.radius)+
        "; curve="+(value.spinning?"true":"false"),item);
}

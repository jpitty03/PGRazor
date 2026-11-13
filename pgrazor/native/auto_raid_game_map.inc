// Game 0.435.2 map, gym, raid-entry, lobby, battle, and resolve adapter.
// Runtime type checks precede game actions. Static evidence and remaining live checks
// are recorded under .artifacts/auto-raid; fixture passes do not validate these bindings.
int32_t raid_free_item=0,raid_paid_item=0,raid_remote_item=0;
int32_t raid_location_both=0,raid_location_local=0,raid_location_remote=0;

int policy_raid_item(int32_t raw){
    if(raw==raid_free_item)return pgr_auto_raid::ITEM_FREE_RAID_TICKET;
    if(raw==raid_paid_item)return pgr_auto_raid::ITEM_PAID_RAID_TICKET;
    if(raw==raid_remote_item)return pgr_auto_raid::ITEM_REMOTE_RAID_TICKET;
    return 0;
}

void* map_method_return_type(void* m) {
    return fn<void* (*)(void*)>(exp_sym("method_get_return_type"))(m);
}

void* map_method_param_type(void* m, uint32_t idx) {
    return fn<void* (*)(void*, uint32_t)>(exp_sym("method_get_param"))(m, idx);
}

void* require_map_method(void* k,
                         const char* name,
                         int argc,
                         const char* ret_type,
                         const char* p0 = nullptr,
                         const char* p1 = nullptr) {
    const std::string owner=klass_name_of(k);
    auto methods=fn<void*(*)(void*,void**)>(exp_sym("class_get_methods"));
    auto method_name=fn<const char*(*)(void*)>(boot.meta.method_get_name);
    auto param_count=fn<uint32_t(*)(void*)>(exp_sym("method_get_param_count"));
    auto parent=fn<void*(*)(void*)>(exp_sym("class_get_parent"));
    for(int depth=0;k&&depth<12;++depth,k=parent(k)){
        void* iter=nullptr;void* found=nullptr;
        while(void* m=methods(k,&iter)){
            const char* n=method_name(m);
            if(!n||std::strcmp(n,name)||param_count(m)!=static_cast<uint32_t>(argc))continue;
            if(ret_type&&type_name_of(map_method_return_type(m))!=ret_type)continue;
            if(p0&&(argc<1||type_name_of(map_method_param_type(m,0))!=p0))continue;
            if(p1&&(argc<2||type_name_of(map_method_param_type(m,1))!=p1))continue;
            require(!found,"Ambiguous raid method signature");found=m;
        }
        if(found)return found;
    }
    throw Failure{owner+"."+name+" signature unavailable"};
}

void* require_map_field(void* k,
                        const char* field_name,
                        const char* expected_type = nullptr,
                        size_t expected_offset = 0) {
    void* f = find_field_by_name_checked(k, field_name, expected_type);
    if (expected_offset != 0) {
        const size_t actual_off = fn<size_t (*)(void*)>(boot.meta.field_get_offset)(f);
        if(actual_off!=expected_offset)throw Failure{klass_name_of(k)+"."+field_name+" offset mismatch: expected "+std::to_string(expected_offset)+", got "+std::to_string(actual_off)};
    }
    return f;
}

int normalize_runtime_raid_level(int32_t raw_level) {
    if (md && md->raid_tier_count > 0) {
        const uint32_t n = std::min<uint32_t>(md->raid_tier_count, static_cast<uint32_t>(kRaidTierMax));
        for (uint32_t i = 0; i < n; ++i) {
            if (md->raid_tiers[i].value == raw_level) {
                return pgr_auto_raid::normalize_raid_level(md->raid_tiers[i].name);
            }
        }
    }
    if (void* level_k = resolve_class_optional("Holoholo.Rpc", "RaidLevel")) {
        auto get_fields = fn<void* (*)(void*, void**)>(exp_sym("class_get_fields"));
        auto get_flags  = fn<int (*)(void*)>(boot.meta.field_get_flags);
        auto get_name   = fn<const char* (*)(void*)>(exp_sym("field_get_name"));
        void* iter = nullptr;
        while (void* f = get_fields(level_k, &iter)) {
            if ((get_flags(f) & 0x8050) != 0x8050) continue;
            int32_t val = 0;
            fn<void (*)(void*, void*)>(boot.runtime.field_static_get_value)(f, &val);
            if (val == raw_level) {
                const char* fname = get_name(f);
                if (fname) return pgr_auto_raid::normalize_raid_level(fname);
            }
        }
    }
    return pgr_auto_raid::RAID_LEVEL_UNSET;
}

// Read RepeatedField<T> elements via its verified internal fields ("array" and "count", as in map_poi.cpp).
int32_t read_repeated_field_items(void* repeated_field, int32_t max_items, std::vector<Root>& out_items) {
    out_items.clear();
    if (!repeated_field) return 0;
    void* rf_k = obj_klass(repeated_field);
    void* f_arr = require_map_field(rf_k, "array", nullptr, 0);
    void* f_cnt = require_map_field(rf_k, "count", "System.Int32", 0);
    Root arr(read_field_val<void*>(repeated_field, f_arr));
    int32_t count = read_field_val<int32_t>(repeated_field, f_cnt);
    require(count>=0 && count<=max_items,"Raid collection count unsupported; refusing partial inspection");
    if (count == 0) return 0;
    require(bool(arr),"Raid collection array unavailable");

    const size_t arr_len = fn<size_t (*)(void*)>(exp_sym("array_length"))(arr.get());
    require(static_cast<size_t>(count)<=arr_len,"Raid collection count exceeds its array");
    for (int32_t i = 0; i < count; ++i) {
        void** elems = reinterpret_cast<void**>(static_cast<char*>(arr.get()) + 32);
        Root elem(elems[i]);
        require(bool(elem),"Null raid collection entry; refusing partial inspection");
        out_items.push_back(std::move(elem));
    }
    return static_cast<int32_t>(out_items.size());
}

bool real_ensure_ready() {
    if (!boot_ready) {
        const auto* api = static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));
        boot.version = 1;
        boot.size = sizeof(boot);
        require(md != nullptr && api != nullptr && api->snapshot(&boot) == 0, "Runtime bootstrap unavailable");
        boot_ready = true;
    }

    void* item_enum=resolve_class("Holoholo.Rpc","Item");
    raid_free_item=read_enum_literal(item_enum,"FreeRaidTicket");
    raid_paid_item=read_enum_literal(item_enum,"PaidRaidTicket");
    raid_remote_item=read_enum_literal(item_enum,"RemoteRaidTicket");
    require(raid_free_item>0&&raid_paid_item>0&&raid_remote_item>0&&
            raid_free_item!=raid_paid_item&&raid_free_item!=raid_remote_item&&raid_paid_item!=raid_remote_item,"Raid pass enum identity invalid");
    void* location_enum=resolve_class("Holoholo.Rpc","RaidLocationRequirement");
    raid_location_both=read_enum_literal(location_enum,"Both");
    raid_location_local=read_enum_literal(location_enum,"InPerson");
    raid_location_remote=read_enum_literal(location_enum,"Remote");
    // 1. Account & server clock classes/methods.
    void* player_svc_k = resolve_class("Niantic.Holoholo", "PlayerService");
    require_map_method(player_svc_k, "get_PlayerId", 0, "System.String");

    void* time_util_k = resolve_class("Niantic.Holoholo", "TimeUtil");
    require_map_method(time_util_k, "ServerNowMs", 0, "System.Int64");
    require_map_method(time_util_k, "NowMs", 0, "System.Int64");

    // 2. Loaded MapGym & Fort/Raid proto classes/fields/methods.
    void* map_gym_k   = resolve_class("Niantic.Holoholo.Gym", "MapGym");
    void* fort_k      = resolve_class("Holoholo.Rpc", "PokemonFortProto");
    void* raid_info_k = resolve_class("Holoholo.Rpc", "RaidInfoProto");
    void* pk_proto_k  = resolve_class("Holoholo.Rpc", "PokemonProto");

    require_map_method(map_gym_k, "get_FortData", 0, "Holoholo.Rpc.PokemonFortProto");
    require_map_method(map_gym_k, "OnTapped", 0, "System.Void");
    require_map_field(map_gym_k, "dyvy", "Niantic.Holoholo.Gym.IGymRootController", 0x158);

    require_map_field(fort_k, "FortId", "System.String", 0x10);
    require_map_field(fort_k, "Latitude", "System.Double", 0x20);
    require_map_field(fort_k, "Longitude", "System.Double", 0x28);
    require_map_field(fort_k, "Enabled", "System.Boolean", 0x3c);
    require_map_field(fort_k, "Closed", "System.Boolean", 0x88);
    require_map_field(fort_k, "RaidInfo", "Holoholo.Rpc.RaidInfoProto", 0x90);

    require_map_field(raid_info_k, "RaidSeed", "System.Int64", 0x10);
    require_map_field(raid_info_k, "RaidSpawnMs", "System.Int64", 0x18);
    require_map_field(raid_info_k, "RaidBattleMs", "System.Int64", 0x20);
    require_map_field(raid_info_k, "RaidEndMs", "System.Int64", 0x28);
    require_map_field(raid_info_k, "RaidPokemon", "Holoholo.Rpc.PokemonProto", 0x30);
    require_map_field(raid_info_k, "RaidLevel", "Holoholo.Rpc.RaidLevel", 0x38);
    require_map_field(raid_info_k, "Complete", "System.Boolean", 0x3c);
    require_map_field(raid_info_k, "IsRaidHidden", "System.Boolean", 0x3d);
    require_map_field(raid_info_k, "IsFree", "System.Boolean", 0x3f);
    require_map_method(raid_info_k, "get_IsMegaRaid", 0, "System.Boolean");
    require_map_method(raid_info_k, "get_IsLegendaryMegaRaid", 0, "System.Boolean");
    require_map_method(raid_info_k, "get_IsPrimalRaid", 0, "System.Boolean");
    require_map_method(raid_info_k, "get_IsShadowRaid", 0, "System.Boolean");

    require_map_field(pk_proto_k, "pokemonId_", "System.Int32", 0x28);
    require_map_method(pk_proto_k, "get_PokemonId", 0, "System.Int32");

    // 3. GymRootController, GymRootState, RaidState, and RaidApproachUIController.
    void* gym_ctrl_k  = resolve_class("Niantic.Holoholo.Gym", "GymRootController");
    void* gym_state_k = resolve_class("Niantic.Holoholo.Gym", "GymRootState");
    void* raid_st_k   = resolve_class("Niantic.Holoholo.Raid", "RaidState");
    void* approach_k  = resolve_class("Niantic.Holoholo.Raid", "RaidApproachUIController");
    void* details_k   = resolve_class("Holoholo.Rpc", "RaidDetailsContainer");

    require_map_field(gym_ctrl_k, "dyhl", "Niantic.Holoholo.Gym.IGymRootState", 0x30);
    require_map_method(gym_ctrl_k, "get_IsActive", 0, "System.Boolean");
    require_map_method(gym_ctrl_k, "get_FortId", 0, "System.String");
    require_map_method(gym_ctrl_k, "get_HasRaidInfo", 0, "System.Boolean");
    require_map_method(gym_ctrl_k, "get_RaidInfo", 0, "Holoholo.Rpc.RaidInfoProto");
    require_map_method(gym_ctrl_k, "get_IsPlayerInRange", 0, "System.Boolean");

    require_map_method(gym_state_k, "SwitchToRaidState", 0, "System.Void");
    require_map_method(gym_state_k, "ExitState", 0, "System.Void");
    require_map_method(gym_state_k, "get_Exiting", 0, "System.Boolean");

    require_map_method(raid_st_k, "ExitState", 0, "System.Void");

    require_map_method(approach_k, "OnJoinButtonPressed", 0, "System.Void");
    require_map_method(approach_k, "OnBackButtonPressed", 0, "System.Void");
    require_map_field(approach_k, "ekir", "Niantic.Holoholo.Gym.IGymRootController", 0x3b0);
    require_map_field(approach_k, "ekis", "Niantic.Holoholo.Gym.IGymRootState", 0x3b8);
    require_map_field(approach_k, "ekit", "Niantic.Holoholo.Raid.IRaidState", 0x3c0);
    require_map_field(approach_k, "ekjv", "Niantic.Holoholo.IItemBag", 0x4a0);
    require_map_field(approach_k, "ekjz", "Niantic.Holoholo.Services.IRaidEntryCostService", 0x4c0);
    require_map_field(approach_k, "ekkk", "Niantic.Holoholo.UI.Raids.IUseRaidTicketGuiController", 0x518);
    require_map_field(approach_k, "ekkp", "System.Boolean", 0x52b);
    require_map_field(approach_k, "ekkr", "System.Boolean", 0x52d);
    require_map_field(approach_k, "ekkz", "Holoholo.Rpc.RaidDetailsContainer", 0x560);

    require_map_method(details_k, "get_HasValidInfo", 0, "System.Boolean");
    require_map_method(details_k, "get_RaidSeed", 0, "System.Int64");
    require_map_method(details_k, "get_RaidLevel", 0, "Holoholo.Rpc.RaidLevel");
    require_map_method(details_k, "get_TicketUsed", 0, "System.Boolean");
    require_map_method(details_k, "get_RemoteTicketUsed", 0, "System.Boolean");
    require_map_method(details_k, "get_RaidComplete", 0, "System.Boolean");

    // 4. RaidEntryCostService, ItemBagImpl, and cost protos.
    void* cost_svc_k  = resolve_class("Niantic.Holoholo.Services", "RaidEntryCostService");
    void* item_bag_k  = resolve_class("Niantic.Holoholo.Internal", "ItemBagImpl");
    void* lvl_cost_k  = resolve_class("Holoholo.Rpc", "RaidLevelEntryCostProto");
    void* ent_cost_k  = resolve_class("Holoholo.Rpc", "RaidEntryCostProto");
    void* item_req_k  = resolve_class("Holoholo.Rpc", "ItemRequirementProto");

    require_map_method(cost_svc_k, "GetRaidLevelEntryCost", 1,
                       "Holoholo.Rpc.RaidLevelEntryCostProto", "Holoholo.Rpc.RaidLevel");
    require_map_method(cost_svc_k, "MeetsEntryCost", 1,
                       "System.Boolean", "Holoholo.Rpc.RaidEntryCostProto");
    require_map_method(cost_svc_k, "FirstMissingCost", 1,
                       "Holoholo.Rpc.ItemRequirementProto", "Holoholo.Rpc.RaidEntryCostProto");
    require_map_method(item_bag_k, "GetItemCount", 1, "System.Int32", "Holoholo.Rpc.Item");

    require_map_field(lvl_cost_k, "RaidEntryCost",
                      "Niantic.Protobuf.Collections.RepeatedField<Holoholo.Rpc.RaidEntryCostProto>", 0x18);
    require_map_field(ent_cost_k, "RaidType", "Holoholo.Rpc.RaidLocationRequirement", 0x10);
    require_map_field(ent_cost_k, "ItemRequirement",
                      "Niantic.Protobuf.Collections.RepeatedField<Holoholo.Rpc.ItemRequirementProto>", 0x18);
    require_map_field(item_req_k, "Item", "Holoholo.Rpc.Item", 0x10);
    require_map_field(item_req_k, "Count", "System.Int32", 0x14);

    // 5. UseRaidTicketGuiController & UseRaidEntryCostWidget.
    void* modal_k  = resolve_class("Niantic.Holoholo.UI.Raids", "UseRaidTicketGuiController");
    void* widget_k = resolve_class("Niantic.Holoholo.UI.Raids", "UseRaidEntryCostWidget");
    void* gui_k    = resolve_class("Niantic.Holoholo.NUI", "GuiController");

    require_map_method(modal_k, "CycleCurrency", 0, "System.Void");
    require_map_method(modal_k, "StartAnimation", 0, "System.Void");
    require_map_method(modal_k, "Dismiss", 0, "System.Void");
    require_map_field(modal_k, "eeiq", "Niantic.Holoholo.Services.IRaidEntryCostService", 0x1e8);
    require_map_field(modal_k, "eeiw", "Holoholo.Rpc.RaidEntryCostProto", 0x208);
    require_map_field(modal_k, "eeix",
                      "System.Collections.Generic.List<Niantic.Holoholo.UI.Raids.IUseRaidEntryCostWidget>", 0x210);
    require_map_field(modal_k, "eejb", "System.Boolean", 0x230);
    require_map_method(widget_k, "OnItemTapped", 0, "System.Void");

    // 6. RaidLobbyGuiController & LobbyProto.
    void* lobby_k       = resolve_class("Niantic.Holoholo.Raid", "RaidLobbyGuiController");
    void* lobby_proto_k = resolve_class("Holoholo.Rpc", "LobbyProto");

    require_map_field(lobby_k,"ekrr","Niantic.Holoholo.Gym.IGymRootController",0x348);
    require_map_method(lobby_k, "get_BattleStarted", 0, "System.Boolean");
    require_map_method(lobby_k, "LeaveLobby", 0, "System.Void");
    require_map_field(lobby_k, "ektq", "System.Int32", 0x4c8);
    require_map_field(lobby_k, "ektx", "Holoholo.Rpc.LobbyProto", 0x4e8);
    require_map_field(lobby_k, "ekty", "Holoholo.Rpc.RaidDetailsContainer", 0x4f0);

    require_map_field(lobby_proto_k, "Players",
                      "Niantic.Protobuf.Collections.RepeatedField<Holoholo.Rpc.BattleParticipantProto>", 0x20);
    require_map_field(lobby_proto_k, "RaidBattleStartMs", "System.Int64", 0x38);
    require_map_field(lobby_proto_k, "TotalPlayerCountInLobby", "System.Int32", 0x8c);

    // 7. RaidResolveUIController & GuiController.
    void* resolve_k = resolve_class("Niantic.Holoholo.Raid", "RaidResolveUIController");
    require_map_field(gui_k, "customBackButtonAction", "System.Action", 0x28);
    require_map_field(gui_k, "currentState", nullptr, 0x70);
    require_map_method(gui_k, "get_IsOpen", 0, "System.Boolean");

    require_map_field(resolve_k, "confirmBonusRow", nullptr, 0x250);
    require_map_field(resolve_k, "elcj", "Niantic.Holoholo.Battle.BattleStatus", 0x2e4);
    require_map_field(resolve_k, "elcv", "System.Boolean", 0x340);
    require_map_method(resolve_k, "CloseLeaderboard", 0, "System.Void");
    require_map_method(resolve_k, "CancelAreaTapped", 0, "System.Void");
    require_map_method(resolve_k, "Close", 0, "System.Void");

    // 8. Validate catch-flow bindings implemented in auto_raid_game_catch.inc.
    real_validate_catch_bindings();
    return true;
}

std::string real_read_account_id() {
    Root player = resolve_di_service("Niantic.Holoholo", "IPlayerService");
    require(bool(player), "PlayerService unavailable");
    void* m = require_map_method(obj_klass(player.get()), "get_PlayerId", 0, "System.String");
    Root id_str(invoke_method(m, player.get(), nullptr));
    std::string id = read_managed_string(id_str.get());
    require(!id.empty(), "Player account ID unavailable");
    return id;
}

int64_t real_read_game_time_ms() {
    if (void* time_util_k = resolve_class_optional("Niantic.Holoholo", "TimeUtil")) {
        if (void* m = find_method_in_hierarchy(time_util_k, "ServerNowMs", 0)) {
            const int64_t ms = unbox_val<int64_t>(invoke_method(m, nullptr, nullptr));
            if (ms > 0) return ms;
        }
        if (void* m2 = find_method_in_hierarchy(time_util_k, "NowMs", 0)) {
            const int64_t ms = unbox_val<int64_t>(invoke_method(m2, nullptr, nullptr));
            if (ms > 0) return ms;
        }
    }
    return wall_clock_ms();
}

bool real_map_ready(){
    Root gym=resolve_di_service("Niantic.Holoholo.Gym","IGymRootController");
    if(!gym || unbox_val<bool>(call_obj(gym.get(),"get_IsActive",0)))return false;
    Root encounter=find_live_unity_object(resolve_class("Niantic.Holoholo.Encounter","EncounterGuiController"));
    if(encounter && unbox_val<bool>(call_obj(encounter.get(),"get_IsOpen",0)))return false;
    Root summary=find_live_unity_object(resolve_class("Niantic.Holoholo.Encounter","EncounterSummaryGuiController"));
    return !summary || !unbox_val<bool>(call_obj(summary.get(),"get_IsOpen",0));
}

void real_scan_gyms(std::vector<pgr_auto_raid::RaidCandidate>& out_candidates,
                    std::vector<Root>& out_gym_roots) {
    out_candidates.clear();
    out_gym_roots.clear();

    void* map_gym_k = (md && md->opt_classes[OC_MAP_GYM])
                          ? const_cast<void*>(md->opt_classes[OC_MAP_GYM])
                          : resolve_class("Niantic.Holoholo.Gym", "MapGym");
    void* find_m = (md && md->opt_handles[H_VGP_M_FIND_ALL])
                       ? const_cast<void*>(md->opt_handles[H_VGP_M_FIND_ALL])
                       : require_map_method(resolve_class("UnityEngine", "Object"),
                                            "FindObjectsOfType", 1, "UnityEngine.Object[]", "System.Type");
    void* get_fort_m = (md && md->opt_handles[H_VGP_M_FORT])
                           ? const_cast<void*>(md->opt_handles[H_VGP_M_FORT])
                           : require_map_method(map_gym_k, "get_FortData", 0, "Holoholo.Rpc.PokemonFortProto");

    void* fort_k      = resolve_class("Holoholo.Rpc", "PokemonFortProto");
    void* raid_info_k = resolve_class("Holoholo.Rpc", "RaidInfoProto");
    void* pk_proto_k  = resolve_class("Holoholo.Rpc", "PokemonProto");

    void* f_fort_id   = require_map_field(fort_k, "FortId", "System.String", 0x10);
    void* f_lat       = require_map_field(fort_k, "Latitude", "System.Double", 0x20);
    void* f_lon       = require_map_field(fort_k, "Longitude", "System.Double", 0x28);
    void* f_enabled   = require_map_field(fort_k, "Enabled", "System.Boolean", 0x3c);
    void* f_closed    = require_map_field(fort_k, "Closed", "System.Boolean", 0x88);
    void* f_raid_info = require_map_field(fort_k, "RaidInfo", "Holoholo.Rpc.RaidInfoProto", 0x90);

    void* f_seed      = require_map_field(raid_info_k, "RaidSeed", "System.Int64", 0x10);
    void* f_spawn     = require_map_field(raid_info_k, "RaidSpawnMs", "System.Int64", 0x18);
    void* f_battle    = require_map_field(raid_info_k, "RaidBattleMs", "System.Int64", 0x20);
    void* f_end       = require_map_field(raid_info_k, "RaidEndMs", "System.Int64", 0x28);
    void* f_boss_pk   = require_map_field(raid_info_k, "RaidPokemon", "Holoholo.Rpc.PokemonProto", 0x30);
    void* f_level     = require_map_field(raid_info_k, "RaidLevel", "Holoholo.Rpc.RaidLevel", 0x38);
    void* f_complete  = require_map_field(raid_info_k, "Complete", "System.Boolean", 0x3c);
    void* f_hidden    = require_map_field(raid_info_k, "IsRaidHidden", "System.Boolean", 0x3d);
    void* f_free      = require_map_field(raid_info_k, "IsFree", "System.Boolean", 0x3f);

    void* m_mega      = require_map_method(raid_info_k, "get_IsMegaRaid", 0, "System.Boolean");
    void* m_leg_mega  = require_map_method(raid_info_k, "get_IsLegendaryMegaRaid", 0, "System.Boolean");
    void* m_primal    = require_map_method(raid_info_k, "get_IsPrimalRaid", 0, "System.Boolean");
    void* m_shadow    = require_map_method(raid_info_k, "get_IsShadowRaid", 0, "System.Boolean");

    void* f_pk_id     = require_map_field(pk_proto_k, "pokemonId_", "System.Int32", 0x28);

    Root gym_type = type_object_of(map_gym_k);
    void* find_args[] = {gym_type.get()};
    Root arr(invoke_method(find_m, nullptr, find_args));
    if (!arr) return;

    const int64_t now_ms = real_read_game_time_ms();
    const size_t n = std::min<size_t>(fn<size_t (*)(void*)>(exp_sym("array_length"))(arr.get()), 256);
    size_t live=0, forts=0, disabled=0, closed_count=0;

    for (size_t i = 0; i < n; ++i) {
        void** elems = reinterpret_cast<void**>(static_cast<char*>(arr.get()) + 32);
        Root gym(elems[i]);
        if (!gym || obj_klass(gym.get()) != map_gym_k || !unity_obj_alive(gym.get())) continue;
        ++live;

        void* concrete_fort=fn<void*(*)(void*,void*)>(exp_sym("object_get_virtual_method"))(gym.get(),get_fort_m);
        Root fort(invoke_method(concrete_fort, gym.get(), nullptr));
        if (!fort || obj_klass(fort.get()) != fort_k) continue;
        ++forts;

        const bool enabled = read_field_val<uint8_t>(fort.get(), f_enabled) != 0;
        const bool closed  = read_field_val<uint8_t>(fort.get(), f_closed) != 0;
        if(!enabled)++disabled;
        if(closed)++closed_count;
        // Like gym_runtime, do not reject the raw Closed flag: live active raid
        // gyms can carry it. The game's approach/entry flow enforces closure.
        if (!enabled) continue;

        Root fort_id_str(read_field_val<void*>(fort.get(), f_fort_id));
        pgr_auto_raid::RaidCandidate c{};
        c.fort_id = read_managed_string(fort_id_str.get());
        if (c.fort_id.empty()) continue;

        c.latitude  = read_field_val<double>(fort.get(), f_lat);
        c.longitude = read_field_val<double>(fort.get(), f_lon);

        Root raid_info(read_field_val<void*>(fort.get(), f_raid_info));
        if (raid_info && obj_klass(raid_info.get()) == raid_info_k) {
            c.raid_seed      = read_field_val<int64_t>(raid_info.get(), f_seed);
            c.spawn_ms       = read_field_val<int64_t>(raid_info.get(), f_spawn);
            c.battle_ms      = read_field_val<int64_t>(raid_info.get(), f_battle);
            c.end_ms         = read_field_val<int64_t>(raid_info.get(), f_end);
            c.is_complete    = read_field_val<uint8_t>(raid_info.get(), f_complete) != 0;
            c.is_raid_hidden = read_field_val<uint8_t>(raid_info.get(), f_hidden) != 0;
            c.is_free        = read_field_val<uint8_t>(raid_info.get(), f_free) != 0;

            const int32_t raw_level = read_field_val<int32_t>(raid_info.get(), f_level);
            c.raid_level = normalize_runtime_raid_level(raw_level);

            c.is_mega           = unbox_val<bool>(invoke_method(m_mega, raid_info.get(), nullptr));
            c.is_legendary_mega = unbox_val<bool>(invoke_method(m_leg_mega, raid_info.get(), nullptr));
            c.is_primal         = unbox_val<bool>(invoke_method(m_primal, raid_info.get(), nullptr));
            c.is_shadow         = unbox_val<bool>(invoke_method(m_shadow, raid_info.get(), nullptr));

            Root boss_pk(read_field_val<void*>(raid_info.get(), f_boss_pk));
            if (boss_pk && obj_klass(boss_pk.get()) == pk_proto_k) {
                c.boss_pokemon_id = read_field_val<int32_t>(boss_pk.get(), f_pk_id);
            }

            const bool within_window = (c.raid_seed != 0 && !c.is_raid_hidden &&
                                        c.end_ms > now_ms &&
                                        (c.spawn_ms <= 0 || c.spawn_ms <= now_ms));
            c.has_active_raid     = within_window;
            c.is_raid_egg_active  = within_window && (c.battle_ms > now_ms);
            c.is_raid_boss_active = within_window && (c.battle_ms > 0 && c.battle_ms <= now_ms &&
                                                      c.boss_pokemon_id > 0);
        }

        out_candidates.push_back(std::move(c));
        out_gym_roots.push_back(std::move(gym));
    }
    static std::string previous;
    const std::string summary="\"found\":"+std::to_string(n)+",\"live\":"+std::to_string(live)+",\"forts\":"+std::to_string(forts)+",\"disabled\":"+std::to_string(disabled)+",\"closed\":"+std::to_string(closed_count);
    if(summary!=previous){
        previous=summary;
        emit_raw_event("{\"token\":"+json_quote(std::to_string(session.command.token))+",\"phase\":\"discovery-diagnostic\","+summary+"}");
    }
}

bool real_is_gym_alive(void* gym) {
    return unity_obj_alive(gym);
}

void real_click_gym_poi(void* gym) {
    require(unity_obj_alive(gym), "Target gym no longer loaded");
    void* m = require_map_method(obj_klass(gym), "OnTapped", 0, "System.Void");
    invoke_method(m, gym, nullptr);
}

bool gym_ctrl_matches_target(void* gym_ctrl) {
    if (!gym_ctrl || session.target.fort_id.empty()) return false;
    void* ctrl_k = obj_klass(gym_ctrl);
    if (!unbox_val<bool>(call_obj(gym_ctrl, "get_IsActive", 0))) return false;
    Root fort_id_str(call_obj(gym_ctrl, "get_FortId", 0));
    if (read_managed_string(fort_id_str.get()) != session.target.fort_id) return false;

    if (session.target.raid_seed != 0 &&
        find_method_in_hierarchy(ctrl_k, "get_HasRaidInfo", 0) &&
        unbox_val<bool>(call_obj(gym_ctrl, "get_HasRaidInfo", 0))) {
        Root raid_info(call_obj(gym_ctrl, "get_RaidInfo", 0));
        if (raid_info) {
            void* f_seed = require_map_field(obj_klass(raid_info.get()), "RaidSeed", "System.Int64", 0x10);
            const int64_t seed = read_field_val<int64_t>(raid_info.get(), f_seed);
            if (seed != 0 && seed != session.target.raid_seed) return false;
        }
    }
    return true;
}

bool real_advance_to_raid_approach(void* target_gym,
                                   Root& out_gym_root,
                                   Root& out_raid_state,
                                   Root& out_approach_ui) {
    void* approach_k = resolve_class("Niantic.Holoholo.Raid", "RaidApproachUIController");
    if (Root app = find_live_unity_object(approach_k)) {
        void* app_k = obj_klass(app.get());
        void* f_gym_ctrl   = require_map_field(app_k, "ekir", "Niantic.Holoholo.Gym.IGymRootController", 0x3b0);
        void* f_gym_state  = require_map_field(app_k, "ekis", "Niantic.Holoholo.Gym.IGymRootState", 0x3b8);
        void* f_raid_state = require_map_field(app_k, "ekit", "Niantic.Holoholo.Raid.IRaidState", 0x3c0);
        void* f_details    = require_map_field(app_k, "ekkz", "Holoholo.Rpc.RaidDetailsContainer", 0x560);

        Root gym_ctrl(read_field_val<void*>(app.get(), f_gym_ctrl));
        require(gym_ctrl && gym_ctrl_matches_target(gym_ctrl.get()),
                "Active RaidApproachUIController belongs to a different gym/raid");

        Root details(read_field_val<void*>(app.get(), f_details));
        if (!details || !unbox_val<bool>(call_obj(details.get(), "get_HasValidInfo", 0))) {
            return false; // Wait until RaidDetailsContainer has loaded valid raid info
        }
        if (session.target.raid_seed != 0) {
            const int64_t seed = unbox_val<int64_t>(call_obj(details.get(), "get_RaidSeed", 0));
            require(seed == session.target.raid_seed,
                    "Active RaidApproachUIController raid seed does not match target raid");
        }

        out_gym_root   = Root(read_field_val<void*>(app.get(), f_gym_state));
        out_raid_state = Root(read_field_val<void*>(app.get(), f_raid_state));
        out_approach_ui = std::move(app);
        return true;
    }

    Root gym_ctrl;
    if (target_gym && unity_obj_alive(target_gym)) {
        void* f_ctrl = require_map_field(obj_klass(target_gym), "dyvy", "Niantic.Holoholo.Gym.IGymRootController", 0x158);
        gym_ctrl = Root(read_field_val<void*>(target_gym, f_ctrl));
    }
    if (!gym_ctrl) {
        gym_ctrl = resolve_di_service("Niantic.Holoholo.Gym", "IGymRootController");
    }
    if (!gym_ctrl || !gym_ctrl_matches_target(gym_ctrl.get())) {
        return false;
    }

    void* f_gym_state = require_map_field(obj_klass(gym_ctrl.get()), "dyhl", "Niantic.Holoholo.Gym.IGymRootState", 0x30);
    Root gym_state(read_field_val<void*>(gym_ctrl.get(), f_gym_state));
    if (!gym_state) return false;

    if (unbox_val<bool>(call_obj(gym_state.get(), "get_Exiting", 0))) {
        return false;
    }
    out_gym_root = std::move(gym_state);
    // MapGym.OnTapped already starts the raid approach for an active raid.
    // Re-entering RaidState while that asynchronous transition is loading
    // replaces its callbacks and can close the pass modal without joining.
    // Wait for its owned approach UI instead of issuing another transition.
    return false;
}

pgr_auto_raid::EntryCostOption parse_managed_entry_cost(void* cost_svc, void* cost_proto) {
    pgr_auto_raid::EntryCostOption opt{};
    if (!cost_proto) return opt;

    void* cost_k = obj_klass(cost_proto);
    void* f_raid_type = require_map_field(cost_k, "RaidType", "Holoholo.Rpc.RaidLocationRequirement", 0x10);
    void* f_reqs      = require_map_field(
        cost_k, "ItemRequirement",
        "Niantic.Protobuf.Collections.RepeatedField<Holoholo.Rpc.ItemRequirementProto>", 0x18);

    const int32_t raw_location=read_field_val<int32_t>(cost_proto,f_raid_type);
    opt.raid_type=raw_location==raid_location_local?pgr_auto_raid::RAID_LOCATION_LOCAL:
        raw_location==raid_location_remote?pgr_auto_raid::RAID_LOCATION_REMOTE:
        raw_location==raid_location_both?pgr_auto_raid::RAID_LOCATION_UNSET:-1;
    Root reqs_rf(read_field_val<void*>(cost_proto, f_reqs));
    std::vector<Root> req_items;
    read_repeated_field_items(reqs_rf.get(), 8, req_items);
    for (const Root& req : req_items) {
        if (!req) continue;
        void* req_k = obj_klass(req.get());
        void* f_item  = require_map_field(req_k, "Item", "Holoholo.Rpc.Item", 0x10);
        void* f_count = require_map_field(req_k, "Count", "System.Int32", 0x14);
        pgr_auto_raid::CostItemRequirement item_req{};
        item_req.item_id = policy_raid_item(read_field_val<int32_t>(req.get(), f_item));
        item_req.count   = read_field_val<int32_t>(req.get(), f_count);
        opt.items.push_back(item_req);
    }

    if (cost_svc) {
        void* m_meets = require_map_method(
            obj_klass(cost_svc), "MeetsEntryCost", 1, "System.Boolean", "Holoholo.Rpc.RaidEntryCostProto");
        void* args[] = {cost_proto};
        const bool meets = unbox_val<bool>(invoke_method(m_meets, cost_svc, args));

        void* m_unmet = require_map_method(
            obj_klass(cost_svc), "FirstMissingCost", 1,
            "Holoholo.Rpc.ItemRequirementProto", "Holoholo.Rpc.RaidEntryCostProto");
        Root unmet(invoke_method(m_unmet, cost_svc, args));
        opt.satisfied_in_bag = meets && !bool(unmet);
    }
    return opt;
}

bool real_inspect_approach_costs(void* approach_ui,
                                 bool raid_is_free,
                                 bool& out_ticket_used,
                                 bool& out_remote_used,
                                 std::vector<pgr_auto_raid::EntryCostOption>& out_options) {
    out_options.clear();
    require(unity_obj_alive(approach_ui), "RaidApproachUIController destroyed");

    void* app_k = obj_klass(approach_ui);
    void* f_gym_ctrl    = require_map_field(app_k, "ekir", "Niantic.Holoholo.Gym.IGymRootController", 0x3b0);
    void* f_item_bag    = require_map_field(app_k, "ekjv", "Niantic.Holoholo.IItemBag", 0x4a0);
    void* f_cost_svc    = require_map_field(app_k, "ekjz", "Niantic.Holoholo.Services.IRaidEntryCostService", 0x4c0);
    void* f_ticket_used = require_map_field(app_k, "ekkp", "System.Boolean", 0x52b);
    void* f_remote_used = require_map_field(app_k, "ekkr", "System.Boolean", 0x52d);
    void* f_details     = require_map_field(app_k, "ekkz", "Holoholo.Rpc.RaidDetailsContainer", 0x560);

    Root gym_ctrl(read_field_val<void*>(approach_ui, f_gym_ctrl));
    require(gym_ctrl && gym_ctrl_matches_target(gym_ctrl.get()),
            "RaidApproachUIController no longer matches target gym");
    require(unbox_val<bool>(call_obj(gym_ctrl.get(), "get_IsPlayerInRange", 0)),
            "Player is not within local in-person range of the raid gym");

    Root details(read_field_val<void*>(approach_ui, f_details));
    if (!details || !unbox_val<bool>(call_obj(details.get(), "get_HasValidInfo", 0))) {
        return false;
    }
    require(!unbox_val<bool>(call_obj(details.get(), "get_RaidComplete", 0)),
            "Target raid is already marked completed");

    out_ticket_used = (read_field_val<uint8_t>(approach_ui, f_ticket_used) != 0) ||
                      unbox_val<bool>(call_obj(details.get(), "get_TicketUsed", 0));
    out_remote_used = (read_field_val<uint8_t>(approach_ui, f_remote_used) != 0) ||
                      unbox_val<bool>(call_obj(details.get(), "get_RemoteTicketUsed", 0));

    Root cost_svc(read_field_val<void*>(approach_ui, f_cost_svc));
    Root item_bag(read_field_val<void*>(approach_ui, f_item_bag));
    require(bool(cost_svc) && bool(item_bag), "RaidEntryCostService or IItemBag missing on RaidApproachUIController");

    int32_t raw_raid_level = unbox_val<int32_t>(call_obj(details.get(), "get_RaidLevel", 0));
    void* m_get_lvl_cost = require_map_method(
        obj_klass(cost_svc.get()), "GetRaidLevelEntryCost", 1,
        "Holoholo.Rpc.RaidLevelEntryCostProto", "Holoholo.Rpc.RaidLevel");
    void* lvl_args[] = {&raw_raid_level};
    Root lvl_cost_proto(invoke_method(m_get_lvl_cost, cost_svc.get(), lvl_args));
    if (!lvl_cost_proto) {
        return true;
    }

    void* f_costs_rf = require_map_field(
        obj_klass(lvl_cost_proto.get()), "RaidEntryCost",
        "Niantic.Protobuf.Collections.RepeatedField<Holoholo.Rpc.RaidEntryCostProto>", 0x18);
    Root costs_rf(read_field_val<void*>(lvl_cost_proto.get(), f_costs_rf));

    // Check FreeRaidTicket (1401) count in IItemBag:
    // RaidApproachUIController.OnJoinButtonPressed() -> clnw() checks GetItemCount(1401) >= 1 and,
    // when true, passes ONLY GenericFreeCost (1401) to UseRaidTicketGuiController (preventing cycling
    // to a Premium pass while a Free pass is in the bag).
    int32_t free_ticket_item = raid_free_item;
    void* m_item_cnt = require_map_method(
        obj_klass(item_bag.get()), "GetItemCount", 1, "System.Int32", "Holoholo.Rpc.Item");
    void* cnt_args[] = {&free_ticket_item};
    const int32_t free_ticket_count = unbox_val<int32_t>(invoke_method(m_item_cnt, item_bag.get(), cnt_args));

    // Match clnw's actual free-ticket override, including when the configured level cost
    // contains only Premium tickets. Never invent a free cost or authorize a paid fallback.
    void* ext=resolve_class("","RaidEntryCostSettingsExt");
    void* requires_ticket=require_map_method(ext,"RequiresRaidTicket",1,"System.Boolean","Holoholo.Rpc.RaidLevelEntryCostProto");
    void* requires_args[]={lvl_cost_proto.get()};
    if(free_ticket_count>0 && unbox_val<bool>(invoke_method(requires_ticket,nullptr,requires_args))){
        fn<void(*)(void*)>(exp_sym("runtime_class_init"))(ext);
        void* field=fn<void*(*)(void*,const char*)>(boot.meta.class_get_field_from_name)(ext,"GenericFreeCost");
        require(field && (fn<int(*)(void*)>(boot.meta.field_get_flags)(field)&0x10),"Generic free raid cost unavailable");
        require(type_name_of(fn<void*(*)(void*)>(boot.meta.field_get_type)(field))==
                "Niantic.Protobuf.Collections.RepeatedField<Holoholo.Rpc.RaidEntryCostProto>","Generic free raid cost type mismatch");
        void* free_cost=nullptr;fn<void(*)(void*,void*)>(boot.runtime.field_static_get_value)(field,&free_cost);
        costs_rf=Root(free_cost);require(bool(costs_rf),"Generic free raid cost missing");
    }

    std::vector<Root> cost_protos;
    read_repeated_field_items(costs_rf.get(), 16, cost_protos);
    for (const Root& cp : cost_protos) {
        if (!cp) continue;
        pgr_auto_raid::EntryCostOption opt = parse_managed_entry_cost(cost_svc.get(), cp.get());
        // Skip remote-only options from local entry consideration unless they are the only ones present,
        // and if a free raid pass is in bag while free_pass=false, OnJoinButtonPressed cannot select Premium.
        if (opt.raid_type == pgr_auto_raid::RAID_LOCATION_REMOTE) {
            continue;
        }
        if (free_ticket_count > 0 && !session.command.free_pass) {
            const auto kind = pgr_auto_raid::classify_entry_cost(opt, raid_is_free, false);
            if (kind == pgr_auto_raid::PassCostKind::PremiumLocal) {
                opt.satisfied_in_bag = false;
            }
        }
        out_options.push_back(std::move(opt));
    }

    (void)raid_is_free;
    return true;
}

void real_press_approach_join(void* approach_ui) {
    require(unity_obj_alive(approach_ui), "RaidApproachUIController unavailable for join");
    void* app_k = obj_klass(approach_ui);
    void* f_gym_ctrl = require_map_field(app_k, "ekir", "Niantic.Holoholo.Gym.IGymRootController", 0x3b0);
    Root gym_ctrl(read_field_val<void*>(approach_ui, f_gym_ctrl));
    require(gym_ctrl && gym_ctrl_matches_target(gym_ctrl.get()),
            "Refusing OnJoinButtonPressed: RaidApproachUIController does not match target gym");
    require(unbox_val<bool>(call_obj(gym_ctrl.get(), "get_IsPlayerInRange", 0)),
            "Refusing OnJoinButtonPressed: player out of local gym range");
    void* m = require_map_method(app_k, "OnJoinButtonPressed", 0, "System.Void");
    invoke_method(m, approach_ui, nullptr);
}

void real_exit_raid_or_gym(void* approach_ui, void* raid_state, void* gym_root_state) {
    (void)gym_root_state;
    if(approach_ui && unity_obj_alive(approach_ui) && unbox_val<bool>(call_obj(approach_ui,"get_IsOpen",0))){
        Root ctrl(read_field_val<void*>(approach_ui,require_map_field(obj_klass(approach_ui),"ekir","Niantic.Holoholo.Gym.IGymRootController",0x3b0)));
        require(ctrl && gym_ctrl_matches_target(ctrl.get()),"Refusing to close an unrelated raid");
        call_obj(approach_ui,"OnBackButtonPressed",0);
        return;
    }
    if(raid_state && obj_class_name(raid_state)=="RaidState"){
        Root ctrl(read_field_val<void*>(raid_state,require_map_field(obj_klass(raid_state),"eldb","Niantic.Holoholo.Gym.IGymRootController",0x98)));
        if(!ctrl || !gym_ctrl_matches_target(ctrl.get()))return;
        void* exit=require_map_method(obj_klass(raid_state),"ExitGym",2,"System.Void",
             "Holoholo.Rpc.RaidEncounterProto","Niantic.Holoholo.Raid.RaidResolveInformation");
        void* args[]={nullptr,nullptr};invoke_method(exit,raid_state,args);
    }
    // Never invoke ExitState directly: it is a lifecycle callback, not a navigation request.
}

bool real_inspect_ticket_modal(void* approach_ui,
                               Root& out_modal,
                               pgr_auto_raid::EntryCostOption& out_selected_cost,
                               bool& out_coin_flag) {
    out_modal.reset();
    if (approach_ui && unity_obj_alive(approach_ui)) {
        void* f_modal = require_map_field(
            obj_klass(approach_ui), "ekkk",
            "Niantic.Holoholo.UI.Raids.IUseRaidTicketGuiController", 0x518);
        Root modal(read_field_val<void*>(approach_ui, f_modal));
        if (modal && unity_obj_alive(modal.get())) {
            out_modal = std::move(modal);
        }
    }
    if (!out_modal) {
        out_modal = find_live_unity_object(
            resolve_class("Niantic.Holoholo.UI.Raids", "UseRaidTicketGuiController"));
    }
    if (!out_modal || !unity_obj_alive(out_modal.get())) return false;
    if (!unbox_val<bool>(call_obj(out_modal.get(), "get_IsOpen", 0))) return false;

    void* modal_k = obj_klass(out_modal.get());
    void* f_cost_svc      = require_map_field(modal_k, "eeiq", "Niantic.Holoholo.Services.IRaidEntryCostService", 0x1e8);
    void* f_selected_cost = require_map_field(modal_k, "eeiw", "Holoholo.Rpc.RaidEntryCostProto", 0x208);
    void* f_coin_flag     = require_map_field(modal_k, "eejb", "System.Boolean", 0x230);

    Root selected_proto(read_field_val<void*>(out_modal.get(), f_selected_cost));
    Root cost_svc(read_field_val<void*>(out_modal.get(), f_cost_svc));
    if (!selected_proto || !cost_svc) return false;

    out_coin_flag     = read_field_val<uint8_t>(out_modal.get(), f_coin_flag) != 0;
    out_selected_cost = parse_managed_entry_cost(cost_svc.get(), selected_proto.get());
    return true;
}

void real_cycle_modal_currency(void* modal) {
    require(unity_obj_alive(modal), "UseRaidTicketGuiController unavailable");
    void* m = require_map_method(obj_klass(modal), "CycleCurrency", 0, "System.Void");
    invoke_method(m, modal, nullptr);
}

void real_confirm_modal_ticket(void* modal) {
    require(unity_obj_alive(modal), "UseRaidTicketGuiController unavailable");
    void* modal_k = obj_klass(modal);

    require(unbox_val<bool>(call_obj(modal,"get_IsOpen",0)),"Raid pass modal is no longer open");
    Root data(read_field_val<void*>(modal,require_map_field(modal_k,"eeiv",nullptr,0x200)));
    require(bool(data),"Raid pass modal data unavailable");
    const bool returns_cost=read_field_val<uint8_t>(data.get(),require_map_field(obj_klass(data.get()),"ReturnChosenEntry","System.Boolean",0x18))!=0;
    emit_raw_event("{\"token\":"+json_quote(std::to_string(session.command.token))+",\"phase\":\"pass-confirm-diagnostic\",\"returnsSelectedCost\":"+(returns_cost?"true":"false")+"}");
    // StartAnimation is the normal controller confirmation path. It runs the
    // widget animation and InteractionComplete callback. cfmb only clears the
    // widget list and must never be used as a confirmation fallback.
    invoke_method(require_map_method(modal_k,"StartAnimation",0,"System.Void"),modal,nullptr);
}

void real_dismiss_modal_ticket(void* modal) {
    if (modal && unity_obj_alive(modal)) {
        try {
            void* m = require_map_method(obj_klass(modal), "Dismiss", 0, "System.Void");
            invoke_method(m, modal, nullptr);
        } catch (...) {}
    }
}

bool real_poll_lobby_state(Root& out_lobby_ui,
                           bool& out_battle_started,
                           int& out_player_count,
                           bool& out_has_countdown,
                           int64_t& out_countdown_ms) {
    if (!out_lobby_ui || !unity_obj_alive(out_lobby_ui.get())) {
        out_lobby_ui = find_live_unity_object(resolve_class("Niantic.Holoholo.Raid", "RaidLobbyGuiController"));
    }
    if (!out_lobby_ui || !unity_obj_alive(out_lobby_ui.get())) return false;

    void* ui = out_lobby_ui.get();
    void* ui_k = obj_klass(ui);
    Root ctrl(read_field_val<void*>(ui,require_map_field(ui_k,"ekrr","Niantic.Holoholo.Gym.IGymRootController",0x348)));
    if(!ctrl || !gym_ctrl_matches_target(ctrl.get()))return false;
    out_battle_started = unbox_val<bool>(call_obj(ui, "get_BattleStarted", 0));
    if(!out_battle_started && !unbox_val<bool>(call_obj(ui,"get_IsOpen",0)))return false;

    out_player_count  = 0;
    out_has_countdown = false;
    out_countdown_ms  = 0;

    void* f_lobby_proto = require_map_field(ui_k, "ektx", "Holoholo.Rpc.LobbyProto", 0x4e8);
    if (Root lobby_proto = Root(read_field_val<void*>(ui, f_lobby_proto))) {
        void* lp_k = obj_klass(lobby_proto.get());
        void* f_players=require_map_field(lp_k,"Players",
            "Niantic.Protobuf.Collections.RepeatedField<Holoholo.Rpc.BattleParticipantProto>",0x20);
        Root players(read_field_val<void*>(lobby_proto.get(),f_players));
        std::vector<Root> participants;
        out_player_count=read_repeated_field_items(players.get(),100,participants);
        const int total=read_field_val<int32_t>(lobby_proto.get(),require_map_field(lp_k,"TotalPlayerCountInLobby","System.Int32",0x8c));
        require(total>=0&&total<=100,"Raid lobby total player count invalid");
        out_player_count=std::max(out_player_count,total);
        void* f_battle_start = require_map_field(lp_k, "RaidBattleStartMs", "System.Int64", 0x38);
        const int64_t battle_start_ms = read_field_val<int64_t>(lobby_proto.get(), f_battle_start);
        const int64_t now_game_ms     = real_read_game_time_ms();
        if (battle_start_ms > 0 && now_game_ms > 0) {
            out_has_countdown = true;
            out_countdown_ms  = std::max<int64_t>(0, battle_start_ms - now_game_ms);
        }
    }

    if (out_player_count <= 0) {
        void* f_details = require_map_field(ui_k, "ekty", "Holoholo.Rpc.RaidDetailsContainer", 0x4f0);
        if (Root details = Root(read_field_val<void*>(ui, f_details))) {
            if (unbox_val<bool>(call_obj(details.get(), "get_HasValidInfo", 0))) {
                const int32_t num_p = unbox_val<int32_t>(call_obj(details.get(), "get_NumPlayersInLobby", 0));
                if (num_p > out_player_count) out_player_count = num_p;
            }
        }
    }
    return true;
}

void real_leave_lobby(void* lobby_ui, void* raid_state) {
    if(lobby_ui && unity_obj_alive(lobby_ui)){
        require(!unbox_val<bool>(call_obj(lobby_ui,"get_BattleStarted",0)),"Battle already started before lobby leave");
        call_obj(lobby_ui,"LeaveLobby",0);return;
    }
    require(raid_state && obj_class_name(raid_state)=="RaidState","Raid lobby owner unavailable; leave not confirmed");
    call_obj(raid_state,"LeaveLobby",0);
}

bool real_is_battle_active() {
    if(!session.lobby_ui || !unity_obj_alive(session.lobby_ui.get()))return false;
    Root ctrl(read_field_val<void*>(session.lobby_ui.get(),require_map_field(obj_klass(session.lobby_ui.get()),
            "ekrr","Niantic.Holoholo.Gym.IGymRootController",0x348)));
    if(!ctrl || !gym_ctrl_matches_target(ctrl.get()))return false;
    return unbox_val<bool>(call_obj(session.lobby_ui.get(),"get_BattleStarted",0));
}

bool real_poll_resolve_state(Root& out_resolve_ui, int& out_battle_status, bool& out_advanced_to_encounter) {
    out_advanced_to_encounter = false;
    if (!out_resolve_ui || !unity_obj_alive(out_resolve_ui.get())) {
        out_resolve_ui = find_live_unity_object(resolve_class("Niantic.Holoholo.Raid", "RaidResolveUIController"));
    }
    if (!out_resolve_ui || !unity_obj_alive(out_resolve_ui.get())) return false;

    void* gui = out_resolve_ui.get();
    void* gui_k = obj_klass(gui);
    void* f_state = require_map_field(resolve_class("Niantic.Holoholo.NUI", "GuiController"), "currentState", nullptr, 0x70);
    const int32_t gui_state = read_field_val<int32_t>(gui, f_state);
    if (gui_state != 1 && gui_state != 2) return false;

    void* f_closing = require_map_field(gui_k, "elcv", "System.Boolean", 0x340);
    if (read_field_val<uint8_t>(gui, f_closing) != 0) return false;

    void* f_status = require_map_field(gui_k, "elcj", "Niantic.Holoholo.Battle.BattleStatus", 0x2e4);
    const int32_t raw_status = read_field_val<int32_t>(gui, f_status);
    void* status_k = resolve_class("Niantic.Holoholo.Battle", "BattleStatus");
    const int victory_val   = read_enum_literal(status_k, "Victory");
    const int defeat_val    = read_enum_literal(status_k, "Defeat");
    const int timed_out_val = read_enum_literal(status_k, "TimedOut");
    const int quit_val      = read_enum_literal(status_k, "Quit");
    const int error_val     = read_enum_literal(status_k, "Error");

    if (raw_status == victory_val) {
        out_battle_status = pgr_auto_raid::BATTLE_STATUS_VICTORY;
    } else if (raw_status == timed_out_val) {
        out_battle_status = pgr_auto_raid::BATTLE_STATUS_TIMED_OUT;
        if (gui_state == 2) {
            try { call_obj(gui, "Close", 0); } catch (...) {}
        }
        return true;
    } else if (raw_status == defeat_val || raw_status == quit_val || raw_status == error_val) {
        out_battle_status = pgr_auto_raid::BATTLE_STATUS_DEFEAT;
        if (gui_state == 2) {
            try { call_obj(gui, "Close", 0); } catch (...) {}
        }
        return true;
    } else {
        out_battle_status = pgr_auto_raid::BATTLE_STATUS_ACTIVE;
        return true;
    }

    if (gui_state != 2) return true;

    // 1. If customBackButtonAction is CloseLeaderboard, invoke it first.
    void* f_back = require_map_field(
        resolve_class("Niantic.Holoholo.NUI", "GuiController"), "customBackButtonAction", "System.Action", 0x28);
    if (Root action = Root(read_field_val<void*>(gui, f_back))) {
        void* close_lb_m = require_map_method(gui_k, "CloseLeaderboard", 0, "System.Void");
        Root target(call_obj(action.get(), "get_Target", 0));
        Root refl(call_obj(action.get(), "get_Method", 0));
        if (target.get() == gui && refl) {
            void* raw_m = fn<void* (*)(void*)>(exp_sym("method_get_from_reflection"))(refl.get());
            if (raw_m == close_lb_m) {
                call_obj(action.get(), "Invoke", 0);
                return true;
            }
        }
    }

    // 2. Check confirmBonusRow.Root active button -> Press(), else CancelAreaTapped().
    void* f_confirm = require_map_field(gui_k, "confirmBonusRow", nullptr, 0x250);
    if (Root row = Root(read_field_val<void*>(gui, f_confirm))) {
        void* f_root = require_map_field(obj_klass(row.get()), "Root", "UnityEngine.GameObject", 0x10);
        if (Root root_go = Root(read_field_val<void*>(row.get(), f_root))) {
            if (unity_obj_alive(root_go.get()) &&
                unbox_val<bool>(call_obj(root_go.get(), "get_activeInHierarchy", 0))) {
                Root btn_type = type_object_of(resolve_class("UnityEngine.UI", "Button"));
                bool include_inactive = false;
                void* comp_args[] = {btn_type.get(), &include_inactive};
                Root btn(call_obj(root_go.get(), "GetComponentInChildren", 2, comp_args));
                if (btn && unity_obj_alive(btn.get()) &&
                    unbox_val<bool>(call_obj(btn.get(), "get_isActiveAndEnabled", 0)) &&
                    unbox_val<bool>(call_obj(btn.get(), "IsInteractable", 0))) {
                    call_obj(btn.get(), "Press", 0);
                    out_advanced_to_encounter = true;
                    return true;
                }
            }
        }
    }

    call_obj(gui, "CancelAreaTapped", 0);
    return true;
}

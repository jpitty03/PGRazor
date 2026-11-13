// Standalone runtime test fixture for auto_raid.cpp; exercises pgr_auto_raid_tick and
// pgr_auto_raid_stop across transport snapshots, pass accounting, cooldown gating,
// lobby bailout, consecutive losses, bonus catch encounter, shiny/max-raids completion,
// cancellation, and GC root lifecycle discipline.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "auto_raid.cpp"

namespace {

std::map<uintptr_t, void*> test_roots;
uintptr_t next_root_handle = 1;
unsigned root_allocations = 0;
unsigned root_frees = 0;

uintptr_t fake_gchandle_new(void* obj, int) {
    assert(obj != nullptr);
    const uintptr_t h = next_root_handle++;
    test_roots[h] = obj;
    ++root_allocations;
    return h;
}

void* fake_gchandle_get_target(uintptr_t h) {
    auto it = test_roots.find(h);
    assert(it != test_roots.end());
    return it->second;
}

void fake_gchandle_free(uintptr_t h) {
    assert(test_roots.erase(h) == 1);
    ++root_frees;
}

// Fake transport state
bool t_fresh = true;
bool t_fault = false;
bool t_changing_seq = false;
uint64_t t_seq = 100;
unsigned t_sample_calls = 0;
std::string t_command = "";
bool t_foreground = true;
bool t_item_access = true;
bool t_has_coords = true;
double t_lat = 37.7749;
double t_lon = -122.4194;
std::vector<std::string> t_events;

int fake_transport_sample(PgrRuntimeTransportSample* out) {
    *out = {};
    out->snapshot_fresh = t_fresh ? 1 : 0;
    out->fault = t_fault ? 1 : 0;
    out->snapshot_seq = t_seq + (t_changing_seq ? t_sample_calls : 0);
    ++t_sample_calls;
    return 0;
}

int fake_transport_get_string(uint32_t slot, char* buf, uint32_t cap, uint32_t* out_len) {
    assert(slot == 12);
    assert(cap > t_command.size());
    *out_len = static_cast<uint32_t>(t_command.size());
    if (!t_command.empty()) {
        std::memcpy(buf, t_command.data(), t_command.size());
    }
    return 0;
}

int fake_transport_get_bool(uint32_t slot, int* out_val) {
    assert(slot == 3 || slot == 16);
    *out_val = (slot == 3) ? (t_foreground ? 1 : 0) : (t_item_access ? 1 : 0);
    return 0;
}

int fake_transport_get_double(uint32_t slot, double* out_val, int* out_has) {
    assert(slot == 0 || slot == 1);
    *out_has = t_has_coords ? 1 : 0;
    *out_val = (slot == 0) ? t_lat : t_lon;
    return 0;
}

int fake_transport_emit(uint32_t op,
                        const int64_t*,
                        uint32_t scalar_cnt,
                        const PgrRuntimeTransportString* strs,
                        uint32_t str_cnt) {
    assert(op == 19 && scalar_cnt == 0 && str_cnt == 2);
    assert( std::string(strs[0].ptr, strs[0].len) == "auto-raid" );
    t_events.emplace_back(strs[1].ptr, strs[1].len);
    return 0;
}

unsigned bootstrap_calls = 0;
int bootstrap_rc = -1;
int fake_bootstrap_snapshot(PgrBootBinding* out) {
    ++bootstrap_calls;
    if (bootstrap_rc != 0) return bootstrap_rc;
    out->runtime.gchandle_new = reinterpret_cast<void*>(fake_gchandle_new);
    out->runtime.gchandle_get_target = reinterpret_cast<void*>(fake_gchandle_get_target);
    out->runtime.gchandle_free = reinterpret_cast<void*>(fake_gchandle_free);
    return 0;
}

int fake_meta_resolve_class(const PgrMetaClassQuery*, PgrMetaClassResult*) {
    return -1; // Simulate missing class on raw metadata path
}

PgrRuntimeTransportApi g_transport_api{};
PgrBootApi g_boot_api{};
PgrMetaApi g_meta_api{};
PgrVgpResultsApi g_results_api{};

// Fake managed game world for the runtime adapter
int dummy_gym_objs[8]       = {101, 102, 103, 104, 105, 106, 107, 108};
int dummy_gym_root_obj      = 201;
int dummy_raid_state_obj    = 202;
int dummy_approach_ui_obj   = 203;
int dummy_ticket_modal_obj  = 204;
int dummy_lobby_ui_obj      = 205;
int dummy_resolve_ui_obj    = 206;
int dummy_coord_enc_obj     = 207;
int dummy_raid_pokemon_obj  = 208;
int dummy_catch_promise_obj = 209;

struct FakeWorld {
    bool map_ready = true;
    std::string account = "trainer-1";
    int64_t game_time_ms = 1000000;
    std::vector<pgr_auto_raid::RaidCandidate> gyms;
    bool gym_alive = true;
    unsigned gym_clicks = 0;
    bool approach_ready = true;
    bool approach_costs_ready = true;
    bool ticket_already_used = false;
    bool remote_ticket_used = false;
    std::vector<pgr_auto_raid::EntryCostOption> entry_costs;
    unsigned join_presses = 0;
    unsigned exit_calls = 0;

    bool modal_open = true;
    pgr_auto_raid::EntryCostOption modal_cost{};
    bool modal_coin_flag = false;
    pgr_auto_raid::EntryCostOption cycled_modal_cost{};
    bool has_cycled_cost = false;
    unsigned modal_cycles = 0;
    unsigned modal_confirms = 0;
    unsigned modal_dismisses = 0;

    bool lobby_open = false;
    bool lobby_battle_started = false;
    int lobby_players = 1;
    bool lobby_has_countdown = true;
    int64_t lobby_countdown_ms = 60000;
    unsigned lobby_leaves = 0;

    bool battle_active = false;
    bool resolve_open = false;
    int resolve_status = pgr_auto_raid::BATTLE_STATUS_VICTORY;
    bool resolve_advance = true;

    bool encounter_open = false;
    bool encounter_shiny = false;
    int encounter_ball = 5;
    unsigned throws_submitted = 0;
    unsigned catch_polls = 0;
    int poll_rc = PGR_VR_DONE;
    int catch_status = 1; // 1 = CatchSuccess
    unsigned summary_closed = 0;
    bool berry_bindings = true, berry_ready = true, berry_feedable = true;
    int active_berry = 0, razz = 5, golden = 5;
    int berry_rc = PGR_VR_DONE, berry_result = BerryAccepted;
    unsigned berry_polls = 0, berry_clears = 0;
    std::vector<int> berries;
};

FakeWorld world{};

bool fake_ensure_ready() { return true; }
std::string fake_read_account_id() { return world.account; }
int64_t fake_read_game_time_ms() { return world.game_time_ms; }

void fake_scan_gyms(std::vector<pgr_auto_raid::RaidCandidate>& out_candidates,
                    std::vector<Root>& out_roots) {
    out_candidates = world.gyms;
    out_roots.clear();
    for (size_t i = 0; i < world.gyms.size(); ++i) {
        out_roots.emplace_back(&dummy_gym_objs[i % 8]);
    }
}

bool fake_is_gym_alive(void*) { return world.gym_alive; }
void fake_click_gym_poi(void*) { ++world.gym_clicks; }

bool fake_advance_to_raid_approach(void*, Root& out_gr, Root& out_rs, Root& out_app) {
    if (!world.approach_ready) return false;
    out_gr = Root(&dummy_gym_root_obj);
    out_rs = Root(&dummy_raid_state_obj);
    out_app = Root(&dummy_approach_ui_obj);
    return true;
}

bool fake_inspect_approach_costs(void*,
                                 bool,
                                 bool& out_ticket_used,
                                 bool& out_remote_used,
                                 std::vector<pgr_auto_raid::EntryCostOption>& out_options) {
    if (!world.approach_costs_ready) return false;
    out_ticket_used = world.ticket_already_used;
    out_remote_used = world.remote_ticket_used;
    out_options = world.entry_costs;
    return true;
}

void fake_press_approach_join(void*) { ++world.join_presses; }
void fake_exit_raid_or_gym(void*, void*, void*) { ++world.exit_calls; }

bool fake_inspect_ticket_modal(void*,
                               Root& out_modal,
                               pgr_auto_raid::EntryCostOption& out_selected_cost,
                               bool& out_coin_flag) {
    if (!world.modal_open) return false;
    out_modal = Root(&dummy_ticket_modal_obj);
    out_selected_cost = world.modal_cost;
    out_coin_flag = world.modal_coin_flag;
    return true;
}

void fake_cycle_modal_currency(void*) {
    ++world.modal_cycles;
    if (world.has_cycled_cost) {
        world.modal_cost = world.cycled_modal_cost;
    }
}

void fake_confirm_modal_ticket(void*) {
    ++world.modal_confirms;
    world.lobby_open = true;
}

void fake_dismiss_modal_ticket(void*) { ++world.modal_dismisses; }

bool fake_poll_lobby_state(Root& out_lobby_ui,
                           bool& out_battle_started,
                           int& out_player_count,
                           bool& out_has_countdown,
                           int64_t& out_countdown_ms) {
    if (!world.lobby_open) return false;
    out_lobby_ui = Root(&dummy_lobby_ui_obj);
    out_battle_started = world.lobby_battle_started;
    out_player_count = world.lobby_players;
    out_has_countdown = world.lobby_has_countdown;
    out_countdown_ms = world.lobby_countdown_ms;
    return true;
}

void fake_leave_lobby(void*, void*) {
    ++world.lobby_leaves;
    world.lobby_open = false;
}

bool fake_is_battle_active() { return world.battle_active; }

bool fake_poll_resolve_state(Root& out_resolve_ui, int& out_battle_status, bool& out_advanced) {
    if (!world.resolve_open) return false;
    out_resolve_ui = Root(&dummy_resolve_ui_obj);
    out_battle_status = world.resolve_status;
    out_advanced = world.resolve_advance;
    if (world.resolve_status == pgr_auto_raid::BATTLE_STATUS_VICTORY && world.resolve_advance) {
        world.resolve_open = false;
        world.encounter_open = true;
    }
    return true;
}

bool fake_poll_raid_encounter(Root& io_coord, Root& io_pk, bool& out_shiny, int& out_ball) {
    if (!world.encounter_open) return false;
    io_coord = Root(&dummy_coord_enc_obj);
    io_pk = Root(&dummy_raid_pokemon_obj);
    out_shiny = world.encounter_shiny;
    out_ball = world.encounter_ball;
    return true;
}

Root fake_submit_raid_throw(void*, void*, int ball) {
    auto value = raid_throw_value(ball);
    assert(value.item == ball && value.radius == 0.1f && value.bullseye == 1 && value.spinning == 1 && value.missed == 0);
    assert(!session.berry_promise);
    ++world.throws_submitted;
    return Root(&dummy_catch_promise_obj);
}

int fake_poll_catch_promise(void*, uint64_t, int& out_status) {
    ++world.catch_polls;
    out_status = world.catch_status;
    return world.poll_rc;
}

void fake_close_encounter_summary() {
    ++world.summary_closed;
    world.encounter_open = false;
}
bool fake_map_ready(){return world.map_ready;}
bool fake_ensure_berry_ready(){return world.berry_bindings;}
bool fake_inspect_berry(void*,void*,RaidBerrySnapshot& out){
    out={world.active_berry,world.razz,world.golden,world.berry_feedable};return world.berry_ready;
}
Root fake_submit_berry(void*,void*,int item){
    assert(!session.pending_promise);world.berries.push_back(item);
    return Root(&dummy_catch_promise_obj);
}
int fake_poll_berry(void*,uint64_t,int& result){
    ++world.berry_polls;result=world.berry_result;
    if(world.berry_rc==PGR_VR_DONE && result==BerryAccepted)world.active_berry=world.berries.back();
    return world.berry_rc;
}
void fake_clear_berry(){world.active_berry=0;++world.berry_clears;}

RuntimeAdapter g_fake_adapter = {
    fake_ensure_ready,
    fake_read_account_id,
    fake_read_game_time_ms,
    fake_scan_gyms,
    fake_is_gym_alive,
    fake_click_gym_poi,
    fake_advance_to_raid_approach,
    fake_inspect_approach_costs,
    fake_press_approach_join,
    fake_exit_raid_or_gym,
    fake_inspect_ticket_modal,
    fake_cycle_modal_currency,
    fake_confirm_modal_ticket,
    fake_dismiss_modal_ticket,
    fake_poll_lobby_state,
    fake_leave_lobby,
    fake_is_battle_active,
    fake_poll_resolve_state,
    fake_poll_raid_encounter,
    fake_submit_raid_throw,
    fake_poll_catch_promise,
    fake_close_encounter_summary,
    fake_map_ready,
    fake_ensure_berry_ready,
    fake_inspect_berry,
    fake_submit_berry,
    fake_poll_berry,
    fake_clear_berry,
};

pgr_auto_raid::RaidCandidate make_active_raid(const std::string& fort_id,
                                              int64_t seed,
                                              double lat,
                                              double lon,
                                              int level = pgr_auto_raid::RAID_LEVEL_5,
                                              int boss_id = 150) {
    pgr_auto_raid::RaidCandidate c{};
    c.fort_id = fort_id;
    c.raid_seed = seed;
    c.latitude = lat;
    c.longitude = lon;
    c.has_active_raid = true;
    c.is_raid_boss_active = true;
    c.is_raid_egg_active = false;
    c.is_complete = false;
    c.is_raid_hidden = false;
    c.spawn_ms = 900000;
    c.battle_ms = 950000;
    c.end_ms = 1200000;
    c.raid_level = level;
    c.boss_pokemon_id = boss_id;
    return c;
}

void reset_fixture() {
    session.reset();
    tracker = pgr_auto_raid::SessionTracker{};
    owner = 0;
    boot_ready = false;
    bootstrap_calls = 0;
    bootstrap_rc = -1;
    t_fresh = true;
    t_fault = false;
    t_changing_seq = false;
    t_seq = 100;
    t_sample_calls = 0;
    t_command.clear();
    t_foreground = true;
    t_item_access = true;
    t_has_coords = true;
    t_lat = 37.7749;
    t_lon = -122.4194;
    t_events.clear();
    world = FakeWorld{};
    test_clock_override_ms = 500000;
    boot.runtime.gchandle_new = reinterpret_cast<void*>(fake_gchandle_new);
    boot.runtime.gchandle_get_target = reinterpret_cast<void*>(fake_gchandle_get_target);
    boot.runtime.gchandle_free = reinterpret_cast<void*>(fake_gchandle_free);
    assert(test_roots.empty());
    assert(root_allocations == root_frees);
}

} // namespace

extern "C" const void* pgrazor_runtime_transport_api(uint32_t v) {
    return v == 1 ? &g_transport_api : nullptr;
}
extern "C" const void* pgrazor_runtime_bootstrap_api(uint32_t v) {
    return v == 1 ? &g_boot_api : nullptr;
}
extern "C" const void* pgrazor_runtime_metadata_api(uint32_t v) {
    return v == 1 ? &g_meta_api : nullptr;
}
extern "C" const void* pgrazor_vgp_results_api(uint32_t v) {
    return v == 1 ? &g_results_api : nullptr;
}

int main() {
    g_transport_api.sample = fake_transport_sample;
    g_transport_api.get_string = fake_transport_get_string;
    g_transport_api.get_bool = fake_transport_get_bool;
    g_transport_api.get_double = fake_transport_get_double;
    g_transport_api.emit = fake_transport_emit;
    g_boot_api.snapshot = fake_bootstrap_snapshot;
    g_meta_api.resolve_class = fake_meta_resolve_class;

    const uint64_t tid = static_cast<uint64_t>(gettid());
    PgrSharedMetadataOutput metadata{};

    // ------------------------------------------------------------------------
    // 1. Inactive / disabled / stale fast-exit BEFORE bootstrap or services
    // ------------------------------------------------------------------------
    reset_fixture();
    g_test_adapter = nullptr;
    assert(!pgr_auto_raid_tick(tid, &metadata));
    assert(bootstrap_calls == 0 && t_events.empty());

    // Stop command with enabled=0 (and both pass flags 0) is valid, acks once, and does not bootstrap.
    t_command = "R1|100|0|0|1|1|45|0|0|0|0||0";
    assert(!pgr_auto_raid_tick(tid, &metadata));
    assert(bootstrap_calls == 0 && t_events.size() == 1);
    assert(t_events.back().find("\"token\":\"100\"") != std::string::npos);
    assert(t_events.back().find("\"phase\":\"stopped\"") != std::string::npos);
    // Repeated tick with same stopped token is a no-op.
    assert(!pgr_auto_raid_tick(tid, &metadata));
    assert(bootstrap_calls == 0 && t_events.size() == 1);

    // Real adapter startup/ABI failure stops with named reason and consumes token without replay.
    t_command = "R1|101|1|0|1|1|45|0|1|0|0||0";
    assert(!pgr_auto_raid_tick(tid, &metadata));
    assert(bootstrap_calls == 1 && tracker.finished_token == 101);
    assert(t_events.back().find("\"token\":\"101\"") != std::string::npos);
    assert(t_events.back().find("\"phase\":\"error\"") != std::string::npos);
    assert(t_events.back().find("Runtime bootstrap unavailable") != std::string::npos);
    assert(!pgr_auto_raid_tick(tid, &metadata));
    assert(bootstrap_calls == 1); // no retry loop on same token

    // ------------------------------------------------------------------------
    // 2. Discovery -> Travel -> Cooldown Wait (13th field) -> Pass Priority ->
    //    Lobby -> Battle -> Resolve -> Bonus Catch -> Shiny Stop
    // ------------------------------------------------------------------------
    reset_fixture();
    g_test_adapter = &g_fake_adapter;

    // Populate gyms: an egg raid (ignored), an expired raid (ignored), a far 5* raid, and a nearer 5* raid.
    auto egg_raid = make_active_raid("gym-egg", 111, 37.7750, -122.4195);
    egg_raid.is_raid_egg_active = true;
    egg_raid.is_raid_boss_active = false;
    auto expired_raid = make_active_raid("gym-exp", 222, 37.7750, -122.4195);
    expired_raid.end_ms = 1005000; // < 25s remaining at game_time_ms=1000000
    auto far_raid = make_active_raid("gym-far", 333, 37.7850, -122.4294, pgr_auto_raid::RAID_LEVEL_5, 150);
    auto near_raid = make_active_raid("gym-near", 444, 37.7790, -122.4194, pgr_auto_raid::RAID_LEVEL_5, 150);
    world.gyms = {egg_raid, expired_raid, far_raid, near_raid};

    // Command: token 200, tierMask 4 (5-star), maxRaids 3, minPlayers 2, wait 45s, free=1, premium=1, stopOnShiny=1, boss 150, cooldownMs=12000
    t_command = "R1|200|1|4|3|2|45|0|1|1|1|150|12000";

    // Tick 1: starts session, scans gyms, selects "gym-near", emits travel event with travel=1
    assert(pgr_auto_raid_tick(tid, &metadata));
    assert(session.active && session.stage == RaidStage::Travel);
    assert(session.target.fort_id == "gym-near");
    assert(t_events.back().find("\"phase\":\"travel\"") != std::string::npos);
    assert(t_events.back().find("\"travel\":1") != std::string::npos);

    // Foreign thread cannot tick or stop the owned session.
    assert(pgr_auto_raid_tick(tid + 1, &metadata));
    pgr_auto_raid_stop(tid + 1);
    assert(session.active && session.stage == RaidStage::Travel);

    // Arrive at gym coordinates, but cooldownMs is still 12000 -> must wait in Travel and emit cooldown phase!
    t_lat = near_raid.latitude;
    t_lon = near_raid.longitude;
    test_clock_override_ms += 1000;
    assert(pgr_auto_raid_tick(tid, &metadata));
    assert(session.stage == RaidStage::Travel && world.gym_clicks == 0);
    assert(t_events.back().find("\"phase\":\"cooldown\"") != std::string::npos);

    // Clear cooldownMs to 0 -> clicks gym POI and transitions to OpenGym
    t_command = "R1|200|1|4|3|2|45|0|1|1|1|150|0";
    test_clock_override_ms += 1000;
    assert(pgr_auto_raid_tick(tid, &metadata));
    assert(world.gym_clicks == 1 && session.stage == RaidStage::OpenGym);

    // Advance to VerifyPass
    test_clock_override_ms += 600;
    assert(pgr_auto_raid_tick(tid, &metadata));
    assert(session.stage == RaidStage::VerifyPass);

    // Both Free (1401) and Premium (1402) passes are satisfied in bag -> Free pass MUST be prioritized first!
    pgr_auto_raid::EntryCostOption free_opt{};
    free_opt.raid_type = pgr_auto_raid::RAID_LOCATION_LOCAL;
    free_opt.items = {{pgr_auto_raid::ITEM_FREE_RAID_TICKET, 1}};
    free_opt.satisfied_in_bag = true;

    pgr_auto_raid::EntryCostOption prem_opt{};
    prem_opt.raid_type = pgr_auto_raid::RAID_LOCATION_LOCAL;
    prem_opt.items = {{pgr_auto_raid::ITEM_PAID_RAID_TICKET, 1}};
    prem_opt.satisfied_in_bag = true;

    world.entry_costs = {prem_opt, free_opt};
    // Simulate modal initially showing Premium pass, then cycling to Free pass on CycleCurrency().
    world.modal_cost = prem_opt;
    world.cycled_modal_cost = free_opt;
    world.has_cycled_cost = true;

    test_clock_override_ms += 600;
    assert(pgr_auto_raid_tick(tid, &metadata));
    assert(world.join_presses == 1 && session.stage == RaidStage::ConfirmModalPass);
    assert(session.pass_decision.status == pgr_auto_raid::PassDecisionStatus::SelectFreePass);

    // First modal check sees Premium pass (mismatch for expected FreeLocal) -> cycles currency!
    test_clock_override_ms += 600;
    assert(pgr_auto_raid_tick(tid, &metadata));
    assert(world.modal_cycles == 1 && world.modal_confirms == 0 && session.stage == RaidStage::ConfirmModalPass);

    // Second modal check sees Free pass -> confirms ticket and enters Lobby!
    test_clock_override_ms += 600;
    assert(pgr_auto_raid_tick(tid, &metadata));
    assert(world.modal_confirms == 1 && session.stage == RaidStage::Lobby);

    // Map itemAccessReady (bool 16) drops to false during lobby/battle/rewards/catch -> must NOT abort!
    t_item_access = false;
    world.lobby_players = 3; // >= min_players (2)
    world.lobby_battle_started = true;
    test_clock_override_ms += 600;
    assert(pgr_auto_raid_tick(tid, &metadata));
    assert(session.stage == RaidStage::Battle);

    // Battle finishes -> RaidResolveUIController opens with Victory -> advances to Encounter
    world.resolve_open = true;
    world.resolve_status = pgr_auto_raid::BATTLE_STATUS_VICTORY;
    world.resolve_advance = true;
    world.encounter_shiny = true; // Shiny boss!
    test_clock_override_ms += 600;
    assert(pgr_auto_raid_tick(tid, &metadata));
    assert(session.stage == RaidStage::Encounter && session.boss_shiny);

    // Throw 1 escapes -> stays in Encounter
    world.poll_rc = PGR_VR_DONE;
    world.catch_status = 2; // CatchEscape
    test_clock_override_ms += 1500;
    assert(pgr_auto_raid_tick(tid, &metadata)); // submits throw 1
    assert(world.throws_submitted == 1 && session.pending_promise);
    test_clock_override_ms += 500;
    assert(pgr_auto_raid_tick(tid, &metadata)); // polls throw 1 -> CatchEscape
    assert(!session.pending_promise && session.stage == RaidStage::Encounter);

    // Throw 2 succeeds -> shiny caught with stopOnShiny=1 -> finishes session with "done"!
    world.catch_status = 1; // CatchSuccess
    test_clock_override_ms += 1500;
    assert(pgr_auto_raid_tick(tid, &metadata)); // submits throw 2
    assert(world.throws_submitted == 2);
    test_clock_override_ms += 500;
    assert(pgr_auto_raid_tick(tid, &metadata)); // catch succeeds; don't travel during result transition
    assert(session.stage==RaidStage::CatchSummary);
    assert(pgr_auto_raid_tick(tid,&metadata));
    assert(world.summary_closed==1&&session.active);
    t_item_access=true;
    assert(!pgr_auto_raid_tick(tid,&metadata)); // map ready -> DoneShinyCaught
    assert(!session.active && tracker.finished_token == 200);
    assert(t_events.back().find("\"phase\":\"done\"") != std::string::npos);
    assert(t_events.back().find("\"completed\":1") != std::string::npos);
    assert(test_roots.empty() && root_allocations == root_frees);

    // ------------------------------------------------------------------------
    // 3. Unauthorized Pass Rejection BEFORE OnJoinButtonPressed
    // ------------------------------------------------------------------------
    reset_fixture();
    g_test_adapter = &g_fake_adapter;
    auto local_raid = make_active_raid("gym-pass", 555, 37.7749, -122.4194);
    world.gyms = {local_raid};
    // Only Remote pass & Shop coin pass offered
    pgr_auto_raid::EntryCostOption remote_opt{};
    remote_opt.raid_type = pgr_auto_raid::RAID_LOCATION_REMOTE;
    remote_opt.items = {{pgr_auto_raid::ITEM_REMOTE_RAID_TICKET, 1}};
    remote_opt.satisfied_in_bag = true;
    world.entry_costs = {remote_opt};

    t_command = "R1|300|1|0|2|1|45|0|1|1|0||0";
    assert(pgr_auto_raid_tick(tid, &metadata)); // Scan -> Travel
    test_clock_override_ms += 600;
    assert(pgr_auto_raid_tick(tid, &metadata)); // Travel -> OpenGym
    test_clock_override_ms += 600;
    assert(pgr_auto_raid_tick(tid, &metadata)); // OpenGym -> VerifyPass
    test_clock_override_ms += 600;
    assert(!pgr_auto_raid_tick(tid, &metadata)); // VerifyPass -> refuses remote pass BEFORE join!
    assert(world.join_presses == 0);
    assert(t_events.back().find("\"phase\":\"error\"") != std::string::npos);
    assert(t_events.back().find("Remote Raid Pass is forbidden") != std::string::npos);
    assert(test_roots.empty());

    // ------------------------------------------------------------------------
    // 4. Lobby Bailout Under minPlayers & Two Consecutive Losses Stop
    // ------------------------------------------------------------------------
    reset_fixture();
    g_test_adapter = &g_fake_adapter;
    auto raid_a = make_active_raid("gym-a", 601, 37.7749, -122.4194);
    auto raid_b = make_active_raid("gym-b", 602, 37.7750, -122.4194);
    auto raid_c = make_active_raid("gym-c", 603, 37.7751, -122.4194);
    world.gyms = {raid_a, raid_b, raid_c};
    world.entry_costs = {free_opt};
    world.modal_cost = free_opt;

    // minPlayers = 4, waitSeconds = 20
    t_command = "R1|400|1|0|3|4|20|0|1|0|0||0";
    assert(pgr_auto_raid_tick(tid, &metadata)); // Scan -> Travel (gym-a)
    test_clock_override_ms += 600;
    assert(pgr_auto_raid_tick(tid, &metadata)); // Travel -> OpenGym
    test_clock_override_ms += 600;
    assert(pgr_auto_raid_tick(tid, &metadata)); // OpenGym -> VerifyPass
    test_clock_override_ms += 600;
    assert(pgr_auto_raid_tick(tid, &metadata)); // VerifyPass -> ConfirmModalPass
    test_clock_override_ms += 600;
    assert(pgr_auto_raid_tick(tid, &metadata)); // ConfirmModalPass -> Lobby

    // In lobby with only 2/4 players; countdown drops to 4000ms (<= 5000ms safety floor) -> must LeaveLobby!
    world.lobby_players = 2;
    world.lobby_battle_started = false;
    world.lobby_has_countdown = true;
    world.lobby_countdown_ms = 4000;
    test_clock_override_ms += 1000;
    assert(pgr_auto_raid_tick(tid, &metadata));
    assert(world.lobby_leaves == 1 && session.stage == RaidStage::Scan);
    assert(tracker.is_raid_excluded("gym-a#601", test_clock_override_ms));

    // Now test 2 consecutive losses on gym-b and gym-c:
    world.lobby_players = 5;
    world.lobby_battle_started = true;
    for (int loss_num = 1; loss_num <= 2; ++loss_num) {
        test_clock_override_ms += 2000;
        assert(pgr_auto_raid_tick(tid, &metadata)); // Scan -> Travel
        test_clock_override_ms += 600;
        assert(pgr_auto_raid_tick(tid, &metadata)); // Travel -> OpenGym
        test_clock_override_ms += 600;
        assert(pgr_auto_raid_tick(tid, &metadata)); // OpenGym -> VerifyPass
        test_clock_override_ms += 600;
        assert(pgr_auto_raid_tick(tid, &metadata)); // VerifyPass -> Lobby (since world.lobby_open is true or modal confirms)
        if (session.stage == RaidStage::ConfirmModalPass) {
            test_clock_override_ms += 600;
            assert(pgr_auto_raid_tick(tid, &metadata));
        }
        test_clock_override_ms += 600;
        assert(pgr_auto_raid_tick(tid, &metadata)); // Lobby -> Battle
        assert(session.stage == RaidStage::Battle);

        world.resolve_open = true;
        world.resolve_status = pgr_auto_raid::BATTLE_STATUS_DEFEAT;
        test_clock_override_ms += 600;
        const bool cont = pgr_auto_raid_tick(tid, &metadata); // Battle -> Resolve -> Defeat
        if (loss_num == 1) {
            assert(cont && session.stage == RaidStage::Scan && tracker.consecutive_losses == 1);
        } else {
            assert(!cont && !session.active && tracker.finished_token == 400);
            assert(t_events.back().find("\"phase\":\"error\"") != std::string::npos);
            assert(t_events.back().find("2 consecutive raid losses") != std::string::npos);
        }
        world.resolve_open = false;
        world.lobby_open = false;
    }

    assert(test_roots.empty() && root_allocations == root_frees);
    // Revocation during an in-flight catch must not poll it or change game UI.
    for(int revoke=0;revoke<5;++revoke){
        reset_fixture();g_test_adapter=&g_fake_adapter;
        t_command="R1|500|1|0|3|1|45|0|1|0|0||0";
        assert(pgr_auto_raid_tick(tid,&metadata));
        session.stage=RaidStage::Encounter;
        session.pending_promise=Root(&dummy_catch_promise_obj);
        session.uncertain_action=true;
        if(revoke==0)t_fresh=false;
        if(revoke==1)t_foreground=false;
        if(revoke==2)t_command.clear();
        if(revoke==3)t_command="invalid";
        if(revoke==4)world.account="trainer-2";
        assert(!pgr_auto_raid_tick(tid,&metadata));
        assert(world.catch_polls==0&&world.summary_closed==0&&world.throws_submitted==0);
        assert(world.lobby_leaves==0&&world.exit_calls==0&&world.modal_dismisses==0);
        assert(test_roots.empty());
    }
    reset_fixture();g_test_adapter=&g_fake_adapter;
    t_command="R1|600|1|0|3|1|45|0|1|0|0||0";
    assert(pgr_auto_raid_tick(tid,&metadata));
    t_changing_seq=true;
    assert(pgr_auto_raid_tick(tid,&metadata)); // Fresh snapshot raced: wait, don't cancel.
    t_changing_seq=false;
    t_command="R1|600|1|0|3|1|45|0|0|1|0||0";
    assert(pgr_auto_raid_tick(tid,&metadata));
    assert(session.command.free_pass&&!session.command.premium_pass); // immutable spending policy
    session.stage=RaidStage::Encounter;
    session.pending_promise=Root(&dummy_catch_promise_obj);
    world.catch_status=999;
    assert(!pgr_auto_raid_tick(tid,&metadata));
    assert(world.summary_closed==0);
    assert(t_events.back().find("Unknown catch result")!=std::string::npos);
    t_command="R1|599|1|0|3|1|45|0|1|0|0||0";
    assert(!pgr_auto_raid_tick(tid,&metadata)); // Older token cannot restart a completed session.
    assert(test_roots.empty()&&root_allocations==root_frees);
    reset_fixture();g_test_adapter=&g_fake_adapter;
    t_command="R1|700|1|0|3|1|45|0|1|0|0||0";
    assert(pgr_auto_raid_tick(tid,&metadata));
    session.stage=RaidStage::Encounter;
    session.pending_promise=Root(&dummy_catch_promise_obj);
    session.throws_made=kMaxRaidThrows;
    world.catch_status=2; // Our safety cap is not evidence the game's bonus balls ran out.
    assert(!pgr_auto_raid_tick(tid,&metadata));
    assert(world.summary_closed==0);
    assert(t_events.back().find("completion not confirmed")!=std::string::npos);
    assert(test_roots.empty());
    reset_fixture();g_test_adapter=&g_fake_adapter;
    world.game_time_ms=1800000000000LL;
    auto skipped=make_active_raid("skipped",800,t_lat,t_lon);
    auto next=make_active_raid("next",801,t_lat+0.001,t_lon);
    for(auto* raid:{&skipped,&next}){
        raid->spawn_ms=world.game_time_ms-60000;
        raid->battle_ms=world.game_time_ms-30000;
        raid->end_ms=world.game_time_ms+1800000;
    }
    world.gyms={skipped,next};
    t_command="R1|800|1|0|3|1|45|0|1|0|0||0";
    assert(pgr_auto_raid_tick(tid,&metadata));
    assert(session.target.fort_id=="skipped");
    quarantine_and_rescan("underfilled",test_clock_override_ms);
    test_clock_override_ms+=3000;
    assert(pgr_auto_raid_tick(tid,&metadata));
    assert(session.target.fort_id=="next"); // Epoch clock must not instantly expire retry quarantine.
    pgr_auto_raid_stop(tid);
    assert(test_roots.empty());
    reset_fixture();g_test_adapter=&g_fake_adapter;
    t_command="R1|900|1|0|1|1|45|0|1|0|0||0";
    world.map_ready=false;
    assert(pgr_auto_raid_tick(tid,&metadata));
    assert(session.stage==RaidStage::Scan && tracker.travel_seq==0);
    // Java item access stays true in gym/encounter screens. It is insufficient
    // to start another trip or count the completed raid before map restoration.
    session.target_key="owned#900";
    await_catch_summary(false,"Caught raid boss",test_clock_override_ms);
    assert(pgr_auto_raid_tick(tid,&metadata));
    assert(tracker.completed_raids==0 && session.active);
    world.map_ready=true;
    assert(!pgr_auto_raid_tick(tid,&metadata));
    assert(t_events.back().find("\"completed\":1")!=std::string::npos && test_roots.empty());
    auto start_berry_encounter=[&](int mode){
        reset_fixture();g_test_adapter=&g_fake_adapter;
        t_command="R2|1000|1|0|3|1|45|0|1|0|0||0|"+std::to_string(mode);
        assert(pgr_auto_raid_tick(tid,&metadata));
        session.stage=RaidStage::Encounter;session.last_step_ms=0;
        world.encounter_open=true;
    };
    auto tick=[&](){return pgr_auto_raid_tick(tid,&metadata);};
    auto later=[&](){test_clock_override_ms+=kThrowCadenceMs;return tick();};
    // Golden is sent once, confirmed before the first excellent/curve/non-missed throw.
    start_berry_encounter(706);world.berry_rc=PGR_VR_PENDING;
    assert(tick() && world.berries==std::vector<int>{706} && world.throws_submitted==0);
    assert(tick() && world.berries.size()==1 && world.throws_submitted==0);
    world.berry_rc=PGR_VR_DONE;assert(tick() && world.throws_submitted==0);
    assert(later() && world.throws_submitted==1);
    // Breakout removes the used effect and feeds the next throw; a miss retains it.
    world.catch_status=2;assert(tick() && world.berry_clears==1);
    assert(later() && world.berries.size()==2 && world.throws_submitted==1);
    assert(tick());assert(later() && world.throws_submitted==2);
    world.catch_status=4;assert(tick());
    assert(later() && world.berries.size()==2 && world.throws_submitted==3 && world.berry_clears==1);
    pgr_auto_raid_stop(tid);assert(test_roots.empty());
    // Fallback is authorized with Golden alone; regular-only never consumes Golden.
    start_berry_encounter(706);world.golden=0;
    assert(tick() && world.berries==std::vector<int>{701});pgr_auto_raid_stop(tid);
    start_berry_encounter(706);world.golden=world.razz=0;
    assert(tick() && world.berries.empty() && world.throws_submitted==1);pgr_auto_raid_stop(tid);
    start_berry_encounter(701);world.razz=0;
    assert(tick() && world.berries.empty() && world.throws_submitted==1);pgr_auto_raid_stop(tid);
    start_berry_encounter(701);
    assert(tick() && world.berries==std::vector<int>{701});pgr_auto_raid_stop(tid);
    // Keep an existing effect without spending any selected berry.
    start_berry_encounter(706);world.active_berry=705;world.berry_feedable=false;
    assert(tick() && world.berries.empty() && world.throws_submitted==1);pgr_auto_raid_stop(tid);
    // Server NoItem overrides stale positive inventory, including fallback exhaustion.
    start_berry_encounter(706);world.berry_result=BerryNoStock;
    assert(tick());assert(tick());assert(tick());
    assert((world.berries==std::vector<int>{706,701}) && world.throws_submitted==0);
    assert(tick());assert(tick());
    assert(world.berries.size()==2 && world.throws_submitted==1);pgr_auto_raid_stop(tid);
    // Neither an uncertain timeout nor an explicit rejection is retried or followed by a throw.
    for(int scenario=0;scenario<3;++scenario){
        start_berry_encounter(706);assert(tick());
        if(scenario==0){world.berry_rc=PGR_VR_PENDING;test_clock_override_ms+=kPromiseTimeoutMs;}
        if(scenario==1)world.berry_rc=PGR_VR_REJECTED;
        if(scenario==2)world.berry_result=BerryRefused;
        assert(!tick() && world.berries.size()==1 && world.throws_submitted==0 && test_roots.empty());
        assert(!tick() && world.berries.size()==1);
    }
    // Lease, foreground, account and token changes revoke pending work before polling.
    for(int scenario=0;scenario<4;++scenario){
        start_berry_encounter(706);assert(tick());
        if(scenario==0)t_fresh=false;
        if(scenario==1)t_foreground=false;
        if(scenario==2)world.account="other";
        if(scenario==3)t_command="R2|1001|1|0|3|1|45|0|1|0|0||0|706";
        assert(!tick() && world.berry_polls==0 && world.throws_submitted==0 && test_roots.empty());
    }
    // A changed policy within the same token cannot authorize a different consumable.
    start_berry_encounter(701);t_command="R2|1000|1|0|3|1|45|0|1|0|0||0|706";
    assert(tick() && world.berries==std::vector<int>{701});pgr_auto_raid_stop(tid);
    // Transient controls wait, but never stall forever or send an unconfirmed throw.
    start_berry_encounter(706);world.berry_ready=false;
    assert(tick() && world.berries.empty() && world.throws_submitted==0);
    test_clock_override_ms+=kPromiseTimeoutMs;
    assert(!tick() && test_roots.empty());
    start_berry_encounter(706);world.berry_feedable=false;
    assert(tick() && world.berries.empty());world.berry_feedable=true;
    assert(tick() && world.berries.size()==1);pgr_auto_raid_stop(tid);
    reset_fixture();g_test_adapter=&g_fake_adapter;world.berry_bindings=false;
    t_command="R2|1000|1|0|3|1|45|0|1|0|0||0|706";
    assert(!tick() && world.gym_clicks==0 && world.join_presses==0 && world.modal_confirms==0);
    assert(t_events.back().find("berry bindings unavailable")!=std::string::npos && test_roots.empty());
    std::puts("PASS auto_raid runtime: lifecycle, immutable policy, catch flow, berry fallback/acknowledgement/exhaustion/revocation and excellent curve Always Hit values");
    return 0;
}

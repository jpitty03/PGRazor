#include "auto_raid.h"
#include "auto_raid_policy.h"
#include "game_bindings.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include "shared_metadata.h"
#include "throws.h"
#include "vgp_results.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

// Auto-Raid runs on the proven Unity main thread. Java owns explicit session tokens,
// settings and movement; this controller owns discovery, authorized local pass selection,
// lobby decisions and confirmed battle/catch outcomes. Transport/account/lifecycle loss
// revokes game work. Managed objects are rooted only for the active session.
// The game adapters live in auto_raid_game_map.inc and auto_raid_game_catch.inc;
// fixture tests replace those adapters and do not establish live binding correctness.

namespace {

using namespace pgr_shared_meta;

struct Failure {
    std::string where;
};

void require(bool ok, const char* where) {
    if (!ok) throw Failure{where};
}

template <class F>
F fn(const void* p) {
    require(p != nullptr, "missing runtime export");
    return reinterpret_cast<F>(const_cast<void*>(p));
}

PgrBootBinding boot{};
const PgrSharedMetadataOutput* md = nullptr;
uint64_t owner = 0;
bool busy = false;
bool boot_ready = false;

constexpr int64_t kHeartbeatIntervalMs    = 4500;  // <= 5000ms so Java never expires 45s silence
constexpr int64_t kDiscoveryScanMs        = 1500;
constexpr int64_t kActionStepMs           = 500;
constexpr int64_t kTravelTimeoutMs        = 7200000;
constexpr int64_t kApproachTimeoutMs      = 25000;
constexpr int64_t kPassConfirmTimeoutMs   = 15000;
constexpr int64_t kBattleTimeoutMs        = 360000;
constexpr int64_t kResolveTimeoutMs       = 45000;
constexpr int64_t kEncounterTimeoutMs     = 300000;
constexpr int64_t kPromiseTimeoutMs       = 15000;
constexpr int64_t kThrowCadenceMs         = 1200;
constexpr int     kMaxRaidThrows          = 24;
constexpr int     kMaxPassCurrencyCycles  = 4;

int64_t test_clock_override_ms = 0;

int64_t clock_ms() {
    if (test_clock_override_ms > 0) return test_clock_override_ms;
    timespec t{};
    if (clock_gettime(CLOCK_BOOTTIME, &t) != 0) {
        clock_gettime(CLOCK_MONOTONIC, &t);
    }
    return static_cast<int64_t>(t.tv_sec) * 1000LL + static_cast<int64_t>(t.tv_nsec) / 1000000LL;
}

int64_t wall_clock_ms() {
    if (test_clock_override_ms > 0) return test_clock_override_ms;
    timespec t{};
    if (clock_gettime(CLOCK_REALTIME, &t) != 0) return clock_ms();
    return static_cast<int64_t>(t.tv_sec) * 1000LL + static_cast<int64_t>(t.tv_nsec) / 1000000LL;
}

// RAII GC root wrapper — never retain raw managed pointers across ticks or safepoints.
struct Root {
    uintptr_t h = 0;
    Root() = default;
    explicit Root(void* v) {
        if (v) {
            h = fn<uintptr_t (*)(void*, int)>(boot.runtime.gchandle_new)(v, 0);
            require(h != 0, "GC root allocation failed");
        }
    }
    Root(const Root&) = delete;
    Root& operator=(const Root&) = delete;
    Root(Root&& r) noexcept : h(r.h) { r.h = 0; }
    Root& operator=(Root&& r) noexcept {
        if (this != &r) {
            reset();
            h = r.h;
            r.h = 0;
        }
        return *this;
    }
    ~Root() { reset(); }
    void reset() {
        if (h) {
            uintptr_t old = h;
            h = 0;
            if (boot.runtime.gchandle_free) {
                reinterpret_cast<void (*)(uintptr_t)>(boot.runtime.gchandle_free)(old);
            }
        }
    }
    void* get() const {
        return (h && boot.runtime.gchandle_get_target)
                   ? reinterpret_cast<void* (*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h)
                   : nullptr;
    }
    explicit operator bool() const { return get() != nullptr; }
};

const PgrRuntimeTransportApi* transport() {
    return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));
}

std::string json_quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c >= 32) out += static_cast<char>(c);
    }
    out += '"';
    return out;
}

void emit_raw_event(const std::string& json) {
    const PgrRuntimeTransportApi* tr = transport();
    if (!tr || !tr->emit) return;
    PgrRuntimeTransportString strs[2] = {
        {"auto-raid", 9},
        {json.c_str(), static_cast<uint32_t>(json.size())},
    };
    tr->emit(19, nullptr, 0, strs, 2);
}

struct TransportPermission {
    bool snapshot_coherent = false; // sample A and Z both fresh, non-faulted, equal seq
    bool retry_snapshot = false; // Fresh host update overlapped the read; do no game work this tick.
    bool empty_command = false;     // coherent snapshot with empty slot-12 command (revoked)
    bool valid = false;             // coherent snapshot + valid R1/R2 command
    bool foreground = false;        // bool 3: app active in foreground
    bool item_access = false;       // bool 16: map itemAccessReady
    bool has_coords = false;        // valid player coordinates in doubles 0, 1
    double latitude = 0.0;
    double longitude = 0.0;
    int64_t cooldown_ms = 0;
    pgr_auto_raid::Command command{};
};

TransportPermission read_permission() {
    TransportPermission p{};
    const PgrRuntimeTransportApi* tr = transport();
    if (!tr || !tr->sample || !tr->get_string || !tr->get_bool || !tr->get_double) return p;

    PgrRuntimeTransportSample a{}, z{};
    if (tr->sample(&a) != 0 || !a.snapshot_fresh || a.fault) return p;

    char raw[1280]{};
    uint32_t raw_len = 0;
    if (tr->get_string(12, raw, sizeof(raw), &raw_len) != 0 || raw_len >= sizeof(raw)) {
        return p;
    }

    int active_flag = 0;
    int ready_flag = 0;
    int has_lat = 0;
    int has_lon = 0;
    double lat = 0.0;
    double lon = 0.0;
    if (tr->get_bool(3, &active_flag) != 0 || tr->get_bool(16, &ready_flag) != 0) return p;
    if (tr->get_double(0, &lat, &has_lat) != 0 || tr->get_double(1, &lon, &has_lon) != 0) return p;

    if (tr->sample(&z) != 0 || !z.snapshot_fresh || z.fault) {
        return p;
    }
    if (a.snapshot_seq != z.snapshot_seq) {p.retry_snapshot=true;return p;}

    p.snapshot_coherent = true;
    p.foreground        = (active_flag == 1);
    p.item_access       = (ready_flag == 1);
    p.has_coords        = (has_lat == 1 && has_lon == 1 && pgr_auto_raid::valid_coordinates(lat, lon));
    p.latitude          = lat;
    p.longitude         = lon;

    if (raw_len == 0) {
        p.empty_command = true;
        return p;
    }

    if (!pgr_auto_raid::parse_command(raw, raw_len, p.command)) {
        return p;
    }

    p.valid       = true;
    p.cooldown_ms = p.command.cooldown_ms;
    return p;
}

// ============================================================================
// Lifecycle Stage & Session State
// ============================================================================
enum class RaidStage {
    Idle = 0,
    Scan,
    Travel,
    OpenGym,
    VerifyPass,
    ConfirmModalPass,
    Lobby,
    Battle,
    Resolve,
    Encounter,
    CatchSummary,
};

struct ActiveSession {
    bool active = false;
    bool stop_requested = false;
    RaidStage stage = RaidStage::Idle;
    pgr_auto_raid::Command command{};
    int64_t cooldown_ms = 0;
    std::string account;

    // Selected raid target
    pgr_auto_raid::RaidCandidate target{};
    std::string target_key;
    Root target_gym;

    // Active UI / state roots (released when leaving stage or session)
    Root gym_root_state;
    Root raid_state;
    Root approach_ui;
    Root ticket_modal;
    Root lobby_ui;
    Root resolve_ui;
    Root raid_map_pokemon;
    Root coord_encounter;
    Root pending_promise;
    Root berry_promise;
    Root berry_interaction;
    int berry_item = 0;
    int64_t berry_wait_started_ms = 0;
    unsigned berry_unavailable = 0;
    bool berry_prepared = false;

    // Pass verification state
    pgr_auto_raid::PassSelectionResult pass_decision{};
    int modal_cycle_attempts = 0;
    bool join_invoked = false;

    // Lobby / battle / resolve / encounter tracking
    int64_t stage_started_ms = 0;
    int64_t last_step_ms = 0;
    int64_t last_heartbeat_ms = 0;
    int64_t lobby_entered_ms = 0;
    int64_t promise_started_ms = 0;
    int throws_made = 0;
    bool resolve_confirmed = false;
    bool boss_shiny = false;
    bool uncertain_action = false;
    bool completion_shiny = false;
    std::string completion_message;
    std::string last_phase;
    std::string last_message;

    void release_roots() {
        berry_promise.reset();
        berry_interaction.reset();
        berry_item = 0;
        berry_wait_started_ms = 0;
        berry_unavailable = 0;
        berry_prepared = false;
        pending_promise.reset();
        coord_encounter.reset();
        raid_map_pokemon.reset();
        resolve_ui.reset();
        lobby_ui.reset();
        ticket_modal.reset();
        approach_ui.reset();
        raid_state.reset();
        gym_root_state.reset();
        target_gym.reset();
    }

    void reset() {
        release_roots();
        active = false;
        stop_requested = false;
        stage = RaidStage::Idle;
        command = pgr_auto_raid::Command{};
        cooldown_ms = 0;
        account.clear();
        target = pgr_auto_raid::RaidCandidate{};
        target_key.clear();
        pass_decision = pgr_auto_raid::PassSelectionResult{};
        modal_cycle_attempts = 0;
        join_invoked = false;
        stage_started_ms = 0;
        last_step_ms = 0;
        last_heartbeat_ms = 0;
        lobby_entered_ms = 0;
        promise_started_ms = 0;
        throws_made = 0;
        resolve_confirmed = false;
        boss_shiny = false;
        uncertain_action = false;
        completion_shiny = false;
        completion_message.clear();
        last_phase.clear();
        last_message.clear();
    }
};

pgr_auto_raid::SessionTracker tracker{};
ActiveSession session{};

void emit_status(uint64_t token, const char* phase, const std::string& message, int completed) {
    if (token == 0 || !phase) return;
    const std::string tok_str = std::to_string(token);
    std::string json = "{\"token\":" + json_quote(tok_str) +
                       ",\"phase\":" + json_quote(phase) +
                       ",\"message\":" + json_quote(message) +
                       ",\"completed\":" + std::to_string(completed) + "}";
    emit_raw_event(json);
    if (session.active && session.command.token == token) {
        session.last_phase = phase;
        session.last_message = message;
        session.last_heartbeat_ms = clock_ms();
    }
}

void emit_travel(uint64_t token, uint64_t travel_seq, double lat, double lon, const std::string& message, int completed) {
    if (token == 0) return;
    char coord_buf[128]{};
    std::snprintf(coord_buf, sizeof(coord_buf),
                  ",\"travel\":%llu,\"latitude\":%.7f,\"longitude\":%.7f",
                  static_cast<unsigned long long>(travel_seq), lat, lon);
    const std::string tok_str = std::to_string(token);
    std::string json = "{\"token\":" + json_quote(tok_str) +
                       ",\"phase\":\"travel\"" +
                       coord_buf +
                       ",\"message\":" + json_quote(message) +
                       ",\"completed\":" + std::to_string(completed) + "}";
    emit_raw_event(json);
    if (session.active && session.command.token == token) {
        session.last_phase = "travel";
        session.last_message = message;
        session.last_heartbeat_ms = clock_ms();
    }
}

void finish_session(const char* terminal_phase, const std::string& message) {
    const uint64_t tok = session.active ? session.command.token : tracker.active_token;
    const int completed = tracker.completed_raids;
    if (tok != 0) {
        tracker.finished_token = std::max(tracker.finished_token,tok);
        emit_status(tok, terminal_phase, message, completed);
    }
    tracker.clear_active();
    session.reset();
}

void maybe_emit_heartbeat(int64_t now_ms) {
    if (!session.active || session.command.token == 0) return;
    if (session.last_heartbeat_ms != 0 && now_ms - session.last_heartbeat_ms < kHeartbeatIntervalMs) {
        return;
    }
    const char* phase = session.last_phase.empty() ? "scanning" : session.last_phase.c_str();
    // Travel events include coordinates; subsequent heartbeats while waiting for arrival/cooldown
    // emit phase "walking" or "approaching" so Java does not re-trigger a duplicate teleport.
    if (std::strcmp(phase, "travel") == 0) {
        phase = session.command.walk ? "walking" : "approaching";
    }
    const std::string msg = session.last_message.empty() ? "Auto-Raid active" : session.last_message;
    emit_status(session.command.token, phase, msg, tracker.completed_raids);
}

// ============================================================================
// Runtime & Managed Reflection Helpers (with Test Seam for Runtime Fixture)
// ============================================================================
struct RaidBerrySnapshot {
    int active = 0, razz = 0, golden = 0;
    bool can_feed = false;
};
enum RaidBerryResult { BerryAccepted = 0, BerryNoStock = 1, BerryRefused = 2 };
struct RuntimeAdapter {
    bool (*ensure_ready)() = nullptr;
    std::string (*read_account_id)() = nullptr;
    int64_t (*read_game_time_ms)() = nullptr;
    void (*scan_gyms)(std::vector<pgr_auto_raid::RaidCandidate>& out_candidates,
                      std::vector<Root>& out_gym_roots) = nullptr;
    bool (*is_gym_alive)(void* gym) = nullptr;
    void (*click_gym_poi)(void* gym) = nullptr;
    bool (*advance_to_raid_approach)(void* target_gym, Root& out_gym_root, Root& out_raid_state, Root& out_approach_ui) = nullptr;
    bool (*inspect_approach_costs)(void* approach_ui,
                                   bool raid_is_free,
                                   bool& out_ticket_used,
                                   bool& out_remote_used,
                                   std::vector<pgr_auto_raid::EntryCostOption>& out_options) = nullptr;
    void (*press_approach_join)(void* approach_ui) = nullptr;
    void (*exit_raid_or_gym)(void* approach_ui, void* raid_state, void* gym_root_state) = nullptr;
    bool (*inspect_ticket_modal)(void* approach_ui,
                                 Root& out_modal,
                                 pgr_auto_raid::EntryCostOption& out_selected_cost,
                                 bool& out_coin_flag) = nullptr;
    void (*cycle_modal_currency)(void* modal) = nullptr;
    void (*confirm_modal_ticket)(void* modal) = nullptr;
    void (*dismiss_modal_ticket)(void* modal) = nullptr;
    bool (*poll_lobby_state)(Root& out_lobby_ui,
                             bool& out_battle_started,
                             int& out_player_count,
                             bool& out_has_countdown,
                             int64_t& out_countdown_ms) = nullptr;
    void (*leave_lobby)(void* lobby_ui, void* raid_state) = nullptr;
    bool (*is_battle_active)() = nullptr;
    bool (*poll_resolve_state)(Root& out_resolve_ui, int& out_battle_status, bool& out_advanced_to_encounter) = nullptr;
    bool (*poll_raid_encounter)(Root& io_coord_enc,
                                Root& io_raid_pokemon,
                                bool& out_is_shiny,
                                int& out_default_ball) = nullptr;
    Root (*submit_raid_throw)(void* coord_enc, void* raid_pokemon, int ball_id) = nullptr;
    int (*poll_catch_promise)(void* promise, uint64_t tid, int& out_catch_status) = nullptr;
    void (*close_encounter_summary)() = nullptr;
    bool (*map_ready)() = nullptr;
    bool (*ensure_berry_ready)() = nullptr;
    bool (*inspect_raid_berry)(void* state, void* pokemon, RaidBerrySnapshot& out) = nullptr;
    Root (*submit_raid_berry)(void* state, void* pokemon, int item) = nullptr;
    int (*poll_berry_promise)(void* promise, uint64_t tid, int& result) = nullptr;
    void (*clear_raid_berry)() = nullptr;
};

RuntimeAdapter* g_test_adapter = nullptr;

// --- Real IL2CPP Reflection Implementation ---
void* exp_sym(const char* name) {
    std::string full = std::string("il2cpp_") + name;
    void* p = boot.handle ? dlsym(boot.handle, full.c_str()) : nullptr;
    require(p != nullptr, name);
    return p;
}

void* obj_klass(void* o) {
    require(o != nullptr, "null managed object");
    return fn<void* (*)(void*)>(boot.runtime.object_get_class)(o);
}

std::string klass_name_of(void* k) {
    require(k != nullptr, "null class");
    const char* s = fn<const char* (*)(void*)>(exp_sym("class_get_name"))(k);
    return s ? std::string(s) : std::string();
}

std::string obj_class_name(void* o) {
    return klass_name_of(obj_klass(o));
}

void* resolve_class_optional(const char* ns, const char* name) {
    static std::map<std::string, void*> cache;
    const std::string key = std::string(ns ? ns : "") + "." + (name ? name : "");
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    const auto* api = static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));
    require(api != nullptr, "metadata API unavailable");
    PgrMetaClassQuery q{};
    q.version = 1;
    q.size = sizeof(q);
    q.name_space = ns;
    q.name = name;
    q.canonical_match = PGR_MD_MATCH_EXACT;
    PgrMetaClassResult out{};
    if (api->resolve_class(&q, &out) != 0 || !out.klass) {
        return nullptr;
    }
    void* k = const_cast<void*>(out.klass);
    cache[key] = k;
    return k;
}

void* resolve_class(const char* ns, const char* name) {
    void* k = resolve_class_optional(ns, name);
    if(!k)throw Failure{std::string(ns)+"."+name+" class unavailable"};
    return k;
}

void* resolve_raid_class(const char* name) {
    if (void* k = resolve_class_optional("Niantic.Holoholo.Raid", name)) return k;
    if (void* k = resolve_class_optional("Niantic.Holoholo.Raid.Internal", name)) return k;
    require(false, name);
    return nullptr;
}

std::string type_name_of(const void* type) {
    require(type != nullptr, "null type");
    char* p = fn<char* (*)(const void*)>(boot.meta.type_get_name)(type);
    require(p != nullptr, "type_get_name failed");
    std::string s(p);
    fn<void (*)(void*)>(boot.meta.mem_free)(p);
    return s;
}

void* find_method_in_hierarchy(void* k, const char* name, int argc) {
    auto get_m = fn<void* (*)(void*, const char*, int)>(boot.meta.class_get_method_from_name);
    auto get_p = fn<void* (*)(void*)>(exp_sym("class_get_parent"));
    for (int depth = 0; k && depth < 16; ++depth, k = get_p(k)) {
        if (void* m = get_m(k, name, argc)) {
            if (fn<unsigned (*)(void*)>(boot.meta.method_get_param_count)(m) == static_cast<unsigned>(argc)) {
                return m;
            }
        }
    }
    return nullptr;
}

void* require_method(void* k, const char* name, int argc) {
    void* m = find_method_in_hierarchy(k, name, argc);
    if(!m)throw Failure{klass_name_of(k)+"."+name+" ("+std::to_string(argc)+" arguments) unavailable"};
    return m;
}

void* invoke_method(void* m, void* obj, void** args = nullptr) {
    require(m != nullptr, "null method");
    void* ex = nullptr;
    void* res = fn<void* (*)(void*, void*, void**, void**)>(boot.runtime.runtime_invoke)(m, obj, args, &ex);
    if(ex){
        Root exception(ex);
        const char* name=fn<const char*(*)(void*)>(boot.meta.method_get_name)(m);
        void* declaring=fn<void*(*)(void*)>(boot.meta.method_get_class)(m);
        char detail[512]{};
        auto format=reinterpret_cast<void(*)(void*,char*,int)>(dlsym(boot.handle,"il2cpp_format_exception"));
        if(format)format(exception.get(),detail,sizeof(detail));
        throw Failure{klass_name_of(declaring)+"."+(name?name:"?")+" threw: "+(detail[0]?detail:"managed exception")};
    }
    return res;
}

void* call_obj(void* obj, const char* name, int argc = 0, void** args = nullptr) {
    return invoke_method(require_method(obj_klass(obj), name, argc), obj, args);
}

template <class T>
T unbox_val(void* boxed) {
    require(boxed != nullptr, "null boxed value");
    void* klass=obj_klass(boxed);int alignment=0;
    require(fn<bool(*)(void*)>(boot.meta.class_is_valuetype)(klass) &&
            fn<int(*)(void*,int*)>(boot.meta.class_value_size)(klass,&alignment)==sizeof(T),"Boxed return type size mismatch");
    void* p = fn<void* (*)(void*)>(boot.runtime.object_unbox)(boxed);
    require(p != nullptr, "object_unbox failed");
    T v{};
    std::memcpy(&v, p, sizeof(T));
    return v;
}

std::string read_managed_string(void* str_obj) {
    if (!str_obj) return {};
    require(obj_klass(str_obj) == resolve_class("System", "String"), "expected System.String");
    int len = fn<int (*)(void*)>(exp_sym("string_length"))(str_obj);
    require(len >= 0 && len <= 1024, "string length out of bounds");
    const uint16_t* chars = fn<const uint16_t* (*)(void*)>(boot.runtime.string_chars)(str_obj);
    std::string out;
    out.reserve(static_cast<size_t>(len));
    for (int i = 0; i < len; ++i) {
        uint32_t c = chars[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < len && chars[i + 1] >= 0xdc00 && chars[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (chars[i + 1] - 0xdc00);
            ++i;
        } else if (c >= 0xd800 && c < 0xe000) {
            c = 0xfffd;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xc0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 63));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xe0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        } else {
            out += static_cast<char>(0xf0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 63));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        }
    }
    return out;
}

void* find_field_by_type_and_offset(void* k, const char* expected_type, size_t expected_offset = 0) {
    const std::string owner_name=klass_name_of(k);
    auto get_fields = fn<void* (*)(void*, void**)>(exp_sym("class_get_fields"));
    auto get_flags  = fn<int (*)(void*)>(boot.meta.field_get_flags);
    auto get_type   = fn<void* (*)(void*)>(boot.meta.field_get_type);
    auto get_offset = fn<size_t (*)(void*)>(boot.meta.field_get_offset);
    auto get_parent = fn<void* (*)(void*)>(exp_sym("class_get_parent"));

    void* matched = nullptr;
    int match_count = 0;
    for (int depth = 0; k && depth < 12; ++depth, k = get_parent(k)) {
        void* iter = nullptr;
        while (void* f = get_fields(k, &iter)) {
            if (get_flags(f) & 0x10) continue; // skip static fields
            const std::string tname = type_name_of(get_type(f));
            if (tname != expected_type) continue;
            if (expected_offset != 0 && get_offset(f) != expected_offset) continue;
            matched = f;
            ++match_count;
        }
        if (match_count == 1) return matched;
    }
    if(match_count!=1||!matched)throw Failure{owner_name+" field type "+expected_type+" at offset "+std::to_string(expected_offset)+" unavailable or ambiguous"};
    return matched;
}

void* find_field_by_name_checked(void* k, const char* field_name, const char* expected_type = nullptr) {
    const std::string owner_name=klass_name_of(k);
    auto get_field  = fn<void* (*)(void*, const char*)>(boot.meta.class_get_field_from_name);
    auto get_flags  = fn<int (*)(void*)>(boot.meta.field_get_flags);
    auto get_type   = fn<void* (*)(void*)>(boot.meta.field_get_type);
    auto get_parent = fn<void* (*)(void*)>(exp_sym("class_get_parent"));

    for (int depth = 0; k && depth < 12; ++depth, k = get_parent(k)) {
        if (void* f = get_field(k, field_name)) {
            require((get_flags(f) & 0x10) == 0, field_name);
            if (expected_type) {
                const auto actual=type_name_of(get_type(f));
                if(actual!=expected_type)throw Failure{owner_name+"."+field_name+" type mismatch: expected "+expected_type+", got "+actual};
            }
            return f;
        }
    }
    throw Failure{owner_name+"."+field_name+" field unavailable"};
    return nullptr;
}

template <class T>
T read_field_val(void* obj, void* field_info) {
    require(obj != nullptr && field_info != nullptr, "null field read");
    T v{};
    fn<void (*)(void*, void*, void*)>(boot.runtime.field_get_value)(obj, field_info, &v);
    return v;
}

int read_enum_literal(void* enum_klass, const char* literal_name) {
    void* f = fn<void* (*)(void*, const char*)>(boot.meta.class_get_field_from_name)(enum_klass, literal_name);
    require(f != nullptr, literal_name);
    require((fn<int (*)(void*)>(boot.meta.field_get_flags)(f) & 0x8050) == 0x8050, literal_name);
    int v = 0;
    fn<void (*)(void*, void*)>(boot.runtime.field_static_get_value)(f, &v);
    return v;
}

bool unity_obj_alive(void* obj) {
    if (!obj) return false;
    Root rooted(obj);
    void* unity_class=resolve_class("UnityEngine","Object");
    if (!fn<bool(*)(void*,void*)>(boot.meta.class_is_assignable_from)(unity_class,obj_klass(rooted.get()))) return false;
    void* alive_m = (md && md->methods[5])
                        ? const_cast<void*>(md->methods[5])
                        : require_method(resolve_class("UnityEngine", "Object"), "op_Implicit", 1);
    void* args[] = {rooted.get()};
    return unbox_val<bool>(invoke_method(alive_m, nullptr, args));
}

Root type_object_of(void* klass) {
    void* t = fn<void* (*)(void*)>(boot.meta.class_get_type)(klass);
    require(t != nullptr, "class_get_type");
    return Root(fn<void* (*)(void*)>(exp_sym("type_get_object"))(t));
}

Root resolve_di_service(const char* ns, const char* name) {
    require(md != nullptr, "shared metadata unavailable");
    void* map_k   = const_cast<void*>(md->opt_handles[H_IL_MAP]);
    void* find_m  = const_cast<void*>(md->opt_handles[H_IL_FIND_M]);
    void* cont_m  = const_cast<void*>(md->opt_handles[H_IL_CONTAINER_M]);
    void* res_m   = const_cast<void*>(md->opt_handles[H_IL_RESOLVE_M]);
    require(map_k && find_m && cont_m && res_m, "DI container lookup handles missing");

    Root map_type = type_object_of(map_k);
    void* find_args[] = {map_type.get()};
    Root map_ctx(invoke_method(find_m, nullptr, find_args));
    if (!map_ctx || !unity_obj_alive(map_ctx.get())) return {};

    Root container(invoke_method(cont_m, map_ctx.get()));
    if (!container) return {};

    Root target_type = type_object_of(resolve_class(ns, name));
    void* res_args[] = {target_type.get()};
    return Root(invoke_method(res_m, container.get(), res_args));
}

Root find_live_unity_object(void* klass) {
    if (!klass) return {};
    require(fn<bool(*)(void*,void*)>(boot.meta.class_is_assignable_from)(resolve_class("UnityEngine","Object"),klass),
            "FindObjectsOfType requires a UnityEngine.Object subclass");
    void* find_all_m = (md && md->opt_handles[H_CS_FIND])
                           ? const_cast<void*>(md->opt_handles[H_CS_FIND])
                           : require_method(resolve_class("UnityEngine", "Resources"), "FindObjectsOfTypeAll", 1);
    Root t_obj = type_object_of(klass);
    void* args[] = {t_obj.get()};
    Root arr(invoke_method(find_all_m, nullptr, args));
    if (!arr) return {};

    auto arr_len_fn = fn<size_t (*)(void*)>(exp_sym("array_length"));
    const size_t n = std::min<size_t>(arr_len_fn(arr.get()), 64);
    for (size_t i = 0; i < n; ++i) {
        void** elems = reinterpret_cast<void**>(static_cast<char*>(arr.get()) + 32);
        Root candidate(elems[i]);
        if (candidate && unity_obj_alive(candidate.get())) {
            return candidate;
        }
    }
    return {};
}

void real_validate_catch_bindings();
#include "auto_raid_game_map.inc"
#include "auto_raid_game_catch.inc"

RuntimeAdapter kRealAdapter = {
    real_ensure_ready,
    real_read_account_id,
    real_read_game_time_ms,
    real_scan_gyms,
    real_is_gym_alive,
    real_click_gym_poi,
    real_advance_to_raid_approach,
    real_inspect_approach_costs,
    real_press_approach_join,
    real_exit_raid_or_gym,
    real_inspect_ticket_modal,
    real_cycle_modal_currency,
    real_confirm_modal_ticket,
    real_dismiss_modal_ticket,
    real_poll_lobby_state,
    real_leave_lobby,
    real_is_battle_active,
    real_poll_resolve_state,
    real_poll_raid_encounter,
    real_submit_raid_throw,
    real_poll_catch_promise,
    real_close_encounter_summary,
    real_map_ready,
    real_ensure_berry_ready,
    real_inspect_raid_berry,
    real_submit_raid_berry,
    real_poll_berry_promise,
    real_clear_raid_berry,
};

RuntimeAdapter& adapter() {
    return g_test_adapter ? *g_test_adapter : kRealAdapter;
}

// ============================================================================
// Stage Execution Helpers
// ============================================================================
void transition_stage(RaidStage next_stage, int64_t now_ms, const char* phase = nullptr, const std::string& msg = "") {
    session.stage = next_stage;
    session.stage_started_ms = now_ms;
    session.last_step_ms = 0;
    if (phase) {
        emit_status(session.command.token, phase, msg, tracker.completed_raids);
    }
}

void handle_raid_completion(bool caught_shiny, const std::string& summary_msg, int64_t now_ms) {
    const auto cont = tracker.record_completion(session.command, session.target_key, caught_shiny);
    session.release_roots();
    session.target = pgr_auto_raid::RaidCandidate{};
    session.target_key.clear();
    session.join_invoked = false;
    session.resolve_confirmed = false;
    session.boss_shiny = false;
    session.throws_made = 0;
    session.uncertain_action = false;

    if (cont == pgr_auto_raid::SessionContinuation::DoneShinyCaught) {
        finish_session("done", "Shiny raid boss caught; Auto-Raid complete");
        return;
    }
    if (cont == pgr_auto_raid::SessionContinuation::DoneMaxRaids) {
        finish_session("done", "Completed maximum configured raids (" +
                                   std::to_string(tracker.completed_raids) + ")");
        return;
    }
    if (session.stop_requested) {
        finish_session("stopped", "Auto-Raid stopped after completing current raid");
        return;
    }
    transition_stage(RaidStage::Scan, now_ms, "scanning", summary_msg);
}

void handle_raid_loss(const std::string& reason, int64_t now_ms) {
    const auto cont = tracker.record_loss(session.target_key, now_ms);
    adapter().exit_raid_or_gym(session.approach_ui.get(), session.raid_state.get(), session.gym_root_state.get());
    session.release_roots();
    session.target = pgr_auto_raid::RaidCandidate{};
    session.target_key.clear();
    session.join_invoked = false;
    session.resolve_confirmed = false;
    session.uncertain_action = false;

    if (cont == pgr_auto_raid::SessionContinuation::StopConsecutiveLosses) {
        finish_session("error", "Stopped after 2 consecutive raid losses (" + reason + ")");
        return;
    }
    if (session.stop_requested) {
        finish_session("stopped", "Auto-Raid stopped after raid loss");
        return;
    }
    transition_stage(RaidStage::Scan, now_ms, "scanning",
                     "Raid lost (" + reason + "); scanning for next active raid");
}

void quarantine_and_rescan(const std::string& reason, int64_t now_ms) {
    tracker.quarantine_raid(session.target_key, now_ms);
    adapter().exit_raid_or_gym(session.approach_ui.get(), session.raid_state.get(), session.gym_root_state.get());
    session.release_roots();
    session.target = pgr_auto_raid::RaidCandidate{};
    session.target_key.clear();
    session.join_invoked = false;
    session.resolve_confirmed = false;
    session.uncertain_action = false;

    if (session.stop_requested) {
        finish_session("stopped", "Auto-Raid stopped by user");
        return;
    }
    transition_stage(RaidStage::Scan, now_ms, "scanning", reason);
}

void step_scan(const TransportPermission& perm, int64_t now_ms) {
    if (session.stop_requested) {
        finish_session("stopped", "Auto-Raid stopped by user");
        return;
    }
    if (!perm.item_access || !perm.has_coords || !adapter().map_ready()) {
        if(now_ms-session.stage_started_ms>=kApproachTimeoutMs){
            finish_session("error","Game did not return to map readiness after the raid action; check the game");
            return;
        }
        maybe_emit_heartbeat(now_ms);
        return;
    }
    if (session.last_step_ms != 0 && now_ms - session.last_step_ms < kDiscoveryScanMs) {
        maybe_emit_heartbeat(now_ms);
        return;
    }
    session.last_step_ms = now_ms;

    std::vector<pgr_auto_raid::RaidCandidate> candidates;
    std::vector<Root> gym_roots;
    adapter().scan_gyms(candidates, gym_roots);

    const int64_t game_now_ms = adapter().read_game_time_ms();
    double dist_m = 0.0;
    const int idx = pgr_auto_raid::select_nearest_raid(
        candidates, session.command, tracker, perm.latitude, perm.longitude, game_now_ms, &dist_m,
        pgr_auto_raid::kDefaultMinRemainingRaidMs, now_ms);

    if (idx < 0 || static_cast<size_t>(idx) >= candidates.size()) {
            size_t active=0, eggs=0, complete=0, eligible=0;
            for(const auto& c:candidates){
                active+=c.is_raid_boss_active;eggs+=c.is_raid_egg_active;complete+=c.is_complete;
                eligible+=pgr_auto_raid::is_raid_candidate_eligible(c,session.command,game_now_ms);
            }
            const std::string message="Scanning: "+std::to_string(candidates.size())+" gyms, "+
                        std::to_string(active)+" active raids, "+std::to_string(eggs)+" eggs, "+
                        std::to_string(complete)+" completed, "+std::to_string(eligible)+" matching";
        if(session.last_message!=message){
            std::string detail="{\"token\":"+json_quote(std::to_string(session.command.token))+",\"phase\":\"scan-diagnostic\",\"gameTimeMs\":"+std::to_string(game_now_ms)+",\"samples\":[";
            size_t count=0;
            for(const auto& c:candidates){
                if(!c.raid_seed||count>=3)continue;
                if(count++)detail+=",";
                detail+="{\"level\":"+std::to_string(c.raid_level)+",\"boss\":"+std::to_string(c.boss_pokemon_id)+",\"spawn\":"+std::to_string(c.spawn_ms)+",\"battle\":"+std::to_string(c.battle_ms)+",\"end\":"+std::to_string(c.end_ms)+",\"hidden\":"+(c.is_raid_hidden?"true":"false")+"}";
            }
            emit_raw_event(detail+"]}");
            emit_status(session.command.token, "scanning",message,
                        tracker.completed_raids);
        } else {
            maybe_emit_heartbeat(now_ms);
        }
        return;
    }

    session.target = candidates[static_cast<size_t>(idx)];
    session.target_key = pgr_auto_raid::make_raid_key(session.target.fort_id, session.target.raid_seed);
    if (static_cast<size_t>(idx) < gym_roots.size()) {
        session.target_gym = std::move(gym_roots[static_cast<size_t>(idx)]);
    }

    const uint64_t seq = ++tracker.travel_seq;
    session.stage = RaidStage::Travel;
    session.stage_started_ms = now_ms;
    session.last_step_ms = now_ms;
    emit_travel(session.command.token, seq, session.target.latitude, session.target.longitude,
                session.command.walk ? "Walking to raid gym" : "Teleporting to raid gym",
                tracker.completed_raids);
}

void step_travel(const TransportPermission& perm, int64_t now_ms) {
    if (session.stop_requested) {
        finish_session("stopped", "Auto-Raid stopped during travel");
        return;
    }
    if (now_ms - session.stage_started_ms >= kTravelTimeoutMs) {
        quarantine_and_rescan("Timed out waiting to arrive and clear cooldown at gym", now_ms);
        return;
    }
    const int64_t game_now=adapter().read_game_time_ms();
    if(game_now>=session.target.end_ms || session.target.end_ms-game_now<=perm.cooldown_ms){
        quarantine_and_rescan("Raid expires before entry is possible; looking for another raid",now_ms);
        return;
    }
    if (!perm.has_coords) {
        maybe_emit_heartbeat(now_ms);
        return;
    }

    const double dist_m = pgr_auto_raid::haversine_meters(
        perm.latitude, perm.longitude, session.target.latitude, session.target.longitude);
    if (dist_m > pgr_auto_raid::kLocalGymInteractionRangeM) {
        maybe_emit_heartbeat(now_ms);
        return;
    }
    if (perm.cooldown_ms > 0) {
        if (session.last_phase != "cooldown" ||
            now_ms - session.last_heartbeat_ms >= kHeartbeatIntervalMs) {
            emit_status(session.command.token, "cooldown",
                        "Waiting for cooldown (" + std::to_string((perm.cooldown_ms + 999) / 1000) + "s)",
                        tracker.completed_raids);
        }
        return;
    }
    if (!perm.item_access) {
        maybe_emit_heartbeat(now_ms);
        return;
    }

    // Re-verify raid has not expired while traveling/cooling down.
    const int64_t game_now_ms = adapter().read_game_time_ms();
    if (!pgr_auto_raid::is_raid_candidate_eligible(session.target, session.command, game_now_ms, 15000)) {
        quarantine_and_rescan("Target raid expired before entry; scanning for next raid", now_ms);
        return;
    }
    if (!session.target_gym || !adapter().is_gym_alive(session.target_gym.get())) {
        quarantine_and_rescan("Target gym unloaded; rescanning", now_ms);
        return;
    }

    adapter().click_gym_poi(session.target_gym.get());
    transition_stage(RaidStage::OpenGym, now_ms, "approaching", "Opening local raid gym");
}

void step_open_gym(int64_t now_ms) {
    if (session.stop_requested) {
        adapter().exit_raid_or_gym(session.approach_ui.get(), session.raid_state.get(), session.gym_root_state.get());
        finish_session("stopped", "Auto-Raid stopped before raid entry");
        return;
    }
    if (now_ms - session.stage_started_ms >= kApproachTimeoutMs) {
        quarantine_and_rescan("Timed out waiting for gym raid approach screen", now_ms);
        return;
    }
    if (session.last_step_ms != 0 && now_ms - session.last_step_ms < kActionStepMs) {
        maybe_emit_heartbeat(now_ms);
        return;
    }
    session.last_step_ms = now_ms;

    if (adapter().advance_to_raid_approach(
            session.target_gym.get(), session.gym_root_state, session.raid_state, session.approach_ui)) {
        transition_stage(RaidStage::VerifyPass, now_ms, "approaching", "Verifying allowed raid pass");
        return;
    }
    maybe_emit_heartbeat(now_ms);
}

void step_verify_pass(int64_t now_ms) {
    if (session.stop_requested) {
        adapter().exit_raid_or_gym(session.approach_ui.get(), session.raid_state.get(), session.gym_root_state.get());
        finish_session("stopped", "Auto-Raid stopped before spending raid pass");
        return;
    }
    if (now_ms - session.stage_started_ms >= kApproachTimeoutMs) {
        quarantine_and_rescan("Timed out waiting for RaidApproachUIController readiness", now_ms);
        return;
    }

    bool ticket_used = false;
    bool remote_used = false;
    std::vector<pgr_auto_raid::EntryCostOption> options;
    if (!adapter().inspect_approach_costs(
            session.approach_ui.get(), session.target.is_free, ticket_used, remote_used, options)) {
        maybe_emit_heartbeat(now_ms);
        return;
    }

    session.pass_decision = pgr_auto_raid::evaluate_pass_selection(
        session.command, ticket_used, remote_used, session.target.is_free, options);

    switch (session.pass_decision.status) {
        case pgr_auto_raid::PassDecisionStatus::RejoinAlreadyPaid:
            session.join_invoked = true;
            adapter().press_approach_join(session.approach_ui.get());
            session.lobby_entered_ms = now_ms;
            transition_stage(RaidStage::Lobby, now_ms, "lobby", "Rejoining paid local raid lobby");
            return;

        case pgr_auto_raid::PassDecisionStatus::SelectFreePass:
        case pgr_auto_raid::PassDecisionStatus::SelectPremiumPass:
            session.join_invoked = true;
            session.modal_cycle_attempts = 0;
            adapter().press_approach_join(session.approach_ui.get());
            transition_stage(RaidStage::ConfirmModalPass, now_ms, "approaching",
                             session.pass_decision.status == pgr_auto_raid::PassDecisionStatus::SelectFreePass
                                 ? "Selecting Free Daily Raid Pass"
                                 : "Selecting Premium Battle Pass");
            return;

        case pgr_auto_raid::PassDecisionStatus::NoAllowedPassAvailable:
            adapter().exit_raid_or_gym(session.approach_ui.get(), session.raid_state.get(), session.gym_root_state.get());
            finish_session("stopped", "No allowed local raid pass available in bag");
            return;

        case pgr_auto_raid::PassDecisionStatus::ForbiddenRemotePass:
            adapter().exit_raid_or_gym(session.approach_ui.get(), session.raid_state.get(), session.gym_root_state.get());
            finish_session("error", "Refused raid entry: Remote Raid Pass is forbidden");
            return;

        case pgr_auto_raid::PassDecisionStatus::ForbiddenShopPurchase:
            adapter().exit_raid_or_gym(session.approach_ui.get(), session.raid_state.get(), session.gym_root_state.get());
            finish_session("error", "Refused raid entry: shop/coin pass purchase is forbidden");
            return;

        case pgr_auto_raid::PassDecisionStatus::UnsupportedOrAmbiguous:
        default:
            adapter().exit_raid_or_gym(session.approach_ui.get(), session.raid_state.get(), session.gym_root_state.get());
            finish_session("error", "Refused raid entry: unsupported or ambiguous raid entry cost");
            return;
    }
}

void step_confirm_modal_pass(int64_t now_ms) {
    if (session.stop_requested) {
        adapter().dismiss_modal_ticket(session.ticket_modal.get());
        adapter().exit_raid_or_gym(session.approach_ui.get(), session.raid_state.get(), session.gym_root_state.get());
        finish_session("stopped", "Auto-Raid stopped before confirming raid pass");
        return;
    }

    // Check if free entry bypassed the ticket modal and went straight to lobby.
    bool battle_started = false;
    int players = 0;
    bool has_cd = false;
    int64_t cd_ms = 0;
    if (adapter().poll_lobby_state(session.lobby_ui, battle_started, players, has_cd, cd_ms)) {
        session.lobby_entered_ms = now_ms;
        transition_stage(RaidStage::Lobby, now_ms, "lobby", "Entered raid lobby");
        return;
    }

    if (now_ms - session.stage_started_ms >= kPassConfirmTimeoutMs) {
        adapter().dismiss_modal_ticket(session.ticket_modal.get());
        finish_session("error", "Raid entry did not reach a confirmed lobby; check the game before restarting");
        return;
    }
    if (session.last_step_ms != 0 && now_ms - session.last_step_ms < kActionStepMs) {
        maybe_emit_heartbeat(now_ms);
        return;
    }
    session.last_step_ms = now_ms;

    pgr_auto_raid::EntryCostOption modal_cost{};
    bool coin_flag = false;
    if (!adapter().inspect_ticket_modal(
            session.approach_ui.get(), session.ticket_modal, modal_cost, coin_flag)) {
        maybe_emit_heartbeat(now_ms);
        return;
    }

    if (pgr_auto_raid::verify_modal_selected_cost(
            session.command,
            session.pass_decision.expected_cost_kind,
            modal_cost,
            session.target.is_free,
            coin_flag)) {
        session.uncertain_action = true;
        adapter().confirm_modal_ticket(session.ticket_modal.get());
        session.lobby_entered_ms = now_ms;
        transition_stage(RaidStage::Lobby, now_ms, "lobby", "Confirmed raid pass; entering lobby");
        return;
    }

    if (!coin_flag && session.modal_cycle_attempts < kMaxPassCurrencyCycles) {
        ++session.modal_cycle_attempts;
        adapter().cycle_modal_currency(session.ticket_modal.get());
        return;
    }

    adapter().dismiss_modal_ticket(session.ticket_modal.get());
    adapter().exit_raid_or_gym(session.approach_ui.get(), session.raid_state.get(), session.gym_root_state.get());
    finish_session("error", "UseRaidTicketGuiController selected unauthorized or coin pass; aborted");
}

void step_lobby(int64_t now_ms) {
    if (adapter().is_battle_active()) {
        session.uncertain_action = false;
        transition_stage(RaidStage::Battle, now_ms, "battle", "Raid battle in progress");
        return;
    }

    bool battle_started = false;
    int current_players = 0;
    bool has_countdown = false;
    int64_t countdown_ms = 0;
    if (!adapter().poll_lobby_state(session.lobby_ui, battle_started, current_players, has_countdown, countdown_ms)) {
        if (now_ms - session.stage_started_ms >= kApproachTimeoutMs) {
            finish_session("error", "Raid lobby did not open; entry outcome is unknown, so no retry was sent");
        } else {
            maybe_emit_heartbeat(now_ms);
        }
        return;
    }

    const int64_t lobby_elapsed_ms = std::max<int64_t>(0, now_ms - session.lobby_entered_ms);
    require(current_players>=0&&current_players<=100,"Raid lobby player count invalid");
    const auto dec = pgr_auto_raid::evaluate_lobby_decision(
        session.command,
        session.stop_requested,
        battle_started,
        current_players,
        lobby_elapsed_ms,
        has_countdown,
        countdown_ms);

    switch (dec) {
        case pgr_auto_raid::LobbyDecision::EnterBattle:
            session.uncertain_action = false;
            transition_stage(RaidStage::Battle, now_ms, "battle", "Raid battle started");
            return;

        case pgr_auto_raid::LobbyDecision::LeaveStopRequested:
            adapter().leave_lobby(session.lobby_ui.get(), session.raid_state.get());
            adapter().exit_raid_or_gym(session.approach_ui.get(), session.raid_state.get(), session.gym_root_state.get());
            finish_session("stopped", "Left raid lobby before battle start");
            return;

        case pgr_auto_raid::LobbyDecision::LeaveUnderMin:
            adapter().leave_lobby(session.lobby_ui.get(), session.raid_state.get());
            quarantine_and_rescan(
                "Leaving lobby: players (" + std::to_string(current_players) + "/" +
                    std::to_string(session.command.min_players) + ") below minimum; "+
                    (has_countdown?"deadline reached":"countdown unavailable"),
                now_ms);
            return;

        case pgr_auto_raid::LobbyDecision::WaitForPlayers:
        case pgr_auto_raid::LobbyDecision::StayReady:
        default:
            if (now_ms - session.last_heartbeat_ms >= kHeartbeatIntervalMs) {
                emit_status(session.command.token, "lobby",
                            "Lobby players: " + std::to_string(current_players) + "/" +
                                std::to_string(session.command.min_players),
                            tracker.completed_raids);
            }
            return;
    }
}

void step_resolve(int64_t now_ms);

void step_battle(int64_t now_ms) {
    int battle_status = pgr_auto_raid::BATTLE_STATUS_ACTIVE;
    bool advanced = false;
    if (adapter().poll_resolve_state(session.resolve_ui, battle_status, advanced)) {
        transition_stage(RaidStage::Resolve, now_ms, "rewards", "Raid battle finished; resolving outcome");
        step_resolve(now_ms);
        return;
    }

    bool shiny = false;
    int default_ball = 5;
    if (adapter().poll_raid_encounter(session.coord_encounter, session.raid_map_pokemon, shiny, default_ball)) {
        session.boss_shiny = shiny;
        transition_stage(RaidStage::Encounter, now_ms, "catching", "Entered raid bonus catch encounter");
        return;
    }

    if (now_ms - session.stage_started_ms >= kBattleTimeoutMs) {
        finish_session("error", "Battle progress timed out; outcome not confirmed, so no retry was sent");
        return;
    }
    maybe_emit_heartbeat(now_ms);
}

void step_resolve(int64_t now_ms) {
    bool shiny = false;
    int default_ball = 5;
    if (adapter().poll_raid_encounter(session.coord_encounter, session.raid_map_pokemon, shiny, default_ball)) {
        session.boss_shiny = shiny;
        transition_stage(RaidStage::Encounter, now_ms, "catching",
                         shiny ? "Shiny raid boss encountered!" : "Catching raid boss");
        return;
    }

    if (now_ms - session.stage_started_ms >= kResolveTimeoutMs) {
        finish_session("error", "Raid rewards did not finish; check the game before restarting");
        return;
    }
    if (session.last_step_ms != 0 && now_ms - session.last_step_ms < kActionStepMs) {
        maybe_emit_heartbeat(now_ms);
        return;
    }
    session.last_step_ms = now_ms;

    int battle_status = pgr_auto_raid::BATTLE_STATUS_UNSET;
    bool advanced = false;
    if (!adapter().poll_resolve_state(session.resolve_ui, battle_status, advanced)) {
        maybe_emit_heartbeat(now_ms);
        return;
    }

    if (battle_status == pgr_auto_raid::BATTLE_STATUS_DEFEAT ||
        battle_status == pgr_auto_raid::BATTLE_STATUS_TIMED_OUT) {
        handle_raid_loss(battle_status == pgr_auto_raid::BATTLE_STATUS_TIMED_OUT ? "timed out" : "defeat",
                         now_ms);
        return;
    }
    if (advanced) {
        session.resolve_confirmed = true;
    }
    maybe_emit_heartbeat(now_ms);
}

void await_catch_summary(bool shiny,const std::string& message,int64_t now_ms){
    session.completion_shiny=shiny;session.completion_message=message;
    transition_stage(RaidStage::CatchSummary,now_ms,"summary",message+"; waiting for the map");
}

void step_catch_summary(const TransportPermission& perm,int64_t now_ms){
    if(perm.item_access && adapter().map_ready()){
        handle_raid_completion(session.completion_shiny,session.completion_message,now_ms);
        return;
    }
    if(now_ms-session.stage_started_ms>=kResolveTimeoutMs){
        finish_session("error","Catch result received, but the game did not return to the map; check the result screen");
        return;
    }
    if(!session.last_step_ms||now_ms-session.last_step_ms>=kActionStepMs){
        session.last_step_ms=now_ms;
        adapter().close_encounter_summary();
    }
    maybe_emit_heartbeat(now_ms);
}

void step_encounter(uint64_t tid, int64_t now_ms) {
    if (session.berry_promise) {
        int result = BerryRefused;
        const int rc = adapter().poll_berry_promise(session.berry_promise.get(), tid, result);
        if (rc == PGR_VR_PENDING) {
            if (now_ms - session.promise_started_ms >= kPromiseTimeoutMs)
                finish_session("error", "Berry request timed out; check the encounter (not retried)");
            else maybe_emit_heartbeat(now_ms);
            return;
        }
        session.berry_promise.reset();
        session.uncertain_action = false;
        if (rc != PGR_VR_DONE || result == BerryRefused) {
            finish_session("error", "Berry request failed; check the encounter (not retried)");
            return;
        }
        if (result == BerryNoStock) {
            session.berry_unavailable |= session.berry_item == 706 ? 2u : 1u;
            emit_status(session.command.token, "catching", "Selected berry unavailable; checking fallback", tracker.completed_raids);
        } else {
            session.berry_prepared = true;
            session.last_step_ms = now_ms;
            emit_status(session.command.token, "catching", "Berry applied; catching raid boss", tracker.completed_raids);
        }
        return;
    }
    if (session.pending_promise) {
        int catch_status = 0;
        const int rc = adapter().poll_catch_promise(session.pending_promise.get(), tid, catch_status);
        if (rc == PGR_VR_PENDING) {
            if (now_ms - session.promise_started_ms >= kPromiseTimeoutMs) {
                session.pending_promise.reset();
                session.uncertain_action = false;
                finish_session("error", "Catch request timed out; check the game before restarting (not retried)");
            } else {
                maybe_emit_heartbeat(now_ms);
            }
            return;
        }
        session.pending_promise.reset();
        session.uncertain_action = false;
        session.last_step_ms = now_ms;

        if (rc != PGR_VR_DONE) {
            finish_session("error", "Catch request failed; check the game before restarting (not retried)");
            return;
        }

        // CatchPokemonOutProto.Types.Status: 1=CatchSuccess, 2=CatchEscape, 3=CatchFlee, 4=CatchMissed
        if (catch_status == 1) {
            await_catch_summary(session.boss_shiny,
                                   session.boss_shiny ? "Caught shiny raid boss!" : "Caught raid boss",
                                   now_ms);
            return;
        }
        if (catch_status == 3) {
            await_catch_summary(false, "Raid boss fled after bonus challenge; raid completed", now_ms);
            return;
        }
        if ((catch_status == 2 || catch_status == 4) && session.throws_made < kMaxRaidThrows) {
            if (session.command.berry_mode) {
                if (catch_status == 2) adapter().clear_raid_berry();
                session.berry_prepared = false;
                session.berry_wait_started_ms = 0;
            }
            maybe_emit_heartbeat(now_ms);
            return;
        }
        if(catch_status!=2 && catch_status!=4){finish_session("error","Unknown catch result "+std::to_string(catch_status)+"; completion not confirmed");return;}
        finish_session("error", "Bonus catch safety limit reached; check the encounter (completion not confirmed)");
        return;
    }

    if (now_ms - session.stage_started_ms >= kEncounterTimeoutMs) {
        finish_session("error", "Timed out in raid bonus encounter; check the game before restarting");
        return;
    }
    if (session.last_step_ms != 0 && now_ms - session.last_step_ms < kThrowCadenceMs) {
        maybe_emit_heartbeat(now_ms);
        return;
    }

    bool shiny = false;
    int default_ball = 5;
    if (!adapter().poll_raid_encounter(session.coord_encounter, session.raid_map_pokemon, shiny, default_ball)) {
        finish_session("error", "Raid bonus encounter unavailable; catch outcome not confirmed");
        return;
    }
    if (shiny) session.boss_shiny = true;

    if (session.command.berry_mode && !session.berry_prepared) {
        RaidBerrySnapshot stock{};
        const bool ready = adapter().inspect_raid_berry(session.coord_encounter.get(), session.raid_map_pokemon.get(), stock);
        if (!ready || (!stock.active && !stock.can_feed)) {
            if (!session.berry_wait_started_ms) {
                session.berry_wait_started_ms = now_ms;
                emit_status(session.command.token, "catching", "Waiting for raid berry controls", tracker.completed_raids);
            } else if (now_ms - session.berry_wait_started_ms >= kPromiseTimeoutMs) {
                finish_session("error", "Raid berry controls did not become ready; no berry or throw sent");
            }
            return;
        }
        session.berry_wait_started_ms = 0;
        const int item = stock.active ? 0 : pgr_auto_raid::select_raid_berry(session.command.berry_mode,
            (session.berry_unavailable & 1u) ? 0 : stock.razz,
            (session.berry_unavailable & 2u) ? 0 : stock.golden);
        emit_raw_event("{\"token\":"+json_quote(std::to_string(session.command.token))+
            ",\"phase\":\"berry-choice\",\"preferred\":"+std::to_string(session.command.berry_mode)+
            ",\"selected\":"+std::to_string(item)+",\"active\":"+std::to_string(stock.active)+
            ",\"razzStock\":"+std::to_string(stock.razz)+",\"goldenStock\":"+std::to_string(stock.golden)+
            ",\"unavailableMask\":"+std::to_string(session.berry_unavailable)+"}");
        if (item) {
            session.berry_item = item;
            session.promise_started_ms = now_ms;
            session.uncertain_action = true;
            session.berry_promise = adapter().submit_raid_berry(session.coord_encounter.get(), session.raid_map_pokemon.get(), item);
            require(bool(session.berry_promise), "Berry request returned no promise; check the encounter (not retried)");
            emit_status(session.command.token, "catching", item == 706 ? "Applying Golden Razz Berry" :
                session.command.berry_mode == 706 ? "Golden Razz unavailable; applying Razz Berry" : "Applying Razz Berry", tracker.completed_raids);
            return;
        }
        session.berry_prepared = true;
        emit_status(session.command.token, "catching", stock.active ? "Keeping active berry; catching raid boss" :
                    "No selected berries in stock; catching without a berry", tracker.completed_raids);
    }

    session.last_step_ms = now_ms;
    session.promise_started_ms = now_ms;
    session.uncertain_action = true;
    ++session.throws_made;
    session.pending_promise = adapter().submit_raid_throw(
        session.coord_encounter.get(), session.raid_map_pokemon.get(), default_ball);
    require(bool(session.pending_promise), "Raid catch throw promise null; outcome unknown");
    maybe_emit_heartbeat(now_ms);
}

bool step_controller(uint64_t tid) {
    const int64_t now_ms = clock_ms();
    const TransportPermission perm = read_permission();

    // Fast-exit BEFORE resolving bootstrap or game services when no session is active
    // and command is absent, invalid, disabled, or already finished for this token.
    if (!session.active) {
        if (!perm.valid) return false;
        if (perm.command.token <= tracker.finished_token) return false;
        if (!perm.command.enabled) {
            tracker.finished_token = perm.command.token;
            emit_status(perm.command.token, "stopped", "Auto-Raid stopped", 0);
            return false;
        }
        if (!perm.foreground) return false;

        adapter().ensure_ready();
        if (perm.command.berry_mode)
            require(adapter().ensure_berry_ready(), "Raid berry bindings unavailable; no raid started");
        const std::string account = adapter().read_account_id();

        tracker.reset_for_token(perm.command.token);
        session.reset();
        session.active         = true;
        session.command        = perm.command;
        session.cooldown_ms    = perm.cooldown_ms;
        session.account        = account;
        transition_stage(RaidStage::Scan, now_ms, "scanning", "Auto-Raid started; scanning nearby gyms");
    } else {
        // A concurrent fresh snapshot is retried, but never licenses game actions.
        if (perm.retry_snapshot) return true;
        // Lease loss, account/foreground teardown and missing commands revoke ALL
        // game work, including promise polling. Only owned GC handles are released.
        if (!perm.snapshot_coherent || !perm.valid || !perm.foreground) {
            finish_session("stopped", !perm.snapshot_coherent ? "Auto-Raid transport expired" :
                           !perm.valid ? "Auto-Raid command revoked" : "Auto-Raid left foreground");
            return false;
        }
        if (perm.command.token != session.command.token) {
            finish_session("stopped", "Auto-Raid session changed; old actions revoked");
            return false;
        }
        session.cooldown_ms = perm.cooldown_ms;
        if (!perm.command.enabled) session.stop_requested = true;
        // Spending/filter settings are frozen for the token. Only cooldown and Stop update live.

        const std::string current_account = adapter().read_account_id();
        if (current_account != session.account) {
            finish_session("stopped", "Account changed; Auto-Raid session terminated");
            return false;
        }
    }

    switch (session.stage) {
        case RaidStage::Scan:
            step_scan(perm, now_ms);
            break;
        case RaidStage::Travel:
            step_travel(perm, now_ms);
            break;
        case RaidStage::OpenGym:
            step_open_gym(now_ms);
            break;
        case RaidStage::VerifyPass:
            step_verify_pass(now_ms);
            break;
        case RaidStage::ConfirmModalPass:
            step_confirm_modal_pass(now_ms);
            break;
        case RaidStage::Lobby:
            step_lobby(now_ms);
            break;
        case RaidStage::Battle:
            step_battle(now_ms);
            break;
        case RaidStage::Resolve:
            step_resolve(now_ms);
            break;
        case RaidStage::Encounter:
            step_encounter(tid, now_ms);
            break;
        case RaidStage::CatchSummary:
            step_catch_summary(perm,now_ms);
            break;
        case RaidStage::Idle:
        default:
            finish_session("stopped", "Auto-Raid idle");
            break;
    }

    return session.active;
}

} // namespace

bool pgr_auto_raid_tick(uint64_t tid, const PgrSharedMetadataOutput* metadata) {
    if (busy || tid == 0 || tid != static_cast<uint64_t>(gettid()) || (owner != 0 && owner != tid)) {
        return session.active;
    }
    struct BusyGuard {
        BusyGuard() { busy = true; }
        ~BusyGuard() { busy = false; }
    } guard;

    owner = tid;
    md = metadata;

    try {
        return step_controller(tid);
    } catch (const Failure& e) {
        if (!session.active) {
            const TransportPermission perm = read_permission();
            if (perm.valid && perm.command.token != 0) {
                tracker.finished_token = perm.command.token;
                emit_status(perm.command.token, "error",
                            std::string("Auto-Raid stopped: ") + e.where,
                            tracker.completed_raids);
            }
            return false;
        }
        finish_session("error", std::string("Auto-Raid stopped: ") + e.where);
        return false;
    } catch (...) {
        if (session.active) {
            finish_session("error", "Auto-Raid stopped: unexpected native failure");
        }
        return false;
    }
}

void pgr_auto_raid_stop(uint64_t tid) {
    if (tid == 0 || (owner != 0 && owner != tid) || tid != static_cast<uint64_t>(gettid())) {
        return;
    }
    if (busy) {
        session.stop_requested = true;
        return;
    }
    if (session.active) {
        finish_session("stopped", "Auto-Raid stopped on lifecycle cleanup");
    } else {
        session.reset();
    }
}

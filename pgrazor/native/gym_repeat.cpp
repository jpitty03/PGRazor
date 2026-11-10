#include "game_bindings.h"
#include "gym_repeat.h"
#include "gym_repeat_policy.h"

#include "hook_engine.h"
#include "heal.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include "shared_metadata.h"

#include <android/log.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Gym "Battle until defeated" native leaf. All managed interaction happens on the
// proven Unity owner thread. Hooks capture lightweight signals and rooted controllers;
// every managed resolve/invoke runs on the scheduled tick (non-reentrant). The single
// game-owned action the policy returns each tick is already consumed by the policy
// (which advances phase/ARMED before returning), so the adapter never mutates policy
// state after an invoke; it only re-validates UI/gym/account immediately before the
// side effect and performs it exactly once.
namespace {
using namespace pgr_shared_meta;
using namespace pgr_gym_repeat;

struct Failure { const char *where; };
void require(bool ok, const char *where) { if (!ok) throw Failure{where}; }

// --- Process-lifetime state -------------------------------------------------------
PgrBootBinding boot{};
const PgrSharedMetadataOutput *md = nullptr;
uint64_t owner = 0;
std::atomic<bool> g_ready{false};     // required hooks installed
std::atomic<bool> g_faulted{false};   // unrecoverable managed fault -> inert
std::atomic<bool> g_cancel{false};    // off-owner stop; consumed on the owner tick
bool busy = false;                    // owner-thread tick reentrancy guard
bool self_action = false;             // our own ClickBattle/Battle/OnTapped invoke in progress
uint32_t g_installed_hooks = 0;

Policy policy;
Timing timing;

// Identity of the armed gym, kept adapter-side so we can compare the FULL FortId
// string (not just its hash) and bind the owning account.
std::string g_gym_fort_id;
uint64_t g_account_hash = 0, g_generation = 0;
bool g_manual_cancel = false;

// Diagnostics
uint64_t g_sessions = 0, g_actions = 0;
EndReason g_last_end = END_NONE; // preserved across report/reset for the sample surface
std::string g_last_status;

// Runtime-resolved enum literals (BattleResultsExit / BattleStatus).
struct Literals { int cont = 0, leave_gym = 0, leave_battle = 0, victory = 0; bool ready = false; } g_lit;

// Hooked method slots.
enum Slot {
    HK_APPROACH_OPEN = 0,
    HK_APPROACH_BACK,
    HK_PREP_OPEN,
    HK_PREP_BATTLE,
    HK_PREP_EXIT,
    HK_RESULTS_EXIT,
    HK_COUNT
};
std::atomic<void *> g_orig[HK_COUNT]{};
void *g_approach_class = nullptr;
void *g_prep_class = nullptr;
void *g_results_class = nullptr;

// Invoke-only resolved methods. OnBackButtonPressed is hooked (manual-cancel capture)
// but never self-invoked: the loop no longer presses Back for a normal repeat.
void *m_click_battle = nullptr;    // GymApproachGuiController.ClickBattle()
void *m_prep_battle = nullptr;     // GymBattlePrepUIController.Battle()
void *m_frame_complete = nullptr;  // BattleFrame.get_BattleComplete()
void *m_frame_fainted = nullptr;   // BattleFrame.get_AllPlayerPokemonFainted()

// --- Ordered event queue (owner-thread single-producer/consumer) -------------------
// Events are captured at hook/forward time (with their event-time gym identity where
// it is resolved synchronously) and drained in FIFO order on the tick, so a deferred
// transition keeps its own identity and the policy observes them in order.
enum EvType : uint8_t {
    EV_ARM = 0, EV_SCOPE_ENTER, EV_SCOPE_EXIT, EV_RESULTS, EV_MANUAL_CANCEL
};
struct Event {
    EvType   type = EV_ARM;
    bool     hostile = false;               // EV_ARM
    bool     victorious_terminal = false;   // EV_RESULTS
    bool     fainted = false;               // EV_RESULTS
    uint64_t gym = 0;                       // EV_ARM / EV_SCOPE_ENTER identity hash
    uint64_t account = 0;                   // EV_ARM
    char     fort[48] = {0};                // EV_ARM FortId string
};
constexpr size_t kEvCap = 32;
Event g_ev[kEvCap];
size_t g_ev_head = 0, g_ev_count = 0;
void push_ev(const Event &e) {
    if (e.type == EV_MANUAL_CANCEL) g_manual_cancel = true;
    if (g_ev_count >= kEvCap) { g_cancel.store(true); return; }
    g_ev[(g_ev_head + g_ev_count) % kEvCap] = e;
    ++g_ev_count;
}
bool pop_ev(Event &e) {
    if (!g_ev_count) return false;
    e = g_ev[g_ev_head];
    g_ev_head = (g_ev_head + 1) % kEvCap;
    --g_ev_count;
    return true;
}
void clear_events() { g_ev_head = g_ev_count = 0; g_manual_cancel = false; }

uint64_t clock_ms() { timespec t{}; clock_gettime(CLOCK_BOOTTIME, &t); return uint64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000; }
uint64_t self_tid() { return uint64_t(gettid()); }

// --- Rooting + managed helpers (mirror gym_runtime.cpp invariants) -----------------
template <class F> F fn(void *p) { require(p, "missing runtime export"); return reinterpret_cast<F>(p); }
void *exp(const char *name) { std::string n = "il2cpp_"; n += name; void *p = dlsym(boot.handle, n.c_str()); require(p, name); return p; }

struct Root {
    uintptr_t h = 0;
    Root() = default;
    explicit Root(void *v) { if (v) { h = fn<uintptr_t (*)(void *, int)>(boot.runtime.gchandle_new)(v, 0); require(h, "GC root"); } }
    Root(const Root &) = delete;
    Root &operator=(const Root &) = delete;
    Root(Root &&r) noexcept : h(r.h) { r.h = 0; }
    Root &operator=(Root &&r) noexcept { if (this != &r) { reset(); h = r.h; r.h = 0; } return *this; }
    ~Root() { reset(); }
    void reset() { if (h) { fn<void (*)(uintptr_t)>(boot.runtime.gchandle_free)(h); h = 0; } }
    void *get() const { return h ? fn<void *(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h) : nullptr; }
    explicit operator bool() const { return get() != nullptr; }
};

// Controllers rooted while a session is re-entering the gym; consumed by the tick.
Root g_approach_root; // GymApproachGuiController for ClickBattle/Back
Root g_prep_root;     // GymBattlePrepUIController for Battle
Root g_badge_root;
bool g_badge_sent=false, g_badge_retry=false;
uint64_t g_badge_scan=0;

void *klass(void *o) { require(o, "null object"); return fn<void *(*)(void *)>(boot.runtime.object_get_class)(o); }
void *cls(const char *ns, const char *name) {
    const auto *api = static_cast<const PgrMetaApi *>(pgrazor_runtime_metadata_api(1));
    require(api && api->resolve_class, "metadata API");
    PgrMetaClassQuery q{}; q.version = 1; q.size = sizeof(q); q.name_space = ns; q.name = name; q.canonical_match = PGR_MD_MATCH_EXACT;
    PgrMetaClassResult out{}; require(api->resolve_class(&q, &out) == 0, name);
    return const_cast<void *>(out.klass);
}
void *method_named(void *k, const char *name, int count) {
    void *m = fn<void *(*)(void *, const char *, int)>(boot.meta.class_get_method_from_name)(k, name, count);
    require(m, name);
    require(fn<unsigned (*)(void *)>(boot.meta.method_get_param_count)(m) == unsigned(count), name);
    return m;
}
void *invoke(void *m, void *o, void **args = nullptr) {
    void *ex = nullptr;
    void *v = fn<void *(*)(void *, void *, void **, void **)>(boot.runtime.runtime_invoke)(m, o, args, &ex);
    require(!ex, "managed call failed");
    return v;
}
void *call(void *o, const char *name, int n = 0, void **args = nullptr) { return invoke(method_named(klass(o), name, n), o, args); }
template <class T> T unbox(void *o) { require(o, "null result"); T v{}; auto p = fn<void *(*)(void *)>(boot.runtime.object_unbox)(o); require(p, "boxed value"); memcpy(&v, p, sizeof(v)); return v; }
bool call_bool0(void *o, const char *name) { return unbox<uint8_t>(call(o, name, 0)) != 0; }

// Field access with full type validation (type name + instance flag + value size),
// mirroring gym_runtime.cpp's field_info; the raw type argument is now enforced.
std::string typename_of(void *type) { char *p = fn<char *(*)(void *)>(boot.meta.type_get_name)(type); require(p, "type name"); std::string s = p; fn<void (*)(void *)>(boot.meta.mem_free)(p); return s; }
void *field_info(void *k, const char *name, const char *type) {
    void *f = fn<void *(*)(void *, const char *)>(boot.meta.class_get_field_from_name)(k, name);
    require(f, name);
    void *ft = fn<void *(*)(void *)>(boot.meta.field_get_type)(f);
    require(typename_of(ft) == type, name);
    require(!(fn<int (*)(void *)>(boot.meta.field_get_flags)(f) & 0x10), "instance field required");
    return f;
}
template <class T> T field(void *o, const char *name, const char *type) {
    T v{}; void *f = field_info(klass(o), name, type);
    fn<void (*)(void *, void *, void *)>(boot.runtime.field_get_value)(o, f, &v);
    return v;
}
// Static enum literal value (STATIC|LITERAL|HAS_DEFAULT), as in gym_runtime.cpp.
int literal(void *k, const char *name) {
    void *f = fn<void *(*)(void *, const char *)>(boot.meta.class_get_field_from_name)(k, name);
    require(f, name);
    require((fn<int (*)(void *)>(boot.meta.field_get_flags)(f) & 0x8050) == 0x8050, name);
    int n = 0; fn<void (*)(void *, void *)>(boot.runtime.field_static_get_value)(f, &n); return n;
}

void *h(unsigned id) { return md ? const_cast<void *>(md->opt_handles[id]) : nullptr; }
Root type_object(void *k) { return Root(fn<void *(*)(void *)>(exp("type_get_object"))(fn<void *(*)(void *)>(boot.meta.class_get_type)(k))); }
Root resolve(const char *ns, const char *name) {
    void *mh = h(H_IL_MAP), *fm = h(H_IL_FIND_M), *cm = h(H_IL_CONTAINER_M), *rm = h(H_IL_RESOLVE_M);
    if (!mh || !fm || !cm || !rm) return {};
    Root type = type_object(mh); void *a[] = {type.get()}; Root mapo(invoke(fm, nullptr, a)); if (!mapo) return {};
    Root container(invoke(cm, mapo.get())); if (!container) return {};
    Root target = type_object(cls(ns, name)); void *ra[] = {target.get()}; return Root(invoke(rm, container.get(), ra));
}

std::string read_string(void *o) {
    if (!o) return {};
    if (klass(o) != cls("System", "String")) return {};
    int n = fn<int (*)(void *)>(exp("string_length"))(o);
    if (n < 0 || n > 256) return {};
    auto p = fn<const uint16_t *(*)(void *)>(boot.runtime.string_chars)(o);
    std::string s; s.reserve(n);
    for (int i = 0; i < n; ++i) { if (p[i] > 127) return {}; s += char(p[i]); }
    return s;
}
uint64_t hash_id(const std::string &s) {
    if (s.empty()) return 0;
    uint64_t v = 1469598103934665603ull;
    for (unsigned char c : s) { v ^= c; v *= 1099511628257ull; }
    return v ? v : 1; // never collapse a real id to the "unknown" sentinel
}

bool alive(void *o) {
    void *am = md ? const_cast<void *>(md->methods[5]) : nullptr; if (!am || !o) return false;
    void *a[] = {o}; void *r = invoke(am, nullptr, a); return r && unbox<uint8_t>(r) != 0;
}

// Lazily resolve the enum literals we compare against, on the attached owner tick.
void ensure_literals() {
    if (g_lit.ready) return;
    try {
        void *be = cls("Niantic.Holoholo.Gym", "BattleResultsExit");
        void *bs = cls("Niantic.Holoholo.Battle", "BattleStatus");
        Literals lit;
        lit.cont = literal(be, "CONTINUE_BATTLE");
        lit.leave_gym = literal(be, "LEAVE_GYM");
        lit.leave_battle = literal(be, "LEAVE_BATTLE");
        lit.victory = literal(bs, "Victory");
        lit.ready = true;
        g_lit = lit;
    } catch (...) { /* retry next tick; classification stays conservative until ready */ }
}

bool raid_active(void *raid) {
    if (!raid) return false;
    require(h(H_MP_BATTLE) && h(H_MP_END), "raid time metadata unavailable");
    int64_t start = 0, end = 0;
    fn<void (*)(void *, void *, void *)>(boot.runtime.field_get_value)(raid, h(H_MP_BATTLE), &start);
    fn<void (*)(void *, void *, void *)>(boot.runtime.field_get_value)(raid, h(H_MP_END), &end);
    int64_t wall = int64_t(time(nullptr)) * 1000;
    return start <= wall && end > wall;
}

// Classify the live gym from the resolved root controller (concrete GymRootController),
// filling the FortId it reports. get_IsNeutral exists on 0.435.2; a missing method
// throws (fail closed) rather than being treated as hostile.
GymStatus root_gym_status(Root &rc, std::string &fort_out) {
    fort_out = read_string(call(rc.get(), "get_FortId"));
    if (fort_out.empty()) return GYM_UNKNOWN;
    if (call_bool0(rc.get(), "get_IsFriendly") || call_bool0(rc.get(), "get_IsNeutral")) return GYM_CLEARED;
    if (!call_bool0(rc.get(), "get_Enabled")) return GYM_UNAVAILABLE;
    if (call_bool0(rc.get(), "get_IsBattleDisabled")) return GYM_UNAVAILABLE;
    if (!call_bool0(rc.get(), "get_IsPlayerInRange")) return GYM_OUT_OF_RANGE;
    if (call_bool0(rc.get(), "get_HasRaidInfo")) {
        Root raid(call(rc.get(), "get_RaidInfo"));
        if (raid && raid_active(raid.get())) return GYM_RAID;
    }
    if (call_bool0(rc.get(), "get_IsFriendly") || call_bool0(rc.get(), "get_IsNeutral")) return GYM_CLEARED;
    return GYM_HOSTILE;
}

uint64_t player_id_hash() {
    try {
        Root player = resolve("Niantic.Holoholo", "IPlayerService");
        if (!player) return 0;
        return hash_id(read_string(call(player.get(), "get_PlayerId")));
    } catch (...) { return 0; }
}

// --- Status surface (Android log tag + existing battle-status transport action) ---
const PgrRuntimeTransportApi *transport() { return static_cast<const PgrRuntimeTransportApi *>(pgrazor_runtime_transport_api(1)); }
void status(const char *msg) {
    if (g_last_status == msg) return;
    g_last_status = msg;
    __android_log_print(ANDROID_LOG_INFO, "PGRazorGymRepeat", "%s", msg);
    const auto *tr = transport();
    if (tr && tr->emit) { PgrRuntimeTransportString s{msg, uint32_t(std::strlen(msg))}; tr->emit(24, nullptr, 0, &s, 1); }
}

// --- Gate from transport (bool 40 enabled, 3 active, 6 battles, fresh/fault) -------
Gate read_gate() {
    Gate g;
    g.foreground = !g_faulted.load(std::memory_order_acquire);
    const auto *tr = transport();
    if (!tr || !tr->sample || !tr->get_bool) { g.fresh = false; return g; }
    PgrRuntimeTransportSample a{};
    if (tr->sample(&a) != 0 || !a.snapshot_fresh || a.fault) { g.fresh = false; return g; }
    int enabled = 0, active = 0, battles = 0;
    tr->get_bool(40, &enabled);
    tr->get_bool(3, &active);
    tr->get_bool(6, &battles);
    PgrRuntimeTransportSample z{};
    if (tr->sample(&z) != 0 || !z.snapshot_fresh || z.snapshot_seq != a.snapshot_seq || z.fault || z.generation != a.generation) { g.fresh = false; return g; }
    g.enabled = enabled == 1;
    g.active = active == 1;
    g.battles_allowed = battles == 1;
    return g;
}
bool map_visible() {
    const auto *tr = transport();
    if (!tr || !tr->get_bool) return false;
    int vis = 0; return tr->get_bool(38, &vis) == 0 && vis == 1;
}

// --- Live map-gym resolution for the on-map re-entry branch ------------------------
double haversine_m(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0, d = M_PI / 180.0;
    double dlat = (lat2 - lat1) * d, dlon = (lon2 - lon1) * d;
    double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
               std::cos(lat1 * d) * std::cos(lat2 * d) * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2.0 * R * std::asin(std::min(1.0, std::sqrt(a)));
}
Root fort_of(Root &gym) {
    void *fm = h(H_VGP_M_FORT); if (!fm) return {};
    void *vm = fn<void *(*)(void *, void *)>(exp("object_get_virtual_method"))(gym.get(), fm);
    if (!vm) return {};
    return Root(invoke(vm, gym.get()));
}
// Returns the live status for the armed gym, keeping the matched MapGym in `out`.
// Matches by the FULL FortId string (exact), never only by hash.
GymStatus resolve_map_gym(Root &out) {
    try {
        void *find = h(H_VGP_M_FIND_ALL); if (!find || !h(H_VGP_M_FORT)) return GYM_UNAVAILABLE;
        if (!map_visible()) return GYM_UNKNOWN; // not actually on the map yet
        const auto *tr = transport();
        double plat = 0, plon = 0; int has = 0;
        if (!tr || tr->get_double(0, &plat, &has) != 0 || !has) return GYM_UNAVAILABLE; has = 0;
        if (tr->get_double(1, &plon, &has) != 0 || !has) return GYM_UNAVAILABLE;
        Root type = type_object(cls("Niantic.Holoholo.Gym", "MapGym"));
        void *a[] = {type.get()}; Root gyms(invoke(find, nullptr, a)); if (!gyms) return GYM_UNAVAILABLE;
        size_t n = fn<size_t (*)(void *)>(exp("array_length"))(gyms.get());
        if (n > 4096) return GYM_UNAVAILABLE;
        for (size_t i = 0; i < n; ++i) {
            Root gym(*reinterpret_cast<void **>(static_cast<char *>(gyms.get()) + 32 + i * 8));
            if (!gym || !alive(gym.get())) continue;
            if (klass(gym.get()) != cls("Niantic.Holoholo.Gym", "MapGym")) continue;
            Root f = fort_of(gym); if (!f) continue;
            std::string fid = read_string(field<void *>(f.get(), "FortId", "System.String"));
            if (fid.empty() || fid != g_gym_fort_id) continue; // exact-string match for OUR gym
            // Matched our gym: classify its live state.
            if (!field<bool>(f.get(), "Enabled", "System.Boolean")) return GYM_UNAVAILABLE;
            double meters = haversine_m(plat, plon,
                                        field<double>(f.get(), "Latitude", "System.Double"),
                                        field<double>(f.get(), "Longitude", "System.Double"));
            if (!(meters <= 80.0)) return GYM_OUT_OF_RANGE;
            Root raid(field<void *>(f.get(), "RaidInfo", "Holoholo.Rpc.RaidInfoProto"));
            if (raid && raid_active(raid.get())) return GYM_RAID;
            if (call_bool0(gym.get(), "get_InGymState")) return GYM_UNKNOWN; // still in a gym screen
            if (call_bool0(gym.get(), "get_IsFriendly") || call_bool0(gym.get(), "get_IsNeutral")) return GYM_CLEARED;
            out = std::move(gym);
            return GYM_HOSTILE;
        }
        return GYM_UNAVAILABLE; // our gym is no longer loaded/visible
    } catch (...) { return GYM_UNAVAILABLE; }
}

// --- Hook handlers ----------------------------------------------------------------
using Fn2 = void (*)(void *, void *);
using FnResults = void (*)(void *, void *, int32_t, void *);

void *load_original(Slot s) { void *p = g_orig[s].load(std::memory_order_acquire); require(p, "original trampoline"); return p; }
// Capture hooks must still fire during our own self_action (a synchronous
// approach->prep transition is generated inside ClickBattle/Battle). Only the manual
// cancel paths additionally gate on !self_action.
bool on_owner() { return g_ready.load(std::memory_order_acquire) && !g_faulted.load(std::memory_order_acquire) &&
                         owner && self_tid() == owner &&
                         boot.runtime.thread_current &&
                         fn<void *(*)()>(boot.runtime.thread_current)() != nullptr; }

template <Slot S> void hook_capture_root(void *self, void *method) {
    auto orig = reinterpret_cast<Fn2>(load_original(S));
    // Only root the approach/prep controllers while a session is live (never root
    // unrelated GUIs while idle/disabled). Capturing survives self_action.
    Root receiver;
    if (on_owner() && self && policy.armed()) {
        try {
            receiver = Root(self);
            if (S == HK_APPROACH_OPEN) g_approach_root = Root(receiver.get());
            else if (S == HK_PREP_OPEN) g_prep_root = Root(receiver.get());
        } catch (...) { /* never break the original on a rooting fault */ }
    }
    orig(receiver ? receiver.get() : self, method);
}

void hook_prep_battle(void *self, void *method) {
    auto orig = reinterpret_cast<Fn2>(load_original(HK_PREP_BATTLE));
    if (!on_owner() || self_action || !self || !read_gate().enabled) { orig(self, method); return; }
    Root receiver;
    try {
        receiver = Root(self);
        Root rc(field<void *>(receiver.get(), "dyka", "Niantic.Holoholo.Gym.IGymRootController"));
        if (rc) {
            std::string fort; const auto st = root_gym_status(rc, fort);
            Event e{}; e.type = EV_ARM;
            if (st == GYM_HOSTILE && !fort.empty() && fort.size() < sizeof(e.fort)) {
                e.hostile = true; e.gym = hash_id(fort); e.account = player_id_hash();
                std::strncpy(e.fort, fort.c_str(), sizeof(e.fort)-1);
                // Read HP before the first manual round; never use an item in this hook.
                // Later rounds compare against the post-heal storage snapshot.
                const Gate gate = read_gate();
                if (e.account && gate.enabled && gate.active && gate.battles_allowed && gate.fresh)
                    pgr_heal_gym_baseline(owner, md);
                push_ev(e);
            }
        }
    } catch (...) { /* cannot prove this manual start */ }
    orig(receiver ? receiver.get() : self, method);
}

void hook_approach_back(void *self, void *method) {
    auto orig = reinterpret_cast<Fn2>(load_original(HK_APPROACH_BACK));
    if (on_owner() && !self_action) { Event e{}; e.type = EV_MANUAL_CANCEL; push_ev(e); }
    orig(self, method);
}
void hook_prep_exit(void *self, void *method) {
    auto orig = reinterpret_cast<Fn2>(load_original(HK_PREP_EXIT));
    if (on_owner() && !self_action) { Event e{}; e.type = EV_MANUAL_CANCEL; push_ev(e); }
    orig(self, method);
}
void hook_results_exit(void *self, void *frame, int32_t exit_arg, void *method) {
    auto orig = reinterpret_cast<FnResults>(load_original(HK_RESULTS_EXIT));
    if (!on_owner() || !self || !frame || !g_lit.ready || !policy.armed()) { orig(self,frame,exit_arg,method); return; }
    Root receiver, result;
    try {
        receiver = Root(self); result = Root(frame);
        const bool leave = exit_arg != g_lit.cont && (exit_arg == g_lit.leave_gym || exit_arg == g_lit.leave_battle);
        const bool complete = unbox<uint8_t>(invoke(m_frame_complete, result.get())) != 0;
        const bool fainted = unbox<uint8_t>(invoke(m_frame_fainted, result.get())) != 0;
        const auto st = field<int32_t>(result.get(), "BattleStatus", "Niantic.Holoholo.Battle.BattleStatus");
        Event e{}; e.type=EV_RESULTS; e.victorious_terminal=leave && st==g_lit.victory && complete && !fainted; e.fainted=fainted; push_ev(e);
    } catch (...) { Event e{}; e.type=EV_RESULTS; push_ev(e); }
    orig(receiver ? receiver.get() : self, result ? result.get() : frame, exit_arg, method);
}

// --- Hook installation ------------------------------------------------------------
struct Bind { Slot slot; void *repl; };

void *validate_hook(void *k, const char *name, const char *ret, int pcount,
                    const char *p0, const char *p1, uint64_t rva, const uint8_t *bytes, size_t len, void **code_out) {
    const auto *api = static_cast<const PgrMetaApi *>(pgrazor_runtime_metadata_api(1));
    require(api && api->validate_method, "metadata validate");
    PgrMetaMethodQuery q{}; q.version = 1; q.size = sizeof(q); q.owner = k; q.name = name;
    q.return_type = ret; q.return_match = PGR_MD_MATCH_EXACT;
    q.param_count = pcount; q.param_types[0] = p0; q.param_types[1] = p1;
    q.param_match[0] = q.param_match[1] = PGR_MD_MATCH_EXACT;
    q.expected_code = static_cast<const char *>(boot.module_base) + rva;
    q.prologue = bytes; q.prologue_len = uint32_t(len);
    PgrMetaMethodResult r{};
    if (api->validate_method(&q, &r) != 0) return nullptr;
    if (code_out) *code_out = const_cast<void *>(r.code);
    return const_cast<void *>(r.method);
}

bool install() {
    const auto *boot_api = static_cast<const PgrBootApi *>(pgrazor_runtime_bootstrap_api(1));
    require(boot_api && boot_api->snapshot && boot_api->snapshot(&boot) == 0 && boot.module_base, "runtime bootstrap");

    g_approach_class = cls("Niantic.Holoholo.Gym.Approach", "GymApproachGuiController");
    g_prep_class = cls("Niantic.Holoholo.Gym", "GymBattlePrepUIController");
    g_results_class = cls("Niantic.Holoholo.Gym", "GymBattleState");
    void *frame_class = cls("Niantic.Holoholo.Battle", "BattleFrame");

    static const uint8_t b_ao[] = PGR_GAME_GYM_REPEAT_CPP_APPROACH_OPEN_BYTES;
    static const uint8_t b_ab[] = PGR_GAME_GYM_REPEAT_CPP_APPROACH_BACK_BYTES;
    static const uint8_t b_po[] = PGR_GAME_GYM_REPEAT_CPP_PREP_OPEN_BYTES;
    static const uint8_t b_pb[] = PGR_GAME_GYM_REPEAT_CPP_PREP_BATTLE_BYTES;
    static const uint8_t b_pe[] = PGR_GAME_GYM_REPEAT_CPP_PREP_EXIT_BYTES;
    static const uint8_t b_re[] = PGR_GAME_GYM_REPEAT_CPP_RESULTS_EXIT_BYTES;

    void *code_ao = nullptr, *code_ab = nullptr, *code_po = nullptr, *code_pb = nullptr, *code_pe = nullptr, *code_re = nullptr;
    void *mo = validate_hook(g_approach_class, "HandleOpen", "System.Void", 0, nullptr, nullptr,
                             PGR_GAME_GYM_REPEAT_CPP_APPROACH_OPEN_RVA, b_ao, sizeof(b_ao), &code_ao);
    void *mb = validate_hook(g_approach_class, "OnBackButtonPressed", "System.Void", 0, nullptr, nullptr,
                             PGR_GAME_GYM_REPEAT_CPP_APPROACH_BACK_RVA, b_ab, sizeof(b_ab), &code_ab);
    void *mpo = validate_hook(g_prep_class, "HandleOpen", "System.Void", 0, nullptr, nullptr,
                              PGR_GAME_GYM_REPEAT_CPP_PREP_OPEN_RVA, b_po, sizeof(b_po), &code_po);
    void *mpb = validate_hook(g_prep_class, "Battle", "System.Void", 0, nullptr, nullptr,
                              PGR_GAME_GYM_REPEAT_CPP_PREP_BATTLE_RVA, b_pb, sizeof(b_pb), &code_pb);
    void *mpe = validate_hook(g_prep_class, "ExitPressed", "System.Void", 0, nullptr, nullptr,
                              PGR_GAME_GYM_REPEAT_CPP_PREP_EXIT_RVA, b_pe, sizeof(b_pe), &code_pe);
    // ExitBattleResultsState(BattleFrame, BattleResultsExit) -> the second parameter is
    // the exact BattleResultsExit enum, not a null/opaque pointer.
    void *mre = validate_hook(g_results_class, "ExitBattleResultsState", "System.Void", 2,
                              "Niantic.Holoholo.Battle.BattleFrame", "Niantic.Holoholo.Gym.BattleResultsExit",
                              PGR_GAME_GYM_REPEAT_CPP_RESULTS_EXIT_RVA, b_re, sizeof(b_re), &code_re);

    // Invoke-only (no fingerprint): the loop never passes synthetic team arrays.
    m_click_battle = method_named(g_approach_class, "ClickBattle", 0);
    m_prep_battle = mpb;
    m_frame_complete = method_named(frame_class, "get_BattleComplete", 0);
    m_frame_fainted = method_named(frame_class, "get_AllPlayerPokemonFainted", 0);

    if (!mo || !mb || !mpo || !mpb || !mpe || !mre || !m_click_battle || !m_frame_complete || !m_frame_fainted) return false;

    const Bind binds[HK_COUNT] = {
        {HK_APPROACH_OPEN, reinterpret_cast<void *>(hook_capture_root<HK_APPROACH_OPEN>)},
        {HK_APPROACH_BACK, reinterpret_cast<void *>(hook_approach_back)},
        {HK_PREP_OPEN,     reinterpret_cast<void *>(hook_capture_root<HK_PREP_OPEN>)},
        {HK_PREP_BATTLE,   reinterpret_cast<void *>(hook_prep_battle)},
        {HK_PREP_EXIT,     reinterpret_cast<void *>(hook_prep_exit)},
        {HK_RESULTS_EXIT,  reinterpret_cast<void *>(hook_results_exit)},
    };
    void *codes[HK_COUNT] = {code_ao, code_ab, code_po, code_pb, code_pe, code_re};

    // Reject null/duplicate code addresses before publishing any hook.
    for (int i = 0; i < HK_COUNT; ++i) {
        if (!codes[i]) return false;
        for (int j = 0; j < i; ++j) if (codes[i] == codes[j]) return false;
    }
    for (int i = 0; i < HK_COUNT; ++i) {
        if (g_orig[binds[i].slot].load(std::memory_order_acquire)) continue;
        void *orig = nullptr;
        const int rc = pgrazor_hook(codes[i], binds[i].repl, &orig);
        if (orig) { g_orig[binds[i].slot].store(orig, std::memory_order_release); ++g_installed_hooks; }
        if (rc != 0 || !orig) return false; // partial install: handlers stay inert via g_ready
    }
    return true;
}

// --- Drain accumulated events into the policy on the owner tick --------------------
void drain_events(uint64_t now, const Gate &g) {
    Event e;
    while (pop_ev(e)) {
        switch (e.type) {
            case EV_ARM:
                if (policy.state == ST_IDLE && e.hostile && e.gym && e.account) {
                    policy.arm(e.gym, now, g, timing);
                    if (policy.state == ST_ARMED) {
                        g_gym_fort_id.assign(e.fort);
                        g_account_hash = e.account;
                        PgrRuntimeTransportSample snap{}; transport()->sample(&snap); g_generation = snap.generation;
                        ++g_sessions;
                    }
                }
                break;
            case EV_SCOPE_ENTER:  policy.on_scope_enter(e.gym, now, timing); break;
            case EV_SCOPE_EXIT:   policy.on_scope_exit(now, timing); break;
            case EV_RESULTS:      policy.on_results(e.victorious_terminal, e.fainted); break;
            case EV_MANUAL_CANCEL: policy.on_manual_cancel(); break;
        }
    }
}

bool ui_ready(Root &controller) {
    return controller && alive(controller.get()) && call_bool0(controller.get(), "get_IsOpen") && call_bool0(controller.get(), "get_IsTopGui");
}
void map_status_to_end(GymStatus st);
void poll_reentry() {
    if (policy.state != ST_WON || policy.pending != ACT_NONE) return;
    const bool approach = policy.phase == PH_RETURN || policy.phase == PH_WAIT_APPROACH;
    Root gui(approach ? g_approach_root.get() : policy.phase == PH_WAIT_PREP ? g_prep_root.get() : nullptr);
    if (!gui || !ui_ready(gui)) return; // keep the root while HandleOpen is still opening
    Root rc(field<void *>(gui.get(), approach ? "dzdt" : "dyka", "Niantic.Holoholo.Gym.IGymRootController"));
    require(bool(rc), "gym root missing");
    std::string fort; const GymStatus st = root_gym_status(rc, fort);
    if (fort != g_gym_fort_id) { policy.on_manual_cancel(); return; }
    // A gym that turned neutral/friendly on the re-shown approach (direct repeat) or on
    // the fresh post-tap approach (map fallback) ends the loop successfully right here;
    // we never ClickBattle a cleared gym and never press Back.
    if (st != GYM_HOSTILE) { map_status_to_end(st); return; }
    if (approach) policy.on_approach_open(policy.gym_hash, timing);
    else {
        Root button(field<void *>(gui.get(), "battleButton", "UnityEngine.UI.Button"));
        policy.on_prep_open(policy.gym_hash, button && alive(button.get()) && call_bool0(button.get(), "get_interactable"), timing);
    }
}

// Map a non-hostile pre-action status to the policy's terminal reason.
void map_status_to_end(GymStatus st) {
    switch (st) {
        case GYM_CLEARED:
        case GYM_RAID:
        case GYM_OUT_OF_RANGE:
        case GYM_UNAVAILABLE:  policy.decide_map_action(st); break; // END_DEFEATED/RAID/RANGE/UNAVAILABLE
        default:               policy.on_manual_cancel(); break;
    }
}

// Every action checks cancellation after managed getters, immediately before invoke.
bool action_allowed() {
    if (g_cancel.load() || g_manual_cancel) { policy.on_manual_cancel(); return false; }
    if (!policy.gate_ok(read_gate())) return false;
    PgrRuntimeTransportSample snap{};
    const auto *tr = transport();
    if (!tr || tr->sample(&snap) || snap.generation != g_generation) { policy.on_account_change(); return false; }
    const auto account = player_id_hash();
    if (!account || account != g_account_hash) { policy.on_account_change(); return false; }
    if (g_cancel.load() || g_manual_cancel) { policy.on_manual_cancel(); return false; }
    if (tr->sample(&snap) || snap.generation != g_generation) { policy.on_account_change(); return false; }
    return policy.gate_ok(read_gate());
}
// Explicit permission for off-map healing: only a completed round, the same gym,
// and an open/top approach or prep. Rechecked by healing immediately before item use.
bool gym_heal_ready() {
    try {
        if (policy.state != ST_WON) return false;
        const bool prep = policy.phase == PH_WAIT_PREP;
        if (!prep && policy.phase != PH_RETURN && policy.phase != PH_WAIT_APPROACH) return false;
        if (!action_allowed()) return false;
        Root gui(prep ? g_prep_root.get() : g_approach_root.get());
        if (!ui_ready(gui)) return false;
        Root rc(field<void *>(gui.get(), prep ? "dyka" : "dzdt", "Niantic.Holoholo.Gym.IGymRootController"));
        if (!rc) return false;
        std::string fort; const auto st = root_gym_status(rc, fort);
        if (fort != g_gym_fort_id) { policy.on_manual_cancel(); return false; }
        // Heal the final round too, before reporting gym-defeated.
        if (st != GYM_HOSTILE && st != GYM_CLEARED) { map_status_to_end(st); return false; }
        if (!action_allowed() || !ui_ready(gui)) return false;
        return policy.state == ST_WON && !g_manual_cancel && !g_cancel.load() &&
            gui.get() == (prep ? g_prep_root.get() : g_approach_root.get()) && policy.gate_ok(read_gate());
    } catch (...) { policy.on_action_failed(); return false; }
}
bool settle_gym_healing() {
    if (!gym_heal_ready()) return false;
    const int result = pgr_heal_gym_tick(owner, md, policy.won_at, gym_heal_ready);
    if (result < 0) { policy.on_action_failed(); status("Gym repeat stopped: healing failed"); return false; }
    if (result > 0) { status("Gym repeat: waiting for Smart healing"); return true; }
    return false;
}

struct SelfAction { bool was; SelfAction():was(self_action){self_action=true;} ~SelfAction(){self_action=was;} };
void perform(Action a, uint64_t) {
    try {
        const bool prep = a == ACT_PREP_BATTLE;
        Root gui(prep ? g_prep_root.get() : g_approach_root.get());
        require(ui_ready(gui), "gym UI not ready");
        Root rc(field<void *>(gui.get(), prep ? "dyka" : "dzdt", "Niantic.Holoholo.Gym.IGymRootController"));
        require(bool(rc), "gym root missing");
        std::string fort; const auto st = root_gym_status(rc, fort);
        if (fort != g_gym_fort_id) { policy.on_manual_cancel(); return; }
        if (st != GYM_HOSTILE) { map_status_to_end(st); return; }
        if (prep) {
            Root button(field<void *>(gui.get(), "battleButton", "UnityEngine.UI.Button"));
            require(button && alive(button.get()) && call_bool0(button.get(), "get_interactable"), "battle button disabled");
        }
        if (!action_allowed()) return;
        // A getter can re-enter, close/replace this UI or change gym identity.
        if (read_string(call(rc.get(), "get_FortId")) != g_gym_fort_id) { policy.on_manual_cancel(); return; }
        if (!ui_ready(gui) || g_manual_cancel || g_cancel.load() || !policy.gate_ok(read_gate())) return;
        if (gui.get() != (prep ? g_prep_root.get() : g_approach_root.get())) { policy.on_manual_cancel(); return; }
        void *m = prep ? m_prep_battle : m_click_battle;
        SelfAction tag; invoke(m, gui.get()); ++g_actions;
        status(prep ? "Gym repeat: round started" : "Gym repeat: preparing next round");
    } catch (...) { policy.on_action_failed(); status("Gym repeat stopped: action failed"); }
}
// The map readiness heartbeat can remain true behind the badge award modal.
// Close only this exact post-battle popup, using its own game-owned Dismiss, and
// wait for dismissal to complete before consuming the gym tap. Never share/save.
bool settle_gym_badge(uint64_t now, bool force=false) {
    if (policy.state != ST_WON) return false;
    if (!force && now < g_badge_scan) return bool(g_badge_root);
    g_badge_scan=now+250;
    if (g_badge_root) {
        if (!alive(g_badge_root.get()) || call_bool0(g_badge_root.get(), "get_IsFullyDismissed")) {
            g_badge_root.reset(); g_badge_sent=false;
        }
    }
    if (!g_badge_root) {
        require(h(H_VGP_M_FIND_ALL), "GUI discovery unavailable");
        void *badge_class=cls("Niantic.Holoholo.GymBadges", "GymBadgeAwardGuiController");
        Root type=type_object(badge_class); void *args[]={type.get()};
        Root list(invoke(h(H_VGP_M_FIND_ALL),nullptr,args)); require(bool(list), "badge list missing");
        const size_t n=fn<size_t(*)(void*)>(exp("array_length"))(list.get()); require(n<=32,"badge list bound");
        for(size_t i=0;i<n;++i) {
            Root candidate(*reinterpret_cast<void**>(static_cast<char*>(list.get())+32+i*8));
            if(!candidate || klass(candidate.get())!=badge_class || !alive(candidate.get()) || call_bool0(candidate.get(),"get_IsFullyDismissed"))continue;
            g_badge_root=std::move(candidate); g_badge_sent=false;
            if(policy.phase==PH_WAIT_APPROACH)g_badge_retry=true;
            break;
        }
    }
    if (g_badge_root) {
        if (!g_badge_sent && ui_ready(g_badge_root) && action_allowed()) {
            // Another owner-thread feature may already have begun dismissing it.
            if (call_bool0(g_badge_root.get(),"get_IsOpen") && !g_manual_cancel && !g_cancel.load() && policy.gate_ok(read_gate())) {
                g_badge_sent=true;
                invoke(method_named(cls("Niantic.Holoholo.NUI","GuiController"),"Dismiss",0),g_badge_root.get());
                ++g_actions; status("Gym repeat: dismissing gym badge");
            }
        }
        return true;
    }
    // A badge can appear after OnTapped was consumed. Retry only after observing
    // that exact popup close AND proving the target is still on the map, not in-gym.
    if(g_badge_retry && policy.phase==PH_WAIT_APPROACH && map_visible()) {
        Root target; const auto st=resolve_map_gym(target);
        if(st==GYM_HOSTILE || st==GYM_CLEARED) {
            policy.phase=PH_MAP_DWELL; policy.map_visible=true; policy.map_at=now;
            policy.pending=ACT_NONE; g_badge_retry=false;
        } else if(st!=GYM_UNKNOWN) {map_status_to_end(st);g_badge_retry=false;}
    }
    return false;
}

void do_tap(Root &map_gym) {
    try {
        if(settle_gym_badge(clock_ms(),true))return;
        if (!action_allowed()) return;
        Root fresh; const auto st = resolve_map_gym(fresh);
        if (st != GYM_HOSTILE) { map_status_to_end(st); return; }
        if (!fresh || fresh.get() != map_gym.get()) { policy.on_manual_cancel(); return; }
        if (g_cancel.load() || g_manual_cancel || !policy.gate_ok(read_gate())) return;
        SelfAction tag; invoke(method_named(klass(fresh.get()), "OnTapped", 0), fresh.get()); ++g_actions;
        status("Gym repeat: re-entering gym");
    } catch (...) { policy.on_action_failed(); status("Gym repeat stopped: re-entry failed"); }
}
void cleanup_roots() {
    // HandleOpen fires before IsOpen becomes true. Keep the bounded controller handles
    // through the bounded navigation deadline, including delayed open animations.
    if (policy.state == ST_IDLE || policy.ended() || policy.state == ST_IN_ROUND) {
        g_approach_root.reset(); g_prep_root.reset(); g_badge_root.reset();
        g_badge_sent=g_badge_retry=false;g_badge_scan=0;
    }
}

void report_end() {
    if (!policy.ended()) return;
    g_last_end = policy.end; // preserved for the sample surface; not reset to END_NONE
    std::string m = std::string("Gym repeat ended: ") + end_reason_label(policy.end);
    status(m.c_str());
    g_approach_root.reset();
    g_prep_root.reset();
    g_badge_root.reset();g_badge_sent=g_badge_retry=false;g_badge_scan=0;
    clear_events();
    g_gym_fort_id.clear();
    g_account_hash = 0;
    policy.reset();
}

// Owner-thread session teardown (used by the consumed off-thread stop flag and the
// host's terminal stop). No managed actions beyond releasing GC roots.
void end_session_local(EndReason r) {
    if (policy.armed()) {
        g_last_end = r;
        std::string m = std::string("Gym repeat ended: ") + end_reason_label(r);
        status(m.c_str());
    }
    g_approach_root.reset();
    g_prep_root.reset();
    g_badge_root.reset();g_badge_sent=g_badge_retry=false;g_badge_scan=0;
    clear_events();
    g_gym_fort_id.clear();
    g_account_hash = 0;
    policy.reset();
}

// --- API ---------------------------------------------------------------------------
int api_configure(const PgrGymRepeatConfig *cfg) {
    if (!cfg) return PGR_GR_ERR_INVALID_ARG;
    if (cfg->version != PGR_GYM_REPEAT_API_VERSION) return PGR_GR_ERR_VERSION;
    if (cfg->size != sizeof(PgrGymRepeatConfig)) return PGR_GR_ERR_SIZE;
    if (!cfg->metadata) return PGR_GR_ERR_METADATA;
    if (!cfg->owner_tid || cfg->owner_tid != self_tid()) return PGR_GR_ERR_THREAD;
    if (g_ready.load(std::memory_order_acquire)) return owner == cfg->owner_tid && md == cfg->metadata ? PGR_GR_OK : PGR_GR_ERR_THREAD;
    md = cfg->metadata;
    owner = cfg->owner_tid;
    try {
        if (!install()) { return PGR_GR_ERR_BINDING; }
    } catch (const Failure &f) {
        __android_log_print(ANDROID_LOG_WARN, "PGRazorGymRepeat", "binding unavailable: %s", f.where);
        return PGR_GR_ERR_BINDING;
    } catch (...) { return PGR_GR_ERR_BINDING; }
    g_ready.store(true, std::memory_order_release);
    return PGR_GR_OK;
}

void api_observe_scope(int entered, uint64_t tid) {
    if (!g_ready.load(std::memory_order_acquire) || !owner || tid != owner || self_tid() != owner) return;
    if (entered) {
        Event e{}; e.type = EV_SCOPE_ENTER;
        try {
            Root rc = resolve("Niantic.Holoholo.Gym", "IGymRootController");
            if (rc) { std::string fort = read_string(call(rc.get(), "get_FortId")); e.gym = hash_id(fort); }
        } catch (...) { e.gym = 0; }
        push_ev(e);
    } else {
        Event e{}; e.type = EV_SCOPE_EXIT; push_ev(e);
    }
}
void api_observe_leave(uint64_t tid) {
    if (!g_ready.load(std::memory_order_acquire) || !owner || tid != owner || self_tid() != owner || self_action) return;
    Event e{}; e.type = EV_MANUAL_CANCEL; push_ev(e);
}

int api_tick(uint64_t proven_tid) {
    if (!g_ready.load(std::memory_order_acquire)) return PGR_GR_NOT_READY;
    if (!proven_tid || proven_tid != owner || self_tid() != owner) return PGR_GR_ERR_THREAD;
    if (busy) return PGR_GR_OK;
    struct Guard { Guard() { busy = true; } ~Guard() { busy = false; } } guard;
    if (!boot.runtime.thread_current || fn<void *(*)()>(boot.runtime.thread_current)() == nullptr) return PGR_GR_ERR_THREAD;

    // Consume an off-thread stop request on the owner thread (no cross-thread reset).
    if (g_cancel.exchange(false, std::memory_order_acq_rel)) end_session_local(END_BACKGROUND);
    if (g_faulted.load(std::memory_order_acquire)) return PGR_GR_OK;

    const uint64_t now = clock_ms();
    try {
        const Gate g = read_gate();
        if (g.enabled || policy.armed()) ensure_literals();
        drain_events(now, g);
        bool visible = map_visible();
        if (visible && policy.state == ST_WON && policy.phase == PH_RETURN) {
            Root target; const auto st = resolve_map_gym(target);
            visible = st == GYM_HOSTILE || st == GYM_CLEARED;
            if (st != GYM_UNKNOWN && !visible) map_status_to_end(st);
        }
        policy.observe_map(visible, now);
        if (!policy.armed()) { cleanup_roots(); report_end(); clear_events(); return PGR_GR_OK; }

        // Account binding: a confirmed different player id ends the session; a transient
        // resolution failure (id == 0) is ordinary world state and is left alone.
        const uint64_t pid = player_id_hash();
        if (!pid || pid != g_account_hash) policy.on_account_change();

        PgrRuntimeTransportSample snap{};
        if (transport()->sample(&snap) || snap.generation != g_generation) policy.on_account_change();
        if(policy.state==ST_WON && now>policy.deadline)policy.end_session(END_TIMEOUT);
        if (policy.gate_ok(read_gate()) && settle_gym_badge(now)) {
            cleanup_roots();report_end();return PGR_GR_OK;
        }
        if (policy.gate_ok(read_gate()) && settle_gym_healing()) {
            cleanup_roots(); report_end(); return PGR_GR_OK;
        }
        if (policy.gate_ok(read_gate())) poll_reentry();

        const bool heal_busy = pgr_heal_busy() != 0;
        GymStatus status_in = GYM_UNKNOWN;
        Root map_gym;
        if (policy.needs_gym_resolution(now, g, timing, heal_busy)) {
            status_in = resolve_map_gym(map_gym);
        }
        if (g_manual_cancel || g_cancel.load()) policy.on_manual_cancel();
        // Refresh modal discovery immediately before consuming a direct action. A badge
        // may have opened inside the cached 250 ms scan interval or a readiness getter.
        if (policy.pending != ACT_NONE && policy.gate_ok(read_gate()) && settle_gym_badge(now, true)) {
            cleanup_roots(); report_end(); return PGR_GR_OK;
        }
        Action a = policy.tick(now, read_gate(), timing, heal_busy, status_in);
        if (a == ACT_RESOLVE_GYM) do_tap(map_gym);
        else if (a != ACT_NONE) perform(a, now);
        cleanup_roots();
        report_end();
    } catch (const Failure &f) {
        // Recoverable: end this session and release roots; a new user Battle press can
        // re-arm. The existing combat hooks are untouched.
        g_last_end = END_ERROR;
        __android_log_print(ANDROID_LOG_WARN, "PGRazorGymRepeat", "session ended: %s", f.where);
        g_approach_root.reset(); g_prep_root.reset(); g_badge_root.reset();g_badge_sent=g_badge_retry=false;g_badge_scan=0; clear_events();
        g_gym_fort_id.clear(); g_account_hash = 0;
        policy.reset();
    } catch (...) {
        // Unexpected fault: go inert without disturbing combat.
        g_faulted.store(true, std::memory_order_release);
        g_approach_root.reset(); g_prep_root.reset(); g_badge_root.reset();g_badge_sent=g_badge_retry=false;g_badge_scan=0; clear_events(); policy.reset();
    }
    return PGR_GR_OK;
}

void api_stop(uint64_t proven_tid) {
    if (!g_ready.load(std::memory_order_acquire)) return;
    if (proven_tid && proven_tid == owner && self_tid() == owner &&
        boot.runtime.thread_current && fn<void *(*)()>(boot.runtime.thread_current)() != nullptr) {
        // On the owner thread we can release roots and reset state directly.
        g_cancel.store(false, std::memory_order_release);
        end_session_local(END_BACKGROUND);
    } else {
        // Off the owner thread: never touch roots/policy here; defer to the owner tick.
        g_cancel.store(true, std::memory_order_release);
    }
}

int api_sample(PgrGymRepeatSample *out) {
    if (!out) return PGR_GR_ERR_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));
    out->version = PGR_GYM_REPEAT_API_VERSION;
    out->size = sizeof(PgrGymRepeatSample);
    out->ready = g_ready.load(std::memory_order_acquire) ? 1u : 0u;
    out->installed_hooks = g_installed_hooks;
    out->state = policy.state;
    out->last_end = (policy.end != END_NONE) ? policy.end : g_last_end; // preserved reason
    out->rounds = uint32_t(policy.rounds < 0 ? 0 : policy.rounds);
    uint32_t roots = 0;
    if (g_approach_root.h) ++roots;
    if (g_prep_root.h) ++roots;
    if (g_badge_root.h) ++roots;
    out->roots = roots;
    out->owner_tid = owner;
    out->sessions = g_sessions;
    out->actions = g_actions;
    return PGR_GR_OK;
}

const PgrGymRepeatApi kApi = {
    PGR_GYM_REPEAT_API_VERSION,
    sizeof(PgrGymRepeatApi),
    api_configure,
    api_observe_scope,
    api_observe_leave,
    api_tick,
    api_stop,
    api_sample,
};

} // namespace

const void *pgrazor_gym_repeat_api(uint32_t version) {
    return version == PGR_GYM_REPEAT_API_VERSION ? &kApi : nullptr;
}

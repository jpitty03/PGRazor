// Production dlopen/query66 fixture and standalone test for the compiled Combat
// Orchestration Host (pgrazor/native/combat_host.{h,cpp}).
//
// Exercises Feature 66 (API 1) end-to-end against real leaves (6, 19, 20, 21) and Dobby:
//   * Feature 56 (`PgrSharedMetadataOutput`, 67304B) validation & leaf configuration
//   * Truthful capability reporting (`0x1F`: Rocket Minigame 6 + Rocket Actions 20 +
//     Rocket Combat 21 + Battles 19 + Shared `H_RK_VN_OBSERVE_CODE` site)
//   * Shared Site (`ChargeAttackDisplayVNext.ObserveBattleState`): single Dobby hook
//     fanning out to BOTH Rocket Combat (`PGR_RC_OBS_CHARGE`) and Battles (`PGR_BATTLE_OBS_CHARGE`)
//   * Raid Battle (mode 0): scope enter/exit, vNext gesture, Raid warning dodge cue,
//     Raid fast attack (transport subprocess), and manual swap picker blocking
//   * Gym Battle (mode 1): scope enter/exit, legacy gesture, legacy charge (`Vector3` by value),
//     Gym warning iterator `MoveNext` (`gym_begin`/`gym_end`), legacy dodge/charge/fast,
//     and `"Gym battle"` cooldown-action emission
//   * Max / Dynamax Battle (mode 2): scope-gesture recovery, Max ability widget refresh &
//     `max-attack` press, Max warning (`float` by value) dodge cue, and cleanup
//   * Team GO Rocket: scope enter/exit, legacy CombatUI `Initialize` (`begin_ui`/`end_ui`),
//     hold-warning gate (`battle-not-active` -> `active`), shield prompt & auto-click,
//     legacy quick tap, vNext UI + vNext charged/quick, and Rocket Particle
//     Minigame (Feature 6) 1000x scale collect + automatic restore on scope exit
//   * Scheduler 48 `on_unity_pulse` callback and `revoke()` / `stop(tid)` root cleanup
#include "combat_host.h"
#include "battles.h"
#include "pgrazor_native_api.h"
#include "rocket_combat.h"
#include "rocket_minigame.h"
#include "runtime_transport.h"

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>

namespace {

using namespace pgr_shared_meta;

uint64_t self_tid() {
    return static_cast<uint64_t>(static_cast<pid_t>(gettid()));
}

#define PGR_NOP_PAD \
    __asm__ volatile( \
        "nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\t" \
        "nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\t" \
        : : : "memory")

struct TestVec3 {
    float x;
    float y;
    float z;
};

// --- Original call counters & captured arguments ----------------------------------
std::atomic<int> g_rk_enter_calls{0};
std::atomic<int> g_rk_exit_calls{0};
std::atomic<int> g_rk_ui_init_calls{0};
std::atomic<int> g_rk_ui_update_calls{0};
std::atomic<int> g_rk_ui_close_calls{0};
std::atomic<int> g_rk_vn_update_calls{0};
std::atomic<int> g_rk_vn_close_calls{0};
std::atomic<int> g_rk_prompt_calls{0};
std::atomic<int> g_rk_shield_calls{0};
std::atomic<int> g_rk_shield_close_calls{0};

std::atomic<int> g_shared_vn_observe_calls{0};

std::atomic<int> g_mn_update_calls{0};
std::atomic<int> g_mn_exit_calls{0};
std::atomic<int> g_mn_hide_calls{0};
std::atomic<int> g_mn_destroy_calls{0};

std::atomic<int> g_bt_sc0_enter_calls{0};
std::atomic<int> g_bt_sc0_exit_calls{0};
std::atomic<int> g_bt_sc1_enter_calls{0};
std::atomic<int> g_bt_sc1_exit_calls{0};
std::atomic<int> g_bt_sc2_enter_calls{0};
std::atomic<int> g_bt_sc2_exit_calls{0};

std::atomic<int> g_bt_sw_calls{0};
std::atomic<int> g_bt_gest_calls{0};
std::atomic<int> g_bt_leg_init_calls{0};
std::atomic<int> g_bt_max_widget_calls{0};
std::atomic<int> g_bt_raid_warn_calls{0};
std::atomic<int> g_bt_max_warn_calls{0};
std::atomic<int> g_bt_gym_warn_calls{0};
std::atomic<int> g_bt_leg_fast_calls{0};
std::atomic<int> g_bt_leg_charge_calls{0};

TestVec3 g_last_leg_init_pos{};
float    g_last_max_warn_sec = 0.0f;
bool g_throw_ui=false, g_throw_gym=false;
void *g_capture_args[3]{};

// Forward declarations for gym iterator state update inside target_bt_gym_warn
struct alignas(8) FakeObj {
    unsigned char b[512]{};
};
void set_i32(FakeObj *o, size_t off, int32_t v);

// --- 10 Rocket Combat Hook Targets ------------------------------------------------
extern "C" __attribute__((noinline, aligned(16)))
void target_rk_enter(void *self, void *a1, void *a2, void *info) {
    PGR_NOP_PAD; (void)self; (void)a1; (void)a2; (void)info;
    g_rk_enter_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_rk_exit(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_rk_exit_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_rk_ui_init(void *self, void *data, void *info) {
    PGR_NOP_PAD; (void)self; (void)data; (void)info;
    g_rk_ui_init_calls.fetch_add(1);
    if(g_throw_ui) throw 601;
}
extern "C" __attribute__((noinline, aligned(16)))
void target_rk_ui_update(void *self, uint8_t flag, void *info) {
    PGR_NOP_PAD; (void)self; (void)flag; (void)info;
    g_rk_ui_update_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_rk_ui_close(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_rk_ui_close_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_rk_vn_update(void *self, void *state, void *info) {
    PGR_NOP_PAD; (void)self; (void)state; (void)info;
    g_rk_vn_update_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_rk_vn_close(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_rk_vn_close_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_rk_prompt(void *self, uint8_t show, void *info) {
    PGR_NOP_PAD; (void)self; (void)show; (void)info;
    g_rk_prompt_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_rk_shield(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_rk_shield_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_rk_shield_close(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_rk_shield_close_calls.fetch_add(1);
}

// --- 1 Shared Hook Target (`ChargeAttackDisplayVNext.ObserveBattleState`) ---------
extern "C" __attribute__((noinline, aligned(16)))
void target_shared_vn_observe(void *self, void *state, void *info) {
    PGR_NOP_PAD; (void)self; (void)state; (void)info;
    g_shared_vn_observe_calls.fetch_add(1);
}

// --- 4 Rocket Minigame Hook Targets (hooked by Feature 6 `install()`) -------------
extern "C" __attribute__((noinline, aligned(16)))
void target_mn_update(void *self, int64_t dt, void *info) {
    PGR_NOP_PAD; (void)self; (void)dt; (void)info;
    g_mn_update_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_mn_exit(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_mn_exit_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_mn_hide(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_mn_hide_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_mn_destroy(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_mn_destroy_calls.fetch_add(1);
}

// --- 30 Battle Hook Targets -------------------------------------------------------
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_sc0_enter(void *self, void *a1, void *a2, void *info) {
    PGR_NOP_PAD; (void)self; (void)a1; (void)a2; (void)info;
    g_capture_args[0]=self;g_capture_args[1]=a1;g_capture_args[2]=a2;
    g_bt_sc0_enter_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_sc0_exit(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_bt_sc0_exit_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_sc1_enter(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_bt_sc1_enter_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_sc1_exit(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_bt_sc1_exit_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_sc2_enter(void *self, void *a1, void *a2, void *info) {
    PGR_NOP_PAD; (void)self; (void)a1; (void)a2; (void)info;
    g_bt_sc2_enter_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_sc2_exit(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_bt_sc2_exit_calls.fetch_add(1);
}

#define DEFINE_SW_TARGETS(IDX) \
    extern "C" __attribute__((noinline, aligned(16))) \
    void target_bt_sw##IDX##_open(void *self, void *info) { \
        PGR_NOP_PAD; (void)self; (void)info; g_bt_sw_calls.fetch_add(1); \
    } \
    extern "C" __attribute__((noinline, aligned(16))) \
    void target_bt_sw##IDX##_cl0(void *self, void *info) { \
        PGR_NOP_PAD; (void)self; (void)info; g_bt_sw_calls.fetch_add(1); \
    } \
    extern "C" __attribute__((noinline, aligned(16))) \
    void target_bt_sw##IDX##_cl1(void *self, void *info) { \
        PGR_NOP_PAD; (void)self; (void)info; g_bt_sw_calls.fetch_add(1); \
    }

DEFINE_SW_TARGETS(0)
DEFINE_SW_TARGETS(1)
DEFINE_SW_TARGETS(2)

extern "C" __attribute__((noinline, aligned(16)))
void target_bt_enable(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info; g_bt_gest_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_buttons(void *self, void *a1, void *info) {
    PGR_NOP_PAD; (void)self; (void)a1; (void)info; g_bt_gest_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_leg_enable(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info; g_bt_gest_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_leg_buttons(void *self, void *a1, void *info) {
    PGR_NOP_PAD; (void)self; (void)a1; (void)info; g_bt_gest_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_disable(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info; g_bt_gest_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_leg_disable(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info; g_bt_gest_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_destroy(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info; g_bt_gest_calls.fetch_add(1);
}

extern "C" __attribute__((noinline, aligned(16)))
void target_bt_leg_init(void *self, void *p1, void *p2, TestVec3 pos, void *info) {
    PGR_NOP_PAD; (void)self; (void)p1; (void)p2; (void)info;
    g_last_leg_init_pos = pos;
    g_bt_leg_init_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_max_refresh(void *self, void *data, void *info) {
    PGR_NOP_PAD; (void)self; (void)data; (void)info;
    g_bt_max_widget_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_max_cleanup(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_bt_max_widget_calls.fetch_add(1);
}

extern "C" __attribute__((noinline, aligned(16)))
void target_bt_raid_warn(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_bt_raid_warn_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_max_warn(void *self, float sec, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_last_max_warn_sec = sec;
    g_bt_max_warn_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
uint8_t target_bt_gym_warn(void *self, void *info) {
    PGR_NOP_PAD; (void)info;
    g_bt_gym_warn_calls.fetch_add(1);
    if(g_throw_gym) throw 602;
    // Complete the eligible warning iterator, returning false (0).
    if (self) set_i32(static_cast<FakeObj *>(self), 0x10, 1);
    return 0;
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_leg_fast(void *self, void *sender, void *args, void *info) {
    PGR_NOP_PAD; (void)self; (void)sender; (void)args; (void)info;
    g_bt_leg_fast_calls.fetch_add(1);
}
extern "C" __attribute__((noinline, aligned(16)))
void target_bt_leg_charge(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
    g_bt_leg_charge_calls.fetch_add(1);
}

// Dummy code pointers for non-hooked methods whose `_CODE` handles exist in Feature 56
extern "C" __attribute__((noinline, aligned(16)))
void dummy_code_target(void *self, void *info) {
    PGR_NOP_PAD; (void)self; (void)info;
}

// --- Synthetic IL2CPP Runtime & Managed Objects -----------------------------------
void set_ptr(FakeObj *o, size_t off, const void *p) {
    std::memcpy(o->b + off, &p, sizeof(p));
}
void *get_ptr(const void *o, size_t off) {
    void *p = nullptr;
    std::memcpy(&p, static_cast<const unsigned char *>(o) + off, sizeof(p));
    return p;
}
void set_i32(FakeObj *o, size_t off, int32_t v) {
    std::memcpy(o->b + off, &v, sizeof(v));
}
void set_u8(FakeObj *o, size_t off, uint8_t v) {
    o->b[off] = v;
}

struct FakeField {
    size_t offset = 0;
    size_t size   = 8;
};

// Volatile calls prevent compiler argument-elision at fixture targets patched by Dobby.
decltype(&target_bt_enable) volatile call_target_bt_enable=&target_bt_enable;
decltype(&target_bt_gym_warn) volatile call_target_bt_gym_warn=&target_bt_gym_warn;
decltype(&target_bt_leg_charge) volatile call_target_bt_leg_charge=&target_bt_leg_charge;
decltype(&target_bt_leg_enable) volatile call_target_bt_leg_enable=&target_bt_leg_enable;
decltype(&target_bt_leg_fast) volatile call_target_bt_leg_fast=&target_bt_leg_fast;
decltype(&target_bt_leg_init) volatile call_target_bt_leg_init=&target_bt_leg_init;
decltype(&target_bt_max_cleanup) volatile call_target_bt_max_cleanup=&target_bt_max_cleanup;
decltype(&target_bt_max_refresh) volatile call_target_bt_max_refresh=&target_bt_max_refresh;
decltype(&target_bt_max_warn) volatile call_target_bt_max_warn=&target_bt_max_warn;
decltype(&target_bt_raid_warn) volatile call_target_bt_raid_warn=&target_bt_raid_warn;
decltype(&target_bt_sc0_enter) volatile call_target_bt_sc0_enter=&target_bt_sc0_enter;
decltype(&target_bt_sc0_exit) volatile call_target_bt_sc0_exit=&target_bt_sc0_exit;
decltype(&target_bt_sc1_enter) volatile call_target_bt_sc1_enter=&target_bt_sc1_enter;
decltype(&target_bt_sc1_exit) volatile call_target_bt_sc1_exit=&target_bt_sc1_exit;
decltype(&target_bt_sc2_enter) volatile call_target_bt_sc2_enter=&target_bt_sc2_enter;
decltype(&target_bt_sc2_exit) volatile call_target_bt_sc2_exit=&target_bt_sc2_exit;
decltype(&target_bt_sw1_cl0) volatile call_target_bt_sw1_cl0=&target_bt_sw1_cl0;
decltype(&target_bt_sw1_open) volatile call_target_bt_sw1_open=&target_bt_sw1_open;
decltype(&target_mn_update) volatile call_target_mn_update=&target_mn_update;
decltype(&target_rk_enter) volatile call_target_rk_enter=&target_rk_enter;
decltype(&target_rk_exit) volatile call_target_rk_exit=&target_rk_exit;
decltype(&target_rk_prompt) volatile call_target_rk_prompt=&target_rk_prompt;
decltype(&target_rk_shield) volatile call_target_rk_shield=&target_rk_shield;
decltype(&target_rk_ui_init) volatile call_target_rk_ui_init=&target_rk_ui_init;
decltype(&target_rk_ui_update) volatile call_target_rk_ui_update=&target_rk_ui_update;
decltype(&target_rk_ui_close) volatile call_target_rk_ui_close=&target_rk_ui_close;
decltype(&target_rk_vn_update) volatile call_target_rk_vn_update=&target_rk_vn_update;
decltype(&target_rk_vn_close) volatile call_target_rk_vn_close=&target_rk_vn_close;
decltype(&target_shared_vn_observe) volatile call_target_shared_vn_observe=&target_shared_vn_observe;
int g_thread_sentinel = 1;
void *fake_thread_current() { return &g_thread_sentinel; }

std::vector<void *> g_gc_slots;
std::vector<int> g_gc_pinned;
void *g_gc_protected[3]{};
bool g_sweep=false;
int g_sweeps=0;
FakeObj g_move_from{},g_move_to{};
int g_live_gc_handles = 0;

uintptr_t fake_gc_new(void *obj, int pinned) {
    if (!obj) return 0;
    if(g_sweep&&!pinned){
        ++g_sweeps;
        for(void *expected:g_gc_protected){bool found=false;
            for(size_t i=0;i<g_gc_slots.size();++i)if(g_gc_slots[i]==expected&&g_gc_pinned[i])found=true;
            assert(found); // every original reference protected before observer GC
        }
        for(size_t i=0;i<g_gc_slots.size();++i)if(g_gc_slots[i]==&g_move_from&&!g_gc_pinned[i])g_gc_slots[i]=&g_move_to;
    }
    for (size_t i = 0; i < g_gc_slots.size(); ++i) {
        if (!g_gc_slots[i]) {
            g_gc_slots[i] = obj;
            g_gc_pinned[i] = pinned;
            ++g_live_gc_handles;
            return static_cast<uintptr_t>(i + 1);
        }
    }
    g_gc_slots.push_back(obj);
    g_gc_pinned.push_back(pinned);
    ++g_live_gc_handles;
    return static_cast<uintptr_t>(g_gc_slots.size());
}

void *fake_gc_get(uintptr_t h) {
    if (h == 0 || h > g_gc_slots.size()) return nullptr;
    return g_gc_slots[h - 1];
}

void fake_gc_free(uintptr_t h) {
    if (h == 0 || h > g_gc_slots.size()) return;
    if (g_gc_slots[h - 1]) {
        g_gc_slots[h - 1] = nullptr;
        --g_live_gc_handles;
    }
}

void *fake_object_get_class(void *o) {
    return o ? get_ptr(o, 0) : nullptr;
}

void *fake_object_unbox(void *o) {
    return o ? (static_cast<unsigned char *>(o) + 8) : nullptr;
}

void fake_field_get_value(void *o, const void *f, void *out) {
    const auto *ff = static_cast<const FakeField *>(f);
    if (!o || !ff || !out) return;
    std::memcpy(out, static_cast<const unsigned char *>(o) + ff->offset, ff->size);
}

// Tokens & objects
int g_core_classes[16]{};
int g_opt_classes[128]{};
int g_extra_classes[16]{};
int g_methods[128]{};
FakeField g_fields[64]{};

FakeObj g_boxed_bool_true{};
FakeObj g_boxed_bool_false{};
FakeObj g_boxed_vec3{};

// Battle objects
FakeObj g_raid_scope{};
FakeObj g_gym_scope{};
FakeObj g_max_scope{};
FakeObj g_battle_director{};
FakeObj g_battle_state{};
FakeObj g_raid_gesture{};
FakeObj g_gym_gesture{};
FakeObj g_max_gesture{};
FakeObj g_tap_obj{};
FakeObj g_flick_obj{};
FakeObj g_vn_charge{};
FakeObj g_leg_charge{};
FakeObj g_charge_btn{};
FakeObj g_max_widget{};
FakeObj g_max_btn{};
FakeObj g_raid_warn{};
FakeObj g_max_warn{};
FakeObj g_gym_iter{};
FakeObj g_raid_swap{};

// Rocket objects
FakeObj g_rk_state{};
FakeObj g_rk_ui{};
FakeObj g_rk_director{};
FakeObj g_rk_charge1{};
FakeObj g_rk_shield{};
FakeObj g_rk_shield_btn{};
FakeObj g_rk_vui{};
FakeObj g_rk_vgest{};
FakeObj g_rk_fast_proto{};

// Minigame objects
FakeObj g_mini_challenge{};
FakeObj g_mini_collider{};
FakeObj g_mini_transform{};
FakeObj g_mini_go{};

// Controllable test flags for `fake_runtime_invoke`
bool g_vn_charged_ready  = false;
bool g_vn_visible_ready  = true;
bool g_leg_charged_ready = false;
bool g_swap_showing      = false;
bool g_rk_dir_active     = false;
bool g_rk_vnext_blocked  = false;

float   g_mini_scale[3] = {2.0f, 2.0f, 2.0f};
uint8_t g_mini_go_active = 0;

int g_raid_dodge_invokes  = 0;
int g_leg_dodge_invokes   = 0;
int g_raid_fast_invokes   = 0;
int g_max_fast_invokes    = 0;
int g_leg_fast_invokes    = 0;
int g_vn_charge_invokes   = 0;
int g_leg_charge_invokes  = 0;
int g_max_press_invokes   = 0;
int g_rk_fast_invokes     = 0;
int g_rk_charge_invokes   = 0;
int g_rk_shield_invokes   = 0;
int g_rk_vn_fast_invokes  = 0;

enum MethodTokenIdx : int {
    M_ALIVE = 1,
    M_BT_CUR_STATE,
    M_BT_ACTIVE,
    M_BT_PRESS,
    M_BT_DODGE,
    M_BT_LEG_DODGE,
    M_BT_RAID_FAST_ACT,
    M_BT_MAX_FAST,
    M_BT_LEG_FAST,
    M_BT_LEG_CHARGE,
    M_BT_LEG_CHARGED,
    M_BT_SW0_SHOW,
    M_BT_SW1_SHOW,
    M_BT_SW2_SHOW,
    M_RK_DIRECTOR,
    M_RK_ACTIVE,
    M_RK_FAST,
    M_RK_CHARGE,
    M_RK_VISIBLE,
    M_RK_INTERACTABLE,
    M_RK_SHIELD,
    M_RK_VN_BLOCKED,
    M_RK_VN_CHARGED,
    M_RK_VN_VISIBLE,
    M_RK_VN_CHARGE,
    M_RK_VN_FAST,
    M_MN_TRANSFORM,
    M_MN_GAMEOBJ,
    M_MN_SCALE,
    M_MN_SETSCALE,
    M_MN_ACTIVE,
    M_MN_SETACTIVE
};

void *box_bool(bool v) {
    FakeObj *b = v ? &g_boxed_bool_true : &g_boxed_bool_false;
    set_ptr(b, 0, &g_core_classes[6]);
    b->b[8] = v ? 1 : 0;
    return b;
}

int g_execute_token=0,g_execute_mode=0;
__attribute__((noinline,aligned(16))) void *fake_runtime_invoke(const void *method, void *obj, void **params, void **exc) {
    PGR_NOP_PAD;
    if (exc) *exc = nullptr;
    if(method==&g_execute_token){
        if(g_execute_mode==1){call_target_bt_sc0_enter(&g_raid_scope,nullptr,nullptr,nullptr);call_target_bt_enable(&g_raid_gesture,nullptr);}
        if(g_execute_mode==2)call_target_bt_sc0_exit(&g_raid_scope,nullptr);
        if(g_execute_mode==3){call_target_rk_enter(&g_rk_state,nullptr,nullptr,nullptr);call_target_rk_ui_init(&g_rk_ui,nullptr,nullptr);}
        if(g_execute_mode==4)call_target_rk_exit(&g_rk_state,nullptr);
        return nullptr;
    }
    if (method == &g_methods[M_ALIVE] || method == &g_methods[M_BT_ACTIVE] ||
        method == &g_methods[M_RK_INTERACTABLE] || method == &g_methods[M_RK_VISIBLE]) {
        return box_bool(true);
    }
    if (method == &g_methods[M_BT_CUR_STATE]) {
        return &g_battle_state;
    }
    if (method == &g_methods[M_BT_SW0_SHOW] || method == &g_methods[M_BT_SW1_SHOW] ||
        method == &g_methods[M_BT_SW2_SHOW]) {
        return box_bool(g_swap_showing);
    }
    if (method == &g_methods[M_RK_VN_CHARGED]) {
        return box_bool(g_vn_charged_ready);
    }
    if (method == &g_methods[M_RK_VN_VISIBLE]) {
        return box_bool(g_vn_visible_ready);
    }
    if (method == &g_methods[M_BT_LEG_CHARGED]) {
        return box_bool(g_leg_charged_ready);
    }
    if (method == &g_methods[M_BT_DODGE]) {
        ++g_raid_dodge_invokes;
        return nullptr;
    }
    if (method == &g_methods[M_BT_LEG_DODGE]) {
        ++g_leg_dodge_invokes;
        return nullptr;
    }
    if (method == &g_methods[M_BT_RAID_FAST_ACT]) {
        ++g_raid_fast_invokes;
        return nullptr;
    }
    if (method == &g_methods[M_BT_MAX_FAST]) {
        ++g_max_fast_invokes;
        return nullptr;
    }
    if (method == &g_methods[M_BT_LEG_FAST]) {
        ++g_leg_fast_invokes;
        // Synchronous invoke of legacyFast triggers the Dobby hook on H_BT_M_LEG_FAST_CODE!
        call_target_bt_leg_fast(obj, params ? params[0] : nullptr, params ? params[1] : nullptr, nullptr);
        return nullptr;
    }
    if (method == &g_methods[M_BT_LEG_CHARGE]) {
        ++g_leg_charge_invokes;
        // Synchronous invoke of legacyCharge triggers the Dobby hook on H_BT_M_LEG_CHARGE_CODE!
        call_target_bt_leg_charge(obj, nullptr);
        return nullptr;
    }
    if (method == &g_methods[M_RK_VN_CHARGE]) {
        ++g_vn_charge_invokes;
        return nullptr;
    }
    if (method == &g_methods[M_BT_PRESS]) {
        ++g_max_press_invokes;
        return nullptr;
    }
    if (method == &g_methods[M_RK_DIRECTOR]) {
        return &g_rk_director;
    }
    if (method == &g_methods[M_RK_ACTIVE]) {
        return box_bool(g_rk_dir_active);
    }
    if (method == &g_methods[M_RK_FAST]) {
        ++g_rk_fast_invokes;
        return &g_rk_fast_proto;
    }
    if (method == &g_methods[M_RK_CHARGE]) {
        ++g_rk_charge_invokes;
        return nullptr;
    }
    if (method == &g_methods[M_RK_SHIELD]) {
        ++g_rk_shield_invokes;
        call_target_rk_shield(obj, nullptr);
        return nullptr;
    }
    if (method == &g_methods[M_RK_VN_BLOCKED]) {
        return box_bool(g_rk_vnext_blocked);
    }
    if (method == &g_methods[M_RK_VN_FAST]) {
        assert(obj==&g_rk_vgest&&params&&params[0]==&g_tap_obj);
        ++g_rk_vn_fast_invokes;
        return nullptr;
    }
    if (method == &g_methods[M_MN_TRANSFORM]) {
        return &g_mini_transform;
    }
    if (method == &g_methods[M_MN_GAMEOBJ]) {
        return &g_mini_go;
    }
    if (method == &g_methods[M_MN_SCALE]) {
        set_ptr(&g_boxed_vec3, 0, &g_extra_classes[9]);
        std::memcpy(g_boxed_vec3.b + 8, g_mini_scale, sizeof(g_mini_scale));
        return &g_boxed_vec3;
    }
    if (method == &g_methods[M_MN_SETSCALE]) {
        if (params && params[0]) {
            std::memcpy(g_mini_scale, params[0], sizeof(g_mini_scale));
        }
        return nullptr;
    }
    if (method == &g_methods[M_MN_ACTIVE]) {
        return box_bool(g_mini_go_active != 0);
    }
    if (method == &g_methods[M_MN_SETACTIVE]) {
        if (params && params[0]) {
            g_mini_go_active = *static_cast<uint8_t *>(params[0]);
        }
        return nullptr;
    }
    return nullptr;
}

__attribute__((noinline,aligned(16))) void *fake_convert(const void *m,void *o,void **p,int32_t,void **e){PGR_NOP_PAD;return fake_runtime_invoke(m,o,p,e);}
decltype(&fake_runtime_invoke) volatile call_invoke=&fake_runtime_invoke;
int verify_scheduler(const PgrUnitySchedulerConfig *c){return c->execute_tasks==&g_execute_token;}
void execute(){void *exc=nullptr;assert(!call_invoke(&g_execute_token,nullptr,nullptr,&exc));assert(!exc);}
const PgrRuntimeTransportApi *g_transport=nullptr;
uint64_t g_snapshot_sequence=0;
void settings(bool battle,bool rocket,bool adjacent){
    uint8_t frame[PGR_TRANSPORT_SNAPSHOT_FIXED + 4 * PGR_TRANSPORT_STRINGS]{};auto u32=[&](size_t o,uint32_t v){memcpy(frame+o,&v,4);};
    u32(0,PGR_TRANSPORT_SNAPSHOT_MAGIC);u32(4,PGR_TRANSPORT_FRAME_VERSION);u32(8,sizeof(frame));
    ++g_snapshot_sequence;memcpy(frame+16,&g_snapshot_sequence,8);
    frame[32+3]=1;frame[32+6]=battle;frame[32+26]=rocket;frame[32+7]=adjacent;frame[32+27]=adjacent;
    u32(PGR_TRANSPORT_OFF_INTS,1023);u32(PGR_TRANSPORT_OFF_INTS+6*4,7);uint8_t out[65536];uint32_t used=0;
    assert(g_transport->exchange(frame,sizeof(frame),out,sizeof(out),&used)==0);
}
struct DrainedEvent {
    uint32_t kind;
    int64_t  code0;
    int64_t  code1;
    std::string json;
};

std::vector<DrainedEvent> drain_all(const PgrCombatHostApi *host) {
    std::vector<DrainedEvent> out;
    PgrCombatHostEvent ev{};
    char buf[4096]{};
    while (host->next_event(&ev, buf, sizeof(buf)) == 1) {
        out.push_back(DrainedEvent{ev.kind, ev.code0, ev.code1, std::string(buf, ev.json_len)});
    }
    return out;
}

bool has_event(const std::vector<DrainedEvent> &evs, uint32_t kind, const char *substr = nullptr) {
    for (const auto &e : evs) {
        if (e.kind == kind) {
            if (!substr || e.json.find(substr) != std::string::npos) return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char **argv) {
    assert(argc==2||argc==3);bool transport_mode=argc==3;
    void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib)puts(dlerror());assert(lib);
    auto entry=reinterpret_cast<const void*(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(entry);
    const auto *core=static_cast<const pgrazor_native_api_v2*>(entry(2));assert(core);
    const auto *host=static_cast<const PgrCombatHostApi*>(core->query_feature(66,1));
    assert(host != nullptr);
    assert(host->version == PGR_COMBAT_HOST_API_VERSION);
    assert(host->size == sizeof(PgrCombatHostApi));

    const uint64_t tid = self_tid();
    if(transport_mode){
        g_transport=static_cast<const PgrRuntimeTransportApi*>(core->query_feature(52,1));assert(g_transport);
        PgrRuntimeTransportConfig tc{1,sizeof(tc),2500,2048,65536,1048576,0,0};assert(g_transport->install(&tc)==0);settings(true,true,false);
    }

    // --- 1. Populate Synthetic Feature 56 (`PgrSharedMetadataOutput`) --------------
    static PgrSharedMetadataOutput md{};
    std::memset(&md, 0, sizeof(md));
    md.version        = 2u;
    md.size           = sizeof(PgrSharedMetadataOutput);
    md.success        = 1;
    md.caught_id_size = 8;

    for (size_t i = 0; i < kClassCount; ++i)    md.classes[i]     = &g_core_classes[i];
    for (size_t i = 0; i < kOptClassCount; ++i) md.opt_classes[i] = &g_opt_classes[i];

    // Core field[1] = GUI.currentState (@0x70, size 4), core method[5] = Object.op_Implicit
    g_fields[0]  = FakeField{0x70, 4}; // GUI.currentState
    md.fields[1] = &g_fields[0];
    md.methods[5] = &g_methods[M_ALIVE];

    md.optional[OPT_ROCKET]  = PGR_SHARED_OPT_AVAILABLE;
    md.optional[OPT_BATTLES] = PGR_SHARED_OPT_AVAILABLE;

    md.opt_ints[I_BAT_STATUS_ACTIVE] = 2;
    md.opt_ints[I_BAT_STATUS_UNSET]  = 0;
    md.opt_ints[I_BAT_DODGE_LEFT]    = 1;
    md.opt_ints[I_BAT_MAX_ATTACK]    = 1;

    // Field definitions (exact byte sizes prevent stack over-writes on 1B/4B reads)
    enum FieldSlot : int {
        F_RK_VN_GESTURE = 1,
        F_RK_VN_ENABLED,
        F_RK_VN_TAP,
        F_RK_VN_BUTTON,
        F_RK_MN_COLLIDER,
        F_RK_MN_RUNNING,
        F_RK_FIRST,
        F_RK_SECOND,
        F_RK_CHARGED,
        F_RK_INPUT,
        F_RK_SHIELDS,
        F_RK_BUTTON,
        F_BT_GYM_WARN_STATE,
        F_BT_MAX_GESTURE,
        F_BT_DIRECTOR,
        F_BT_BATTLE_STATUS,
        F_BT_LEG_FLICK,
        F_BT_LEG_TAP,
        F_BT_LEG_BUTTON,
        F_BT_MAX_BUTTON,
        F_BT_MAX_TYPE
    };
    g_fields[F_RK_VN_GESTURE]     = FakeField{0x20, 8};
    g_fields[F_RK_VN_ENABLED]     = FakeField{0x28, 1};
    g_fields[F_RK_VN_TAP]         = FakeField{0x30, 8};
    g_fields[F_RK_VN_BUTTON]      = FakeField{0x38, 8};
    g_fields[F_RK_MN_COLLIDER]    = FakeField{0x40, 8};
    g_fields[F_RK_MN_RUNNING]     = FakeField{0x48, 1};
    g_fields[F_RK_FIRST]          = FakeField{0x50, 8};
    g_fields[F_RK_SECOND]         = FakeField{0x58, 8};
    g_fields[F_RK_CHARGED]        = FakeField{0x60, 1};
    g_fields[F_RK_INPUT]          = FakeField{0x61, 1};
    g_fields[F_RK_SHIELDS]        = FakeField{0x64, 4};
    g_fields[F_RK_BUTTON]         = FakeField{0x68, 8};
    g_fields[F_BT_GYM_WARN_STATE] = FakeField{0x10, 4};
    g_fields[F_BT_MAX_GESTURE]    = FakeField{0x78, 8};
    g_fields[F_BT_DIRECTOR]       = FakeField{0x80, 8};
    g_fields[F_BT_BATTLE_STATUS]  = FakeField{0x88, 4};
    g_fields[F_BT_LEG_FLICK]      = FakeField{0x90, 8};
    g_fields[F_BT_LEG_TAP]        = FakeField{0x98, 8};
    g_fields[F_BT_LEG_BUTTON]     = FakeField{0xA0, 8};
    g_fields[F_BT_MAX_BUTTON]     = FakeField{0xA8, 8};
    g_fields[F_BT_MAX_TYPE]       = FakeField{0xB0, 4};

    // Rocket handles
    md.opt_handles[H_RK_VN_GESTURE]      = &g_fields[F_RK_VN_GESTURE];
    md.opt_handles[H_RK_VN_ENABLED]      = &g_fields[F_RK_VN_ENABLED];
    md.opt_handles[H_RK_VN_TAP]          = &g_fields[F_RK_VN_TAP];
    md.opt_handles[H_RK_VN_BUTTON]       = &g_fields[F_RK_VN_BUTTON];
    md.opt_handles[H_RK_VN_UPDATE_M]     = &g_methods[60];
    md.opt_handles[H_RK_VN_UPDATE_CODE]  = reinterpret_cast<const void *>(target_rk_vn_update);
    md.opt_handles[H_RK_VN_CLOSE_M]      = &g_methods[61];
    md.opt_handles[H_RK_VN_CLOSE_CODE]   = reinterpret_cast<const void *>(target_rk_vn_close);
    md.opt_handles[H_RK_VN_BLOCKED_M]    = &g_methods[M_RK_VN_BLOCKED];
    md.opt_handles[H_RK_VN_BLOCKED_CODE] = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_RK_VN_FAST_M]       = &g_methods[M_RK_VN_FAST];
    md.opt_handles[H_RK_VN_FAST_CODE]    = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_RK_VN_OBSERVE_M]    = &g_methods[62];
    md.opt_handles[H_RK_VN_OBSERVE_CODE] = reinterpret_cast<const void *>(target_shared_vn_observe);
    md.opt_handles[H_RK_VN_CHARGE_M]     = &g_methods[M_RK_VN_CHARGE];
    md.opt_handles[H_RK_VN_CHARGE_CODE]  = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_RK_VN_CHARGED_M]    = &g_methods[M_RK_VN_CHARGED];
    md.opt_handles[H_RK_VN_CHARGED_CODE] = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_RK_VN_VISIBLE_M]    = &g_methods[M_RK_VN_VISIBLE];
    md.opt_handles[H_RK_VN_VISIBLE_CODE] = reinterpret_cast<const void *>(dummy_code_target);

    // Minigame handles
    md.opt_handles[H_RK_MN_COLLIDER]    = &g_fields[F_RK_MN_COLLIDER];
    md.opt_handles[H_RK_MN_RUNNING]     = &g_fields[F_RK_MN_RUNNING];
    md.opt_handles[H_RK_MN_UPDATE_M]    = &g_methods[63];
    md.opt_handles[H_RK_MN_UPDATE_CODE] = reinterpret_cast<const void *>(target_mn_update);
    md.opt_handles[H_RK_MN_EXIT_M]      = &g_methods[64];
    md.opt_handles[H_RK_MN_EXIT_CODE]   = reinterpret_cast<const void *>(target_mn_exit);
    md.opt_handles[H_RK_MN_HIDE_M]      = &g_methods[65];
    md.opt_handles[H_RK_MN_HIDE_CODE]   = reinterpret_cast<const void *>(target_mn_hide);
    md.opt_handles[H_RK_MN_DESTROY_M]   = &g_methods[66];
    md.opt_handles[H_RK_MN_DESTROY_CODE]= reinterpret_cast<const void *>(target_mn_destroy);
    md.opt_handles[H_RK_MN_TRANSFORM_M] = &g_methods[M_MN_TRANSFORM];
    md.opt_handles[H_RK_MN_GAMEOBJ_M]   = &g_methods[M_MN_GAMEOBJ];
    md.opt_handles[H_RK_MN_SCALE_M]     = &g_methods[M_MN_SCALE];
    md.opt_handles[H_RK_MN_SETSCALE_M]  = &g_methods[M_MN_SETSCALE];
    md.opt_handles[H_RK_MN_ACTIVE_M]    = &g_methods[M_MN_ACTIVE];
    md.opt_handles[H_RK_MN_SETACTIVE_M] = &g_methods[M_MN_SETACTIVE];

    // Legacy Rocket handles
    md.opt_handles[H_RK_FIRST]             = &g_fields[F_RK_FIRST];
    md.opt_handles[H_RK_SECOND]            = &g_fields[F_RK_SECOND];
    md.opt_handles[H_RK_CHARGED]           = &g_fields[F_RK_CHARGED];
    md.opt_handles[H_RK_INPUT]             = &g_fields[F_RK_INPUT];
    md.opt_handles[H_RK_SHIELDS]           = &g_fields[F_RK_SHIELDS];
    md.opt_handles[H_RK_BUTTON]            = &g_fields[F_RK_BUTTON];
    md.opt_handles[H_RK_ENTER_M]           = &g_methods[67];
    md.opt_handles[H_RK_ENTER_CODE]        = reinterpret_cast<const void *>(target_rk_enter);
    md.opt_handles[H_RK_EXIT_M]            = &g_methods[68];
    md.opt_handles[H_RK_EXIT_CODE]         = reinterpret_cast<const void *>(target_rk_exit);
    md.opt_handles[H_RK_UPDATE_M]          = &g_methods[69];
    md.opt_handles[H_RK_UPDATE_CODE]       = reinterpret_cast<const void *>(target_rk_ui_update);
    md.opt_handles[H_RK_UI_M]              = &g_methods[70];
    md.opt_handles[H_RK_UI_CODE]           = reinterpret_cast<const void *>(target_rk_ui_init);
    md.opt_handles[H_RK_CLOSE_M]           = &g_methods[71];
    md.opt_handles[H_RK_CLOSE_CODE]        = reinterpret_cast<const void *>(target_rk_ui_close);
    md.opt_handles[H_RK_DIRECTOR_M]        = &g_methods[M_RK_DIRECTOR];
    md.opt_handles[H_RK_DIRECTOR_CODE]     = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_RK_ACTIVE_M]          = &g_methods[M_RK_ACTIVE];
    md.opt_handles[H_RK_ACTIVE_CODE]       = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_RK_FAST_M]            = &g_methods[M_RK_FAST];
    md.opt_handles[H_RK_FAST_CODE]         = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_RK_CHARGE_M]          = &g_methods[M_RK_CHARGE];
    md.opt_handles[H_RK_CHARGE_CODE]       = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_RK_VISIBLE_M]         = &g_methods[M_RK_VISIBLE];
    md.opt_handles[H_RK_VISIBLE_CODE]      = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_RK_PROMPT_M]          = &g_methods[72];
    md.opt_handles[H_RK_PROMPT_CODE]       = reinterpret_cast<const void *>(target_rk_prompt);
    md.opt_handles[H_RK_SHIELD_M]          = &g_methods[M_RK_SHIELD];
    md.opt_handles[H_RK_SHIELD_CODE]       = reinterpret_cast<const void *>(target_rk_shield);
    md.opt_handles[H_RK_SHIELD_CLOSE_M]    = &g_methods[73];
    md.opt_handles[H_RK_SHIELD_CLOSE_CODE] = reinterpret_cast<const void *>(target_rk_shield_close);
    md.opt_handles[H_RK_INTERACTABLE_M]    = &g_methods[M_RK_INTERACTABLE];

    // Battle scope & swap handles
    md.opt_handles[H_BT_SC0_CLASS]      = &g_extra_classes[0];
    md.opt_handles[H_BT_SC0_ENTER_M]    = &g_methods[74];
    md.opt_handles[H_BT_SC0_ENTER_CODE] = reinterpret_cast<const void *>(target_bt_sc0_enter);
    md.opt_handles[H_BT_SC0_EXIT_M]     = &g_methods[75];
    md.opt_handles[H_BT_SC0_EXIT_CODE]  = reinterpret_cast<const void *>(target_bt_sc0_exit);

    md.opt_handles[H_BT_SC1_CLASS]      = &g_extra_classes[1];
    md.opt_handles[H_BT_SC1_ENTER_M]    = &g_methods[76];
    md.opt_handles[H_BT_SC1_ENTER_CODE] = reinterpret_cast<const void *>(target_bt_sc1_enter);
    md.opt_handles[H_BT_SC1_EXIT_M]     = &g_methods[77];
    md.opt_handles[H_BT_SC1_EXIT_CODE]  = reinterpret_cast<const void *>(target_bt_sc1_exit);

    md.opt_handles[H_BT_SC2_CLASS]      = &g_extra_classes[2];
    md.opt_handles[H_BT_SC2_ENTER_M]    = &g_methods[78];
    md.opt_handles[H_BT_SC2_ENTER_CODE] = reinterpret_cast<const void *>(target_bt_sc2_enter);
    md.opt_handles[H_BT_SC2_EXIT_M]     = &g_methods[79];
    md.opt_handles[H_BT_SC2_EXIT_CODE]  = reinterpret_cast<const void *>(target_bt_sc2_exit);

    md.opt_handles[H_BT_SW0_CLASS]     = &g_extra_classes[3];
    md.opt_handles[H_BT_SW0_OPEN_M]    = &g_methods[80];
    md.opt_handles[H_BT_SW0_OPEN_CODE] = reinterpret_cast<const void *>(target_bt_sw0_open);
    md.opt_handles[H_BT_SW0_CL0_M]     = &g_methods[81];
    md.opt_handles[H_BT_SW0_CL0_CODE]  = reinterpret_cast<const void *>(target_bt_sw0_cl0);
    md.opt_handles[H_BT_SW0_CL1_M]     = &g_methods[82];
    md.opt_handles[H_BT_SW0_CL1_CODE]  = reinterpret_cast<const void *>(target_bt_sw0_cl1);
    md.opt_handles[H_BT_SW0_SHOW_M]    = &g_methods[M_BT_SW0_SHOW];
    md.opt_handles[H_BT_SW0_SHOW_CODE] = reinterpret_cast<const void *>(dummy_code_target);

    md.opt_handles[H_BT_SW1_CLASS]     = &g_extra_classes[4];
    md.opt_handles[H_BT_SW1_OPEN_M]    = &g_methods[83];
    md.opt_handles[H_BT_SW1_OPEN_CODE] = reinterpret_cast<const void *>(target_bt_sw1_open);
    md.opt_handles[H_BT_SW1_CL0_M]     = &g_methods[84];
    md.opt_handles[H_BT_SW1_CL0_CODE]  = reinterpret_cast<const void *>(target_bt_sw1_cl0);
    md.opt_handles[H_BT_SW1_CL1_M]     = &g_methods[85];
    md.opt_handles[H_BT_SW1_CL1_CODE]  = reinterpret_cast<const void *>(target_bt_sw1_cl1);
    md.opt_handles[H_BT_SW1_SHOW_M]    = &g_methods[M_BT_SW1_SHOW];
    md.opt_handles[H_BT_SW1_SHOW_CODE] = reinterpret_cast<const void *>(dummy_code_target);

    md.opt_handles[H_BT_SW2_CLASS]     = &g_extra_classes[5];
    md.opt_handles[H_BT_SW2_OPEN_M]    = &g_methods[86];
    md.opt_handles[H_BT_SW2_OPEN_CODE] = reinterpret_cast<const void *>(target_bt_sw2_open);
    md.opt_handles[H_BT_SW2_CL0_M]     = &g_methods[87];
    md.opt_handles[H_BT_SW2_CL0_CODE]  = reinterpret_cast<const void *>(target_bt_sw2_cl0);
    md.opt_handles[H_BT_SW2_CL1_M]     = &g_methods[88];
    md.opt_handles[H_BT_SW2_CL1_CODE]  = reinterpret_cast<const void *>(target_bt_sw2_cl1);
    md.opt_handles[H_BT_SW2_SHOW_M]    = &g_methods[M_BT_SW2_SHOW];
    md.opt_handles[H_BT_SW2_SHOW_CODE] = reinterpret_cast<const void *>(dummy_code_target);

    md.opt_handles[H_BT_GYM_ITER]         = &g_extra_classes[6];
    md.opt_handles[H_BT_GYM_WARN_STATE]   = &g_fields[F_BT_GYM_WARN_STATE];
    md.opt_handles[H_BT_F_MAX_GESTURE]    = &g_fields[F_BT_MAX_GESTURE];
    md.opt_handles[H_BT_F_DIRECTOR]       = &g_fields[F_BT_DIRECTOR];
    md.opt_handles[H_BT_F_BATTLE_STATUS]  = &g_fields[F_BT_BATTLE_STATUS];
    md.opt_handles[H_BT_F_LEG_FLICK]      = &g_fields[F_BT_LEG_FLICK];
    md.opt_handles[H_BT_F_LEG_TAP]        = &g_fields[F_BT_LEG_TAP];
    md.opt_handles[H_BT_F_LEG_BUTTON]     = &g_fields[F_BT_LEG_BUTTON];
    md.opt_handles[H_BT_F_MAX_BUTTON]     = &g_fields[F_BT_MAX_BUTTON];
    md.opt_handles[H_BT_F_MAX_TYPE]       = &g_fields[F_BT_MAX_TYPE];

    md.opt_handles[H_BT_M_CUR_STATE]          = &g_methods[M_BT_CUR_STATE];
    md.opt_handles[H_BT_M_CUR_STATE_CODE]     = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_BT_M_RAID_FAST_ACT]      = &g_methods[M_BT_RAID_FAST_ACT];
    md.opt_handles[H_BT_M_RAID_FAST_ACT_CODE] = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_BT_M_DESTROY]            = &g_methods[89];
    md.opt_handles[H_BT_M_DESTROY_CODE]       = reinterpret_cast<const void *>(target_bt_destroy);
    md.opt_handles[H_BT_M_DODGE]              = &g_methods[M_BT_DODGE];
    md.opt_handles[H_BT_M_DODGE_CODE]         = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_BT_M_GYM_WARN]           = &g_methods[90];
    md.opt_handles[H_BT_M_GYM_WARN_CODE]      = reinterpret_cast<const void *>(target_bt_gym_warn);
    md.opt_handles[H_BT_M_RAID_WARN]          = &g_methods[91];
    md.opt_handles[H_BT_M_RAID_WARN_CODE]     = reinterpret_cast<const void *>(target_bt_raid_warn);
    md.opt_handles[H_BT_M_MAX_WARN]           = &g_methods[92];
    md.opt_handles[H_BT_M_MAX_WARN_CODE]      = reinterpret_cast<const void *>(target_bt_max_warn);
    md.opt_handles[H_BT_M_LEG_DODGE]          = &g_methods[M_BT_LEG_DODGE];
    md.opt_handles[H_BT_M_LEG_DODGE_CODE]     = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_BT_M_ENABLE]             = &g_methods[93];
    md.opt_handles[H_BT_M_ENABLE_CODE]        = reinterpret_cast<const void *>(target_bt_enable);
    md.opt_handles[H_BT_M_DISABLE]            = &g_methods[94];
    md.opt_handles[H_BT_M_DISABLE_CODE]       = reinterpret_cast<const void *>(target_bt_disable);
    md.opt_handles[H_BT_M_BUTTONS]            = &g_methods[95];
    md.opt_handles[H_BT_M_BUTTONS_CODE]       = reinterpret_cast<const void *>(target_bt_buttons);
    md.opt_handles[H_BT_M_RAID_FAST]          = &g_methods[96];
    md.opt_handles[H_BT_M_RAID_FAST_CODE]     = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_BT_M_MAX_FAST]           = &g_methods[M_BT_MAX_FAST];
    md.opt_handles[H_BT_M_MAX_FAST_CODE]      = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_BT_M_LEG_ENABLE]         = &g_methods[97];
    md.opt_handles[H_BT_M_LEG_ENABLE_CODE]    = reinterpret_cast<const void *>(target_bt_leg_enable);
    md.opt_handles[H_BT_M_LEG_DISABLE]        = &g_methods[98];
    md.opt_handles[H_BT_M_LEG_DISABLE_CODE]   = reinterpret_cast<const void *>(target_bt_leg_disable);
    md.opt_handles[H_BT_M_LEG_BUTTONS]        = &g_methods[99];
    md.opt_handles[H_BT_M_LEG_BUTTONS_CODE]   = reinterpret_cast<const void *>(target_bt_leg_buttons);
    md.opt_handles[H_BT_M_LEG_FAST]           = &g_methods[M_BT_LEG_FAST];
    md.opt_handles[H_BT_M_LEG_FAST_CODE]      = reinterpret_cast<const void *>(target_bt_leg_fast);
    md.opt_handles[H_BT_M_LEG_INIT]           = &g_methods[100];
    md.opt_handles[H_BT_M_LEG_INIT_CODE]      = reinterpret_cast<const void *>(target_bt_leg_init);
    md.opt_handles[H_BT_M_LEG_CHARGED]        = &g_methods[M_BT_LEG_CHARGED];
    md.opt_handles[H_BT_M_LEG_CHARGED_CODE]   = reinterpret_cast<const void *>(dummy_code_target);
    md.opt_handles[H_BT_M_LEG_CHARGE]         = &g_methods[M_BT_LEG_CHARGE];
    md.opt_handles[H_BT_M_LEG_CHARGE_CODE]    = reinterpret_cast<const void *>(target_bt_leg_charge);
    md.opt_handles[H_BT_M_MAX_REFRESH]        = &g_methods[101];
    md.opt_handles[H_BT_M_MAX_REFRESH_CODE]   = reinterpret_cast<const void *>(target_bt_max_refresh);
    md.opt_handles[H_BT_M_MAX_CLEANUP]        = &g_methods[102];
    md.opt_handles[H_BT_M_MAX_CLEANUP_CODE]   = reinterpret_cast<const void *>(target_bt_max_cleanup);
    md.opt_handles[H_BT_M_ACTIVE]             = &g_methods[M_BT_ACTIVE];
    md.opt_handles[H_BT_M_PRESS]              = &g_methods[M_BT_PRESS];

    // --- 2. Configure Feature 66 (`combat_host`) -----------------------------------
    PgrCombatHostConfig cfg{};
    cfg.version             = PGR_COMBAT_HOST_API_VERSION;
    cfg.size                = sizeof(cfg);
    cfg.flags               = PGR_CBH_CFG_REQUIRE_BATTLES | PGR_CBH_CFG_REQUIRE_ROCKET;
    if(transport_mode)cfg.flags|=PGR_CBH_CFG_USE_TRANSPORT;
    cfg.metadata            = &md;
    cfg.thread_current      = reinterpret_cast<uintptr_t>(fake_thread_current);
    cfg.gchandle_new        = reinterpret_cast<uintptr_t>(fake_gc_new);
    cfg.gchandle_get_target = reinterpret_cast<uintptr_t>(fake_gc_get);
    cfg.gchandle_free       = reinterpret_cast<uintptr_t>(fake_gc_free);
    cfg.object_get_class    = reinterpret_cast<uintptr_t>(fake_object_get_class);
    cfg.object_unbox        = reinterpret_cast<uintptr_t>(fake_object_unbox);
    cfg.runtime_invoke      = reinterpret_cast<uintptr_t>(fake_runtime_invoke);
    cfg.field_get_value     = reinterpret_cast<uintptr_t>(fake_field_get_value);
    cfg.owner_tid           = tid;

    int rc = host->configure(&cfg);
    assert(rc == PGR_CBH_OK);

    PgrCombatHostSample s{};
    assert(host->sample(&s) == PGR_CBH_OK);
    assert(s.ready == 1);
    assert(s.failed == 0);
    assert(s.capabilities == (PGR_CBH_CAP_ROCKET_MINIGAME |
                              PGR_CBH_CAP_ROCKET_ACTIONS |
                              PGR_CBH_CAP_ROCKET_COMBAT |
                              PGR_CBH_CAP_BATTLES |
                              PGR_CBH_CAP_SHARED_VNEXT_OBSERVE));
    assert(s.installed_hooks == 45); // 10 Rocket + 30 Battle + 1 Shared + 4 Minigame
    assert(s.owner_tid == tid);

    auto init_evs = drain_all(host);
    if(transport_mode)assert(init_evs.empty());else{
    assert(has_event(init_evs, PGR_CBH_EV_ROCKET_MINIGAME_READY, "\"available\":true"));
    assert(has_event(init_evs, PGR_CBH_EV_ROCKET_COMBAT_READY, "\"available\":true"));
    assert(has_event(init_evs, PGR_CBH_EV_BATTLES_READY, "\"available\":true"));
    assert(has_event(init_evs, PGR_CBH_EV_HOST_READY, "\"available\":true"));
    }

    // --- 3. Initialize Synthetic Managed Object Graph ------------------------------
    // Battle state & scopes
    set_ptr(&g_battle_state, 0, md.opt_classes[OC_BATTLE_44226]);
    set_i32(&g_battle_state, 0x88, 2); // status_active = 2

    set_ptr(&g_raid_scope, 0, md.opt_handles[H_BT_SC0_CLASS]);
    set_ptr(&g_raid_scope, 0x80, &g_battle_director);

    set_ptr(&g_gym_scope, 0, md.opt_handles[H_BT_SC1_CLASS]);
    set_ptr(&g_gym_scope, 0x80, &g_battle_director);

    set_ptr(&g_max_scope, 0, md.opt_handles[H_BT_SC2_CLASS]);
    set_ptr(&g_max_scope, 0x80, &g_battle_director);
    set_ptr(&g_max_scope, 0x78, &g_max_gesture); // f_max_gesture for scope recovery

    // Gestures & Charge displays
    set_ptr(&g_raid_gesture, 0, md.opt_classes[OC_BATTLE_31499]);
    set_u8(&g_raid_gesture, 0x28, 1);  // f_vnext_enabled = 1
    set_ptr(&g_raid_gesture, 0x30, &g_tap_obj);
    set_ptr(&g_raid_gesture, 0x80, &g_battle_director); // live-status getter reads the gesture's director

    set_ptr(&g_gym_gesture, 0, md.opt_classes[OC_BATTLE_9182]);
    set_ptr(&g_gym_gesture, 0x90, &g_flick_obj); // f_legacy_flick
    set_ptr(&g_gym_gesture, 0x98, &g_tap_obj);   // f_legacy_tap

    set_ptr(&g_max_gesture, 0, md.opt_classes[OC_BATTLE_31285]);
    set_u8(&g_max_gesture, 0x28, 1);
    set_ptr(&g_max_gesture, 0x30, &g_tap_obj);
    set_ptr(&g_max_gesture, 0x80, &g_battle_director);

    set_ptr(&g_vn_charge, 0, md.opt_classes[OC_RVCHARGE]);
    set_ptr(&g_vn_charge, 0x38, &g_charge_btn); // f_vnext_button

    set_ptr(&g_leg_charge, 0, md.opt_classes[OC_BATTLE_9308]);
    set_ptr(&g_leg_charge, 0xA0, &g_charge_btn); // f_legacy_button

    set_ptr(&g_max_widget, 0, md.opt_classes[OC_BATTLE_31280]);
    set_ptr(&g_max_widget, 0xA8, &g_max_btn); // f_max_button
    set_i32(&g_max_widget, 0xB0, 1);          // f_max_type == I_BAT_MAX_ATTACK (1)

    set_ptr(&g_raid_warn, 0, md.opt_classes[OC_BATTLE_31623]);
    set_ptr(&g_max_warn, 0, md.opt_classes[OC_BATTLE_31263]);
    set_ptr(&g_gym_iter, 0, md.opt_handles[H_BT_GYM_ITER]);
    set_ptr(&g_raid_swap, 0, md.opt_handles[H_BT_SW1_CLASS]);

    // Rocket objects
    set_ptr(&g_rk_state, 0, md.opt_classes[OC_ROCKET]);
    set_ptr(&g_rk_ui, 0, md.opt_classes[OC_RUI]);
    set_i32(&g_rk_ui, 0x70, PGR_CBH_STATE_OPEN);
    set_ptr(&g_rk_ui, 0x50, &g_rk_charge1); // f_first
    set_ptr(&g_rk_director, 0, md.opt_classes[OC_RDIR]);
    set_ptr(&g_rk_charge1, 0, md.opt_classes[OC_RCHARGE]);
    set_u8(&g_rk_charge1, 0x60, 0); // f_charged
    set_u8(&g_rk_charge1, 0x61, 1); // f_input
    set_ptr(&g_rk_shield, 0, md.opt_classes[OC_RSHIELD]);
    set_i32(&g_rk_shield, 0x70, PGR_CBH_STATE_OPEN);
    set_i32(&g_rk_shield, 0x64, 2); // f_shields = 2
    set_ptr(&g_rk_shield, 0x68, &g_rk_shield_btn);

    set_ptr(&g_rk_vui, 0, md.opt_classes[OC_RVUI]);
    set_i32(&g_rk_vui, 0x70, PGR_CBH_STATE_OPEN);
    set_ptr(&g_rk_vui, 0x20, &g_rk_vgest);
    set_ptr(&g_rk_vgest, 0, md.opt_classes[OC_RVGEST]);
    set_u8(&g_rk_vgest, 0x28, 1); // f_vnext_enabled
    set_ptr(&g_rk_vgest, 0x30, &g_tap_obj);

    set_ptr(&g_mini_challenge, 0, md.opt_classes[OC_RMINI]);
    set_ptr(&g_mini_challenge, 0x40, &g_mini_collider);
    set_u8(&g_mini_challenge, 0x48, 1); // running = 1

    // Arm both Battle (all 1023 bits) and Rocket (all 7 bits) leases
    PgrCombatHostOptions opts{};
    opts.version         = PGR_COMBAT_HOST_API_VERSION;
    opts.size            = sizeof(opts);
    opts.flags           = PGR_CBH_OPT_ACTIVE | PGR_CBH_OPT_BATTLES_ALLOWED | PGR_CBH_OPT_ROCKET_ALLOWED;
    opts.battle_mask     = PGR_CBH_BATTLE_MASK_ALL;
    opts.rocket_mask     = PGR_CBH_ROCKET_MASK_ALL;
    opts.battle_lease_ms = PGR_CBH_MAX_LEASE_MS;
    opts.rocket_lease_ms = PGR_CBH_MAX_LEASE_MS;
    opts.epoch           = 1;
    assert(host->update(&opts) == PGR_CBH_OK);

    if(transport_mode){
        const auto *scheduler=static_cast<const PgrUnitySchedulerApi3*>(core->query_feature(48,3));assert(scheduler);
        PgrUnitySchedulerConfig sc{};sc.version=2;sc.size=sizeof(sc);sc.runtime_invoke=reinterpret_cast<void*>(fake_runtime_invoke);
        sc.runtime_invoke_convert_args=reinterpret_cast<void*>(fake_convert);sc.execute_tasks=&g_execute_token;sc.thread_current=fake_thread_current;
        sc.verify_metadata=verify_scheduler;sc.expected_owner_tid=tid;sc.exclusive_owner=1;sc.subscriber_count=1;sc.subscribers[0].end=host->on_unity_pulse;
        assert(scheduler->install(&sc)==0);const auto generation=scheduler->activate();assert(generation);
        call_target_bt_sc0_enter(&g_raid_scope,nullptr,nullptr,nullptr);
        call_target_bt_enable(&g_raid_gesture,nullptr);
        // Natural lifecycle callbacks occur outside ExecuteTasks on the same
        // Unity owner. Capture state now, but leave attacks to the next pulse.
        assert(scheduler->executing_generation(tid)==0);
        assert(host->sample(&s)==0&&s.battle_scope_active&&g_live_gc_handles>0);
        assert(g_raid_fast_invokes==0);
        g_execute_mode=0;execute();assert(g_raid_fast_invokes==1);assert(host->sample(&s)==0&&s.battle_scope_active);
        g_execute_mode=2;execute();settings(false,false,true);
        g_execute_mode=1;execute();assert(g_raid_fast_invokes==1); // adjacent7 cannot authorize
        g_execute_mode=2;execute();g_execute_mode=3;execute();assert(host->sample(&s)==0&&!s.mini_active&&g_rk_fast_invokes==0);
        settings(true,true,false);g_rk_dir_active=true;g_execute_mode=0;execute();
        assert(host->sample(&s)==0&&s.mini_active&&g_rk_fast_invokes==1); // correct26 authorizes despite27 false
        g_execute_mode=4;execute();
        // More events than the diagnostic queue capacity, regularly drained by52.
        for(int n=0;n<300;++n){g_execute_mode=1;execute();g_execute_mode=2;execute();settings(true,true,false);}
        PgrRuntimeTransportSample ts{};assert(g_transport->sample(&ts)==0&&ts.events_emitted>600&&ts.events_dropped==0&&!ts.fault);
        assert(drain_all(host).empty());assert(scheduler->revoke(generation)==0);
        call_target_bt_sc0_enter(&g_raid_scope,nullptr,nullptr,nullptr);
        assert(host->sample(&s)==0&&!s.battle_scope_active&&g_live_gc_handles==0);
        assert(host->stop(tid)==0);assert(g_live_gc_handles==0);
        puts("PASS combat_host transport52/scheduler48/sustained-events");return 0;
    }

    // A real leaf observer allocation attempts a collection. All three original
    // managed references must already be pinned, while an unrelated root moves.
    FakeObj arg1{},arg2{};auto moving=fake_gc_new(&g_move_from,0);
    g_gc_protected[0]=&g_raid_scope;g_gc_protected[1]=&arg1;g_gc_protected[2]=&arg2;g_sweep=true;
    call_target_bt_sc0_enter(&g_raid_scope,&arg1,&arg2,nullptr);g_sweep=false;
    assert(g_sweeps>0&&fake_gc_get(moving)==&g_move_to);
    assert(g_capture_args[0]==&g_raid_scope&&g_capture_args[1]==&arg1&&g_capture_args[2]==&arg2);
    fake_gc_free(moving);call_target_bt_sc0_exit(&g_raid_scope,nullptr);drain_all(host);
    g_bt_sc0_enter_calls.store(0);

    // --- 4. Scenario A: Raid Battle (Mode 0) + Shared `ObserveBattleState` ---------
    call_target_bt_sc0_enter(&g_raid_scope, nullptr, nullptr, nullptr);
    assert(g_bt_sc0_enter_calls.load() == 1);
    call_target_bt_enable(&g_raid_gesture, nullptr);
    call_target_shared_vn_observe(&g_vn_charge, &g_battle_state, nullptr);
    assert(g_shared_vn_observe_calls.load() == 1);

    // Trigger Raid warning -> dodge cue -> dodge action on tick
    call_target_bt_raid_warn(&g_raid_warn, nullptr);
    assert(g_bt_raid_warn_calls.load() == 1);
    assert(host->tick(tid) == PGR_CBH_OK);
    assert(g_raid_dodge_invokes == 1);

    auto raid_evs = drain_all(host);
    assert(has_event(raid_evs, PGR_CBH_EV_BATTLE_SCOPE, "\"mode\":0,\"active\":true"));
    assert(has_event(raid_evs, PGR_CBH_EV_BATTLE_INPUT, "\"mode\":0,\"legacy\":false"));
    assert(has_event(raid_evs, PGR_CBH_EV_BATTLE_DODGE_CUE, "\"mode\":0"));
    assert(has_event(raid_evs, PGR_CBH_EV_BATTLE_ACTION, "\"mode\":0,\"action\":\"dodge\""));

    // Swap picker open blocks attacks
    g_swap_showing = true;
    call_target_bt_sw1_open(&g_raid_swap, nullptr);
    assert(host->sample(&s) == PGR_CBH_OK);
    assert(s.battle_swap_blocked == 1);
    g_swap_showing = false;
    call_target_bt_sw1_cl0(&g_raid_swap, nullptr);

    call_target_bt_sc0_exit(&g_raid_scope, nullptr);
    assert(host->sample(&s) == PGR_CBH_OK);
    assert(s.battle_scope_active == 0);
    assert(s.battle_roots == 0);
    drain_all(host);

    // --- 5. Scenario B: Gym Battle (Mode 1) — Legacy Charge/Fast, Gym Warn, Cooldown
    call_target_bt_sc1_enter(&g_gym_scope, nullptr);
    call_target_bt_leg_enable(&g_gym_gesture, nullptr);
    TestVec3 init_pos{1.25f, -2.5f, 3.75f};
    call_target_bt_leg_init(&g_leg_charge, nullptr, nullptr, init_pos, nullptr);
    assert(g_bt_leg_init_calls.load() == 1);
    assert(std::fabs(g_last_leg_init_pos.x - 1.25f) < 1e-5f);
    assert(std::fabs(g_last_leg_init_pos.y + 2.5f) < 1e-5f);
    assert(std::fabs(g_last_leg_init_pos.z - 3.75f) < 1e-5f);

    // Gym warning iterator MoveNext: eligible state, ret=0 triggers gym dodge cue
    set_i32(&g_gym_iter, 0x10, 0);
    drain_all(host);const int gym_roots=g_live_gc_handles;g_throw_gym=true;
    try{call_target_bt_gym_warn(&g_gym_iter,nullptr);assert(false);}catch(int e){assert(e==602);}g_throw_gym=false;
    assert(g_live_gc_handles==gym_roots);assert(host->sample(&s)==0&&!s.active_frames);
    assert(!has_event(drain_all(host),PGR_CBH_EV_BATTLE_DODGE_CUE));
    int32_t mv = call_target_bt_gym_warn(&g_gym_iter, nullptr);
    assert(mv == 0);
    assert(host->tick(tid) == PGR_CBH_OK);
    assert(g_leg_dodge_invokes == 1);

    // Direct manual tap on legacy charge button during Gym battle emits "Gym battle" cooldown-action
    call_target_bt_leg_charge(&g_leg_charge, nullptr);
    call_target_bt_leg_fast(&g_gym_gesture, &g_tap_obj, nullptr, nullptr);
    auto gym_evs = drain_all(host);
    assert(has_event(gym_evs, PGR_CBH_EV_BATTLE_DODGE_CUE, "\"mode\":1"));
    assert(has_event(gym_evs, PGR_CBH_EV_BATTLE_ACTION, "\"mode\":1,\"action\":\"dodge\""));
    assert(has_event(gym_evs, PGR_CBH_EV_COOLDOWN_ACTION, "\"action\":\"Gym battle\""));
    assert(host->sample(&s) == PGR_CBH_OK);
    assert(s.gym_cooldown_serial >= 2);

    call_target_bt_sc1_exit(&g_gym_scope, nullptr);
    assert(host->sample(&s) == PGR_CBH_OK);
    assert(s.battle_scope_active == 0);
    assert(s.battle_roots == 0);
    drain_all(host);

    // --- 6. Scenario C: Max / Dynamax Battle (Mode 2) ------------------------------
    call_target_bt_sc2_enter(&g_max_scope, nullptr, nullptr, nullptr);
    call_target_bt_max_refresh(&g_max_widget, nullptr, nullptr);
    assert(host->tick(tid) == PGR_CBH_OK);
    assert(g_max_press_invokes == 1);

    call_target_bt_max_warn(&g_max_warn, 0.625f, nullptr);
    assert(std::fabs(g_last_max_warn_sec - 0.625f) < 1e-5f);
    auto max_evs = drain_all(host);
    assert(has_event(max_evs, PGR_CBH_EV_BATTLE_INPUT, "\"source\":\"scope\""));
    assert(has_event(max_evs, PGR_CBH_EV_BATTLE_ACTION, "\"mode\":2,\"action\":\"max-attack\""));
    assert(has_event(max_evs, PGR_CBH_EV_BATTLE_DODGE_CUE, "\"mode\":2"));

    call_target_bt_max_cleanup(&g_max_widget, nullptr);
    call_target_bt_sc2_exit(&g_max_scope, nullptr);
    assert(host->sample(&s) == PGR_CBH_OK);
    assert(s.battle_scope_active == 0);
    assert(s.battle_roots == 0);
    drain_all(host);

    // --- 7. Scenario D: Team GO Rocket + Hold Gate + Shield + vNext + Minigame -----
    call_target_rk_enter(&g_rk_state, nullptr, nullptr, nullptr);
    const int ui_roots=g_live_gc_handles;g_throw_ui=true;
    try{call_target_rk_ui_init(&g_rk_ui,nullptr,nullptr);assert(false);}catch(int e){assert(e==601);}g_throw_ui=false;
    assert(g_live_gc_handles==ui_roots);assert(host->sample(&s)==0&&!s.active_frames&&!s.rocket_ui_active);
    call_target_rk_ui_init(&g_rk_ui, nullptr, nullptr);
    assert(host->sample(&s) == PGR_CBH_OK && s.rocket_ui_active == 1);
    // begin/end initialization publishes silently; live-update telemetry occurs
    // only when UpdateActionsUI adopts an uncaptured UI.
    call_target_rk_ui_close(&g_rk_ui, nullptr);
    call_target_rk_ui_update(&g_rk_ui, 1, nullptr);
    assert(g_rk_enter_calls.load() == 1);
    assert(g_rk_ui_init_calls.load() == 2);

    // Director not yet active -> hold gate ("battle-not-active", rocket_gate == 1)
    g_rk_dir_active = false;
    assert(host->tick(tid) == PGR_CBH_OK);
    assert(host->sample(&s) == PGR_CBH_OK);
    assert(s.rocket_scope_active == 1);
    assert(s.rocket_ui_active == 1);
    assert(s.rocket_gate == 1);

    // Director becomes active -> combat-ready ("active", rocket_gate == 2) + quick tap
    g_rk_dir_active = true;
    assert(host->tick(tid) == PGR_CBH_OK);
    assert(g_rk_fast_invokes >= 1);
    assert(host->sample(&s) == PGR_CBH_OK);
    assert(s.rocket_gate == 2);

    // Shield prompt -> auto shield click on next tick
    call_target_rk_prompt(&g_rk_shield, 1, nullptr);
    assert(host->sample(&s) == PGR_CBH_OK);
    assert(s.rocket_shield_active == 1);
    // Real feature20 enforces a 150ms cooldown after the preceding quick action.
    usleep(170000);
    assert(host->tick(tid) == PGR_CBH_OK);
    assert(g_rk_shield_invokes == 1);

    // Rocket Particle Minigame (Feature 6): UpdateMinigame scales collider by 1000x
    g_mini_scale[0] = 2.0f; g_mini_scale[1] = 2.0f; g_mini_scale[2] = 2.0f;
    g_mini_go_active = 0;
    call_target_mn_update(&g_mini_challenge, 16, nullptr);
    assert(g_mn_update_calls.load() == 1);
    assert(std::fabs(g_mini_scale[0] - 2000.0f) < 1e-2f);
    assert(std::fabs(g_mini_scale[1] - 2000.0f) < 1e-2f);
    assert(std::fabs(g_mini_scale[2] - 2000.0f) < 1e-2f);
    assert(g_mini_go_active == 1);

    // vNext must honor its blocked gate, then select charged before quick. Real
    // feature20 enforces 250ms after shield and 600ms after vNext charged action.
    assert(host->update(&opts)==0);
    call_target_rk_vn_update(&g_rk_vui,&g_battle_state,nullptr);
    call_target_shared_vn_observe(&g_vn_charge,&g_battle_state,nullptr);
    g_vn_charged_ready=true;g_rk_vnext_blocked=true;usleep(270000);
    const int charged_before=g_vn_charge_invokes;
    assert(host->tick(tid)==0&&g_vn_charge_invokes==charged_before&&g_rk_vn_fast_invokes==0);
    g_rk_vnext_blocked=false;
    assert(host->tick(tid)==0&&g_vn_charge_invokes==charged_before+1&&g_rk_vn_fast_invokes==0);
    g_vn_charged_ready=false;usleep(620000);
    assert(host->tick(tid)==0&&g_rk_vn_fast_invokes==1);
    call_target_rk_vn_close(&g_rk_vui,nullptr);

    // Exiting Rocket scope automatically restores minigame scale (2.0f) and activeSelf (0)
    call_target_rk_exit(&g_rk_state, nullptr);
    assert(std::fabs(g_mini_scale[0] - 2.0f) < 1e-3f);
    assert(std::fabs(g_mini_scale[1] - 2.0f) < 1e-3f);
    assert(std::fabs(g_mini_scale[2] - 2.0f) < 1e-3f);
    assert(g_mini_go_active == 0);

    auto rk_evs = drain_all(host);
    assert(has_event(rk_evs, PGR_CBH_EV_ROCKET_SCOPE, "\"active\":true"));
    assert(has_event(rk_evs, PGR_CBH_EV_ROCKET_UI, "\"source\":\"live-update\""));
    assert(has_event(rk_evs, PGR_CBH_EV_ROCKET_ATTACK_GATE, "\"reason\":\"battle-not-active\""));
    assert(has_event(rk_evs, PGR_CBH_EV_ROCKET_ATTACK_GATE, "\"reason\":\"active\""));
    assert(has_event(rk_evs, PGR_CBH_EV_ROCKET_ACTION, "\"action\":\"quick\""));
    assert(has_event(rk_evs, PGR_CBH_EV_ROCKET_ACTION, "\"action\":\"shield\""));
    assert(has_event(rk_evs, PGR_CBH_EV_ROCKET_SCOPE, "\"active\":false"));

    // --- 8. Scheduler 48 `on_unity_pulse` + `revoke()` + `stop(tid)` Cleanup -------
    assert(host->sample(&s) == PGR_CBH_OK);
    const uint64_t ticks_before = s.ticks;
    PgrUnityPulse pulse{1, 1, static_cast<int32_t>(tid), 0};
    host->on_unity_pulse(nullptr, &pulse);
    assert(host->sample(&s) == PGR_CBH_OK);
    assert(s.ticks == ticks_before + 1);

    assert(host->revoke() == PGR_CBH_OK);
    assert(host->stop(tid) == PGR_CBH_OK);
    assert(host->sample(&s) == PGR_CBH_OK);
    assert(s.stopping == 1);
    assert(s.roots == 0);
    assert(s.battle_roots == 0);
    assert(s.rocket_roots == 0);
    assert(s.active_frames == 0);
    assert(g_live_gc_handles == 0);

    std::printf("PASS combat_host_test (caps=0x%x hooks=%u ticks=%llu)\n",
                s.capabilities, s.installed_hooks,
                static_cast<unsigned long long>(s.ticks));
    pid_t child=fork();assert(child>=0);if(child==0){execl(argv[0],argv[0],argv[1],"--transport",nullptr);_exit(127);}
    int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    return 0;
}

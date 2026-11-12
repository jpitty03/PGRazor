#pragma once
#include <stdint.h>

// Gym "Battle until defeated" — pure lifecycle/decision state machine.
//
// This header is deliberately free of any IL2CPP / game / transport dependency so
// it can be unit tested on the host compiler (see gym_repeat_test.cpp). The native
// leaf (gym_repeat.cpp) owns all managed interaction and feeds this machine with
// events captured on the Unity owner thread, then performs the single action the
// machine decides to take on each scheduled tick.
//
// Feature contract (default OFF): a session is armed ONLY by the user manually
// pressing Battle at a hostile gym. After each full battle round completes with a
// victory, the loop starts the next round directly from the gym approach screen the
// game re-shows for the SAME gym (ClickBattle -> prep Battle), without returning to
// the map and without pressing Back. If the game instead drops back to the actual
// map on its own, an optional fallback re-enters the same gym with a single map tap
// (which also leaves room for on-map healing to run). It never roams to other gyms,
// never changes attack/dodge/heal/team preferences and never enters raids. Turning
// the toggle off, a manual leave, an account/background/stop, out-of-range, a
// neutral/friendly/no-defender gym, an unavailable gym, a loss, a managed failure or
// a timeout ends the session. A cleared gym ends the loop successfully on the gym
// screen with no further Battle press and no forced Back. Live gym state ends the
// loop; there is no fixed round count and there are no blind retries.
namespace pgr_gym_repeat {

enum State : int {
    ST_IDLE = 0,    // no session armed
    ST_ARMED,       // user pressed Battle; awaiting battle-scope-enter confirmation
    ST_IN_ROUND,    // battle scope active; combat_host drives defenders, we wait
    ST_WON,         // round completed victorious; re-entry pipeline in progress
    ST_ENDED        // session terminated (see EndReason)
};

// A single managed action the native leaf should attempt this tick.
enum Action : int {
    ACT_NONE = 0,
    ACT_CLICK_BATTLE,   // gym approach is open for our gym -> GymApproachGuiController.ClickBattle()
    ACT_PREP_BATTLE,    // battle prep is open for our gym  -> GymBattlePrepUIController.Battle()
    ACT_RESOLVE_GYM     // live target confirmed on map (map fallback only) -> MapGym.OnTapped() once
};

enum EndReason : int {
    END_NONE = 0,
    END_DISABLED,       // toggle turned off
    END_STALE,          // transport snapshot stale / faulted
    END_BACKGROUND,     // host not active / backgrounded / stopping
    END_ACCOUNT,        // account identity changed
    END_MANUAL_CANCEL,  // user manually left (leave battle / prep exit / approach back)
    END_DEFEATED,       // gym cleared: neutral / friendly / no defenders (success)
    END_RAID,           // raid became active on the target gym
    END_OUT_OF_RANGE,   // player moved out of interaction range
    END_UNAVAILABLE,    // gym disabled / closed / not found / re-entry binding unavailable
    END_LOSS,           // player's party fainted
    END_TIMEOUT,        // false start or re-entry deadline elapsed (no blind retry)
    END_ERROR,          // managed invocation failed / safety bound hit
    END_COUNT
};

// Live gym observation resolved by the caller when the machine asks for it
// before tick(). The machine never infers these; it only reacts to them.
enum GymStatus : int {
    GYM_UNKNOWN = 0,    // not resolved yet / still in a gym screen; keep waiting
    GYM_HOSTILE,        // enemy team with defenders present -> battle again
    GYM_CLEARED,        // neutral / friendly / no defenders -> defeated (success)
    GYM_RAID,           // raid active
    GYM_OUT_OF_RANGE,   // out of interaction range
    GYM_UNAVAILABLE     // disabled / closed / not found / unresolvable
};

// Host permission snapshot. All fields must hold for the loop to keep acting.
struct Gate {
    bool enabled = false;         // transport bool 40 (Control.gymRepeatEnabled)
    bool active = false;          // transport bool 3  (automation active)
    bool battles_allowed = false; // transport bool 6  (gym/battle automation on)
    bool fresh = true;            // snapshot fresh AND not faulted
    bool foreground = true;       // host not stopping/failed and owner attached
};

struct Timing {
    uint64_t arm_confirm_ms = 15000; // await battle-scope-enter after a Battle press
    uint64_t reenter_ms     = 60000; // overall budget from round-won to next scope enter
    uint64_t heal_dwell_ms  = 4500;  // minimum on-map dwell so one heal scan can run
    int      max_rounds     = 60;    // safety bound; live gym state normally ends the loop
};

enum Phase : int {
    PH_RETURN = 0,       // post-victory: ClickBattle the re-shown approach, or fall back to map
    PH_MAP_DWELL,        // natural map return visible; allow healing to run before re-tap
    PH_WAIT_APPROACH,    // one OnTapped consumed (map fallback); await fresh approach
    PH_WAIT_PREP        // one ClickBattle consumed; await ready party UI
};

class Policy {
public:
    State state = ST_IDLE;
    EndReason end = END_NONE;
    Action pending = ACT_NONE;
    Phase phase = PH_RETURN;
    uint64_t gym_hash = 0, deadline = 0, won_at = 0, map_at = 0;
    bool has_gym = false, map_visible = false;
    int rounds = 0;
    bool last_complete = false, last_fainted = false;

    bool armed() const { return state != ST_IDLE && state != ST_ENDED; }
    bool ended() const { return state == ST_ENDED; }
    void reset() { *this = Policy{}; }
    void end_session(EndReason reason) {
        state = ST_ENDED;
        if (end == END_NONE) end = reason;
        pending = ACT_NONE;
    }
    bool gate_ok(const Gate &g) {
        if (!armed()) return false;
        if (!g.enabled) { end_session(END_DISABLED); return false; }
        if (!g.fresh) { end_session(END_STALE); return false; }
        if (!g.active || !g.foreground) { end_session(END_BACKGROUND); return false; }
        if (!g.battles_allowed) { end_session(END_UNAVAILABLE); return false; }
        return true;
    }
    void arm(uint64_t identity, uint64_t now, const Gate &g, const Timing &t) {
        if (state != ST_IDLE || !identity || !g.enabled || !g.active ||
            !g.battles_allowed || !g.fresh || !g.foreground) return;
        reset(); state = ST_ARMED; gym_hash = identity; has_gym = true;
        deadline = now + t.arm_confirm_ms;
    }
    bool same_gym(uint64_t identity) {
        if (!identity || identity != gym_hash) { end_session(END_MANUAL_CANCEL); return false; }
        return true;
    }
    void on_scope_enter(uint64_t identity, uint64_t now, const Timing &) {
        if (state != ST_ARMED || !same_gym(identity)) return;
        if (now > deadline) { end_session(END_TIMEOUT); return; }
        state = ST_IN_ROUND; last_complete = last_fainted = false;
        deadline = 0; // navigation timeouts never cancel a legitimate long battle
    }
    // The adapter proves known terminal leave + Victory + BattleComplete. A
    // per-defender CONTINUE or a generic BattleComplete alone is NOT a round win.
    void on_results(bool victorious_terminal, bool fainted) {
        if (state != ST_IN_ROUND) return;
        last_complete = victorious_terminal; last_fainted = fainted;
    }
    void on_scope_exit(uint64_t now, const Timing &t) {
        if (state != ST_IN_ROUND) return;
        if (last_fainted) { end_session(END_LOSS); return; }
        if (!last_complete) { end_session(END_MANUAL_CANCEL); return; }
        if (++rounds > t.max_rounds) { end_session(END_ERROR); return; }
        state = ST_WON; phase = PH_RETURN; pending = ACT_NONE;
        map_visible = false; map_at = 0; won_at = now;
        deadline = now + t.reenter_ms;
    }
    // Called only after the adapter proves this is the open/top approach for the exact
    // target. Both the direct repeat (PH_RETURN, the approach the game re-shows after a
    // win) and the post-tap map fallback (PH_WAIT_APPROACH) start the next round with
    // ClickBattle; neither ever presses Back. Repeated readiness polling cannot
    // re-issue a consumed action.
    void on_approach_open(uint64_t identity, const Timing &) {
        if (state != ST_WON || !same_gym(identity)) return;
        if (phase == PH_RETURN || phase == PH_WAIT_APPROACH) pending = ACT_CLICK_BATTLE;
    }
    void on_prep_open(uint64_t identity, bool ready, const Timing &) {
        if (state != ST_WON || phase != PH_WAIT_PREP || !same_gym(identity)) return;
        if (ready) pending = ACT_PREP_BATTLE;
    }
    // The natural map fallback: the game dropped back to the real map on its own.
    // Direct repeat never forces this; we only react if the map actually appears.
    void observe_map(bool visible, uint64_t now) {
        if (state != ST_WON) return;
        map_visible = visible;
        if (visible && phase == PH_RETURN) {
            phase = PH_MAP_DWELL; map_at = now; pending = ACT_NONE;
        } else if (!visible && phase == PH_MAP_DWELL) {
            end_session(END_MANUAL_CANCEL); // user left map before our tap
        }
    }
    void on_manual_cancel() { if (armed()) end_session(END_MANUAL_CANCEL); }
    void on_account_change() { if (armed()) end_session(END_ACCOUNT); }
    bool needs_gym_resolution(uint64_t now, const Gate &g, const Timing &t, bool heal_busy) const {
        return state == ST_WON && phase == PH_MAP_DWELL && map_visible &&
               pending == ACT_NONE && now <= deadline && now >= map_at &&
               now - map_at >= t.heal_dwell_ms && !heal_busy &&
               g.enabled && g.active && g.battles_allowed && g.fresh && g.foreground;
    }
    Action decide_map_action(GymStatus status) {
        switch (status) {
            case GYM_HOSTILE: phase = PH_WAIT_APPROACH; return ACT_RESOLVE_GYM;
            case GYM_CLEARED: end_session(END_DEFEATED); break;
            case GYM_RAID: end_session(END_RAID); break;
            case GYM_OUT_OF_RANGE: end_session(END_OUT_OF_RANGE); break;
            case GYM_UNAVAILABLE: end_session(END_UNAVAILABLE); break;
            default: break;
        }
        return ACT_NONE;
    }
    Action tick(uint64_t now, const Gate &g, const Timing &t, bool heal_busy, GymStatus live) {
        if (!gate_ok(g)) return ACT_NONE;
        if (state == ST_IN_ROUND) return ACT_NONE;
        if (now > deadline) { end_session(END_TIMEOUT); return ACT_NONE; }
        if (state != ST_WON) return ACT_NONE;
        Action action = pending; pending = ACT_NONE;
        // Advance BEFORE managed invocation: synchronous callbacks may confirm
        // the next phase, and post-invoke bookkeeping must never overwrite it.
        // Direct repeat (PH_RETURN) and map fallback (PH_WAIT_APPROACH) both ClickBattle.
        if (action == ACT_CLICK_BATTLE && (phase == PH_RETURN || phase == PH_WAIT_APPROACH)) {
            phase = PH_WAIT_PREP; return action;
        }
        if (action == ACT_PREP_BATTLE && phase == PH_WAIT_PREP) {
            state = ST_ARMED; deadline = now + t.arm_confirm_ms; return action;
        }
        return needs_gym_resolution(now, g, t, heal_busy) ? decide_map_action(live) : ACT_NONE;
    }
    void on_action_ok(Action, uint64_t, const Timing &) {} // state already consumed
    void on_action_failed() { if (armed()) end_session(END_ERROR); }
};

inline const char *end_reason_label(EndReason r) {
    switch (r) {
        case END_NONE:          return "none";
        case END_DISABLED:      return "disabled";
        case END_STALE:         return "stale";
        case END_BACKGROUND:    return "background";
        case END_ACCOUNT:       return "account-changed";
        case END_MANUAL_CANCEL: return "manual-cancel";
        case END_DEFEATED:      return "gym-defeated";
        case END_RAID:          return "raid-active";
        case END_OUT_OF_RANGE:  return "out-of-range";
        case END_UNAVAILABLE:   return "unavailable";
        case END_LOSS:          return "loss";
        case END_TIMEOUT:       return "timeout";
        case END_ERROR:         return "error";
        default:                return "unknown";
    }
}

} // namespace pgr_gym_repeat

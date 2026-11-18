#pragma once
#include <cstdint>
#include <cstring>

// Pure decisions for "Exchange gifts with Mateo". The adapter (route_gift.cpp) reports what the game
// shows; this decides which single button, if any, to press this tick. It acts only inside an NPC
// encounter whose steps include a gift exchange and no Pokemon trade, presses each one-time button at
// most once per encounter, and gives up (leaving the screen to the player) when a press has no effect.
namespace pgr_route_gift {

inline bool parse_config(const char *s, uint32_t n, bool &enabled) {
    enabled = false;
    if (!s || n != 4 || std::memcmp(s, "M1|", 3) != 0 || (s[3] != '0' && s[3] != '1')) return false;
    enabled = s[3] == '1';
    return true;
}

enum EncounterKind { ENC_NONE = 0, ENC_GIFT = 1, ENC_OTHER = 2 };

// steps_events: the NpcEventProto.Event value of each step (Unset when a step has no event).
inline EncounterKind classify(const int *step_events, int count, int gift_value, int trade_value) {
    if (!step_events || count <= 0) return ENC_OTHER;
    bool gift = false;
    for (int i = 0; i < count; ++i) {
        if (step_events[i] == trade_value) return ENC_OTHER;
        if (step_events[i] == gift_value) gift = true;
    }
    return gift ? ENC_GIFT : ENC_OTHER;
}

enum Screen { SC_DIALOG = 0, SC_ACCEPT, SC_SEND, SC_OPEN, SC_COUNT };
enum Action { ACT_NONE = -1, ACT_CONTINUE = SC_DIALOG, ACT_ACCEPT = SC_ACCEPT, ACT_SEND = SC_SEND, ACT_OPEN = SC_OPEN };

constexpr int64_t kGapMs = 800;              // between any two presses
constexpr int64_t kDwellMs[SC_COUNT] = {700, 1200, 1500, 1200};
constexpr int kLimit[SC_COUNT] = {30, 1, 1, 1};
constexpr int64_t kStuckMs = 10000;          // a one-time screen still open this long after its press
constexpr int64_t kEncounterMs = 180000;     // stop helping after this long in one encounter

struct State {
    bool active = false;        // inside a gift encounter
    int64_t started_ms = 0;
    int64_t last_press_ms = 0;
    int64_t open_since[SC_COUNT] = {0, 0, 0, 0};
    bool seen[SC_COUNT] = {false, false, false, false};
    int64_t pressed_at[SC_COUNT] = {0, 0, 0, 0};
    int presses[SC_COUNT] = {0, 0, 0, 0};
    bool gave_up = false;
    const char *why = "";
    void begin(int64_t now) { *this = State{}; active = true; started_ms = now; }
    void end() { *this = State{}; }
};

// open[s]: whether screen s is open and idle right now. Returns the press to make, or ACT_NONE.
inline Action decide(State &st, bool enabled, const bool open[SC_COUNT], int64_t now) {
    for (int s = 0; s < SC_COUNT; ++s) {
        if (!open[s]) st.seen[s] = false;
        else if (!st.seen[s]) { st.seen[s] = true; st.open_since[s] = now; }
    }
    if (!enabled || !st.active || st.gave_up) return ACT_NONE;
    if (now - st.started_ms > kEncounterMs) { st.gave_up = true; st.why = "Mateo's gift exchange took too long; finish it yourself"; return ACT_NONE; }
    for (int s = SC_ACCEPT; s < SC_COUNT; ++s) {
        if (st.presses[s] >= kLimit[s] && open[s] && now - st.pressed_at[s] > kStuckMs &&
            st.open_since[s] <= st.pressed_at[s]) {
            st.gave_up = true; st.why = "A Mateo screen did not respond; finish the exchange yourself"; return ACT_NONE;
        }
    }
    if (st.last_press_ms && now - st.last_press_ms < kGapMs) return ACT_NONE;
    // The newest screen sits on top: the gift screens, then the question, then the conversation.
    static const int order[SC_COUNT] = {SC_SEND, SC_OPEN, SC_ACCEPT, SC_DIALOG};
    for (int s : order) {
        if (!open[s]) continue;
        if (now - st.open_since[s] < kDwellMs[s]) return ACT_NONE;  // the top screen is still settling
        if (st.presses[s] >= kLimit[s]) {
            if (s == SC_DIALOG) { st.gave_up = true; st.why = "Mateo kept talking; finish the exchange yourself"; }
            return ACT_NONE;  // never press a screen below one already handled
        }
        st.presses[s]++; st.pressed_at[s] = now; st.last_press_ms = now;
        return static_cast<Action>(s);
    }
    return ACT_NONE;
}

}  // namespace pgr_route_gift

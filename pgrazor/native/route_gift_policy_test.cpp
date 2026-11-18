#include "route_gift_policy.h"

#include <cassert>
#include <cstdio>
#include <initializer_list>

using namespace pgr_route_gift;
namespace {
constexpr int GIFT = 2, TRADE = 3, YESNO = 5, END = 1;
Action tick(State &st, int64_t now, bool dialog, bool accept, bool send, bool open) {
    const bool screens[SC_COUNT] = {dialog, accept, send, open};
    return decide(st, true, screens, now);
}
}  // namespace

int main() {
    bool on = true;
    assert(parse_config("M1|1", 4, on) && on);
    assert(parse_config("M1|0", 4, on) && !on);
    for (const char *bad : {"", "M1|", "M1|2", "M2|1", "M1|10", "m1|1"}) {
        on = true;
        assert(!parse_config(bad, uint32_t(__builtin_strlen(bad)), on) && !on);
    }

    // Only an encounter with a gift exchange and no Pokemon trade is helped.
    const int gift[] = {0, YESNO, GIFT, END};
    const int trade[] = {0, YESNO, TRADE, END};
    const int both[] = {GIFT, TRADE};
    const int talk[] = {0, END};
    assert(classify(gift, 4, GIFT, TRADE) == ENC_GIFT);
    assert(classify(trade, 4, GIFT, TRADE) == ENC_OTHER);
    assert(classify(both, 2, GIFT, TRADE) == ENC_OTHER);
    assert(classify(talk, 2, GIFT, TRADE) == ENC_OTHER);
    assert(classify(nullptr, 0, GIFT, TRADE) == ENC_OTHER);

    // Nothing happens outside a gift encounter or while the setting is off.
    State st;
    assert(tick(st, 1000, true, false, false, false) == ACT_NONE);
    st.begin(1000);
    const bool dialog_only[SC_COUNT] = {true, false, false, false};
    assert(decide(st, false, dialog_only, 5000) == ACT_NONE);

    // A full exchange: conversation, question, gift card, Mateo's gift.
    st.begin(10000);
    assert(tick(st, 10000, true, false, false, false) == ACT_NONE);       // just opened
    assert(tick(st, 10699, true, false, false, false) == ACT_NONE);       // still settling
    assert(tick(st, 10700, true, false, false, false) == ACT_CONTINUE);
    assert(tick(st, 11000, true, false, false, false) == ACT_NONE);       // gap between presses
    assert(tick(st, 11500, true, false, false, false) == ACT_CONTINUE);
    // The question opens over the conversation: it is answered, never the conversation behind it.
    assert(tick(st, 12000, true, true, false, false) == ACT_NONE);
    assert(tick(st, 13200, true, true, false, false) == ACT_ACCEPT);
    assert(tick(st, 14100, true, true, false, false) == ACT_NONE);         // pressed once only
    assert(tick(st, 14200, false, false, true, false) == ACT_NONE);
    assert(tick(st, 15700, false, false, true, false) == ACT_SEND);
    assert(tick(st, 16600, false, false, true, false) == ACT_NONE);
    assert(tick(st, 16700, false, false, false, true) == ACT_NONE);
    assert(tick(st, 17900, false, false, false, true) == ACT_OPEN);
    assert(tick(st, 19000, true, false, false, false) == ACT_NONE);        // thanks line appears
    assert(tick(st, 19700, true, false, false, false) == ACT_CONTINUE);
    assert(!st.gave_up);

    // A one-time screen that stays open after its press is left to the player.
    st.begin(0);
    assert(tick(st, 0, false, false, true, false) == ACT_NONE);
    assert(tick(st, 1500, false, false, true, false) == ACT_SEND);
    assert(tick(st, 11500, false, false, true, false) == ACT_NONE && !st.gave_up);
    assert(tick(st, 11501, false, false, true, false) == ACT_NONE && st.gave_up);
    assert(tick(st, 20000, true, false, false, false) == ACT_NONE);

    // A conversation that never ends stops after the press limit.
    st.begin(0);
    int64_t t = 0;
    int presses = 0;
    for (int i = 0; i < 100; ++i, t += 800) presses += tick(st, t, true, false, false, false) == ACT_CONTINUE;
    assert(presses == kLimit[SC_DIALOG] && st.gave_up);

    // The whole encounter has a time limit.
    st.begin(0);
    assert(tick(st, kEncounterMs + 1, true, false, false, false) == ACT_NONE && st.gave_up);

    std::puts("route_gift_policy_test: OK");
    return 0;
}

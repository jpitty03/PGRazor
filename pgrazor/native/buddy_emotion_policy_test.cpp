#include "buddy_emotion_policy.h"
#include <cassert>
#include <cstring>
#include <iostream>

using namespace pgr_buddy_ep;

int main() {
    Settings s;
    assert(!s.enabled && s.berry == 703 && s.reserve == 50 && !s.gifts);
    assert(s.berries[0] == 703 && s.berries[1] == 701 && s.berries[2] == 705 &&
           s.berries[3] == 0 && s.berries[4] == 0);
    for (const char* good : {"1;1;703;50;0", "1;0;0;0;1", "1;1;701;1000;1", "1;1;705;1;0"})
    {
        assert(parse(good, std::strlen(good), s));
        assert(s.berries[0] == s.berry);
        for (size_t i = 1; i < 5; ++i) assert(s.berries[i] == 0);
        const int legacy_counts[5] = {1001, 1001, 1001, 1001, 1001};
        assert(select_berry(s, legacy_counts) == s.berry);
    }
    const char* ordered = "2;1;708;703;701;706;705;50;1";
    assert(parse(ordered, std::strlen(ordered), s));
    assert(s.enabled && s.berry == 708 && s.reserve == 50 && s.gifts);
    int counts[5] = {100, 100, 100, 100, 100};
    assert(select_berry(s, counts) == 708);
    counts[0] = 50;
    assert(select_berry(s, counts) == 703);
    counts[1] = 49;
    assert(select_berry(s, counts) == 701);
    counts[2] = 0;
    assert(select_berry(s, counts) == 706);
    counts[3] = 50;
    assert(select_berry(s, counts) == 705);
    counts[4] = 50;
    assert(select_berry(s, counts) == 0);
    const char* empty = "2;1;0;0;0;0;0;0;1";
    assert(parse(empty, std::strlen(empty), s));
    const int full_counts[5] = {100, 100, 100, 100, 100};
    assert(s.berry == 0 && select_berry(s, full_counts) == 0);
    const char* partial = "2;0;706;708;0;0;0;1000;0";
    assert(parse(partial, std::strlen(partial), s));
    assert(!s.enabled && s.berry == 706 && s.berries[1] == 708 && s.reserve == 1000 && !s.gifts);
    s = Settings{true, 703, 50, true};
    for (const char* bad : {"", "1;;703;50;0", "1;1;703;50;0;", "1;1;703;50;0x",
         "2;1;703;50;0", "01;1;703;50;0", "1;2;703;50;0", "1;1;703;50;2", "1;1;704;50;0",
         "1;1;703;-1;0", "1;1;703;1001;0", "1;1;703;18446744073709551616;0",
         "1;1;703; 50;0", "1;1;703;+50;0", "1;1;703;50",
         "1;1;706;50;0", "1;1;708;50;0", "2;1;703;703;0;0;0;50;0",
         "2;1;703;0;701;0;0;50;0", "2;1;0;701;0;0;0;50;0",
         "2;1;709;0;0;0;0;50;0", "2;1;704;0;0;0;0;50;0",
         "3;1;703;701;705;0;0;50;0", "02;1;703;701;705;0;0;50;0",
         "2;2;703;701;705;0;0;50;0", "2;1;703;701;705;0;0;1001;0",
         "2;1;703;701;705;0;0;50;2", "2;1;703;701;705;0;0;50;0;",
         "2;1;703;701;705;0;0;50", "2;1;703;701;705;0;0;-1;0"}) {
        assert(!parse(bad, std::strlen(bad), s));
        assert(s.enabled && s.berry == 703 && s.reserve == 50 && s.gifts);
        assert(s.berries[0] == 703 && s.berries[1] == 701 && s.berries[2] == 705 &&
               s.berries[3] == 0 && s.berries[4] == 0);
    }
    assert(!parse(nullptr, 10, s));
    std::string long_config(65, '0');
    assert(!parse(long_config.data(), long_config.size(), s));
    constexpr uint64_t minute = 60000;
    const uint64_t now = 100 * minute;
    History h;
    s.gifts = false;
    assert(choose(s, h, now, true, 100, true) == Snapshot);
    mark_attempt(h, Snapshot, now);
    assert(h.interaction == 0);
    assert(choose(s, h, now, true, 100, true) == Pet);
    mark_attempt(h, Pet, now);
    assert(choose(s, h, now, true, 50, false) == None);
    assert(choose(s, h, now, true, 51, false) == Feed);
    assert(choose(s, h, now, false, 51, false) == None);
    s.berry = 0;
    assert(choose(s, h, now, true, 51, false) == None);
    s.berry = 703;
    mark_attempt(h, Feed, now);
    assert(choose(s, h, now + 30 * minute - 1, true, 100, false) == None);
    assert(choose(s, h, now + 30 * minute, true, 100, false) == Snapshot);
    s.gifts = true;
    assert(choose(s, h, now, true, 100, true) == Gift);
    mark_attempt(h, Gift, now);
    assert(choose(s, h, now + 30 * minute - 1, true, 100, true) == None);
    assert(choose(s, h, now + 30 * minute, true, 100, true) == Gift);
    s.enabled = false;
    assert(choose(s, History{}, now, true, 100, true) == None);
    s.enabled = true;
    mark_success(h, now);
    assert(choose(s, h, now + 20 * minute - 1, true, 100, false) == None);
    assert(choose(s, h, now + 20 * minute, true, 100, false) == Pet);
    mark_attempt(h, Pet, now + 20 * minute);
    assert(choose(s, h, now + 20 * minute, true, 100, false) == Snapshot);
    mark_attempt(h, Snapshot, now + 20 * minute);
    assert(choose(s, h, now + 20 * minute, true, 100, false) == None);
    for (auto& time : h.action) time = now + 1;
    h.interaction = now + 1;
    assert(choose(s, h, now, true, 100, true) == None);
    mark_attempt(h, None, 1);
    assert(h.action[0] == now + 1);
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    History largest{max, {max, max, max, max}, max}, restored;
    assert(parse_history(encode(largest), restored));
    assert(encode(restored) == encode(largest));
    for (const std::string bad : {"", "1;2;3;4;5", "1;2;3;4;5;6;", "1;2;3;4;5;6x",
         "1;2;3;4;5;-6", "1;2;3;4;5;18446744073709551616", "1;;3;4;5;6"}) {
        assert(!parse_history(bad, restored));
        assert(encode(restored) == encode(largest));
    }
    assert(!parse_history(std::string(257, '0'), restored));
    assert(parse_history("0;0;0;0;0;0", restored));
    std::cout << "buddy emotion policy tests passed\n";
}

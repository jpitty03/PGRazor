// Deterministic unit tests for pgrazor/native/auto_raid_policy.h
//
// Standalone, dependency-free C++17. Build & run (coordinator does this):
//   g++ -std=c++17 -O2 -Wall -Wextra -o art_test auto_raid_policy_test.cpp && ./art_test
// Exit code 0 = all checks passed, 1 = at least one check failed.
//
// ---------------------------------------------------------------------------
// PROTOCOL / HEADER ASSUMPTIONS (updated contract supplied by the coordinator)
// ---------------------------------------------------------------------------
// These tests target the *updated* slot-12 R1 wire protocol with 13 fields:
//     R1|token|enabled|tierMask|maxRaids|minPlayers|waitSeconds|walk|
//        freePass|premiumPass|stopOnShiny|bosses|cooldownMs
//   * cooldownMs is an integer in [0, 7200000].
//   * Max wire size is 1200 bytes.
//   * Max boss text is 1000 bytes / 200 IDs.
//   * parse_command must populate a new field `Command::cooldown_ms`.
//
// The header snapshot read when these tests were written still parsed the OLD
// 12-field protocol (max wire 600, boss text 500, no cooldown). Therefore:
//   * The parsing tests below will NOT compile/pass until the coordinator
//     updates parse_command + adds Command::cooldown_ms.  This is intended:
//     the tests encode the target contract.
//   * Tests marked "CONTRACT" encode expected behavior the current header does
//     not yet satisfy (see auto_raid_policy_test findings in the handoff). They
//     are expected to FAIL until the header is fixed. Concrete bugs are NOT
//     worked around here; they are reported to the coordinator instead.
//
// All other helper APIs (classify_*, is_raid_candidate_eligible, pass
// selection, lobby, SessionTracker) are treated as stable and tested against
// real scenarios rather than by asserting internal constants.

#include "auto_raid_policy.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace pgr_auto_raid;

// ----------------------------- tiny harness --------------------------------
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

#define CHECK_MSG(cond, msg)                                                   \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s | %s\n", __FILE__, __LINE__,  \
                         #cond, (msg));                                        \
        }                                                                      \
    } while (0)

// ----------------------------- wire builder --------------------------------
// Aggregate with sane defaults; tweak one field then call str() to produce a
// malformed/boundary variant.
struct Wire {
    std::string token    = "1";
    std::string enabled  = "1";
    std::string tier     = "0";
    std::string maxr     = "1";
    std::string minp     = "1";
    std::string wait     = "15";
    std::string walk     = "0";
    std::string freep    = "1";
    std::string prem     = "0";
    std::string shiny    = "0";
    std::string bosses   = "";
    std::string cooldown = "0";

    std::string str() const {
        return "R1|" + token + "|" + enabled + "|" + tier + "|" + maxr + "|" +
               minp + "|" + wait + "|" + walk + "|" + freep + "|" + prem + "|" +
               shiny + "|" + bosses + "|" + cooldown;
    }
};

static bool parse(const std::string& s, Command& out) {
    return parse_command(s.data(), s.size(), out);
}

// =====================================================================
// 1. Parsing: valid baselines
// =====================================================================
static void test_parse_valid() {
    Command c;

    // Minimal valid "run" command.
    CHECK(parse(Wire{}.str(), c));
    CHECK(c.token == 1);
    CHECK(c.enabled == true);
    CHECK(c.tier_mask == 0);
    CHECK(c.max_raids == 1);
    CHECK(c.min_players == 1);
    CHECK(c.wait_seconds == 15);
    CHECK(c.walk == false);
    CHECK(c.free_pass == true);
    CHECK(c.premium_pass == false);
    CHECK(c.stop_on_shiny == false);
    CHECK(c.boss_ids.empty());
    CHECK(c.cooldown_ms == 0);  // updated protocol field

    // Fully populated command at the extreme ends of every bound.
    Wire w;
    w.token = "9223372036854775807";  // INT64_MAX (inclusive upper bound)
    w.tier = "127";
    w.maxr = "100";
    w.minp = "20";
    w.wait = "120";
    w.walk = "1";
    w.freep = "1";
    w.prem = "1";
    w.shiny = "1";
    w.bosses = "3,1,2";       // unsorted + will be sorted/deduped
    w.cooldown = "7200000";   // max cooldown
    CHECK(parse(w.str(), c));
    CHECK(c.token == static_cast<uint64_t>(INT64_MAX));
    CHECK(c.tier_mask == 127);
    CHECK(c.max_raids == 100);
    CHECK(c.min_players == 20);
    CHECK(c.wait_seconds == 120);
    CHECK(c.walk == true);
    CHECK(c.free_pass == true);
    CHECK(c.premium_pass == true);
    CHECK(c.stop_on_shiny == true);
    CHECK(c.cooldown_ms == 7200000);
    CHECK((c.boss_ids == std::vector<int>{1, 2, 3}));

    // Boss list dedup + sort.
    w.bosses = "3,3,1,3";
    CHECK(parse(w.str(), c));
    CHECK((c.boss_ids == std::vector<int>{1, 3}));
}

// =====================================================================
// 2. Parsing: malformed structure
// =====================================================================
static void test_parse_malformed() {
    Command c;

    // Null / too short / too-long-but-garbage.
    CHECK(!parse_command(nullptr, 40, c));
    CHECK(!parse("R1|1", c));                 // far too short
    CHECK(!parse("", c));

    // Wrong magic prefix.
    CHECK(!parse("R2|1|1|0|1|1|15|0|1|0|0||0", c));
    CHECK(!parse("X1|1|1|0|1|1|15|0|1|0|0||0", c));
    CHECK(!parse("r1|1|1|0|1|1|15|0|1|0|0||0", c));

    // Wrong field count. Old 12-field layout (no cooldown) must now be rejected.
    CHECK(!parse("R1|1|1|0|1|1|15|0|1|0|0|", c));      // 12 segments
    CHECK(!parse("R1|1|1|0|1|1|15|0|1|0|0|1,2", c));   // 12 segments, bosses set
    {
        std::string too_many = Wire{}.str() + "|5";   // 14 segments
        CHECK(!parse(too_many, c));
    }

    // Non-numeric / empty numeric segments.
    {
        Wire w; w.token = "";    CHECK(!parse(w.str(), c)); // empty token
    }
    {
        Wire w; w.token = "12a"; CHECK(!parse(w.str(), c)); // trailing non-digit
    }
    {
        Wire w; w.token = "0";   CHECK(!parse(w.str(), c)); // token must be >= 1
    }
    {
        Wire w; w.wait = "4x";   CHECK(!parse(w.str(), c));
    }
    {
        Wire w; w.cooldown = ""; CHECK(!parse(w.str(), c)); // empty cooldown segment
    }
    {
        Wire w; w.tier = "-1";   CHECK(!parse(w.str(), c)); // sign not allowed
    }
}

// =====================================================================
// 3. Parsing: numeric overflow (per-segment, not just token)
// =====================================================================
static void test_parse_overflow() {
    Command c;
    {
        Wire w; w.token = "9223372036854775808"; // INT64_MAX + 1
        CHECK(!parse(w.str(), c));
    }
    {
        Wire w; w.token = "99999999999999999999999999"; // >> UINT64
        CHECK(!parse(w.str(), c));
    }
    {
        Wire w; w.cooldown = "99999999999999999999"; // overflow in cooldown segment
        CHECK(!parse(w.str(), c));
    }
    {
        Wire w; w.tier = "18446744073709551616"; // UINT64_MAX + 1
        CHECK(!parse(w.str(), c));
    }
}

// =====================================================================
// 4. Parsing: numeric bounds
// =====================================================================
static void test_parse_bounds() {
    Command c;

    auto reject = [&](Wire w) { CHECK(!parse(w.str(), c)); };
    auto accept = [&](Wire w) { CHECK(parse(w.str(), c)); };

    // enabled 0/1 only.
    { Wire w; w.enabled = "2"; reject(w); }

    // tier_mask 0..127.
    { Wire w; w.tier = "128"; reject(w); }
    { Wire w; w.tier = "127"; accept(w); }

    // max_raids 1..100.
    { Wire w; w.maxr = "0";   reject(w); }
    { Wire w; w.maxr = "101"; reject(w); }
    { Wire w; w.maxr = "100"; accept(w); }

    // min_players 1..20.
    { Wire w; w.minp = "0";  reject(w); }
    { Wire w; w.minp = "21"; reject(w); }
    { Wire w; w.minp = "20"; accept(w); }

    // wait_seconds 15..120.
    { Wire w; w.wait = "14";  reject(w); }
    { Wire w; w.wait = "121"; reject(w); }
    { Wire w; w.wait = "15";  accept(w); }
    { Wire w; w.wait = "120"; accept(w); }

    // boolean flags are 0/1 only.
    { Wire w; w.walk  = "2"; reject(w); }
    { Wire w; w.freep = "2"; reject(w); }
    { Wire w; w.prem  = "2"; reject(w); }
    { Wire w; w.shiny = "2"; reject(w); }

    // cooldown 0..7200000.
    { Wire w; w.cooldown = "7200001"; reject(w); }
    { Wire w; w.cooldown = "7200000"; accept(w); }
    { Wire w; w.cooldown = "0";       accept(w); }
}

// =====================================================================
// 5. Parsing: stop commands + pass-availability rule
// =====================================================================
static void test_parse_stop_command() {
    Command c;

    // A stop command (enabled=0) does NOT require any pass to be allowed.
    {
        Wire w; w.enabled = "0"; w.freep = "0"; w.prem = "0";
        CHECK(parse(w.str(), c));
        CHECK(c.enabled == false);
        CHECK(c.free_pass == false);
        CHECK(c.premium_pass == false);
    }

    // A run command (enabled=1) with no pass enabled is invalid.
    {
        Wire w; w.enabled = "1"; w.freep = "0"; w.prem = "0";
        CHECK(!parse(w.str(), c));
    }

    // Run command with premium-only is valid.
    {
        Wire w; w.enabled = "1"; w.freep = "0"; w.prem = "1";
        CHECK(parse(w.str(), c));
        CHECK(c.enabled == true);
        CHECK(c.free_pass == false);
        CHECK(c.premium_pass == true);
    }
}

// =====================================================================
// 6. Parsing: boss list + wire/length caps (updated 1000B/200ID, 1200 wire)
// =====================================================================
static void test_parse_boss_and_limits() {
    Command c;

    // Structural boss-list errors.
    auto bad_boss = [&](const std::string& b) {
        Wire w; w.bosses = b; CHECK_MSG(!parse(w.str(), c), b.c_str());
    };
    bad_boss(",1");      // leading comma
    bad_boss("1,");      // trailing comma
    bad_boss("1,,2");    // double comma
    bad_boss("01");      // leading zero
    bad_boss("1,0,2");   // id 0 out of range (also leading zero)
    bad_boss("0");       // id 0
    bad_boss("2001");    // id > 2000
    bad_boss("1,a,2");   // non-digit
    bad_boss("1, 2");    // embedded space

    // 200 IDs -> accepted. Boss text here is ~691 bytes and whole wire ~715
    // bytes: both exceed the OLD caps (500 boss / 600 wire) and so this test
    // only passes under the updated 1000/1200 limits.
    {
        std::string bl;
        for (int i = 1; i <= 200; ++i) {
            if (i > 1) bl += ',';
            bl += std::to_string(i);
        }
        Wire w; w.bosses = bl;
        CHECK(parse(w.str(), c));
        CHECK(c.boss_ids.size() == 200);
        CHECK(c.boss_ids.front() == 1);
        CHECK(c.boss_ids.back() == 200);
    }

    // 201 IDs -> rejected (exceeds 200-ID cap; stays under the 1000-byte cap).
    {
        std::string bl;
        for (int i = 1; i <= 201; ++i) {
            if (i > 1) bl += ',';
            bl += std::to_string(i);
        }
        Wire w; w.bosses = bl;
        CHECK(!parse(w.str(), c));
    }

    // Boss text > 1000 bytes -> rejected by the boss-length cap (wire still
    // under 1200 so the top-level size guard is not what trips here).
    {
        Wire w; w.bosses = std::string(1001, '1');
        CHECK(w.str().size() <= 1200);
        CHECK(!parse(w.str(), c));
    }

    // Whole wire well over 1200 bytes -> rejected by the size guard.
    {
        Wire w; w.bosses = std::string(1300, '1');
        CHECK(w.str().size() > 1200);
        CHECK(!parse(w.str(), c));
    }
}

// =====================================================================
// 7. Tier classification
// =====================================================================
static void test_classify_tier() {
    // Standard star tiers.
    CHECK(classify_raid_tier_mask(RAID_LEVEL_1) == TIER_1_STAR);
    CHECK(classify_raid_tier_mask(RAID_LEVEL_2) == TIER_1_STAR);
    CHECK(classify_raid_tier_mask(RAID_LEVEL_3) == TIER_3_STAR);
    CHECK(classify_raid_tier_mask(RAID_LEVEL_4) == TIER_3_STAR);
    CHECK(classify_raid_tier_mask(RAID_LEVEL_5) == TIER_5_STAR);
    CHECK(classify_raid_tier_mask(RAID_LEVEL_ELITE) == TIER_5_STAR);

    // Mega (level + flags).
    CHECK(classify_raid_tier_mask(RAID_LEVEL_MEGA) == TIER_MEGA);
    CHECK(classify_raid_tier_mask(RAID_LEVEL_LEGENDARY_MEGA) == TIER_MEGA);
    CHECK(classify_raid_tier_mask(RAID_LEVEL_1, /*mega*/ true) == TIER_MEGA);
    CHECK(classify_raid_tier_mask(RAID_LEVEL_1, false, /*legmega*/ true) == TIER_MEGA);

    // Primal / Shadow / Ultra Beast.
    CHECK(classify_raid_tier_mask(RAID_LEVEL_PRIMAL) == TIER_PRIMAL);
    CHECK(classify_raid_tier_mask(RAID_LEVEL_UNSET, false, false, /*primal*/ true) == TIER_PRIMAL);
    CHECK(classify_raid_tier_mask(RAID_LEVEL_SHADOW_1) == TIER_SHADOW);
    CHECK(classify_raid_tier_mask(RAID_LEVEL_SHADOW_5) == TIER_SHADOW);
    CHECK(classify_raid_tier_mask(RAID_LEVEL_UNSET, false, false, false, /*shadow*/ true) == TIER_SHADOW);
    CHECK(classify_raid_tier_mask(RAID_LEVEL_ULTRA_BEAST) == TIER_ULTRA_BEAST);

    // A special flag suppresses the standard star bit (mutual exclusivity).
    CHECK(classify_raid_tier_mask(RAID_LEVEL_5, /*mega*/ true) == TIER_MEGA);
    CHECK((classify_raid_tier_mask(RAID_LEVEL_3, false, false, /*primal*/ true) & TIER_3_STAR) == 0);

    // Non-eligible / unknown levels classify to 0.
    CHECK(classify_raid_tier_mask(RAID_LEVEL_EX) == 0);     // invite-only
    CHECK(classify_raid_tier_mask(RAID_LEVEL_UNSET) == 0);
    CHECK(classify_raid_tier_mask(99) == 0);
}

// =====================================================================
// 8. Tier + boss filter matching
// =====================================================================
static void test_matches_filters() {
    Command cmd;  // tier ANY, no boss filter

    // ANY tier accepts any classifiable level with a valid boss.
    CHECK(matches_raid_filters(cmd, RAID_LEVEL_5, 100));
    // EX / unclassifiable rejected regardless of filter.
    CHECK(!matches_raid_filters(cmd, RAID_LEVEL_EX, 100));
    // Boss id out of range rejected.
    CHECK(!matches_raid_filters(cmd, RAID_LEVEL_5, 0));
    CHECK(!matches_raid_filters(cmd, RAID_LEVEL_5, 2001));

    // Tier mask filtering.
    cmd.tier_mask = TIER_3_STAR;
    CHECK(matches_raid_filters(cmd, RAID_LEVEL_3, 100));   // 3-star matches
    CHECK(!matches_raid_filters(cmd, RAID_LEVEL_5, 100));  // 5-star excluded
    cmd.tier_mask = TIER_1_STAR | TIER_5_STAR;
    CHECK(matches_raid_filters(cmd, RAID_LEVEL_1, 100));
    CHECK(matches_raid_filters(cmd, RAID_LEVEL_5, 100));
    CHECK(!matches_raid_filters(cmd, RAID_LEVEL_3, 100));

    // Boss id allow-list (must be sorted for binary_search, as parse guarantees).
    cmd.tier_mask = TIER_ANY;
    cmd.boss_ids = {25, 150};
    CHECK(matches_raid_filters(cmd, RAID_LEVEL_5, 25));
    CHECK(matches_raid_filters(cmd, RAID_LEVEL_5, 150));
    CHECK(!matches_raid_filters(cmd, RAID_LEVEL_5, 26));
}

// =====================================================================
// 9. Active raid candidate eligibility (egg/expired/completed/hidden/...)
// =====================================================================
static RaidCandidate baseline_candidate() {
    RaidCandidate c;
    c.fort_id = "FORT_A";
    c.raid_seed = 777;
    c.latitude = 40.0;
    c.longitude = -75.0;
    c.has_active_raid = true;
    c.is_raid_boss_active = true;
    c.is_raid_egg_active = false;
    c.is_complete = false;
    c.is_raid_hidden = false;
    c.spawn_ms = 1000;
    c.battle_ms = 2000;
    c.end_ms = 10'000'000;
    c.raid_level = RAID_LEVEL_5;
    c.boss_pokemon_id = 100;
    return c;
}

static void test_candidate_eligibility() {
    const Command cmd;          // ANY tier, no boss filter
    const int64_t now = 5000;   // after spawn+battle, well before end

    CHECK(is_raid_candidate_eligible(baseline_candidate(), cmd, now));

    auto ineligible = [&](RaidCandidate c, const char* why) {
        CHECK_MSG(!is_raid_candidate_eligible(c, cmd, now), why);
    };

    { auto c = baseline_candidate(); c.fort_id.clear();            ineligible(c, "empty fort"); }
    { auto c = baseline_candidate(); c.raid_seed = 0;              ineligible(c, "zero seed"); }
    { auto c = baseline_candidate(); c.latitude = 0.0; c.longitude = 0.0; ineligible(c, "null island"); }
    { auto c = baseline_candidate(); c.latitude = 91.0;           ineligible(c, "lat out of range"); }
    { auto c = baseline_candidate(); c.has_active_raid = false;    ineligible(c, "no active raid"); }
    { auto c = baseline_candidate(); c.is_raid_boss_active = false; ineligible(c, "boss not active"); }
    { auto c = baseline_candidate(); c.is_raid_egg_active = true;  ineligible(c, "egg still active"); }
    { auto c = baseline_candidate(); c.is_complete = true;         ineligible(c, "already complete"); }
    { auto c = baseline_candidate(); c.is_raid_hidden = true;      ineligible(c, "hidden raid"); }
    { auto c = baseline_candidate(); c.battle_ms = 0;              ineligible(c, "no battle time"); }
    { auto c = baseline_candidate(); c.end_ms = c.battle_ms;       ineligible(c, "end <= battle"); }
    { auto c = baseline_candidate(); c.spawn_ms = now + 1;         ineligible(c, "not yet spawned"); }
    { auto c = baseline_candidate(); c.battle_ms = now + 1;        ineligible(c, "egg not hatched"); }
    { auto c = baseline_candidate(); c.end_ms = now + 10;          ineligible(c, "expiring within floor"); }
    { auto c = baseline_candidate(); c.raid_level = RAID_LEVEL_EX; ineligible(c, "EX raid"); }
    { auto c = baseline_candidate(); c.boss_pokemon_id = 3000;     ineligible(c, "boss out of range"); }

    // now_ms must be positive.
    CHECK(!is_raid_candidate_eligible(baseline_candidate(), cmd, 0));

    // Tier filter mismatch makes an otherwise-fine candidate ineligible.
    { Command f; f.tier_mask = TIER_1_STAR;
      CHECK(!is_raid_candidate_eligible(baseline_candidate(), f, now)); } // candidate is 5-star

    // Expiry is governed by min_remaining_ms; a long buffer rejects a soon-to-end raid.
    { auto c = baseline_candidate(); c.end_ms = now + 20'000;
      CHECK(is_raid_candidate_eligible(c, cmd, now, /*min_remaining*/ 1000));
      CHECK(!is_raid_candidate_eligible(c, cmd, now, /*min_remaining*/ 30'000)); }
}

// =====================================================================
// 10. Nearest selection with exclusion + tie-break
// =====================================================================
static RaidCandidate at(std::string fort, int64_t seed, double lat, double lon) {
    auto c = baseline_candidate();
    c.fort_id = std::move(fort);
    c.raid_seed = seed;
    c.latitude = lat;
    c.longitude = lon;
    return c;
}

static void test_select_nearest() {
    const Command cmd;
    const int64_t now = 5000;
    const double plat = 40.0, plon = -75.0;

    // A ~85 m, C ~111 m, B ~853 m from the player.
    std::vector<RaidCandidate> cands = {
        at("FORT_A", 1, 40.0,    -75.0010),  // index 0, nearest
        at("FORT_B", 2, 40.0,    -75.0100),  // index 1, farthest
        at("FORT_C", 3, 40.0010, -75.0),     // index 2, middle
    };

    {
        SessionTracker s;
        double dist = -1.0;
        int idx = select_nearest_raid(cands, cmd, s, plat, plon, now, &dist);
        CHECK(idx == 0);
        CHECK(dist > 0.0 && dist < 150.0);
    }

    // Exclude the nearest via completed_keys -> middle candidate wins.
    {
        SessionTracker s;
        s.completed_keys.insert(make_raid_key("FORT_A", 1));
        int idx = select_nearest_raid(cands, cmd, s, plat, plon, now);
        CHECK(idx == 2);
    }

    // Exclude the nearest via quarantine -> middle candidate wins.
    {
        SessionTracker s;
        s.quarantine_raid(make_raid_key("FORT_A", 1), now, /*dur*/ 60'000);
        int idx = select_nearest_raid(cands, cmd, s, plat, plon, now);
        CHECK(idx == 2);
    }

    // No eligible candidates -> -1 (all marked complete).
    {
        SessionTracker s;
        auto blocked = cands;
        for (auto& c : blocked) c.is_complete = true;
        CHECK(select_nearest_raid(blocked, cmd, s, plat, plon, now) == -1);
    }

    // Invalid player coordinates -> -1.
    {
        SessionTracker s;
        CHECK(select_nearest_raid(cands, cmd, s, 0.0, 0.0, now) == -1);
    }

    // Tie-break: two equidistant candidates -> lexicographically smaller key.
    {
        SessionTracker s;
        std::vector<RaidCandidate> tie = {
            at("FORT_E", 9, 40.0, -74.9990),  // index 0, same distance
            at("FORT_D", 9, 40.0, -75.0010),  // index 1, smaller key
        };
        int idx = select_nearest_raid(tie, cmd, s, plat, plon, now);
        CHECK(idx == 1);  // "FORT_D#9" < "FORT_E#9"
    }
}

// =====================================================================
// 11. Entry cost classification (remote / coin / multi-item refused)
// =====================================================================
static EntryCostOption cost(int item_id, int count, bool satisfied,
                            int raid_type = RAID_LOCATION_LOCAL) {
    EntryCostOption o;
    o.raid_type = raid_type;
    o.items = {{item_id, count}};
    o.satisfied_in_bag = satisfied;
    return o;
}

static void test_classify_entry_cost() {
    // Coin flag always wins.
    CHECK(classify_entry_cost(cost(ITEM_FREE_RAID_TICKET, 1, true), false, /*coin*/ true)
          == PassCostKind::ForbiddenShopCoin);

    // Remote raid-type.
    {
        EntryCostOption o = cost(ITEM_FREE_RAID_TICKET, 1, true, RAID_LOCATION_REMOTE);
        CHECK(classify_entry_cost(o) == PassCostKind::ForbiddenRemote);
    }
    // Ambiguous raid-type.
    {
        EntryCostOption o = cost(ITEM_FREE_RAID_TICKET, 1, true, /*bad*/ 5);
        CHECK(classify_entry_cost(o) == PassCostKind::InvalidOrAmbiguous);
    }

    // Single-item tickets.
    CHECK(classify_entry_cost(cost(ITEM_FREE_RAID_TICKET, 1, true)) == PassCostKind::FreeLocal);
    CHECK(classify_entry_cost(cost(ITEM_PAID_RAID_TICKET, 1, true)) == PassCostKind::PremiumLocal);
    CHECK(classify_entry_cost(cost(ITEM_REMOTE_RAID_TICKET, 1, true)) == PassCostKind::ForbiddenRemote);
    CHECK(classify_entry_cost(cost(ITEM_COIN_RAID_TICKET, 1, true)) == PassCostKind::ForbiddenShopCoin);
    CHECK(classify_entry_cost(cost(ITEM_LEGENDARY_RAID_TICKET, 1, true)) == PassCostKind::UnsupportedOther);
    CHECK(classify_entry_cost(cost(ITEM_EVENT_RAID_TICKET, 1, true)) == PassCostKind::UnsupportedOther);
    CHECK(classify_entry_cost(cost(9999, 1, true)) == PassCostKind::InvalidOrAmbiguous);

    // count out of [0,1] is ambiguous.
    CHECK(classify_entry_cost(cost(ITEM_FREE_RAID_TICKET, 2, true)) == PassCostKind::InvalidOrAmbiguous);
    CHECK(classify_entry_cost(cost(ITEM_FREE_RAID_TICKET, -1, true)) == PassCostKind::InvalidOrAmbiguous);

    // count == 0 is free only when the raid itself is free.
    CHECK(classify_entry_cost(cost(ITEM_FREE_RAID_TICKET, 0, true), /*free*/ true) == PassCostKind::FreeLocal);
    CHECK(classify_entry_cost(cost(ITEM_FREE_RAID_TICKET, 0, true), /*free*/ false) == PassCostKind::InvalidOrAmbiguous);

    // Empty items: free when raid is free, else ambiguous.
    {
        EntryCostOption o; o.raid_type = RAID_LOCATION_LOCAL; o.satisfied_in_bag = true;
        CHECK(classify_entry_cost(o, /*free*/ true) == PassCostKind::FreeLocal);
        CHECK(classify_entry_cost(o, /*free*/ false) == PassCostKind::InvalidOrAmbiguous);
    }

    // Multi-item bundles are refused; remote/coin reported most-specifically.
    {
        EntryCostOption o; o.raid_type = RAID_LOCATION_LOCAL; o.satisfied_in_bag = true;
        o.items = {{ITEM_FREE_RAID_TICKET, 1}, {ITEM_PAID_RAID_TICKET, 1}};
        CHECK(classify_entry_cost(o) == PassCostKind::InvalidOrAmbiguous);

        o.items = {{ITEM_FREE_RAID_TICKET, 1}, {ITEM_REMOTE_RAID_TICKET, 1}};
        CHECK(classify_entry_cost(o) == PassCostKind::ForbiddenRemote);

        o.items = {{ITEM_FREE_RAID_TICKET, 1}, {ITEM_COIN_RAID_TICKET, 1}};
        CHECK(classify_entry_cost(o) == PassCostKind::ForbiddenShopCoin);
    }
}

// =====================================================================
// 12. Pass selection: free-pass preference vs explicit premium, refusals
// =====================================================================
static void test_pass_selection() {
    Command cmd; cmd.free_pass = true; cmd.premium_pass = false;

    // Remote ticket already used short-circuits to forbidden-remote.
    {
        auto r = evaluate_pass_selection(cmd, /*used*/ false, /*remote_used*/ true,
                                         /*free_raid*/ false, {cost(ITEM_FREE_RAID_TICKET, 1, true)});
        CHECK(r.status == PassDecisionStatus::ForbiddenRemotePass);
    }

    // Ticket already locally consumed -> rejoin is free.
    {
        auto r = evaluate_pass_selection(cmd, /*used*/ true, false, false, {});
        CHECK(r.status == PassDecisionStatus::RejoinAlreadyPaid);
    }

    // Empty options but a free raid + free pass enabled -> select free (index 0).
    {
        auto r = evaluate_pass_selection(cmd, false, false, /*free_raid*/ true, {});
        CHECK(r.status == PassDecisionStatus::SelectFreePass);
        CHECK(r.selected_option_index == -1); // Empty options have no valid index.
    }
    // Empty options, not a free raid -> unsupported.
    {
        auto r = evaluate_pass_selection(cmd, false, false, false, {});
        CHECK(r.status == PassDecisionStatus::UnsupportedOrAmbiguous);
    }

    // Free pass is ALWAYS preferred, even when a premium option is listed first.
    {
        std::vector<EntryCostOption> opts = {
            cost(ITEM_PAID_RAID_TICKET, 1, true),   // index 0
            cost(ITEM_FREE_RAID_TICKET, 1, true),   // index 1
        };
        auto r = evaluate_pass_selection(cmd, false, false, false, opts);
        CHECK(r.status == PassDecisionStatus::SelectFreePass);
        CHECK(r.selected_option_index == 1);
        CHECK(r.expected_cost_kind == PassCostKind::FreeLocal);
    }

    // free_pass disabled + premium enabled: skip the satisfied free, take premium.
    {
        Command c2; c2.free_pass = false; c2.premium_pass = true;
        std::vector<EntryCostOption> opts = {
            cost(ITEM_FREE_RAID_TICKET, 1, true),   // index 0 (disabled -> skip)
            cost(ITEM_PAID_RAID_TICKET, 1, true),   // index 1
        };
        auto r = evaluate_pass_selection(c2, false, false, false, opts);
        CHECK(r.status == PassDecisionStatus::SelectPremiumPass);
        CHECK(r.selected_option_index == 1);
    }

    // free_pass disabled + premium disabled, only free satisfied -> none allowed.
    {
        Command c2; c2.free_pass = false; c2.premium_pass = false;
        auto r = evaluate_pass_selection(c2, false, false, false, {cost(ITEM_FREE_RAID_TICKET, 1, true)});
        CHECK(r.status == PassDecisionStatus::NoAllowedPassAvailable);
    }

    // No free satisfied, premium satisfied + enabled -> premium.
    {
        Command c2; c2.free_pass = true; c2.premium_pass = true;
        auto r = evaluate_pass_selection(c2, false, false, false, {cost(ITEM_PAID_RAID_TICKET, 1, true)});
        CHECK(r.status == PassDecisionStatus::SelectPremiumPass);
    }
    // Premium satisfied but premium disabled -> none allowed.
    {
        Command c2; c2.free_pass = true; c2.premium_pass = false;
        auto r = evaluate_pass_selection(c2, false, false, false, {cost(ITEM_PAID_RAID_TICKET, 1, true)});
        CHECK(r.status == PassDecisionStatus::NoAllowedPassAvailable);
    }

    // A satisfiable pass exists in principle but is not in the bag -> none available.
    {
        auto r = evaluate_pass_selection(cmd, false, false, false, {cost(ITEM_FREE_RAID_TICKET, 1, /*satisfied*/ false)});
        CHECK(r.status == PassDecisionStatus::NoAllowedPassAvailable);
    }

    // Only a remote option -> forbidden remote.
    {
        auto r = evaluate_pass_selection(cmd, false, false, false, {cost(ITEM_REMOTE_RAID_TICKET, 1, true)});
        CHECK(r.status == PassDecisionStatus::ForbiddenRemotePass);
    }
    // Only a coin option -> forbidden shop.
    {
        auto r = evaluate_pass_selection(cmd, false, false, false, {cost(ITEM_COIN_RAID_TICKET, 1, true)});
        CHECK(r.status == PassDecisionStatus::ForbiddenShopPurchase);
    }
    // Shop + remote present -> shop reported first.
    {
        std::vector<EntryCostOption> opts = {
            cost(ITEM_COIN_RAID_TICKET, 1, true),
            cost(ITEM_REMOTE_RAID_TICKET, 1, true),
        };
        auto r = evaluate_pass_selection(cmd, false, false, false, opts);
        CHECK(r.status == PassDecisionStatus::ForbiddenShopPurchase);
    }
}

// =====================================================================
// 13. Modal cost verification before ticket consumption
// =====================================================================
static void test_verify_modal() {
    Command cmd; cmd.free_pass = true; cmd.premium_pass = true;

    // Matching free cost, in bag, free pass allowed -> true.
    CHECK(verify_modal_selected_cost(cmd, PassCostKind::FreeLocal,
                                     cost(ITEM_FREE_RAID_TICKET, 1, true), false, false));
    // Matching premium cost -> true when premium enabled.
    CHECK(verify_modal_selected_cost(cmd, PassCostKind::PremiumLocal,
                                     cost(ITEM_PAID_RAID_TICKET, 1, true), false, false));

    // Coin flag -> never.
    CHECK(!verify_modal_selected_cost(cmd, PassCostKind::FreeLocal,
                                      cost(ITEM_FREE_RAID_TICKET, 1, true), false, /*coin*/ true));
    // Not in bag -> never.
    CHECK(!verify_modal_selected_cost(cmd, PassCostKind::FreeLocal,
                                      cost(ITEM_FREE_RAID_TICKET, 1, /*satisfied*/ false), false, false));
    // Expected/actual mismatch -> false.
    CHECK(!verify_modal_selected_cost(cmd, PassCostKind::FreeLocal,
                                      cost(ITEM_PAID_RAID_TICKET, 1, true), false, false));

    // Pass-type disabled by command -> false even when the modal cost matches.
    {
        Command nofree; nofree.free_pass = false; nofree.premium_pass = true;
        CHECK(!verify_modal_selected_cost(nofree, PassCostKind::FreeLocal,
                                          cost(ITEM_FREE_RAID_TICKET, 1, true), false, false));
        Command noprem; noprem.free_pass = true; noprem.premium_pass = false;
        CHECK(!verify_modal_selected_cost(noprem, PassCostKind::PremiumLocal,
                                          cost(ITEM_PAID_RAID_TICKET, 1, true), false, false));
    }
}

// =====================================================================
// 14. Lobby bailout / ready decisions
// =====================================================================
static void test_lobby() {
    Command cmd; cmd.min_players = 3; cmd.wait_seconds = 45;  // 45s deadline
    const int64_t deadline_ms = 45'000;

    // Battle started trumps everything.
    CHECK(evaluate_lobby_decision(cmd, /*stop*/ true, /*battle*/ true, 1, 99'999, true, 0)
          == LobbyDecision::EnterBattle);

    // Stop requested before battle.
    CHECK(evaluate_lobby_decision(cmd, true, false, 5, 1000, false, 0)
          == LobbyDecision::LeaveStopRequested);

    // Enough players -> stay ready.
    CHECK(evaluate_lobby_decision(cmd, false, false, 3, 1000, false, 0)
          == LobbyDecision::StayReady);
    CHECK(evaluate_lobby_decision(cmd, false, false, 4, 1000, true, 30'000)
          == LobbyDecision::StayReady);

    // Under min, still within deadline and countdown floor -> keep waiting.
    CHECK(evaluate_lobby_decision(cmd, false, false, 2, 1000, true, 30'000)
          == LobbyDecision::WaitForPlayers);
    CHECK(evaluate_lobby_decision(cmd, false, false, 2, deadline_ms - 1, false, 0)
          == LobbyDecision::LeaveUnderMin); // Cannot guarantee leaving before start without countdown.

    // Under min + countdown at/below the 5s safety floor -> bail immediately.
    CHECK(evaluate_lobby_decision(cmd, false, false, 2, 1000, true, 5000)
          == LobbyDecision::LeaveUnderMin);
    CHECK(evaluate_lobby_decision(cmd, false, false, 2, 1000, true, 4999)
          == LobbyDecision::LeaveUnderMin);
    // Just above the floor still waits.
    CHECK(evaluate_lobby_decision(cmd, false, false, 2, 1000, true, 5001)
          == LobbyDecision::WaitForPlayers);

    // Under min + wait deadline reached -> bail.
    CHECK(evaluate_lobby_decision(cmd, false, false, 2, deadline_ms, false, 0)
          == LobbyDecision::LeaveUnderMin);
    CHECK(evaluate_lobby_decision(cmd, false, false, 2, deadline_ms + 10, false, 0)
          == LobbyDecision::LeaveUnderMin);
}

// =====================================================================
// 15. Session token lifecycle (cancellation / reset helpers)
// =====================================================================
static void test_session_tokens() {
    SessionTracker s;

    s.reset_for_token(42);
    CHECK(s.active_token == 42);
    CHECK(s.completed_raids == 0);
    CHECK(s.consecutive_losses == 0);
    CHECK(s.shiny_caught == false);
    CHECK(s.completed_keys.empty());

    // Dirty the state, then a new token supersedes and clears session state.
    s.completed_keys.insert("K#1");
    s.consecutive_losses = 1;
    s.reset_for_token(99);
    CHECK(s.active_token == 99);
    CHECK(s.completed_keys.empty());
    CHECK(s.consecutive_losses == 0);

    // clear_active zeroes the active token.
    s.clear_active();
    CHECK(s.active_token == 0);
    CHECK(s.completed_raids == 0);

    // Exclusion + quarantine pruning.
    CHECK(s.is_raid_excluded("", 1000) == true);        // empty key always excluded
    CHECK(s.is_raid_excluded("FORT#1", 1000) == false); // unknown key not excluded
    s.quarantine_raid("FORT#1", 1000, /*dur*/ 5000);    // excluded until 6000
    CHECK(s.is_raid_excluded("FORT#1", 2000) == true);
    CHECK(s.is_raid_excluded("FORT#1", 7000) == false); // pruned after expiry
}

// =====================================================================
// 16. Completion dedup + max/shiny caps
// =====================================================================
static void test_session_completion() {
    Command cmd; cmd.token = 1; cmd.max_raids = 3; cmd.stop_on_shiny = false;
    SessionTracker s; s.reset_for_token(1);

    CHECK(s.record_completion(cmd, "K1", false) == SessionContinuation::Continue);
    CHECK(s.completed_raids == 1);

    // Duplicate completion of the same raid must NOT double-count.
    CHECK(s.record_completion(cmd, "K1", false) == SessionContinuation::Continue);
    CHECK(s.completed_raids == 1);

    CHECK(s.record_completion(cmd, "K2", false) == SessionContinuation::Continue);
    CHECK(s.completed_raids == 2);
    // Third distinct raid reaches the cap.
    CHECK(s.record_completion(cmd, "K3", false) == SessionContinuation::DoneMaxRaids);
    CHECK(s.completed_raids == 3);

    // stop_on_shiny ends the session on a shiny catch regardless of cap.
    {
        Command shiny; shiny.token = 2; shiny.max_raids = 10; shiny.stop_on_shiny = true;
        SessionTracker s2; s2.reset_for_token(2);
        CHECK(s2.record_completion(shiny, "S1", /*shiny*/ true) == SessionContinuation::DoneShinyCaught);
        CHECK(s2.shiny_caught == true);
    }
    // Without stop_on_shiny, a shiny is recorded but the session continues.
    {
        Command noshiny; noshiny.token = 3; noshiny.max_raids = 10; noshiny.stop_on_shiny = false;
        SessionTracker s2; s2.reset_for_token(3);
        CHECK(s2.record_completion(noshiny, "S1", true) == SessionContinuation::Continue);
        CHECK(s2.shiny_caught == true);
    }
}

// =====================================================================
// 17. Loss tracking: 2-consecutive stop, reset-on-win, duplicate handling
// =====================================================================
static void test_session_losses() {
    // Two DIFFERENT raids lost in a row -> stop.
    {
        SessionTracker s; s.reset_for_token(1);
        CHECK(s.record_loss("L1", 1000) == SessionContinuation::Continue);
        CHECK(s.consecutive_losses == 1);
        CHECK(s.record_loss("L2", 1000) == SessionContinuation::StopConsecutiveLosses);
        CHECK(s.consecutive_losses == 2);
        // A loss also quarantines the raid key.
        CHECK(s.is_raid_excluded("L1", 1000) == true);
    }

    // A win in between resets the consecutive-loss streak.
    {
        Command cmd; cmd.token = 2; cmd.max_raids = 10;
        SessionTracker s; s.reset_for_token(2);
        CHECK(s.record_loss("L1", 1000) == SessionContinuation::Continue);
        CHECK(s.record_completion(cmd, "W1", false) == SessionContinuation::Continue);
        CHECK(s.consecutive_losses == 0);
        // Next single loss must not trip the stop by itself.
        CHECK(s.record_loss("L2", 1000) == SessionContinuation::Continue);
        CHECK(s.consecutive_losses == 1);
    }

    // CONTRACT (expected to FAIL on the pre-fix header): the SAME raid lost
    // twice (e.g. a duplicated defeat tick) must count only once. The current
    // record_loss() increments unconditionally and has no loss-key dedup, so
    // it wrongly reaches StopConsecutiveLosses on a single raid. Reported as a
    // bug for the coordinator; the header is NOT modified here.
    {
        SessionTracker s; s.reset_for_token(3);
        CHECK(s.record_loss("DUP", 1000) == SessionContinuation::Continue);
        CHECK(s.consecutive_losses == 1);
        CHECK_MSG(s.record_loss("DUP", 1000) == SessionContinuation::Continue,
                  "duplicate loss of same raid must not count twice");
        CHECK_MSG(s.consecutive_losses == 1,
                  "duplicate loss must leave consecutive_losses at 1");
    }
}

// =====================================================================
// 18. Geo helpers
// =====================================================================
static void test_geo_helpers() {
    // make_raid_key.
    CHECK(make_raid_key("FORT", 123) == "FORT#123");
    CHECK(make_raid_key("", 123).empty());
    CHECK(make_raid_key("FORT", 0).empty());

    // valid_coordinates.
    CHECK(valid_coordinates(40.0, -75.0));
    CHECK(!valid_coordinates(0.0, 0.0));
    CHECK(!valid_coordinates(91.0, 0.0));
    CHECK(!valid_coordinates(0.0, 181.0));
    CHECK(!valid_coordinates(std::nan(""), -75.0));
    CHECK(!valid_coordinates(40.0, INFINITY));

    // haversine: zero for identical points, ~111.2 km for one degree of latitude.
    CHECK(haversine_meters(40.0, -75.0, 40.0, -75.0) < 1e-6);
    const double one_deg = haversine_meters(40.0, -75.0, 41.0, -75.0);
    CHECK_MSG(std::fabs(one_deg - 111194.9) < 100.0, "1-degree latitude distance");
}

// =====================================================================
int main() {
    // Versioned berry policy keeps old clients berry-free and rejects malformed updates.
    {
        Command c;
        std::string legacy=Wire{}.str();
        CHECK(parse_command(legacy.data(),legacy.size(),c) && c.berry_mode==0);
        for(int mode:{0,701,706}){
            std::string wire="R2"+legacy.substr(2)+"|"+std::to_string(mode);
            CHECK(parse_command(wire.data(),wire.size(),c) && c.berry_mode==mode);
        }
        for(const char* suffix:{"","|","|-1","|705","|707","|706x","|706|0","|999999999999999999999999"}){
            std::string wire="R2"+legacy.substr(2)+suffix;
            CHECK(!parse_command(wire.data(),wire.size(),c));
        }
        std::string extra=legacy+"|706";
        CHECK(!parse_command(extra.data(),extra.size(),c));
        CHECK(select_raid_berry(0,5,5)==0);
        CHECK(select_raid_berry(701,1,5)==701);
        CHECK(select_raid_berry(701,0,5)==0);
        CHECK(select_raid_berry(706,5,1)==706);
        CHECK(select_raid_berry(706,1,0)==701);
        CHECK(select_raid_berry(706,0,0)==0);
    }
    test_parse_valid();
    test_parse_malformed();
    test_parse_overflow();
    test_parse_bounds();
    test_parse_stop_command();
    test_parse_boss_and_limits();
    test_classify_tier();
    test_matches_filters();
    test_candidate_eligibility();
    test_select_nearest();
    test_classify_entry_cost();
    test_pass_selection();
    test_verify_modal();
    test_lobby();
    test_session_tokens();
    test_session_completion();
    test_session_losses();
    test_geo_helpers();

    std::printf("auto_raid_policy_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

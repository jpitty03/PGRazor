// Shiny Scanner rules: command parsing, range, the once-per-Pokemon memory and pacing.
#include "shiny_scan_policy.h"
#include <cstdio>
#include <string>

using namespace pgr_shiny_scan;
static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static bool parse(const std::string& s, Command& c) { return parse_command(s.data(), s.size(), c); }

int main() {
    Command c;
    CHECK(parse("N2|7|1|700|2|8000|0|", c) && c.session == 7 && c.on && c.gap_ms == 700 && c.max_in_flight == 2 && c.budget == 8000 && !c.count && c.species.empty());
    CHECK(parse("N2|7|0|2000|1|0|1|", c) && !c.on && c.budget == 0 && c.count);  // counting works with the scanner off
    CHECK(parse("N2|7|1|300|3|1000000|1|25,129,2000", c) && c.count && c.species.size() == 3 && c.species[1] == 129);
    CHECK(wanted(c, 129) && !wanted(c, 130));
    CHECK(parse("N2|7|1|300|3|5|0|", c) && wanted(c, 130));  // no list = every species
    for (const char* bad : {"", "N2|", "N1|7|1|700|2|8000|0|", "N3|7|1|700|2|8000|0|", "N2|0|1|700|2|8000|0|", "N2|7|2|700|2|8000|0|", "N2|7|1|99|2|8000|0|",
                            "N2|7|1|60001|2|8000|0|", "N2|7|1|700|0|8000|0|", "N2|7|1|700|9|8000|0|", "N2|7|1|700|2|1000001|0|",
                            "N2|7|1|700|2|8000|", "N2|7|1|700|2|8000|2|", "N2|7|1|700|2|8000|0", "N2|7|1|700|2|8000|0|0", "N2|7|1|700|2|8000|0|2001", "N2|7|1|700|2|8000|0|25,",
                            "N2|7|1|700|2|8000|0|,25", "N2|7|1|700|2|8000|0|129,25", "N2|7|1|700|2|8000|0|25,25", "N2|7|1|700|2|8000|0|25|",
                            "N2|7|1|700|2|8000|0|x", "N2|-7|1|700|2|8000|0|", "N2|99999999999999999999|1|700|2|8000|0|"}) {
        Command rejected;
        CHECK(!parse(bad, rejected) && !rejected.on && rejected.session == 0);
    }
    CHECK(!parse_command(nullptr, 20, c));

    // Range: about 80 m either way; unknown or zero coordinates never count as near.
    CHECK(in_range(40.0, -75.0, 40.0007, -75.0));    // ~78 m
    CHECK(!in_range(40.0, -75.0, 40.00075, -75.0));  // ~83 m
    CHECK(!in_range(0.0, 0.0, 0.0, 0.0) && !in_range(40.0, -75.0, 91.0, -75.0) && !in_range(NAN, -75.0, 40.0, -75.0));

    // Each encounter id is asked about once until it expires.
    Seen seen;
    CHECK(!seen.has(1, 1000));
    seen.add(1, 1000);
    CHECK(seen.has(1, 1000) && seen.has(1, 1000 + kSeenMs - 1) && !seen.has(1, 1000 + kSeenMs) && seen.size() == 0);
    for (uint64_t id = 1; id <= kSeenMax; ++id) seen.add(id, 5000);
    CHECK(seen.size() == kSeenMax);
    seen.add(kSeenMax + 1, 6000);  // full: one old entry makes room, the new one is kept
    CHECK(seen.size() == kSeenMax && seen.has(kSeenMax + 1, 6000));
    seen.add(kSeenMax + 2, 5000 + kSeenMs);  // full of expired entries: they all go
    CHECK(seen.size() == 2);

    // Pacing: the gap, the number waiting at once, the budget, and a pause after repeated failures.
    CHECK(parse("N2|7|1|700|2|3|0|", c));
    Pace pace;
    CHECK(pace.may_send(c, 1000, 0));
    pace.sent(c, 1000);
    CHECK(!pace.may_send(c, 1699, 1) && pace.may_send(c, 1700, 1) && !pace.may_send(c, 1700, 2));
    Command spent = c; spent.budget = 0;
    CHECK(!pace.may_send(spent, 5000, 0));
    Command off = c; off.on = false;
    CHECK(!pace.may_send(off, 5000, 0));
    CHECK(!pace.failed(2000) && !pace.failed(2100));
    pace.answered();  // an answer in between starts the count again
    CHECK(!pace.failed(2200) && !pace.failed(2300) && pace.failed(2400));
    CHECK(!pace.may_send(c, 2400 + kPauseMs - 1, 0) && pace.may_send(c, 2400 + kPauseMs, 0));

    std::printf("shiny_scan_policy_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

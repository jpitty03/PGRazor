#include "auto_rocket_policy.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace pgr_auto_rocket;

static bool parse(const char* s, Command& c) { return parse_command(s, std::strlen(s), c); }

int main() {
    Command c;
    assert(parse("T1|42|1|1|20|1|0|0|2000", c));
    assert(c.token == 42 && c.running && c.targets == 1 && c.max_battles == 20 && c.walk && !c.transfer &&
           c.cooldown_ms == 0 && c.radius_m == 2000);
    assert(parse("T1|7|0|0|1|0|1|120000|0", c) && !c.running && c.targets == 0 && !c.walk && c.transfer &&
           c.cooldown_ms == 120000);
    assert(parse("T1|9|1|0|999|1|0|0|0", c) && c.max_battles == 999);  // the settings maximum
    // Other slot-12 commands and malformed frames are never taken for Auto Rocket.
    const char* bad[] = {"R3|1|1|1|1|1|1|1|1|1|1|1|1|1|1", "G1|5|1|0", "T2|42|1|0|20|1|0|0|0", "T1|0|1|0|20|1|0|0|0",
                         "T1|42|1|2|20|1|0|0|0", "T1|42|1|0|0|1|0|0|0", "T1|42|1|0|1000|1|0|0|0", "T1|42|1|0|20|1|0|0",
                         "T1|42|1|0|20|1|0|0|0|9", "T1|42|1|0|20|1|0|-1|0", "T1|42|1|0|20|1|0|7200001|0",
                         "T1|42|1|0|20|1|0|0|50001", "T1|42|1|0|20|1|0||0", "1|42|1|0|20|1|0|0"};
    for (const char* s : bad) assert(!parse(s, c));

    DisplayTypes t;
    t.grunt = 1; t.leader = 2; t.giovanni = 3; t.grunt_b = 4;
    assert(classify(1, t) == KIND_GRUNT && classify(4, t) == KIND_GRUNT && classify(2, t) == KIND_LEADER &&
           classify(3, t) == KIND_GIOVANNI && classify(5, t) == KIND_NONE && classify(0, t) == KIND_NONE);
    assert(wanted(KIND_GRUNT, 0) && !wanted(KIND_LEADER, 0) && wanted(KIND_LEADER, 1) && !wanted(KIND_GIOVANNI, 1));

    // Nearest wanted invasion; leaders only when asked; quarantine, radius and expiry respected.
    const double lat = 37.8050, lon = -122.4040;
    std::vector<Candidate> all;
    auto add = [&](const char* key, double dlat, Kind kind, int64_t end) {
        Candidate x; x.key = key; x.lat = lat + dlat; x.lon = lon; x.kind = kind; x.end_ms = end; all.push_back(x);
    };
    const int64_t wall = 1000000000;
    add("far-grunt", 0.0040, KIND_GRUNT, wall + 3600000);     // ~445 m
    add("near-leader", 0.0010, KIND_LEADER, wall + 3600000);  // ~111 m
    add("near-giovanni", 0.0005, KIND_GIOVANNI, wall + 3600000);
    add("mid-grunt", 0.0020, KIND_GRUNT, wall + 3600000);     // ~222 m
    std::map<std::string, int64_t> q;
    assert(all[select_target(all, lat, lon, 0, q, 0, wall, true, false, 0, 0, 0)].key == "mid-grunt");
    assert(all[select_target(all, lat, lon, 1, q, 0, wall, true, false, 0, 0, 0)].key == "near-leader");
    q["mid-grunt"] = 500;
    assert(all[select_target(all, lat, lon, 0, q, 0, wall, true, false, 0, 0, 0)].key == "far-grunt");
    assert(all[select_target(all, lat, lon, 0, q, 600, wall, true, false, 0, 0, 0)].key == "mid-grunt");  // expired quarantine
    assert(select_target(all, lat, lon, 0, q, 0, wall, true, true, lat, lon, 300) == -1);  // far one outside the radius
    // Too little time left to walk there and battle: skipped when walking, kept when teleporting.
    std::vector<Candidate> ending;
    Candidate e; e.key = "ending"; e.lat = lat + 0.0040; e.lon = lon; e.kind = KIND_GRUNT; e.end_ms = wall + 200000;
    ending.push_back(e);
    assert(select_target(ending, lat, lon, 0, {}, 0, wall, true, false, 0, 0, 0) == -1);
    assert(select_target(ending, lat, lon, 0, {}, 0, wall, false, false, 0, 0, 0) == 0);
    assert(select_target({}, lat, lon, 0, {}, 0, wall, true, false, 0, 0, 0) == -1);
    assert(distance_m(lat, lon, lat + 0.001, lon) > 110 && distance_m(lat, lon, lat + 0.001, lon) < 112);
    std::puts("PASS Auto Rocket policy: command parse, foreign/malformed rejection, kinds, targets, quarantine, radius, expiry");
}

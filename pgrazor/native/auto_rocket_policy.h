#pragma once
// Auto Rocket decisions that need no game: the session command Java sends on transport string slot 12,
// which invaded PokéStop to battle next, and when to give up. auto_rocket.cpp owns the game work.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pgr_auto_rocket {

// "T1|token|running|targets|maxBattles|walk|transfer|cooldownMs|radiusM"
//  targets: 0 grunts only, 1 grunts and leaders. transfer: 0 keep every catch, 1 apply the transfer rules.
struct Command {
    uint64_t token = 0;
    bool running = false;
    int targets = 0;
    int max_battles = 0;
    bool walk = true;
    bool transfer = false;
    int64_t cooldown_ms = 0;
    int64_t radius_m = 0;
};

inline bool parse_u64(const char* s, size_t n, uint64_t limit, uint64_t& out) {
    if (n == 0 || n > 19) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + static_cast<uint64_t>(s[i] - '0');
    }
    if (v > limit) return false;
    out = v;
    return true;
}

inline bool parse_command(const char* data, size_t size, Command& out) {
    if (!data || size < 19 || size > 160 || data[0] != 'T' || data[1] != '1' || data[2] != '|') return false;
    uint64_t v[8]{};
    size_t field = 0, start = 3;
    for (size_t i = 3; i <= size; ++i) {
        if (i < size && data[i] != '|') continue;
        if (field >= 8) return false;
        static const uint64_t limits[8] = {
            static_cast<uint64_t>(INT64_MAX), 1, 1, 999, 1, 1, 7200000, 50000};
        if (!parse_u64(data + start, i - start, limits[field], v[field])) return false;
        ++field;
        start = i + 1;
    }
    if (field != 8 || v[0] == 0 || v[3] == 0) return false;
    Command c;
    c.token = v[0];
    c.running = v[1] == 1;
    c.targets = static_cast<int>(v[2]);
    c.max_battles = static_cast<int>(v[3]);
    c.walk = v[4] == 1;
    c.transfer = v[5] == 1;
    c.cooldown_ms = static_cast<int64_t>(v[6]);
    c.radius_m = static_cast<int64_t>(v[7]);
    out = c;
    return true;
}

// Holoholo.Rpc.IncidentDisplayType literal values, resolved from the game by name.
struct DisplayTypes {
    int grunt = -1, grunt_b = -1, leader = -1, giovanni = -1;
};
enum Kind { KIND_NONE = 0, KIND_GRUNT = 1, KIND_LEADER = 2, KIND_GIOVANNI = 3 };
inline Kind classify(int display_type, const DisplayTypes& t) {
    if (display_type <= 0) return KIND_NONE;
    if (display_type == t.grunt || display_type == t.grunt_b) return KIND_GRUNT;
    if (display_type == t.leader) return KIND_LEADER;
    if (display_type == t.giovanni) return KIND_GIOVANNI;
    return KIND_NONE;
}
// Grunts always; leaders only when asked for. Giovanni is never battled automatically.
inline bool wanted(Kind kind, int targets) {
    return kind == KIND_GRUNT || (targets >= 1 && kind == KIND_LEADER);
}

inline bool valid_coordinates(double lat, double lon) {
    return std::isfinite(lat) && std::isfinite(lon) && std::fabs(lat) <= 90.0 && std::fabs(lon) <= 180.0 &&
           !(lat == 0.0 && lon == 0.0);
}

inline double distance_m(double lat1, double lon1, double lat2, double lon2) {
    constexpr double kEarth = 6371000.0, kRad = 3.14159265358979323846 / 180.0;
    const double dlat = (lat2 - lat1) * kRad, dlon = (lon2 - lon1) * kRad;
    const double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                     std::cos(lat1 * kRad) * std::cos(lat2 * kRad) * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2 * kEarth * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

struct Candidate {
    std::string key;     // fort id + incident id
    std::string fort_id;
    double lat = 0.0, lon = 0.0;
    Kind kind = KIND_NONE;
    int64_t end_ms = 0;  // server wall clock
};

// Conservative walking pace for "will we get there before it ends" (Walk's default is 5 km/h).
constexpr double kPlanningSpeedMps = 1.1;
constexpr int64_t kArrivalMarginMs = 90000;  // time for the battle itself after arriving

// Nearest wanted, not-quarantined invasion that can still be reached before it ends; -1 when none.
// With a radius, only invasions within it of the anchor (where the session started) count.
inline int select_target(const std::vector<Candidate>& candidates, double lat, double lon, int targets,
                         const std::map<std::string, int64_t>& quarantine, int64_t now_ms, int64_t wall_ms,
                         bool walk, bool has_anchor, double anchor_lat, double anchor_lon, int64_t radius_m) {
    int best = -1;
    double best_d = 0.0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const Candidate& c = candidates[i];
        if (!wanted(c.kind, targets) || !valid_coordinates(c.lat, c.lon)) continue;
        auto q = quarantine.find(c.key);
        if (q != quarantine.end() && q->second > now_ms) continue;
        if (radius_m > 0 && has_anchor && distance_m(anchor_lat, anchor_lon, c.lat, c.lon) > double(radius_m)) continue;
        const double d = distance_m(lat, lon, c.lat, c.lon);
        const int64_t travel_ms = walk ? static_cast<int64_t>(d / kPlanningSpeedMps * 1000.0) : 0;
        if (c.end_ms > 0 && wall_ms > 0 && c.end_ms - wall_ms < travel_ms + kArrivalMarginMs) continue;
        if (best < 0 || d < best_d) {
            best = static_cast<int>(i);
            best_d = d;
        }
    }
    return best;
}

constexpr int kMaxLossesInRow = 2;

}  // namespace pgr_auto_rocket

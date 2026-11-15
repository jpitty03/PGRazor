#pragma once
// Shiny Scanner rules with no game calls, so they can be tested on their own.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace pgr_shiny_scan {

constexpr double kRangeM = 80.0;             // only wild Pokemon this close are asked about
constexpr uint64_t kSeenMs = 30 * 60 * 1000; // a checked Pokemon is not asked about again for this long
constexpr size_t kSeenMax = 4096;
constexpr unsigned kFailuresBeforePause = 3; // refusals or timeouts in a row
constexpr uint64_t kPauseMs = 30000;

struct Command {
    uint64_t session = 0;
    bool on = false;
    unsigned gap_ms = 0;          // least time between two requests
    unsigned max_in_flight = 0;   // requests waiting for an answer at once
    unsigned budget = 0;          // requests still allowed in the rolling 24 hours
    bool count = false;           // report every wild Pokemon the game has an encounter answer for
    std::vector<int> species;     // sorted Dex ids; empty = every species
};

namespace detail {
inline bool number(const char*& p, const char* end, uint64_t limit, uint64_t& out) {
    if (p >= end || *p < '0' || *p > '9') return false;
    uint64_t v = 0;
    for (; p < end && *p >= '0' && *p <= '9'; ++p) {
        const uint64_t d = uint64_t(*p - '0');
        if (d > limit || v > (limit - d) / 10) return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}
inline bool bar(const char*& p, const char* end) { if (p >= end || *p != '|') return false; ++p; return true; }
}  // namespace detail

// "N2|<session>|<on>|<gap ms>|<max in flight>|<budget>|<count>|<species,...>"; anything else means off.
inline bool parse_command(const char* data, size_t size, Command& out) {
    out = Command{};
    if (!data || size < 15 || size > 4096 || std::memcmp(data, "N2|", 3) != 0) return false;
    const char* p = data + 3; const char* const end = data + size;
    uint64_t session = 0, on = 0, gap = 0, flight = 0, budget = 0, count = 0;
    if (!detail::number(p, end, INT64_MAX, session) || !detail::bar(p, end) ||
        !detail::number(p, end, 1, on) || !detail::bar(p, end) ||
        !detail::number(p, end, 60000, gap) || !detail::bar(p, end) ||
        !detail::number(p, end, 8, flight) || !detail::bar(p, end) ||
        !detail::number(p, end, 1000000, budget) || !detail::bar(p, end) ||
        !detail::number(p, end, 1, count) || !detail::bar(p, end)) return false;
    if (!session || gap < 100 || flight < 1) return false;
    Command c;
    while (p < end) {
        uint64_t id = 0;
        if (!detail::number(p, end, 2000, id) || id < 1) return false;
        if (!c.species.empty() && int(id) <= c.species.back()) return false;  // sorted, no repeats
        c.species.push_back(int(id));
        if (c.species.size() > 1000) return false;
        if (p < end) { if (*p != ',' || p + 1 == end) return false; ++p; }
    }
    c.session = session; c.on = on == 1; c.gap_ms = unsigned(gap); c.max_in_flight = unsigned(flight); c.budget = unsigned(budget); c.count = count == 1;
    out = std::move(c);
    return true;
}

inline bool wanted(const Command& c, int species) {
    return c.species.empty() || std::binary_search(c.species.begin(), c.species.end(), species);
}

inline bool valid_coordinates(double lat, double lon) {
    return std::isfinite(lat) && std::isfinite(lon) && std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && (lat != 0 || lon != 0);
}
inline double meters(double lat1, double lon1, double lat2, double lon2) {
    const double r = 3.14159265358979323846 / 180, x = (lat2 - lat1) * r, y = (lon2 - lon1) * r;
    const double v = std::sin(x / 2) * std::sin(x / 2) + std::cos(lat1 * r) * std::cos(lat2 * r) * std::sin(y / 2) * std::sin(y / 2);
    return 6371000.0 * 2 * std::asin(std::sqrt(std::min(1.0, v)));
}
inline bool in_range(double lat, double lon, double target_lat, double target_lon) {
    return valid_coordinates(lat, lon) && valid_coordinates(target_lat, target_lon) && meters(lat, lon, target_lat, target_lon) <= kRangeM;
}

// Encounter ids already answered (or asked about), so each wild Pokemon costs at most one request.
class Seen {
public:
    bool has(uint64_t id, uint64_t now) {
        auto it = until_.find(id);
        if (it == until_.end()) return false;
        if (now >= it->second) { until_.erase(it); return false; }
        return true;
    }
    void add(uint64_t id, uint64_t now) {
        if (until_.size() >= kSeenMax && !until_.count(id)) prune(now);
        if (until_.size() >= kSeenMax && !until_.count(id)) until_.erase(until_.begin());
        until_[id] = now + kSeenMs;
    }
    void prune(uint64_t now) { for (auto it = until_.begin(); it != until_.end();) it = now >= it->second ? until_.erase(it) : std::next(it); }
    void clear() { until_.clear(); }
    size_t size() const { return until_.size(); }
private:
    std::unordered_map<uint64_t, uint64_t> until_;
};

// Requests the game refuses or never answers slow the scanner down instead of being repeated.
struct Pace {
    uint64_t next_ms = 0, paused_until = 0;
    unsigned failures = 0;
    bool may_send(const Command& c, uint64_t now, size_t in_flight) const {
        return c.on && c.budget > 0 && in_flight < c.max_in_flight && now >= next_ms && now >= paused_until;
    }
    void sent(const Command& c, uint64_t now) { next_ms = now + c.gap_ms; }
    void answered() { failures = 0; }
    // True when this failure starts a pause.
    bool failed(uint64_t now) {
        if (++failures < kFailuresBeforePause) return false;
        failures = 0; paused_until = now + kPauseMs;
        return true;
    }
};

}  // namespace pgr_shiny_scan

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace pgr_buddy_ep {

struct Settings {
    bool enabled = false;
    int berry = 703;
    int reserve = 50;
    bool gifts = false;
    int berries[5] = {703, 701, 705, 0, 0};
};

enum Action { None = -1, Snapshot = 0, Pet = 1, Feed = 2, Gift = 3 };

struct History {
    uint64_t buddy = 0;
    uint64_t action[4]{};
    uint64_t interaction = 0;
};

namespace detail {
// Require exactly count nonempty decimal fields, with no signs or whitespace.
inline bool fields(const char* text, size_t size, uint64_t* out, size_t count) {
    if (!text || !size) return false;
    size_t field = 0;
    uint64_t value = 0;
    bool digit = false;
    for (size_t i = 0; i < size; ++i) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            const uint64_t d = static_cast<uint64_t>(c - '0');
            if (value > (std::numeric_limits<uint64_t>::max() - d) / 10) return false;
            value = value * 10 + d;
            digit = true;
        } else if (c == ';' && digit && field + 1 < count) {
            out[field++] = value;
            value = 0;
            digit = false;
        } else return false;
    }
    if (!digit || field + 1 != count) return false;
    out[field] = value;
    return true;
}

inline bool elapsed(uint64_t now, uint64_t last, uint64_t interval) {
    return now >= last && now - last >= interval;
}
inline bool due(uint64_t now, uint64_t last, uint64_t interval) {
    return last == 0 || elapsed(now, last, interval);
}
} // namespace detail

inline bool parse(const char* text, size_t size, Settings& out) {
    if (!text || size < 2 || size > 64 || text[1] != ';') return false;
    uint64_t f[9]{};
    Settings parsed;
    for (int& berry : parsed.berries) berry = 0;
    if (text[0] == '1') {
        if (!detail::fields(text, size, f, 5) ||
            f[1] > 1 || f[4] > 1 || f[3] > 1000 ||
            !(f[2] == 0 || f[2] == 701 || f[2] == 703 || f[2] == 705)) return false;
        parsed.enabled = f[1] != 0;
        parsed.berries[0] = static_cast<int>(f[2]);
        parsed.reserve = static_cast<int>(f[3]);
        parsed.gifts = f[4] != 0;
    } else if (text[0] == '2') {
        if (!detail::fields(text, size, f, 9) ||
            f[1] > 1 || f[7] > 1000 || f[8] > 1) return false;
        bool ended = false;
        for (size_t i = 0; i < 5; ++i) {
            const uint64_t berry = f[i + 2];
            if (berry == 0) { ended = true; continue; }
            if (ended || !(berry == 701 || berry == 703 || berry == 705 ||
                           berry == 706 || berry == 708)) return false;
            for (size_t j = 0; j < i; ++j)
                if (parsed.berries[j] == static_cast<int>(berry)) return false;
            parsed.berries[i] = static_cast<int>(berry);
        }
        parsed.enabled = f[1] != 0;
        parsed.reserve = static_cast<int>(f[7]);
        parsed.gifts = f[8] != 0;
    } else return false;
    parsed.berry = parsed.berries[0];
    out = parsed;
    return true;
}

// Each inventory count corresponds to the berry in the same priority slot.
inline int select_berry(const Settings& s, const int counts[5]) {
    for (size_t i = 0; i < 5; ++i) {
        if (s.berries[i] == 0) break;
        if (counts[i] > s.reserve) return s.berries[i];
    }
    return 0;
}

inline std::string encode(const History& h) {
    return std::to_string(h.buddy) + ";" + std::to_string(h.action[Snapshot]) + ";" +
        std::to_string(h.action[Pet]) + ";" + std::to_string(h.action[Feed]) + ";" +
        std::to_string(h.action[Gift]) + ";" + std::to_string(h.interaction);
}

inline bool parse_history(const std::string& text, History& out) {
    uint64_t f[6]{};
    if (text.size() > 256 || !detail::fields(text.data(), text.size(), f, 6)) return false;
    History h;
    h.buddy = f[0];
    for (size_t i = 0; i < 4; ++i) h.action[i] = f[i + 1];
    h.interaction = f[5];
    out = h;
    return true;
}

inline Action choose(const Settings& s, const History& h, uint64_t now,
                     bool hungry, int stock, bool has_gift) {
    constexpr uint64_t cooldown = 30 * 60 * 1000;
    constexpr uint64_t maintenance = 20 * 60 * 1000;
    if (!s.enabled) return None;
    if (s.gifts && has_gift && detail::due(now, h.action[Gift], cooldown)) return Gift;
    if (h.interaction != 0 && detail::elapsed(now, h.interaction, maintenance)) {
        if (detail::elapsed(now, h.action[Pet], maintenance)) return Pet;
        if (detail::elapsed(now, h.action[Snapshot], maintenance)) return Snapshot;
    }
    if (detail::due(now, h.action[Snapshot], cooldown)) return Snapshot;
    if (detail::due(now, h.action[Pet], cooldown)) return Pet;
    if (s.berry != 0 && hungry && stock > s.reserve &&
        detail::due(now, h.action[Feed], cooldown)) return Feed;
    return None;
}

inline void mark_attempt(History& h, Action action, uint64_t now) {
    if (action >= Snapshot && action <= Gift) h.action[static_cast<int>(action)] = now;
}

inline void mark_success(History& h, uint64_t now) { h.interaction = now; }

} // namespace pgr_buddy_ep

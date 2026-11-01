#ifndef PGR_REMOTE_GYM_POLICY_H
#define PGR_REMOTE_GYM_POLICY_H

// Pure, host-independent policy for the one-shot Remote Gym Feeding module (Feature 2 of
// GALARIAN_BIRD_FEATURE.md). This header carries ONLY decisions that can be unit-tested without a
// game runtime: the manual command wire format, the surplus-berry selection (never premium berries),
// and the per-defender / per-run feed caps. The managed/il2cpp state machine lives in remote_gym.cpp.
//
// Verified berry item ids and the 10-feeds/30-min server rule are documented in
// .artifacts/one-shot-automation/remote-research/verified-contracts.txt.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>

namespace pgr_remote_gym {

// Holoholo.Rpc.Item enum values == item ids (proven by gym_runtime feeding 703/701). Only surplus
// catch-neutral berries are ever fed; golden razz (706) and silver pinap (708) are never selected.
enum BerryItem { ITEM_RAZZ = 701, ITEM_NANAB = 703, ITEM_PINAP = 705 };

struct Command {
    uint64_t token = 0;     // session token (decimal, <= Java long == int64 max)
    int mode = 0;           // 0 = stop, 1 = run
    int nanab_reserve = 0;  // feed Nanab only while stock exceeds this
    int razz_reserve = 0;   // feed Razz only while stock exceeds this
    bool pinap_enabled = false;
    int pinap_reserve = 0;  // feed Pinap (optional) only while stock exceeds this
    // INCENSE_ACTIVE is parsed purely for wire-shape validation and is intentionally ignored: it is
    // an unrelated flag that the host multiplexes onto the same snapshot.
};

namespace detail {
// Require exactly `count` nonempty decimal fields separated by '|' with no sign/whitespace; each
// accumulated value is bounded by `limit` (overflow fails closed). Individual range checks are the
// caller's job. Returns false on any malformation.
inline bool fields(const char* data, size_t size, uint64_t* out, size_t count, uint64_t limit) {
    if (!data || !size || count == 0) return false;
    size_t field = 0;
    uint64_t value = 0;
    bool digit = false;
    for (size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (c >= '0' && c <= '9') {
            const uint64_t d = static_cast<uint64_t>(c - '0');
            if (value > (limit - d) / 10) return false;
            value = value * 10 + d;
            digit = true;
        } else if (c == '|' && digit && field + 1 < count) {
            out[field++] = value;
            value = 0;
            digit = false;
        } else {
            return false;
        }
    }
    if (!digit || field + 1 != count) return false;
    out[field] = value;
    return true;
}
} // namespace detail

// Parse "1|TOKEN|MODE|NANAB_RESERVE|RAZZ_RESERVE|PINAP_ENABLED|PINAP_RESERVE|INCENSE_ACTIVE".
inline bool parse(const char* data, size_t size, Command& out) {
    // Smallest well-formed frame is "1|0|0|0|0|0|0|0" (15 bytes); cap generously to bound the reader.
    if (!data || size < 15 || size > 96 || data[0] != '1' || data[1] != '|') return false;
    constexpr uint64_t token_limit = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    uint64_t f[8]{};
    if (!detail::fields(data, size, f, 8, token_limit)) return false;
    if (f[0] != 1) return false;                              // version
    if (f[2] > 1) return false;                               // mode 0|1
    if (f[3] > 1000 || f[4] > 1000 || f[6] > 1000) return false; // reserves
    if (f[5] > 1 || f[7] > 1) return false;                   // pinap enabled, incense active (ignored)
    if (f[2] != 0 && f[1] == 0) return false;                 // a run must carry a nonzero token
    Command parsed;
    parsed.token = f[1];
    parsed.mode = static_cast<int>(f[2]);
    parsed.nanab_reserve = static_cast<int>(f[3]);
    parsed.razz_reserve = static_cast<int>(f[4]);
    parsed.pinap_enabled = f[5] != 0;
    parsed.pinap_reserve = static_cast<int>(f[6]);
    out = parsed;
    return true;
}

// Choose the next surplus berry to feed given live stock. Priority: disposal-first Nanab, then Razz,
// then optional Pinap. Returns 0 when nothing is above its reserve (caller aborts the cycle). Premium
// berries are never returned.
inline int select_berry(const Command& c, int nanab_stock, int razz_stock, int pinap_stock) {
    if (nanab_stock > c.nanab_reserve) return ITEM_NANAB;
    if (razz_stock > c.razz_reserve) return ITEM_RAZZ;
    if (c.pinap_enabled && pinap_stock > c.pinap_reserve) return ITEM_PINAP;
    return 0;
}

// The reserve that guards the chosen berry, so the caller can re-check stock just before a send.
inline int reserve_for(const Command& c, int berry) {
    switch (berry) {
        case ITEM_NANAB: return c.nanab_reserve;
        case ITEM_RAZZ:  return c.razz_reserve;
        case ITEM_PINAP: return c.pinap_reserve;
        default:         return std::numeric_limits<int>::max();
    }
}

// Per-run feed accounting mirroring the server's limits (10 berries per defender per 30 minutes) plus
// a conservative distinct-defender cap. Entries are not evicted while live: a saturated map refuses
// further work rather than losing an attempt count. An attempt counts even if the server outcome is
// never seen, so a lost response can never silently exceed the cap. See verified-contracts.txt for the
// cross-feature shared-history concern flagged to the coordinator.
struct FeedHistory {
    static constexpr unsigned kMaxPerDefender = 10;
    static constexpr size_t   kMaxDefenders   = 10;
    static constexpr uint64_t kWindowMs       = 30ULL * 60ULL * 1000ULL;

    struct Entry { uint64_t until = 0; unsigned count = 0; };
    std::map<uint64_t, Entry> feeds;

    bool feed_allowed(uint64_t id, uint64_t now) {
        for (auto it = feeds.begin(); it != feeds.end();) {
            if (it->second.until <= now) it = feeds.erase(it); else ++it;
        }
        if (!id) return false;
        auto it = feeds.find(id);
        if (it != feeds.end()) return it->second.count < kMaxPerDefender;
        return feeds.size() < kMaxDefenders;
    }

    bool feed_attempt(uint64_t id, uint64_t now) {
        if (!feed_allowed(id, now)) return false;
        auto& entry = feeds[id];
        ++entry.count;
        entry.until = now + kWindowMs;
        return true;
    }

    // A declined/unknown feed latches the defender for the window so it is not retried this run.
    void refuse(uint64_t id, uint64_t now) {
        if (!id) return;
        auto it = feeds.find(id);
        if (it != feeds.end()) it->second = {now + kWindowMs, kMaxPerDefender};
        else if (feeds.size() < kMaxDefenders) feeds[id] = {now + kWindowMs, kMaxPerDefender};
    }
};

} // namespace pgr_remote_gym

#endif

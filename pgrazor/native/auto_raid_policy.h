#ifndef PGR_AUTO_RAID_POLICY_H
#define PGR_AUTO_RAID_POLICY_H

// Pure, host-independent policy and state rules for the native Auto-Raid module.
// Contains only deterministic logic that can be unit-tested without an IL2CPP runtime:
// - R1 slot-12 command parsing and parameter bounds validation
// - Raid tier bitmask classification and boss National Dex filtering
// - Active raid eligibility (no egg/expired/completed/hidden raids) and nearest Haversine selection
// - Raid pass cost classification and selection (Free pass first, optional Premium pass, never Remote/Shop)
// - Lobby minimum-player and wait-deadline bailout rules
// - Session completion deduplication, max-raids cap, shiny-caught stop, and 2-consecutive-losses stop

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pgr_auto_raid {

enum TierMaskBit : int {
    TIER_ANY         = 0,
    TIER_1_STAR      = 1 << 0, // 1
    TIER_3_STAR      = 1 << 1, // 2
    TIER_5_STAR      = 1 << 2, // 4
    TIER_MEGA        = 1 << 3, // 8
    TIER_SHADOW      = 1 << 4, // 16
    TIER_PRIMAL      = 1 << 5, // 32
    TIER_ULTRA_BEAST = 1 << 6, // 64
    TIER_MASK_ALL    = 127
};

// Internal policy identifiers, NOT game enum values. The runtime must normalize
// the metadata-resolved enum name before using these (field ordinals are not values).
enum RaidLevelProto : int {
    RAID_LEVEL_UNSET          = 0,
    RAID_LEVEL_1              = 1,
    RAID_LEVEL_2              = 2,
    RAID_LEVEL_3              = 3,
    RAID_LEVEL_4              = 4,
    RAID_LEVEL_5              = 5,
    RAID_LEVEL_MEGA           = 6,
    RAID_LEVEL_EX             = 7,
    RAID_LEVEL_ULTRA_BEAST    = 8,
    RAID_LEVEL_ELITE          = 9,
    RAID_LEVEL_PRIMAL         = 10,
    RAID_LEVEL_LEGENDARY_MEGA = 11,
    RAID_LEVEL_SHADOW_1       = 13,
    RAID_LEVEL_SHADOW_2       = 14,
    RAID_LEVEL_SHADOW_3       = 15,
    RAID_LEVEL_SHADOW_4       = 16,
    RAID_LEVEL_SHADOW_5       = 17
};

inline int normalize_raid_level(const std::string& name) {
    if (name == "Mega" || name == "_4MegaEnhanced") return RAID_LEVEL_MEGA;
    if (name == "Mega5" || name == "_5MegaEnhanced") return RAID_LEVEL_LEGENDARY_MEGA;
    if (name == "Primal") return RAID_LEVEL_PRIMAL;
    if (name == "UltraBeast" || name == "UltraBurst") return RAID_LEVEL_ULTRA_BEAST;
    if (name == "ExtendedEgg") return RAID_LEVEL_ELITE;
    for (int stars=1;stars<=5;++stars) {
        const std::string prefix="_"+std::to_string(stars);
        if (name == prefix || name == prefix+"Worldwide") return stars;
        if (name == prefix+"Shadow") return RAID_LEVEL_SHADOW_1+stars-1;
        if (name == prefix+"WorldwideUltraBeast") return RAID_LEVEL_ULTRA_BEAST;
    }
    return RAID_LEVEL_UNSET;
}

// Internal policy identifiers. The adapter maps actual Item enum literals into these
// values; never pass a game item number directly (e.g. game 1404 is a Star Piece).
enum RaidTicketItem : int {
    ITEM_FREE_RAID_TICKET      = 1401, // 0x579
    ITEM_PAID_RAID_TICKET      = 1402, // 0x57a (Premium Battle Pass)
    ITEM_LEGENDARY_RAID_TICKET = 1403, // 0x57b (EX Raid Pass)
    ITEM_REMOTE_RAID_TICKET    = 1404, // 0x57c (Forbidden)
    ITEM_EVENT_RAID_TICKET     = 1408, // 0x580
    ITEM_COIN_RAID_TICKET      = 1410  // 0x582 (Shop/Coin purchase pass - Forbidden)
};

// Holoholo.Rpc.RaidEntryCostProto.RaidLocationRequirement enum values.
enum RaidLocationRequirement : int {
    RAID_LOCATION_UNSET  = 0,
    RAID_LOCATION_LOCAL  = 1,
    RAID_LOCATION_REMOTE = 2
};

// Holoholo.Rpc.BattleStatus enum values.
enum BattleStatusProto : int {
    BATTLE_STATUS_UNSET     = 0,
    BATTLE_STATUS_ACTIVE    = 1,
    BATTLE_STATUS_VICTORY   = 2,
    BATTLE_STATUS_DEFEAT    = 3,
    BATTLE_STATUS_TIMED_OUT = 4
};

struct Command {
    uint64_t token = 0;         // 1 .. INT64_MAX
    bool enabled = false;       // 1 = run, 0 = stop
    int tier_mask = 0;          // 0 .. 127 (0 = any eligible tier)
    int max_raids = 1;          // 1 .. 100
    int min_players = 1;        // 1 .. 20
    int wait_seconds = 45;      // 15 .. 120
    bool walk = false;          // 1 = walk, 0 = teleport
    bool free_pass = true;      // 1 = allow daily free pass (prioritized first)
    bool premium_pass = false;  // 1 = allow premium battle pass when free pass unavailable
    bool stop_on_shiny = false; // 1 = stop session immediately when a shiny raid boss is caught
    std::vector<int> boss_ids;  // sorted unique National Dex IDs (1..2000); empty = any boss
    int64_t cooldown_ms = 0;    // Current-position cooldown; wait before local entry.
};

namespace detail {

inline bool parse_uint64_segment(const char* begin, const char* end, uint64_t limit, uint64_t& out) {
    if (!begin || !end || begin >= end) return false;
    uint64_t val = 0;
    for (const char* p = begin; p < end; ++p) {
        const char c = *p;
        if (c < '0' || c > '9') return false;
        const uint64_t d = static_cast<uint64_t>(c - '0');
        if (val > (limit - d) / 10ULL) return false;
        val = val * 10ULL + d;
    }
    out = val;
    return true;
}

inline bool parse_boss_list(const char* begin, const char* end, std::vector<int>& out) {
    out.clear();
    if (!begin || !end || begin > end) return false;
    const size_t len = static_cast<size_t>(end - begin);
    if (len == 0) return true; // empty = any boss
    if (len > 1000) return false;

    const char* seg_start = begin;
    while (seg_start <= end) {
        const char* comma = seg_start;
        while (comma < end && *comma != ',') ++comma;
        if (comma == seg_start) return false; // empty segment (leading, trailing, or double comma)
        if (*seg_start == '0') return false;  // boss IDs are 1..2000; reject leading zeros
        uint64_t id = 0;
        if (!parse_uint64_segment(seg_start, comma, 2000ULL, id)) return false;
        if (id < 1ULL || id > 2000ULL) return false;
        out.push_back(static_cast<int>(id));
        if (out.size() > 200) return false;
        if (comma == end) break;
        seg_start = comma + 1;
        if (seg_start == end) return false; // trailing comma
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return true;
}

} // namespace detail

// Parse slot-12 wire command:
// "R1|token|enabled|tierMask|maxRaids|minPlayers|waitSeconds|walk|freePass|premiumPass|stopOnShiny|bosses|cooldownMs"
inline bool parse_command(const char* data, size_t size, Command& out) {
    if (!data || size < 26 || size > 1200) return false;
    if (data[0] != 'R' || data[1] != '1' || data[2] != '|') return false;

    const char* seg_starts[13]{};
    const char* seg_ends[13]{};
    size_t seg_count = 0;
    const char* cur = data;
    const char* const end = data + size;

    for (const char* p = data;; ++p) {
        if (p == end || *p == '|') {
            if (seg_count >= 13) return false; // too many '|' delimiters
            seg_starts[seg_count] = cur;
            seg_ends[seg_count] = p;
            ++seg_count;
            if (p == end) break;
            cur = p + 1;
        }
    }
    if (seg_count != 13) return false;

    constexpr uint64_t kMaxToken = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    uint64_t num[10]{};
    for (size_t i = 0; i < 10; ++i) {
        if (!detail::parse_uint64_segment(seg_starts[i + 1], seg_ends[i + 1], kMaxToken, num[i])) {
            return false;
        }
    }

    const uint64_t token = num[0];
    const uint64_t enabled = num[1];
    const uint64_t tier_mask = num[2];
    const uint64_t max_raids = num[3];
    const uint64_t min_players = num[4];
    const uint64_t wait_seconds = num[5];
    const uint64_t walk = num[6];
    const uint64_t free_pass = num[7];
    const uint64_t premium_pass = num[8];
    const uint64_t stop_on_shiny = num[9];

    if (token == 0 || token > kMaxToken) return false;
    if (enabled > 1) return false;
    if (tier_mask > static_cast<uint64_t>(TIER_MASK_ALL)) return false;
    if (max_raids < 1 || max_raids > 100) return false;
    if (min_players < 1 || min_players > 20) return false;
    if (wait_seconds < 15 || wait_seconds > 120) return false;
    if (walk > 1 || free_pass > 1 || premium_pass > 1 || stop_on_shiny > 1) return false;
    if (enabled == 1 && free_pass == 0 && premium_pass == 0) return false;

    std::vector<int> bosses;
    if (!detail::parse_boss_list(seg_starts[11], seg_ends[11], bosses)) return false;
    uint64_t cooldown = 0;
    if (!detail::parse_uint64_segment(seg_starts[12], seg_ends[12], 7200000, cooldown)) return false;

    Command parsed;
    parsed.token = token;
    parsed.enabled = (enabled == 1);
    parsed.tier_mask = static_cast<int>(tier_mask);
    parsed.max_raids = static_cast<int>(max_raids);
    parsed.min_players = static_cast<int>(min_players);
    parsed.wait_seconds = static_cast<int>(wait_seconds);
    parsed.walk = (walk == 1);
    parsed.free_pass = (free_pass == 1);
    parsed.premium_pass = (premium_pass == 1);
    parsed.stop_on_shiny = (stop_on_shiny == 1);
    parsed.boss_ids = std::move(bosses);
    parsed.cooldown_ms = static_cast<int64_t>(cooldown);
    out = std::move(parsed);
    return true;
}

// Classify a raid's tier bitmask from its RaidInfoProto level and boolean properties.
// Returns 0 for unset, EX-only (level 7), or unrecognized raid levels.
inline int classify_raid_tier_mask(
    int raid_level,
    bool is_mega = false,
    bool is_legendary_mega = false,
    bool is_primal = false,
    bool is_shadow = false) {
    if (raid_level == RAID_LEVEL_EX) {
        return 0; // Invite-only EX raids are never eligible for standard pass auto-raid.
    }
    int mask = 0;
    if (is_shadow || (raid_level >= RAID_LEVEL_SHADOW_1 && raid_level <= RAID_LEVEL_SHADOW_5)) {
        mask |= TIER_SHADOW;
    }
    if (is_primal || raid_level == RAID_LEVEL_PRIMAL) {
        mask |= TIER_PRIMAL;
    }
    if (is_mega || is_legendary_mega ||
        raid_level == RAID_LEVEL_MEGA || raid_level == RAID_LEVEL_LEGENDARY_MEGA) {
        mask |= TIER_MEGA;
    }
    if (raid_level == RAID_LEVEL_ULTRA_BEAST) {
        mask |= TIER_ULTRA_BEAST;
    }
    // Standard star tiers apply when the raid is not a Shadow/Mega/Primal/UltraBeast special level.
    if (!is_shadow && !is_primal && !is_mega && !is_legendary_mega) {
        if (raid_level == RAID_LEVEL_1 || raid_level == RAID_LEVEL_2) {
            mask |= TIER_1_STAR;
        } else if (raid_level == RAID_LEVEL_3 || raid_level == RAID_LEVEL_4) {
            mask |= TIER_3_STAR;
        } else if (raid_level == RAID_LEVEL_5 || raid_level == RAID_LEVEL_ELITE) {
            mask |= TIER_5_STAR;
        }
    }
    return mask;
}

inline bool matches_raid_filters(
    const Command& cmd,
    int raid_level,
    int boss_pokemon_id,
    bool is_mega = false,
    bool is_legendary_mega = false,
    bool is_primal = false,
    bool is_shadow = false) {
    const int raid_mask = classify_raid_tier_mask(
        raid_level, is_mega, is_legendary_mega, is_primal, is_shadow);
    if (raid_mask == 0) return false;
    if (cmd.tier_mask != TIER_ANY && (raid_mask & cmd.tier_mask) == 0) return false;
    if (boss_pokemon_id < 1 || boss_pokemon_id > 2000) return false;
    if (!cmd.boss_ids.empty()) {
        if (!std::binary_search(cmd.boss_ids.begin(), cmd.boss_ids.end(), boss_pokemon_id)) {
            return false;
        }
    }
    return true;
}

struct RaidCandidate {
    std::string fort_id;
    int64_t raid_seed = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    bool has_active_raid = false;
    bool is_raid_boss_active = false;
    bool is_raid_egg_active = false;
    bool is_complete = false;
    bool is_raid_hidden = false;
    bool is_free = false;
    int64_t spawn_ms = 0;
    int64_t battle_ms = 0;
    int64_t end_ms = 0;
    int raid_level = 0;
    int boss_pokemon_id = 0;
    bool is_mega = false;
    bool is_legendary_mega = false;
    bool is_primal = false;
    bool is_shadow = false;
};

inline std::string make_raid_key(const std::string& fort_id, int64_t raid_seed) {
    if (fort_id.empty() || raid_seed == 0) return std::string();
    return fort_id + "#" + std::to_string(raid_seed);
}

inline bool valid_coordinates(double lat, double lon) {
    if (!std::isfinite(lat) || !std::isfinite(lon)) return false;
    if (std::fabs(lat) > 90.0 || std::fabs(lon) > 180.0) return false;
    if (std::fabs(lat) < 1e-6 && std::fabs(lon) < 1e-6) return false;
    return true;
}

inline double haversine_meters(double lat1, double lon1, double lat2, double lon2) {
    constexpr double kEarthRadiusM = 6371000.0;
    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
    const double dlat = (lat2 - lat1) * kDegToRad;
    const double dlon = (lon2 - lon1) * kDegToRad;
    const double rlat1 = lat1 * kDegToRad;
    const double rlat2 = lat2 * kDegToRad;
    const double sin_dlat = std::sin(dlat * 0.5);
    const double sin_dlon = std::sin(dlon * 0.5);
    const double a = sin_dlat * sin_dlat +
                     std::cos(rlat1) * std::cos(rlat2) * sin_dlon * sin_dlon;
    const double c = 2.0 * std::atan2(std::sqrt(std::max(0.0, a)), std::sqrt(std::max(0.0, 1.0 - a)));
    return kEarthRadiusM * c;
}

// Minimum remaining raid lifetime required to start a new raid attempt (default 25s).
constexpr int64_t kDefaultMinRemainingRaidMs = 25000;
// Maximum local gym interaction radius in meters for in-person raid entry.
constexpr double kLocalGymInteractionRangeM = 75.0;

inline bool is_raid_candidate_eligible(
    const RaidCandidate& c,
    const Command& cmd,
    int64_t now_ms,
    int64_t min_remaining_ms = kDefaultMinRemainingRaidMs) {
    if (c.fort_id.empty() || c.raid_seed == 0) return false;
    if (!valid_coordinates(c.latitude, c.longitude)) return false;
    if (!c.has_active_raid || !c.is_raid_boss_active || c.is_raid_egg_active) return false;
    if (c.is_complete || c.is_raid_hidden) return false;
    if (c.battle_ms <= 0 || c.end_ms <= c.battle_ms) return false;
    if (now_ms <= 0) return false;
    if (c.spawn_ms > 0 && now_ms < c.spawn_ms) return false;
    if (now_ms < c.battle_ms) return false; // Egg not yet hatched
    if (now_ms >= c.end_ms || std::max<int64_t>(0, min_remaining_ms) >= c.end_ms-now_ms) return false;
    return matches_raid_filters(
        cmd, c.raid_level, c.boss_pokemon_id, c.is_mega, c.is_legendary_mega, c.is_primal, c.is_shadow);
}

// Pass cost classification and selection policy.
struct CostItemRequirement {
    int item_id = 0;
    int count = 0;
};

struct EntryCostOption {
    int raid_type = RAID_LOCATION_LOCAL; // RaidLocationRequirement (1 = local, 2 = remote)
    std::vector<CostItemRequirement> items;
    bool satisfied_in_bag = false;       // MeetsEntryCost && FirstMissingCost == null
};

enum class PassCostKind {
    InvalidOrAmbiguous = 0,
    FreeLocal          = 1,
    PremiumLocal       = 2,
    ForbiddenRemote    = 3,
    ForbiddenShopCoin  = 4,
    UnsupportedOther   = 5
};

inline PassCostKind classify_entry_cost(
    const EntryCostOption& opt,
    bool raid_is_free = false,
    bool coin_ticket_flag = false) {
    if (coin_ticket_flag) return PassCostKind::ForbiddenShopCoin;
    if (opt.raid_type == RAID_LOCATION_REMOTE) return PassCostKind::ForbiddenRemote;
    if (opt.raid_type != RAID_LOCATION_LOCAL && opt.raid_type != RAID_LOCATION_UNSET) {
        return PassCostKind::InvalidOrAmbiguous;
    }
    if (opt.items.empty()) {
        return raid_is_free ? PassCostKind::FreeLocal : PassCostKind::InvalidOrAmbiguous;
    }
    if (opt.items.size() != 1) {
        // Check if any item is remote or coin first so we report the most specific rejection.
        for (const auto& req : opt.items) {
            if (req.item_id == ITEM_REMOTE_RAID_TICKET) return PassCostKind::ForbiddenRemote;
            if (req.item_id == ITEM_COIN_RAID_TICKET) return PassCostKind::ForbiddenShopCoin;
        }
        return PassCostKind::InvalidOrAmbiguous;
    }
    const CostItemRequirement& req = opt.items[0];
    if (req.item_id == ITEM_REMOTE_RAID_TICKET) return PassCostKind::ForbiddenRemote;
    if (req.item_id == ITEM_COIN_RAID_TICKET) return PassCostKind::ForbiddenShopCoin;
    if (req.count < 0 || req.count > 1) return PassCostKind::InvalidOrAmbiguous;
    if (req.count == 0) {
        return raid_is_free ? PassCostKind::FreeLocal : PassCostKind::InvalidOrAmbiguous;
    }
    switch (req.item_id) {
        case ITEM_FREE_RAID_TICKET:
            return PassCostKind::FreeLocal;
        case ITEM_PAID_RAID_TICKET:
            return PassCostKind::PremiumLocal;
        case ITEM_REMOTE_RAID_TICKET:
            return PassCostKind::ForbiddenRemote;
        case ITEM_COIN_RAID_TICKET:
            return PassCostKind::ForbiddenShopCoin;
        case ITEM_LEGENDARY_RAID_TICKET:
        case ITEM_EVENT_RAID_TICKET:
            return PassCostKind::UnsupportedOther;
        default:
            return PassCostKind::InvalidOrAmbiguous;
    }
}

enum class PassDecisionStatus {
    RejoinAlreadyPaid      = 0, // Ticket already used locally on this raid; rejoin costs nothing
    SelectFreePass         = 1, // Spend Free Daily Pass (1401 or free raid entry)
    SelectPremiumPass      = 2, // Spend Premium Battle Pass (1402)
    NoAllowedPassAvailable = 3, // Allowed pass types are exhausted / not in bag (never buy from shop)
    ForbiddenRemotePass    = 4, // Raid or cost requires/used a Remote Raid Pass
    ForbiddenShopPurchase  = 5, // Cost would trigger a shop/coin ticket purchase
    UnsupportedOrAmbiguous = 6  // Unknown, EX, or malformed entry cost
};

struct PassSelectionResult {
    PassDecisionStatus status = PassDecisionStatus::UnsupportedOrAmbiguous;
    PassCostKind expected_cost_kind = PassCostKind::InvalidOrAmbiguous;
    int selected_option_index = -1;
};

inline PassSelectionResult evaluate_pass_selection(
    const Command& cmd,
    bool ticket_already_used,
    bool remote_ticket_used,
    bool raid_is_free,
    const std::vector<EntryCostOption>& options) {
    PassSelectionResult result;
    if (remote_ticket_used) {
        result.status = PassDecisionStatus::ForbiddenRemotePass;
        result.expected_cost_kind = PassCostKind::ForbiddenRemote;
        return result;
    }
    if (ticket_already_used) {
        result.status = PassDecisionStatus::RejoinAlreadyPaid;
        result.expected_cost_kind = PassCostKind::FreeLocal;
        return result;
    }
    if (options.empty()) {
        if (raid_is_free && cmd.free_pass) {
            result.status = PassDecisionStatus::SelectFreePass;
            result.expected_cost_kind = PassCostKind::FreeLocal;
            return result;
        }
        result.status = PassDecisionStatus::UnsupportedOrAmbiguous;
        return result;
    }

    int satisfied_free_idx = -1;
    int unsatisfied_free_idx = -1;
    int satisfied_premium_idx = -1;
    int unsatisfied_premium_idx = -1;
    bool saw_remote = false;
    bool saw_shop = false;
    bool saw_invalid = false;

    for (size_t i = 0; i < options.size(); ++i) {
        const PassCostKind kind = classify_entry_cost(options[i], raid_is_free, false);
        switch (kind) {
            case PassCostKind::FreeLocal:
                if (options[i].satisfied_in_bag) {
                    if (satisfied_free_idx < 0) satisfied_free_idx = static_cast<int>(i);
                } else {
                    if (unsatisfied_free_idx < 0) unsatisfied_free_idx = static_cast<int>(i);
                }
                break;
            case PassCostKind::PremiumLocal:
                if (options[i].satisfied_in_bag) {
                    if (satisfied_premium_idx < 0) satisfied_premium_idx = static_cast<int>(i);
                } else {
                    if (unsatisfied_premium_idx < 0) unsatisfied_premium_idx = static_cast<int>(i);
                }
                break;
            case PassCostKind::ForbiddenRemote:
                saw_remote = true;
                break;
            case PassCostKind::ForbiddenShopCoin:
                saw_shop = true;
                break;
            default:
                saw_invalid = true;
                break;
        }
    }

    // Rule 1: Free Daily Pass is ALWAYS prioritized first when enabled and satisfied in bag.
    if (satisfied_free_idx >= 0) {
        if (cmd.free_pass) {
            result.status = PassDecisionStatus::SelectFreePass;
            result.expected_cost_kind = PassCostKind::FreeLocal;
            result.selected_option_index = satisfied_free_idx;
            return result;
        }
        // If user disabled free_pass, only proceed if a satisfied Premium pass is also selectable.
        if (cmd.premium_pass && satisfied_premium_idx >= 0) {
            result.status = PassDecisionStatus::SelectPremiumPass;
            result.expected_cost_kind = PassCostKind::PremiumLocal;
            result.selected_option_index = satisfied_premium_idx;
            return result;
        }
        result.status = PassDecisionStatus::NoAllowedPassAvailable;
        return result;
    }

    // Rule 2: No Free Pass is satisfied; use Premium Battle Pass only if enabled and satisfied in bag.
    if (satisfied_premium_idx >= 0) {
        if (cmd.premium_pass) {
            result.status = PassDecisionStatus::SelectPremiumPass;
            result.expected_cost_kind = PassCostKind::PremiumLocal;
            result.selected_option_index = satisfied_premium_idx;
            return result;
        }
        result.status = PassDecisionStatus::NoAllowedPassAvailable;
        return result;
    }

    // Neither free nor premium pass is satisfied in bag.
    if (unsatisfied_free_idx >= 0 || unsatisfied_premium_idx >= 0) {
        result.status = PassDecisionStatus::NoAllowedPassAvailable;
        return result;
    }
    if (saw_shop) {
        result.status = PassDecisionStatus::ForbiddenShopPurchase;
        result.expected_cost_kind = PassCostKind::ForbiddenShopCoin;
        return result;
    }
    if (saw_remote) {
        result.status = PassDecisionStatus::ForbiddenRemotePass;
        result.expected_cost_kind = PassCostKind::ForbiddenRemote;
        return result;
    }
    (void)saw_invalid;
    result.status = PassDecisionStatus::UnsupportedOrAmbiguous;
    return result;
}

// Verify the live UseRaidTicketGuiController selected cost (eeiw at +0x208, eeje at +0x230)
// right before confirming ticket consumption.
inline bool verify_modal_selected_cost(
    const Command& cmd,
    PassCostKind expected_kind,
    const EntryCostOption& modal_cost,
    bool raid_is_free,
    bool coin_ticket_flag) {
    if (coin_ticket_flag) return false;
    if (!modal_cost.satisfied_in_bag) return false;
    const PassCostKind actual_kind = classify_entry_cost(modal_cost, raid_is_free, coin_ticket_flag);
    if (actual_kind != expected_kind) return false;
    if (actual_kind == PassCostKind::FreeLocal) return cmd.free_pass;
    if (actual_kind == PassCostKind::PremiumLocal) return cmd.premium_pass;
    return false;
}

// Lobby monitoring and bailout policy.
constexpr int64_t kLobbyCountdownSafetyFloorMs = 5000;

enum class LobbyDecision {
    WaitForPlayers      = 0, // Under min_players, still within wait_seconds and countdown floor
    StayReady           = 1, // current_players >= min_players; wait for battle start
    EnterBattle         = 2, // Battle has started
    LeaveUnderMin       = 3, // Under min_players at wait_seconds deadline or countdown safety floor
    LeaveStopRequested  = 4  // User requested stop before battle started
};

inline LobbyDecision evaluate_lobby_decision(
    const Command& cmd,
    bool stop_requested,
    bool battle_started,
    int current_players,
    int64_t lobby_elapsed_ms,
    bool has_countdown_ms,
    int64_t countdown_remaining_ms) {
    if (battle_started) {
        return LobbyDecision::EnterBattle;
    }
    if (stop_requested) {
        return LobbyDecision::LeaveStopRequested;
    }
    // If countdown is about to expire and player count dropped below min_players, bail out immediately!
    if (current_players < cmd.min_players) {
        if (!has_countdown_ms) return LobbyDecision::LeaveUnderMin;
        if (has_countdown_ms && countdown_remaining_ms <= kLobbyCountdownSafetyFloorMs) {
            return LobbyDecision::LeaveUnderMin;
        }
        const int64_t max_wait_ms = static_cast<int64_t>(cmd.wait_seconds) * 1000LL;
        if (lobby_elapsed_ms >= max_wait_ms) {
            return LobbyDecision::LeaveUnderMin;
        }
        return LobbyDecision::WaitForPlayers;
    }
    return LobbyDecision::StayReady;
}

enum class SessionContinuation {
    Continue               = 0,
    DoneMaxRaids           = 1,
    DoneShinyCaught        = 2,
    StopConsecutiveLosses  = 3
};

// Session state tracker for raid completions, deduplication, consecutive losses, and quarantine.
struct SessionTracker {
    static constexpr int kMaxConsecutiveLosses = 2;
    static constexpr int64_t kDefaultQuarantineMs = 10LL * 60LL * 1000LL; // 10 minutes

    uint64_t active_token = 0;
    uint64_t finished_token = 0;
    int completed_raids = 0;
    int consecutive_losses = 0;
    bool shiny_caught = false;
    uint64_t travel_seq = 0;
    std::set<std::string> completed_keys;
    std::map<std::string, int64_t> quarantined_until_ms;
    std::map<std::string, int64_t> loss_recorded_until_ms;

    void reset_for_token(uint64_t token) {
        active_token = token;
        completed_raids = 0;
        consecutive_losses = 0;
        shiny_caught = false;
        travel_seq = 0;
        completed_keys.clear();
        quarantined_until_ms.clear();
        loss_recorded_until_ms.clear();
    }

    void clear_active() {
        active_token = 0;
        completed_raids = 0;
        consecutive_losses = 0;
        shiny_caught = false;
        completed_keys.clear();
        quarantined_until_ms.clear();
        loss_recorded_until_ms.clear();
    }

    void prune_quarantine(int64_t now_ms) {
        for (auto it = quarantined_until_ms.begin(); it != quarantined_until_ms.end();) {
            if (it->second <= now_ms) {
                it = quarantined_until_ms.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool is_raid_excluded(const std::string& raid_key, int64_t now_ms) {
        if (raid_key.empty()) return true;
        if (completed_keys.find(raid_key) != completed_keys.end()) return true;
        prune_quarantine(now_ms);
        return quarantined_until_ms.find(raid_key) != quarantined_until_ms.end();
    }

    void quarantine_raid(const std::string& raid_key, int64_t now_ms, int64_t duration_ms = kDefaultQuarantineMs) {
        if (raid_key.empty() || duration_ms <= 0) return;
        quarantined_until_ms[raid_key] = now_ms + duration_ms;
    }

    // Record a completed raid (victory + reward/catch resolution). Deduplicated by raid_key so repeated
    // ticks in resolve/encounter cannot double-count the same raid.
    SessionContinuation record_completion(
        const Command& cmd,
        const std::string& raid_key,
        bool caught_shiny) {
        if (cmd.token != active_token) return SessionContinuation::Continue;
        if (!raid_key.empty() && completed_keys.insert(raid_key).second) {
            ++completed_raids;
            consecutive_losses = 0;
            if (caught_shiny) shiny_caught = true;
        }
        if (cmd.stop_on_shiny && shiny_caught) {
            return SessionContinuation::DoneShinyCaught;
        }
        if (completed_raids >= cmd.max_raids) {
            return SessionContinuation::DoneMaxRaids;
        }
        return SessionContinuation::Continue;
    }

    SessionContinuation record_loss(
        const std::string& raid_key,
        int64_t now_ms,
        int64_t quarantine_ms = kDefaultQuarantineMs) {
        if (raid_key.empty() || completed_keys.count(raid_key)) return SessionContinuation::Continue;
        const auto previous = loss_recorded_until_ms.find(raid_key);
        if (previous == loss_recorded_until_ms.end() || previous->second <= now_ms) {
            quarantine_raid(raid_key, now_ms, quarantine_ms);
            loss_recorded_until_ms[raid_key] = now_ms+std::max<int64_t>(1,quarantine_ms);
            ++consecutive_losses;
        }
        if (consecutive_losses >= kMaxConsecutiveLosses) {
            return SessionContinuation::StopConsecutiveLosses;
        }
        return SessionContinuation::Continue;
    }
};

// Select the nearest eligible, non-excluded active raid candidate from `candidates`.
// Returns -1 if no candidate qualifies.
inline int select_nearest_raid(
    const std::vector<RaidCandidate>& candidates,
    const Command& cmd,
    SessionTracker& session,
    double player_lat,
    double player_lon,
    int64_t now_ms,
    double* out_distance_m = nullptr,
    int64_t min_remaining_ms = kDefaultMinRemainingRaidMs,
    int64_t quarantine_now_ms = -1) {
    if (!valid_coordinates(player_lat, player_lon)) return -1;
    // Raid timestamps use server epoch time; session retry delays use monotonic time.
    const int64_t retry_now = quarantine_now_ms < 0 ? now_ms : quarantine_now_ms;
    session.prune_quarantine(retry_now);

    int best_idx = -1;
    double best_dist = std::numeric_limits<double>::infinity();
    std::string best_key;

    for (size_t i = 0; i < candidates.size(); ++i) {
        const RaidCandidate& c = candidates[i];
        if (!is_raid_candidate_eligible(c, cmd, now_ms, min_remaining_ms)) continue;
        const std::string key = make_raid_key(c.fort_id, c.raid_seed);
        if (session.is_raid_excluded(key, retry_now)) continue;
        const double dist = haversine_meters(player_lat, player_lon, c.latitude, c.longitude);
        if (best_idx < 0 || dist < best_dist - 1e-3 ||
            (std::fabs(dist - best_dist) <= 1e-3 && key < best_key)) {
            best_idx = static_cast<int>(i);
            best_dist = dist;
            best_key = key;
        }
    }
    if (best_idx >= 0 && out_distance_m) {
        *out_distance_m = best_dist;
    }
    return best_idx;
}

} // namespace pgr_auto_raid

#endif // PGR_AUTO_RAID_POLICY_H

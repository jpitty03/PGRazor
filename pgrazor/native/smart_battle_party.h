#pragma once
#include <algorithm>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <string>

namespace pgr_smart_party {
struct HealingPolicy { bool on=false; int threshold=50; };

inline HealingPolicy healing_policy(const char *settings) {
    if (!settings) return {};
    int enabled=0,when=0,threshold=0,revive=0,full=0,end=0;
    if (std::sscanf(settings,"H2|%d|%d|%d|%d|%d%n",&enabled,&when,&threshold,&revive,&full,&end)!=5 ||
        settings[end]!='\0' || (enabled!=0&&enabled!=1) || when<0 || when>2 ||
        (threshold!=25&&threshold!=50&&threshold!=75&&threshold!=100) ||
        (revive!=0&&revive!=1) || (full!=0&&full!=1)) return {};
    return {enabled==1,threshold};
}

// Pre-battle ranking only: above the heal boundary, missing HP must not
// penalize an otherwise better counter. This never changes actual health.
inline int selection_hp(int hp,int max_hp,const HealingPolicy &healing) {
    if (healing.on && hp>0 && max_hp>=hp &&
        int64_t(hp)*100>=int64_t(healing.threshold)*max_hp) return max_hp;
    return hp;
}

// IL2CPP exposes the C# enum names (CharacterFireGruntMale), not protobuf's
// uppercase spelling. Match whole names so untyped grunts/leaders stay unknown.
inline int grunt_type(const char *name) {
    if (!name) return 0;
    std::string key;
    for (const char *p = name; *p; ++p) {
        if (*p == '_') continue;
        key += (*p >= 'A' && *p <= 'Z') ? char(*p + ('a' - 'A')) : *p;
    }
    static const char *const types[] = {"normal", "fighting", "flying", "poison", "ground", "rock", "bug", "ghost", "metal", "fire", "water", "grass", "electric", "psychic", "ice", "dragon", "dark", "fairy"};
    for (int i = 0; i < 18; ++i) {
        const std::string prefix = std::string("character") + types[i];
        for (const char *suffix : {"gruntmale", "gruntfemale", "balloongruntmale", "balloongruntfemale"})
            if (key == prefix + suffix) return i + 1;
    }
    return 0;
}

// Line-up key of an InvasionCharacter name: its lowercase words sorted and joined by '-', without
// "character", "executive" or "balloon" (CHARACTER_FIRE_GRUNT_FEMALE -> "female-fire-grunt").
inline std::string lineup_key(const char *name) {
    std::vector<std::string> words(1);
    for (const char *p = name ? name : ""; *p; ++p) {
        const char c = *p;
        const bool upper = c >= 'A' && c <= 'Z', lower = c >= 'a' && c <= 'z';
        if (!upper && !lower) { if (!words.back().empty()) words.emplace_back(); continue; }
        if (upper && !words.back().empty() && p > name && p[-1] >= 'a' && p[-1] <= 'z') words.emplace_back();
        words.back() += upper ? char(c + ('a' - 'A')) : c;
    }
    std::vector<std::string> kept;
    for (const std::string &w : words)
        if (!w.empty() && w != "character" && w != "executive" && w != "balloon") kept.push_back(w);
    std::sort(kept.begin(), kept.end());
    std::string key;
    for (const std::string &w : kept) { if (!key.empty()) key += '-'; key += w; }
    return key;
}

inline int team_size(int saved_size, bool rocket) {
    if (saved_size < 0 || saved_size > 6) return 0;
    return rocket ? 3 : saved_size;
}
}

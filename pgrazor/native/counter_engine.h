#pragma once
// Shared counter-picking engine for gym, raid and Rocket adapters.
// Pure data in / picks out: no managed objects, no game calls, no settings source.
#include "battle_counter.h"
#include "smart_battle_party.h"
#include <cstdint>
#include <vector>

namespace pgr_counter_engine {

using Fighter = pgr_counter::Fighter;
using Foe = pgr_counter::Foe;

struct Options {
    int team_size = 3;                              // raid: 6, Rocket: 3, gym: party size
    pgr_smart_party::HealingPolicy healing{};       // HP selection policy, never changes live HP
};

// One owned Pokemon reduced to the numbers the engine needs. `object` is only a caller-owned
// handle for later lookup; the engine never dereferences it.
struct Entry {
    uint64_t id = 0;
    void *object = nullptr;
    int hp = 0, max_hp = 0;
    Fighter fighter{};
};

struct Ranked {
    std::vector<int> picks;                         // indexes into entries, in team order
    std::vector<std::vector<double>> scores;        // scores[entry][foe_slot]
};

inline Ranked rank(const std::vector<Entry> &entries,
                   const std::vector<std::vector<Foe>> &foes,
                   const Options &opts) {
    Ranked out;
    if (foes.empty()) return out;
    out.scores.resize(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const Fighter &f = entries[i].fighter;
        if (f.hp <= 0 || !f.fast.known || f.fast.seconds <= 0 || entries[i].hp <= 0) {
            out.scores[i].assign(foes.size(), 0);
            continue;
        }
        Fighter adjusted = f;
        adjusted.hp = pgr_smart_party::selection_hp(entries[i].hp, entries[i].max_hp, opts.healing);
        for (const auto &group : foes)
            out.scores[i].push_back(pgr_counter::group_matchup(adjusted, group));
    }
    out.picks = pgr_counter::pick_team(out.scores, int(foes.size()), opts.team_size);
    return out;
}

}  // namespace pgr_counter_engine

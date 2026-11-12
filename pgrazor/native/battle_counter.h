#pragma once
// Counter scoring for Smart battling. Pure math with no game or runtime dependency: the caller fills
// in types, stats and moves from the game master.
//
// A matchup is scored the way raid and gym counters are usually ranked: (DPS^3 * TDO)^(1/4), where
// DPS is the attacker's damage per second against this foe over a fast/charged cycle and TDO is the
// damage it deals before fainting. Damage is the game's formula, floor(0.5 * power * atk/def * mult)+1,
// with same-type attack bonus 1.2 and type multipliers 1.6 / 0.625 / 0.390625. Weather is not modelled.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pgr_counter {

// HoloPokemonType values: 1 normal ... 18 fairy, in this order. 0 means "no type".
constexpr int kTypeCount = 18;
// Row: attacking type. Column: defending type. +1 super effective, -1 not very, -2 double resisted.
static constexpr int8_t kChart[kTypeCount][kTypeCount] = {
    //NOR FIG FLY POI GRO ROC BUG GHO STE FIR WAT GRA ELE PSY ICE DRA DAR FAI
    {  0,  0,  0,  0,  0, -1,  0, -2, -1,  0,  0,  0,  0,  0,  0,  0,  0,  0},  // normal
    {  1,  0, -1, -1,  0,  1, -1, -2,  1,  0,  0,  0,  0, -1,  1,  0,  1, -1},  // fighting
    {  0,  1,  0,  0,  0, -1,  1,  0, -1,  0,  0,  1, -1,  0,  0,  0,  0,  0},  // flying
    {  0,  0,  0, -1, -1, -1,  0, -1, -2,  0,  0,  1,  0,  0,  0,  0,  0,  1},  // poison
    {  0,  0, -2,  1,  0,  1, -1,  0,  1,  1,  0, -1,  1,  0,  0,  0,  0,  0},  // ground
    {  0, -1,  1,  0, -1,  0,  1,  0, -1,  1,  0,  0,  0,  0,  1,  0,  0,  0},  // rock
    {  0, -1, -1, -1,  0,  0,  0, -1, -1, -1,  0,  1,  0,  1,  0,  0,  1, -1},  // bug
    { -2,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  1,  0,  0, -1,  0},  // ghost
    {  0,  0,  0,  0,  0,  1,  0,  0, -1, -1, -1,  0, -1,  0,  1,  0,  0,  1},  // steel
    {  0,  0,  0,  0,  0, -1,  1,  0,  1, -1, -1,  1,  0,  0,  1, -1,  0,  0},  // fire
    {  0,  0,  0,  0,  1,  1,  0,  0,  0,  1, -1, -1,  0,  0,  0, -1,  0,  0},  // water
    {  0,  0, -1, -1,  1,  1, -1,  0, -1, -1,  1, -1,  0,  0,  0, -1,  0,  0},  // grass
    {  0,  0,  1,  0, -2,  0,  0,  0,  0,  0,  1, -1, -1,  0,  0, -1,  0,  0},  // electric
    {  0,  1,  0,  1,  0,  0,  0,  0, -1,  0,  0,  0,  0, -1,  0,  0, -2,  0},  // psychic
    {  0,  0,  1,  0,  1,  0,  0,  0, -1, -1, -1,  1,  0,  0, -1,  1,  0,  0},  // ice
    {  0,  0,  0,  0,  0,  0,  0,  0, -1,  0,  0,  0,  0,  0,  0,  1,  0, -2},  // dragon
    {  0, -1,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  1,  0,  0, -1, -1},  // dark
    {  0,  1,  0, -1,  0,  0,  0,  0, -1, -1,  0,  0,  0,  0,  0,  1,  1,  0},  // fairy
};

struct Move {
    int type = 0;
    double power = 0;
    double seconds = 1;
    double energy = 0;  // Fast move: energy gained. Charged move: energy spent, as a positive number.
    bool known = false;
};

struct Fighter {
    int type[2] = {0, 0};
    double attack = 1, defense = 1, hp = 1;
    Move fast;
    Move charged[2];
};

struct Foe {
    int type[2] = {0, 0};
    double attack = 170, defense = 170;  // Typical values, used when the real stats are unknown.
    Move fast;
    Move charged;
};

inline double step(int change) { return change > 0 ? 1.6 : change == -1 ? 0.625 : change == -2 ? 0.390625 : 1.0; }

inline double effectiveness(int move_type, const int defender[2]) {
    if (move_type < 1 || move_type > kTypeCount) return 1.0;
    double mult = 1.0;
    for (int i = 0; i < 2; ++i) {
        const int d = defender[i];
        if (d >= 1 && d <= kTypeCount && (i == 0 || d != defender[0])) mult *= step(kChart[move_type - 1][d - 1]);
    }
    return mult;
}

inline double hit(const Move &m, const int user[2], double attack, double defense, const int target[2]) {
    const double stab = m.type && (m.type == user[0] || m.type == user[1]) ? 1.2 : 1.0;
    return std::floor(0.5 * m.power * (attack / defense) * stab * effectiveness(m.type, target)) + 1.0;
}

// Best damage per second over a cycle of fast moves followed by one charged move.
inline double cycle_dps(const Move &fast, const Move *charged, int charged_count, const int user[2], double attack,
                        double defense, const int target[2]) {
    if (!fast.known || fast.seconds <= 0) return 0;
    const double fast_hit = hit(fast, user, attack, defense, target);
    double best = fast_hit / fast.seconds;
    for (int i = 0; i < charged_count; ++i) {
        const Move &c = charged[i];
        if (!c.known || c.seconds <= 0) continue;
        const double fasts = fast.energy > 0 ? std::ceil(c.energy / fast.energy) : 1.0;
        const double rate = (fasts * fast_hit + hit(c, user, attack, defense, target)) / (fasts * fast.seconds + c.seconds);
        best = std::max(best, rate);
    }
    return best;
}

// Damage per second this foe is expected to deal to the fighter. With unknown moves, assumes the foe
// has a same-type move of whichever of its types hurts this fighter most.
inline double foe_dps(const Fighter &f, const Foe &foe) {
    if (foe.fast.known) return cycle_dps(foe.fast, &foe.charged, 1, foe.type, foe.attack, f.defense, f.type);
    double worst = 1.0;
    for (int t : foe.type)
        if (t) worst = std::max(worst, effectiveness(t, f.type));
    return 10.0 * 1.2 * worst * foe.attack / f.defense;
}

// Higher is better; 0 means the fighter cannot be scored (for example it has no known fast move).
inline double matchup(const Fighter &f, const Foe &foe) {
    const double dps = cycle_dps(f.fast, f.charged, 2, f.type, f.attack, foe.defense, foe.type);
    if (dps <= 0 || f.hp <= 0) return 0;
    const double incoming = std::max(foe_dps(f, foe), 0.01);
    return std::pow(dps * dps * dps * (dps * f.hp / incoming), 0.25);
}

// scores[attacker][foe]. Takes foes in turn and gives each its best unused attacker, so a six-defender
// gym gets one counter per defender and a single raid boss gets the top attackers in order. Returns
// attacker indexes in team order; attackers scored 0 for a foe are never picked for it.
inline std::vector<int> pick_team(const std::vector<std::vector<double>> &scores, int foes, int size) {
    std::vector<int> team;
    if (foes <= 0 || size <= 0) return team;
    std::vector<bool> used(scores.size(), false);
    int misses = 0;
    for (int slot = 0; int(team.size()) < size && misses < foes; ++slot) {
        const int foe = slot % foes;
        int best = -1;
        for (int a = 0; a < int(scores.size()); ++a)
            if (!used[a] && scores[a][foe] > 0 && (best < 0 || scores[a][foe] > scores[best][foe])) best = a;
        if (best < 0) {
            ++misses;
            continue;
        }
        misses = 0;
        used[best] = true;
        team.push_back(best);
    }
    return team;
}

// A swap must be worth at least this much more than staying in.
constexpr double kSwapMargin = 1.3;

// scores: one per team member, 0 for a fainted or unusable one. active: current index, or -1 if none
// can continue. Returns the index to swap to, or -1 to stay.
inline int choose_swap(const std::vector<double> &scores, int active) {
    int best = -1;
    for (int i = 0; i < int(scores.size()); ++i)
        if (scores[i] > 0 && (best < 0 || scores[i] > scores[best])) best = i;
    if (best < 0 || best == active) return -1;
    if (active < 0 || active >= int(scores.size()) || scores[active] <= 0) return best;
    return scores[best] >= scores[active] * kSwapMargin ? best : -1;
}

}  // namespace pgr_counter

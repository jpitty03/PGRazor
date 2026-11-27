// Pure regression for the shared counter engine (gym / raid / Rocket adapters).
#include "counter_engine.h"
#include <cassert>
#include <cstdio>

namespace {
enum { NORMAL = 1, FIGHTING, FLYING, POISON, GROUND, ROCK, BUG, GHOST, STEEL, FIRE, WATER, GRASS, ELECTRIC, PSYCHIC, ICE, DRAGON, DARK, FAIRY };

pgr_counter_engine::Entry fighter(uint64_t id, int type, double attack, double hp, int max_hp) {
    pgr_counter_engine::Entry e;
    e.id = id; e.hp = int(hp); e.max_hp = max_hp;
    e.fighter.type[0] = type; e.fighter.attack = attack; e.fighter.defense = 150; e.fighter.hp = hp;
    e.fighter.fast = {type, 100, 1.5, 10, true};
    return e;
}
pgr_counter_engine::Foe foe(int type, double attack = 170) {
    pgr_counter_engine::Foe f; f.type[0] = type; f.attack = attack; f.defense = 150; return f;
}
}

int main() {
    using namespace pgr_counter_engine;
    std::vector<Entry> entries = {
        fighter(1, WATER, 200, 150, 150),
        fighter(2, FIRE, 260, 150, 150),
        fighter(3, GRASS, 220, 150, 150),
    };
    Options raid; raid.team_size = 6;
    auto one = rank(entries, {{foe(FIRE)}}, raid);
    assert(one.picks.size() == 3);          // team size requested 6, only 3 candidates
    assert(one.picks[0] == 0);              // waterbest vs fire
    assert(one.scores.size() == entries.size() && one.scores[0].size() == 1);

    Options gym; gym.team_size = 2;
    auto two = rank(entries, {{foe(FIRE)}, {foe(WATER)}}, gym);
    assert(two.picks.size() == 2);
    assert(two.picks[0] != two.picks[1]);   // no attacker used twice

    Options rocket; rocket.team_size = 3;
    auto group = rank(entries, {{foe(FIRE), foe(GROUND)}}, rocket);
    assert(group.picks.size() == 3);

    Options heal; heal.team_size = 3; heal.healing = {true, 50};
    Entry hurt = fighter(9, WATER, 200, 75, 150);
    auto damaged = rank({hurt}, {{foe(FIRE)}}, heal);
    assert(damaged.picks.size() == 1);
    assert(damaged.scores[0][0] > 0);

    Entry fainted = fighter(8, WATER, 200, 0, 150);
    assert(rank({fainted}, {{foe(FIRE)}}, raid).picks.empty());
    assert(rank(entries, {}, raid).picks.empty());

    std::puts("counter_engine_test ok");
    return 0;
}

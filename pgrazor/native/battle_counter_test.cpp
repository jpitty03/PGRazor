#include "battle_counter.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace pgr_counter;

namespace {
enum { NORMAL = 1, FIGHTING, FLYING, POISON, GROUND, ROCK, BUG, GHOST, STEEL, FIRE, WATER, GRASS, ELECTRIC, PSYCHIC, ICE, DRAGON, DARK, FAIRY };
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}
bool near_(double a, double b) { return std::fabs(a - b) < 1e-9; }
Move move(int type, double power, double seconds, double energy) {
    Move m;
    m.type = type;
    m.power = power;
    m.seconds = seconds;
    m.energy = energy;
    m.known = true;
    return m;
}
Fighter fighter(int t1, int t2, int move_type, double attack = 200, double defense = 150, double hp = 150) {
    Fighter f;
    f.type[0] = t1;
    f.type[1] = t2;
    f.attack = attack;
    f.defense = defense;
    f.hp = hp;
    f.fast = move(move_type, 12, 1.0, 10);
    f.charged[0] = move(move_type, 90, 3.0, 50);
    return f;
}
Foe foe(int t1, int t2 = 0) {
    Foe e;
    e.type[0] = t1;
    e.type[1] = t2;
    return e;
}
}  // namespace

int main() {
    // The chart must be the game's: every pair the game calls out, both directions where they differ.
    const int water[2] = {WATER, 0}, fire[2] = {FIRE, 0}, rock_ground[2] = {ROCK, GROUND}, ghost_dark[2] = {GHOST, DARK},
              flying_ground[2] = {FLYING, GROUND}, dragon_flying[2] = {DRAGON, FLYING};
    check(near_(effectiveness(WATER, fire), 1.6), "water beats fire");
    check(near_(effectiveness(FIRE, water), 0.625), "fire resisted by water");
    check(near_(effectiveness(WATER, rock_ground), 2.56), "water double super on rock/ground");
    check(near_(effectiveness(NORMAL, ghost_dark), 0.390625), "normal double resisted by ghost");
    check(near_(effectiveness(ELECTRIC, flying_ground), 1.6 * 0.390625), "electric: flying weak, ground immune");
    check(near_(effectiveness(ICE, dragon_flying), 2.56), "ice double super on dragon/flying");
    check(near_(effectiveness(FAIRY, dragon_flying), 1.6), "fairy on dragon/flying");
    check(near_(effectiveness(0, fire), 1.0), "no move type is neutral");
    const int repeated[2] = {FIRE, FIRE};
    check(near_(effectiveness(WATER, repeated), 1.6), "a repeated type counts once");
    // Every immunity in the main series is a double resist here.
    struct Pair { int a, d; } immune[] = {{NORMAL, GHOST}, {FIGHTING, GHOST}, {POISON, STEEL}, {GROUND, FLYING}, {GHOST, NORMAL}, {ELECTRIC, GROUND}, {PSYCHIC, DARK}, {DRAGON, FAIRY}};
    for (const Pair &p : immune) check(kChart[p.a - 1][p.d - 1] == -2, "immunity is a double resist");
    int total = 0;
    for (const auto &row : kChart)
        for (int v : row) total += v == -2;
    check(total == 8, "exactly eight immunities");

    // Damage formula.
    const int none[2] = {0, 0}, normal[2] = {NORMAL, 0};
    check(near_(hit(move(NORMAL, 10, 1, 1), none, 100, 100, none), 6.0), "floor(0.5*10)+1");
    check(near_(hit(move(NORMAL, 10, 1, 1), normal, 100, 100, none), 7.0), "same-type bonus applies");

    // A counter outscores a bad matchup against the same foe.
    const Foe charizard = foe(FIRE, FLYING);
    const double rock = matchup(fighter(ROCK, 0, ROCK), charizard);
    const double grass = matchup(fighter(GRASS, 0, GRASS), charizard);
    check(rock > grass * 2, "rock attacker far better than grass against fire/flying");

    // Bulk matters: same damage, more HP scores higher.
    check(matchup(fighter(ROCK, 0, ROCK, 200, 150, 300), charizard) > matchup(fighter(ROCK, 0, ROCK, 200, 150, 100), charizard), "more HP is better");
    // A fainted or moveless fighter cannot score.
    check(matchup(fighter(ROCK, 0, ROCK, 200, 150, 0), charizard) == 0, "no HP, no score");
    Fighter moveless = fighter(ROCK, 0, ROCK);
    moveless.fast.known = false;
    check(matchup(moveless, charizard) == 0, "no fast move, no score");

    // Known foe moves change who survives: a fire attacker takes less from a known fire move.
    Foe known = foe(GRASS);
    known.fast = move(FIRE, 12, 1, 10);
    known.charged = move(FIRE, 90, 3, 50);
    Fighter wet = fighter(WATER, 0, NORMAL), dry = fighter(GRASS, 0, NORMAL);
    check(matchup(wet, known) > matchup(dry, known), "resisting the foe's real moves scores higher");

    // Team building: each foe gets its best counter first.
    {
        std::vector<std::vector<double>> s = {{9, 1}, {8, 1}, {1, 7}, {1, 6}};
        auto t = pick_team(s, 2, 2);
        check(t.size() == 2 && t[0] == 0 && t[1] == 2, "one counter per foe");
        auto six = pick_team(s, 1, 6);
        check(six.size() == 4 && six[0] == 0 && six[1] == 1 && six[2] == 2 && six[3] == 3, "single foe: best first, no repeats, stops when out");
        std::vector<std::vector<double>> none_for_one = {{5, 0}, {4, 0}};
        auto left = pick_team(none_for_one, 2, 2);
        check(left.size() == 2 && left[0] == 0 && left[1] == 1, "a foe nobody can fight does not block the rest");
        check(pick_team(s, 0, 3).empty() && pick_team(s, 2, 0).empty(), "no foes or no slots");
    }

    // Swapping.
    check(choose_swap({10, 14, 5}, 0) == 1, "swap when at least 30% better");
    check(choose_swap({10, 12, 5}, 0) == -1, "stay when the gain is small");
    check(choose_swap({10, 13, 5}, 0) == 1, "exactly the margin swaps");
    check(choose_swap({14, 10}, 0) == -1, "stay when already best");
    check(choose_swap({0, 3, 7}, 0) == 2, "fainted active takes the best regardless of margin");
    check(choose_swap({5, 6}, -1) == 1, "no active takes the best");
    check(choose_swap({0, 0}, 0) == -1, "nobody left");
    check(choose_swap({}, 0) == -1, "empty team");

    if (failures) return EXIT_FAILURE;
    std::printf("battle_counter_test ok\n");
    return 0;
}

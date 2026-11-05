#pragma once
#include <stdint.h>
// Team GO Rocket auto swap: when a Pokemon faints in a Rocket battle and the game asks for the next
// one, the first Pokemon whose button can be pressed is picked, the same as tapping it. Only while
// Team GO Rocket automation is on (any of auto quick attack, auto charged attack, auto shield).
void pgr_rocket_swap_tick(uint64_t tid);
void pgr_rocket_swap_stop();

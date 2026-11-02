#pragma once
#include <stdint.h>
// Called only from the existing Unity owner after ExecuteTasks. No second scheduler/hook.
void pgr_egg_hatch_tick(uint64_t now,uint64_t owner_tid,bool cleanup);
void pgr_egg_hatch_stop();

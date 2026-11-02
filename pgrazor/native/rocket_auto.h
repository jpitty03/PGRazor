#pragma once
#include "runtime_bootstrap.h"
#include "rocket_combat.h"
namespace pgr_rocket_auto {
void install(const PgrBootBinding &,const pgr_rocket_combat_config_v1 &);
void tick(uint64_t proven_tid,bool rocket_scope,bool rocket_battle,bool stopping);
void stop(uint64_t proven_tid);
uint32_t roots();
bool spin_blocked();
}

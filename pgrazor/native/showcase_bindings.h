#pragma once
#include "showcase_score.h"
namespace pgr_showcase {
// Resolve all score-reader dependencies for the supported game version on the
// proven Unity owner. No hooks, RPCs, retained managed objects or global cache.
// Request deadline is absolute CLOCK_BOOTTIME and is never extended here.
bool resolve_live_score_bindings(const ScoreRequest&, ScoreBindings&);
}

#pragma once
#include <cstdint>
// Called by the initialized encounter host on Unity's thread, after shared metadata
// validates the pointer-sized GC handle ABI. Observe the top UI and main menu only;
// never change the game's menu or automation state.
void pgr_menu_visibility_tick(uint64_t now_ms);

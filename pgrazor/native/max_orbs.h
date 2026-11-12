#pragma once
// Read-only probe, called on the battle controller's owner thread with a rooted
// live BattleState. Returns the game's Left/Right enum, or 0 when no move is due.
int pgr_max_orb_direction(void *state,void *dodge_method);
// Best-effort eager binding; failure leaves other battle automation available.
bool pgr_max_orbs_prepare(void *dodge_method);

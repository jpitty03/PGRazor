#pragma once
#include <cstdint>
struct PgrSharedMetadataOutput;
// One move row: visible flag, power-number position as a fraction of the screen (top-left origin), TM count.
struct PgrTmRow { int visible; float x, y; int count; };
void pgr_tm_tick(uint64_t tid, const PgrSharedMetadataOutput *metadata);
void pgr_tm_state(PgrTmRow out[3]);
void pgr_tm_request(int row);
extern "C" void pgr_tm_note_panel(void *panel);
